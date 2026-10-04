# Volumetric Lightmap(阶段 1:均匀网格闭环)实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 为 Vulkan 后端实现 VLM 阶段 1 闭环:显式 BakeVolume + 均匀探针网格 + GPU 路径追踪烘焙物理 SH9 + `.vlm` 文件保存/加载 + deferred/forward 共用采样,并通过常量天空/方向约定/光照所有权三项数值验收。

**Architecture:** 三个纯 CPU 模块(SH 投影、网格布局、资产格式,各自带独立测试)→ 抽取 `rt_lighting.hlsl` 的命中/材质/BRDF/NEE 为共享头 `rt_path_common.hlsl` → 新烘焙 shader `vlm_bake.hlsl`(每 probe 一个 lane、批量采样、FP32 SH 累加)+ `VlmBaker`(独立 RT pipeline,复用 TLAS 与场景 buffer,首帧同步执行)→ `VlmRuntime`(加载 `.vlm`,上传 SH SSBO + 参数 UBO)+ `vlm_common.hlsl`(显式 8 邻居 gather、三线性、边界过渡带混回天空 diffuse)。

**Tech Stack:** Vulkan 1.3 + KHR_ray_tracing、dxc(SPIR-V,HLSL,lib_6_3)、C++20、MSVC/CMake(in-source 构建,输出 `Bin/`)、stb_image。

**Spec:** `docs/superpowers/specs/2026-10-01-volumetric-lightmap-design.md`(本计划只覆盖其 §11 的阶段 1;阶段 2「积分与覆盖质量」、阶段 3「稀疏布局」、阶段 4「室内质量档」不在本计划内,各自另行立项)

阶段 1 明确不含(执行时不得顺手实现):收敛/方差估计与断点续烘(§6,阶段 2)、探针有效性/实体内探针识别(§5.1,阶段 2)、去振铃(§7,阶段 2 起)、稀疏砖块与 Atlas(§5.2,阶段 3)、距离矩可见性(§8.2,阶段 4)、probe 球/SH lobes 等调试可视化(§10,阶段 2 起;阶段 1 用数值日志 + ImGui 开关验收)、GPU timestamp 计时(阶段 1 记 wall time)。

## Global Constraints

- 工作分支:从 `ibl` 切出 `vlm`(`git checkout -b vlm`)。工作区有大量未跟踪构建/贴图产物,`git add` **必须逐文件**,禁止 `git add -A` / `git add .`。
- shader 编译:`cmd /c "cd /d F:\AdvancedVulkanRendering\shaders && .\compile_shaders.bat"`(必须用 `.\` 前缀,`NoDefaultCurrentDirectoryInExePath=1` 导致 `cmd /c foo.bat` 静默失败)。DX12 批处理同理用 `.\compile_shaders_dx12.bat`。dxc 路径硬编码 `D:\VulkanSDK\1.3.296.0\Bin\dxc.exe`。
- 应用构建:`cmake --build F:\AdvancedVulkanRendering --config Debug --target AdvancedVulkanRendering`(repo 根即 in-source MSVC 构建目录,产物在 `Bin/`)。
- 运行:工作目录必须在 repo 根(`_rootPath` 解析依赖),`.\Bin\AdvancedVulkanRendering.exe`。
- 所有新增 HLSL 绑定/调用必须包在 `#ifndef DX12_BACKEND` 内;`commonstruct.hlsl` 改动会波及双后端全部 shader,DX12 批处理重编译必须零错误。
- `FrameConstants` 布局变动必须同步 `Common.h` 的 static_assert;本计划只把尾部 `_padIbl0/_padIbl1` 改名为 `vlmScale/vlmFlags`(offset 136/140),`sizeof` 保持 144 不变,DX12 侧行为不变。
- equirect↔方向约定在 CPU(`ComputeSH9FromEquirect`)与 GPU(`vlm_bake.hlsl` 的 `DirectionToEquirectUV`)间必须逐字一致:`u = atan2(-d.x, d.z)/(2π)+0.5`,`v = acos(d.y)/π`(`-d.x` 镜像是对 Apple 烘焙 KTX 的实证校准,见 `Src/Include/SphericalHarmonics.h` 注释)。
- 纯 CPU 模块(`VlmSh`/`VlmLayout`/`VlmAsset`)不得 include `vulkan.h` 或项目数学库,保证与 `Tests/` 独立编译(同 `SphericalHarmonics` 先例,`Tests/CMakeLists.txt` 只链对应 cpp)。
- RT 共享抽取(Task 4)不得改变相机 PT 数值行为:只允许移动代码与把 `sunConeRadius`/点光数/聚光数改为函数参数,调用点传回与原值相同的实参。
- 提交信息遵循仓库风格(`feat:` / `test:` / `docs:`),结尾带 `Co-Authored-By: Claude <noreply@anthropic.com>`。

## Review Focus

1. **查询点落在 BakeVolume 最大边界或外部** → cell 必须 clamp 到最后一个有效 cell 且局部坐标为 1(规格 §5.1:不能访问第 N 个不存在的邻居);外部 weight=0 回退天空 diffuse。Task 2 测试钉死 CPU 公式,Task 8 的 `vlm_common.hlsl` 用同一公式。
2. **SH 系数 FP16 溢出或非有限(如太阳直照探针)** → `VlmShPackFp16` 返回 false,bake 拒绝发布并报告探针号,不静默饱和(规格 §7)。Task 1 测试钉死,Task 6 bake 失败路径钉死。
3. **路径样本出现 NaN/Inf** → shader 置 errorFlags 并丢弃该样本,CPU 判整批失败、不写资产,绝不当零样本计入均值(规格 §6)。Task 5 shader 代码内,Task 6 检查 + 日志钉死。
4. **换了 env/灯光/几何后加载旧 `.vlm`** → 哈希不匹配 = Stale,默认禁用并日志告警,不得静默使用过期 GI(规格 §9)。Task 3 测试钉死判定,Task 8 加载路径钉死默认禁用。
5. **per-frame 填充覆盖 ImGui 的 `vlmScale`**(与 IBL `_padIbl` 教训同源)→ 默认值只在 `GpuScene.h` 成员初始化器写一次;Task 8 钉死并复查 per-frame 填充段(`GpuScene.cpp:3644-3675`)不含 `vlmScale`。

---

### Task 1: SH 常量共享化 + VlmSh 纯函数模块 + 测试

**Files:**
- Modify: `Src/Include/SphericalHarmonics.h`(追加共享常量声明)、`Src/SphericalHarmonics.cpp`(改用共享常量,值不变)
- Create: `Src/Include/VlmSh.h`、`Src/VlmSh.cpp`
- Test: `Tests/VlmShTests.cpp`
- Modify: `Tests/CMakeLists.txt`

**Interfaces:**
- Consumes: 现有 `struct SH9 { float c[9][3]; };`(`Src/Include/SphericalHarmonics.h:6-8`)。
- Produces(Task 5/6 依赖):
  - `inline constexpr float kSh9Y[9]` / `inline constexpr float kSh9A[9]` — SH 基归一化常数与卷积权重。
  - `inline void EvalSh9Basis(float dx, float dy, float dz, float out[9])` — 逐方向基求值(kY 已折入)。
  - `struct VlmShAccumulator { double sum[9][3]; uint64_t sampleCount; };`
  - `void VlmShAccumulate(VlmShAccumulator& acc, const float dir[3], const float radiance[3], float pdf);`
  - `SH9 VlmShFinalize(const VlmShAccumulator& acc);` — `c_j = kSh9A_j · kSh9Y_j · sum_j / N`,与 `ComputeSH9FromEquirect` 输出同语义(物理辐照度多项式)。
  - `void VlmShEvaluate(const SH9& sh, const float n[3], float outE[3]);` — 与 `ibl_common.hlsl` 的 `evaluateShCoefficients` 同约定。
  - `bool VlmShPackFp16(const SH9& sh, uint16_t out28[28]);` — `j*3+rgb` 展平 27 个数填 7 组 RGBA16F,第 28 通道写 0;任一值非有限或 |v|>65504 返回 false。
  - `uint16_t VlmFloatToHalf(float v);` / `float VlmHalfToFloat(uint16_t h);`

- [ ] **Step 1: 追加共享常量到 SphericalHarmonics.h(先过现有测试)**

`Src/Include/SphericalHarmonics.h` 在 `struct SH9` 声明之后追加:

```cpp
// —— VLM 共享物理约定(值与 ComputeSH9FromEquirect 内部常量逐字相同,行为不变)——
// kSh9Y:实数 SH 基归一化常数;kSh9A:辐照度卷积权重(π, 2π/3×3, π/4×5)。
inline constexpr float kSh9Y[9] = {
    0.282095f,
    0.488603f, 0.488603f, 0.488603f,
    1.092548f, 1.092548f, 0.315392f, 1.092548f, 0.546274f,
};
inline constexpr float kSh9A[9] = {
    3.1415926535897932f,
    2.0943951023931953f, 2.0943951023931953f, 2.0943951023931953f,
    0.7853981633974483f, 0.7853981633974483f, 0.7853981633974483f,
    0.7853981633974483f, 0.7853981633974483f,
};
// 逐方向基求值,顺序:常量、y、z、x、yx、yz、3z²-1、zx、x²-y²(kSh9Y 已折入)。
inline void EvalSh9Basis(float dx, float dy, float dz, float out[9]) {
  out[0] = kSh9Y[0];
  out[1] = kSh9Y[1] * dy;
  out[2] = kSh9Y[2] * dz;
  out[3] = kSh9Y[3] * dx;
  out[4] = kSh9Y[4] * dy * dx;
  out[5] = kSh9Y[5] * dy * dz;
  out[6] = kSh9Y[6] * (3.0f * dz * dz - 1.0f);
  out[7] = kSh9Y[7] * dz * dx;
  out[8] = kSh9Y[8] * (dx * dx - dy * dy);
}
```

`Src/SphericalHarmonics.cpp`:删除匿名命名空间里的 `kY`/`kA`/`EvalBasis` 定义,`ComputeSH9FromEquirect` 内 `EvalBasis(...)` 改为 `EvalSh9Basis(...)`,`kA[i] * kY[i]` 改为 `kSh9A[i] * kSh9Y[i]`;`ComputeMetalSH9FromEquirect` 内 `kY[i]` 改为 `kSh9Y[i]`。**数值不得有任何变化**。

编译运行现有测试验证无回归:

```powershell
cmake -S F:\AdvancedVulkanRendering\Tests -B F:\AdvancedVulkanRendering\build-vlm-tests
cmake --build F:\AdvancedVulkanRendering\build-vlm-tests --config Debug
F:\AdvancedVulkanRendering\build-vlm-tests\Debug\SphericalHarmonicsTests.exe
```

Expected: `all SH tests passed`(输出与改动前一致)。

- [ ] **Step 2: 写 VlmSh 失败测试**

`Tests/VlmShTests.cpp`(与 `Tests/SphericalHarmonicsTests.cpp` 同风格:自带 main、expect 计数):

```cpp
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
```

`Tests/CMakeLists.txt`:把

```cmake
add_executable(KtxTextureTests KtxTextureTests.cpp ../Src/KtxTexture.cpp)
add_executable(SphericalHarmonicsTests SphericalHarmonicsTests.cpp ../Src/SphericalHarmonics.cpp)
add_executable(IBLComparisonTests IBLComparisonTests.cpp)
foreach(test IN ITEMS KtxTextureTests SphericalHarmonicsTests IBLComparisonTests)
```

改为

```cmake
add_executable(KtxTextureTests KtxTextureTests.cpp ../Src/KtxTexture.cpp)
add_executable(SphericalHarmonicsTests SphericalHarmonicsTests.cpp ../Src/SphericalHarmonics.cpp)
add_executable(IBLComparisonTests IBLComparisonTests.cpp)
add_executable(VlmShTests VlmShTests.cpp ../Src/VlmSh.cpp ../Src/SphericalHarmonics.cpp)
foreach(test IN ITEMS KtxTextureTests SphericalHarmonicsTests IBLComparisonTests VlmShTests)
```

- [ ] **Step 3: 编译运行,确认失败**

```powershell
cmake -S F:\AdvancedVulkanRendering\Tests -B F:\AdvancedVulkanRendering\build-vlm-tests
cmake --build F:\AdvancedVulkanRendering\build-vlm-tests --config Debug
```

Expected: 编译失败(`VlmSh.h` 不存在)。

- [ ] **Step 4: 实现 VlmSh**

`Src/Include/VlmSh.h`:

```cpp
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
```

`Src/VlmSh.cpp`:

```cpp
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
```

- [ ] **Step 5: 重编译运行,确认通过**

```powershell
cmake --build F:\AdvancedVulkanRendering\build-vlm-tests --config Debug
F:\AdvancedVulkanRendering\build-vlm-tests\Debug\VlmShTests.exe
F:\AdvancedVulkanRendering\build-vlm-tests\Debug\SphericalHarmonicsTests.exe
```

Expected: `all VlmSh tests passed` + `all SH tests passed`(Step 1 重构无回归)。

- [ ] **Step 6: Commit**

```bash
git add Src/Include/SphericalHarmonics.h Src/SphericalHarmonics.cpp Src/Include/VlmSh.h Src/VlmSh.cpp Tests/VlmShTests.cpp Tests/CMakeLists.txt
git commit -m "feat: VLM SH9 Monte-Carlo projection/finalize/FP16 pack + shared SH constants"
```

### Task 2: VlmLayout 均匀网格布局纯函数 + 测试

**Files:**
- Create: `Src/Include/VlmLayout.h`、`Src/VlmLayout.cpp`
- Test: `Tests/VlmLayoutTests.cpp`
- Modify: `Tests/CMakeLists.txt`

**Interfaces:**
- Consumes: 无(纯 CPU,不 include vulkan.h / 项目数学库)。
- Produces(Task 5 shader 布局公式 / Task 6 VlmBaker / Task 8 VlmRuntime 依赖):
  - `struct VlmUniformLayout { float bmin[3]; float step[3]; uint32_t cells[3]; uint64_t ProbeCount() const; };` — 各轴 probe 数 = `cells[i] + 1`。
  - `VlmUniformLayout VlmMakeUniformLayout(const float bmin[3], const float bmax[3], float spacing);` — `cells[i] = max(1, ceil((bmax-bmin)/spacing))`,step 按 cells 重算使 `bmin + step*cells == bmax` 精确成立。
  - `uint32_t VlmProbeIndex(const VlmUniformLayout& L, uint32_t x, uint32_t y, uint32_t z);` — `(z*PY + y)*PX + x`,`PX=cells[0]+1, PY=cells[1]+1`。
  - `void VlmProbeWorldPos(const VlmUniformLayout& L, uint32_t x, uint32_t y, uint32_t z, float out[3]);` — `bmin + (x*step[0], y*step[1], z*step[2])`。
  - `void VlmCellLookup(const VlmUniformLayout& L, const float pos[3], int32_t outBaseCell[3], float outFrac[3]);` — `local=(pos-bmin)/step; cell=clamp(floor(local),0,cells-1); frac=saturate(local-cell)`(最大边界 → cell=cells-1、frac=1,规格 §5.1)。
  - `bool VlmInside(const VlmUniformLayout& L, const float pos[3]);` — 全部分量有限且在闭区间 `[bmin, bmin+step*cells]` 内。
  - `float VlmBoundaryWeight(const VlmUniformLayout& L, const float pos[3], float bandWidth);` — `d = min(pos-bmin, bmax-pos)` 三轴取最小,`saturate(dMin / bandWidth)`(外部→0,边界→0,向内一层 cell→1,规格 §8.3)。

- [ ] **Step 1: 写失败测试**

`Tests/VlmLayoutTests.cpp`:

```cpp
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
  TestMakeLayout();
  TestTinyExtent();
  TestIndexRoundTrip();
  TestCellLookupBounds();
  TestBoundaryWeight();
  if (failures == 0) std::printf("all VlmLayout tests passed\n");
  return failures == 0 ? 0 : 1;
}
```

`Tests/CMakeLists.txt` 追加 `add_executable(VlmLayoutTests VlmLayoutTests.cpp ../Src/VlmLayout.cpp)` 并把 `VlmLayoutTests` 加入 `foreach` 列表(同 Task 1 Step 2 的模式)。

- [ ] **Step 2: 编译运行,确认失败**

```powershell
cmake --build F:\AdvancedVulkanRendering\build-vlm-tests --config Debug
```

Expected: 编译失败(`VlmLayout.h` 不存在;若构建目录未重配置,先重跑 Step 1 末尾的 `cmake -S` 配置命令)。

- [ ] **Step 3: 实现**

`Src/Include/VlmLayout.h`:

```cpp
#pragma once

#include <cstdint>

// VLM 均匀网格布局(规格 §5.1)。各轴 probe 数 = cells+1;
// 查询最大边界时必须选最后一个有效 cell 且局部坐标为 1。
struct VlmUniformLayout {
  float bmin[3];
  float step[3];
  uint32_t cells[3];

  uint64_t ProbeCount() const {
    return (uint64_t)(cells[0] + 1) * (uint64_t)(cells[1] + 1) * (uint64_t)(cells[2] + 1);
  }
};

VlmUniformLayout VlmMakeUniformLayout(const float bmin[3], const float bmax[3], float spacing);
uint32_t VlmProbeIndex(const VlmUniformLayout& L, uint32_t x, uint32_t y, uint32_t z);
void VlmProbeWorldPos(const VlmUniformLayout& L, uint32_t x, uint32_t y, uint32_t z, float out[3]);
void VlmCellLookup(const VlmUniformLayout& L, const float pos[3], int32_t outBaseCell[3], float outFrac[3]);
bool VlmInside(const VlmUniformLayout& L, const float pos[3]);
float VlmBoundaryWeight(const VlmUniformLayout& L, const float pos[3], float bandWidth);
```

`Src/VlmLayout.cpp`:

```cpp
#include "VlmLayout.h"

#include <cmath>

namespace {
float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
}

VlmUniformLayout VlmMakeUniformLayout(const float bmin[3], const float bmax[3], float spacing) {
  VlmUniformLayout L{};
  for (int i = 0; i < 3; ++i) {
    const float extent = bmax[i] - bmin[i];
    const uint32_t cells = extent > 0.0f
        ? (uint32_t)std::max(1.0f, std::ceil(extent / spacing))
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
```

- [ ] **Step 4: 重编译运行,确认通过**

```powershell
cmake --build F:\AdvancedVulkanRendering\build-vlm-tests --config Debug
F:\AdvancedVulkanRendering\build-vlm-tests\Debug\VlmLayoutTests.exe
```

Expected: `all VlmLayout tests passed`。

- [ ] **Step 5: Commit**

```bash
git add Src/Include/VlmLayout.h Src/VlmLayout.cpp Tests/VlmLayoutTests.cpp Tests/CMakeLists.txt
git commit -m "feat: VLM uniform grid layout (probe indexing, cell lookup, boundary weight)"
```

### Task 3: VlmAsset(.vlm 资产格式)+ 测试

**Files:**
- Create: `Src/Include/VlmAsset.h`、`Src/VlmAsset.cpp`
- Test: `Tests/VlmAssetTests.cpp`
- Modify: `Tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `VlmUniformLayout`(Task 2,仅概念对齐;本模块自含不依赖)。
- Produces(Task 6 填充 / Task 8 加载依赖):
  - `constexpr uint32_t kVlmMagic = 0x314D4C56u;`(`'VLM1'` LE)、`kVlmVersion = 1`、`kVlmEncodingIrradiancePolynomialSH9V1 = 1`、`kVlmLayoutUniform = 1`、`kVlmIntegratorVersion = 1`。
  - `struct VlmAssetData`(字段见下);`uint64_t VlmAssetData::ProbeCount() const`。
  - `bool VlmSaveAsset(const std::string& path, const VlmAssetData& asset);` — 写 `path + ".tmp"` 校验后原子替换。
  - `enum class VlmLoadResult { Ok, Stale, Rejected };`
  - `VlmLoadResult VlmLoadAsset(const std::string& path, VlmAssetData& out, uint64_t expectedSceneHash, std::string& err);`
  - `uint32_t VlmCrc32(const void* data, size_t size);`
  - `uint64_t VlmFnv1a64(const void* data, size_t size, uint64_t seed);` + `inline constexpr uint64_t kVlmFnv1aBasis = 1469598103934665603ull;`

**文件格式(全部小端逐字段写,不序列化 C++ struct):**

```text
[0,4)    u32 magic = 0x314D4C56
[4,8)    u32 version = 1
[8,12)   u32 endianMarker = 0x01020304(读者必须读到相同值,否则拒收)
[12,16)  u32 encoding = 1(IrradiancePolynomialSH9_v1)
[16,20)  u32 layoutType = 1(uniform)
[20,24)  u32 headerFlags = 0
[24,32)  u64 totalLength(= 文件总字节数)
[32,36)  u32 crc32(覆盖 [64, totalLength) 区间)
[36,40)  u32 probeCount(必须 == Π(cells[i]+1))
[40,48)  u64 sceneHash
[48,56)  u64 envHash
[56,64)  u64 lightHash
[64,72)  u64 settingsHash
[72,76)  u32 integratorVersion = 1
[76,80)  u32 samplesPerProbe
[80,92)  f32 bmin[3]
[92,104) f32 step[3]
[104,116) u32 cells[3]
[116,120) f32 bandWidth
[120,124) f32 unitScale
[124,128) u32 diagFlags(v1 必须为 0;bit0 预留给"有偏异常值抑制"元数据)
[128,132) u32 reserved = 0
[132, 132+probeCount)            validity:probeCount × u8
[132+probeCount, totalLength)    lighting:probeCount × 28 × u16(56 B/probe)
totalLength == 132 + probeCount × 57
```

**加载校验(任一失败 → Rejected 并填 err):** 文件可读;size ≥ 132;magic/version/endianMarker/encoding/layoutType/headerFlags;`totalLength == 实际文件大小`;`totalLength == 132 + probeCount × 57`;crc32 匹配;`probeCount == Π(cells+1)`;`cells[i] ∈ [1, 4096]`;bmin/step/bandWidth/unitScale 全部有限且 step>0、unitScale>0;diagFlags == 0。`sceneHash != expectedSceneHash` → **Stale**(out 仍填满,由调用方决定禁用)。

- [ ] **Step 1: 写失败测试**

`Tests/VlmAssetTests.cpp`:

```cpp
#include "VlmAsset.h"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <vector>

namespace {

int failures = 0;

void expectTrue(bool cond, const char* message) {
  if (!cond) { std::fprintf(stderr, "FAIL %s\n", message); ++failures; }
}

VlmAssetData makeAsset() {
  VlmAssetData a;
  a.sceneHash = 111; a.envHash = 222; a.lightHash = 333; a.settingsHash = 444;
  a.bmin[0] = -1; a.bmin[1] = -2; a.bmin[2] = -3;
  a.step[0] = 1.0f; a.step[1] = 1.0f; a.step[2] = 1.0f;
  a.cells[0] = 1; a.cells[1] = 2; a.cells[2] = 3; // probes = 2*3*4 = 24
  a.bandWidth = 1.0f; a.unitScale = 1.0f;
  a.samplesPerProbe = 2048; a.integratorVersion = 1; a.flags = 0;
  const size_t probes = (size_t)a.ProbeCount();
  a.validity.assign(probes, 1);
  a.shFp16.resize(probes * 28);
  for (size_t i = 0; i < a.shFp16.size(); ++i) a.shFp16[i] = (uint16_t)(i * 37 % 65536);
  return a;
}

std::vector<uint8_t> readFile(const char* path) {
  std::ifstream f(path, std::ios::binary);
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(f), {});
}

bool assetEqual(const VlmAssetData& a, const VlmAssetData& b) {
  if (a.sceneHash != b.sceneHash || a.envHash != b.envHash ||
      a.lightHash != b.lightHash || a.settingsHash != b.settingsHash) return false;
  for (int i = 0; i < 3; ++i) {
    if (a.bmin[i] != b.bmin[i] || a.step[i] != b.step[i] || a.cells[i] != b.cells[i]) return false;
  }
  return a.bandWidth == b.bandWidth && a.unitScale == b.unitScale &&
         a.samplesPerProbe == b.samplesPerProbe && a.flags == b.flags &&
         a.validity == b.validity && a.shFp16 == b.shFp16;
}

void rewriteFile(const char* path, std::vector<uint8_t> bytes) {
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  f.write((const char*)bytes.data(), (std::streamsize)bytes.size());
}

void TestRoundTrip() {
  const char* path = "vlm_test_roundtrip.vlm";
  VlmAssetData a = makeAsset();
  expectTrue(VlmSaveAsset(path, a), "save ok");
  expectTrue(std::ifstream(path).good(), "file exists");
  expectTrue(!std::ifstream(std::string(path) + ".tmp").good(), "tmp file cleaned");

  VlmAssetData b;
  std::string err;
  expectTrue(VlmLoadAsset(path, b, 111, err) == VlmLoadResult::Ok, "load ok");
  expectTrue(assetEqual(a, b), "round trip equal");
  std::remove(path);
}

void TestStaleOnHashMismatch() {
  const char* path = "vlm_test_stale.vlm";
  expectTrue(VlmSaveAsset(path, makeAsset()), "save ok");
  VlmAssetData b;
  std::string err;
  expectTrue(VlmLoadAsset(path, b, 999, err) == VlmLoadResult::Stale, "hash mismatch -> Stale");
  expectTrue(b.cells[1] == 2, "Stale still fills data");
  std::remove(path);
}

void TestRejectCorruptions() {
  const char* path = "vlm_test_corrupt.vlm";
  expectTrue(VlmSaveAsset(path, makeAsset()), "save ok");
  const std::vector<uint8_t> good = readFile(path);
  VlmAssetData b;
  std::string err;

  auto expectReject = [&](std::vector<uint8_t> bytes, const char* what) {
    rewriteFile(path, bytes);
    err.clear();
    if (VlmLoadAsset(path, b, 111, err) != VlmLoadResult::Rejected) {
      std::fprintf(stderr, "FAIL corruption not rejected: %s\n", what);
      ++failures;
    }
  };

  { auto v = good; v[0] ^= 0xFF; expectReject(v, "bad magic"); }
  { auto v = good; v[4] = 99; expectReject(v, "bad version"); }
  { auto v = good; v[12] = 7; expectReject(v, "unknown encoding"); }
  { auto v = good; v[16] = 9; expectReject(v, "unknown layout"); }
  { auto v = good; v.resize(good.size() - 10); expectReject(v, "truncated"); }
  { auto v = good; v.back() ^= 0x01; expectReject(v, "crc mismatch"); }
  { auto v = good; v[104] = 0; expectReject(v, "cells[0]=0 rejected"); }
  { // bmin 改 NaN 后必须重算 crc 才能走到 NaN 检查:直接改前 64 字节外的 bmin 区,
    // 但 crc 会失配——所以本用例验证的是"crc 先挡下";NaN 专项在 save 侧不负责,
    // 加载侧对通过 crc 的数据仍逐字段查有限性,此处用合法 crc 的脏数据:
    auto v = good;
    float nan = NAN;
    for (int i = 0; i < 4; ++i) v[80 + i] = ((const uint8_t*)&nan)[i];
    // 重算 crc 使数据"形式合法":
    uint32_t crc = VlmCrc32(v.data() + 64, v.size() - 64);
    for (int i = 0; i < 4; ++i) v[32 + i] = ((const uint8_t*)&crc)[i];
    expectReject(v, "NaN bmin rejected");
  }
  std::remove(path);
}

void TestMissingFile() {
  VlmAssetData b;
  std::string err;
  expectTrue(VlmLoadAsset("vlm_test_does_not_exist.vlm", b, 0, err) == VlmLoadResult::Rejected,
             "missing file rejected");
  expectTrue(!err.empty(), "error message filled");
}

} // namespace

