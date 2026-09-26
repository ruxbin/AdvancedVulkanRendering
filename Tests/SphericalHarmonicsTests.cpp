#include "SphericalHarmonics.h"

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

} // namespace

int main() {
  TestConstantEnvironment();
  TestTopHeavyEnvironment();
  if (failures == 0) std::printf("all SH tests passed\n");
  return failures == 0 ? 0 : 1;
}
