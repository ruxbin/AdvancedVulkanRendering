#include "VlmSh.h"

#include <cmath>
#include <cstring>

void VlmShAccumulate(VlmShAccumulator& acc, const float dir[3], const float radiance[3], float pdf) {
  float basis[9];
  EvalSh9Basis(dir[0], dir[1], dir[2], basis);
  const double invPdf = 1.0 / (double)pdf;
  for (int j = 0; j < 9; ++j)
    for (int ch = 0; ch < 3; ++ch)
      acc.sum[j][ch] += (double)radiance[ch] * (double)basis[j] * invPdf;
  acc.sampleCount += 1;
}

SH9 VlmShFinalize(const VlmShAccumulator& acc) {
  SH9 sh{};
  if (acc.sampleCount == 0) return sh; // 全零 = 无贡献
  const double invN = 1.0 / (double)acc.sampleCount;
  for (int j = 0; j < 9; ++j)
    for (int ch = 0; ch < 3; ++ch)
      sh.c[j][ch] = (float)(kSh9A[j] * kSh9Y[j] * acc.sum[j][ch] * invN);
  return sh;
}

void VlmShEvaluate(const SH9& sh, const float n[3], float outE[3]) {
  const float x = n[0], y = n[1], z = n[2];
  const float poly[9] = {
      1.0f, y, z, x,
      y * x, y * z, 3.0f * z * z - 1.0f, z * x, x * x - y * y,
  };
  for (int ch = 0; ch < 3; ++ch) {
    float e = 0.0f;
    for (int j = 0; j < 9; ++j) e += sh.c[j][ch] * poly[j];
    outE[ch] = e;
  }
}

uint16_t VlmFloatToHalf(float v) {
  uint32_t f;
  std::memcpy(&f, &v, 4);
  uint32_t sign = (f >> 16) & 0x8000u;
  int exp = (int)((f >> 23) & 0xFF) - 127 + 15;
  uint32_t mant = f & 0x7FFFFFu;
  if (exp <= 0) return (uint16_t)sign;              // 下溢 → ±0(半精度次正规忽略)
  if (exp >= 31) return (uint16_t)(sign | 0x7C00u); // 上溢 → ±inf(调用方负责先查范围)
  return (uint16_t)(sign | ((uint32_t)exp << 10) | (mant >> 13));
}

float VlmHalfToFloat(uint16_t h) {
  uint32_t sign = (uint32_t)(h & 0x8000u) << 16;
  uint32_t exp = (h >> 10) & 0x1F;
  uint32_t mant = h & 0x3FF;
  uint32_t f;
  if (exp == 0) {
    float v = (float)mant / 1024.0f * 6.103515625e-05f;
    std::memcpy(&f, &v, 4);
    f |= sign;
  } else if (exp == 31) {
    f = sign | 0x7F800000u | (mant << 13);
  } else {
    f = sign | ((exp + 112) << 23) | (mant << 13);
  }
  float out;
  std::memcpy(&out, &f, 4);
  return out;
}

bool VlmShPackFp16(const SH9& sh, uint16_t out28[28]) {
  for (int j = 0; j < 9; ++j)
    for (int ch = 0; ch < 3; ++ch) {
      const float v = sh.c[j][ch];
      if (!std::isfinite(v) || std::fabs(v) > 65504.0f) return false;
      out28[j * 3 + ch] = VlmFloatToHalf(v);
    }
  out28[27] = 0;
  return true;
}