int main() {
  TestRoundTrip();
  TestStaleOnHashMismatch();
  TestRejectCorruptions();
  TestMissingFile();
  if (failures == 0) std::printf("all VlmAsset tests passed\n");
  return failures == 0 ? 0 : 1;
}
```

`Tests/CMakeLists.txt` 追加 `add_executable(VlmAssetTests VlmAssetTests.cpp ../Src/VlmAsset.cpp)` 并把 `VlmAssetTests` 加入 `foreach` 列表。

- [ ] **Step 2: 编译运行,确认失败**

```powershell
cmake --build F:\AdvancedVulkanRendering\build-vlm-tests --config Debug
```

Expected: 编译失败(`VlmAsset.h` 不存在)。

- [ ] **Step 3: 实现**

`Src/Include/VlmAsset.h`:

```cpp
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// .vlm 资产(规格 §9):小端逐字段写,不直接序列化 C++ struct。
// 编码 IrradiancePolynomialSH9_v1:物理辐照度多项式系数(A_l 与基常数已折入),
// 与 ComputeSH9FromEquirect 输出同语义;禁止按 Apple compatibility SH 解读。
inline constexpr uint32_t kVlmMagic = 0x314D4C56u; // 'VLM1' little-endian
inline constexpr uint32_t kVlmVersion = 1;
inline constexpr uint32_t kVlmEndianMarker = 0x01020304u;
inline constexpr uint32_t kVlmEncodingIrradiancePolynomialSH9V1 = 1;
inline constexpr uint32_t kVlmLayoutUniform = 1;
inline constexpr uint32_t kVlmIntegratorVersion = 1;

struct VlmAssetData {
  uint64_t sceneHash = 0, envHash = 0, lightHash = 0, settingsHash = 0;
  float bmin[3] = {};
  float step[3] = {};
  uint32_t cells[3] = {};
  float bandWidth = 0.0f;
  float unitScale = 1.0f;
  uint32_t samplesPerProbe = 0;
  uint32_t integratorVersion = kVlmIntegratorVersion;
  uint32_t flags = 0;
  std::vector<uint8_t> validity; // probeCount 字节(阶段 1 全 1)
  std::vector<uint16_t> shFp16;  // probeCount × 28

  uint64_t ProbeCount() const {
    return (uint64_t)(cells[0] + 1) * (uint64_t)(cells[1] + 1) * (uint64_t)(cells[2] + 1);
  }
};

bool VlmSaveAsset(const std::string& path, const VlmAssetData& asset);

enum class VlmLoadResult { Ok, Stale, Rejected };
VlmLoadResult VlmLoadAsset(const std::string& path, VlmAssetData& out,
                           uint64_t expectedSceneHash, std::string& err);

uint32_t VlmCrc32(const void* data, size_t size);
inline constexpr uint64_t kVlmFnv1aBasis = 1469598103934665603ull;
uint64_t VlmFnv1a64(const void* data, size_t size, uint64_t seed);
```

`Src/VlmAsset.cpp`:

```cpp
#include "VlmAsset.h"

#include <cmath>
#include <cstdio>
#include <fstream>

namespace {

void putU32(std::vector<uint8_t>& o, uint32_t v) {
  o.push_back((uint8_t)(v & 0xFF));
  o.push_back((uint8_t)((v >> 8) & 0xFF));
  o.push_back((uint8_t)((v >> 16) & 0xFF));
  o.push_back((uint8_t)((v >> 24) & 0xFF));
}
void putU64(std::vector<uint8_t>& o, uint64_t v) {
  for (int i = 0; i < 8; ++i) o.push_back((uint8_t)((v >> (i * 8)) & 0xFF));
}
void putF32(std::vector<uint8_t>& o, float f) {
  uint32_t v;
  static_assert(sizeof(v) == sizeof(f));
  std::memcpy(&v, &f, 4);
  putU32(o, v);
}

struct Reader {
  const uint8_t* p;
  size_t size;
  size_t pos = 0;
  bool ok = true;
  uint32_t u32() {
    if (pos + 4 > size) { ok = false; return 0; }
    uint32_t v = (uint32_t)p[pos] | ((uint32_t)p[pos + 1] << 8) |
                 ((uint32_t)p[pos + 2] << 16) | ((uint32_t)p[pos + 3] << 24);
    pos += 4;
    return v;
  }
  uint64_t u64() {
    uint64_t lo = u32();
    uint64_t hi = u32();
    return lo | (hi << 32);
  }
  float f32() {
    uint32_t v = u32();
    float f;
    std::memcpy(&f, &v, 4);
    return f;
  }
};

bool writeFileAtomic(const std::string& path, const std::vector<uint8_t>& bytes) {
  const std::string tmp = path + ".tmp";
  {
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    f.write((const char*)bytes.data(), (std::streamsize)bytes.size());
    if (!f.good()) { std::remove(tmp.c_str()); return false; }
  }
#ifdef _WIN32
  // Windows:rename 不覆盖已存在目标,用 MoveFileExA 保证原子替换。
  if (MoveFileExA(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0) {
    std::remove(tmp.c_str());
    return false;
  }
  return true;
#else
  std::error_code ec;
  std::filesystem::rename(tmp, path, ec);
  if (ec) { std::remove(tmp.c_str()); return false; }
  return true;
#endif
}

} // namespace

uint32_t VlmCrc32(const void* data, size_t size) {
  static uint32_t table[256];
  static bool init = false;
  if (!init) {
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
      table[i] = c;
    }
    init = true;
  }
  uint32_t crc = 0xFFFFFFFFu;
  const uint8_t* b = (const uint8_t*)data;
  for (size_t i = 0; i < size; ++i) crc = table[(crc ^ b[i]) & 0xFF] ^ (crc >> 8);
  return crc ^ 0xFFFFFFFFu;
}

uint64_t VlmFnv1a64(const void* data, size_t size, uint64_t seed) {
  uint64_t h = seed;
  const uint8_t* b = (const uint8_t*)data;
  for (size_t i = 0; i < size; ++i) { h ^= b[i]; h *= 1099511628211ull; }
  return h;
}

bool VlmSaveAsset(const std::string& path, const VlmAssetData& a) {
  const uint64_t probes = a.ProbeCount();
  if (probes == 0 || a.validity.size() != probes || a.shFp16.size() != probes * 28) return false;

  std::vector<uint8_t> o;
  o.reserve(132 + (size_t)probes * 57);
  // 头部 [0,64):先占位,CRC 后补。
  putU32(o, kVlmMagic);
  putU32(o, kVlmVersion);
  putU32(o, kVlmEndianMarker);
  putU32(o, kVlmEncodingIrradiancePolynomialSH9V1);
  putU32(o, kVlmLayoutUniform);
  putU32(o, 0);                    // headerFlags
  putU64(o, 0);                    // totalLength 占位
  putU32(o, 0);                    // crc32 占位
  putU32(o, (uint32_t)probes);
  putU64(o, a.sceneHash);
  putU64(o, a.envHash);
  putU64(o, a.lightHash);
  // [64, ...)
  putU64(o, a.settingsHash);
  putU32(o, a.integratorVersion);
  putU32(o, a.samplesPerProbe);
  for (int i = 0; i < 3; ++i) putF32(o, a.bmin[i]);
  for (int i = 0; i < 3; ++i) putF32(o, a.step[i]);
  for (int i = 0; i < 3; ++i) putU32(o, a.cells[i]);
  putF32(o, a.bandWidth);
  putF32(o, a.unitScale);
  putU32(o, 0); // diagFlags:v1 恒 0
  putU32(o, 0); // reserved
  for (uint8_t v : a.validity) o.push_back(v);
  for (uint16_t v : a.shFp16) {
    o.push_back((uint8_t)(v & 0xFF));
    o.push_back((uint8_t)((v >> 8) & 0xFF));
  }

  // 回填 totalLength 与 crc32(覆盖 [64, end))。
  const uint64_t total = o.size();
  for (int i = 0; i < 8; ++i) o[24 + i] = (uint8_t)((total >> (i * 8)) & 0xFF);
  const uint32_t crc = VlmCrc32(o.data() + 64, o.size() - 64);
  for (int i = 0; i < 4; ++i) o[32 + i] = (uint8_t)((crc >> (i * 8)) & 0xFF);

  return writeFileAtomic(path, o);
}

