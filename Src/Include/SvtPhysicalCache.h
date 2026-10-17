#pragma once
// SVT 物理缓存（格式分池）的 CPU 侧簿记：槽位几何、分配/释放、预算统计。
// 设计依据：docs/superpowers/specs/2026-10-16-streaming-virtual-texture-design.md §7.2、§12。
//
// 本模块不创建 Vulkan 资源（GPU 侧归 SvtVulkanRuntime），不决定材质 UV/LOD。
// 槽位几何（slot -> slotX/slotY）必须与 shaders/svt_common.hlsl 的计算一致。
//
// 槽状态机（设计 §7.2）：
//   Free -> Reserved -> UploadQueued -> Resident -> Retiring -> Free
// M2（全驻留）只使用 Free/Reserved/Resident；UploadQueued/Retiring 与
// 退休协议（serial）在 M3/M4 启用。首版保持状态单向清晰。

#include "SvtAsset.h"

#include <cstdint>
#include <string>
#include <vector>

namespace svt {

enum class SvtSlotState : uint8_t {
  Free = 0,
  Reserved = 1,     // 已预留给某 tile，payload 尚未安排上传
  UploadQueued = 2, // 已进入某帧上传批次（M3+）
  Resident = 3,     // payload 已在 atlas 上
  Retiring = 4,     // 等待退休序号完成（M4）
};

struct SvtPoolConfig {
  SvtFormat format = SvtFormat::RGBA8;
  uint32_t tileSize = 128; // T
  uint32_t border = 4;     // B
  uint32_t slotsPerRow = 0;
  uint32_t rows = 0;
};

// 给定需求槽数与限制，规划一个尽量接近方形的池网格。
// 返回 false 表示连一行都放不下（尺寸限制或预算为 0）。
struct SvtPoolPlan {
  uint32_t slotsPerRow = 0;
  uint32_t rows = 0;
  uint32_t capacity = 0;   // slotsPerRow * rows
  uint64_t atlasBytes = 0; // 理论 payload 记账（BC 按 block 网格）
  bool truncated = false;  // true = 预算/尺寸导致容量小于需求
};
bool svtPlanPool(SvtFormat format, uint32_t tileSize, uint32_t border,
                 uint64_t desiredSlots, uint32_t atlasDimensionLimit,
                 uint64_t byteBudget, SvtPoolPlan &out);

class SvtPhysicalCache {
public:
  struct PoolInfo {
    SvtPoolConfig config;
    uint32_t paddedTile = 0; // P = T + 2B
    uint32_t slotCount = 0;  // slotsPerRow * rows
    uint32_t atlasWidth = 0;
    uint32_t atlasHeight = 0;
    uint64_t atlasBytes = 0; // 理论 payload 记账；GPU 预算以 vkGetImageMemoryRequirements 为准
  };

  // 创建池并返回 poolIndex；相同 {format, T, B} 的池已存在时直接返回其索引。
  // 失败（参数非法/超上限）返回 -1 并写 error。
  int createPool(const SvtPoolConfig &config, std::string *error = nullptr);
  int findPool(SvtFormat format, uint32_t tileSize, uint32_t border) const;

  // 分配一个空闲槽（Free -> Reserved）。池满返回 false。
  bool allocateSlot(uint32_t poolIndex, uint32_t &slotOut);
  // Reserved/UploadQueued -> Resident（payload 上传安排确认后调用）。
  bool markSlotResident(uint32_t poolIndex, uint32_t slot);
  // 释放槽回到 Free。M4 起淘汰走 Retiring 协议而非直接 release。
  // 对 Free 槽调用是错误，返回 false。
  bool releaseSlot(uint32_t poolIndex, uint32_t slot);
  bool retireSlot(uint32_t poolIndex, uint32_t slot, uint64_t lastUseSerial);
  void collectRetired(uint64_t completedSerial);

  size_t poolCount() const { return pools_.size(); }
  const PoolInfo *poolInfo(uint32_t poolIndex) const;
  SvtSlotState slotState(uint32_t poolIndex, uint32_t slot) const;

  // 各状态的槽数统计（ImGui 展示用，设计 §12）。
  struct PoolUsage {
    uint32_t freeCount = 0;
    uint32_t reservedCount = 0;
    uint32_t uploadQueuedCount = 0;
    uint32_t residentCount = 0;
    uint32_t retiringCount = 0;
  };
  bool poolUsage(uint32_t poolIndex, PoolUsage &out) const;

  uint64_t totalAtlasBytes() const; // 所有池的 payload 记账总和

  // 槽位几何（与 shader 一致：slotX = slot % slotsPerRow）。
  static void slotToXY(uint32_t slotsPerRow, uint32_t slot, uint32_t &x, uint32_t &y);
  static uint32_t paddedTileSize(uint32_t tileSize, uint32_t border) {
    return tileSize + 2 * border;
  }
  // 理论 payload 字节数（BC: (w/4)*(h/4)*bytesPerBlock；RGBA8: w*h*4）。
  static uint64_t atlasByteSize(SvtFormat format, uint32_t atlasWidth, uint32_t atlasHeight);

  static constexpr uint32_t kMaxPools = 8; // 与 shader SVT_MAX_POOLS 一致

private:
  struct Pool {
    PoolInfo info;
    std::vector<SvtSlotState> states;
    std::vector<uint64_t> retireSerials;
    std::vector<uint32_t> freeList;
  };
  std::vector<Pool> pools_;
};

} // namespace svt
