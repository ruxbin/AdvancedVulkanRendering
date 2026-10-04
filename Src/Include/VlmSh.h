#pragma once

#include <cstdint>

#include "SphericalHarmonics.h"

// VLM Monte-Carlo SH 投影:累积 Σ Li·Y_j(wi)/pdf(基常数 kSh9Y 已折入 basis)。
// VlmShFinalize 输出与 ComputeSH9FromEquirect 同语义的物理辐照度多项式系数:
//   c_j = kSh9A_j · kSh9Y_j · (Σ Li·basis_j(wi)/pdf) / N
// 推导对照:ComputeSH9FromEquirect 的 c = kA·kY·proj·(4π/Σw),
// 均匀球面 pdf=1/(4π) 时两者在期望上相等。
struct VlmShAccumulator {
  double sum[9][3] = {};
  uint64_t sampleCount = 0;
};

void VlmShAccumulate(VlmShAccumulator& acc, const float dir[3], const float radiance[3], float pdf);
SH9 VlmShFinalize(const VlmShAccumulator& acc);

// E(n) = c0 + c1·y + c2·z + c3·x + c4·yx + c5·yz + c6·(3z²-1) + c7·zx + c8·(x²-y²)
// 与 shaders/ibl_common.hlsl 的 evaluateShCoefficients 同约定(+N 求值,无 kSh9Y)。
void VlmShEvaluate(const SH9& sh, const float n[3], float outE[3]);

// 按 j*3+rgb 展平 27 个数,顺序填充 7 组 RGBA16F(第 28 通道写 0)= 56 B/probe。
// 任一系数非有限或 |v| > 65504 时返回 false(规格 §7:拒绝打包并报告,不静默饱和)。
bool VlmShPackFp16(const SH9& sh, uint16_t out28[28]);

uint16_t VlmFloatToHalf(float v);
float VlmHalfToFloat(uint16_t h);