VlmLoadResult VlmLoadAsset(const std::string& path, VlmAssetData& out,
                           uint64_t expectedSceneHash, std::string& err) {
  auto reject = [&](const char* what) {
    err = std::string("vlm load rejected: ") + what;
    return VlmLoadResult::Rejected;
  };

  std::ifstream f(path, std::ios::binary);
  if (!f.good()) return reject("file not readable");
  std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), {});
  if (bytes.size() < 132) return reject("file too small");

  Reader r{bytes.data(), bytes.size()};
  if (r.u32() != kVlmMagic) return reject("bad magic");
  if (r.u32() != kVlmVersion) return reject("unsupported version");
  if (r.u32() != kVlmEndianMarker) return reject("endian marker mismatch");
  if (r.u32() != kVlmEncodingIrradiancePolynomialSH9V1) return reject("unknown encoding");
  if (r.u32() != kVlmLayoutUniform) return reject("unknown layout type");
  if (r.u32() != 0) return reject("unknown header flags");
  const uint64_t totalLength = r.u64();
  const uint32_t crc = r.u32();
  const uint32_t probeCount = r.u32();
  out.sceneHash = r.u64();
  out.envHash = r.u64();
  out.lightHash = r.u64();
  out.settingsHash = r.u64();
  out.integratorVersion = r.u32();
  out.samplesPerProbe = r.u32();
  for (int i = 0; i < 3; ++i) out.bmin[i] = r.f32();
  for (int i = 0; i < 3; ++i) out.step[i] = r.f32();
  for (int i = 0; i < 3; ++i) out.cells[i] = r.u32();
  out.bandWidth = r.f32();
  out.unitScale = r.f32();
  const uint32_t diagFlags = r.u32();
  r.u32(); // reserved
  if (!r.ok) return reject("header truncated");

  if (totalLength != bytes.size()) return reject("totalLength != file size");
  if (VlmCrc32(bytes.data() + 64, bytes.size() - 64) != crc) return reject("crc32 mismatch");
  if (probeCount == 0 || probeCount != out.ProbeCount()) return reject("probeCount mismatch");
  for (int i = 0; i < 3; ++i)
    if (out.cells[i] < 1 || out.cells[i] > 4096) return reject("cells out of range");
  for (int i = 0; i < 3; ++i)
    if (!std::isfinite(out.bmin[i]) || !std::isfinite(out.step[i]) || out.step[i] <= 0.0f)
      return reject("non-finite or non-positive volume params");
  if (!std::isfinite(out.bandWidth) || !std::isfinite(out.unitScale) || out.unitScale <= 0.0f)
    return reject("non-finite band/unit params");
  if (diagFlags != 0) return reject("diagFlags unsupported in v1");
  if (totalLength != 132ull + (uint64_t)probeCount * 57ull) return reject("chunk size mismatch");

  out.validity.assign(bytes.begin() + 132, bytes.begin() + 132 + probeCount);
  out.shFp16.resize((size_t)probeCount * 28);
  const uint8_t* sp = bytes.data() + 132 + probeCount;
  for (size_t i = 0; i < out.shFp16.size(); ++i)
    out.shFp16[i] = (uint16_t)sp[i * 2] | ((uint16_t)sp[i * 2 + 1] << 8);
  out.flags = 0;

  if (out.sceneHash != expectedSceneHash) {
    err = "vlm load: scene hash mismatch (stale asset)";
    return VlmLoadResult::Stale;
  }
  return VlmLoadResult::Ok;
}
```

注意 `writeFileAtomic` 的 `_WIN32` 分支需要 `#include <windows.h>`(放文件顶部 `#ifdef _WIN32` 内);非 Windows 分支需要 `#include <filesystem>`。`Reader`/`putF32` 用到 `std::memcpy`,顶部加 `#include <cstring>`。

- [ ] **Step 4: 重编译运行,确认通过**

```powershell
cmake --build F:\AdvancedVulkanRendering\build-vlm-tests --config Debug
F:\AdvancedVulkanRendering\build-vlm-tests\Debug\VlmAssetTests.exe
```

Expected: `all VlmAsset tests passed`。

- [ ] **Step 5: Commit**

```bash
git add Src/Include/VlmAsset.h Src/VlmAsset.cpp Tests/VlmAssetTests.cpp Tests/CMakeLists.txt
git commit -m "feat: .vlm asset format (chunked LE, crc32, scene-hash staleness, atomic save)"
```

### Task 4: 抽取 rt_path_common.hlsl(共享命中/材质/BRDF/NEE)+ RT 路径回归

**Files:**
- Create: `shaders/rt_path_common.hlsl`
- Modify: `shaders/rt_lighting.hlsl`(改为 include + 三个调用点适配)

**Interfaces:**
- Consumes: `shaders/commonstruct.hlsl`(`AAPLFrameConstants`/`CameraParamsBufferFull`/`AAPLMeshChunk`/`AAPLPointLightCullingData`/`AAPLSpotLightCullingData`)。
- Produces(Task 5 的 `vlm_bake.hlsl` include 同一头,直接使用以下全部符号):
  - 绑定:set0 `cbuffer cam { CameraParamsBufferFull cameraParams; AAPLFrameConstants frameConstants; }` (0,0);set1 `tlas` (0)、`vbPositions/vbNormals/vbTangents`(ByteAddressBuffer,2/3/4)、`vbUVs`(StructuredBuffer<float2>,5)、`ibIndices`(ByteAddressBuffer,6)、`meshChunksRT` (7)、`materialsRT` (8)、`pointLightsRT` (9)、`_Textures[]` (10)、`_LinearRepeatSampler` (11)、`spotLightsRT` (13)。
  - 类型:`AAPLShaderMaterial`、`PrimaryPayload`、`ShadowPayload`、`HitInputs`。
  - 常量:`ALPHA_CUTOUT`、`PI`、`TWO_PI`、`INV_PI`。
  - 函数:`pcgNext/rngF/rng2F`、`buildBasis/localToWorld`、`evalBRDF`、`sampleDiffuse/diffusePdf`、`sampleGGX/specularPdf`、`sampleBRDF`、`fetchTriangleIndices`、`gatherHit`、`fetchHitUV`、`distanceAttenuation`、`traceShadowRay(origin,dir,tMax)`。
  - **签名变化**:`float3 sampleSunDir(inout uint rng, float coneRadius)`(原读 `pc.sunConeRadius`);`float3 evalLocalLightsNEE(float3 wsP, float3 N, float3 V, float3 albedo, float3 F0, float roughness, uint pointCount, uint spotCount)`(原读 `pc.pointLightCount/spotLightCount`)。
  - shader 入口(被两个模块共用):`ClosestHitPrimary`、`AnyHitAlpha`、`AnyHitAlphaShadow`、`MissShadow`。

**抽取清单(全部逐字移动,行号指改动前 `rt_lighting.hlsl`):**

移入 `rt_path_common.hlsl`:`#include "commonstruct.hlsl"`(14);cam cbuffer(17-20);`tlas`(22);几何/材质/灯光/纹理绑定(28-40 行中除 `outLitColor`/`outAccumColor` 外全部:28-32、34-35、37-38、40)+ `AAPLShaderMaterial`+`materialsRT`(43-53);`ALPHA_CUTOUT/PI/TWO_PI/INV_PI`(69-72);`PrimaryPayload/ShadowPayload`(76-87);`pcgNext/rngF/rng2F`(90-97);`buildBasis/localToWorld`(100-109);`evalBRDF`(125-147);`sampleDiffuse/diffusePdf`(150-158);`sampleGGX/specularPdf`(161-182);`sampleBRDF`(185-209);`fetchTriangleIndices`(212-215);`HitInputs/gatherHit`(217-253);`sampleSunDir`(258-270,改签名);`distanceAttenuation`(273-280);`traceShadowRay`(283-295);`evalLocalLightsNEE`(301-357,改签名);`ClosestHitPrimary`(504-545);`fetchHitUV`(550-562);`AnyHitAlpha`(564-574);`AnyHitAlphaShadow`(576-586);`MissShadow`(499-502)。

留在 `rt_lighting.hlsl`:`outLitColor`(23)、`outAccumColor`(39)、`RTPushConsts`+`pc`(56-66)、`FIREFLY_CLAMP`(73)、`skyColor`(112-115)、`aces`(118-121)、`cameraRayFromPixel`(360-370)、`RayGen`(376-492)、`MissPrimary`(494-497),头部注释与编译说明。

- [ ] **Step 1: 拍摄 RT 基线**

构建当前代码并运行,ImGui 勾选 "Ray Tracing",等 accumulation 稳定(~500 samples)后截图保存为 `vlm_rt_baseline.png`(放 repo 外或用 RenderDoc 抓帧存 `captures/`):

```powershell
cmake --build F:\AdvancedVulkanRendering --config Debug --target AdvancedVulkanRendering
.\Bin\AdvancedVulkanRendering.exe
```

Expected: RT 画面正常(bistro 路径追踪,右上角 "Samples accumulated" 递增)。

- [ ] **Step 2: 创建 rt_path_common.hlsl 并改写 rt_lighting.hlsl**

`shaders/rt_path_common.hlsl` 头部与两个改签名函数的全文(其余按抽取清单逐字移动):

```hlsl
// rt_path_common.hlsl — 相机 PT(rt_lighting.hlsl)与 VLM 烘焙(vlm_bake.hlsl)
// 共享的命中/材质/BRDF/NEE 工具。仅 Vulkan RT shader 使用(dxc lib_6_3)。
// 本文件包含 shader 入口:ClosestHitPrimary / AnyHitAlpha / AnyHitAlphaShadow /
// MissShadow,include 本文件的模块会直接获得这些入口。
// 注意:SBT 约定在两个模块间必须一致——miss[0]=primary, miss[1]=shadow;
// hit group 每 geometry 2 条:[g*2+0]=primary(chit[+ahit]), [g*2+1]=shadow([ahit])。

#include "commonstruct.hlsl"

// --- Bindings(set0 全局相机/帧常量;set1 RT 场景资源,与 Raytracing.cpp 布局一致)---
[[vk::binding(0,0)]] cbuffer cam {
    CameraParamsBufferFull cameraParams;
    AAPLFrameConstants     frameConstants;
};

[[vk::binding(0,1)]] RaytracingAccelerationStructure tlas;

// vbPositions/Normals/Tangents 为紧凑 float3(stride=12),用 ByteAddressBuffer
// 避免 StructuredBuffer<float3> 的 16B stride  padding 与 BLAS vertexStride=12 错位。
[[vk::binding(2,1)]] ByteAddressBuffer               vbPositions;
[[vk::binding(3,1)]] ByteAddressBuffer               vbNormals;
[[vk::binding(4,1)]] ByteAddressBuffer               vbTangents;
[[vk::binding(5,1)]] StructuredBuffer<float2>        vbUVs;
[[vk::binding(6,1)]] ByteAddressBuffer               ibIndices;

[[vk::binding(7,1)]]  StructuredBuffer<AAPLMeshChunk>             meshChunksRT;
[[vk::binding(9,1)]]  StructuredBuffer<AAPLPointLightCullingData> pointLightsRT;

[[vk::binding(10,1)]] Texture2D<half4>    _Textures[];
[[vk::binding(11,1)]] SamplerState        _LinearRepeatSampler;
[[vk::binding(13,1)]] StructuredBuffer<AAPLSpotLightCullingData> spotLightsRT;

// AAPLShaderMaterial layout matches GpuScene.h (alignas(16)).
struct AAPLShaderMaterial {
    uint   albedo_texture_index;
    uint   roughness_texture_index;
    uint   normal_texture_index;
    uint   emissive_texture_index;
    float  alpha;
    uint   hasMetallicRoughness;
    uint   hasEmissive;
    uint   _pad;
};
[[vk::binding(8,1)]] StructuredBuffer<AAPLShaderMaterial> materialsRT;

// ... 其余按抽取清单逐字移动(常量/payload/RNG/basis/BRDF/采样/gather/NEE/入口)...
```

两个改签名的函数(移动时一并改):

```hlsl
// coneRadius 原为 pc.sunConeRadius;调用方传入,rt 传 pc.sunConeRadius,
// vlm_bake 传 pc.sunConeRadius(烘焙 push const 同名字段)。
float3 sampleSunDir(inout uint rng, float coneRadius) {
    float3 sunAxis = normalize(frameConstants.sunDirection);
    float cosThetaMax = cos(coneRadius);
    float2 xi = rng2F(rng);
    float cosTheta = lerp(cosThetaMax, 1.0f, xi.x);
    float sinTheta = sqrt(saturate(1.0f - cosTheta * cosTheta));
    float phi = TWO_PI * xi.y;
    float3 local = float3(cos(phi) * sinTheta, sin(phi) * sinTheta, cosTheta);
    float3 up = abs(sunAxis.z) < 0.999f ? float3(0,0,1) : float3(1,0,0);
    float3 t  = normalize(cross(up, sunAxis));
    float3 b  = cross(sunAxis, t);
    return normalize(local.x * t + local.y * b + local.z * sunAxis);
}
```

```hlsl
// pointCount/spotCount 原为 pc.pointLightCount/spotLightCount;调用方传入。
float3 evalLocalLightsNEE(float3 wsP, float3 N, float3 V,
                          float3 albedo, float3 F0, float roughness,
                          uint pointCount, uint spotCount) {
    float3 result = (float3)0;
    for (uint i = 0; i < pointCount; ++i) {
        // ... 循环体逐字同原 301-357,仅 pc.pointLightCount→pointCount ...
    }
    for (uint j = 0; j < spotCount; ++j) {
        // ... 逐字同上,pc.spotLightCount→spotCount ...
    }
    return result;
}
```

`rt_lighting.hlsl` 三个调用点适配(其余不动):

```hlsl
// 1) RayGen 太阳 NEE(原 426-446):sampleSunDir 传 coneRadius;
//    内联阴影 TraceRay 改为共享 traceShadowRay(语义逐字相同:
//    ACCEPT_FIRST_HIT_AND_END_SEARCH|SKIP_CLOSEST_HIT、hgOffset=1、stride=2、missIdx=1、TMin=1e-3)。
        {
            float3 sdir = sampleSunDir(rng, pc.sunConeRadius);
            if (dot(N, sdir) > 0.0f) {
                if (traceShadowRay(wsP, sdir, 1e30f) > 0.0f) {
                    float3 sunLight = frameConstants.sunColor * PI;
                    float3 contrib  = throughput * evalBRDF(N, V, sdir, p.albedo, p.F0, p.roughness) * sunLight;
                    radiance += contrib;
                    radiance  = min(radiance, FIREFLY_CLAMP);
                }
            }
        }

// 2) RayGen 局部灯 NEE(原 451-456):补两个灯数实参。
        if (bounce == 0) {
            float3 localContrib = evalLocalLightsNEE(wsP, N, V,
                                                     p.albedo, p.F0, p.roughness,
                                                     pc.pointLightCount, pc.spotLightCount);
            radiance += throughput * localContrib;
            radiance  = min(radiance, FIREFLY_CLAMP);
        }
```

`rt_lighting.hlsl` 顶部 include 区改为:

```hlsl
#include "rt_path_common.hlsl"
```

(原 `#include "commonstruct.hlsl"` 由共享头传递;`rt_lighting.hlsl` 内不再重复声明已移走的绑定/结构/函数。)

- [ ] **Step 3: 编译 shader + 确认 DX12 批处理不涉及该文件**

```powershell
cmd /c "cd /d F:\AdvancedVulkanRendering\shaders && .\compile_shaders.bat"
findstr /C:"rt_lighting" F:\AdvancedVulkanRendering\shaders\compile_shaders_dx12.bat
```

Expected: dxc 零错误,`rt_lighting.lib.spv` 重新生成;`findstr` 无输出(DX12 批处理不编译 RT shader,抽取不影响 DX12)。

- [ ] **Step 4: 构建应用 + RT 画面回归**

```powershell
cmake --build F:\AdvancedVulkanRendering --config Debug --target AdvancedVulkanRendering
.\Bin\AdvancedVulkanRendering.exe
```

ImGui 勾选 "Ray Tracing",等 accumulation 稳定后截图,与 Step 1 的 `vlm_rt_baseline.png` 对比。用 RenderDoc 抓一帧(MCP 工具 `get_draw_calls` 过滤 `TraceRays`)确认 trace 调用与输出图正常。

Expected: 画面与基线一致(同机同场景同累积帧数下肉眼无差;像素级允许 Monte Carlo 噪声差异——种子路径未变时应几乎逐像素一致)。

- [ ] **Step 5: Commit**

```bash
git add shaders/rt_path_common.hlsl shaders/rt_lighting.hlsl shaders/rt_lighting.lib.spv
git commit -m "refactor: extract shared RT hit/material/BRDF/NEE into rt_path_common.hlsl"
```

---

### Task 5: vlm_bake.hlsl 烘焙 shader + 编译

**Files:**
- Create: `shaders/vlm_bake.hlsl`
- Modify: `shaders/compile_shaders.bat`

**Interfaces:**
- Consumes: `rt_path_common.hlsl`(Task 4 的全部共享符号)、push const 布局(与 Task 6 的 C++ `VlmBakePC` 逐字节一致,112 B)。
- Produces(Task 6 C++ 侧依赖):
  - shader 入口:`VlmProbeRayGen`(raygeneration)、`VlmMissPrimary`(miss,仅置 `p.hit=false`)。
  - 新绑定(set1):`envTex` Texture2D<float4> (14)、`envSampler` SamplerState (15)、`shAccum` RWStructuredBuffer<float> (16,probeCount×27)、`errorFlags` RWStructuredBuffer<uint> (17,1 个 uint)。
  - push const 标志位:`bit0=skyOnly, bit1=constEnv, bit2=sunIsEnvironment`。
  - SBT 约定与 rt 相同(miss[0]=VlmMissPrimary、miss[1]=MissShadow;hit 每 geometry 2 条)。

- [ ] **Step 1: 写 vlm_bake.hlsl**

