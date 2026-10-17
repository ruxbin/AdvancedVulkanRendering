// SVT Vulkan runtime: bounded streaming, per-slot publication and serial retirement.
// 设计依据：2026-10-16 设计 §7（页表/物理池）、§8.1（set2）、§10.1（单 queue 上传 + legacy barrier）。
//
// Pinned tails are loaded at startup; tiles are requested from GPU feedback.

#include "SvtVulkanRuntime.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstring>

namespace svt {

// ---------------------------------------------------------------------------
// 格式映射（与 mapFromApple 的语义对齐；sRGB 变体独立成 pool key）
// ---------------------------------------------------------------------------

bool svtFormatToVk(SvtFormat format, VkFormat &out) {
  switch (format) {
  case SvtFormat::RGBA8: out = VK_FORMAT_R8G8B8A8_UNORM; return true;
  case SvtFormat::RGBA8_SRGB: out = VK_FORMAT_R8G8B8A8_SRGB; return true;
  case SvtFormat::BC1: out = VK_FORMAT_BC1_RGBA_UNORM_BLOCK; return true;
  case SvtFormat::BC1_SRGB: out = VK_FORMAT_BC1_RGBA_SRGB_BLOCK; return true;
  case SvtFormat::BC3: out = VK_FORMAT_BC3_UNORM_BLOCK; return true;
  case SvtFormat::BC3_SRGB: out = VK_FORMAT_BC3_SRGB_BLOCK; return true;
  case SvtFormat::BC4: out = VK_FORMAT_BC4_UNORM_BLOCK; return true;
  case SvtFormat::BC5: out = VK_FORMAT_BC5_UNORM_BLOCK; return true;
  case SvtFormat::BC5_SNORM: out = VK_FORMAT_BC5_SNORM_BLOCK; return true;
  case SvtFormat::BC7: out = VK_FORMAT_BC7_UNORM_BLOCK; return true;
  case SvtFormat::BC7_SRGB: out = VK_FORMAT_BC7_SRGB_BLOCK; return true;
  }
  return false;
}

namespace {

std::string rtFail(std::string *error, const std::string &msg) {
  if (error)
    *error = msg;
  return msg;
}

// 单时间命令（一次性上传用；与 VulkanDevice::beginSingleTimeCommands 相同模式，
// 但 queue/pool 由本类缓存，避免依赖 device 的 impl 细节）。
struct OneShotCommands {
  VkDevice device = VK_NULL_HANDLE;
  VkQueue queue = VK_NULL_HANDLE;
  VkCommandPool pool = VK_NULL_HANDLE;
  VkCommandBuffer cmd = VK_NULL_HANDLE;

  bool begin(VkDevice dev, VkQueue q, VkCommandPool p) {
    device = dev;
    queue = q;
    pool = p;
    VkCommandBufferAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    alloc.commandPool = pool;
    alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(device, &alloc, &cmd) != VK_SUCCESS)
      return false;
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(cmd, &bi) != VK_SUCCESS)
      return false;
    return true;
  }

  bool endAndSubmit() {
    if (vkEndCommandBuffer(cmd) != VK_SUCCESS)
      return false;
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    VkFence fence = VK_NULL_HANDLE;
    if (vkQueueSubmit(queue, 1, &si, fence) != VK_SUCCESS)
      return false;
    vkQueueWaitIdle(queue); // 一次性路径允许等待；热路径不用（设计 §10.1）
    vkFreeCommandBuffers(device, pool, 1, &cmd);
    cmd = VK_NULL_HANDLE;
    return true;
  }
};

// legacy barrier helper（不引 synchronization2，设计 §10.1）。
void recordImageBarrier(VkCommandBuffer cmd, VkImage image,
                        VkPipelineStageFlags srcStage, VkAccessFlags srcAccess,
                        VkImageLayout oldLayout, VkPipelineStageFlags dstStage,
                        VkAccessFlags dstAccess, VkImageLayout newLayout) {
  VkImageMemoryBarrier b{};
  b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.image = image;
  b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  b.subresourceRange.baseMipLevel = 0;
  b.subresourceRange.levelCount = VK_REMAINING_MIP_LEVELS;
  b.subresourceRange.baseArrayLayer = 0;
  b.subresourceRange.layerCount = VK_REMAINING_ARRAY_LAYERS;
  b.srcAccessMask = srcAccess;
  b.dstAccessMask = dstAccess;
  b.oldLayout = oldLayout;
  b.newLayout = newLayout;
  vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &b);
}

} // namespace

// ---------------------------------------------------------------------------
// 生命周期
// ---------------------------------------------------------------------------

SvtVulkanRuntime::~SvtVulkanRuntime() { destroy(); }

void SvtVulkanRuntime::destroy() {
  if (device_ == VK_NULL_HANDLE)
    return;
  streamer_.reset();
  system_.reset();
  vkDeviceWaitIdle(device_);

  if (svtDescriptorPool_ != VK_NULL_HANDLE)
    vkDestroyDescriptorPool(device_, svtDescriptorPool_, nullptr);
  if (svtSetLayout_ != VK_NULL_HANDLE)
    vkDestroyDescriptorSetLayout(device_, svtSetLayout_, nullptr);
  if (atlasSampler_ != VK_NULL_HANDLE)
    vkDestroySampler(device_, atlasSampler_, nullptr);
  if (tailSampler_ != VK_NULL_HANDLE)
    vkDestroySampler(device_, tailSampler_, nullptr);

  destroyBuffer_(assetDescBuffer_);
  destroyBuffer_(poolDescBuffer_);
  destroyBuffer_(materialBindingBuffer_);
  for (auto &pt : pageTables_)
    destroyBuffer_(pt);
  for (auto &t : tails_)
    destroyImage_(t);
  for (auto &a : atlases_)
    destroyImage_(a);
  destroyFeedback_();

  for (auto &staging : uploadStaging_) destroyBuffer_(staging);
  uploadStaging_.clear();
  readyPayloads_.clear(); resident_.clear(); pendingUploads_.clear();
  slotSerials_.clear(); feedbackValid_.clear(); pageMirror_.clear();
  pageTables_.clear();
  tails_.clear();
  atlases_.clear();
  assets_.clear();
  assetDescs_.clear();
  svtDescriptorSets_.clear();
  cache_ = SvtPhysicalCache{}; // 重置簿记

  svtDescriptorPool_ = VK_NULL_HANDLE; svtSetLayout_ = VK_NULL_HANDLE;
  atlasSampler_ = tailSampler_ = VK_NULL_HANDLE;
  tailBytes_ = pageTableBytes_ = actualAtlasBytes_ = 0;
  submittedSerial_ = completedSerial_ = 0;
  uploadedPages_ = uploadedBytes_ = evictions_ = 0;
  totalTiles_ = 0; streaming_ = false; batchRecorded_ = false;
  device_ = VK_NULL_HANDLE;
  initialized_ = false;
}

// ---------------------------------------------------------------------------
// 小工具
// ---------------------------------------------------------------------------

bool SvtVulkanRuntime::createBuffer_(VkDeviceSize size, VkBufferUsageFlags usage,
                                     VkMemoryPropertyFlags props, GpuBuffer &out) {
  VkBufferCreateInfo bi{};
  bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  bi.size = std::max<VkDeviceSize>(size, 4);
  bi.usage = usage;
  bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  if (vkCreateBuffer(device_, &bi, nullptr, &out.buffer) != VK_SUCCESS)
    return false;
  VkMemoryRequirements req;
  vkGetBufferMemoryRequirements(device_, out.buffer, &req);
  VkMemoryAllocateInfo ai{};
  ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  ai.allocationSize = req.size;
  // findMemoryType 属 VulkanDevice（私有头不可用），此处内联等价逻辑。
  VkPhysicalDeviceMemoryProperties memProps;
  vkGetPhysicalDeviceMemoryProperties(physicalDevice_, &memProps);
  bool found = false;
  for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i) {
    if ((req.memoryTypeBits & (1u << i)) &&
        (memProps.memoryTypes[i].propertyFlags & props) == props) {
      ai.memoryTypeIndex = i;
      found = true;
      break;
    }
  }
  if (!found || vkAllocateMemory(device_, &ai, nullptr, &out.memory) != VK_SUCCESS) {
    vkDestroyBuffer(device_, out.buffer, nullptr);
    out.buffer = VK_NULL_HANDLE;
    return false;
  }
  vkBindBufferMemory(device_, out.buffer, out.memory, 0);
  return true;
}

