#pragma once
// SVT Vulkan 运行时（M2 全驻留范围）：GPU 资源、set2 描述符、一次性上传。
// 设计依据：docs/superpowers/specs/2026-10-16-streaming-virtual-texture-design.md
//   §5（所有权：本模块不拥有场景资源解析）、§7.1/7.2（页表/物理池）、
//   §8.1（set2 绑定布局）、§10.1（首版单 graphics queue 上传 + legacy barrier）。
//
// M2 范围（设计 §14 M2：全驻留采样，先不启用反馈驱动淘汰）：
//   - 注册资产（.svt 文件 + 完整 tile/tail 上传）
//   - 每 {format,T,B} 一张 atlas（容量 = 各资产 tile 总和，M2 不淘汰）
//   - 扁平页表 buffer（所有资产共享，每个 sync slot 一份快照）
//   - tail 纹理数组（每资产一张完整 mip 链小纹理）
//   - set2 描述符（binding 0-7，见 shaders/svt_common.hlsl 文件头）
//   - 上传走一次性单时间命令（场景加载期），Draw 热路径零 SVT 工作
//
// M3+ 范围（不在本文件首版实现）：反馈/readback、增量上传、退休协议。

#include "SvtAsset.h"
#include "SvtPhysicalCache.h"
#include "SvtSystem.h"
#include "SvtTileStreamer.h"
#include <memory>

#include "VulkanSetup.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace svt {

// 与 shaders/svt_common.hlsl 的结构逐字段对应（std430 无 padding 语义）。
// SvtAssetGpuDesc：8 个 u32 + SVT_MAX_TILED_MIPS(16) 个 u32 = 24 u32 = 96 字节。
struct SvtAssetGpuDesc {
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t mipCount = 0;
  uint32_t tailFirstMip = 0;
  uint32_t pageBase = 0;
  uint32_t poolIndex = 0;
  uint32_t tailTextureIndex = 0xFFFFFFFFu; // 尾纹理数组索引
  uint32_t flags = 0;                      // bit0 sRGB
  uint32_t mipPageOffsets[16] = {};        // SVT_MAX_TILED_MIPS
};
struct SvtPoolGpuDesc {
  uint32_t atlasWidth = 0;
  uint32_t atlasHeight = 0;
  uint32_t slotsPerRow = 0;
  uint32_t slotCount = 0;
  uint32_t paddedTile = 0;
  uint32_t tileSize = 0;
  uint32_t border = 0;
  uint32_t pad = 0;
};
static_assert(sizeof(SvtAssetGpuDesc) == 96, "shader layout mismatch");
static_assert(sizeof(SvtPoolGpuDesc) == 32, "shader layout mismatch");

class SvtVulkanRuntime {
public:
  struct CreateInfo {
    // .svt 文件列表（manifest 已由调用方解析、按材质槽选好）。
    std::vector<std::filesystem::path> assetFiles;
    // 材质 side table：materials[i] 的四个槽（0=albedo 1=roughness 2=normal
    // 3=emissive）各指向 assetFiles 的索引，0xFFFFFFFF 表示走旧路径。
    std::vector<uint32_t> materialAssets; // materialCount * 4
    uint32_t syncSlotCount = 2;           // 每 slot 一份页表快照
    VkDeviceSize atlasByteBudget = 128ull << 20;
    VkDeviceSize tailByteBudget = 32ull << 20;
    uint32_t atlasDimensionLimit = 4096;
    VkFormat fallbackFormat = VK_FORMAT_R8G8B8A8_UNORM; // RGBA8 资产的 Vk 格式
    // M3 模式：不一次性上传全部 tile；页表初始全 0（缺页回退 tail），
    // tile 由 uploadTiles() 按需发布。预算仍约束池容量（满后上传暂停，
    // 直到 M4 淘汰释放槽位）。M2 全驻留模式 = false。
    bool streamingMode = false;
  };

  SvtVulkanRuntime() = default;
  ~SvtVulkanRuntime();
  SvtVulkanRuntime(const SvtVulkanRuntime &) = delete;
  SvtVulkanRuntime &operator=(const SvtVulkanRuntime &) = delete;

  // 全驻留初始化：解析资产 -> 规划池 -> 创建 GPU 资源 -> 一次性上传 ->
  // 写 set2 描述符。任一资产失败即整体失败（调用方应回退非 SVT 路径）。
  // 必须在 GpuScene 的图形队列空闲时调用（内部使用单时间命令）。
  bool init(const VulkanDevice &device, const CreateInfo &info, std::string *error = nullptr);
  void destroy();

  bool isReady() const { return device_ != VK_NULL_HANDLE; }
  // Caller has waited this sync slot's fence. All host writes are slot-local.
  void beginFrame(uint32_t syncSlot, uint32_t frameIndex, bool capture,
                  bool pauseReads, float mipBias);
  void recordFrameUploads(VkCommandBuffer cmd);
  void onSubmitted();
  void cancelFrame();