```hlsl
// vlm_bake.hlsl — VLM 探针离线烘焙(规格 §6/§7)。
// 每 lane 一个 probe:批量低差异方向 × 迭代路径追踪,首段方向做 SH9 投影,
// FP32 累加进 shAccum[probe*27 .. +27)(lane 独占 probe,无需原子)。
// 与相机 PT 的差异(规格 §2/§6):
//   - 首段为均匀球面低差异序列(pdf=1/(4π)),探针无接收法线,不用余弦半球;
//   - miss 采样原始线性 HDR equirect(非程序化天空);
//   - 局部灯 NEE 在所有命中执行(相机 PT 仅 bounce==0);
//   - 不继承 FIREFLY_CLAMP;NaN/Inf 置 errorFlags 并丢弃样本,CPU 判整批失败;
//   - 无累积平均:GPU 只存 Σ,均值由 CPU finalize。
//
// Keep VlmBakePushConsts in sync with Src/VlmBaker.cpp VlmBakePC struct (112 bytes).
//
// Compile with(与 rt_lighting 同参数):
//   dxc -spirv -T lib_6_3 vlm_bake.hlsl -fspv-target-env=vulkan1.2
//        -fspv-extension=SPV_KHR_ray_tracing -fspv-extension=SPV_KHR_non_semantic_info
//        -fspv-extension=SPV_EXT_descriptor_indexing -Fo vlm_bake.lib.spv

#include "rt_path_common.hlsl"

// --- VLM 自有绑定(set1 追加)---
[[vk::binding(14,1)]] Texture2D<float4>       envTex;
[[vk::binding(15,1)]] SamplerState            envSampler;
[[vk::binding(16,1)]] RWStructuredBuffer<float> shAccum;    // probeCount × 27
[[vk::binding(17,1)]] RWStructuredBuffer<uint>  errorFlags; // 1 uint

// --- Push constants(112 B,与 C++ VlmBakePC 逐字段一致)---
struct VlmBakePushConsts {
    uint  probeCount; uint  samplesThisBatch; uint  batchSeed; uint  maxBounces; // 0..15
    uint  pointLightCount; uint spotLightCount; uint flags; uint cellsX;         // 16..31
    uint  cellsY; uint cellsZ; float sunConeRadius; float sunScale;              // 32..47
    float envScale; float localLightScale; float pad0; float pad1;               // 48..63
    float3 constEnvRGB; float pad2;   // 64..79
    float3 bmin; float pad3;          // 80..95
    float3 step; float pad4;          // 96..111
};
[[vk::push_constant]] VlmBakePushConsts pc;

// flags 位(与 C++ 一致)
#define VLM_BAKE_SKY_ONLY          1u
#define VLM_BAKE_CONST_ENV         2u
#define VLM_BAKE_SUN_IS_ENVIRONMENT 4u

// 方向→equirect uv:必须与 Src/SphericalHarmonics.h 注释及 ibl.hlsl:14-19 逐字一致
// (-d.x 方位角镜像是对 Apple 烘焙 KTX 的实证校准)。
float2 DirectionToEquirectUV(float3 d)
{
    float phi = atan2(-d.x, d.z);
    float theta = acos(clamp(d.y, -1.0, 1.0));
    return float2(phi / (2.0 * PI) + 0.5, theta / PI);
}

float3 envRadiance(float3 dir) {
    float3 e = (pc.flags & VLM_BAKE_CONST_ENV)
        ? pc.constEnvRGB
        : envTex.SampleLevel(envSampler, DirectionToEquirectUV(dir), 0).rgb;
    return e * pc.envScale;
}

// Fibonacci 球面 + Cranley-Patterson 旋转(按 probeId/batchId 扰乱,规格 §6)。
// 与 Tests/VlmShTests.cpp 的 FibonacciDir 同基序列;均匀球面 pdf 恒 1/(4π)。
float3 sampleSphereDir(uint s, uint n, inout uint rng) {
    float r1 = rngF(rng);
    float r2 = rngF(rng);
    float u = (float(s) + r1) / float(n);
    float z = 1.0f - 2.0f * u;
    float phi = TWO_PI * frac(float(s) * 0.61803398875f + r2);
    float r = sqrt(max(0.0f, 1.0f - z * z));
    return float3(r * cos(phi), z, r * sin(phi));
}

// 物理 SH 基(kY 已折入),与 EvalSh9Basis(SphericalHarmonics.h)逐字一致。
void evalSh9Basis(float3 d, out float b[9]) {
    b[0] = 0.282095f;
    b[1] = 0.488603f * d.y;
    b[2] = 0.488603f * d.z;
    b[3] = 0.488603f * d.x;
    b[4] = 1.092548f * d.y * d.x;
    b[5] = 1.092548f * d.y * d.z;
    b[6] = 0.315392f * (3.0f * d.z * d.z - 1.0f);
    b[7] = 1.092548f * d.z * d.x;
    b[8] = 0.546274f * (d.x * d.x - d.y * d.y);
}

float3 probeWorldPos(uint probeIdx) {
    uint PX = pc.cellsX + 1u;
    uint PY = pc.cellsY + 1u;
    uint x = probeIdx % PX;
    uint y = (probeIdx / PX) % PY;
    uint z = probeIdx / (PX * PY);
    return pc.bmin + pc.step * float3(float(x), float(y), float(z));
}

[shader("raygeneration")]
void VlmProbeRayGen() {
    uint probeIdx = DispatchRaysIndex().x;
    if (probeIdx >= pc.probeCount) return;

    float3 probePos = probeWorldPos(probeIdx);
    uint rng = (probeIdx * 2654435761u) ^ (pc.batchSeed * 805459861u);
    pcgNext(rng); pcgNext(rng); // warm up

    const float invPdf = 4.0f * PI; // 1/pdf,pdf=1/(4π)
    float3 acc[9];
    [unroll] for (int j = 0; j < 9; ++j) acc[j] = (float3)0;

    for (uint s = 0; s < pc.samplesThisBatch; ++s) {
        float3 dir0 = sampleSphereDir(s, pc.samplesThisBatch, rng);
        float3 Li;

        if (pc.flags & VLM_BAKE_SKY_ONLY) {
            // 无几何模式(常量/方向验收):不追射线,直接采样环境。
            Li = envRadiance(dir0);
        } else {
            Li = (float3)0;
            float3 throughput = float3(1.0f, 1.0f, 1.0f);

            RayDesc ray;
            ray.Origin    = probePos;
            ray.Direction = dir0;
            ray.TMin      = 1e-3f;
            ray.TMax      = 1e30f;

            for (uint bounce = 0; bounce < max(1u, pc.maxBounces); ++bounce) {
                PrimaryPayload p;
                p.hit = false; p.wsPos = 0; p.normal = 0; p.albedo = 0;
                p.F0 = 0.04f; p.roughness = 1.0f; p.emissive = 0; p.alpha = 1; p.hitT = 0;

                TraceRay(tlas, RAY_FLAG_CULL_BACK_FACING_TRIANGLES, 0xFF, 0, 2, 0, ray, p);

                if (!p.hit) {
                    Li += throughput * envRadiance(ray.Direction);
                    break;
                }

                // 自发光出射(规格 §3:命中发光表面记录其出射 emission)
                Li += throughput * p.emissive * frameConstants.emissiveScale;

                float3 N = normalize(p.normal);
                float3 V = -ray.Direction;
                if (dot(N, V) < 0.0f) N = -N;
                float3 wsP = p.wsPos + N * 2e-3f;

                // 解析太阳 NEE(sunIsEnvironment 时跳过,避免与环境太阳盘双重计能,规格 §3)
                if ((pc.flags & VLM_BAKE_SUN_IS_ENVIRONMENT) == 0u) {
                    float3 sdir = sampleSunDir(rng, pc.sunConeRadius);
                    if (dot(N, sdir) > 0.0f &&
                        traceShadowRay(wsP, sdir, 1e30f) > 0.0f) {
                        Li += throughput *
                              evalBRDF(N, V, sdir, p.albedo, p.F0, p.roughness) *
                              (frameConstants.sunColor * PI) * pc.sunScale;
                    }
                }

                // 局部灯 NEE:所有命中都执行(规格 §6;相机 PT 仅 bounce==0)
                Li += throughput *
                      evalLocalLightsNEE(wsP, N, V, p.albedo, p.F0, p.roughness,
                                         pc.pointLightCount, pc.spotLightCount) *
                      pc.localLightScale;

                // BSDF 采样下一跳(完整混合 pdf,与返回权重一致)
                float3 wi;
                float3 weight = sampleBRDF(N, V, p.albedo, p.F0, p.roughness, rng, wi);
                if (dot(wi, N) <= 0.0f || !any(weight > 0.0f)) break;

                throughput *= weight;

                // Russian Roulette(第三次表面散射后,与相机 PT 同阈值)
                if (bounce >= 2) {
                    float q = clamp(max(throughput.r, max(throughput.g, throughput.b)), 0.01f, 0.95f);
                    if (rngF(rng) > q) break;
                    throughput /= q;
                }

                ray.Origin    = wsP;
                ray.Direction = wi;
            }
        }

        // NaN/Inf:置错误标志并丢弃样本;CPU 判整批失败(规格 §6,不当零样本计入)
        if (!(isfinite(Li.x) && isfinite(Li.y) && isfinite(Li.z))) {
            InterlockedOr(errorFlags[0], 1u);
            continue;
        }

        // 首段方向 SH 投影:acc_j += Li · Y_j(dir0) / pdf
        float basis[9];
        evalSh9Basis(dir0, basis);
        [unroll] for (int j = 0; j < 9; ++j) acc[j] += Li * (basis[j] * invPdf);
    }

    // lane 独占 probe,跨 dispatch 由 host 端 barrier 保证顺序,直接 RMW。
    uint base = probeIdx * 27u;
    [unroll] for (int j = 0; j < 9; ++j) {
        shAccum[base + j * 3 + 0] += acc[j].x;
        shAccum[base + j * 3 + 1] += acc[j].y;
        shAccum[base + j * 3 + 2] += acc[j].z;
    }
}

[shader("miss")]
void VlmMissPrimary(inout PrimaryPayload p) {
    p.hit = false;
}
```

- [ ] **Step 2: compile_shaders.bat 追加 + 编译**

在 `compile_shaders.bat` 的 `rt_lighting.hlsl` 行(75 行)之后追加同构行:

```bat
D:\VulkanSDK\1.3.296.0\Bin\dxc.exe -spirv -T lib_6_3 vlm_bake.hlsl -fspv-target-env=vulkan1.2 -fspv-extension=SPV_KHR_ray_tracing -fspv-extension=SPV_KHR_physical_storage_buffer -fspv-extension=SPV_KHR_non_semantic_info -fspv-extension=SPV_EXT_descriptor_indexing -Fo vlm_bake.lib.spv
```

```powershell
cmd /c "cd /d F:\AdvancedVulkanRendering\shaders && .\compile_shaders.bat"
```

Expected: 零错误,生成 `shaders/vlm_bake.lib.spv`。

- [ ] **Step 3: Commit**

```bash
git add shaders/vlm_bake.hlsl shaders/compile_shaders.bat shaders/vlm_bake.lib.spv
git commit -m "feat: VLM probe bake shader (uniform-sphere LD sequence, SH9 projection, NEE at all hits)"
```

### Task 6: VlmBaker C++ + 命令行接线 + 常量天空验收

**Files:**
- Create: `Src/Include/VlmCommandLine.h`、`Src/Include/VlmBaker.h`、`Src/VlmBaker.cpp`
- Modify: `Src/Window.cpp`(参数解析 + 传入 + 退出检查)、`Src/Include/GpuScene.h`(ctor 参数 + 成员 + 方法声明)、`Src/GpuScene.cpp`(ctor 存参、`recordCommandBuffer` 挂钩、`RunVlmBake`、`ComputeVlmSceneHash`、`HdrEnvPath`)、`Src/Include/Raytracing.h`(`GetTlas()`)、`Src/CMakeLists.txt`(SOURCES 追加)

**Interfaces:**
- Consumes: Task 1 (`VlmShFinalize/VlmShPackFp16`)、Task 2 (`VlmUniformLayout`)、Task 3 (`VlmAssetData/VlmSaveAsset/VlmFnv1a64`)、Task 5 (`vlm_bake.lib.spv` 入口与绑定)。
- Produces(Task 7 日志格式 / Task 8 复用):
  - `struct VlmCommandLine`(字段见下)+ `bool ParseVlmCommandLine(int argc, char** argv, VlmCommandLine& out);`
  - `class VlmBaker { public: VlmBaker(VulkanDevice&, GpuScene&); void Init(VkImageView equirectView); bool Bake(const Settings&, VlmAssetData& out); };`(`Settings` 内嵌 `VlmUniformLayout layout`、采样/弹跳/模式字段)
  - 日志行(spdlog::info,Task 7 脚本按前缀匹配):
    - `vlm bake: probes=<N> samples=<N> batches=<N> batchSamples=<N>`
    - `vlm validate const-sky: maxRelErr=<f> meanRelErr=<f> (threshold 0.010000)`(仅 constEnv)
    - `vlm validate direction: c0RelErr=<f> weightedRelErr=<f> (thresholds 0.010000/0.050000)`(仅 skyOnly 且非 constEnv)
    - `vlm probe <i> E(+Y)=<r>,<g>,<b>`(前 min(8, probes) 个 probe,固定 6 位小数)
    - 成功 `vlm bake: wrote <path> (<N> probes, <B> bytes)`;失败 `vlm bake: FAILED (<reason>)`

- [ ] **Step 1: VlmCommandLine + Window.cpp 接线**

`Src/Include/VlmCommandLine.h`:

```cpp
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

// VLM 命令行(Window.cpp main 解析,传入 GpuScene ctor)。
// 体积语法:
//   --vlm-bake-volume camera,sx,sy,sz      以 scene.scene 的 camera_position 为中心、尺寸 sx×sy×sz
//   --vlm-bake-volume x0,y0,z0,x1,y1,z1    显式 AABB
struct VlmCommandLine {
  bool bakeRequested = false;        // --vlm-bake <path>
  std::string bakeOutPath;
  std::string bakeVolume;            // --vlm-bake-volume <spec>(必需)
  float bakeSpacing = 1.0f;          // --vlm-bake-spacing <m>
  uint32_t bakeSamples = 2048;       // --vlm-bake-samples <N>
  uint32_t bakeBatchSamples = 32;    // --vlm-bake-batch-samples <N>(单次 dispatch 每 probe 样本数)
  uint32_t bakeMaxBounces = 6;       // --vlm-bake-max-bounces <N>
  bool bakeSkyOnly = false;          // --vlm-bake-sky-only(无几何,纯环境投影)
  bool bakeConstEnv = false;         // --vlm-bake-const-env r,g,b
  float constEnvRGB[3] = {1, 1, 1};
  bool sunIsEnvironment = false;     // --vlm-sun-mode environment(默认 analytic)
  float sunScale = 1.0f;             // --vlm-sun-scale
  float envScale = 1.0f;             // --vlm-env-scale
  float localLightScale = 1.0f;      // --vlm-local-light-scale
  bool exitAfterBake = false;        // --vlm-exit-after-bake
  std::string loadPath;              // --vlm <path>(启动时加载资产)
};

// 只负责识别 VLM 参数并前移消费索引;缺值的 flag 返回 ""(调用方判空)。
inline bool ParseVlmCommandLine(int argc, char** argv, VlmCommandLine& out) {
  auto parse3f = [](const char* s, float v[3]) {
    return std::sscanf(s, "%f,%f,%f", &v[0], &v[1], &v[2]) == 3;
  };
  for (int i = 1; i < argc; ++i) {
    auto next = [&](const char*) -> const char* {
      return (i + 1 < argc) ? argv[++i] : "";
    };
    if (!std::strcmp(argv[i], "--vlm-bake")) { out.bakeRequested = true; out.bakeOutPath = next("--vlm-bake"); }
    else if (!std::strcmp(argv[i], "--vlm")) out.loadPath = next("--vlm");
    else if (!std::strcmp(argv[i], "--vlm-bake-volume")) out.bakeVolume = next("--vlm-bake-volume");
    else if (!std::strcmp(argv[i], "--vlm-bake-spacing")) out.bakeSpacing = (float)atof(next("--vlm-bake-spacing"));
    else if (!std::strcmp(argv[i], "--vlm-bake-samples")) out.bakeSamples = (uint32_t)atoi(next("--vlm-bake-samples"));
    else if (!std::strcmp(argv[i], "--vlm-bake-batch-samples")) out.bakeBatchSamples = (uint32_t)atoi(next("--vlm-bake-batch-samples"));
    else if (!std::strcmp(argv[i], "--vlm-bake-max-bounces")) out.bakeMaxBounces = (uint32_t)atoi(next("--vlm-bake-max-bounces"));
    else if (!std::strcmp(argv[i], "--vlm-bake-sky-only")) out.bakeSkyOnly = true;
    else if (!std::strcmp(argv[i], "--vlm-bake-const-env")) { out.bakeConstEnv = true; parse3f(next("--vlm-bake-const-env"), out.constEnvRGB); }
    else if (!std::strcmp(argv[i], "--vlm-sun-mode")) { const char* m = next("--vlm-sun-mode"); out.sunIsEnvironment = !std::strcmp(m, "environment"); }
    else if (!std::strcmp(argv[i], "--vlm-sun-scale")) out.sunScale = (float)atof(next("--vlm-sun-scale"));
    else if (!std::strcmp(argv[i], "--vlm-env-scale")) out.envScale = (float)atof(next("--vlm-env-scale"));
    else if (!std::strcmp(argv[i], "--vlm-local-light-scale")) out.localLightScale = (float)atof(next("--vlm-local-light-scale"));
    else if (!std::strcmp(argv[i], "--vlm-exit-after-bake")) out.exitAfterBake = true;
  }
  return true;
}
```

`Src/Window.cpp`:
- main() 的参数循环(30-43 行)之后加:
```cpp
  VlmCommandLine vlmCmd;
  ParseVlmCommandLine(nargs, args, vlmCmd);
```
  (`#include "VlmCommandLine.h"`;`--dx12` 循环保持原样。)
- Vulkan 构造点(167 行)`GpuScene gpuScene(currentPath, vk);` 改为:
```cpp
  GpuScene gpuScene(currentPath, vk, &vlmCmd);
```
- 主循环中 `gpuScene.Draw();` 调用之后加:
```cpp
      if (gpuScene.QuitRequested()) quit = true;
```

- [ ] **Step 2: GpuScene.h/.cpp 接线**

`Src/Include/Raytracing.h` 的 public 区(`IsBuilt()` 之后)加:

```cpp
  // VLM 烘焙共享静态 TLAS(规格 §2:提取共享静态追踪资源视图)。
  VkAccelerationStructureKHR GetTlas() const { return _tlas; }
```

`Src/Include/GpuScene.h`:
- 顶部 `#include "VlmCommandLine.h"`(成员按值存储需要完整类型,不能用前置声明)。
- ctor 声明改为 `GpuScene(std::filesystem::path &root, const VulkanDevice &device, const VlmCommandLine *vlmCmd = nullptr);`
- 成员(public 区,与 `_pointLights` 同区即可):
```cpp
  VlmCommandLine _vlmCmdStorage{};       // 仅当 ctor 传入时有效
  bool _vlmCmdValid = false;
  bool _vlmBakeDone = false;
  bool _vlmQuitRequested = false;
  bool QuitRequested() const { return _vlmQuitRequested; }
```
- 方法声明(private 区):`void RunVlmBake();`、`uint64_t ComputeVlmSceneHash() const;`