bool SvtVulkanRuntime::createImage_(uint32_t width, uint32_t height,
                                    uint32_t mipLevels, VkFormat format,
                                    VkImageUsageFlags usage, GpuImage &out) {
  VkImageCreateInfo ii{};
  ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  ii.imageType = VK_IMAGE_TYPE_2D;
  ii.extent = {width, height, 1};
  ii.mipLevels = mipLevels;
  ii.arrayLayers = 1;
  ii.format = format;
  ii.tiling = VK_IMAGE_TILING_OPTIMAL;
  ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  ii.usage = usage;
  ii.samples = VK_SAMPLE_COUNT_1_BIT;
  if (vkCreateImage(device_, &ii, nullptr, &out.image) != VK_SUCCESS)
    return false;
  VkMemoryRequirements req;
  vkGetImageMemoryRequirements(device_, out.image, &req);
  VkMemoryAllocateInfo ai{};
  ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  ai.allocationSize = req.size;
  VkPhysicalDeviceMemoryProperties memProps;
  vkGetPhysicalDeviceMemoryProperties(physicalDevice_, &memProps);
  bool found = false;
  for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i) {
    if ((req.memoryTypeBits & (1u << i)) &&
        (memProps.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) ==
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) {
      ai.memoryTypeIndex = i;
      found = true;
      break;
    }
  }
  if (!found || vkAllocateMemory(device_, &ai, nullptr, &out.memory) != VK_SUCCESS) {
    vkDestroyImage(device_, out.image, nullptr);
    out.image = VK_NULL_HANDLE;
    return false;
  }
  vkBindImageMemory(device_, out.image, out.memory, 0);
  out.allocatedBytes = req.size;

  VkImageViewCreateInfo vi{};
  vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  vi.image = out.image;
  vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
  vi.format = format;
  vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  vi.subresourceRange.baseMipLevel = 0;
  vi.subresourceRange.levelCount = mipLevels;
  vi.subresourceRange.baseArrayLayer = 0;
  vi.subresourceRange.layerCount = 1;
  if (vkCreateImageView(device_, &vi, nullptr, &out.view) != VK_SUCCESS) {
    destroyImage_(out);
    return false;
  }
  return true;
}

void SvtVulkanRuntime::destroyBuffer_(GpuBuffer &b) {
  if (b.buffer != VK_NULL_HANDLE)
    vkDestroyBuffer(device_, b.buffer, nullptr);
  if (b.memory != VK_NULL_HANDLE)
    vkFreeMemory(device_, b.memory, nullptr);
  b = {};
}

void SvtVulkanRuntime::destroyImage_(GpuImage &img) {
  if (img.view != VK_NULL_HANDLE)
    vkDestroyImageView(device_, img.view, nullptr);
  if (img.image != VK_NULL_HANDLE)
    vkDestroyImage(device_, img.image, nullptr);
  if (img.memory != VK_NULL_HANDLE)
    vkFreeMemory(device_, img.memory, nullptr);
  img = {};
}

// ---------------------------------------------------------------------------
// init（全驻留）
// ---------------------------------------------------------------------------