  // Draw 接入（设计 §5：set2 明确绑定 svtDescriptorSets[_syncSlot]）。
  VkDescriptorSet descriptorSet(uint32_t syncSlot) const;
  // set2 布局；pipeline layout 由 GpuScene 用
  // {globalSetLayout, applSetLayout, svtSetLayout} 组装（三 set 布局在本类之外）。
  VkDescriptorSetLayout setLayout() const { return svtSetLayout_; }
  // 预算准入后的资产索引重映射（旧 assetFiles 索引 -> 保留后索引；
  // -1 = 被剔除，材质槽应回 INVALID 走旧路径）。init() 成功后有效。
  const std::vector<int32_t> &assetRemap() const { return assetRemap_; }

  // ---- 反馈采集（M3，设计 §8.3/§10.1）
  // 每帧 Draw 前：更新 frame params（frameIndex/factor/enabled）、清本 slot
  // feedback header（录制到当前 command buffer，需在任何 SVT pass 之前）。
  void updateFrameParams(uint32_t syncSlot, uint32_t frameIndex, uint32_t feedbackFactor,
                         bool feedbackEnabled, float mipBias);

  // ---- 增量上传（M3，设计 §10.2"发布新页"）
  // 输入：SvtSystem prepareFrame 的 uploads 批次 + streamer 的 payload。
  // 行为：分配槽位（物理缓存）-> staging 写入 -> 录制到当前 command buffer
  // （atlas copy + 页表快照更新 + barrier）。调用方在 submit 成功后调
  // commitUploads() 提交逻辑状态；提交失败调 cancelUploads()。
  // 注意：录制进的是"当前帧"command buffer——atlas barrier 需覆盖整图
  // （设计 §10.1：layout 转换覆盖整张单 mip image，可接受的串行成本）。
  struct UploadRequest {
    uint32_t assetIndex; // runtime 资产索引（非 assetFiles 旧索引）
    uint32_t mip;
    uint32_t x;
    uint32_t y;
    const uint8_t *payload; // 解码后 tile 数据（调用方保持有效到 submit）
    uint32_t payloadSize;
  };
  // 返回实际接受的请求数（槽位耗尽/尺寸不符则跳过该条）。
  // accepted[i] 标记 UploadRequest[i] 是否被接受。
  size_t recordUploads(VkCommandBuffer cmd, const UploadRequest *requests,
                       size_t count, std::vector<bool> *accepted = nullptr);
  // submit 成功后：物理槽标记 Resident（M4 起含页表版本/serial 语义）。
  void commitUploads(const UploadRequest *requests, size_t count,
                     const std::vector<bool> &accepted);
  // 提交失败/放弃：归还槽位（Reserved -> Free），页表快照由下次覆盖。
  void cancelUploads(const UploadRequest *requests, size_t count,
                     const std::vector<bool> &accepted);
  // 在当前 command buffer 上录制：清 header -> （SVT pass 由调用方录制）->
  // 复制到 readback。设计 §8.3：所有请求写完之后才复制 readback。
  void recordFeedbackClear(VkCommandBuffer cmd, uint32_t syncSlot);
  void recordFeedbackReadback(VkCommandBuffer cmd, uint32_t syncSlot);
  // 消费已完成的 readback（CPU 映射读取 + invalidate；设计 §10.1）。
  // 调用方必须先确认该 slot 的提交已完成（fence），本方法不做等待。
  // outWords：header(4) + min(attemptCount,capacity)*2 个 uint。
  // 返回 false = readback 未就绪/参数非法。
  bool readbackFeedback(uint32_t syncSlot, std::vector<uint32_t> &outWords);
  // 上次 readback 的溢出统计（readbackFeedback 后有效）。
  uint64_t lastFeedbackOverflow() const { return feedbackOverflow_; }
  uint32_t lastFeedbackAttempts() const { return feedbackAttempts_; }
  uint32_t lastFeedbackUnique() const { return feedbackUnique_; }
  uint32_t syncSlotCount() const { return uint32_t(slotSerials_.size()); }

  // 反馈容量（条目数，设计 §8.3 默认 262144）。
  uint32_t feedbackCapacity() const { return kFeedbackCapacity; }

  // 统计（ImGui，设计 §12）。
  size_t assetCount() const { return assets_.size(); }
  uint64_t atlasBytes() const { return actualAtlasBytes_; }
  uint64_t tailBytes() const { return tailBytes_; }
  uint64_t pageTableBytes() const { return pageTableBytes_; }

