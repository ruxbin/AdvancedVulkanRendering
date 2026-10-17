#pragma once
// SVT CPU 调度系统：反馈去重、请求状态机、优先级调度、预算、generation。
// 设计依据：docs/superpowers/specs/2026-10-16-streaming-virtual-texture-design.md
//   §5（SvtSystem：反馈去重、优先级、预算、generation、逻辑驻留；
//       不直接组织场景绘制）、§9（请求/预算/调度顺序）。
//
// 本模块纯 CPU、无 Vulkan 依赖（设计 §13.1：资产解析、地址计算、调度和
// 回收策略可不依赖 Vulkan 测试）。I/O 由 SvtTileStreamer 执行（M3 后续），
// 本模块只维护逻辑状态并产生有界的帧批次描述。
//
// 反馈闭环（设计 §8.3/§9）：
//   GPU feedback(uint2) -> ingestFeedback() 去重累加 -> prepareFrame()
//   按词典序取出有界批次 -> [M3 后续：streamer 读取 -> uploadQueued ->
//   GPU 上传 -> commitSubmittedBatch 发布]。
//
// 首版语义（与设计一致的关键点）：
//   - 命中也反馈：CPU 沿当前驻留层级 touch fallback 链，防止父页被淘汰（M4）。
//   - 冷启动缺页：合成尚未驻留的祖先请求，最粗缺失祖先优先。
//   - 一个 tile 只允许一个未完成任务；过期结果用 generation/token 丢弃。
//   - 反馈溢出：容量截断，保留 overflow 计数。