bool SvtVulkanRuntime::init(const VulkanDevice &device, const CreateInfo &info,
                            std::string *error) {
  destroy();
  if (info.assetFiles.empty())
    return rtFail(error, "no svt assets"), false;
  if (info.materialAssets.size() % 4 != 0)
    return rtFail(error, "materialAssets must be materialCount*4"), false;
  if (info.syncSlotCount == 0 || info.syncSlotCount > 8)
    return rtFail(error, "syncSlotCount out of range"), false;

  device_ = device.getLogicalDevice();
  physicalDevice_ = device.getPhysicalDevice();
  queue_ = device.getGraphicsQueue();
  cmdPool_ = device.getCommandPool();

  streaming_ = info.streamingMode;
  VkPhysicalDeviceFeatures features{};
  vkGetPhysicalDeviceFeatures(physicalDevice_, &features);
  if (!features.fragmentStoresAndAtomics)
    return rtFail(error, "fragmentStoresAndAtomics unavailable"), false;
  VkPhysicalDeviceProperties properties{};
  vkGetPhysicalDeviceProperties(physicalDevice_, &properties);
  if (info.assetFiles.size() > 1024 || info.materialAssets.empty())
    return rtFail(error, "asset/material capacity exceeded"), false;

  // ---- 1. 打开并校验全部资产，统计每池需求
  struct AssetPlan {
    uint32_t tileCount = 0;
    int poolIndex = -1;
    uint32_t pageBase = 0;
  };
  std::vector<AssetPlan> plans(info.assetFiles.size());
  struct KeyAggregate {
    SvtFormat format;
    uint32_t tileSize, border;
    uint64_t tiles;
  };
  std::vector<KeyAggregate> aggregates;

  assets_.resize(info.assetFiles.size());
  for (size_t i = 0; i < info.assetFiles.size(); ++i) {
    if (!assets_[i].open(info.assetFiles[i], error))
      return false;
    const SvtAsset &a = assets_[i];
    plans[i].tileCount = a.pageTableEntryCount();
    VkFormat fmt;
    VkFormatProperties fp{};
    if (!svtFormatToVk(a.format(), fmt)) return rtFail(error, "unknown format"), false;
    vkGetPhysicalDeviceFormatProperties(physicalDevice_, fmt, &fp);
    const auto required = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
        VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    if ((fp.optimalTilingFeatures & required) != required)
      return rtFail(error, "unsupported SVT format sampling/copy"), false;

    // 只允许 T/B 与 manifest 预期一致的资产（当前默认 128/4；诊断资产可不同，
    // 但同一池的 P 必须一致，这里按资产的 T/B 分池）。
    bool aggFound = false;
    for (auto &agg : aggregates) {
      if (agg.format == a.format() && agg.tileSize == a.tileSize() && agg.border == a.border()) {
        agg.tiles += plans[i].tileCount;
        aggFound = true;
        break;
      }
    }
    if (!aggFound)
      aggregates.push_back({a.format(), a.tileSize(), a.border(), plans[i].tileCount});
    // 统计完立即释放句柄：数百资产同时持有 FILE* 会撞 MSVC CRT 的
    // stdio 上限（~512）。元数据保留在 SvtAsset 内，上传阶段 reopen。
    assets_[i].closeFile();
  }

  if (streaming_) {
    assetRemap_.resize(assets_.size());
    for (uint32_t i = 0; i < assets_.size(); ++i) assetRemap_[i] = i;
    uint64_t remaining = info.atlasByteBudget;
    for (size_t i = 0; i < aggregates.size(); ++i) {
      const auto &agg = aggregates[i];
      SvtPoolPlan plan;
      const uint64_t share = remaining / (aggregates.size() - i);
      // Reserve padding for the allocator; actual requirements are checked below.
      const uint64_t usable = share > 65536 ? share - 65536 : share;
      const auto dim = (std::min)(info.atlasDimensionLimit, properties.limits.maxImageDimension2D);
      const uint64_t perTile = svtMipByteSize(agg.format, agg.tileSize+2*agg.border, agg.tileSize+2*agg.border);
      const uint64_t desired = std::max<uint64_t>(1, (std::min)(agg.tiles, usable / perTile));
      if (!svtPlanPool(agg.format, agg.tileSize, agg.border, desired, dim, usable, plan))
        return rtFail(error, "budget too small for format pool"), false;
      SvtPoolConfig cfg{agg.format, agg.tileSize, agg.border, plan.slotsPerRow, plan.rows};
      if (cache_.createPool(cfg, error) < 0) return false;
      VkFormat fmt; svtFormatToVk(agg.format, fmt);
      GpuImage atlas;
      if (!createImage_(plan.slotsPerRow*(agg.tileSize+2*agg.border),
                        plan.rows*(agg.tileSize+2*agg.border), 1, fmt,
                        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, atlas))
        return rtFail(error, "atlas allocation failed"), false;
      atlases_.push_back(atlas);
      if (atlas.allocatedBytes > remaining) return rtFail(error, "actual atlas budget exceeded"), false;
      remaining -= atlas.allocatedBytes;
      actualAtlasBytes_ += atlas.allocatedBytes;
    }
  } else {
  // ---- 2. 资产准入 + 池规划（预算全局共享，设计 §9 "atlas 总预算是所有池之和"）
  // M2 策略：预算不足以全驻留时，按 tile 数升序贪心接纳资产（小资产优先，
  // 保证尽量多的材质完整走 SVT），装不下的资产整体剔除——其材质槽回
  // INVALID 走旧路径（设计 §6.3 兼容语义）。被剔除资产不产生"部分驻留"
  // 状态：全 tail + 全 tile 或完全不注册，维持 M2 全驻留不变式。
  {
    // 每格式聚合初始为空，逐资产累加；某格式装不下一个资产时跳过该资产。
    for (auto &agg : aggregates)
      agg.tiles = 0;

    // 资产按 tileCount 升序（稳定：同大小按索引）。
    std::vector<uint32_t> order(assets_.size());
    for (uint32_t i = 0; i < order.size(); ++i)
      order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](uint32_t x, uint32_t y) {
      return plans[x].tileCount < plans[y].tileCount;
    });

    std::vector<bool> admitted(assets_.size(), false);
    for (uint32_t idx : order) {
      const SvtAsset &a = assets_[idx];
      // 找到（或创建候选）聚合。
      KeyAggregate *target = nullptr;
      for (auto &agg : aggregates)
        if (agg.format == a.format() && agg.tileSize == a.tileSize() &&
            agg.border == a.border())
          target = &agg;
      const uint64_t newTiles = target->tiles + plans[idx].tileCount;

      // 全局预算 + 尺寸可规划性检查（接纳本资产后）。
      // truncated 表示该格式池在 atlasDimensionLimit 下装不下聚合 tiles
      // ——接纳会违反全驻留不变式，拒绝本资产。
      uint64_t projectedTotal = 0;
      bool plannable = true;
      {
        for (const auto &agg : aggregates) {
          if (agg.tiles == 0)
            continue;
          SvtPoolPlan p;
          if (svtPlanPool(agg.format, agg.tileSize, agg.border, agg.tiles,
                          info.atlasDimensionLimit, ~0ull, p)) {
            projectedTotal += p.atlasBytes;
            if (p.truncated)
              plannable = false;
          } else {
            plannable = false;
          }
        }
        SvtPoolPlan pThis;
        if (svtPlanPool(a.format(), a.tileSize(), a.border(), newTiles,
                        info.atlasDimensionLimit, ~0ull, pThis)) {
          projectedTotal += pThis.atlasBytes;
          if (pThis.truncated)
            plannable = false;
        } else {
          plannable = false; // 单池都放不下
        }
      }
      if (!plannable || projectedTotal > info.atlasByteBudget)
        continue; // 预算外/不可规划：剔除该资产（材质槽走旧路径）
      target->tiles = newTiles;
      admitted[idx] = true;
    }

    // 剔除未接纳资产：从 assets_/plans 中移除，调用方的 materialAssets 里
    // 指向它们的槽需要回退 INVALID。为保持索引稳定，采用"压缩"方案：
    // 保留资产的顺序不变，删除未接纳项，并记录旧->新索引映射。
    std::vector<int32_t> remap(assets_.size(), -1);
    std::vector<SvtAsset> keptAssets;
    std::vector<AssetPlan> keptPlans;
    for (size_t i = 0; i < assets_.size(); ++i) {
      if (!admitted[i])
        continue;
      remap[i] = static_cast<int32_t>(keptAssets.size());
      keptAssets.push_back(std::move(assets_[i]));
      keptPlans.push_back(plans[i]);
    }
    const size_t droppedCount = assets_.size() - keptAssets.size();
    assets_ = std::move(keptAssets);
    plans = std::move(keptPlans);
    if (assets_.empty())
      return rtFail(error, "atlas budget too small for any asset"), false;
    if (droppedCount > 0)
      spdlog::warn("svt: {} assets exceed full-residency budget; "
                   "their material slots use the legacy path",
                   droppedCount);

    // 用接纳后的聚合创建池（每池恰好容纳需求）。
    // 预算约束已在准入阶段按"全池总和"检查，此处单池不再设预算上限。
    for (auto &agg : aggregates) {
      if (agg.tiles == 0)
        continue;
      SvtPoolPlan plan;
      if (!svtPlanPool(agg.format, agg.tileSize, agg.border, agg.tiles,
                       info.atlasDimensionLimit, ~0ull, plan) ||
          plan.truncated) {
        // 贪心阶段已保证放得下；此处失败属簿记错误。
        return rtFail(error, "pool planning inconsistent after admission"), false;
      }
      SvtPoolConfig cfg;
      cfg.format = agg.format;
      cfg.tileSize = agg.tileSize;
      cfg.border = agg.border;
      cfg.slotsPerRow = plan.slotsPerRow;
      cfg.rows = plan.rows;
      const int poolIdx = cache_.createPool(cfg, error);
      if (poolIdx < 0)
        return false;

      VkFormat vkFormat;
      if (!svtFormatToVk(agg.format, vkFormat))
        return rtFail(error, "no VkFormat for svt format"), false;
      GpuImage atlas;
      // atlas 只有 mip0（设计 §7.2 / §15 "atlas mip 造成接缝"）。
      if (!createImage_(plan.slotsPerRow * (agg.tileSize + 2 * agg.border),
                        plan.rows * (agg.tileSize + 2 * agg.border), 1, vkFormat,
                        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                        atlas))
        return rtFail(error, "failed to create atlas image"), false;
      atlases_.push_back(atlas);
    }

    // 保存 remap 供调用方修正 materialAssets（通过输出参数约定：
    // init 无法直接改调用方的 vector，这里记录在成员里由 GpuScene 读取）。
    assetRemap_ = std::move(remap);
  }

  for (const auto &atlas : atlases_) actualAtlasBytes_ += atlas.allocatedBytes;
  if (actualAtlasBytes_ > info.atlasByteBudget)
    return rtFail(error, "actual atlas budget exceeded"), false;
  }

  // ---- 3. 资产页表基址 + tail 纹理
  {
    uint32_t pageBase = 0;
    for (size_t i = 0; i < assets_.size(); ++i) {
      const SvtAsset &a = assets_[i];
      plans[i].pageBase = pageBase;
      if (plans[i].tileCount > UINT32_MAX - pageBase)
        return rtFail(error, "page table overflow"), false;
      pageBase += plans[i].tileCount;
      // pageBase + count 有界检查（uint32 页表项）。
      if (static_cast<uint64_t>(pageBase) > 0xFFFFFFFFull)
        return rtFail(error, "global page table exceeds uint32"), false;
    }
    totalTiles_ = pageBase;
    pageTableBytes_ = std::max<uint64_t>(4, static_cast<uint64_t>(pageBase) * sizeof(uint32_t));
    if (pageTableBytes_ > properties.limits.maxStorageBufferRange)
      return rtFail(error, "page table exceeds device buffer range"), false;

    // tail 纹理：每资产一张完整 mip 链（[tailFirstMip, mipCount)）。
    for (size_t i = 0; i < assets_.size(); ++i) {
      const SvtAsset &a = assets_[i];
      VkFormat vkFormat;
      if (!svtFormatToVk(a.format(), vkFormat))
        return rtFail(error, "no VkFormat for tail"), false;
      GpuImage tail;
      // tail 用完整尺寸 mip0？不——tail 链从 tailFirstMip 开始，mip0 尺寸即
      // mipWidth(tailFirstMip)。tail mip 链层数 = mipCount - tailFirstMip。
      if (!createImage_(a.mipWidth(a.tailFirstMip()), a.mipHeight(a.tailFirstMip()),
                        a.tailMipCount(), vkFormat,
                        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                        tail))
        return rtFail(error, "failed to create tail image"), false;
      tails_.push_back(tail);
      tailBytes_ += tail.allocatedBytes;
      if (tailBytes_ > info.tailByteBudget)
        return rtFail(error, "pinned tail budget exceeded"), false;
    }
  }

  // ---- 4. 一次性上传：tile -> atlas、tail mip、页表快照、元数据 buffer
  // 页表 CPU 镜像贯穿第 4/5 阶段（第 5 阶段写入每 slot 的 GPU 快照）。
  std::vector<uint32_t> pageTableMirror(totalTiles_, 0);
  {
    OneShotCommands osc;
    if (!osc.begin(device_, queue_, cmdPool_))
      return rtFail(error, "failed to begin one-shot commands"), false;

    // 4a. atlas: UNDEFINED -> TRANSFER_DST（首次使用，允许整图转换，设计 §10.1）
    for (auto &atlas : atlases_)
      recordImageBarrier(osc.cmd, atlas.image,
                         VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0,
                         VK_IMAGE_LAYOUT_UNDEFINED,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_ACCESS_TRANSFER_WRITE_BIT,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    for (auto &tail : tails_)
      recordImageBarrier(osc.cmd, tail.image,
                         VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0,
                         VK_IMAGE_LAYOUT_UNDEFINED,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_ACCESS_TRANSFER_WRITE_BIT,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

    // 4b. 逐资产上传 tile payload 到 atlas 槽 + 写 CPU 页表镜像。
    // staging：host visible，一次性写所有 tile。
    std::vector<VkBufferImageCopy> atlasCopies;
    std::vector<VkBufferImageCopy> tailCopies;
    // 打包 staging：atlas tiles + tail mips 两大段，逐资产追加。
    // 简化：逐 tile 一次 map/write/unmap 会太慢；这里逐资产 staging buffer。
    for (size_t i = 0; i < assets_.size(); ++i) {
      // 上传前短暂重开文件（统计阶段已释放句柄，见上）。
      if (!assets_[i].reopen(error))
        return false;
      const SvtAsset &a = assets_[i];
      // 离开作用域前确保句柄释放（含所有 early-return 路径）。
      struct FileGuard {
        SvtAsset &asset;
        ~FileGuard() { asset.closeFile(); }
      } fileGuard{assets_[i]};
      // 池索引按 key 查找（剔除资产的 aggregate 可能未建池，直接用
      // aggregates 下标会与 cache_ 池顺序错位）。
      const int poolIdx =
          cache_.findPool(a.format(), a.tileSize(), a.border());
      if (poolIdx < 0)
        return rtFail(error, "asset pool missing"), false;
      const auto *pinfo = cache_.poolInfo(static_cast<uint32_t>(poolIdx));
      if (!pinfo)
        return rtFail(error, "pool info missing"), false;
      plans[i].poolIndex = poolIdx;

      // tiles
      if (!streaming_ && plans[i].tileCount > 0) {
        // 单资产 staging：所有 tile 连续。
        const VkDeviceSize tileBytes = a.tilePayloadBytes();
        const VkDeviceSize stagingSize = tileBytes * plans[i].tileCount;
        GpuBuffer staging;
        if (!createBuffer_(stagingSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                               VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                           staging))
          return rtFail(error, "failed to create tile staging"), false;
        uint8_t *mapped = nullptr;
        if (vkMapMemory(device_, staging.memory, 0, stagingSize, 0,
                        reinterpret_cast<void **>(&mapped)) != VK_SUCCESS) {
          destroyBuffer_(staging);
          return rtFail(error, "failed to map staging"), false;
        }

        atlasCopies.clear();
        atlasCopies.reserve(plans[i].tileCount);
        uint32_t placed = 0;
        for (uint32_t m = 0; m < a.tailFirstMip(); ++m) {
          for (uint32_t ty = 0; ty < a.tilesY(m); ++ty) {
            for (uint32_t tx = 0; tx < a.tilesX(m); ++tx) {
              std::vector<uint8_t> payload;
              std::string readError;
              if (!a.readTile(m, tx, ty, payload, &readError)) {
                vkUnmapMemory(device_, staging.memory);
                destroyBuffer_(staging);
                return rtFail(error, "tile read failed: " + readError), false;
              }
              uint32_t slot = 0;
              if (!cache_.allocateSlot(static_cast<uint32_t>(poolIdx), slot)) {
                vkUnmapMemory(device_, staging.memory);
                destroyBuffer_(staging);
                return rtFail(error, "pool exhausted (bookkeeping mismatch)"), false;
              }
              memcpy(mapped + static_cast<VkDeviceSize>(placed) * tileBytes,
                     payload.data(), payload.size());

              // 页表镜像：bit31 valid | slot（设计 §7.1）。
              pageTableMirror[plans[i].pageBase + a.tileIndex(m, tx, ty)] =
                  0x80000000u | slot;

              const uint32_t slotX = slot % pinfo->config.slotsPerRow;
              const uint32_t slotY = slot / pinfo->config.slotsPerRow;
              VkBufferImageCopy c{};
              c.bufferOffset = static_cast<VkDeviceSize>(placed) * tileBytes;
              c.bufferRowLength = 0;
              c.bufferImageHeight = 0;
              c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
              c.imageSubresource.mipLevel = 0;
              c.imageSubresource.baseArrayLayer = 0;
              c.imageSubresource.layerCount = 1;
              c.imageOffset = {static_cast<int32_t>(slotX * pinfo->paddedTile),
                               static_cast<int32_t>(slotY * pinfo->paddedTile), 0};
              c.imageExtent = {pinfo->paddedTile, pinfo->paddedTile, 1};
              atlasCopies.push_back(c);
              cache_.markSlotResident(poolIdx, slot);
              ++placed;
            }
          }
        }
        vkUnmapMemory(device_, staging.memory);
        vkCmdCopyBufferToImage(osc.cmd, staging.buffer,
                               atlases_[static_cast<size_t>(poolIdx)].image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               static_cast<uint32_t>(atlasCopies.size()),
                               atlasCopies.data());
        // staging 生命周期：单时间命令同步等待后即可销毁。
        if (!osc.endAndSubmit())
          return rtFail(error, "one-shot submit failed"), false;
        destroyBuffer_(staging);
        if (!osc.begin(device_, queue_, cmdPool_))
          return rtFail(error, "failed to re-begin one-shot commands"), false;
      }

      // tail mips
      {
        uint64_t tailTotal = 0;
        for (uint32_t m = a.tailFirstMip(); m < a.mipCount(); ++m)
          tailTotal += a.tailMipPayloadBytes(m);
        GpuBuffer staging;
        if (!createBuffer_(tailTotal, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                               VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                           staging))
          return rtFail(error, "failed to create tail staging"), false;
        uint8_t *mapped = nullptr;
        if (vkMapMemory(device_, staging.memory, 0, tailTotal, 0,
                        reinterpret_cast<void **>(&mapped)) != VK_SUCCESS) {
          destroyBuffer_(staging);
          return rtFail(error, "failed to map tail staging"), false;
        }
        tailCopies.clear();
        VkDeviceSize offset = 0;
        for (uint32_t m = a.tailFirstMip(); m < a.mipCount(); ++m) {
          std::vector<uint8_t> payload;
          std::string readError;
          if (!a.readTailMip(m, payload, &readError)) {
            vkUnmapMemory(device_, staging.memory);
            destroyBuffer_(staging);
            return rtFail(error, "tail read failed: " + readError), false;
          }
          memcpy(mapped + offset, payload.data(), payload.size());
          VkBufferImageCopy c{};
          c.bufferOffset = offset;
          c.bufferRowLength = 0;
          c.bufferImageHeight = 0;
          c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
          c.imageSubresource.mipLevel = m - a.tailFirstMip();
          c.imageSubresource.baseArrayLayer = 0;
          c.imageSubresource.layerCount = 1;
          c.imageOffset = {0, 0, 0};
          c.imageExtent = {a.mipWidth(m), a.mipHeight(m), 1};
          tailCopies.push_back(c);
          offset += payload.size();
        }
        vkUnmapMemory(device_, staging.memory);
        vkCmdCopyBufferToImage(osc.cmd, staging.buffer, tails_[i].image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               static_cast<uint32_t>(tailCopies.size()),
                               tailCopies.data());
        if (!osc.endAndSubmit())
          return rtFail(error, "one-shot submit failed"), false;
        destroyBuffer_(staging);
        // 重新开始一个 batch，用于后续资产的 atlas barrier/copy。
        if (!osc.begin(device_, queue_, cmdPool_))
          return rtFail(error, "failed to re-begin one-shot commands"), false;
      }
    }

    // 4c. atlas/tail: TRANSFER_DST -> SHADER_READ_ONLY（fragment sampled read）。
    for (auto &atlas : atlases_)
      recordImageBarrier(osc.cmd, atlas.image,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_ACCESS_TRANSFER_WRITE_BIT,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         VK_ACCESS_SHADER_READ_BIT,
                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    for (auto &tail : tails_)
      recordImageBarrier(osc.cmd, tail.image,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_ACCESS_TRANSFER_WRITE_BIT,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         VK_ACCESS_SHADER_READ_BIT,
                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    if (!osc.endAndSubmit())
      return rtFail(error, "final barrier submit failed"), false;
  }

  // ---- 5. 元数据 buffer：资产描述、池描述、材质 side table、每 slot 页表
  {
    assetDescs_.resize(assets_.size());
    for (size_t i = 0; i < assets_.size(); ++i) {
      const SvtAsset &a = assets_[i];
      SvtAssetGpuDesc &d = assetDescs_[i];
      d.width = a.width();
      d.height = a.height();
      d.mipCount = a.mipCount();
      d.tailFirstMip = a.tailFirstMip();
      d.pageBase = plans[i].pageBase;
      d.poolIndex = static_cast<uint32_t>(plans[i].poolIndex);
      d.tailTextureIndex = static_cast<uint32_t>(i);
      d.flags = svtFormatIsSrgb(a.format()) ? 1u : 0u;
      // mipPageOffsets：资产内每 tiled mip 的起始偏移（前缀和）。
      uint32_t off = 0;
      for (uint32_t m = 0; m < a.tailFirstMip() && m < 16; ++m) {
        d.mipPageOffsets[m] = off;
        off += a.tilesX(m) * a.tilesY(m);
      }
    }

    if (!createBuffer_(assetDescs_.size() * sizeof(SvtAssetGpuDesc),
                       VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                           VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                       assetDescBuffer_))
      return rtFail(error, "failed to create asset desc buffer"), false;
    uint8_t *mapped = nullptr;
    vkMapMemory(device_, assetDescBuffer_.memory, 0, VK_WHOLE_SIZE, 0,
                reinterpret_cast<void **>(&mapped));
    memcpy(mapped, assetDescs_.data(), assetDescs_.size() * sizeof(SvtAssetGpuDesc));
    vkUnmapMemory(device_, assetDescBuffer_.memory);

    // 池描述
    std::vector<SvtPoolGpuDesc> poolDescs(cache_.poolCount());
    for (size_t p = 0; p < cache_.poolCount(); ++p) {
      const auto *pi = cache_.poolInfo(static_cast<uint32_t>(p));
      poolDescs[p].atlasWidth = pi->atlasWidth;
      poolDescs[p].atlasHeight = pi->atlasHeight;
      poolDescs[p].slotsPerRow = pi->config.slotsPerRow;
      poolDescs[p].slotCount = pi->slotCount;
      poolDescs[p].paddedTile = pi->paddedTile;
      poolDescs[p].tileSize = pi->config.tileSize;
      poolDescs[p].border = pi->config.border;
      poolDescs[p].pad = 0;
    }
    if (!createBuffer_(poolDescs.size() * sizeof(SvtPoolGpuDesc),
                       VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                           VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                       poolDescBuffer_))
      return rtFail(error, "failed to create pool desc buffer"), false;
    vkMapMemory(device_, poolDescBuffer_.memory, 0, VK_WHOLE_SIZE, 0,
                reinterpret_cast<void **>(&mapped));
    memcpy(mapped, poolDescs.data(), poolDescs.size() * sizeof(SvtPoolGpuDesc));
    vkUnmapMemory(device_, poolDescBuffer_.memory);

    // 材质 side table（uint4/material，与 shader SvtMaterialBinding 对应）。
    // 经 assetRemap_ 转换：被剔除资产（-1）的槽回 INVALID 走旧路径。
    const size_t materialCount = info.materialAssets.size() / 4;
    std::vector<uint32_t> remapped(info.materialAssets.size(), 0xFFFFFFFFu);
    materialBindingCount_ = 0;
    for (size_t s = 0; s < info.materialAssets.size(); ++s) {
      const uint32_t v = info.materialAssets[s];
      if (v == 0xFFFFFFFFu || v >= assetRemap_.size())
        continue;
      const int32_t mappedIdx = assetRemap_[v];
      if (mappedIdx < 0)
        continue; // 预算剔除
      remapped[s] = static_cast<uint32_t>(mappedIdx);
      ++materialBindingCount_;
    }
    if (!createBuffer_(materialCount * 16, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                           VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                       materialBindingBuffer_))
      return rtFail(error, "failed to create material binding buffer"), false;
    vkMapMemory(device_, materialBindingBuffer_.memory, 0, VK_WHOLE_SIZE, 0,
                reinterpret_cast<void **>(&mapped));
    memcpy(mapped, remapped.data(), remapped.size() * sizeof(uint32_t));
    vkUnmapMemory(device_, materialBindingBuffer_.memory);

    // 页表快照：每 sync slot 一份相同内容（M3 起独立演进）。
    pageTables_.resize(info.syncSlotCount);
    for (auto &pt : pageTables_) {
      if (!createBuffer_(pageTableBytes_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                             VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                         pt))
        return rtFail(error, "failed to create page table buffer"), false;
      vkMapMemory(device_, pt.memory, 0, VK_WHOLE_SIZE, 0,
                  reinterpret_cast<void **>(&mapped));
      memcpy(mapped, pageTableMirror.data(), pageTableMirror.size() * sizeof(uint32_t));
      vkUnmapMemory(device_, pt.memory);
    }
    // 页表镜像是 init 局部变量，之后资产文件句柄保留用于 M3 读取。
  }

  // ---- 6. sampler（设计 §8.1：首版禁 anisotropy；atlas 无 mip；tail 三线性）
  {
    VkSamplerCreateInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    si.magFilter = VK_FILTER_LINEAR;
    si.minFilter = VK_FILTER_LINEAR;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.maxLod = 0.0f;
    if (vkCreateSampler(device_, &si, nullptr, &atlasSampler_) != VK_SUCCESS)
      return rtFail(error, "failed to create atlas sampler"), false;

    si.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    si.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    si.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    si.maxLod = VK_LOD_CLAMP_NONE;
    if (vkCreateSampler(device_, &si, nullptr, &tailSampler_) != VK_SUCCESS)
      return rtFail(error, "failed to create tail sampler"), false;
  }

  // ---- 7. set2 描述符布局 + pool + sets（binding 编号与 shader 一致）
  {
    const uint32_t poolArraySize = SvtPhysicalCache::kMaxPools; // 8
    const uint32_t tailArraySize = 1024;                        // SVT_MAX_TAILS

    VkDescriptorSetLayoutBinding b[10] = {};
    // 0 资产 SSBO
    b[0] = {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    // 1 材质 side table SSBO
    b[1] = {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    // 2 页表 SSBO
    b[2] = {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    // 3 atlas 数组
    b[3] = {3, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, poolArraySize,
            VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    // 4 tail 数组
    b[4] = {4, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, tailArraySize,
            VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    // 5 atlas sampler
    b[5] = {5, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    // 6 tail sampler
    b[6] = {6, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    // 7 池元数据 SSBO
    b[7] = {7, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    // 8 SVT frame params UBO（M3，设计 §8.1）
    b[8] = {8, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    // 9 反馈 UAV（M3，设计 §8.3；fragment 写需 fragmentStoresAndAtomics）
    b[9] = {9, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};

    VkDescriptorSetLayoutCreateInfo li{};
    li.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    li.bindingCount = 10;
    li.pBindings = b;
    if (vkCreateDescriptorSetLayout(device_, &li, nullptr, &svtSetLayout_) != VK_SUCCESS)
      return rtFail(error, "failed to create svt set layout"), false;

    // descriptor 索引（无 update-after-bind；场景加载期一次写入 + 反馈 per-slot）。
    std::vector<VkDescriptorPoolSize> sizes = {
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 6 * info.syncSlotCount},
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1 * info.syncSlotCount},
        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
         (poolArraySize + tailArraySize) * info.syncSlotCount},
        {VK_DESCRIPTOR_TYPE_SAMPLER, 2 * info.syncSlotCount}};
    VkDescriptorPoolCreateInfo pi{};
    pi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pi.maxSets = info.syncSlotCount;
    pi.poolSizeCount = static_cast<uint32_t>(sizes.size());
    pi.pPoolSizes = sizes.data();
    if (vkCreateDescriptorPool(device_, &pi, nullptr, &svtDescriptorPool_) != VK_SUCCESS)
      return rtFail(error, "failed to create svt descriptor pool"), false;

    svtDescriptorSets_.resize(info.syncSlotCount);
    std::vector<VkDescriptorSetLayout> layouts(info.syncSlotCount, svtSetLayout_);
    VkDescriptorSetAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    ai.descriptorPool = svtDescriptorPool_;
    ai.descriptorSetCount = info.syncSlotCount;
    ai.pSetLayouts = layouts.data();
    if (vkAllocateDescriptorSets(device_, &ai, svtDescriptorSets_.data()) != VK_SUCCESS)
      return rtFail(error, "failed to allocate svt descriptor sets"), false;

    for (uint32_t s = 0; s < info.syncSlotCount; ++s) {
      VkDescriptorBufferInfo abi{assetDescBuffer_.buffer, 0, VK_WHOLE_SIZE};
      VkDescriptorBufferInfo mbi{materialBindingBuffer_.buffer, 0, VK_WHOLE_SIZE};
      VkDescriptorBufferInfo pbi{pageTables_[s].buffer, 0, VK_WHOLE_SIZE};
      VkDescriptorBufferInfo dbi{poolDescBuffer_.buffer, 0, VK_WHOLE_SIZE};

      std::vector<VkDescriptorImageInfo> atlasInfos(poolArraySize);
      // 数组以固定容量完整填充有效资源（设计 §8.1）。
      for (uint32_t k = 0; k < poolArraySize; ++k) {
        const uint32_t src = std::min<uint32_t>(k, static_cast<uint32_t>(atlases_.size()) - 1);
        atlasInfos[k] = {VK_NULL_HANDLE, atlases_[src].view,
                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
      }
      std::vector<VkDescriptorImageInfo> tailInfos(tailArraySize);
      for (uint32_t k = 0; k < tailArraySize; ++k) {
        const uint32_t src = std::min<uint32_t>(k, static_cast<uint32_t>(tails_.size()) - 1);
        tailInfos[k] = {VK_NULL_HANDLE, tails_[src].view,
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
      }
      VkDescriptorImageInfo atlasSamplerInfo{atlasSampler_, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
      VkDescriptorImageInfo tailSamplerInfo{tailSampler_, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};

      VkWriteDescriptorSet w[8] = {};
      for (int k = 0; k < 8; ++k)
        w[k].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      w[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
              svtDescriptorSets_[s], 0, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &abi, nullptr};
      w[1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
              svtDescriptorSets_[s], 1, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &mbi, nullptr};
      w[2] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
              svtDescriptorSets_[s], 2, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &pbi, nullptr};
      w[3] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
              svtDescriptorSets_[s], 3, 0, poolArraySize, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
              atlasInfos.data(), nullptr, nullptr};
      w[4] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
              svtDescriptorSets_[s], 4, 0, tailArraySize, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
              tailInfos.data(), nullptr, nullptr};
      w[5] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
              svtDescriptorSets_[s], 5, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLER,
              &atlasSamplerInfo, nullptr, nullptr};
      w[6] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
              svtDescriptorSets_[s], 6, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLER,
              &tailSamplerInfo, nullptr, nullptr};
      w[7] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
              svtDescriptorSets_[s], 7, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &dbi, nullptr};
      vkUpdateDescriptorSets(device_, 8, w, 0, nullptr);
    }
    // binding 8/9（frame params / feedback UAV）per-slot，由 initFeedback_ 写。
  }

  // ---- 8. 反馈资源（每 sync slot：feedback buffer + readback + params UBO）
  if (!initFeedback_(info.syncSlotCount, error))
    return false;

  pageMirror_ = std::move(pageTableMirror);
  slotSerials_.assign(info.syncSlotCount, 0);
  feedbackValid_.assign(info.syncSlotCount, false);
  uploadStaging_.resize(info.syncSlotCount);
  if (streaming_) {
    system_ = std::make_unique<SvtSystem>();
    streamer_ = std::make_unique<SvtTileStreamer>(2, 256);
    for (uint32_t i = 0; i < assets_.size(); ++i) {
      const auto &a = assets_[i];
      SvtSystem::AssetDesc d;
      d.width=a.width(); d.height=a.height(); d.mipCount=a.mipCount();
      d.tailFirstMip=a.tailFirstMip(); d.tileSize=a.tileSize(); d.tilePayloadBytes=a.tilePayloadBytes();
      system_->registerAsset(d);
    }
    for (size_t old = 0; old < assetRemap_.size(); ++old)
      if (assetRemap_[old] >= 0) streamer_->registerAsset(assetRemap_[old], info.assetFiles[old]);
    for (auto &staging : uploadStaging_)
      if (!createBuffer_(kUploadBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, staging))
        return rtFail(error, "upload staging allocation failed"), false;
  }
  initialized_ = true;
  return true;
}

VkDescriptorSet SvtVulkanRuntime::descriptorSet(uint32_t syncSlot) const {
  if (syncSlot >= svtDescriptorSets_.size())
    return VK_NULL_HANDLE;
  return svtDescriptorSets_[syncSlot];
}

SvtVulkanRuntime::Stats SvtVulkanRuntime::stats() const {
  Stats s;
  s.poolCount = cache_.poolCount();
  for (size_t p = 0; p < cache_.poolCount(); ++p) {
    SvtPhysicalCache::PoolUsage u;
    if (cache_.poolUsage(static_cast<uint32_t>(p), u))
      { s.residentTiles += u.residentCount; s.retiringTiles += u.retiringCount; s.freeTiles += u.freeCount; }
  }
  s.totalTiles = totalTiles_;
  s.atlasBytes = actualAtlasBytes_;
  s.tailBytes = tailBytes_;
  s.pageTableBytes = pageTableBytes_;
  s.materialBindings = materialBindingCount_;
  s.uploadedPages=uploadedPages_; s.uploadedBytes=uploadedBytes_; s.evictions=evictions_;
  if (system_) { s.pendingRequests=system_->pendingCount(); s.bufferedBytes=system_->bufferedBytes(); }
  if (streamer_) { s.readBytes=streamer_->totalReadBytes(); s.failures=streamer_->totalFailCount(); }
  return s;
}

// ---------------------------------------------------------------------------
// 反馈资源（M3，设计 §8.3、§10.1）
// ---------------------------------------------------------------------------

bool SvtVulkanRuntime::initFeedback_(uint32_t slotCount, std::string *error) {
  destroyFeedback_();
  feedback_.resize(slotCount);
  const VkDeviceSize bufferSize =
      16 + static_cast<VkDeviceSize>(kFeedbackCapacity) * 8; // header + uint2*N

  for (uint32_t s = 0; s < slotCount; ++s) {
    FeedbackResources &f = feedback_[s];
    // feedback：device-local，UAV 写（fragment storage）+ transfer（fill 是
    // dst、copy 是 src，两个 usage 都需要）。
    if (!createBuffer_(bufferSize,
                       VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                           VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                           VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                       VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, f.buffer))
      return rtFail(error, "failed to create feedback buffer"), false;
    // readback：host-visible，transfer dst + host read。
    if (!createBuffer_(bufferSize,
                       VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                           VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                       f.readback))
      return rtFail(error, "failed to create readback buffer"), false;
    // params UBO：host-visible 持久映射。
    if (!createBuffer_(32, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                           VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                       f.params))
      return rtFail(error, "failed to create frame params buffer"), false;
    if (vkMapMemory(device_, f.params.memory, 0, VK_WHOLE_SIZE, 0,
                    &f.paramsMapped) != VK_SUCCESS) {
      f.paramsMapped = nullptr;
      return rtFail(error, "failed to map frame params"), false;
    }
    // 初始 params（feedback 关闭直到调用方开启；capacity/frame 每帧更新）。
    memset(f.paramsMapped, 0, 32);

    // binding 8（params UBO）/9（feedback UAV）写入本 slot 的 set。
    VkDescriptorBufferInfo pbi{f.params.buffer, 0, VK_WHOLE_SIZE};
    VkDescriptorBufferInfo fbi{f.buffer.buffer, 0, VK_WHOLE_SIZE};
    VkWriteDescriptorSet w[2] = {};
    w[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w[0].dstSet = svtDescriptorSets_[s];
    w[0].dstBinding = 8;
    w[0].descriptorCount = 1;
    w[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    w[0].pBufferInfo = &pbi;
    w[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w[1].dstSet = svtDescriptorSets_[s];
    w[1].dstBinding = 9;
    w[1].descriptorCount = 1;
    w[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    w[1].pBufferInfo = &fbi;
    vkUpdateDescriptorSets(device_, 2, w, 0, nullptr);
  }

  // 反馈 buffer 初始清零（header attemptCount 必须 0；一次性命令，加载期）。
  {
    OneShotCommands osc;
    if (!osc.begin(device_, queue_, cmdPool_))
      return rtFail(error, "feedback init: begin failed"), false;
    for (auto &f : feedback_) {
      VkMemoryBarrier b{};
      b.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
      b.srcAccessMask = 0;
      b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
      vkCmdPipelineBarrier(osc.cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                           VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &b, 0, nullptr,
                           0, nullptr);
      vkCmdFillBuffer(osc.cmd, f.buffer.buffer, 0, VK_WHOLE_SIZE, 0);
    }
    if (!osc.endAndSubmit())
      return rtFail(error, "feedback init: submit failed"), false;
  }
  return true;
}

void SvtVulkanRuntime::destroyFeedback_() {
  for (auto &f : feedback_) {
    if (f.paramsMapped) {
      vkUnmapMemory(device_, f.params.memory);
      f.paramsMapped = nullptr;
    }
    destroyBuffer_(f.buffer);
    destroyBuffer_(f.readback);
    destroyBuffer_(f.params);
  }
  feedback_.clear();
}

void SvtVulkanRuntime::updateFrameParams(uint32_t syncSlot, uint32_t frameIndex,
                                         uint32_t feedbackFactor,
                                         bool feedbackEnabled, float mipBias) {
  // 与 shader SvtFrameParams 布局一致（8 uint = 32B）。
  struct Params {
    uint32_t feedbackCapacity;
    uint32_t frameIndex;
    uint32_t feedbackFactor;
    uint32_t feedbackEnabled;
    float mipBias;
    uint32_t generationBase;
    uint32_t pad0;
    uint32_t pad1;
  };
  static_assert(sizeof(Params) == 32, "SvtFrameParams layout mismatch");
  if (syncSlot < feedback_.size()) {
    auto &f = feedback_[syncSlot];
    if (!f.paramsMapped) return;
    Params p{};
    p.feedbackCapacity = kFeedbackCapacity;
    p.frameIndex = frameIndex;
    p.feedbackFactor = feedbackFactor ? feedbackFactor : 8;
    p.feedbackEnabled = feedbackEnabled ? 1u : 0u;
    p.mipBias = mipBias;
    p.pad0 = debugView_;
    memcpy(f.paramsMapped, &p, sizeof(p));
  }
}

void SvtVulkanRuntime::recordFeedbackClear(VkCommandBuffer cmd, uint32_t syncSlot) {
  if (syncSlot >= feedback_.size())
    return;
  // 清 header（16B）为 0：transfer write -> 后续 fragment read/write（设计 §10.1）。
  // buffer 全程无 layout（纯 buffer），barrier 用 memory barrier。
  VkMemoryBarrier pre{};
  pre.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
  pre.srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
  pre.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                       VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &pre, 0, nullptr,
                       0, nullptr);
  vkCmdFillBuffer(cmd, feedback_[syncSlot].buffer.buffer, 0, 16, 0);
  VkMemoryBarrier post{};
  post.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
  post.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  post.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 1, &post, 0,
                       nullptr, 0, nullptr);
}

void SvtVulkanRuntime::recordFeedbackReadback(VkCommandBuffer cmd,
                                              uint32_t syncSlot) {
  if (syncSlot >= feedback_.size())
    return;
  // fragment writes -> transfer reads（设计 §8.3/§10.1：所有请求写完之后才复制）。
  VkMemoryBarrier b{};
  b.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
  b.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
  b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &b, 0, nullptr, 0,
                       nullptr);
  feedbackValid_[syncSlot] = true;
  const VkDeviceSize size =
      16 + static_cast<VkDeviceSize>(kFeedbackCapacity) * 8;
  VkBufferCopy region{0, 0, size};
  vkCmdCopyBuffer(cmd, feedback_[syncSlot].buffer.buffer,
                  feedback_[syncSlot].readback.buffer, 1, &region);
  VkMemoryBarrier post{};
  post.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
  post.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  post.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &post, 0, nullptr, 0,
                       nullptr);
}

bool SvtVulkanRuntime::readbackFeedback(uint32_t syncSlot,
                                        std::vector<uint32_t> &outWords) {
  outWords.clear();
  if (syncSlot >= feedback_.size() || !feedbackValid_[syncSlot])
    return false;
  feedbackValid_[syncSlot] = false;
  const FeedbackResources &f = feedback_[syncSlot];
  void *mapped = nullptr;
  if (vkMapMemory(device_, f.readback.memory, 0, VK_WHOLE_SIZE, 0, &mapped) !=
      VK_SUCCESS)
    return false;
  const uint32_t *words = static_cast<const uint32_t *>(mapped);
  const uint32_t attemptCount = words[0];
  feedbackAttempts_ = attemptCount;
  const uint32_t used = attemptCount < kFeedbackCapacity ? attemptCount
                                                         : kFeedbackCapacity;
  feedbackOverflow_ =
      attemptCount > kFeedbackCapacity ? attemptCount - kFeedbackCapacity : 0;
  outWords.resize(4 + static_cast<size_t>(used) * 2);
  memcpy(outWords.data(), words, outWords.size() * sizeof(uint32_t));
  vkUnmapMemory(device_, f.readback.memory);
  return true;
}

void SvtVulkanRuntime::beginFrame(uint32_t slot, uint32_t frame, bool capture,
                                 bool pauseReads, float mipBias) {
  if (slot >= slotSerials_.size()) throw std::runtime_error("SVT sync slot out of range");
  activeSlot_ = slot; activeFrame_ = frame;
  completedSerial_ = (std::max)(completedSerial_, slotSerials_[slot]);
  cache_.collectRetired(completedSerial_);
  updateFrameParams(slot, frame, 8, capture, mipBias);
  std::vector<uint32_t> words;
  if (readbackFeedback(slot, words) && system_ && capture) {
    // Aggregate raw duplicates before walking ancestors (large flat surfaces).
    std::unordered_map<uint64_t, uint32_t> unique;
    for (size_t i = 4; i + 1 < words.size(); i += 2)
      ++unique[(uint64_t(words[i]) << 32) | words[i+1]];
    feedbackUnique_ = static_cast<uint32_t>(unique.size());
    for (const auto &[packed, count] : unique) {
      SvtFeedbackItem f;
      if (!svtDecodeFeedback(uint32_t(packed >> 32), uint32_t(packed), f)) continue;
      system_->ingestFeedback(&f, 1, frame);
      // Count is only a priority hint; cap work without losing hit/ancestor touch.
      if (auto *entry = const_cast<SvtResidencyEntry*>(system_->entry(
          {f.assetIndex, f.generation, f.mip, f.pageX, f.pageY})))
        entry->count = std::min<uint64_t>(UINT32_MAX, entry->count + count - 1);
    }
  }
  if (!system_) return;
  for (auto &[key, resident] : resident_)
    if (const auto *entry = system_->entry(key)) resident.lastSeen = entry->lastSeenFrame;

  std::vector<SvtTileReadResult> results;
  streamer_->drainCompleted(results);
  for (auto &r : results) {
    if (r.success && system_->completeRead(r.key, r.payload.size()))
      readyPayloads_[r.key] = std::move(r.payload);
    else if (!r.success) system_->failRead(r.key, frame);
  }
  frameBatch_ = system_->prepareFrame(frame);
  if (!pauseReads) for (const auto &r : frameBatch_.reads) {
    if (!system_->beginRead(r.key)) continue;
    if (!streamer_->submitRead(r.key)) system_->cancelRead(r.key);
  }
}

size_t SvtVulkanRuntime::recordUploads(VkCommandBuffer cmd,
    const UploadRequest *requests, size_t count, std::vector<bool> *accepted) {
  if (accepted) accepted->assign(count, false);
  if (!streaming_) return 0;
  pendingUploads_.clear();
  std::vector<uint32_t> table = pageMirror_;
  VkDeviceSize offset = 0;
  void *mapped = nullptr;
  if (vkMapMemory(device_, uploadStaging_[activeSlot_].memory, 0, kUploadBytes, 0, &mapped) != VK_SUCCESS)
    throw std::runtime_error("SVT staging map failed");
  std::vector<bool> transitioned(atlases_.size(), false);
  for (size_t i = 0; i < count; ++i) {
    const auto &r = requests[i];
    if (r.assetIndex >= assets_.size()) continue;
    const auto &a = assets_[r.assetIndex];
    if (r.mip >= a.tailFirstMip() || r.x >= a.tilesX(r.mip) || r.y >= a.tilesY(r.mip) ||
        !r.payload || r.payloadSize != a.tilePayloadBytes()) continue;
    offset = (offset + 15) & ~VkDeviceSize(15);
    if (r.payloadSize > kUploadBytes - offset) continue;
    const uint32_t pool = assetDescs_[r.assetIndex].poolIndex;
    uint32_t slot;
    if (!cache_.allocateSlot(pool, slot)) {
      // Retire at most one cold resident per blocked request. Future snapshots
      // always rebuild from pageMirror_, so completed dormant snapshots are safe.
      auto victim = resident_.end();
      if (feedbackOverflow_ == 0) for (auto it = resident_.begin(); it != resident_.end(); ++it) {
        if (it->second.pool != pool || it->second.lastSeen + 8 >= activeFrame_) continue;
        if (victim == resident_.end() || it->second.lastSeen < victim->second.lastSeen)
          victim = it;
      }
      if (victim != resident_.end()) {
        const auto key = victim->first;
        const uint32_t page = assetDescs_[key.assetIndex].pageBase +
            assets_[key.assetIndex].tileIndex(key.mip, key.x, key.y);
        pageMirror_[page] = table[page] = 0;
        cache_.retireSlot(pool, victim->second.slot, submittedSerial_);
        system_->evictResident(key);
        resident_.erase(victim); ++evictions_;
        cache_.collectRetired(completedSerial_);
      }
      if (!cache_.allocateSlot(pool, slot)) continue;
    }
    if (!transitioned[pool]) {
      recordImageBarrier(cmd, atlases_[pool].image,
          VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT,
          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
          VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
      transitioned[pool] = true;
    }
    memcpy(static_cast<uint8_t*>(mapped) + offset, r.payload, r.payloadSize);
    const auto *pi = cache_.poolInfo(pool);
    VkBufferImageCopy copy{};
    copy.bufferOffset = offset;
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageOffset = {int32_t(slot % pi->config.slotsPerRow * pi->paddedTile),
                        int32_t(slot / pi->config.slotsPerRow * pi->paddedTile), 0};
    copy.imageExtent = {pi->paddedTile, pi->paddedTile, 1};
    vkCmdCopyBufferToImage(cmd, uploadStaging_[activeSlot_].buffer, atlases_[pool].image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    const auto page = assetDescs_[r.assetIndex].pageBase + a.tileIndex(r.mip,r.x,r.y);
    table[page] = 0x80000000u | slot;
    pendingUploads_.push_back({{r.assetIndex,0,r.mip,r.x,r.y},pool,slot,page,offset});
    if (accepted) (*accepted)[i] = true;
    offset += r.payloadSize;
  }
  vkUnmapMemory(device_, uploadStaging_[activeSlot_].memory);
  for (uint32_t p = 0; p < transitioned.size(); ++p) if (transitioned[p])
    recordImageBarrier(cmd, atlases_[p].image, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  // Slot fence was waited before beginFrame. Coherent host writes are made
  // available by queue submission; shader sees tiles only after copy barriers.
  if (vkMapMemory(device_, pageTables_[activeSlot_].memory, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS)
    throw std::runtime_error("SVT page table map failed");
  if (!table.empty()) memcpy(mapped, table.data(), table.size()*sizeof(uint32_t));
  else memset(mapped, 0, 4);
  vkUnmapMemory(device_, pageTables_[activeSlot_].memory);
  batchRecorded_ = true;
  return pendingUploads_.size();
}

void SvtVulkanRuntime::recordFrameUploads(VkCommandBuffer cmd) {
  std::vector<UploadRequest> requests;
  if (!streaming_) return;
  for (const auto &r : frameBatch_.uploads) {
    auto it = readyPayloads_.find(r.key);
    if (it == readyPayloads_.end()) { system_->cancelUpload(r.key); continue; }
    requests.push_back({r.key.assetIndex,r.key.mip,r.key.x,r.key.y,
                        it->second.data(),uint32_t(it->second.size())});
  }
  std::vector<bool> accepted;
  recordUploads(cmd, requests.data(), requests.size(), &accepted);
  for (size_t i = 0; i < requests.size(); ++i) if (!accepted[i]) {
    const auto &r = requests[i];
    system_->cancelUpload({r.assetIndex,0,r.mip,r.x,r.y});
  }
}

void SvtVulkanRuntime::onSubmitted() {
  slotSerials_[activeSlot_] = ++submittedSerial_;
  for (const auto &p : pendingUploads_) {
    pageMirror_[p.page] = 0x80000000u | p.slot;
    cache_.markSlotResident(p.pool,p.slot);
    resident_[p.key] = {p.pool,p.slot,activeFrame_};
    system_->commitUploaded(p.key);
    uploadedBytes_ += readyPayloads_[p.key].size(); ++uploadedPages_;
    readyPayloads_.erase(p.key);
  }
  pendingUploads_.clear(); batchRecorded_=false;
  if (submittedSerial_ % 120 == 0) {
    const auto s = stats();
    spdlog::info("svt progress: frame={} resident={} pending={} uploaded={} bytes={} evicted={} ioFailures={} overflow={}",
        submittedSerial_, s.residentTiles, s.pendingRequests, uploadedPages_, uploadedBytes_, evictions_,
        streamer_ ? streamer_->totalFailCount() : 0, feedbackOverflow_);
  }
}

void SvtVulkanRuntime::cancelFrame() {
  for (const auto &p : pendingUploads_) cache_.releaseSlot(p.pool,p.slot);
  for (const auto &r : frameBatch_.uploads) if (system_) system_->cancelUpload(r.key);
  pendingUploads_.clear(); batchRecorded_=false;
  if (activeSlot_ < feedbackValid_.size()) feedbackValid_[activeSlot_]=false;
}
void SvtVulkanRuntime::commitUploads(const UploadRequest*, size_t, const std::vector<bool>&) { onSubmitted(); }
void SvtVulkanRuntime::cancelUploads(const UploadRequest*, size_t, const std::vector<bool>&) { cancelFrame(); }

} // namespace svt
