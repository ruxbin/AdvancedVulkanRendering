#pragma once

#include <cstdint>

// VLM 均匀网格布局(规格 §5.1)。各轴 probe 数 = cells+1;
// 查询最大边界时必须选最后一个有效 cell 且局部坐标为 1。
struct VlmUniformLayout {
  float bmin[3];
  float step[3];
  uint32_t cells[3];

  uint64_t ProbeCount() const {
    for (auto c : cells) if (!c || c > 4096) return 0;
    return (uint64_t(cells[0]) + 1) * (uint64_t(cells[1]) + 1) * (uint64_t(cells[2]) + 1);
  }
};

VlmUniformLayout VlmMakeUniformLayout(const float bmin[3], const float bmax[3], float spacing);
uint32_t VlmProbeIndex(const VlmUniformLayout& L, uint32_t x, uint32_t y, uint32_t z);
void VlmProbeWorldPos(const VlmUniformLayout& L, uint32_t x, uint32_t y, uint32_t z, float out[3]);
void VlmCellLookup(const VlmUniformLayout& L, const float pos[3], int32_t outBaseCell[3], float outFrac[3]);
bool VlmInside(const VlmUniformLayout& L, const float pos[3]);
float VlmBoundaryWeight(const VlmUniformLayout& L, const float pos[3], float bandWidth);