#include "SvtAsset.h"

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace svt {

// ---------------------------------------------------------------------------
// 请求/反馈键（设计 §9：CPU 哈希键为完整 {assetIndex,generation,mip,x,y}）
// ---------------------------------------------------------------------------

struct SvtTileKey {
  uint32_t assetIndex = 0;
  uint32_t generation = 0;
  uint32_t mip = 0;
  uint32_t x = 0;
  uint32_t y = 0;

  bool operator==(const SvtTileKey &o) const {
    return assetIndex == o.assetIndex && generation == o.generation &&
           mip == o.mip && x == o.x && y == o.y;
  }
};

struct SvtTileKeyHash {
  size_t operator()(const SvtTileKey &k) const {
    // FNV-1a over 20 bytes。
    uint64_t h = 14695981039346656037ull;
    const auto mix = [&h](uint32_t v) {
      for (int i = 0; i < 4; ++i) {
        h ^= (v >> (8 * i)) & 0xFF;
        h *= 1099511628211ull;
      }
    };
    mix(k.assetIndex);
    mix(k.generation);
    mix(k.mip);
    mix(k.x);
    mix(k.y);
    return static_cast<size_t>(h);
  }
};

// GPU 反馈条目（设计 §8.3 的 uint2 编码的解码结果）。
struct SvtFeedbackItem {
  uint32_t assetIndex = 0;
  uint32_t generation = 0; // 低 16 位（编码协议，设计 §8.3）
  uint32_t pageX = 0;
  uint32_t pageY = 0;
  uint32_t mip = 0;
};

// 反馈解码（word0/word1 布局见设计 §8.3）。
bool svtDecodeFeedback(uint32_t word0, uint32_t word1, SvtFeedbackItem &out);

// ---------------------------------------------------------------------------
// 请求状态机（设计 §9）
// ---------------------------------------------------------------------------

enum class SvtRequestState : uint8_t {
  Missing = 0,
  Queued = 1,      // 已入请求队列，等待 I/O worker 领取
  Reading = 2,     // worker 正在读取/解压
  Ready = 3,       // payload 已就绪（CPU 侧），等待上传预算
  UploadQueued = 4,// 已进入帧上传批次
  Resident = 5,    // 已发布驻留
  RetryAfter = 6,  // 读失败退避中（retryAtFrame 有效）
  Cancelled = 7,   // 本 generation 的坏页；资产卸载时删除
};

// 逻辑驻留记录（与 SvtPhysicalCache 的物理槽位解耦；M4 起含淘汰热度）。
struct SvtResidencyEntry {
  SvtRequestState state = SvtRequestState::Missing;
  uint64_t count = 0;          // 反馈热度（饱和累加，设计 §9）
  uint64_t lastSeenFrame = 0;  // 最近被反馈命中的帧号（LRU 依据，M4）
  uint64_t lastRequestedFrame = 0;
  uint64_t retryAtFrame = 0;   // RetryAfter 状态的退避截止帧
  uint64_t payloadBytes = 0;   // Ready payload 字节数（背压记账）
  uint32_t retryCount = 0;     // CRC/短读重试计数（最多 3 次，设计 §9）
  uint32_t requestToken = 0;   // 过期结果丢弃（generation 内单调）
  uint8_t priority = 2;
  uint64_t reservedBytes = 0;
};

// ---------------------------------------------------------------------------
// 帧批次（设计 §5：不可变批次；本模块只产出描述，不提交 Vulkan）
// ---------------------------------------------------------------------------

struct SvtFrameRequest {
  SvtTileKey key;
  uint8_t priority;    // 0 = 显式预取（最高），越大越低
  uint64_t count;      // 去重后的反馈热度
  uint64_t waitFrames; // 从首次请求起等待的帧数
};

struct SvtFrameBatchDesc {
  std::vector<SvtFrameRequest> reads;      // 本帧安排 I/O 读取的 tile
  std::vector<SvtFrameRequest> uploads;    // Ready 且预算允许上传的 tile
  size_t feedbackDropped = 0;              // 溢出/无效被丢弃的反馈数
  size_t requestsDropped = 0;              // pending 容量满被丢弃的低优先级数
};

// ---------------------------------------------------------------------------
// 配置（设计 §9 表格的初值）
// ---------------------------------------------------------------------------

struct SvtSystemConfig {
  uint64_t pendingTileRequests = 8192;  // 逻辑请求容量（满时保高优先级）
  uint32_t maxUploadPagesPerFrame = 32; // 页/帧
  uint64_t maxUploadBytesPerFrame = 4ull << 20; // 字节/帧
  uint64_t readyPayloadBytes = 32ull << 20;     // Reading 预留 + Ready/UploadQueued 背压上限
  uint32_t maxRetries = 3;                      // CRC/短读重试
  uint64_t retryBackoffFrames[3] = {6, 30, 120};// 100/500/2000ms @60fps 折算
};

// ---------------------------------------------------------------------------
// SvtSystem
// ---------------------------------------------------------------------------

class SvtSystem {
public:
  explicit SvtSystem(const SvtSystemConfig &config = SvtSystemConfig());

  // ---- 资产生命周期（注册顺序即 assetIndex；generation 见设计 §8.3）
  struct AssetDesc {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t mipCount = 0;
    uint32_t tailFirstMip = 0;
    uint32_t tilesHint = 0; // 页表项数（可选，仅用于容量预估）
    uint32_t tileSize = 128;
    uint64_t tilePayloadBytes = 0; // 解码后大小；0 兼容旧调用方，只有完成时检查预算
  };
  // 返回 assetIndex；重复注册同一描述返回新索引（资产身份 = 索引+generation）。
  uint32_t registerAsset(const AssetDesc &desc);
  // 卸载：generation+1，取消所有相关请求。之后同索引重新注册前必须排空
  // 旧引用（设计 §8.3 回绕协议）。
  void unregisterAsset(uint32_t assetIndex);
  uint32_t generation(uint32_t assetIndex) const;
  size_t assetCount() const { return assets_.size(); }

  // ---- 反馈入口（每帧把去重后的 GPU 反馈喂进来；命中也算，设计 §8.3）
  // ingestFeedback 之前应先在调用方做原始 append 去重（同一帧内重复条目
  // 合并 count）；本方法做跨帧状态合并与祖先合成。
  // frameIndex：当前帧号（用于 lastSeenFrame/退避计时）。
  // 返回实际登记的请求数（含祖先）。
  size_t ingestFeedback(const SvtFeedbackItem *items, size_t count,
                        uint64_t frameIndex);

  // 显式预取（priority 0，设计 §9 词典序第一优先）。
  bool prefetchTile(uint32_t assetIndex, uint32_t mip, uint32_t x, uint32_t y,
                    uint64_t frameIndex);

  // ---- 帧调度（设计 §9 词典序：预取 > 粗缺口 > 反馈热度 > 等待年龄 > 稳定 key）
  // 上传按 completeRead 记录的实际 payload 字节数限额。
  SvtFrameBatchDesc prepareFrame(uint64_t frameIndex);

  // ---- 状态推进（由 I/O 层回调；M3 后续接 SvtTileStreamer）
  // worker 领取读取：Queued -> Reading，预留 AssetDesc.tilePayloadBytes。
  // 状态不符或 Reading + Ready 预算不足时返回 false。
  bool beginRead(const SvtTileKey &key);
  // streamer 拒绝领取时 Reading -> Queued，并释放预留。
  bool cancelRead(const SvtTileKey &key);
  // 物理映射撤销后移除逻辑 Resident；后续反馈可重新请求。
  bool evictResident(const SvtTileKey &key);
  // 读取完成：Reading -> Ready。payloadBytes 计入背压。
  bool completeRead(const SvtTileKey &key, uint64_t payloadBytes);
  // 读取失败：Reading -> RetryAfter（退避后回 Queued）或 Cancelled（重试耗尽）。
  bool failRead(const SvtTileKey &key, uint64_t frameIndex);
  // 上传完成发布：UploadQueued -> Resident。
  bool commitUploaded(const SvtTileKey &key);
  // 上传批次被取消（提交失败等）：UploadQueued -> Ready。
  bool cancelUpload(const SvtTileKey &key);

  // ---- 查询
  SvtRequestState state(const SvtTileKey &key) const;
  bool isResident(uint32_t assetIndex, uint32_t mip, uint32_t x, uint32_t y) const;
  const SvtResidencyEntry *entry(const SvtTileKey &key) const;
  uint64_t readyBytes() const { return readyBytes_; }
  uint64_t bufferedBytes() const { return readyBytes_ + readingBytes_; }
  size_t pendingCount() const { return pendingCount_; }

  // 常驻判定辅助：mip 是否属于 tail（tail 请求无需 tile）。
  static bool mipIsTail(const AssetDesc &d, uint32_t mip) {
    return mip >= d.tailFirstMip;
  }

private:
  struct AssetState {
    AssetDesc desc;
    uint32_t generation = 0;
    bool alive = true;
  };

  SvtResidencyEntry *findEntry(const SvtTileKey &k);
  const SvtResidencyEntry *findEntry(const SvtTileKey &k) const;
  // 反馈单 tile：登记/刷新请求 + 祖先链合成。返回是否产生新请求。
  bool feedbackTile(uint32_t assetIndex, uint32_t mip, uint32_t x, uint32_t y,
                   uint64_t frameIndex, uint64_t count);
  bool enqueueRequest(const SvtTileKey &key, uint64_t frameIndex, uint8_t priority,
                      uint64_t count);

  SvtSystemConfig config_;
  std::vector<AssetState> assets_;
  std::unordered_map<SvtTileKey, SvtResidencyEntry, SvtTileKeyHash> entries_;
  uint64_t readyBytes_ = 0;
  uint64_t readingBytes_ = 0;
  size_t pendingCount_ = 0; // Queued+Reading+Ready+UploadQueued 计数
  uint32_t nextToken_ = 1;
};

} // namespace svt