`Src/GpuScene.cpp`:
- ctor 签名同步,开头加:`if (vlmCmd) { _vlmCmdStorage = *vlmCmd; _vlmCmdValid = true; }`
- `recordCommandBuffer` 的 RT 惰性初始化块(3746-3761,`CreatePipelineAndSBT();` 与 `}`)之后加:
```cpp
    if (_vlmCmdValid && _vlmCmdStorage.bakeRequested && !_vlmBakeDone && _raytracing) {
      _vlmBakeDone = true; // 失败也只试一次,避免每帧重烘
      RunVlmBake();
      if (_vlmCmdStorage.exitAfterBake) _vlmQuitRequested = true;
    }
```
- `initIBL()` 的 HDR 路径硬编码(6496 行)提取为文件作用域 helper 并复用:
```cpp
namespace {
std::filesystem::path HdrEnvPath(const std::filesystem::path& root) {
  return root / "textures" / "san_giuseppe_bridge_2k.hdr";
}
}
```
  (`IBLGenerator.cpp:971-972` 的另一处硬编码本次不动,加注释 `// 与 GpuScene.cpp HdrEnvPath 同源,换资产需同步`。)
- `ComputeVlmSceneHash()`:
```cpp
uint64_t GpuScene::ComputeVlmSceneHash() const {
  uint64_t h = kVlmFnv1aBasis;
  const std::string sceneText = sceneFile.dump(); // 覆盖 sun/point_lights/spot_lights
  h = VlmFnv1a64(sceneText.data(), sceneText.size(), h);
  std::error_code ec;
  const auto meshSize = std::filesystem::file_size(_rootPath / "bistro.dxt.bin", ec);
  h = VlmFnv1a64(&meshSize, sizeof(meshSize), h);
  const uint32_t counts[3] = {
      applMesh ? (uint32_t)applMesh->_opaqueChunkCount : 0,
      applMesh ? (uint32_t)applMesh->_alphaMaskedChunkCount : 0,
      applMesh ? (uint32_t)applMesh->_vertexCount : 0,
  };
  h = VlmFnv1a64(counts, sizeof(counts), h);
  const std::string hdrPath = HdrEnvPath(_rootPath).generic_string();
  std::ifstream hdr(hdrPath, std::ios::binary);
  if (hdr.good()) {
    const std::vector<char> bytes((std::istreambuf_iterator<char>(hdr)), {});
    h = VlmFnv1a64(bytes.data(), bytes.size(), h);
  }
  return h;
}
```
  (网格内容为文件大小 + chunk/顶点计数,完整逐字节哈希留待阶段 2;灯光另算 `lightHash`:对 `sceneFile["point_lights"].dump() + sceneFile["spot_lights"].dump() + sceneFile["sun_direction"].dump()` 做 FNV——`sceneHash` 已含,分开存便于阶段 2 细粒度失效。)
- `RunVlmBake()`:
```cpp
namespace {
// "camera,sx,sy,sz" 或 "x0,y0,z0,x1,y1,z1" → bmin/bmax;失败返回 false。
bool ParseVlmVolumeSpec(const std::string& spec, const nlohmann::json& sceneFile,
                        float outBmin[3], float outBmax[3]) {
  if (spec.rfind("camera,", 0) == 0) {
    float size[3];
    if (std::sscanf(spec.c_str() + 7, "%f,%f,%f", &size[0], &size[1], &size[2]) != 3) return false;
    const auto& cam = sceneFile["camera_position"];
    const float c[3] = {cam[0].get<float>(), cam[1].get<float>(), cam[2].get<float>()};
    for (int i = 0; i < 3; ++i) {
      outBmin[i] = c[i] - size[i] * 0.5f;
      outBmax[i] = c[i] + size[i] * 0.5f;
    }
    return true;
  }
  float v[6];
  if (std::sscanf(spec.c_str(), "%f,%f,%f,%f,%f,%f",
                  &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6) return false;
  for (int i = 0; i < 3; ++i) {
    outBmin[i] = std::fmin(v[i], v[i + 3]);
    outBmax[i] = std::fmax(v[i], v[i + 3]);
  }
  return true;
}
}

void GpuScene::RunVlmBake() {
  // 已知限制(规格 §3):首版不支持同一发光体双重计能——bistro 中若发光材质与
  // 解析点/聚光共位,其能量会被 emission 命中与 NEE 各计一次。阶段 1 接受该偏差
  // (验收场景以太阳/环境为主);阶段 2 需在 bake 设置中显式指定唯一照明来源。
  // 另一限制:透明材质策略为引擎既有 alpha-cutout(已写入 settingsHash),
  // 不把玻璃当正确透射(规格 §6)。
  const VlmCommandLine& cmd = _vlmCmdStorage;
  if (!device.isRayTracingSupported() || !_raytracing || _raytracing->GetTlas() == VK_NULL_HANDLE) {
    spdlog::error("vlm bake: FAILED (ray tracing not available)");
    return;
  }
  float bmin[3], bmax[3];
  if (cmd.bakeVolume.empty() ||
      !ParseVlmVolumeSpec(cmd.bakeVolume, sceneFile, bmin, bmax)) {
    spdlog::error("vlm bake: FAILED (--vlm-bake-volume required: camera,sx,sy,sz | x0,y0,z0,x1,y1,z1)");
    return;
  }
  if (!cmd.bakeConstEnv && _equirectView == VK_NULL_HANDLE) {
    spdlog::error("vlm bake: FAILED (equirect env missing; use --vlm-bake-const-env or fix hdr path)");
    return;
  }

  VlmBaker::Settings s;
  s.layout = VlmMakeUniformLayout(bmin, bmax, cmd.bakeSpacing);
  s.samplesPerProbe = cmd.bakeSamples;
  s.batchSamples = cmd.bakeBatchSamples;
  s.maxBounces = cmd.bakeMaxBounces;
  s.skyOnly = cmd.bakeSkyOnly;
  s.constEnv = cmd.bakeConstEnv;
  s.constEnvRGB[0] = cmd.constEnvRGB[0];
  s.constEnvRGB[1] = cmd.constEnvRGB[1];
  s.constEnvRGB[2] = cmd.constEnvRGB[2];
  s.sunIsEnvironment = cmd.sunIsEnvironment;
  s.sunScale = cmd.sunScale;
  s.envScale = cmd.envScale;
  s.localLightScale = cmd.localLightScale;
  s.hdrPathForReference = HdrEnvPath(_rootPath);

  VlmAssetData asset;
  asset.sceneHash = ComputeVlmSceneHash();
  asset.envHash = asset.sceneHash; // env 字节已折入 sceneHash;分开字段留待阶段 2 细粒度化
  {
    const std::string lt = sceneFile["point_lights"].dump() +
                           sceneFile["spot_lights"].dump() +
                           sceneFile["sun_direction"].dump();
    asset.lightHash = VlmFnv1a64(lt.data(), lt.size(), kVlmFnv1aBasis);
  }
  for (int i = 0; i < 3; ++i) {
    asset.bmin[i] = s.layout.bmin[i];
    asset.step[i] = s.layout.step[i];
    asset.cells[i] = s.layout.cells[i];
  }
  asset.bandWidth = std::fmin(asset.step[0], std::fmin(asset.step[1], asset.step[2])); // 一层粗 cell(规格 §8.3)
  asset.unitScale = 1.0f;
  asset.samplesPerProbe = s.samplesPerProbe;
  asset.integratorVersion = kVlmIntegratorVersion;
  {
    char buf[256];
    std::snprintf(buf, sizeof(buf), "n=%u;b=%u;sky=%d;ce=%d;sem=%d;ss=%.3f;es=%.3f;lls=%.3f;sp=%.4f;v=%s;alpha=cutout",
                  s.samplesPerProbe, s.maxBounces, (int)s.skyOnly, (int)s.constEnv,
                  (int)s.sunIsEnvironment, s.sunScale, s.envScale, s.localLightScale,
                  cmd.bakeSpacing, cmd.bakeVolume.c_str());
    asset.settingsHash = VlmFnv1a64(buf, std::strlen(buf), kVlmFnv1aBasis);
  }
  const uint64_t probes = s.layout.ProbeCount();
  asset.validity.assign((size_t)probes, 1);
  asset.shFp16.resize((size_t)probes * 28);

  spdlog::info("vlm bake: volume=({:.2f},{:.2f},{:.2f})..({:.2f},{:.2f},{:.2f}) cells={}x{}x{}",
               bmin[0], bmin[1], bmin[2], bmax[0], bmax[1], bmax[2],
               asset.cells[0], asset.cells[1], asset.cells[2]);

  VlmBaker baker(const_cast<VulkanDevice&>(device), *this);
  baker.Init(_equirectView);
  if (!baker.Bake(s, asset)) return; // 失败原因已由其内部日志输出,不发布资产

  if (!VlmSaveAsset(cmd.bakeOutPath, asset)) {
    spdlog::error("vlm bake: FAILED (write {})", cmd.bakeOutPath);
    return;
  }
  const uint64_t bytes = 132ull + probes * 57ull;
  spdlog::info("vlm bake: wrote {} ({} probes, {} bytes)", cmd.bakeOutPath, probes, bytes);
}
```

- [ ] **Step 3: VlmBaker.h**

`Src/Include/VlmBaker.h`:

```cpp
#pragma once

#include "VlmAsset.h"
#include "VlmLayout.h"
#include "VulkanSetup.h"
#include <filesystem>
#include <vector>

class GpuScene;

// VLM GPU 烘焙器(阶段 1:均匀网格,首帧一次性同步执行)。
// 复用 RayTracing 的静态 TLAS 与 GpuScene 的几何/材质/灯光/纹理 buffer;
// 自有 pipeline/SBT/descriptor,不复制相机渲染流程(规格 §2)。
class VlmBaker {
public:
  struct Settings {
    VlmUniformLayout layout{};
    uint32_t samplesPerProbe = 2048;
    uint32_t batchSamples = 32;
    uint32_t maxBounces = 6;
    bool skyOnly = false;
    bool constEnv = false;
    float constEnvRGB[3] = {1, 1, 1};
    bool sunIsEnvironment = false;
    float sunScale = 1.0f;
    float envScale = 1.0f;
    float localLightScale = 1.0f;
    std::filesystem::path hdrPathForReference; // 方向约定 CPU 参考用
  };

  VlmBaker(VulkanDevice& device, GpuScene& scene);
  VlmBaker() = delete;
  ~VlmBaker();

  // equirectView 可为 VK_NULL_HANDLE(仅 constEnv 模式允许;此时内部建 1×1 占位图)。
  void Init(VkImageView equirectView);

  // 完整烘焙:分批 dispatch → readback → finalize → FP16 打包填入 out.shFp16。
  // 返回 false = 失败(NaN/Inf、FP16 溢出、env 缺失),调用方不得发布资产。
  bool Bake(const Settings& s, VlmAssetData& out);

private:
  VulkanDevice& _device;
  GpuScene& _scene;

  void ensureHostBuffer(VkDeviceSize bytes, VkBuffer& buffer, VkDeviceMemory& memory);
  void writeAccumDescriptors(); // binding 16/17(ensureHostBuffer 重建后必须重绑)

  VkDescriptorSetLayout _setLayout = VK_NULL_HANDLE;
  VkDescriptorPool _pool = VK_NULL_HANDLE;
  VkDescriptorSet _set = VK_NULL_HANDLE;
  VkPipelineLayout _pipelineLayout = VK_NULL_HANDLE;
  VkPipeline _pipeline = VK_NULL_HANDLE;
  VkShaderModule _shaderModule = VK_NULL_HANDLE;
  VkBuffer _sbtBuffer = VK_NULL_HANDLE;
  VkDeviceMemory _sbtMemory = VK_NULL_HANDLE;
  VkStridedDeviceAddressRegionKHR _rgenRegion{};
  VkStridedDeviceAddressRegionKHR _missRegion{};
  VkStridedDeviceAddressRegionKHR _hitRegion{};
  VkStridedDeviceAddressRegionKHR _callRegion{};

  VkBuffer _shAccum = VK_NULL_HANDLE;
  VkDeviceMemory _shAccumMemory = VK_NULL_HANDLE;
  VkBuffer _errorFlags = VK_NULL_HANDLE;
  VkDeviceMemory _errorFlagsMemory = VK_NULL_HANDLE;
  VkSampler _envSampler = VK_NULL_HANDLE;
  VkImage _envFallback = VK_NULL_HANDLE;
  VkDeviceMemory _envFallbackMemory = VK_NULL_HANDLE;
  VkImageView _envFallbackView = VK_NULL_HANDLE;

  PFN_vkGetBufferDeviceAddressKHR pfnGetBufferDeviceAddress = nullptr;
  PFN_vkCreateRayTracingPipelinesKHR pfnCreateRayTracingPipelines = nullptr;
  PFN_vkGetRayTracingShaderGroupHandlesKHR pfnGetRayTracingShaderGroupHandles = nullptr;
  PFN_vkCmdTraceRaysKHR pfnCmdTraceRays = nullptr;
};
```

- [ ] **Step 4: VlmBaker.cpp — Init(pipeline/SBT/descriptor)**

`Src/VlmBaker.cpp` 结构(完整实现,样板段按 `Src/Raytracing.cpp:620-798`(descriptor)与 `:356-543`(pipeline/SBT)同模式):

```cpp
#include "VlmBaker.h"

#include "GpuScene.h"
#include "Light.h"
#include "Raytracing.h"
#include "SphericalHarmonics.h"
#include "VlmSh.h"

#include <spdlog/spdlog.h>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>

// 与 vlm_bake.hlsl 的 VlmBakePushConsts 逐字段一致(112 B)。
struct VlmBakePC {
  uint32_t probeCount, samplesThisBatch, batchSeed, maxBounces;      // 0..15
  uint32_t pointLightCount, spotLightCount, flags, cellsX;           // 16..31
  uint32_t cellsY, cellsZ; float sunConeRadius, sunScale;            // 32..47
  float envScale, localLightScale, pad0, pad1;                       // 48..63
  float constEnvRGB[3]; float pad2;                                  // 64..79
  float bmin[3]; float pad3;                                         // 80..95
  float step[3]; float pad4;                                         // 96..111
};
static_assert(sizeof(VlmBakePC) == 112, "VlmBakePC must stay 112 bytes (push constant)");
static_assert(offsetof(VlmBakePC, flags) == 24);
static_assert(offsetof(VlmBakePC, sunConeRadius) == 40);
static_assert(offsetof(VlmBakePC, constEnvRGB) == 64);
static_assert(offsetof(VlmBakePC, bmin) == 80);
static_assert(offsetof(VlmBakePC, step) == 96);

namespace {
constexpr uint32_t kFlagSkyOnly = 1u, kFlagConstEnv = 2u, kFlagSunIsEnvironment = 4u;

std::vector<char> readSpv(const std::filesystem::path& p) {
  std::ifstream f(p, std::ios::binary | std::ios::ate);
  if (!f.good()) return {};
  const auto size = f.tellg();
  std::vector<char> bytes((size_t)size);
  f.seekg(0);
  f.read(bytes.data(), size);
  return bytes;
}
} // namespace

// host-visible coherent STORAGE buffer;容量不足时销毁重建(模式同
// IBLGenerator.cpp:101 的 createHostBuffer,usage 固定 STORAGE_BUFFER)。
void VlmBaker::ensureHostBuffer(VkDeviceSize bytes, VkBuffer& buffer, VkDeviceMemory& memory) {
  if (buffer != VK_NULL_HANDLE) {
    VkDevice dev = _device.getLogicalDevice();
    vkDestroyBuffer(dev, buffer, nullptr);
    vkFreeMemory(dev, memory, nullptr);
    buffer = VK_NULL_HANDLE;
    memory = VK_NULL_HANDLE;
  }
  VkDevice dev = _device.getLogicalDevice();
  VkBufferCreateInfo bi{};
  bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  bi.size = bytes;
  bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
  bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  if (vkCreateBuffer(dev, &bi, nullptr, &buffer) != VK_SUCCESS)
    throw std::runtime_error("VlmBaker: failed to create buffer");
  VkMemoryRequirements mr;
  vkGetBufferMemoryRequirements(dev, buffer, &mr);
  VkMemoryAllocateInfo ai{};
  ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  ai.allocationSize = mr.size;
  ai.memoryTypeIndex = _device.findMemoryType(
      mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  if (vkAllocateMemory(dev, &ai, nullptr, &memory) != VK_SUCCESS)
    throw std::runtime_error("VlmBaker: failed to allocate buffer memory");
  vkBindBufferMemory(dev, buffer, memory, 0);
}

// binding 16/17 重绑(Init 末尾先以 16 B 占位 buffer 调用一次,Bake 重建后再次调用)。
void VlmBaker::writeAccumDescriptors() {
  VkDevice dev = _device.getLogicalDevice();
  VkDescriptorBufferInfo shInfo{_shAccum, 0, VK_WHOLE_SIZE};
  VkDescriptorBufferInfo errInfo{_errorFlags, 0, VK_WHOLE_SIZE};
  VkWriteDescriptorSet w[2]{};
  for (int i = 0; i < 2; ++i) {
    w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w[i].dstSet = _set;
    w[i].descriptorCount = 1;
    w[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  }
  w[0].dstBinding = 16;
  w[0].pBufferInfo = &shInfo;
  w[1].dstBinding = 17;
  w[1].pBufferInfo = &errInfo;
  vkUpdateDescriptorSets(dev, 2, w, 0, nullptr);
}
```

`Init()` 要点(按以下精确规格实现,Vulkan 样板代码风格对齐 `Raytracing::CreatePipelineAndSBT`/`CreateOutputImagesAndDescriptorSet`):

1. 加载 4 个函数指针(`vkGetDeviceProcAddr`)。
2. set1 layout,15 个 binding,stage 全为 `VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR | VK_SHADER_STAGE_ANY_HIT_BIT_KHR | VK_SHADER_STAGE_MISS_BIT_KHR`:

| binding | type | 内容 |
|---|---|---|
| 0 | ACCELERATION_STRUCTURE_KHR | `_scene._raytracing->GetTlas()` |
| 2,3,4 | STORAGE_BUFFER | `applVertexBuffer/applNormalBuffer/applTangentBuffer` |
| 5 | STORAGE_BUFFER | `applUVBuffer` |
| 6 | STORAGE_BUFFER | `applIndexBuffer` |
| 7 | STORAGE_BUFFER | `meshChunksBuffer` |
| 8 | STORAGE_BUFFER | `applMaterialBuffer` |
| 9 | STORAGE_BUFFER | `_lightCuller->GetPointLightCullingDataBuffer()` |
| 10 | SAMPLED_IMAGE × N | `_scene.textures[i].second`(N=`_scene.textures.size()`,带 `VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT_EXT`,set layout 带 `VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT`) |
| 11 | SAMPLER | `_scene.textureSampler` |
| 13 | STORAGE_BUFFER | `_lightCuller->GetSpotLightCullingDataBuffer()` |
| 14 | SAMPLED_IMAGE | equirectView(或内部 1×1 占位 R32G32B32A32 图) |
| 15 | SAMPLER | 自有 `_envSampler`(linear,U repeat / V clamp) |
| 16 | STORAGE_BUFFER | `_shAccum`(host-visible coherent,`VK_BUFFER_USAGE_STORAGE_BUFFER_BIT`;Init 时 16 B 占位,Bake 按 probes×27×4 B 重建并重绑) |
| 17 | STORAGE_BUFFER | `_errorFlags`(host-visible coherent,16 B) |

