// SVT 物理缓存簿记实现（设计 §7.2、§12）。

#include "SvtPhysicalCache.h"

#include <algorithm>
#include <cmath>

namespace svt {

// ---------------------------------------------------------------------------
// 池规划
// ---------------------------------------------------------------------------

uint64_t SvtPhysicalCache::atlasByteSize(SvtFormat format, uint32_t atlasWidth,
                                         uint32_t atlasHeight) {
  return svtMipByteSize(format, atlasWidth, atlasHeight);
}

bool svtPlanPool(SvtFormat format, uint32_t tileSize, uint32_t border,
                 uint64_t desiredSlots, uint32_t atlasDimensionLimit,
                 uint64_t byteBudget, SvtPoolPlan &out) {
  out = SvtPoolPlan{};
  if (tileSize == 0 || border * 2 >= tileSize || desiredSlots == 0)
    return false;
  if (svtFormatIsBlockCompressed(format)) {
    // P 必须按 4 texel block 对齐（设计 §6.2）。
    if (tileSize % 4 != 0 || border % 4 != 0)
      return false;
  }
  const uint32_t P = SvtPhysicalCache::paddedTileSize(tileSize, border);
  const uint32_t maxPerRow = atlasDimensionLimit / P; // floor（设计 §7.2）
  if (maxPerRow == 0)
    return false;

  // 尽量接近方形：slotsPerRow = ceil(sqrt(desired))，clamp 到行上限。
  const double sqrtDesired = std::ceil(std::sqrt(static_cast<double>(desiredSlots)));
  uint64_t slotsPerRow = std::min<uint64_t>(maxPerRow,
                                            std::max<uint64_t>(1, static_cast<uint64_t>(sqrtDesired)));
  uint64_t rows = (desiredSlots + slotsPerRow - 1) / slotsPerRow;
  rows = std::min<uint64_t>(rows, atlasDimensionLimit / P);

  // 预算收敛：逐行减少直到放得下；一行都放不下则失败。
  auto bytesFor = [&](uint64_t r) {
    return SvtPhysicalCache::atlasByteSize(format, static_cast<uint32_t>(slotsPerRow * P),
                                           static_cast<uint32_t>(r * P));
  };
  while (rows > 0 && bytesFor(rows) > byteBudget)
    --rows;
  if (rows == 0)
    return false;

  out.slotsPerRow = static_cast<uint32_t>(slotsPerRow);
  out.rows = static_cast<uint32_t>(rows);
  out.capacity = out.slotsPerRow * out.rows;
  out.atlasBytes = bytesFor(rows);
  out.truncated = out.capacity < desiredSlots;
  return true;
}

// ---------------------------------------------------------------------------
// SvtPhysicalCache
// ---------------------------------------------------------------------------

int SvtPhysicalCache::createPool(const SvtPoolConfig &config, std::string *error) {
  // 参数校验先于 key 复用：非法配置即使撞上已有 key 也必须拒绝。
  if (config.tileSize == 0 || config.border * 2 >= config.tileSize ||
      config.slotsPerRow == 0 || config.rows == 0) {
    if (error)
      *error = "invalid pool config";
    return -1;
  }
  if (svtFormatIsBlockCompressed(config.format) &&
      (config.tileSize % 4 != 0 || config.border % 4 != 0)) {
    if (error)
      *error = "BC pool tile/border not block aligned";
    return -1;
  }

  const int existing = findPool(config.format, config.tileSize, config.border);
  if (existing >= 0)
    return existing;

  if (pools_.size() >= kMaxPools) {
    if (error)
      *error = "pool count exceeds SVT_MAX_POOLS";
    return -1;
  }

  Pool pool;
  pool.info.config = config;
  pool.info.paddedTile = paddedTileSize(config.tileSize, config.border);
  pool.info.slotCount = config.slotsPerRow * config.rows;
  pool.info.atlasWidth = config.slotsPerRow * pool.info.paddedTile;
  pool.info.atlasHeight = config.rows * pool.info.paddedTile;
  pool.info.atlasBytes = atlasByteSize(config.format, pool.info.atlasWidth,
                                       pool.info.atlasHeight);
  if (pool.info.slotCount > 0x100000u) { // slot 位宽 [19:0]：最大 2^20 个槽
    if (error)
      *error = "slot count exceeds page-table slot bits";
    return -1;
  }

  pool.states.assign(pool.info.slotCount, SvtSlotState::Free);
  pool.retireSerials.assign(pool.info.slotCount, 0);
  pool.freeList.reserve(pool.info.slotCount);
  for (uint32_t i = 0; i < pool.info.slotCount; ++i)
    pool.freeList.push_back(pool.info.slotCount - 1 - i); // pop 出递增序号
  pools_.push_back(std::move(pool));
  return static_cast<int>(pools_.size() - 1);
}

int SvtPhysicalCache::findPool(SvtFormat format, uint32_t tileSize, uint32_t border) const {
  for (size_t i = 0; i < pools_.size(); ++i)
    if (pools_[i].info.config.format == format &&
        pools_[i].info.config.tileSize == tileSize &&
        pools_[i].info.config.border == border)
      return static_cast<int>(i);
  return -1;
}

bool SvtPhysicalCache::allocateSlot(uint32_t poolIndex, uint32_t &slotOut) {
  if (poolIndex >= pools_.size())
    return false;
  Pool &pool = pools_[poolIndex];
  if (pool.freeList.empty())
    return false;
  const uint32_t slot = pool.freeList.back();
  pool.freeList.pop_back();
  if (pool.states[slot] != SvtSlotState::Free)
    return false; // 簿记损坏（防御）
  pool.states[slot] = SvtSlotState::Reserved;
  slotOut = slot;
  return true;
}

bool SvtPhysicalCache::markSlotResident(uint32_t poolIndex, uint32_t slot) {
  if (poolIndex >= pools_.size())
    return false;
  Pool &pool = pools_[poolIndex];
  if (slot >= pool.states.size())
    return false;
  if (pool.states[slot] == SvtSlotState::Resident)
    return true; // 幂等：重复确认不改变状态
  if (pool.states[slot] != SvtSlotState::Reserved &&
      pool.states[slot] != SvtSlotState::UploadQueued)
    return false;
  pool.states[slot] = SvtSlotState::Resident;
  return true;
}

bool SvtPhysicalCache::releaseSlot(uint32_t poolIndex, uint32_t slot) {
  if (poolIndex >= pools_.size())
    return false;
  Pool &pool = pools_[poolIndex];
  if (slot >= pool.states.size())
    return false;
  if (pool.states[slot] != SvtSlotState::Reserved &&
      pool.states[slot] != SvtSlotState::UploadQueued)
    return false;
  pool.states[slot] = SvtSlotState::Free;
  pool.freeList.push_back(slot);
  return true;
}

bool SvtPhysicalCache::retireSlot(uint32_t poolIndex, uint32_t slot,
                                uint64_t lastUseSerial) {
  if (poolIndex >= pools_.size()) return false;
  auto &p = pools_[poolIndex];
  if (slot >= p.states.size() || p.states[slot] != SvtSlotState::Resident)
    return false;
  p.states[slot] = SvtSlotState::Retiring;
  p.retireSerials[slot] = lastUseSerial;
  return true;
}

void SvtPhysicalCache::collectRetired(uint64_t completedSerial) {
  for (auto &p : pools_)
    for (uint32_t i = 0; i < p.states.size(); ++i)
      if (p.states[i] == SvtSlotState::Retiring &&
          p.retireSerials[i] <= completedSerial) {
        p.states[i] = SvtSlotState::Free;
        p.freeList.push_back(i);
      }
}

const SvtPhysicalCache::PoolInfo *SvtPhysicalCache::poolInfo(uint32_t poolIndex) const {
  if (poolIndex >= pools_.size())
    return nullptr;
  return &pools_[poolIndex].info;
}

SvtSlotState SvtPhysicalCache::slotState(uint32_t poolIndex, uint32_t slot) const {
  if (poolIndex >= pools_.size() || slot >= pools_[poolIndex].states.size())
    return SvtSlotState::Free;
  return pools_[poolIndex].states[slot];
}

bool SvtPhysicalCache::poolUsage(uint32_t poolIndex, PoolUsage &out) const {
  if (poolIndex >= pools_.size())
    return false;
  out = PoolUsage{};
  for (SvtSlotState s : pools_[poolIndex].states) {
    switch (s) {
    case SvtSlotState::Free: ++out.freeCount; break;
    case SvtSlotState::Reserved: ++out.reservedCount; break;
    case SvtSlotState::UploadQueued: ++out.uploadQueuedCount; break;
    case SvtSlotState::Resident: ++out.residentCount; break;
    case SvtSlotState::Retiring: ++out.retiringCount; break;
    }
  }
  return true;
}

uint64_t SvtPhysicalCache::totalAtlasBytes() const {
  uint64_t total = 0;
  for (const Pool &p : pools_)
    total += p.info.atlasBytes;
  return total;
}

void SvtPhysicalCache::slotToXY(uint32_t slotsPerRow, uint32_t slot, uint32_t &x,
                                uint32_t &y) {
  x = slot % slotsPerRow;
  y = slot / slotsPerRow;
}

} // namespace svt
