#include "VlmLayout.h"

#include <algorithm>
#include <cmath>

namespace {
float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
}

VlmUniformLayout VlmMakeUniformLayout(const float bmin[3], const float bmax[3], float spacing) {
  VlmUniformLayout L{};
  for (int i = 0; i < 3; ++i) {
    const float extent = bmax[i] - bmin[i];
    // 最近整数取 cells(spacing 是目标间距,step 重算贴回 extent)。
    // 不能用 ceil:bmin/bmax 由 camera±size/2 经 float 往返,extent 可能
    // 比整数倍 spacing 多出 ~1e-6(如 4.0000038),ceil 会错误地多开一个 cell。
    const uint32_t cells = extent > 0.0f
        ? (uint32_t)std::max(1.0f, std::floor(extent / spacing + 0.5f))
        : 1u;
    L.cells[i] = cells;
    L.step[i] = extent / (float)cells; // 重算使 bmin+step*cells == bmax 精确成立
    L.bmin[i] = bmin[i];
  }
  return L;
}

uint32_t VlmProbeIndex(const VlmUniformLayout& L, uint32_t x, uint32_t y, uint32_t z) {
  const uint32_t PX = L.cells[0] + 1;
  const uint32_t PY = L.cells[1] + 1;
  return (z * PY + y) * PX + x;
}

void VlmProbeWorldPos(const VlmUniformLayout& L, uint32_t x, uint32_t y, uint32_t z, float out[3]) {
  out[0] = L.bmin[0] + (float)x * L.step[0];
  out[1] = L.bmin[1] + (float)y * L.step[1];
  out[2] = L.bmin[2] + (float)z * L.step[2];
}

void VlmCellLookup(const VlmUniformLayout& L, const float pos[3], int32_t outBaseCell[3], float outFrac[3]) {
  for (int i = 0; i < 3; ++i) {
    const float local = (pos[i] - L.bmin[i]) / L.step[i];
    const float fl = std::floor(local);
    int32_t cell = (int32_t)fl;
    if (cell < 0) cell = 0;
    if (cell > (int32_t)L.cells[i] - 1) cell = (int32_t)L.cells[i] - 1;
    outBaseCell[i] = cell;
    outFrac[i] = clampf(local - (float)cell, 0.0f, 1.0f); // 最大边界 → frac=1
  }
}

bool VlmInside(const VlmUniformLayout& L, const float pos[3]) {
  for (int i = 0; i < 3; ++i) {
    if (!std::isfinite(pos[i])) return false;
    const float hi = L.bmin[i] + L.step[i] * (float)L.cells[i];
    if (pos[i] < L.bmin[i] || pos[i] > hi) return false;
  }
  return true;
}

float VlmBoundaryWeight(const VlmUniformLayout& L, const float pos[3], float bandWidth) {
  float dMin = 1e30f;
  for (int i = 0; i < 3; ++i) {
    const float hi = L.bmin[i] + L.step[i] * (float)L.cells[i];
    const float d = std::fmin(pos[i] - L.bmin[i], hi - pos[i]);
    dMin = std::fmin(dMin, d);
  }
  return clampf(dMin / (bandWidth > 1e-6f ? bandWidth : 1e-6f), 0.0f, 1.0f);
}
