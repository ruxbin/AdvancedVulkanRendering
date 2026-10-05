#include "VlmLayout.h"

#include <cmath>
#include <cstdio>

namespace {

int failures = 0;

void expectTrue(bool cond, const char* message) {
  if (!cond) { std::fprintf(stderr, "FAIL %s\n", message); ++failures; }
}
void expectNear(float actual, float expected, float tol, const char* message) {
  if (std::fabs(actual - expected) > tol) {
    std::fprintf(stderr, "FAIL %s: expected %f, got %f\n", message, expected, actual);
    ++failures;
  }
}

// extent 2.5 / spacing 1.0 → cells=3, probes=4/axis, total 64;step 重算为 2.5/3。
void TestMakeLayout() {
  const float bmin[3] = {0, 0, 0};
  const float bmax[3] = {2.5f, 2.5f, 2.5f};
  VlmUniformLayout L = VlmMakeUniformLayout(bmin, bmax, 1.0f);
  expectTrue(L.cells[0] == 3 && L.cells[1] == 3 && L.cells[2] == 3, "cells == 3");
  expectTrue(L.ProbeCount() == 64, "probe count == 4^3");
  expectNear(L.step[0], 2.5f / 3.0f, 1e-6f, "step recomputed");
  expectNear(L.bmin[0] + L.step[0] * L.cells[0], 2.5f, 1e-5f, "bmin+step*cells == bmax");
}

// extent 小于 spacing → cells=1(下限)。
void TestTinyExtent() {
  const float bmin[3] = {1, 2, 3};
  const float bmax[3] = {1.4f, 2.4f, 3.4f};
  VlmUniformLayout L = VlmMakeUniformLayout(bmin, bmax, 1.0f);
  expectTrue(L.cells[0] == 1 && L.ProbeCount() == 8, "cells clamped to 1");
}

// 索引与坐标往返一致。
void TestIndexRoundTrip() {
  const float bmin[3] = {-1, -2, -3};
  const float bmax[3] = {3, 2, 1};
  VlmUniformLayout L = VlmMakeUniformLayout(bmin, bmax, 1.0f);
  const uint32_t PX = L.cells[0] + 1, PY = L.cells[1] + 1;
  for (uint32_t z = 0; z <= L.cells[2]; ++z)
    for (uint32_t y = 0; y <= L.cells[1]; ++y)
      for (uint32_t x = 0; x <= L.cells[0]; ++x) {
        uint32_t idx = VlmProbeIndex(L, x, y, z);
        expectTrue(idx == (z * PY + y) * PX + x, "index formula");
        float p[3];
        VlmProbeWorldPos(L, x, y, z, p);
        expectNear(p[0], L.bmin[0] + x * L.step[0], 1e-5f, "probe pos x");
        expectNear(p[1], L.bmin[1] + y * L.step[1], 1e-5f, "probe pos y");
        expectNear(p[2], L.bmin[2] + z * L.step[2], 1e-5f, "probe pos z");
      }
}

// 最大边界查询 → 最后有效 cell + frac=1(Review Focus #1);内部点常规;下边界 frac=0。
void TestCellLookupBounds() {
  const float bmin[3] = {0, 0, 0};
  const float bmax[3] = {4, 4, 4};
  VlmUniformLayout L = VlmMakeUniformLayout(bmin, bmax, 1.0f); // cells=4
  int32_t cell[3];
  float frac[3];

  const float atMax[3] = {4, 4, 4};
  VlmCellLookup(L, atMax, cell, frac);
  expectTrue(cell[0] == 3 && cell[1] == 3 && cell[2] == 3, "max bound -> last cell");
  expectNear(frac[0], 1.0f, 1e-6f, "max bound -> frac 1");

  const float inside[3] = {1.3f, 2.7f, 0.5f};
  VlmCellLookup(L, inside, cell, frac);
  expectTrue(cell[0] == 1 && cell[1] == 2 && cell[2] == 0, "interior cell");
  expectNear(frac[0], 0.3f, 1e-5f, "interior frac x");
  expectNear(frac[1], 0.7f, 1e-5f, "interior frac y");

  const float atMin[3] = {0, 0, 0};
  VlmCellLookup(L, atMin, cell, frac);
  expectTrue(cell[0] == 0, "min bound -> cell 0");
  expectNear(frac[0], 0.0f, 1e-6f, "min bound -> frac 0");
}

// Inside / 边界权重:外部→0,边界→0,向内一个 cell 后→1;NaN → not inside。
void TestBoundaryWeight() {
  const float bmin[3] = {0, 0, 0};
  const float bmax[3] = {4, 4, 4};
  VlmUniformLayout L = VlmMakeUniformLayout(bmin, bmax, 1.0f);
  const float band = 1.0f;

  const float outside[3] = {-0.5f, 2, 2};
  expectTrue(!VlmInside(L, outside), "outside not inside");
  expectNear(VlmBoundaryWeight(L, outside, band), 0.0f, 1e-6f, "outside weight 0");

  const float onEdge[3] = {0, 2, 2};
  expectTrue(VlmInside(L, onEdge), "edge counts as inside");
  expectNear(VlmBoundaryWeight(L, onEdge, band), 0.0f, 1e-6f, "edge weight 0");

  const float halfCell[3] = {0.5f, 2, 2};
  expectNear(VlmBoundaryWeight(L, halfCell, band), 0.5f, 1e-5f, "half cell weight");

  const float deep[3] = {2, 2, 2};
  expectNear(VlmBoundaryWeight(L, deep, band), 1.0f, 1e-6f, "deep weight 1");

  const float nanPos[3] = {NAN, 2, 2};
  expectTrue(!VlmInside(L, nanPos), "NaN not inside");
}

} // namespace

int main() {
  const float lo[3] = {0,0,0}, hi[3] = {2.1f,2.1f,2.1f};
  const auto fractional = VlmMakeUniformLayout(lo, hi, 1.0f);
  expectTrue(fractional.cells[0] == 3, "spacing is a maximum, not nearest cell size");
  expectTrue(VlmMakeUniformLayout(lo, hi, 0).ProbeCount() == 0, "zero spacing rejected");
  expectTrue(VlmMakeUniformLayout(lo, hi, NAN).ProbeCount() == 0, "NaN spacing rejected");
  expectTrue(VlmMakeUniformLayout(lo, lo, 1).ProbeCount() == 0, "degenerate volume rejected");
  TestMakeLayout();
  TestTinyExtent();
  TestIndexRoundTrip();
  TestCellLookupBounds();
  TestBoundaryWeight();
  if (failures == 0) std::printf("all VlmLayout tests passed\n");
  return failures == 0 ? 0 : 1;
}
