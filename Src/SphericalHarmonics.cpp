#include "SphericalHarmonics.h"

#include <cmath>
#include <vector>

namespace {
constexpr float kPi = 3.1415926535897932f;

// 每像素 2×2 子采样:像素颜色按分段常量取 texel 本身,方向/权重在子采样点求值。
// 纯中心采样时 θ 向 midpoint 积分误差使 (x²-y²) 通道在 64×32 下残留 ~1.2e-3,
// 超过测试 1e-3 容差;2×2 子采样把该误差降到 ~3e-4(h² 收敛)。
constexpr int kSubSamples = 2;
} // namespace

SH9 ComputeSH9FromEquirect(const float* rgba, int width, int height) {
  if (!rgba || width <= 0 || height <= 0) return SH9{}; // 全零 = 无环境贡献

  double proj[9][3] = {};
  double weightSum = 0.0;

  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      // 索引全程 size_t:int 乘法在超大纹理下会先溢出再提升。
      const float* px = rgba + ((size_t)y * (size_t)width + (size_t)x) * 4;
      for (int sy = 0; sy < kSubSamples; ++sy) {
        const float v = (y + (sy + 0.5f) / kSubSamples) / height;
        const float theta = v * kPi; // 0..π,自 +Y 起
        const float sinTheta = std::sin(theta);
        const float cosTheta = std::cos(theta);
        for (int sx = 0; sx < kSubSamples; ++sx) {
          const float u = (x + (sx + 0.5f) / kSubSamples) / width;
          const float phi = (u - 0.5f) * 2.0f * kPi;
          // 方位角镜像(对 Apple 烘焙 KTX 的实证校准,见头文件约定注释)
          const float dx = -sinTheta * std::sin(phi);
          const float dy = cosTheta;
          const float dz = sinTheta * std::cos(phi);

          float basis[9];
          EvalSh9Basis(dx, dy, dz, basis);

          const double w = sinTheta; // 立体角 ∝ sinθ(常数因子 dθdφ 归一化时约掉)
          weightSum += w;
          for (int i = 0; i < 9; ++i)
            for (int ch = 0; ch < 3; ++ch)
              proj[i][ch] += (double)px[ch] * basis[i] * w;
        }
      }
    }
  }

  // L_lm = 4π * Σ(color·Y·w) / Σw(对恒定环境精确);
  // 浓缩形式 c[i] = A_l · Y_i常量 · L_i(Y 归一化常数折入,见头文件注释)。
  SH9 sh;
  const double norm = 4.0 * kPi / weightSum;
  for (int i = 0; i < 9; ++i)
    for (int ch = 0; ch < 3; ++ch)
      sh.c[i][ch] = (float)(kSh9A[i] * kSh9Y[i] * proj[i][ch] * norm);
  return sh;
}

SH9 ComputeMetalSH9FromEquirect(const float* rgba, int width, int height) {
  if (!rgba || width <= 0 || height <= 0) return SH9{};
  std::vector<float> bounded(rgba, rgba + size_t(width) * height * 4);
  for (float& v : bounded) v = v < 0.0f ? 0.0f : (v > 256.0f ? 256.0f : v);
  SH9 sh = ComputeSH9FromEquirect(bounded.data(), width, height);
  for (int i = 0; i < 9; ++i) {
    const float sign = (i == 2 || i == 5 || i == 7) ? -1.0f : 1.0f;
    for (int c = 0; c < 3; ++c) sh.c[i][c] *= sign / (kPi * kSh9Y[i]);
  }
  return sh;
}
