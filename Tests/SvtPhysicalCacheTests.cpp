// SVT 物理缓存 CPU 测试（设计 §7.2、§13.1）。
// 覆盖：槽位几何与 shader 语义一致、池规划（尺寸/预算截断）、
// 分配/释放状态机、重复 key 复用、预算记账。

#include "SvtPhysicalCache.h"

#include <cstdio>
#include <string>

using svt::SvtPhysicalCache;

namespace {

int failures = 0;

void expectTrue(bool cond, const char *message) {
  if (!cond) {
    std::fprintf(stderr, "FAIL %s\n", message);
    ++failures;
  }
}

void expectEqU64(uint64_t actual, uint64_t expected, const char *message) {
  if (actual != expected) {
    std::fprintf(stderr, "FAIL %s (actual=%llu expected=%llu)\n", message,
                 static_cast<unsigned long long>(actual),
                 static_cast<unsigned long long>(expected));
    ++failures;
  }
}

void TestGeometry() {
  // P = 128 + 2*4 = 136；limit 4096 -> floor(4096/136) = 30 列，atlas 宽 4080。
  expectEqU64(SvtPhysicalCache::paddedTileSize(128, 4), 136, "P = T + 2B");
  expectEqU64(4096 / 136, 30, "max slots per row at 4096");

  // slot -> (x, y) 必须与 shader 的 slot % / slot / 一致。
  uint32_t x = 0, y = 0;
  SvtPhysicalCache::slotToXY(30, 0, x, y);
  expectTrue(x == 0 && y == 0, "slot 0 -> (0,0)");
  SvtPhysicalCache::slotToXY(30, 29, x, y);
  expectTrue(x == 29 && y == 0, "slot 29 -> (29,0)");
  SvtPhysicalCache::slotToXY(30, 30, x, y);
  expectTrue(x == 0 && y == 1, "slot 30 -> (0,1)");
  SvtPhysicalCache::slotToXY(30, 91, x, y);
  expectTrue(x == 1 && y == 3, "slot 91 -> (1,3)");

  // 理论 payload 记账（设计 §12 的表格）：
  // BC1 8B/block：4080x4080 -> (4080/4)^2 * 8 = 1020^2 * 8 = 8,323,200。
  expectEqU64(SvtPhysicalCache::atlasByteSize(svt::SvtFormat::BC1, 4080, 4080),
              1020ull * 1020 * 8, "BC1 atlas bytes");
  // BC7 16B/block：136x136 单 tile payload = 34^2 * 16 = 18,496（与设计 §12 一致）。
  expectEqU64(SvtPhysicalCache::atlasByteSize(svt::SvtFormat::BC7, 136, 136), 34ull * 34 * 16,
              "BC7 single tile payload");
  expectEqU64(SvtPhysicalCache::atlasByteSize(svt::SvtFormat::RGBA8, 136, 136),
              136ull * 136 * 4, "RGBA8 single tile payload");
  // 尺寸必须按 block 对齐才能成立：4082 不是 4 的倍数，按 ceil(block) 记账。
  expectEqU64(SvtPhysicalCache::atlasByteSize(svt::SvtFormat::BC1, 4082, 4082),
              1021ull * 1021 * 8, "BC1 non-aligned dims round up blocks");
}

void TestPlanPool() {
  // 无预算限制：16 slots -> 4x4 方形。
  svt::SvtPoolPlan plan;
  expectTrue(svt::svtPlanPool(svt::SvtFormat::BC7, 128, 4, 16, 4096, ~0ull, plan),
             "plan 16 slots");
  expectEqU64(plan.slotsPerRow, 4, "plan 16 -> 4 per row");
  expectEqU64(plan.rows, 4, "plan 16 -> 4 rows");
  expectEqU64(plan.capacity, 16, "plan capacity 16");
  expectTrue(!plan.truncated, "plan not truncated");
  expectEqU64(plan.atlasBytes, SvtPhysicalCache::atlasByteSize(svt::SvtFormat::BC7,
                                                               4 * 136, 4 * 136),
             "plan bytes matches geometry");

  // 尺寸截断：想要 200000 slots，4096 限宽只有 30 列。
  expectTrue(svt::svtPlanPool(svt::SvtFormat::BC1, 128, 4, 200000, 4096, ~0ull, plan),
             "plan oversized");
  expectTrue(plan.truncated, "oversized truncated");
  expectEqU64(plan.slotsPerRow, 30, "oversized clamped to 30 per row");
  expectEqU64(plan.rows, 30, "oversized clamped to 30 rows (square limit)");
  expectEqU64(plan.capacity, 900, "oversized capacity 900");

  // 预算截断：一行 BC7（30 slots）payload = 4080*136 像素。
  // 预算恰好只够一行多一点。
  const uint64_t oneRow = SvtPhysicalCache::atlasByteSize(svt::SvtFormat::BC7, 4080, 136);
  expectTrue(svt::svtPlanPool(svt::SvtFormat::BC7, 128, 4, 900, 4096, oneRow, plan),
             "plan budget one row");
  expectEqU64(plan.rows, 1, "budget one row -> 1 row");
  expectEqU64(plan.capacity, 30, "budget one row capacity 30");
  expectTrue(plan.truncated, "budget one row truncated");
  expectTrue(plan.atlasBytes <= oneRow, "budget respected");

  // 预算连一行都不够 -> 失败。
  expectTrue(!svt::svtPlanPool(svt::SvtFormat::BC7, 128, 4, 900, 4096, oneRow - 1, plan),
             "plan below one row fails");

  // 非法参数。
  expectTrue(!svt::svtPlanPool(svt::SvtFormat::BC7, 128, 4, 0, 4096, ~0ull, plan),
             "plan zero desired fails");
  expectTrue(!svt::svtPlanPool(svt::SvtFormat::BC7, 126, 4, 16, 4096, ~0ull, plan),
             "plan non-block-aligned BC tile fails");
  expectTrue(!svt::svtPlanPool(svt::SvtFormat::BC7, 128, 4, 16, 100, ~0ull, plan),
             "plan limit below P fails");
}

void TestPoolLifecycle() {
  svt::SvtPhysicalCache cache;
  svt::SvtPoolConfig cfg;
  cfg.format = svt::SvtFormat::BC7;
  cfg.tileSize = 128;
  cfg.border = 4;
  cfg.slotsPerRow = 4;
  cfg.rows = 2; // 8 slots

  const int pool = cache.createPool(cfg);
  expectTrue(pool == 0, "first pool index 0");
  // 相同 key 复用同一池。
  expectEqU64(cache.createPool(cfg), 0, "duplicate pool key reused");

  const auto *info = cache.poolInfo(0);
  expectTrue(info != nullptr, "pool info exists");
  expectEqU64(info->slotCount, 8, "pool slot count 8");
  expectEqU64(info->atlasWidth, 4 * 136, "pool atlas width");
  expectEqU64(info->atlasHeight, 2 * 136, "pool atlas height");
  expectEqU64(cache.totalAtlasBytes(), info->atlasBytes, "total bytes single pool");

  // 分配 8 个后耗尽。
  uint32_t slots[8];
  for (int i = 0; i < 8; ++i)
    expectTrue(cache.allocateSlot(0, slots[i]), "allocate slot");
  uint32_t extra = 0;
  expectTrue(!cache.allocateSlot(0, extra), "pool exhausted");
  // 分配序号应当是 0..7 的排列。
  bool seen[8] = {};
  for (int i = 0; i < 8; ++i)
    seen[slots[i]] = true;
  for (int i = 0; i < 8; ++i)
    expectTrue(seen[i], "slot ids are a permutation");

  // 状态机：Reserved -> Resident -> Free。
  expectTrue(cache.slotState(0, slots[0]) == svt::SvtSlotState::Reserved, "allocated slot Reserved");
  expectTrue(cache.markSlotResident(0, slots[0]), "mark resident");
  expectTrue(cache.slotState(0, slots[0]) == svt::SvtSlotState::Resident, "slot Resident");
  expectTrue(cache.markSlotResident(0, slots[0]), "re-mark resident idempotent-ish (UploadQueued path)");
  expectTrue(!cache.releaseSlot(0, 99), "release out-of-range rejected");
  expectTrue(cache.releaseSlot(0, slots[1]), "release reserved slot");
  expectTrue(cache.slotState(0, slots[1]) == svt::SvtSlotState::Free, "released slot Free");
  expectTrue(!cache.releaseSlot(0, slots[1]), "double release rejected");

  // 释放后可重新分配，且占用统计正确。
  uint32_t reused = ~0u;
  expectTrue(cache.allocateSlot(0, reused), "reallocate freed slot");
  expectEqU64(reused, slots[1], "freed slot comes back first (LIFO)");

  svt::SvtPhysicalCache::PoolUsage usage;
  expectTrue(cache.poolUsage(0, usage), "usage query");
  expectEqU64(usage.reservedCount, 7, "7 reserved after release+realloc");
  expectEqU64(usage.residentCount, 1, "1 resident");
  expectEqU64(usage.freeCount, 0, "no free");

  // 不同 key -> 新池；非法参数 -> -1。
  svt::SvtPoolConfig cfg2 = cfg;
  cfg2.format = svt::SvtFormat::BC1;
  expectEqU64(cache.createPool(cfg2), 1, "second pool index 1");
  svt::SvtPoolConfig bad = cfg;
  bad.slotsPerRow = 0;
  expectTrue(cache.createPool(bad) < 0, "invalid pool rejected");
  svt::SvtPoolConfig badAlign = cfg;
  badAlign.tileSize = 126;
  expectTrue(cache.createPool(badAlign) < 0, "non-aligned BC pool rejected");

  // 池数上限（kMaxPools = 8，已建 2 个）。
  for (int i = 0; i < 6; ++i) {
    svt::SvtPoolConfig c = cfg;
    c.border = 4 + 4 * (i + 1); // 产生新 key
    c.tileSize = 128;
    if (svt::SvtPhysicalCache::paddedTileSize(c.tileSize, c.border) % 4 == 0) {
      expectEqU64(cache.createPool(c), 2 + i, "fill pool slots");
    }
  }
  svt::SvtPoolConfig overflow = cfg;
  overflow.border = 64;
  overflow.tileSize = 256;
  expectTrue(cache.createPool(overflow) < 0, "pool cap enforced");

  // slot 位宽上限：一个 slotCount 超过 2^20 的池被拒绝。
  svt::SvtPhysicalCache big;
  svt::SvtPoolConfig huge = cfg;
  huge.slotsPerRow = 1024;
  huge.rows = 1024; // 1,048,576 slots
  expectTrue(big.createPool(huge) >= 0, "exactly 2^20 slots allowed");
  svt::SvtPoolConfig huger = cfg;
  huger.border = 8; // 换 key，避免与 huge 池复用
  huger.slotsPerRow = 1048;
  huger.rows = 1001; // 1,050,848 > 2^20
  expectTrue(big.createPool(huger) < 0, "over 2^20 slots rejected");
}

void TestBistroScalePlanning() {
  // 真实 bistro cook 结果的规模检查：BC7 格式 ~340 tiles/资产（2048²）。
  // 默认 128 MiB 预算下 BC7 池能容纳多少 tile。
  svt::SvtPoolPlan plan;
  const uint64_t budget128MiB = 128ull << 20;
  expectTrue(svt::svtPlanPool(svt::SvtFormat::BC7, 128, 4, 6000, 16384, budget128MiB, plan),
             "plan bistro-scale pool");
  expectTrue(plan.atlasBytes <= budget128MiB, "bistro plan within budget");
  expectTrue(plan.capacity >= 300, "bistro plan holds at least 300 tiles");
  // 16K 限宽时行容量 = 16384/136 = 120。
  expectTrue(plan.slotsPerRow <= 120, "bistro plan per-row within 16K limit");
}

void TestRetirement() {
  svt::SvtPhysicalCache cache;
  svt::SvtPoolConfig c;
  c.slotsPerRow = c.rows = 1;
  cache.createPool(c);
  uint32_t slot = 0;
  cache.allocateSlot(0, slot);
  cache.markSlotResident(0, slot);
  expectTrue(!cache.releaseSlot(0, slot), "resident cannot bypass retirement");
  expectTrue(cache.retireSlot(0, slot, 12), "retire resident at last referencing submit");
  expectTrue(!cache.releaseSlot(0, slot), "retiring cannot bypass fence");
  cache.collectRetired(11);
  expectTrue(!cache.allocateSlot(0, slot), "slow GPU keeps slot unavailable");
  cache.collectRetired(12);
  expectTrue(cache.allocateSlot(0, slot), "completed serial makes slot reusable");
  expectTrue(cache.releaseSlot(0, slot), "unsubmitted reservation can be cancelled");
}

} // namespace

int main() {
  const struct {
    const char *name;
    void (*fn)();
  } tests[] = {
      {"Geometry", TestGeometry},
      {"Retirement", TestRetirement},
      {"PlanPool", TestPlanPool},
      {"PoolLifecycle", TestPoolLifecycle},
      {"BistroScalePlanning", TestBistroScalePlanning},
  };
  for (const auto &t : tests) {
    std::fprintf(stderr, "[run] %s\n", t.name);
    std::fflush(stderr);
    t.fn();
  }
  if (failures == 0)
    std::printf("SvtPhysicalCacheTests: all passed\n");
  else
    std::printf("SvtPhysicalCacheTests: %d failures\n", failures);
  return failures == 0 ? 0 : 1;
}