3. pool:maxSets=1,sizes = AS×1、STORAGE_BUFFER×11、SAMPLED_IMAGE×(N+1)、SAMPLER×2;pool 带 UPDATE_AFTER_BIND。分配 1 个 set 并写入上表全部 binding(buffer 用 `VK_WHOLE_SIZE`)。
4. pipeline layout:set0=`_scene.globalSetLayout`,set1=本 layout;push constant range offset 0、size 112、stages = RAYGEN|CLOSEST_HIT|ANY_HIT|MISS。
5. pipeline:shader module 从 `readSpv(_scene.RootPath() / "shaders" / "vlm_bake.lib.spv")` 加载;6 个 stage(entry 名:`VlmProbeRayGen`、`VlmMissPrimary`、`MissShadow`、`ClosestHitPrimary`、`AnyHitAlpha`、`AnyHitAlphaShadow`),7 个 group 与 `Raytracing.cpp:365-435` 同构:
   - [0] GENERAL `VlmProbeRayGen`;[1] GENERAL `VlmMissPrimary`;[2] GENERAL `MissShadow`
   - [3] TRIANGLES_HIT_GROUP closestHit=[3];[4] TRIANGLES_HIT_GROUP closestHit=[3]+anyHit=[4];[5] TRIANGLES_HIT_GROUP(全 `VK_SHADER_UNUSED_KHR`);[6] TRIANGLES_HIT_GROUP anyHit=[5]
   - `maxPipelineRayRecursionDepth = 2`
6. SBT:与 `Raytracing.cpp:471-543` 同构——`_rgenRegion.stride = alignUp(handleSizeAligned, baseAlignment)`(size==stride)、`_missRegion` 2 条(alignedSize 步进)、`_hitRegion` `geomCount*2` 条,`geomCount = _scene.applMesh->_opaqueChunkCount + _scene.applMesh->_alphaMaskedChunkCount`;记录映射与 rt 相同:`[g*2+0] = g < opaqueCount ? group3 : group4`,`[g*2+1] = g < opaqueCount ? group5 : group6`。RT properties 用 `vkGetPhysicalDeviceProperties2` 自取(`VkPhysicalDeviceRayTracingPipelinePropertiesKHR`),不依赖 device 内部成员。
7. `_envSampler`:`VK_FILTER_LINEAR`、U=`VK_SAMPLER_ADDRESS_MODE_REPEAT`、V/W=`VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE`。
8. equirectView 为 null 时创建 1×1 `R32G32B32A32_SFLOAT` 黑图 + view(仅 constEnv 会用到,shader 不采样)。
9. 创建 16 B 占位 `_shAccum`/`_errorFlags`(`ensureHostBuffer`)并在全部其他 binding 写完后调用 `writeAccumDescriptors()`,保证 set 内 descriptor 全部有效。

- [ ] **Step 5: VlmBaker.cpp — Bake / readback / finalize / 验收日志**

```cpp
bool VlmBaker::Bake(const Settings& s, VlmAssetData& out) {
  const uint32_t probes = (uint32_t)s.layout.ProbeCount();
  if (probes == 0 || _pipeline == VK_NULL_HANDLE) {
    spdlog::error("vlm bake: FAILED (not initialized)");
    return false;
  }

  const VkDevice dev = _device.getLogicalDevice();
  const VkDeviceSize shBytes = (VkDeviceSize)probes * 27 * sizeof(float);
  // shAccum/errorFlags 尺寸依赖 layout,在 Bake 开头按需(重)建并清零。
  // 注意:buffer 重建后原 descriptor 指向已销毁对象,必须立即重绑 16/17。
  ensureHostBuffer(shBytes, _shAccum, _shAccumMemory);
  ensureHostBuffer(16, _errorFlags, _errorFlagsMemory);
  writeAccumDescriptors();
  {
    void* p = nullptr;
    vkMapMemory(dev, _shAccumMemory, 0, shBytes, 0, &p);
    std::memset(p, 0, (size_t)shBytes);
    vkUnmapMemory(dev, _shAccumMemory);
    vkMapMemory(dev, _errorFlagsMemory, 0, 16, 0, &p);
    std::memset(p, 0, 16);
    vkUnmapMemory(dev, _errorFlagsMemory);
  }

  const uint32_t total = s.samplesPerProbe;
  const uint32_t batches = (total + s.batchSamples - 1) / s.batchSamples;
  spdlog::info("vlm bake: probes={} samples={} batches={} batchSamples={}",
               probes, total, batches, s.batchSamples);
  const auto t0 = std::chrono::steady_clock::now();

  VkCommandBuffer cmdBuf = _device.beginSingleTimeCommands();
  for (uint32_t batch = 0, done = 0; done < total; ++batch) {
    const uint32_t n = std::min(s.batchSamples, total - done);
    VlmBakePC pc{};
    pc.probeCount = probes;
    pc.samplesThisBatch = n;
    pc.batchSeed = batch;
    pc.maxBounces = s.maxBounces;
    pc.pointLightCount = (uint32_t)_scene._pointLights.size();
    pc.spotLightCount = (uint32_t)_scene._spotLights.size();
    pc.flags = (s.skyOnly ? kFlagSkyOnly : 0u) |
               (s.constEnv ? kFlagConstEnv : 0u) |
               (s.sunIsEnvironment ? kFlagSunIsEnvironment : 0u);
    pc.cellsX = s.layout.cells[0]; pc.cellsY = s.layout.cells[1]; pc.cellsZ = s.layout.cells[2];
    pc.sunConeRadius = 0.0087f; // 与 Raytracing.cpp RTPC 一致(tan(0.5°))
    pc.sunScale = s.sunScale;
    pc.envScale = s.envScale;
    pc.localLightScale = s.localLightScale;
    pc.constEnvRGB[0] = s.constEnvRGB[0];
    pc.constEnvRGB[1] = s.constEnvRGB[1];
    pc.constEnvRGB[2] = s.constEnvRGB[2];
    for (int i = 0; i < 3; ++i) { pc.bmin[i] = s.layout.bmin[i]; pc.step[i] = s.layout.step[i]; }

    vkCmdBindPipeline(cmdBuf, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, _pipeline);
    VkDescriptorSet sets[2] = {_scene.globalDescriptorSets[_scene.currentFrame], _set};
    vkCmdBindDescriptorSets(cmdBuf, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR,
                            _pipelineLayout, 0, 2, sets, 0, nullptr);
    vkCmdPushConstants(cmdBuf, _pipelineLayout,
                       VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR |
                           VK_SHADER_STAGE_ANY_HIT_BIT_KHR | VK_SHADER_STAGE_MISS_BIT_KHR,
                       0, sizeof(pc), &pc);
    pfnCmdTraceRays(cmdBuf, &_rgenRegion, &_missRegion, &_hitRegion, &_callRegion,
                    probes, 1, 1);
    done += n;
    if (done < total) {
      // 跨 dispatch 的 RMW 依赖:写 → 读|写
      VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
      mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
      mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
      vkCmdPipelineBarrier(cmdBuf, VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
                           VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR, 0, 1, &mb, 0, nullptr, 0, nullptr);
    }
  }
  _device.endSingleTimeCommands(cmdBuf);

  const auto t1 = std::chrono::steady_clock::now();
  spdlog::info("vlm bake: gpu dispatch wall time {:.2f}s",
               std::chrono::duration<double>(t1 - t0).count());

  // --- 错误检查(Review Focus #3)---
  uint32_t* flags = nullptr;
  vkMapMemory(dev, _errorFlagsMemory, 0, 4, 0, (void**)&flags);
  const bool nanFail = flags && *flags != 0;
  vkUnmapMemory(dev, _errorFlagsMemory);
  if (nanFail) {
    spdlog::error("vlm bake: FAILED (NaN/Inf in path samples, batch rejected)");
    return false;
  }

  // --- readback + finalize + pack ---
  float* sums = nullptr;
  vkMapMemory(dev, _shAccumMemory, 0, shBytes, 0, (void**)&sums);

  std::vector<SH9> finalized(probes);
  for (uint32_t i = 0; i < probes; ++i) {
    VlmShAccumulator acc{};
    acc.sampleCount = total;
    for (int j = 0; j < 9; ++j)
      for (int c = 0; c < 3; ++c)
        acc.sum[j][c] = (double)sums[(size_t)i * 27 + j * 3 + c];
    finalized[i] = VlmShFinalize(acc);
    if (!VlmShPackFp16(finalized[i], &out.shFp16[(size_t)i * 28])) {
      vkUnmapMemory(dev, _shAccumMemory);
      spdlog::error("vlm bake: FAILED (FP16 overflow at probe {}, |c|>65504)", i);
      return false; // Review Focus #2:拒绝发布,不静默饱和
    }
  }

  // --- 验收日志(格式与 Task 7 脚本钉死)---
  const double kPi = 3.1415926535897932;
  {
    // 量化验收(规格 §10 量化行):FP32 重建 E vs FP16 打包解码重建 E,
    // 固定参考亮度 1.0 归一化(零附近不除小量)。
    static const float kNormals[6][3] = {
        {1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}};
    double qMax = 0.0;
    for (uint32_t i = 0; i < probes; ++i) {
      SH9 unpacked{};
      for (int j = 0; j < 9; ++j)
        for (int c = 0; c < 3; ++c)
          unpacked.c[j][c] = VlmHalfToFloat(out.shFp16[(size_t)i * 28 + j * 3 + c]);
      for (auto& n : kNormals) {
        float ef[3], eq[3];
        VlmShEvaluate(finalized[i], n, ef);
        VlmShEvaluate(unpacked, n, eq);
        for (int c = 0; c < 3; ++c)
          qMax = std::max(qMax, (double)std::fabs(ef[c] - eq[c]) /
                                    (double)std::fmax(1.0f, std::fabs(ef[c])));
      }
    }
    spdlog::info("vlm validate quantization: maxRelErr={:.6f} (threshold 0.010000)", qMax);
  }
  if (s.constEnv) {
    static const float kNormals[6][3] = {
        {1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}};
    double maxRel = 0.0, sumRel = 0.0;
    uint64_t cnt = 0;
    for (uint32_t i = 0; i < probes; ++i)
      for (auto& n : kNormals) {
        float E[3];
        VlmShEvaluate(finalized[i], n, E);
        for (int c = 0; c < 3; ++c) {
          const double expect = kPi * (double)s.constEnvRGB[c];
          const double rel = expect > 1e-9 ? std::fabs(E[c] - expect) / expect : std::fabs(E[c]);
          maxRel = std::max(maxRel, rel);
          sumRel += rel;
          ++cnt;
        }
      }
    spdlog::info("vlm validate const-sky: maxRelErr={:.6f} meanRelErr={:.6f} (threshold 0.010000)",
                 maxRel, sumRel / (double)cnt);
  } else if (s.skyOnly) {
    // 方向约定:CPU 参考 = ComputeSH9FromEquirect(同一 hdr)
    int w = 0, h = 0;
    float* px = stbi_loadf(s.hdrPathForReference.generic_string().c_str(), &w, &h, nullptr, 4);
    if (px) {
      SH9 ref = ComputeSH9FromEquirect(px, w, h);
      stbi_image_free(px);
      double c0Rel = 0.0, sumRef = 0.0, sumDiff = 0.0;
      for (int j = 0; j < 9; ++j)
        for (int c = 0; c < 3; ++c) {
          sumRef += std::fabs(ref.c[j][c]);
          sumDiff += std::fabs(finalized[0].c[j][c] - ref.c[j][c]);
          if (j == 0) c0Rel = std::max(c0Rel, (double)std::fabs(finalized[0].c[0][c] - ref.c[0][c]) /
                                                  std::max(1e-6, (double)std::fabs(ref.c[0][c])));
        }
      spdlog::info("vlm validate direction: c0RelErr={:.6f} weightedRelErr={:.6f} (thresholds 0.010000/0.050000)",
                   c0Rel, sumDiff / std::max(1e-6, sumRef));
    } else {
      spdlog::warn("vlm validate direction: skipped (hdr not readable: {})",
                   s.hdrPathForReference.generic_string());
    }
  }

  {
    const float up[3] = {0, 1, 0};
    const uint32_t logCount = std::min(8u, probes);
    for (uint32_t i = 0; i < logCount; ++i) {
      float E[3];
      VlmShEvaluate(finalized[i], up, E);
      spdlog::info("vlm probe {} E(+Y)={:.6f},{:.6f},{:.6f}", i, E[0], E[1], E[2]);
    }
  }

  vkUnmapMemory(dev, _shAccumMemory);
  return true;
}
```

实现注意:
- `_shAccum`/`_errorFlags` 的创建放进 `Bake` 开头(尺寸依赖 layout);host-visible + coherent + `VK_BUFFER_USAGE_STORAGE_BUFFER_BIT`;创建后立即 map+memset 清零(unmap 前);host-coherent 免 flush。
- `stbi_loadf`/`stbi_image_free` 需要 stb_image;`GpuScene.cpp` 已有的 include 方式照抄。
- 析构释放全部 Vulkan 对象(pipeline/layout/module/sbt/buffers/sampler/占位图/pool/set layout)。

`Src/CMakeLists.txt` 的 SOURCES 列表(26-49 行,`SphericalHarmonics.cpp` 所在区)追加:`VlmSh.cpp`、`VlmLayout.cpp`、`VlmAsset.cpp`、`VlmBaker.cpp`(以及 Task 8 的 `VlmRuntime.cpp`,Task 8 再加)。

- [ ] **Step 6: 构建 + 常量天空验收(规格 §10 第一行)**

```powershell
cmake --build F:\AdvancedVulkanRendering --config Debug --target AdvancedVulkanRendering
.\Bin\AdvancedVulkanRendering.exe --vlm-bake out_const.vlm --vlm-bake-volume camera,4,4,4 --vlm-bake-spacing 1 --vlm-bake-samples 4096 --vlm-bake-const-env 1,1,1 --vlm-bake-sky-only --vlm-exit-after-bake > vlm_const.log 2>&1
Get-Content vlm_const.log | Select-String "vlm"
```

Expected:
- `vlm bake: probes=125 samples=4096 batches=128 batchSamples=32`(cells=4³,probes=5³)
- `vlm validate const-sky: maxRelErr=0.00xxxx ... (threshold 0.010000)` — **maxRelErr < 0.01**(规格判据:E≈π,相对误差 ≤1%)
- `vlm bake: wrote out_const.vlm (125 probes, 7257 bytes)`(132 + 125×57)
- 无 `VUID-`、无 `vlm bake: FAILED`
- 应用自行退出(`--vlm-exit-after-bake`)

若 maxRelErr 超阈:优先查 SH 基常数(HLSL `evalSh9Basis` vs CPU `EvalSh9Basis`)、invPdf 因子、finalize 的 `kSh9A·kSh9Y` 双因子(常见错误:漏乘一个 kY,c0 会差 0.282 倍)。

- [ ] **Step 7: Commit**

```bash
git add Src/Include/VlmCommandLine.h Src/Include/VlmBaker.h Src/VlmBaker.cpp Src/Window.cpp Src/Include/GpuScene.h Src/GpuScene.cpp Src/Include/Raytracing.h Src/CMakeLists.txt
git commit -m "feat: VLM GPU baker (pipeline/SBT/batch loop/readback) + bake command line + const-sky gate"
```

---

### Task 7: 方向约定 + 光照所有权(叠加)验收脚本

**Files:**
- Create: `Tests/validate_vlm_log.py`

**Interfaces:**
- Consumes: Task 6 的日志行格式与 `.vlm` 产物。
- Produces: 三条验收命令的统一判据;`python Tests\validate_vlm_log.py <mode> <log...>` 退出码 0/1。

- [ ] **Step 1: 写脚本**

`Tests/validate_vlm_log.py`:

```python
#!/usr/bin/env python3
"""VLM 阶段 1 验收日志校验(规格 §10 常量天空/方向约定/光照所有权)。
用法:
  python Tests/validate_vlm_log.py const <bake.log>
  python Tests/validate_vlm_log.py direction <bake.log>
  python Tests/validate_vlm_log.py ownership <envonly.log> <sunonly.log> <both.log>
"""
import re
import sys

def fail(msg):
    print(f"FAIL {msg}")
    sys.exit(1)

def read(path):
    try:
        with open(path, "r", errors="replace") as f:
            return f.read()
    except OSError as e:
        fail(f"cannot read {path}: {e}")

def check_common(text, path):
    if "VUID-" in text or "Validation Error" in text:
        fail(f"{path}: validation layer errors present")
    if "vlm bake: FAILED" in text:
        fail(f"{path}: bake failed")
    if "vlm bake: wrote" not in text:
        fail(f"{path}: missing 'vlm bake: wrote' line")
    q = re.search(r"vlm validate quantization: maxRelErr=([\d.]+)", text)
    if not q:
        fail(f"{path}: missing quantization validation line")
    if float(q.group(1)) >= 0.01:
        fail(f"{path}: quantization maxRelErr {q.group(1)} >= 0.01")

def mode_const(path):
    text = read(path)
    check_common(text, path)
    m = re.search(r"vlm validate const-sky: maxRelErr=([\d.]+) meanRelErr=([\d.]+)", text)
    if not m:
        fail(f"{path}: missing const-sky validation line")
    if float(m.group(1)) >= 0.01:
        fail(f"{path}: const-sky maxRelErr {m.group(1)} >= 0.01")
    print(f"PASS const-sky maxRelErr={m.group(1)} meanRelErr={m.group(2)}")

def mode_direction(path):
    text = read(path)
    check_common(text, path)
    m = re.search(r"vlm validate direction: c0RelErr=([\d.]+) weightedRelErr=([\d.]+)", text)
    if not m:
        fail(f"{path}: missing direction validation line")
    if float(m.group(1)) >= 0.01:
        fail(f"{path}: direction c0RelErr {m.group(1)} >= 0.01")
    if float(m.group(2)) >= 0.05:
        fail(f"{path}: direction weightedRelErr {m.group(2)} >= 0.05")
    print(f"PASS direction c0RelErr={m.group(1)} weightedRelErr={m.group(2)}")

def probe_e(text):
    out = {}
    for m in re.finditer(r"vlm probe (\d+) E\(\+Y\)=([-\d.e]+),([-\d.e]+),([-\d.e]+)", text):
        out[int(m.group(1))] = tuple(float(m.group(k)) for k in (2, 3, 4))
    return out

def mode_ownership(paths):
    texts = [read(p) for p in paths]
    for t, p in zip(texts, paths):
        check_common(t, p)
    ea, eb, ec = (probe_e(t) for t in texts)
    keys = sorted(set(ea) & set(eb) & set(ec))
    if not keys:
        fail("ownership: no common probe E(+Y) lines across the three logs")
    worst = 0.0
    for k in keys:
        for ch in range(3):
            lhs = ec[k][ch]
            rhs = ea[k][ch] + eb[k][ch]
            rel = abs(lhs - rhs) / max(abs(lhs), 0.05)
            worst = max(worst, rel)
    # 三次烘焙同种子同路径(RR 不依赖 env/sun 强度),叠加应近似精确。
    if worst >= 0.02:
        fail(f"ownership: superposition rel err {worst:.4f} >= 0.02")
    print(f"PASS ownership superposition worstRelErr={worst:.6f} over {len(keys)} probes")

if __name__ == "__main__":
    if len(sys.argv) < 3:
        fail("usage: validate_vlm_log.py <const|direction|ownership> <log...>")
    mode = sys.argv[1]
    if mode == "const":
        mode_const(sys.argv[2])
    elif mode == "direction":
        mode_direction(sys.argv[2])
    elif mode == "ownership":
        if len(sys.argv) != 5:
            fail("ownership needs exactly 3 logs (envonly sunonly both)")
        mode_ownership(sys.argv[2:5])
    else:
        fail(f"unknown mode {mode}")
```

