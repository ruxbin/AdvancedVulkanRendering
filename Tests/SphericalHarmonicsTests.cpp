#include "SphericalHarmonics.h"

#include <cmath>
#include <cstdio>
#include <vector>

namespace {

int failures = 0;

void expectNear(float actual, float expected, float tol, const char* message) {
  if (!std::isfinite(actual) || std::fabs(actual - expected) > tol) {
    std::fprintf(stderr, "FAIL %s: expected %f, got %f\n", message, expected, actual);
    ++failures;
  }
}

// 恒定辐射环境:辐照度 SH 只剩 L00,值 = π * c(推导见 spec §4.1)。
void TestConstantEnvironment() {
  const int W = 64, H = 32;
  std::vector<float> pixels(W * H * 4, 0.0f);
  for (int i = 0; i < W * H; ++i) {
    pixels[i * 4 + 0] = 1.0f; pixels[i * 4 + 1] = 0.5f; pixels[i * 4 + 2] = 0.25f;
  }
  SH9 sh = ComputeSH9FromEquirect(pixels.data(), W, H);
  const float kPi = 3.14159265f;
  expectNear(sh.c[0][0], kPi * 1.0f, kPi * 0.01f, "const L00 r");
  expectNear(sh.c[0][1], kPi * 0.5f, kPi * 0.01f, "const L00 g");
  expectNear(sh.c[0][2], kPi * 0.25f, kPi * 0.01f, "const L00 b");
  for (int i = 1; i < 9; ++i)
    for (int ch = 0; ch < 3; ++ch)
      expectNear(sh.c[i][ch], 0.0f, 1e-3f, "const higher band must vanish");
}

// 上半球白、下半球黑:y 系数(索引 1)必须显著为正,x/z(2,3)≈ 0。
// 这钉死 equirect 的 v=0 端对应 +Y 的约定。
void TestTopHeavyEnvironment() {
  const int W = 64, H = 32;
  std::vector<float> pixels(W * H * 4, 0.0f);
  for (int y = 0; y < H; ++y)
    for (int x = 0; x < W; ++x) {
      float v = (y < H / 2) ? 1.0f : 0.0f;
      pixels[(y * W + x) * 4 + 0] = v;
      pixels[(y * W + x) * 4 + 1] = v;
      pixels[(y * W + x) * 4 + 2] = v;
    }
  SH9 sh = ComputeSH9FromEquirect(pixels.data(), W, H);
  if (sh.c[1][0] <= 0.1f) {
    std::fprintf(stderr, "FAIL top-heavy: y coefficient must be positive, got %f\n", sh.c[1][0]);
    ++failures;
  }
  expectNear(sh.c[2][0], 0.0f, 1e-3f, "top-heavy z coeff");
  expectNear(sh.c[3][0], 0.0f, 1e-3f, "top-heavy x coeff");
}

// A white hemisphere has E(n)=pi/2 * (1 + dot(n,axis)) in SH9.
// Red: u<0.5 -> +X; green: middle half -> +Z; blue: outer half -> -Z.
// This catches a mirrored azimuth or an accidental X/Z swap independently of Y.
void TestAzimuthHemispheres() {
  const int W = 128, H = 64;
  std::vector<float> pixels(W * H * 4, 0.0f);
  for (int y = 0; y < H; ++y)
    for (int x = 0; x < W; ++x) {
      const size_t i = (y * W + x) * 4;
      pixels[i] = x < W / 2 ? 1.0f : 0.0f;
      pixels[i + 1] = x >= W / 4 && x < 3 * W / 4 ? 1.0f : 0.0f;
      pixels[i + 2] = 1.0f - pixels[i + 1];
    }
  const SH9 sh = ComputeSH9FromEquirect(pixels.data(), W, H);
  constexpr float halfPi = 1.570796327f;
  for (int channel = 0; channel < 3; ++channel)
    for (int i = 0; i < 9; ++i) {
      float expected = i == 0 ? halfPi : 0.0f;
      if (channel == 0 && i == 3) expected = halfPi;
      if (channel == 1 && i == 2) expected = halfPi;
      if (channel == 2 && i == 2) expected = -halfPi;
      expectNear(sh.c[i][channel], expected, 1e-3f, "azimuth hemisphere coefficient");
    }
}

// Analytic quadrupoles: convolving xy/yz/zx with the cosine kernel gives
// (pi/4) times the same polynomial. This tests all mixed terms and both
// second-band Z signs without comparing against the physical implementation.
void TestMetalQuadrupoles() {
  constexpr int w = 256, h = 128;
  constexpr double pi = 3.141592653589793;
  std::vector<float> pixels(w * h * 4, 0.0f);
  for (int row = 0; row < h; ++row)
    for (int col = 0; col < w; ++col) {
      const double theta = (row + 0.5) * pi / h;
      const double phi = ((col + 0.5) / w - 0.5) * 2.0 * pi;
      const double x = -std::sin(theta) * std::sin(phi);
      const double y = std::cos(theta);
      const double z = std::sin(theta) * std::cos(phi);
      const size_t i = (row * w + col) * 4;
      pixels[i] = float(2.0 + x * y);
      pixels[i + 1] = float(2.0 + y * z);
      pixels[i + 2] = float(2.0 + z * x);
    }
  const auto sh = ComputeMetalSH9FromEquirect(pixels.data(), w, h);
  const float mixed = float(std::sqrt(pi / 15.0) / 2.0);
  for (int c = 0; c < 3; ++c)
    for (int i = 0; i < 9; ++i) {
      float expected = i == 0 ? float(4.0 * std::sqrt(pi)) : 0.0f;
      if (c == 0 && i == 4) expected = mixed;
      if (c == 1 && i == 5) expected = -mixed;
      if (c == 2 && i == 7) expected = -mixed;
      expectNear(sh.c[i][c], expected, 0.0005f, "Metal analytic quadrupole");
    }
}

} // namespace

int main() {
  TestConstantEnvironment();
  TestTopHeavyEnvironment();
  TestAzimuthHemispheres();
  TestMetalQuadrupoles();
  {
    const int w = 128, h = 64;
    std::vector<float> pixels(w * h * 4, 0.0f);
    for (int y = 0; y < h; ++y)
      for (int x = 0; x < w; ++x) {
        const int i = (y * w + x) * 4;
        pixels[i] = 1.0f;
        pixels[i + 1] = 512.0f;
        pixels[i + 2] = x >= w / 4 && x < 3 * w / 4 ? 1.0f : 0.0f;
      }
    const auto sh = ComputeMetalSH9FromEquirect(pixels.data(), w, h);
    expectNear(sh.c[0][0], 3.5449077f, 1e-4f, "Metal normalized constant basis");
    expectNear(sh.c[0][1], 907.49637f, 0.002f, "Metal source saturation at 256");
    expectNear(sh.c[2][2], -1.0233267f, 0.001f, "Metal Z evaluation convention");
  }
  if (failures == 0) std::printf("all SH tests passed\n");
  return failures == 0 ? 0 : 1;
}
