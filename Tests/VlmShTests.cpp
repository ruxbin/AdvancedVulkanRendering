#include "SphericalHarmonics.h"
#include "VlmSh.h"

#include <cmath>
#include <cstdio>
#include <vector>

namespace {

int failures = 0;

void expectNear(float actual, float expected, float tol, const char* message) {
  if (std::fabs(actual - expected) > tol) {
    std::fprintf(stderr, "FAIL %s: expected %f, got %f\n", message, expected, actual);
    ++failures;
  }
}

constexpr float kPi = 3.1415926535897932f;

// 与 vlm_bake.hlsl 相同的 Fibonacci 球面方向(均匀球面 pdf=1/(4π))。
void FibonacciDir(unsigned i, unsigned n, float out[3]) {
  const float golden = 2.39996323f;
  float z = 1.0f - (2.0f * i + 1.0f) / (float)n;
  float r = std::sqrt(z * z > 1.0f ? 0.0f : 1.0f - z * z);
  float phi = golden * (float)i;
  out[0] = r * std::cos(phi);
  out[1] = z;
  out[2] = r * std::sin(phi);
}

// 恒定辐射场:c0 ≈ π·L,高阶 band ≈ 0,E(n) 对任意法线 ≈ π·L。
void TestConstantField() {
  const unsigned N = 4096;
  const float L[3] = {1.0f, 0.5f, 0.25f};
  VlmShAccumulator acc{};
  for (unsigned i = 0; i < N; ++i) {
    float d[3];
    FibonacciDir(i, N, d);
    VlmShAccumulate(acc, d, L, 1.0f / (4.0f * kPi));
  }
  SH9 sh = VlmShFinalize(acc);
  for (int ch = 0; ch < 3; ++ch)
    expectNear(sh.c[0][ch], kPi * L[ch], kPi * L[ch] * 0.005f + 1e-4f, "const c0");
  for (int j = 1; j < 9; ++j)
    for (int ch = 0; ch < 3; ++ch)
      expectNear(sh.c[j][ch], 0.0f, 2e-3f, "const higher band must vanish");
  const float n[3] = {0.36f, 0.48f, 0.8f};
  float E[3];
  VlmShEvaluate(sh, n, E);
  for (int ch = 0; ch < 3; ++ch)
    expectNear(E[ch], kPi * L[ch], kPi * L[ch] * 0.005f + 1e-3f, "const E(n)");
}

// 方向约定交叉验证:同一合成 equirect(+X 方向亮斑),
// MC 投影(VlmSh)与确定积分(ComputeSH9FromEquirect)必须给出一致的多项式系数。
// 方向→uv 映射与 shader DirectionToEquirectUV 逐字一致。
void TestDirectionConventionVsEquirect() {
  const int W = 64, H = 32;
  std::vector<float> pixels(W * H * 4, 0.0f);
  for (int y = 0; y < H; ++y)
    for (int x = 0; x < W; ++x) {
      float u = (x + 0.5f) / W, v = (y + 0.5f) / H;
      float phi = (u - 0.5f) * 2.0f * kPi;
      float theta = v * kPi;
      float dx = -std::sin(theta) * std::sin(phi);
      float dy = std::cos(theta);
      float dz = std::sin(theta) * std::cos(phi);
      float spot = dx > 0.9f ? 10.0f : 0.0f; // +X 附近亮斑
      float* px = &pixels[(y * W + x) * 4];
      px[0] = px[1] = px[2] = spot; px[3] = 1.0f;
    }
  SH9 ref = ComputeSH9FromEquirect(pixels.data(), W, H);

  auto sampleEnv = [&](const float d[3], float rgb[3]) {
    float u = std::atan2(-d[0], d[2]) / (2.0f * kPi) + 0.5f;
    float v = std::acos(d[1] > 1.0f ? 1.0f : (d[1] < -1.0f ? -1.0f : d[1])) / kPi;
    int x = (int)(u * W); if (x < 0) x = 0; if (x >= W) x = W - 1;
    int y = (int)(v * H); if (y < 0) y = 0; if (y >= H) y = H - 1;
    const float* px = &pixels[(y * W + x) * 4];
    rgb[0] = px[0]; rgb[1] = px[1]; rgb[2] = px[2];
  };

  const unsigned N = 8192;
  VlmShAccumulator acc{};
  for (unsigned i = 0; i < N; ++i) {
    float d[3], rgb[3];
    FibonacciDir(i, N, d);
    sampleEnv(d, rgb);
    VlmShAccumulate(acc, d, rgb, 1.0f / (4.0f * kPi));
  }
  SH9 mc = VlmShFinalize(acc);

  float sumRef = 0.0f, sumDiff = 0.0f;
  for (int j = 0; j < 9; ++j)
    for (int ch = 0; ch < 3; ++ch) {
      sumRef += std::fabs(ref.c[j][ch]);
      sumDiff += std::fabs(mc.c[j][ch] - ref.c[j][ch]);
    }
  if (sumDiff / sumRef > 0.03f) {
    std::fprintf(stderr, "FAIL direction convention: weighted rel err %f\n", sumDiff / sumRef);
    ++failures;
  }
  // +X 亮斑:E(+x) 必须显著大于 E(-x)(钉死镜像/轴错误)。
  float epx[3], enx[3];
  const float pxd[3] = {1, 0, 0}, nxd[3] = {-1, 0, 0};
  VlmShEvaluate(mc, pxd, epx);
  VlmShEvaluate(mc, nxd, enx);
  if (epx[0] <= enx[0] * 1.5f) {
    std::fprintf(stderr, "FAIL direction: E(+x)=%f must dominate E(-x)=%f\n", epx[0], enx[0]);
    ++failures;
  }
}

// FP16 打包:round-trip 误差、负系数保留、第 28 通道为 0。
void TestPackRoundTrip() {
  SH9 sh{};
  for (int j = 0; j < 9; ++j)
    for (int ch = 0; ch < 3; ++ch)
      sh.c[j][ch] = (j - 4) * 7.25f + ch * 0.5f; // 覆盖正负
  uint16_t packed[28];
  if (!VlmShPackFp16(sh, packed)) { std::fprintf(stderr, "FAIL pack rejected valid data\n"); ++failures; }
  for (int j = 0; j < 9; ++j)
    for (int ch = 0; ch < 3; ++ch)
      expectNear(VlmHalfToFloat(packed[j * 3 + ch]), sh.c[j][ch],
                 std::fabs(sh.c[j][ch]) * 1e-3f + 1e-2f, "pack round trip");
  expectNear(VlmHalfToFloat(packed[27]), 0.0f, 0.0f, "channel 28 must be zero");
}

// 溢出/非有限必须拒绝(Review Focus #2)。
void TestPackOverflowRejected() {
  SH9 sh{};
  uint16_t packed[28];
  sh.c[0][0] = 70000.0f; // > 65504
  if (VlmShPackFp16(sh, packed)) { std::fprintf(stderr, "FAIL pack accepted 70000\n"); ++failures; }
  sh.c[0][0] = 1.0f; sh.c[5][1] = INFINITY;
  if (VlmShPackFp16(sh, packed)) { std::fprintf(stderr, "FAIL pack accepted inf\n"); ++failures; }
  sh.c[5][1] = NAN;
  if (VlmShPackFp16(sh, packed)) { std::fprintf(stderr, "FAIL pack accepted nan\n"); ++failures; }
  sh.c[5][1] = -60000.0f; // 边界内合法负值
  if (!VlmShPackFp16(sh, packed)) { std::fprintf(stderr, "FAIL pack rejected -60000\n"); ++failures; }
}

} // namespace

int main() {
  TestConstantField();
  TestDirectionConventionVsEquirect();
  TestPackRoundTrip();
  TestPackOverflowRejected();
  if (failures == 0) std::printf("all VlmSh tests passed\n");
  return failures == 0 ? 0 : 1;
}