- [ ] **Step 2: 方向约定验收(规格 §10 第二行)**

```powershell
.\Bin\AdvancedVulkanRendering.exe --vlm-bake out_dir.vlm --vlm-bake-volume camera,4,4,4 --vlm-bake-spacing 1 --vlm-bake-samples 8192 --vlm-bake-sky-only --vlm-exit-after-bake > vlm_dir.log 2>&1
python Tests\validate_vlm_log.py direction vlm_dir.log
```

Expected: `PASS direction c0RelErr=0.00xx weightedRelErr=0.0x`。若失败且误差呈镜像特征(E(+x)/E(-x) 对调):查 `DirectionToEquirectUV` 的 `-d.x` 是否与 CPU 一致。

- [ ] **Step 3: 光照所有权叠加验收(规格 §10 第五行;真实几何 + 三次烘焙)**

同一体积三次烘焙(太阳/环境/局部灯缩放互斥;路径与种子三次一致,叠加应近似精确):

```powershell
.\Bin\AdvancedVulkanRendering.exe --vlm-bake out_own_env.vlm --vlm-bake-volume camera,4,4,4 --vlm-bake-spacing 1 --vlm-bake-samples 2048 --vlm-sun-scale 0 --vlm-local-light-scale 0 --vlm-exit-after-bake > vlm_own_env.log 2>&1
.\Bin\AdvancedVulkanRendering.exe --vlm-bake out_own_sun.vlm --vlm-bake-volume camera,4,4,4 --vlm-bake-spacing 1 --vlm-bake-samples 2048 --vlm-env-scale 0 --vlm-local-light-scale 0 --vlm-exit-after-bake > vlm_own_sun.log 2>&1
.\Bin\AdvancedVulkanRendering.exe --vlm-bake out_own_both.vlm --vlm-bake-volume camera,4,4,4 --vlm-bake-spacing 1 --vlm-bake-samples 2048 --vlm-local-light-scale 0 --vlm-exit-after-bake > vlm_own_both.log 2>&1
python Tests\validate_vlm_log.py ownership vlm_own_env.log vlm_own_sun.log vlm_own_both.log
```

Expected: `PASS ownership superposition worstRelErr=0.000xxx`。这同时验证:(a) 真实几何命中路径与 BSDF 续跳工作;(b) 太阳 NEE 与环境贡献各自只计一次;(c) FP32 累加无 clamp/截断。若超阈:查 `sunScale/envScale` 是否都进了 shader push const、RR 是否被强度分支意外影响(不应)。

- [ ] **Step 4: Commit**

```bash
git add Tests/validate_vlm_log.py
git commit -m "test: VLM phase-1 acceptance script (const-sky, direction, ownership superposition)"
```

### Task 8: VlmRuntime + deferred/forward 接入 + ImGui + 边界混合验证

**Files:**
- Create: `Src/Include/VlmRuntime.h`、`Src/VlmRuntime.cpp`、`shaders/vlm_common.hlsl`
- Modify: `shaders/ibl_common.hlsl`(拆出 `IBLWithDiffuseE`)、`shaders/deferredlighting.hlsl`(绑定 + 钩子)、`shaders/drawcluster.hlsl`(绑定 + 两处钩子)、`shaders/commonstruct.hlsl`(`AAPLFrameConstants` 尾部改名)、`Src/Include/Common.h`(同步 + static_assert)、`Src/Include/GpuScene.h`(成员 + 初始化器)、`Src/GpuScene.cpp`(`initVlmRuntime` + descriptor 布局/写入 + ImGui + bake 后即时加载)、`Src/CMakeLists.txt`(`VlmRuntime.cpp`)

**Interfaces:**
- Consumes: Task 3 (`VlmLoadAsset/VlmAssetData`)、Task 6 (`RunVlmBake` 产物)。
- Produces:
  - `class VlmRuntime { public: bool LoadFromAsset(const VlmAssetData&); bool LoadFromFile(const std::string& path, uint64_t expectedSceneHash); bool IsActive() const; VkBuffer ParamsBuffer() const; VkBuffer ShBuffer() const; uint32_t ProbeCount() const; void WriteDeferredDescriptors(const std::vector<VkDescriptorSet>& sets) const; void WriteApplDescriptors(const std::vector<VkDescriptorSet>& sets) const; };`
  - HLSL:`struct VlmParams`(64 B)、`struct VlmSampleResult { float3 E; float weight; }`、`VlmSampleResult SampleVlm(VlmParams P, StructuredBuffer<float4> shData, float3 worldPos, float3 shadingNormal, float3 geometricNormal)`、`float3 IBLWithDiffuseE(...)`(ibl_common.hlsl)。
  - descriptor:deferred set1 binding 22(`VlmParams` UBO)/ 23(SH SSBO)/ 24(物理天空 SH UBO);appl set1 binding 9 / 10 / 11(物理天空 SH UBO)。
  - `FrameConstants.vlmScale`(offset 136)、`FrameConstants.vlmFlags`(offset 140)。
  - GpuScene 物理天空 SH:`_vlmSkySHBuffer`(`ComputeSH9FromEquirect` 输出,与烘焙同物理域;**不用** Apple 兼容的 `_iblSHBuffer` 做 VLM 边界混合,规格 §8.3:Apple diffuse IBL 只作为整体关闭 VLM 后的旧模式)。

- [ ] **Step 1: ibl_common.hlsl 拆出 IBLWithDiffuseE(行为不变)**

`shaders/ibl_common.hlsl` 的 `IBL()` 重构为两个函数,`IBL()` 保持原签名与行为:

```hlsl
// IBL 主体:diffuseE 由调用方给出(天空 SH 或 VLM 混合后),specular 部分不变。
float3 IBLWithDiffuseE(AAPLPixelSurfaceData surface,
                       TextureCube envMap,
                       Texture2D<float2> dfgLut,
                       SamplerState samp,
                       float3 diffuseE,
                       float3 viewDir,
                       float scale,
                       float specularScale)
{
    float3 diffuseIBL = diffuseE * (float3)surface.albedo;

    float perceptualRoughness = (float)surface.roughness;
    float NoV = max(dot((float3)surface.normal, viewDir), 0.0);
    float3 r = reflect(-viewDir, (float3)surface.normal);

    const float mipLevels = 8.0;
    float lod = perceptualRoughness * mipLevels;
    float3 indirectSpecular = envMap.SampleLevel(samp, r, lod).rgb;
    indirectSpecular = min(indirectSpecular, 8192.0);

    float2 dfg = dfgLut.SampleLevel(samp, float2(NoV, perceptualRoughness), 0);
    float3 specularColor = (float3)surface.F0 * dfg.x + dfg.y;

    float3 specularIBL = indirectSpecular * specularColor;
    return (diffuseIBL + specularIBL * specularScale) * scale;
}

float3 IBL(AAPLPixelSurfaceData surface,
           TextureCube envMap,
           Texture2D<float2> dfgLut,
           SamplerState samp,
           float4 sh[9],
           float3 viewDir,
           float scale,
           float specularScale)
{
    return IBLWithDiffuseE(surface, envMap, dfgLut, samp,
                           evaluateShCoefficients((float3)surface.normal, sh),
                           viewDir, scale, specularScale);
}
```

编译验证(IBL 行为不变,后续统一截图确认):

```powershell
cmd /c "cd /d F:\AdvancedVulkanRendering\shaders && .\compile_shaders.bat"
```

- [ ] **Step 2: vlm_common.hlsl(共享采样,与 Task 2 CPU 公式逐字一致)**

```hlsl
// vlm_common.hlsl — VLM 运行时采样(deferred/forward 共用;Vulkan only,
// 调用方自行用 #ifndef DX12_BACKEND 保护)。
// 阶段 1:均匀网格、显式 8 邻居 gather、三线性 × 有效性归一化、边界过渡带混回天空。
// 法线偏置用 geometricNormal,SH 方向求值用 shadingNormal(规格 §4/§8.1)。

struct VlmParams {
    float3 boundsMin;    float bandWidth;     // 16
    float3 step;         float normalBias;    // 32
    float3 invStep;      float normalBiasMax; // 48
    uint3  cells;        uint  probeTotal;    // 64
};  // 64 B,与 Src/VlmRuntime.cpp 的 VlmParamsUBO 一致

struct VlmSampleResult { float3 E; float weight; };

// 与 VlmCellLookup / VlmBoundaryWeight(VlmLayout.cpp)逐字一致:
// cell=clamp(floor(local),0,cells-1),frac=saturate(local-cell)(最大边界 frac=1)。
float vlmBoundaryWeight(VlmParams P, float3 pos) {
    float3 hi = P.boundsMin + P.step * float3(P.cells);
    float3 d = min(pos - P.boundsMin, hi - pos);
    float dMin = min(d.x, min(d.y, d.z));
    return saturate(dMin / max(P.bandWidth, 1e-6));
}

// 从 7×float4/probe 的打包数据还原 9×float4(flat[j*3+ch],第 28 通道忽略)。
void vlmLoadSh9(StructuredBuffer<float4> shData, uint probeIdx, out float4 sh[9]) {
    float e[27];
    [unroll] for (uint k = 0; k < 7; ++k) {
        float4 v = shData[probeIdx * 7u + k];
        e[k * 4 + 0] = v.x; e[k * 4 + 1] = v.y; e[k * 4 + 2] = v.z;
        if (k * 4 + 3 < 27) e[k * 4 + 3] = v.w;
    }
    [unroll] for (uint j = 0; j < 9; ++j)
        sh[j] = float4(e[j * 3], e[j * 3 + 1], e[j * 3 + 2], 0.0f);
}

VlmSampleResult SampleVlm(VlmParams P, StructuredBuffer<float4> shData,
                          float3 worldPos, float3 shadingNormal, float3 geometricNormal) {
    VlmSampleResult r;
    r.E = (float3)0;
    r.weight = 0.0f;

    // 查询偏置:沿 geometricNormal,cell 比例配置并封顶(规格 §8.1)
    float biasScale = min(P.step.x, min(P.step.y, P.step.z));
    float bias = min(P.normalBias * biasScale, P.normalBiasMax);
    float3 q = worldPos + geometricNormal * bias;

    float w = vlmBoundaryWeight(P, q);
    if (w <= 0.0f) return r; // 体积外:weight=0,调用方回退天空 diffuse

    float3 local = (q - P.boundsMin) * P.invStep;
    int3 cell = clamp((int3)floor(local), int3(0, 0, 0), (int3)P.cells - 1);
    float3 f = saturate(local - (float3)cell);

    uint3 PC = P.cells + 1;
    float3 Esum = (float3)0;
    float wsum = 0.0f;
    [unroll] for (uint dz = 0; dz <= 1; ++dz)
    [unroll] for (uint dy = 0; dy <= 1; ++dy)
    [unroll] for (uint dx = 0; dx <= 1; ++dx) {
        float wx = (dx != 0u) ? f.x : 1.0f - f.x;
        float wy = (dy != 0u) ? f.y : 1.0f - f.y;
        float wz = (dz != 0u) ? f.z : 1.0f - f.z;
        float w8 = wx * wy * wz;
        uint3 c = (uint3)cell + uint3(dx, dy, dz);
        uint probeIdx = (c.z * PC.y + c.y) * PC.x + c.x; // 与 VlmProbeIndex 一致
        float4 sh[9];
        vlmLoadSh9(shData, probeIdx, sh);
        // 阶段 1 有效性恒 1(资产加载时已校验);无效探针修补属阶段 2。
        Esum += w8 * evaluateShCoefficients(shadingNormal, sh);
        wsum += w8;
    }

    if (wsum < 1e-4) {
        // 覆盖缺口(规格 §8.3):返回零 GI + weight 标记,不回退明亮天空
        r.weight = w;
        return r;
    }
    r.E = Esum / wsum;
    r.weight = w;
    return r;
}
```

- [ ] **Step 3: FrameConstants 尾部改名(双端同步)**

`shaders/commonstruct.hlsl` 的 `AAPLFrameConstants`(74-98)末尾:

```hlsl
    float iblScale;         // offset 128
    float iblSpecularScale; // offset 132
    float vlmScale;         // offset 136(0 = 关闭 VLM,默认加载资产后 1)
    float vlmFlags;         // offset 140(保留;位定义见 VlmRuntime)
```

`Src/Include/Common.h`(94-99)同步:

```cpp
  float iblScale;          // IBL 总强度(0 = 关闭);ImGui 可调,默认 1.0
  float iblSpecularScale;  // IBL 高光强度;ImGui 可调,默认 4.0
  float vlmScale;          // VLM 漫反射强度(0 = 关闭);ImGui 可调,默认见 GpuScene.h
  float vlmFlags;          // 保留
```

static_assert 区(101-107)追加:

```cpp
static_assert(offsetof(FrameConstants, vlmScale)  == 136, "vlmScale offset mismatch vs HLSL Offset(136)");
static_assert(offsetof(FrameConstants, vlmFlags)  == 140, "vlmFlags offset mismatch vs HLSL Offset(140)");
```

`sizeof == 144` 的既有断言不变。跑 DX12 批处理确认零错误:

```powershell
cmd /c "cd /d F:\AdvancedVulkanRendering\shaders && .\compile_shaders_dx12.bat"
```

- [ ] **Step 4: VlmRuntime**

`Src/Include/VlmRuntime.h`:

```cpp
#pragma once

#include "VlmAsset.h"
#include "VulkanSetup.h"
#include <string>
#include <vector>

// VLM 运行时:加载 .vlm → 上传 SH SSBO(7×float4/probe,fp16 解码为 float)+ 参数 UBO。
// 采样语义见 shaders/vlm_common.hlsl;材质与显示变换不进入采样函数(规格 §4)。
class VlmRuntime {
public:
  VlmRuntime(VulkanDevice& device);
  ~VlmRuntime();

  bool LoadFromAsset(const VlmAssetData& asset);                          // 烘焙后即时激活
  bool LoadFromFile(const std::string& path, uint64_t expectedSceneHash); // --vlm <path>

  bool IsActive() const { return _active; }
  VkBuffer ParamsBuffer() const { return _paramsBuffer; }
  VkBuffer ShBuffer() const { return _shBuffer; }
  uint32_t ProbeCount() const { return _probeCount; }

  // 写 deferred set1 binding 22/23 或 appl set1 binding 9/10(每帧 set 各一次)。
  // 资产不存在时绑定内建兜底(1 probe 全零 SH + bias=0 参数)。
  void WriteDeferredDescriptors(const std::vector<VkDescriptorSet>& sets) const;
  void WriteApplDescriptors(const std::vector<VkDescriptorSet>& sets) const;

private:
  VulkanDevice& _device;
  bool _active = false;
  uint32_t _probeCount = 0;
  VkBuffer _paramsBuffer = VK_NULL_HANDLE;
  VkDeviceMemory _paramsMemory = VK_NULL_HANDLE;
  VkBuffer _shBuffer = VK_NULL_HANDLE;
  VkDeviceMemory _shMemory = VK_NULL_HANDLE;
  // 兜底资源(构造时创建,保证 descriptor 永远有效)
  VkBuffer _fallbackShBuffer = VK_NULL_HANDLE;
  VkDeviceMemory _fallbackShMemory = VK_NULL_HANDLE;
  VkBuffer _fallbackParamsBuffer = VK_NULL_HANDLE;
  VkDeviceMemory _fallbackParamsMemory = VK_NULL_HANDLE;

  void writeSets(const std::vector<VkDescriptorSet>& sets,
                 uint32_t paramsBinding, uint32_t shBinding) const;
};
```