  struct Stats {
    size_t poolCount = 0;
    size_t residentTiles = 0;
    size_t totalTiles = 0;
    uint64_t atlasBytes = 0;
    uint64_t tailBytes = 0;
    uint64_t pageTableBytes = 0;
    size_t materialBindings = 0; // 非 0xFFFFFFFF 的槽数
    size_t retiringTiles = 0, freeTiles = 0, pendingRequests = 0;
    uint64_t uploadedPages = 0, uploadedBytes = 0, evictions = 0;
    uint64_t bufferedBytes = 0, readBytes = 0, failures = 0;
  };
  Stats stats() const;
  void setDebugView(uint32_t view) { debugView_ = view; }
  bool poolUsage(uint32_t pool, SvtPhysicalCache::PoolUsage &usage) const {
    return cache_.poolUsage(pool, usage);
  }

private:
  struct GpuBuffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
  };
  struct GpuImage {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkDeviceSize allocatedBytes = 0;
  };

  bool createBuffer_(VkDeviceSize size, VkBufferUsageFlags usage,
                     VkMemoryPropertyFlags props, GpuBuffer &out);
  bool createImage_(uint32_t width, uint32_t height, uint32_t mipLevels,
                    VkFormat format, VkImageUsageFlags usage, GpuImage &out);
  void destroyBuffer_(GpuBuffer &b);
  void destroyImage_(GpuImage &img);

  // device 句柄的轻量持有（不拥有，不负责销毁；生命周期由 GpuScene 保证）。
  // 另缓存 queue/command pool 句柄用于一次性上传（复用 VulkanDevice 的）。
  VkDevice device_ = VK_NULL_HANDLE;
  VkPhysicalDevice physicalDevice_ = VK_NULL_HANDLE;
  VkQueue queue_ = VK_NULL_HANDLE;
  VkCommandPool cmdPool_ = VK_NULL_HANDLE;

  std::vector<SvtAsset> assets_;
  std::vector<SvtAssetGpuDesc> assetDescs_;
  SvtPhysicalCache cache_;
  std::vector<GpuImage> atlases_;       // 每 pool 一张
  std::vector<GpuImage> tails_;         // 每资产一张
  std::vector<GpuBuffer> pageTables_;   // 每 sync slot 一份
  GpuBuffer assetDescBuffer_;
  GpuBuffer poolDescBuffer_;
  GpuBuffer materialBindingBuffer_;
  VkSampler atlasSampler_ = VK_NULL_HANDLE;  // bilinear, clamp, no mip
  VkSampler tailSampler_ = VK_NULL_HANDLE;   // bilinear, repeat, trilinear

  VkDescriptorSetLayout svtSetLayout_ = VK_NULL_HANDLE;
  VkDescriptorPool svtDescriptorPool_ = VK_NULL_HANDLE;
  std::vector<VkDescriptorSet> svtDescriptorSets_; // per sync slot（内容相同，
                                                   // M3 起每 slot 独立快照）

  uint64_t tailBytes_ = 0;
  uint64_t pageTableBytes_ = 0;
  uint32_t totalTiles_ = 0;
  size_t materialBindingCount_ = 0;
  std::vector<int32_t> assetRemap_; // 预算准入的旧->新资产索引（-1 剔除）
  bool streaming_ = false;
  uint64_t actualAtlasBytes_ = 0;
  std::unique_ptr<SvtSystem> system_;
  std::unique_ptr<SvtTileStreamer> streamer_;
  std::unordered_map<SvtTileKey, std::vector<uint8_t>, SvtTileKeyHash> readyPayloads_;
  struct Resident { uint32_t pool, slot; uint64_t lastSeen; };
  std::unordered_map<SvtTileKey, Resident, SvtTileKeyHash> resident_;
  struct PendingUpload { SvtTileKey key; uint32_t pool, slot, page; VkDeviceSize offset; };
  std::vector<PendingUpload> pendingUploads_;
  std::vector<uint32_t> pageMirror_;
  std::vector<GpuBuffer> uploadStaging_;
  std::vector<uint64_t> slotSerials_;
  std::vector<bool> feedbackValid_;
  SvtFrameBatchDesc frameBatch_;
  uint32_t activeSlot_ = 0, activeFrame_ = 0;
  uint64_t submittedSerial_ = 0, completedSerial_ = 0;
  uint64_t uploadedPages_ = 0, uploadedBytes_ = 0, evictions_ = 0;
  bool batchRecorded_ = false;
  uint32_t debugView_ = 0;
  static constexpr VkDeviceSize kUploadBytes = 4ull << 20;

  // ---- 反馈资源（每 sync slot 一份，设计 §8.3/§9 "readback 资源每 slot 一份"）
  // feedback UAV（set2 binding 9）与 frame params（binding 8）按 slot 写入
  // 各自的 svtDescriptorSets_[slot]——从此每 slot 的 set 内容分化。
  static constexpr uint32_t kFeedbackCapacity = 262144; // uint2 条目（~2 MiB）
  struct FeedbackResources {
    GpuBuffer buffer;   // device-local：header(16B) + capacity*8B，UAV 写 + transfer src
    GpuBuffer readback; // host-visible：同尺寸，transfer dst + host read
    GpuBuffer params;   // host-visible：SvtFrameParams UBO（32B）
    void *paramsMapped = nullptr;
  };
  std::vector<FeedbackResources> feedback_; // size = syncSlotCount
  uint64_t feedbackOverflow_ = 0;
  uint32_t feedbackAttempts_ = 0, feedbackUnique_ = 0;

  bool initFeedback_(uint32_t slotCount, std::string *error);
  void destroyFeedback_();

  bool initialized_ = false;
};

// SvtFormat -> VkFormat（sRGB 变体独立，pool key 语义一致）。
bool svtFormatToVk(SvtFormat format, VkFormat &out);

} // namespace svt