`Src/VlmRuntime.cpp` 要点:
- `VlmParamsUBO`(C++ 镜像,64 B):`float boundsMin[3], bandWidth; float step[3], normalBias; float invStep[3], normalBiasMax; uint32_t cells[3], probeTotal;` + `static_assert(sizeof(VlmParamsUBO) == 64)`。`normalBias = 0.3f`、`normalBiasMax = 0.5f`(规格 §8.1:cell 比例 + 上限;单位验证记录于资产 `unitScale`)。
- 构造时创建兜底:fallback SH = 7 个 float4 全零(1 probe);fallback params = `boundsMin={0,0,0}, step={1,1,1}, invStep={1,1,1}, cells={1,1,1}, probeTotal=8, bandWidth=1, normalBias=0, normalBiasMax=0`(bias 全 0 + vlmScale=0 时 shader 不进入分支,双保险)。
- `LoadFromAsset`:校验 `validity` 全 1(阶段 1 断言;有为 0 者 `spdlog::warn` 计数,仍加载——阶段 2 才接入有效性权重);fp16 → float 解码(`VlmHalfToFloat`,flat 27 顺序与 `vlmLoadSh9` 一致)→ host-visible coherent STORAGE buffer 一次性 map+memcpy;填充 params UBO(`bandWidth` 取资产值);`_active = true`。
- `LoadFromFile`:`VlmLoadAsset` → `Ok` 走 `LoadFromAsset`;`Stale` → `spdlog::warn("vlm: stale asset (scene changed), VLM disabled")` + `_active=false`(Review Focus #4);`Rejected` → `spdlog::error` + `_active=false`。
- descriptor 写入(两个函数仅 binding 号不同,实现体共享一个 static helper):

```cpp
void VlmRuntime::WriteDeferredDescriptors(const std::vector<VkDescriptorSet>& sets) const {
  writeSets(sets, 22, 23);
}
void VlmRuntime::WriteApplDescriptors(const std::vector<VkDescriptorSet>& sets) const {
  writeSets(sets, 9, 10);
}
// 文件作用域 static:
static void writeSetsImpl(VkDevice dev, const std::vector<VkDescriptorSet>& sets,
                          uint32_t paramsBinding, uint32_t shBinding,
                          VkBuffer params, VkBuffer sh) {
  VkDescriptorBufferInfo pi{params, 0, 64};
  VkDescriptorBufferInfo si{sh, 0, VK_WHOLE_SIZE};
  for (VkDescriptorSet set : sets) {
    VkWriteDescriptorSet w[2]{};
    w[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w[0].dstSet = set;
    w[0].dstBinding = paramsBinding;
    w[0].descriptorCount = 1;
    w[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    w[0].pBufferInfo = &pi;
    w[1] = w[0];
    w[1].dstBinding = shBinding;
    w[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    w[1].pBufferInfo = &si;
    vkUpdateDescriptorSets(dev, 2, w, 0, nullptr);
  }
}
// writeSets(成员):_active ? (_paramsBuffer,_shBuffer) : (_fallbackParamsBuffer,_fallbackShBuffer)
```

- [ ] **Step 5: GpuScene descriptor 接线**

`Src/Include/GpuScene.h` 成员(`_iblSHBuffer` 同区,510-528 附近):

```cpp
  VlmRuntime *_vlmRuntime = nullptr;
  VkBuffer _vlmSkySHBuffer = VK_NULL_HANDLE;
  VkDeviceMemory _vlmSkySHMemory = VK_NULL_HANDLE;
```

`GpuScene.cpp`:
- **物理天空 SH UBO**(VLM 边界 fallback 必须同为物理域,规格 §3/§8.3):`initIBL()` 在 `ComputeMetalSH9FromEquirect`(6507)之后,用同一 stbi 像素再算 `SH9 physSh = ComputeSH9FromEquirect(pixels, w, h);`,随后镜像 `_iblSHBuffer` 的创建代码(6564-6574)创建 `_vlmSkySHBuffer`(144 B host-coherent UBO,`shGpu` 填充方式相同)。`createBlackFallbackIBL()`(6379-6493)同步创建全零 `_vlmSkySHBuffer`。成员声明加在 `GpuScene.h` 的 `_iblSHBuffer` 同区:`VkBuffer _vlmSkySHBuffer = VK_NULL_HANDLE; VkDeviceMemory _vlmSkySHMemory = VK_NULL_HANDLE;`
- `initVlmRuntime()`:`initIBL()`(3016 行)调用之后、`init_appl_descriptors()`(3025)之前插入调用:
```cpp
  _vlmRuntime = new VlmRuntime(const_cast<VulkanDevice &>(device));
  if (_vlmCmdValid && !_vlmCmdStorage.loadPath.empty()) {
    if (_vlmRuntime->LoadFromFile(_vlmCmdStorage.loadPath, ComputeVlmSceneHash()))
      frameConstants.vlmScale = 1.0f; // 只写一次(Review Focus #5)
  }
```
- deferred set layout(`init_deferredlighting_descriptors`,在 binding 21 的 `shUboBinding` 之后,1376-1377 的 `vkCreateDescriptorSetLayout` 之前)追加三个 binding:
```cpp
    // VLM(22-24,see deferredlighting.hlsl;Vulkan only)
    VkDescriptorSetLayoutBinding vlmParamsBinding = {};
    vlmParamsBinding.binding = 22;
    vlmParamsBinding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    vlmParamsBinding.descriptorCount = 1;
    vlmParamsBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutBinding vlmShBinding = vlmParamsBinding;
    vlmShBinding.binding = 23;
    vlmShBinding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    VkDescriptorSetLayoutBinding vlmSkyShBinding = vlmParamsBinding;
    vlmSkyShBinding.binding = 24; // 物理天空 SH(ComputeSH9FromEquirect,VLM 边界 fallback)
```
  加入 bindings 数组;pool sizes 对应 +2 UBO +1 SSBO。
- appl set layout(`init_appl_descriptors`,binding 8 的 `shBinding` 之后):同样三个 binding,号为 9(`VlmParams` UBO)/10(SH SSBO)/11(物理天空 SH UBO),stageFlags = `VK_SHADER_STAGE_FRAGMENT_BIT`;pool sizes 同上。
- 物理天空 SH 的写入是静态的,直接仿照 `_iblSHBuffer` 的写入位置:deferred 在 binding 21 写入(1497-1505)之后加 binding 24 = `_vlmSkySHBuffer`(range `9*16`);appl 在 binding 8 写入(1989-2001)之后加 binding 11。
- 两个 init 函数末尾(每帧 set 写入完成后)分别调用:
```cpp
  _vlmRuntime->WriteDeferredDescriptors(deferredLightingDescriptorSet); // init_deferredlighting_descriptors 末尾
  _vlmRuntime->WriteApplDescriptors(applDescriptorSet);                  // init_appl_descriptors 末尾
```
  (成员名:`deferredLightingDescriptorSet` / `applDescriptorSet`,均为 `std::vector<VkDescriptorSet>` per-frame,声明于 `GpuScene.h:166-176`。)
- `RunVlmBake()` 成功保存资产后追加即时激活:
```cpp
  if (_vlmRuntime && _vlmRuntime->LoadFromAsset(asset)) {
    frameConstants.vlmScale = 1.0f;
    _vlmRuntime->WriteDeferredDescriptors(deferredLightingDescriptorSet);
    _vlmRuntime->WriteApplDescriptors(applDescriptorSet);
    spdlog::info("vlm: runtime activated ({} probes, {} MB SH)",
                 _vlmRuntime->ProbeCount(),
                 (double)_vlmRuntime->ProbeCount() * 112.0 / 1e6);
  }
```
- `GpuScene.h` 的 `frameConstants` 成员初始化器(994-1011 区,与 `iblScale = 1.0f` 并列):`vlmScale = 0.0f`(默认关,加载/烘焙成功后由上述两处置 1)、`vlmFlags = 0.0f`。per-frame 填充段(3644-3675)**不得**出现 `vlmScale`(Review Focus #5)。
- ImGui(`renderImGuiOverlay` 的 IBL 块 9103-9107 之后):
```cpp
  if (ImGui::CollapsingHeader("VLM")) {
    ImGui::SliderFloat("VLM Scale", &frameConstants.vlmScale, 0.0f, 2.0f);
    if (!_vlmRuntime || !_vlmRuntime->IsActive()) ImGui::TextDisabled("no VLM asset (bake or --vlm)");
    else ImGui::Text("%u probes", _vlmRuntime->ProbeCount());
  }
```

- [ ] **Step 6: deferredlighting.hlsl 钩子**

include 区(1-4 行)追加:

```hlsl
#ifndef DX12_BACKEND
#include "vlm_common.hlsl"
#endif
```

IBL 绑定块(43-49)内追加:

```hlsl
VK_BINDING(22,1) cbuffer VlmParamsCB { VlmParams vlmParams; };
VK_BINDING(23,1) StructuredBuffer<float4> vlmShData;
VK_BINDING(24,1) cbuffer VlmSkySHCoefficients { float4 vlmSkyShCoefs[9]; }; // 物理天空 SH
```

IBL 调用块(152-164)替换为(VLM 与 IBL 的开关/强度解耦:`iblScale` 不作用于 VLM,规格 §3;diffuse 替换用同一权重 lerp,禁止相加;specular 沿用 Apple 兼容域):

```hlsl
#ifndef DX12_BACKEND
    // IBL:(sun*shadow + IBL) * AO —— 对齐 Apple AAPLLightingCommon.h 的顺序。
    // sky 像素(depth≈0)不加:随后的 sky 分支会整体覆盖 result。
    if (depth >= 0.0001f)
    {
        float3 camPosIBL = float3(cameraParams.invViewMatrix._m03,
                                  cameraParams.invViewMatrix._m13,
                                  cameraParams.invViewMatrix._m23);
        float3 viewDirIBL = normalize(camPosIBL - worldPosition.xyz);
        if (frameConstants.vlmScale > 0.0f)
        {
            // GBuffer 未存几何法线:deferred 以 shading normal 代 geometric normal
            // (记录在案的偏差,规格 §8.1;forward 传入真实几何法线)。
            VlmSampleResult vs = SampleVlm(vlmParams, vlmShData, worldPosition.xyz,
                                           (float3)surfaceData.normal, (float3)surfaceData.normal);
            // 边界 fallback 用物理天空 SH(非 Apple 兼容域),同一权重混合,禁止相加(规格 §3/§8.3)。
            float3 skyPhysE = evaluateShCoefficients((float3)surfaceData.normal, vlmSkyShCoefs);
            float3 diffuseIBL = lerp(skyPhysE, vs.E, vs.weight)
                                * frameConstants.vlmScale * (float3)surfaceData.albedo;
            result += (half3)diffuseIBL;
            if (frameConstants.iblScale > 0.0f)
            {
                // specular IBL 保留原 Apple 兼容路径(diffuseE=0 → 只贡献高光)。
                result += (half3)IBLWithDiffuseE(surfaceData, envMap, dfgLutTex, iblSampler,
                                                 (float3)0, viewDirIBL,
                                                 frameConstants.iblScale, frameConstants.iblSpecularScale);
            }
        }
        else if (frameConstants.iblScale > 0.0f)
        {
            result += (half3)IBL(surfaceData, envMap, dfgLutTex, iblSampler, shCoefs,
                                 viewDirIBL, frameConstants.iblScale, frameConstants.iblSpecularScale);
        }
    }
#endif
```

- [ ] **Step 7: drawcluster.hlsl 两处钩子**

include 区(第 3 行 ibl_common 之后)同样 guard 追加 `vlm_common.hlsl`;IBL 绑定块(66-72)内追加:

```hlsl
VK_BINDING(9,1) cbuffer VlmParamsCB { VlmParams vlmParams; };
VK_BINDING(10,1) StructuredBuffer<float4> vlmShData;
VK_BINDING(11,1) cbuffer VlmSkySHCoefficients { float4 vlmSkyShCoefs[9]; }; // 物理天空 SH
```

`RenderSceneForwardPS`(300-310)与 `RenderSceneForwardPSIndirect`(395-405)的 IBL 块各自替换为(两处相同;forward 有真实几何法线 `geonormal` 可用作偏置方向,规格 §8.1):

```hlsl
#ifndef DX12_BACKEND
    {
        float3 camPosIBL = float3(cameraParams.invViewMatrix._m03,
                                  cameraParams.invViewMatrix._m13,
                                  cameraParams.invViewMatrix._m23);
        float3 viewDirIBL = normalize(camPosIBL - input.wsPosition.xyz);
        if (frameConstants.vlmScale > 0.0f)
        {
            VlmSampleResult vs = SampleVlm(vlmParams, vlmShData, input.wsPosition.xyz,
                                           (float3)surfaceData.normal, (float3)geonormal);
            float3 skyPhysE = evaluateShCoefficients((float3)surfaceData.normal, vlmSkyShCoefs);
            float3 diffuseIBL = lerp(skyPhysE, vs.E, vs.weight)
                                * frameConstants.vlmScale * (float3)surfaceData.albedo;
            res += (half3)diffuseIBL;
            if (frameConstants.iblScale > 0.0f)
            {
                res += (half3)IBLWithDiffuseE(surfaceData, envMap, dfgLutTex, iblSampler,
                                              (float3)0, viewDirIBL,
                                              frameConstants.iblScale, frameConstants.iblSpecularScale);
            }
        }
        else if (frameConstants.iblScale > 0.0f)
        {
            res += (half3)IBL(surfaceData, envMap, dfgLutTex, iblSampler, shCoefs,
                              viewDirIBL, frameConstants.iblScale, frameConstants.iblSpecularScale);
        }
    }
#endif
```

- [ ] **Step 8: 编译 + 接入验证**

```powershell
cmd /c "cd /d F:\AdvancedVulkanRendering\shaders && .\compile_shaders.bat"
cmd /c "cd /d F:\AdvancedVulkanRendering\shaders && .\compile_shaders_dx12.bat"
cmake --build F:\AdvancedVulkanRendering --config Debug --target AdvancedVulkanRendering
```

Expected: 三个构建全部零错误(DX12 批处理证明 `commonstruct.hlsl` 改名与 guard 无副作用)。

- [ ] **Step 9: 端到端验证(烘焙→加载→画面)**

```powershell
.\Bin\AdvancedVulkanRendering.exe --vlm-bake out_bistro.vlm --vlm-bake-volume camera,8,6,8 --vlm-bake-spacing 1 --vlm-bake-samples 2048 > vlm_e2e.log 2>&1
```

Expected:
- 烘焙完成后 `vlm: runtime activated (...)`;画面阴影区出现反弹光(与 `vlm_const.log` 同场景对比,VLM Scale 拉 0 恢复原画面);
- ImGui "VLM" 块显示 probe 数;`VLM Scale` 滑动实时生效(Review Focus #5:拖几下后切别的小节再回来,值不被重置);
- 截图留存:`vlm_e2e_on.png`(scale 1)/ `vlm_e2e_off.png`(scale 0);
- RenderDoc 抓帧:`get_draw_calls` 找 deferred lighting draw,`get_shader_details`(pixel stage)确认 binding 22/23/24 已绑定非兜底 buffer;forward draw 确认 binding 9/10/11;
- `vlm_e2e.log` 无 `VUID-`、无 `vlm bake: FAILED`;
- 内存记录:`vlm: runtime activated` 日志行包含 probe 数与 SH SSBO 十进制 MB(规格 §10 运行时成本行的阶段 1 数据;GPU 时间测量留待阶段 2)。

边界混合验证:同一资产下把相机移到 BakeVolume 外(自由飞行 Q/E/W/S),画面应平滑从 VLM 过渡到**物理天空** diffuse、无硬边界闪烁;体积外远处区域在 `vlmScale=1` 时即显示物理天空 diffuse(与 `vlmScale=0` 的 Apple 兼容 IBL 画面可能略有差异,属规格 §8.3 的预期行为,不是回归)。

旧资产加载回归:

```powershell
.\Bin\AdvancedVulkanRendering.exe --vlm out_bistro.vlm > vlm_load.log 2>&1
```

Expected: 启动即 `vlm: runtime activated`,无重新烘焙日志;再改 `--vlm-bake-const-env` 烘一个不同签名的资产后 `--vlm` 加载旧资产 → `vlm: stale asset` 警告且 VLM 禁用(Review Focus #4 的运行时表现;scene 哈希相同则不会 stale,用不同 volume 烘焙制造 settingsHash/sceneHash 不变的假象不算——stale 演示以手工改 `expectedSceneHash` 单测为准,此处只验证 Ok 路径)。

- [ ] **Step 10: Commit**

```bash
git add shaders/vlm_common.hlsl shaders/ibl_common.hlsl shaders/deferredlighting.hlsl shaders/drawcluster.hlsl shaders/commonstruct.hlsl Src/Include/Common.h Src/Include/VlmRuntime.h Src/VlmRuntime.cpp Src/Include/GpuScene.h Src/GpuScene.cpp Src/CMakeLists.txt
git add -u shaders/
git commit -m "feat: VLM runtime sampling in deferred + forward (uniform grid gather, boundary blend, imgui toggle)"
```

---

### Task 9: 全量回归 + 文档收尾

**Files:**
- Modify: `docs/superpowers/specs/2026-10-01-volumetric-lightmap-design.md`(状态行)

- [ ] **Step 1: 测试套件全绿**

```powershell
cmake --build F:\AdvancedVulkanRendering\build-vlm-tests --config Debug
F:\AdvancedVulkanRendering\build-vlm-tests\Debug\SphericalHarmonicsTests.exe
F:\AdvancedVulkanRendering\build-vlm-tests\Debug\VlmShTests.exe
F:\AdvancedVulkanRendering\build-vlm-tests\Debug\VlmLayoutTests.exe
F:\AdvancedVulkanRendering\build-vlm-tests\Debug\VlmAssetTests.exe
```

Expected: 四个 `all ... tests passed`。

- [ ] **Step 2: shader 双后端零错误**

```powershell
cmd /c "cd /d F:\AdvancedVulkanRendering\shaders && .\compile_shaders.bat"
cmd /c "cd /d F:\AdvancedVulkanRendering\shaders && .\compile_shaders_dx12.bat"
```

Expected: 均无错误输出。

- [ ] **Step 3: 三条验收脚本全过**

```powershell
python Tests\validate_vlm_log.py const vlm_const.log
python Tests\validate_vlm_log.py direction vlm_dir.log
python Tests\validate_vlm_log.py ownership vlm_own_env.log vlm_own_sun.log vlm_own_both.log
```

(若日志已清理,按 Task 6 Step 6 / Task 7 Step 2-3 的命令重跑生成。)

- [ ] **Step 4: RT 相机路径 + DX12 回归**

```powershell
.\Bin\AdvancedVulkanRendering.exe
.\Bin\AdvancedVulkanRendering.exe --dx12
```

Expected:Vulkan 默认 raster 画面与 `ibl` 分支一致(IBL 正常、VLM 默认关);ImGui 勾 Ray Tracing 后 PT 画面与 Task 4 基线一致;DX12 启动正常、画面无回归(DX12 无 VLM 属预期)。

- [ ] **Step 5: 规格状态更新 + Commit**

`docs/superpowers/specs/2026-10-01-volumetric-lightmap-design.md` 第 5 行状态改为:

```text
状态:阶段 1(均匀网格闭环)已实现并通过 §10 常量天空/方向约定/光照所有权验收;阶段 2-4 未开始。
```

```bash
git add docs/superpowers/specs/2026-10-01-volumetric-lightmap-design.md
git commit -m "docs: mark VLM phase 1 as implemented and validated"
```
