# Vulkan IBL 实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 为 Vulkan 后端实现运行时生成的 IBL(预过滤环境 cube + GGX DFG LUT + SH 漫反射),覆盖 deferred 与 forward 两个 pass。

**Architecture:** 从 `.hdr` equirect 出发:CPU 积分 SH9(纯函数模块)→ GPU 一次性 compute 链(equirect→cube mip0 → GGX 重要性采样预过滤 mip1-8 → Karis DFG LUT)→ 两张纹理 + SH UBO 绑定进 deferred(set1 binding 18-21)与 forward(appl set binding 5-8),shader 侧新增 `ibl_common.hlsl` 提供与 Metal 同数学的 `IBL()`。

**Tech Stack:** Vulkan 1.3、dxc(SPIR-V,HLSL 2021)、stb_image(`stbi_loadf`)、C++20、MSVC/CMake(in-source 构建,输出 `Bin/`)。

**Spec:** `docs/superpowers/specs/2026-09-26-vulkan-ibl-design.md`

## Global Constraints

- 工作分支:`ibl`(已从 `texture_streaming` 切出)。**不要**提交 `Src/PbrtExporter.cpp/.h` 的既有未提交改动(与本特性无关),`git add` 时逐文件添加。
- shader 编译:`cmd /c "cd /d F:\AdvancedVulkanRendering\shaders && .\compile_shaders.bat"`(必须用 `.\` 前缀,`NoDefaultCurrentDirectoryInExePath=1` 导致 `cmd /c foo.bat` 静默失败)。dxc 路径硬编码 `D:\VulkanSDK\1.3.296.0\Bin\dxc.exe`。
- 应用构建:`cmake --build F:\AdvancedVulkanRendering --config Debug --target AdvancedVulkanRendering`(repo 根即 in-source MSVC 构建目录,产物在 `Bin/`)。
- 运行:工作目录必须在 repo 根(`_rootPath` 解析依赖),`.\Bin\AdvancedVulkanRendering.exe`;DX12 回归用 `.\Bin\AdvancedVulkanRendering.exe --dx12`。
- 所有新增 HLSL 绑定/调用必须包在 `#ifndef DX12_BACKEND` 内;`commonstruct.hlsl` 改动会波及双后端全部 shader,DX12 批处理(`compile_shaders_dx12.bat`)重编译必须零错误。
- `FrameConstants` 布局变动必须同步更新 `Common.h` 的 static_assert;DX12 侧只加默认值,行为不变。
- equirect↔方向约定在 CPU(SH)与 GPU(CS1)间必须逐字一致(见 Task 1 的约定注释)。
- 提交信息遵循仓库风格(`feat:` / `test:` / `docs:`),结尾带 `Co-Authored-By: Claude <noreply@anthropic.com>`。

## Review Focus

1. **缺失/损坏的 hdr 文件** → 应用不得崩溃:绑定 1×1 黑色兜底资源,IBL 数学贡献为 0,日志告警(Task 2 的 Step 5 钉死)。
2. **HDR 像素超过 half 浮点范围(如太阳,>65504)** → CS1 写入前 clamp 到 60000,防止 R16G16B16A16_SFLOAT 存 inf 导致高光爆闪(Task 3 shader 代码内,RenderDoc 检查 mip0 无 inf)。
3. **NdotV=0 / roughness=0 的 LUT 边界** → `gVis` 除法有 `max(...,1e-4)` 保护;Task 5 的 readback 校验必须打印 NaN 计数且为 0。
4. **IBL 生成与 descriptor 写入的 init 顺序** → `initIBL()` 必须在 `init_appl_descriptors()`(GpuScene.cpp:2804)与 `init_deferredlighting_descriptors()`(:2807)之前完成,否则写入的是空句柄(Task 2 Step 4 钉死调用点)。
5. **每帧填充覆盖 ImGui 值** → `GpuScene.cpp:3423-3454` 的 per-frame `frameConstants` 填充不得写 `iblScale/iblSpecularScale`,否则 ImGui 拉条被每帧重置(Task 6 Step 3 钉死:默认值只在初始化处写一次)。

---

### Task 1: SphericalHarmonics 纯函数模块 + 独立测试

**Files:**
- Create: `Src/Include/SphericalHarmonics.h`
- Create: `Src/SphericalHarmonics.cpp`
- Test: `Tests/SphericalHarmonicsTests.cpp`

**Interfaces:**
- Consumes: 无(仅 `<cmath>`,刻意不依赖项目数学库,保证可独立编译)。
- Produces: `struct SH9 { float c[9][3]; };` 与 `SH9 ComputeSH9FromEquirect(const float* rgba, int width, int height);` —— Task 2 的 `initIBL()` 用它从 stb 像素算 SH。

- [ ] **Step 1: 写失败测试**

`Tests/SphericalHarmonicsTests.cpp`(与 `Tests/DX12TextureStreamingPolicyTests.cpp` 同风格:自带 main、expect 计数):

```cpp
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
```

- [ ] **Step 2: 编译运行,确认失败**

用 vswhere 定位 MSVC 环境再编译(与仓库 DX12*Tests.exe 的 ad-hoc 构建方式一致):

```powershell
$vcvars = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -find VC\Auxiliary\Build\vcvars64.bat
cmd /c "`"$vcvars`" >nul 2>&1 && cl /std:c++20 /EHsc /nologo /I F:\AdvancedVulkanRendering\Src\Include F:\AdvancedVulkanRendering\Tests\SphericalHarmonicsTests.cpp F:\AdvancedVulkanRendering\Src\SphericalHarmonics.cpp /Fe:F:\AdvancedVulkanRendering\Bin\SphericalHarmonicsTests.exe /Fo:F:\AdvancedVulkanRendering\Bin\\"
F:\AdvancedVulkanRendering\Bin\SphericalHarmonicsTests.exe
```

Expected: 编译失败(`SphericalHarmonics.h` 不存在)。

- [ ] **Step 3: 实现**

`Src/Include/SphericalHarmonics.h`:

```cpp
#pragma once

// 从 equirectangular HDR 像素积分出 9 个"浓缩形式"球谐系数,
// 形式与 Apple AAPLLightingCommon.h evaluateShCoefficients 的硬编码常数一致:
//   E(n) = c0 + c1*y + c2*z + c3*x + c4*yx + c5*yz + c6*(3z^2-1) + c7*zx + c8*(x^2-y^2)
// 即 A_l(带权重 π, 2π/3, 2π/3, 2π/3, π/4×5)与 Y_lm 归一化常数已折入 c[i]。
struct SH9 {
  float c[9][3];
};

// rgba: width*height*4 的 float 像素(stbi_loadf 的 STBI_rgb_alpha 输出)。
// 方向约定(必须与 shaders/ibl.hlsl 的 DirectionToEquirectUV 逐字一致):
//   u = atan2(dir.x, dir.z) / (2π) + 0.5
//   v = acos(dir.y) / π            (v=0 端 = +Y)
// 逆映射:φ=(u-0.5)*2π,θ=v*π;dir=(sinθ sinφ, cosθ, sinθ cosφ)
SH9 ComputeSH9FromEquirect(const float* rgba, int width, int height);
```

`Src/SphericalHarmonics.cpp`:

```cpp
#include "SphericalHarmonics.h"

#include <cmath>

namespace {
constexpr float kPi = 3.1415926535897932f;

// 实数 SH 基常量(不含方向项):Y00, Y1(y,z,x), Y2(yx, yz, 3z^2-1, zx, x^2-y^2)
constexpr float kY[9] = {
    0.282095f, // Y00
    0.488603f, 0.488603f, 0.488603f, // Y1
    1.092548f, 1.092548f, 0.315392f, 1.092548f, 0.546274f, // Y2
};
// A_l 卷积权重(辐照度):l0=π, l1=2π/3, l2=π/4
constexpr float kA[9] = {
    kPi,
    2.0f * kPi / 3.0f, 2.0f * kPi / 3.0f, 2.0f * kPi / 3.0f,
    kPi / 4.0f, kPi / 4.0f, kPi / 4.0f, kPi / 4.0f, kPi / 4.0f,
};

inline void EvalBasis(float dx, float dy, float dz, float out[9]) {
  out[0] = kY[0];
  out[1] = kY[1] * dy;
  out[2] = kY[2] * dz;
  out[3] = kY[3] * dx;
  out[4] = kY[4] * dy * dx;
  out[5] = kY[5] * dy * dz;
  out[6] = kY[6] * (3.0f * dz * dz - 1.0f);
  out[7] = kY[7] * dz * dx;
  out[8] = kY[8] * (dx * dx - dy * dy);
}
} // namespace

SH9 ComputeSH9FromEquirect(const float* rgba, int width, int height) {
  double proj[9][3] = {};
  double weightSum = 0.0;

  for (int y = 0; y < height; ++y) {
    const float v = (y + 0.5f) / height;
    const float theta = v * kPi; // 0..π,自 +Y 起
    const float sinTheta = std::sin(theta);
    const float cosTheta = std::cos(theta);
    for (int x = 0; x < width; ++x) {
      const float u = (x + 0.5f) / width;
      const float phi = (u - 0.5f) * 2.0f * kPi;
      const float dx = sinTheta * std::sin(phi);
      const float dy = cosTheta;
      const float dz = sinTheta * std::cos(phi);

      float basis[9];
      EvalBasis(dx, dy, dz, basis);

      const float* px = rgba + (size_t)(y * width + x) * 4;
      const double w = sinTheta; // 立体角 ∝ sinθ(常数因子 dθdφ 归一化时约掉)
      weightSum += w;
      for (int i = 0; i < 9; ++i)
        for (int ch = 0; ch < 3; ++ch)
          proj[i][ch] += (double)px[ch] * basis[i] * w;
    }
  }

  // L_lm = 4π * Σ(color·Y·w) / Σw(对恒定环境精确);sh[i] = A_l · L_i
  SH9 sh;
  const double norm = 4.0 * kPi / weightSum;
  for (int i = 0; i < 9; ++i)
    for (int ch = 0; ch < 3; ++ch)
      sh.c[i][ch] = (float)(kA[i] * proj[i][ch] * norm);
  return sh;
}
```

- [ ] **Step 4: 重编译运行,确认通过**

同 Step 2 的两条命令。Expected: `all SH tests passed`,退出码 0。

- [ ] **Step 5: Commit**

```bash
git add Src/Include/SphericalHarmonics.h Src/SphericalHarmonics.cpp Tests/SphericalHarmonicsTests.cpp
git commit -m "feat: CPU SH9 irradiance projection from equirect HDR

Co-Authored-By: Claude <noreply@anthropic.com>"
```

> **勘误(实现后补录,commit 5232332c):** Step 3 的 .cpp 清单有两处缺陷,以实现为准:
> ① 末行须为 `sh.c[i][ch] = (float)(kA[i] * kY[i] * proj[i][ch] * norm);` —— 浓缩形式要求 `c_i = A_l·kY[i]·L_i`(重建 `E(n)=Σ c_i·f_i(n)`,`Y_i=kY[i]·f_i`),原清单漏乘 `kY[i]`,L00 偏大 1/0.282095≈3.5449 倍;
> ② 64×32 网格 sinθ-midpoint 正交在 (x²−y²) 通道残留 1.19e-3 > 1e-3 容差,实现按像素 2×2 子采样(颜色仍取 texel 本身,方向/基/权重在子采样点求值)将残留降至 2.96e-4;接口与方向约定不变。
> 另:3 个新文件需 UTF-8 BOM(MSVC/CP936 会按 GBK 误读无 BOM 文件,与仓库既有文件一致)。

---

### Task 2: HDR 资产 + equirect 上传 + initIBL 接线(含兜底资源)

**Files:**
- Create(下载): `textures/san_giuseppe_bridge_2k.hdr`
- Modify: `Src/Include/GpuScene.h`(成员 + 方法声明)
- Modify: `Src/GpuScene.cpp`(createEquirectTexture / createBlackFallbackIBL / initIBL + 调用点)
- Modify: `Src/CMakeLists.txt`(SOURCES 加 `SphericalHarmonics.cpp`;`IBLGenerator.cpp` 在 Task 3 加)

**Interfaces:**
- Consumes: Task 1 的 `ComputeSH9FromEquirect`;既有 `GpuScene::createBuffer`(GpuScene.h:933)、`VulkanDevice::transitionImageLayout`、`copyBufferToImage`(VulkanSetup.h:466-492)。
- Produces: GpuScene 成员(后续任务直接用):`_equirectView`, `_iblEnvCube/_iblEnvCubeMemory/_iblEnvCubeView`, `_iblDfgLut/_iblDfgLutMemory/_iblDfgLutView`, `_iblSHBuffer/_iblSHMemory`, `_iblAvailable`(bool)。**任何路径结束后这些成员都是有效句柄**(缺 hdr 走 1×1 黑色兜底)。

- [ ] **Step 1: 下载 HDR 资产**

```powershell
curl -L -o F:\AdvancedVulkanRendering\textures\san_giuseppe_bridge_2k.hdr https://dl.polyhaven.org/file/ph-assets/HDRIs/hdr/2k/san_giuseppe_bridge_2k.hdr
```

验证:文件 ~11MB,前 11 字节为 `#?RADIANCE`。若下载失败,停止并告知用户手动放置该文件(Polyhaven 任意 2k HDR 均可,文件名保持一致或同步改代码)。

- [ ] **Step 2: GpuScene.h 加成员与声明**

在 GpuScene 类私有区(如 `_linearClampSampler` 声明附近)添加:

```cpp
  // ---- IBL (runtime-generated, Vulkan only) ----
  VkImage _equirectImage = VK_NULL_HANDLE;
  VkDeviceMemory _equirectMemory = VK_NULL_HANDLE;
  VkImageView _equirectView = VK_NULL_HANDLE;

  VkImage _iblEnvCube = VK_NULL_HANDLE;
  VkDeviceMemory _iblEnvCubeMemory = VK_NULL_HANDLE;
  VkImageView _iblEnvCubeView = VK_NULL_HANDLE; // cube view, 9 mips

  VkImage _iblDfgLut = VK_NULL_HANDLE;
  VkDeviceMemory _iblDfgLutMemory = VK_NULL_HANDLE;
  VkImageView _iblDfgLutView = VK_NULL_HANDLE;

  VkBuffer _iblSHBuffer = VK_NULL_HANDLE;
  VkDeviceMemory _iblSHMemory = VK_NULL_HANDLE;

  bool _iblAvailable = false;

  void initIBL();
  bool createEquirectTexture(const std::filesystem::path& hdrPath);
  void createBlackFallbackIBL();
```

- [ ] **Step 3: GpuScene.cpp 实现三个函数**

文件顶部 include 区加:

```cpp
#include "SphericalHarmonics.h"
#include <spdlog/spdlog.h>
```

实现(放 `createTexture` 定义之后):

```cpp
// 加载 .hdr equirect 并上传为 RGBA32F 2D 纹理;失败返回 false。
bool GpuScene::createEquirectTexture(const std::filesystem::path& hdrPath) {
  int w = 0, h = 0, ch = 0;
  float* pixels = stbi_loadf(hdrPath.generic_string().c_str(), &w, &h, &ch, STBI_rgb_alpha);
  if (!pixels || w <= 0 || h <= 0) {
    spdlog::warn("IBL: failed to load {}, IBL disabled", hdrPath.generic_string());
    return false;
  }
  spdlog::info("IBL: loaded equirect {} ({}x{})", hdrPath.filename().generic_string(), w, h);

  const VkDeviceSize imageSize = (VkDeviceSize)w * h * 4 * sizeof(float);
  VkBuffer staging; VkDeviceMemory stagingMem;
  createBuffer(imageSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
               staging, stagingMem);
  void* data;
  vkMapMemory(device.getLogicalDevice(), stagingMem, 0, imageSize, 0, &data);
  memcpy(data, pixels, imageSize);
  vkUnmapMemory(device.getLogicalDevice(), stagingMem);
  stbi_image_free(pixels);

  VkImageCreateInfo imageInfo{};
  imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  imageInfo.imageType = VK_IMAGE_TYPE_2D;
  imageInfo.extent = {(uint32_t)w, (uint32_t)h, 1};
  imageInfo.mipLevels = 1;
  imageInfo.arrayLayers = 1;
  imageInfo.format = VK_FORMAT_R32G32B32A32_SFLOAT;
  imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
  imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
  imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
  vkCreateImage(device.getLogicalDevice(), &imageInfo, nullptr, &_equirectImage);

  VkMemoryRequirements memReq;
  vkGetImageMemoryRequirements(device.getLogicalDevice(), _equirectImage, &memReq);
  VkMemoryAllocateInfo allocInfo{};
  allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  allocInfo.allocationSize = memReq.size;
  allocInfo.memoryTypeIndex = device.findMemoryType(memReq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  vkAllocateMemory(device.getLogicalDevice(), &allocInfo, nullptr, &_equirectMemory);
  vkBindImageMemory(device.getLogicalDevice(), _equirectImage, _equirectMemory, 0);

  device.transitionImageLayout(_equirectImage, imageInfo.format,
                               VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
  device.copyBufferToImage(staging, _equirectImage, (uint32_t)w, (uint32_t)h);
  device.transitionImageLayout(_equirectImage, imageInfo.format,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  vkDestroyBuffer(device.getLogicalDevice(), staging, nullptr);
  vkFreeMemory(device.getLogicalDevice(), stagingMem, nullptr);

  VkImageViewCreateInfo viewInfo{};
  viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  viewInfo.image = _equirectImage;
  viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
  viewInfo.format = imageInfo.format;
  viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  vkCreateImageView(device.getLogicalDevice(), &viewInfo, nullptr, &_equirectView);
  return true;
}

// 1×1 黑色兜底:cube(6 层)+ RG16F LUT + 全零 SH UBO。
// 黑 cube + 零 SH 使 IBL 数学贡献恒为 0,无需 shader 特判。
void GpuScene::createBlackFallbackIBL() {
  // --- 1x1x6 black cube ---
  VkImageCreateInfo ci{};
  ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  ci.imageType = VK_IMAGE_TYPE_2D;
  ci.extent = {1, 1, 1};
  ci.mipLevels = 1;
  ci.arrayLayers = 6;
  ci.format = VK_FORMAT_R16G16B16A16_SFLOAT;
  ci.tiling = VK_IMAGE_TILING_OPTIMAL;
  ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  ci.samples = VK_SAMPLE_COUNT_1_BIT;
  ci.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
  vkCreateImage(device.getLogicalDevice(), &ci, nullptr, &_iblEnvCube);
  VkMemoryRequirements mr;
  vkGetImageMemoryRequirements(device.getLogicalDevice(), _iblEnvCube, &mr);
  VkMemoryAllocateInfo ai{};
  ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  ai.allocationSize = mr.size;
  ai.memoryTypeIndex = device.findMemoryType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  vkAllocateMemory(device.getLogicalDevice(), &ai, nullptr, &_iblEnvCubeMemory);
  vkBindImageMemory(device.getLogicalDevice(), _iblEnvCube, _iblEnvCubeMemory, 0);

  // --- 1x1 RG16F LUT ---
  ci.arrayLayers = 1;
  ci.flags = 0;
  ci.format = VK_FORMAT_R16G16_SFLOAT;
  vkCreateImage(device.getLogicalDevice(), &ci, nullptr, &_iblDfgLut);
  vkGetImageMemoryRequirements(device.getLogicalDevice(), _iblDfgLut, &mr);
  ai.allocationSize = mr.size;
  ai.memoryTypeIndex = device.findMemoryType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  vkAllocateMemory(device.getLogicalDevice(), &ai, nullptr, &_iblDfgLutMemory);
  vkBindImageMemory(device.getLogicalDevice(), _iblDfgLut, _iblDfgLutMemory, 0);

  // clear both to black via one-shot command buffer
  VkCommandBuffer cmd = device.beginSingleTimeCommands();
  VkImageMemoryBarrier barrier{};
  barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.srcAccessMask = 0;
  barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  VkImageMemoryBarrier barriers[2] = {barrier, barrier};
  barriers[0].image = _iblEnvCube;
  barriers[0].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 6};
  barriers[1].image = _iblDfgLut;
  barriers[1].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                       0, 0, nullptr, 0, nullptr, 2, barriers);
  VkClearColorValue black = {{0.f, 0.f, 0.f, 1.f}};
  VkImageSubresourceRange rangeCube = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 6};
  VkImageSubresourceRange rangeLut  = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  vkCmdClearColorImage(cmd, _iblEnvCube, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &rangeCube);
  vkCmdClearColorImage(cmd, _iblDfgLut, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &rangeLut);
  for (auto& b : barriers) {
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
  }
  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                       0, 0, nullptr, 0, nullptr, 2, barriers);
  device.endSingleTimeCommands(cmd);

  VkImageViewCreateInfo vi{};
  vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  vi.viewType = VK_IMAGE_VIEW_TYPE_CUBE;
  vi.format = VK_FORMAT_R16G16B16A16_SFLOAT;
  vi.image = _iblEnvCube;
  vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 6};
  vkCreateImageView(device.getLogicalDevice(), &vi, nullptr, &_iblEnvCubeView);
  vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
  vi.format = VK_FORMAT_R16G16_SFLOAT;
  vi.image = _iblDfgLut;
  vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  vkCreateImageView(device.getLogicalDevice(), &vi, nullptr, &_iblDfgLutView);

  // zero SH UBO(9 * float4)
  createBuffer(9 * 16, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
               _iblSHBuffer, _iblSHMemory);
  void* p;
  vkMapMemory(device.getLogicalDevice(), _iblSHMemory, 0, 9 * 16, 0, &p);
  memset(p, 0, 9 * 16);
  vkUnmapMemory(device.getLogicalDevice(), _iblSHMemory);
}

void GpuScene::initIBL() {
  const auto hdrPath = _rootPath / "textures" / "san_giuseppe_bridge_2k.hdr";

  int w = 0, h = 0, ch = 0;
  float* pixels = stbi_loadf(hdrPath.generic_string().c_str(), &w, &h, &ch, STBI_rgb_alpha);
  if (!pixels) {
    spdlog::warn("IBL: {} not found or unreadable, using black fallback", hdrPath.generic_string());
    createBlackFallbackIBL();
    _iblAvailable = false;
    return;
  }

  SH9 sh = ComputeSH9FromEquirect(pixels, w, h);
  stbi_image_free(pixels); // 注意:createEquirectTexture 会重新加载;为省事让它自己读。

  if (!createEquirectTexture(hdrPath)) {
    createBlackFallbackIBL();
    _iblAvailable = false;
    return;
  }

  // SH UBO(float4[9],w 分量置 0)
  createBuffer(9 * 16, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
               _iblSHBuffer, _iblSHMemory);
  float shGpu[9][4] = {};
  for (int i = 0; i < 9; ++i)
    for (int c = 0; c < 3; ++c) shGpu[i][c] = sh.c[i][c];
  void* p;
  vkMapMemory(device.getLogicalDevice(), _iblSHMemory, 0, sizeof(shGpu), 0, &p);
  memcpy(p, shGpu, sizeof(shGpu));
  vkUnmapMemory(device.getLogicalDevice(), _iblSHMemory);

  _iblAvailable = true; // envCube/dfgLut 由 Task 3-5 的 IBLGenerator 填充
}
```

- [ ] **Step 4: 钉死 init 调用点(Review Focus #4)**

GpuScene.cpp:2795 `createLinearClampSampler();` 之后、:2801 `init_GlobaldescriptorSet();` 之前插入:

```cpp
  initIBL(); // 必须在 init_appl_descriptors/init_deferredlighting_descriptors 之前完成
```

- [ ] **Step 5: CMakeLists + 构建运行(Review Focus #1)**

`Src/CMakeLists.txt` 的 `set(SOURCES ...)` 加一行:

```cmake
    ${CMAKE_CURRENT_LIST_DIR}/SphericalHarmonics.cpp
```

构建:`cmake --build F:\AdvancedVulkanRendering --config Debug --target AdvancedVulkanRendering`
运行:`cd F:\AdvancedVulkanRendering && .\Bin\AdvancedVulkanRendering.exe`
Expected: 日志出现 `IBL: loaded equirect san_giuseppe_bridge_2k.hdr (2048x1024)`,应用正常渲染(画面尚无变化)。
再把 hdr 文件改名一次重跑:应出现 `IBL: ... not found or unreadable, using black fallback` 且应用不崩溃(Review Focus #1)。改回文件名。

- [ ] **Step 6: Commit**

```bash
git add Src/Include/GpuScene.h Src/GpuScene.cpp Src/CMakeLists.txt
git commit -m "feat: load HDR equirect and wire initIBL with black fallback

Co-Authored-By: Claude <noreply@anthropic.com>"
```

(注意:`textures/*.hdr` 不入库 —— 确认 `.gitignore` 已覆盖 `textures/` 或本次不 add 它。)

---

### Task 3: IBLGenerator + CS1(equirect → cube mip0)

**Files:**
- Create: `shaders/ibl.hlsl`
- Create: `Src/Include/IBLGenerator.h`
- Create: `Src/IBLGenerator.cpp`
- Modify: `shaders/compile_shaders.bat`
- Modify: `Src/CMakeLists.txt`(SOURCES 加 `IBLGenerator.cpp`)
- Modify: `Src/GpuScene.cpp`(initIBL 尾部调用 generator)

**Interfaces:**
- Consumes: Task 2 的 `_equirectView`、`device`(VulkanDevice);`readFile`(Common.h:47)。
- Produces: `struct IBLResources`(见下)与 `class IBLGenerator { public: IBLResources generate(VulkanDevice&, VkImageView equirectView, const std::filesystem::path& rootPath); };`。Task 4/5 在同一 `generate()` 内追加 dispatch,产出物经成员赋值给 GpuScene:`_iblEnvCube*`, `_iblDfgLut*`, `_iblSampler`。

方向约定注释必须出现在 `ibl.hlsl` 顶部且与 `SphericalHarmonics.h` 的注释逐字一致(Global Constraints)。

- [ ] **Step 1: 写 shaders/ibl.hlsl(CS1 + 共享 helper;CS2/CS3 后续任务追加)**

```hlsl
// ibl.hlsl — IBL 运行时生成内核(Vulkan only,DX12 批处理不编译本文件)。
//
// equirect 方向约定(必须与 Src/SphericalHarmonics.h 注释逐字一致):
//   u = atan2(dir.x, dir.z) / (2π) + 0.5
//   v = acos(dir.y) / π            (v=0 端 = +Y)
// 逆映射:φ=(u-0.5)*2π,θ=v*π;dir=(sinθ sinφ, cosθ, sinθ cosφ)
#include "shadercompat.hlsl"

static const float IBL_PI = 3.1415926535897932f;

float2 DirectionToEquirectUV(float3 d)
{
    float phi = atan2(d.x, d.z);
    float theta = acos(clamp(d.y, -1.0, 1.0));
    return float2(phi / (2.0 * IBL_PI) + 0.5, theta / IBL_PI);
}

// Vulkan/GL cube 面约定:face == array layer(0:+X 1:-X 2:+Y 3:-Y 4:+Z 5:-Z)。
// uv01: 面内 [0,1] 坐标,y 向下(与 storage 写坐标一致)。
float3 CubeFaceDirection(uint face, float2 uv01)
{
    float sc = uv01.x * 2.0 - 1.0;
    float tc = uv01.y * 2.0 - 1.0;
    switch (face)
    {
    case 0:  return normalize(float3( 1.0, -tc, -sc));
    case 1:  return normalize(float3(-1.0, -tc,  sc));
    case 2:  return normalize(float3( sc,  1.0,  tc));
    case 3:  return normalize(float3( sc, -1.0, -tc));
    case 4:  return normalize(float3( sc, -tc,  1.0));
    default: return normalize(float3(-sc, -tc, -1.0));
    }
}

VK_BINDING(0,0) Texture2D<float4> equirectTex;
VK_BINDING(1,0) SamplerState iblLinearSampler;
VK_BINDING(2,0) RWTexture2DArray<float4> outputFace;

struct EquirectToCubeParams { uint mipSize; uint _pad0; uint _pad1; uint _pad2; };
DECLARE_PUSH_CONSTANTS(EquirectToCubeParams, epc, 0);

[numthreads(8, 8, 1)]
void EquirectToCubeCS(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= epc.mipSize || tid.y >= epc.mipSize || tid.z >= 6) return;
    float2 uv = (tid.xy + 0.5) / (float)epc.mipSize;
    float3 dir = CubeFaceDirection(tid.z, uv);
    float3 color = equirectTex.SampleLevel(iblLinearSampler, DirectionToEquirectUV(dir), 0).rgb;
    // R16G16B16A16_SFLOAT 上限 65504;超高动态像素(太阳)clamp 防 inf(Review Focus #2)
    outputFace[tid] = float4(min(color, 60000.0), 1.0);
}
```

- [ ] **Step 2: compile_shaders.bat 追加 + 编译**

```bat
REM IBL runtime generation kernels
D:\VulkanSDK\1.3.296.0\Bin\dxc.exe -spirv -T cs_6_2 ibl.hlsl -E EquirectToCubeCS -Fo ibl_equirect.cs.spv
```

运行 `cmd /c "cd /d F:\AdvancedVulkanRendering\shaders && .\compile_shaders.bat"`,确认 `ibl_equirect.cs.spv` 生成且无错误(其它 shader 重编译也应零错误)。

- [ ] **Step 3: IBLGenerator.h/.cpp**

`Src/Include/IBLGenerator.h`:

```cpp
#pragma once

#include <vulkan/vulkan.h>
#include <filesystem>

class VulkanDevice;

// IBLGenerator 的输出资源(所有权归调用方,负责最终销毁)。
struct IBLResources {
  VkImage envCube = VK_NULL_HANDLE;        // 256x256x6, 9 mips, R16G16B16A16_SFLOAT
  VkDeviceMemory envCubeMemory = VK_NULL_HANDLE;
  VkImageView envCubeView = VK_NULL_HANDLE; // VK_IMAGE_VIEW_TYPE_CUBE, 全 mip
  VkImage dfgLut = VK_NULL_HANDLE;          // 256x256 RG16F
  VkDeviceMemory dfgLutMemory = VK_NULL_HANDLE;
  VkImageView dfgLutView = VK_NULL_HANDLE;
  VkSampler sampler = VK_NULL_HANDLE;       // 三线性 + clamp,生成与光照共用
};

// 一次性 compute 链:equirect→cube mip0(Task 3)→ GGX 预过滤 mip1-8(Task 4)
// → DFG LUT(Task 5)。在单个 one-shot command buffer 内完成全部 dispatch 与
// layout 转换,返回时所有输出已处 SHADER_READ_ONLY_OPTIMAL。
class IBLGenerator {
public:
  static constexpr uint32_t kEnvMapSize = 256;
  static constexpr uint32_t kEnvMipCount = 9; // 256..1,lod = roughness * 8

  IBLResources generate(VulkanDevice& device, VkImageView equirectView,
                        const std::filesystem::path& rootPath);
};
```

`Src/IBLGenerator.cpp`(本任务实现:资源创建 + CS1 + 最终 transition;预过滤/LUT 留注释锚点):

```cpp
#include "IBLGenerator.h"

#include "Common.h"       // readFile
#include "VulkanSetup.h"  // VulkanDevice

#include <array>
#include <cstring>
#include <stdexcept>

namespace {

VkShaderModule loadModule(VkDevice dev, const std::filesystem::path& path) {
  std::vector<char> code = readFile(path.generic_string());
  VkShaderModuleCreateInfo ci{};
  ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
  ci.codeSize = code.size();
  ci.pCode = reinterpret_cast<const uint32_t*>(code.data());
  VkShaderModule m;
  if (vkCreateShaderModule(dev, &ci, nullptr, &m) != VK_SUCCESS)
    throw std::runtime_error("IBL: failed to create shader module");
  return m;
}

void createImage2D(VulkanDevice& device, VkFormat format, uint32_t w, uint32_t h,
                   uint32_t mips, uint32_t layers, VkImageCreateFlags flags,
                   VkImageUsageFlags usage, VkImage& image, VkDeviceMemory& memory) {
  VkImageCreateInfo ci{};
  ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  ci.imageType = VK_IMAGE_TYPE_2D;
  ci.extent = {w, h, 1};
  ci.mipLevels = mips;
  ci.arrayLayers = layers;
  ci.format = format;
  ci.tiling = VK_IMAGE_TILING_OPTIMAL;
  ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  ci.usage = usage;
  ci.samples = VK_SAMPLE_COUNT_1_BIT;
  ci.flags = flags;
  if (vkCreateImage(device.getLogicalDevice(), &ci, nullptr, &image) != VK_SUCCESS)
    throw std::runtime_error("IBL: failed to create image");
  VkMemoryRequirements mr;
  vkGetImageMemoryRequirements(device.getLogicalDevice(), image, &mr);
  VkMemoryAllocateInfo ai{};
  ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  ai.allocationSize = mr.size;
  ai.memoryTypeIndex = device.findMemoryType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  if (vkAllocateMemory(device.getLogicalDevice(), &ai, nullptr, &memory) != VK_SUCCESS)
    throw std::runtime_error("IBL: failed to allocate image memory");
  vkBindImageMemory(device.getLogicalDevice(), image, memory, 0);
}

void transitionRange(VkCommandBuffer cmd, VkImage image,
                     VkImageLayout oldLayout, VkImageLayout newLayout,
                     uint32_t baseMip, uint32_t mipCount, uint32_t layerCount,
                     VkAccessFlags srcAccess, VkAccessFlags dstAccess,
                     VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage) {
  VkImageMemoryBarrier b{};
  b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  b.oldLayout = oldLayout;
  b.newLayout = newLayout;
  b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.image = image;
  b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, baseMip, mipCount, 0, layerCount};
  b.srcAccessMask = srcAccess;
  b.dstAccessMask = dstAccess;
  vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &b);
}

} // namespace

IBLResources IBLGenerator::generate(VulkanDevice& device, VkImageView equirectView,
                                    const std::filesystem::path& rootPath) {
  VkDevice dev = device.getLogicalDevice();
  IBLResources out;

  // --- sampler(生成与光照共用:三线性 + clamp) ---
  VkSamplerCreateInfo si{};
  si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
  si.magFilter = VK_FILTER_LINEAR;
  si.minFilter = VK_FILTER_LINEAR;
  si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
  si.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  si.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  si.maxLod = (float)kEnvMipCount;
  if (vkCreateSampler(dev, &si, nullptr, &out.sampler) != VK_SUCCESS)
    throw std::runtime_error("IBL: failed to create sampler");

  // --- 目标资源 ---
  createImage2D(device, VK_FORMAT_R16G16B16A16_SFLOAT, kEnvMapSize, kEnvMapSize,
                kEnvMipCount, 6, VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT,
                VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT,
                out.envCube, out.envCubeMemory);

  // mip0 的 2D-array storage view(CS1 输出)
  VkImageViewCreateInfo vi{};
  vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  vi.image = out.envCube;
  vi.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
  vi.format = VK_FORMAT_R16G16B16A16_SFLOAT;
  vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 6};
  VkImageView mip0StorageView;
  vkCreateImageView(dev, &vi, nullptr, &mip0StorageView);

  // cube 采样 view(全 mip;Task 4 预过滤读 mip0、光照 SampleLevel 用)
  vi.viewType = VK_IMAGE_VIEW_TYPE_CUBE;
  vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, kEnvMipCount, 0, 6};
  vkCreateImageView(dev, &vi, nullptr, &out.envCubeView);

  // --- CS1 pipeline ---
  VkDescriptorSetLayoutBinding cs1Bindings[3] = {};
  cs1Bindings[0] = {0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
  cs1Bindings[1] = {1, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
  cs1Bindings[2] = {2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
  VkDescriptorSetLayoutCreateInfo slci{};
  slci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  slci.bindingCount = 3;
  slci.pBindings = cs1Bindings;
  VkDescriptorSetLayout cs1SetLayout;
  vkCreateDescriptorSetLayout(dev, &slci, nullptr, &cs1SetLayout);

  VkPushConstantRange pcRange{VK_SHADER_STAGE_COMPUTE_BIT, 0, 16};
  VkPipelineLayoutCreateInfo plci{};
  plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  plci.setLayoutCount = 1;
  plci.pSetLayouts = &cs1SetLayout;
  plci.pushConstantRangeCount = 1;
  plci.pPushConstantRanges = &pcRange;
  VkPipelineLayout cs1Layout;
  vkCreatePipelineLayout(dev, &plci, nullptr, &cs1Layout);

  VkShaderModule cs1Module = loadModule(dev, rootPath / "shaders" / "ibl_equirect.cs.spv");
  VkComputePipelineCreateInfo cpci{};
  cpci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
  cpci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                VK_SHADER_STAGE_COMPUTE_BIT, cs1Module, "EquirectToCubeCS", nullptr};
  cpci.layout = cs1Layout;
  VkPipeline cs1Pipeline;
  if (vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpci, nullptr, &cs1Pipeline) != VK_SUCCESS)
    throw std::runtime_error("IBL: failed to create equirect-to-cube pipeline");

  // --- descriptor pool/set(生成专用,一次性) ---
  VkDescriptorPoolSize poolSizes[] = {
      {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 4},
      {VK_DESCRIPTOR_TYPE_SAMPLER, 4},
      {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 12}, // Task 4 会用到 9 个 mip view
  };
  VkDescriptorPoolCreateInfo dpci{};
  dpci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  dpci.maxSets = 12;
  dpci.poolSizeCount = 3;
  dpci.pPoolSizes = poolSizes;
  VkDescriptorPool pool;
  vkCreateDescriptorPool(dev, &dpci, nullptr, &pool);

  VkDescriptorSetAllocateInfo dsai{};
  dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  dsai.descriptorPool = pool;
  dsai.descriptorSetCount = 1;
  dsai.pSetLayouts = &cs1SetLayout;
  VkDescriptorSet cs1Set;
  vkAllocateDescriptorSets(dev, &dsai, &cs1Set);

  // 注意:VkDescriptorImageInfo 字段序为 (sampler, imageView, imageLayout),
  // 与 GpuScene.cpp:1383 一致用成员赋值,避免 brace 顺序错误。
  VkDescriptorImageInfo eqInfo{};
  eqInfo.imageView = equirectView;
  eqInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  VkDescriptorImageInfo sampInfo{};
  sampInfo.sampler = out.sampler;
  VkDescriptorImageInfo outInfo{};
  outInfo.imageView = mip0StorageView;
  outInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
  VkWriteDescriptorSet writes[3] = {};
  writes[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, cs1Set, 0, 0, 1,
               VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &eqInfo, nullptr, nullptr};
  writes[1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, cs1Set, 1, 0, 1,
               VK_DESCRIPTOR_TYPE_SAMPLER, &sampInfo, nullptr, nullptr};
  writes[2] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, cs1Set, 2, 0, 1,
               VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &outInfo, nullptr, nullptr};
  vkUpdateDescriptorSets(dev, 3, writes, 0, nullptr);

  // --- 录制 one-shot 命令 ---
  VkCommandBuffer cmd = device.beginSingleTimeCommands();

  // 全 mip → GENERAL(生成期统一;Task 4 写 mip1-8 无需再转)
  transitionRange(cmd, out.envCube, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                  0, kEnvMipCount, 6,
                  0, VK_ACCESS_SHADER_WRITE_BIT,
                  VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, cs1Pipeline);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, cs1Layout, 0, 1, &cs1Set, 0, nullptr);
  uint32_t pc[4] = {kEnvMapSize, 0, 0, 0};
  vkCmdPushConstants(cmd, cs1Layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 16, pc);
  vkCmdDispatch(cmd, kEnvMapSize / 8, kEnvMapSize / 8, 6);

  // mip0: STORAGE_WRITE → SHADER_READ(Task 4 的预过滤从 mip0 采样)
  transitionRange(cmd, out.envCube, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                  0, 1, 6,
                  VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                  VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

  // === Task 4 锚点:在此插入 PrefilterSpecularCS 的 mip1-8 dispatch ===
  // === Task 5 锚点:在此插入 DfgLutCS dispatch ===

  // 最终:全 mip → SHADER_READ_ONLY
  transitionRange(cmd, out.envCube, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                  0, kEnvMipCount, 6,
                  VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_SHADER_READ_BIT,
                  VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);

  device.endSingleTimeCommands(cmd);

  // 清理生成期临时对象(输出资源保留)
  vkDestroyImageView(dev, mip0StorageView, nullptr);
  vkDestroyShaderModule(dev, cs1Module, nullptr);
  vkDestroyPipeline(dev, cs1Pipeline, nullptr);
  vkDestroyPipelineLayout(dev, cs1Layout, nullptr);
  vkDestroyDescriptorSetLayout(dev, cs1SetLayout, nullptr);
  vkDestroyDescriptorPool(dev, pool, nullptr);

  return out;
}
```

- [ ] **Step 4: 接线 initIBL + CMakeLists**

`Src/CMakeLists.txt` SOURCES 加 `${CMAKE_CURRENT_LIST_DIR}/IBLGenerator.cpp`。
`Src/GpuScene.cpp` include 加 `#include "IBLGenerator.h"`;`initIBL()` 尾部(`_iblAvailable = true;` 之前)加:

```cpp
  // 生成预过滤 env cube 与 DFG LUT(Task 3-5 逐步补全 generate 内部)
  IBLResources ibl = IBLGenerator().generate(device, _equirectView, _rootPath);
  _iblEnvCube = ibl.envCube;
  _iblEnvCubeMemory = ibl.envCubeMemory;
  _iblEnvCubeView = ibl.envCubeView;
  _iblDfgLut = ibl.dfgLut;
  _iblDfgLutMemory = ibl.dfgLutMemory;
  _iblDfgLutView = ibl.dfgLutView;
  _iblSampler = ibl.sampler;
```

`Src/Include/GpuScene.h` 成员区补 `VkSampler _iblSampler = VK_NULL_HANDLE;`。

注意:此时 `_iblDfgLut/_iblDfgLutView` 仍是 VK_NULL_HANDLE(Task 5 才产出)——descriptor 写入在 Task 7 才接入,中间版本不会在渲染中使用它们,安全。

- [ ] **Step 5: 构建运行 + RenderDoc 验证 mip0**

构建:`cmake --build F:\AdvancedVulkanRendering --config Debug --target AdvancedVulkanRendering`
用 RenderDoc 抓一帧(renderdoccmd 或 UI),用 MCP 工具检查:
`mcp__renderdoc__list_captures` → `open_capture` → `get_draw_calls`(应能搜到 `EquirectToCubeCS` dispatch)→ `find_draws_by_texture` 找 env cube → `get_texture_data`(mip 0, slice 0-5):6 个面拼起来应呈现与 equirect 一致的场景(桥),无全黑/全白/inf。

- [ ] **Step 6: Commit**

```bash
git add shaders/ibl.hlsl shaders/compile_shaders.bat Src/Include/IBLGenerator.h Src/IBLGenerator.cpp Src/CMakeLists.txt Src/GpuScene.cpp Src/Include/GpuScene.h
git commit -m "feat: IBL generator with equirect-to-cubemap compute pass

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 4: CS2 GGX 预过滤(mip1-8)

**Files:**
- Modify: `shaders/ibl.hlsl`(追加 Hammersley/ImportanceSampleGGX/PrefilterSpecularCS)
- Modify: `shaders/compile_shaders.bat`
- Modify: `Src/IBLGenerator.cpp`(预过滤 pipeline + 8 次 dispatch)

**Interfaces:**
- Consumes: Task 3 的 `outputFace`/`iblLinearSampler` 绑定声明与 `CubeFaceDirection`;Task 3 锚点位置。
- Produces: `shaders/ibl_prefilter.cs.spv`;`generate()` 返回的 env cube 9 mip 全部有效(后续 Task 7 采样)。

- [ ] **Step 1: ibl.hlsl 追加(放 EquirectToCubeCS 之后)**

```hlsl
// ---- 预过滤(GGX 重要性采样,split-sum 第一步;Epic/Karis) ----
float IBL_RadicalInverse(uint bits)
{
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return float(bits) * 2.3283064365386963e-10;
}

float2 IBL_Hammersley(uint i, uint n)
{
    return float2(float(i) / float(n), IBL_RadicalInverse(i));
}

float3 IBL_ImportanceSampleGGX(float2 xi, float3 n, float roughness)
{
    float a = roughness * roughness;
    float phi = 2.0 * IBL_PI * xi.x;
    float cosTheta = sqrt((1.0 - xi.y) / (1.0 + (a * a - 1.0) * xi.y));
    float sinTheta = sqrt(1.0 - cosTheta * cosTheta);
    float3 h = float3(cos(phi) * sinTheta, sin(phi) * sinTheta, cosTheta);
    float3 up = abs(n.z) < 0.999 ? float3(0, 0, 1) : float3(1, 0, 0);
    float3 tx = normalize(cross(up, n));
    float3 ty = cross(n, tx);
    return normalize(tx * h.x + ty * h.y + n * h.z);
}

VK_BINDING(3,0) TextureCube prefilterInput;

struct PrefilterParams { float roughness; uint mipSize; uint _pad0; uint _pad1; };
DECLARE_PUSH_CONSTANTS(PrefilterParams, ppc, 0);

[numthreads(8, 8, 1)]
void PrefilterSpecularCS(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= ppc.mipSize || tid.y >= ppc.mipSize || tid.z >= 6) return;
    float2 uv = (tid.xy + 0.5) / (float)ppc.mipSize;
    float3 N = CubeFaceDirection(tid.z, uv);
    float3 V = N;

    const uint SAMPLE_COUNT = 128;
    float totalWeight = 0.0;
    float3 prefiltered = 0.0;
    for (uint i = 0; i < SAMPLE_COUNT; ++i)
    {
        float2 xi = IBL_Hammersley(i, SAMPLE_COUNT);
        float3 H = IBL_ImportanceSampleGGX(xi, N, ppc.roughness);
        float3 L = 2.0 * dot(V, H) * H - V;
        float ndl = saturate(dot(N, L));
        if (ndl > 0.0)
        {
            prefiltered += prefilterInput.SampleLevel(iblLinearSampler, L, 0).rgb * ndl;
            totalWeight += ndl;
        }
    }
    outputFace[tid] = float4(prefiltered / max(totalWeight, 1e-4), 1.0);
}
```

注意:CS2 复用 binding 1(sampler)/2(output storage),输入 cube 走 binding 3(与 CS1 不同布局,见 Step 3 的 set layout)。

- [ ] **Step 2: compile_shaders.bat 追加 + 编译**

```bat
D:\VulkanSDK\1.3.296.0\Bin\dxc.exe -spirv -T cs_6_2 ibl.hlsl -E PrefilterSpecularCS -Fo ibl_prefilter.cs.spv
```

运行编译命令,确认 `ibl_prefilter.cs.spv` 生成。

- [ ] **Step 3: IBLGenerator.cpp 加预过滤**

在 `generate()` 中 CS1 pipeline 创建之后准备 CS2 pipeline。注意:CS1 与 CS2 的 binding 声明在同一 shader 文件的不同 entry 中,dxc/SPIR-V 按 entry 只生成各自实际引用的 binding,所以 **CS2 的 set layout 只含 CS2 引用的 binding(1, 2, 3),无需 binding 0**:

```cpp
  VkDescriptorSetLayoutBinding cs2Bindings[3] = {};
  cs2Bindings[0] = {3, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
  cs2Bindings[1] = {1, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
  cs2Bindings[2] = {2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
  VkDescriptorSetLayoutCreateInfo slci2{};
  slci2.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  slci2.bindingCount = 3;
  slci2.pBindings = cs2Bindings;
  VkDescriptorSetLayout cs2SetLayout;
  vkCreateDescriptorSetLayout(dev, &slci2, nullptr, &cs2SetLayout);

  VkPipelineLayoutCreateInfo plci2{};
  plci2.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  plci2.setLayoutCount = 1;
  plci2.pSetLayouts = &cs2SetLayout;
  plci2.pushConstantRangeCount = 1;
  plci2.pPushConstantRanges = &pcRange;
  VkPipelineLayout cs2Layout;
  vkCreatePipelineLayout(dev, &plci2, nullptr, &cs2Layout);

  VkShaderModule cs2Module = loadModule(dev, rootPath / "shaders" / "ibl_prefilter.cs.spv");
  VkComputePipelineCreateInfo cpci2{};
  cpci2.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
  cpci2.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                 VK_SHADER_STAGE_COMPUTE_BIT, cs2Module, "PrefilterSpecularCS", nullptr};
  cpci2.layout = cs2Layout;
  VkPipeline cs2Pipeline;
  if (vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpci2, nullptr, &cs2Pipeline) != VK_SUCCESS)
    throw std::runtime_error("IBL: failed to create prefilter pipeline");
```

然后为 mip1-8 建 storage view + descriptor set 并 dispatch(插在 Task 3 锚点处)。**生命周期规则:view/set 的销毁必须在 `endSingleTimeCommands` 之后**(命令录制时引用的资源在执行完成前不得销毁),所以 mipView 先存 vector 统一收尾:

```cpp
  std::vector<VkImageView> mipViews; // endSingleTimeCommands 之后统一销毁
  VkDescriptorImageInfo cubeInfo{};
  cubeInfo.imageView = out.envCubeView;
  cubeInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL; // 生成期 GENERAL 下采样合法
  VkDescriptorImageInfo sampInfo2{};
  sampInfo2.sampler = out.sampler;

  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, cs2Pipeline);
  for (uint32_t mip = 1; mip < kEnvMipCount; ++mip) {
    const uint32_t mipSize = kEnvMapSize >> mip;

    VkImageViewCreateInfo mvi{};
    mvi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    mvi.image = out.envCube;
    mvi.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    mvi.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    mvi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, mip, 1, 0, 6};
    VkImageView mipView;
    vkCreateImageView(dev, &mvi, nullptr, &mipView);
    mipViews.push_back(mipView);

    VkDescriptorSetAllocateInfo dsai2{};
    dsai2.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsai2.descriptorPool = pool;
    dsai2.descriptorSetCount = 1;
    dsai2.pSetLayouts = &cs2SetLayout;
    VkDescriptorSet set;
    vkAllocateDescriptorSets(dev, &dsai2, &set);

    VkDescriptorImageInfo outInfo2{};
    outInfo2.imageView = mipView;
    outInfo2.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkWriteDescriptorSet w[3] = {};
    w[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, 3, 0, 1,
            VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &cubeInfo, nullptr, nullptr};
    w[1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, 1, 0, 1,
            VK_DESCRIPTOR_TYPE_SAMPLER, &sampInfo2, nullptr, nullptr};
    w[2] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, 2, 0, 1,
            VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &outInfo2, nullptr, nullptr};
    vkUpdateDescriptorSets(dev, 3, w, 0, nullptr);

    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, cs2Layout, 0, 1, &set, 0, nullptr);
    struct { float roughness; uint32_t mipSize, p0, p1; } pc2 = {(float)mip / (float)(kEnvMipCount - 1), mipSize, 0, 0};
    vkCmdPushConstants(cmd, cs2Layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 16, &pc2);
    vkCmdDispatch(cmd, (mipSize + 7) / 8, (mipSize + 7) / 8, 6);

    // 该 mip 后续不再被读,各 dispatch 间无需 barrier
  }
```

- [ ] **Step 4: 收尾销毁(endSingleTimeCommands 之后)**

在 `device.endSingleTimeCommands(cmd)` 之后、既有 CS1 清理代码旁补:

```cpp
  for (VkImageView v : mipViews) vkDestroyImageView(dev, v, nullptr);
  vkDestroyShaderModule(dev, cs2Module, nullptr);
  vkDestroyPipeline(dev, cs2Pipeline, nullptr);
  vkDestroyPipelineLayout(dev, cs2Layout, nullptr);
  vkDestroyDescriptorSetLayout(dev, cs2SetLayout, nullptr);
```

(descriptor set 随 pool 销毁自动释放,pool 的销毁 Task 3 已在 end 之后,OK。)

- [ ] **Step 5: 构建 + RenderDoc 验证 mip 链**

构建运行后抓帧:`get_texture_data` 依次读 env cube mip 0/2/4/6/8:画面应逐级变糊,mip8 接近平均色,无 NaN(全黑/全白异常)。

- [ ] **Step 6: Commit**

```bash
git add shaders/ibl.hlsl shaders/compile_shaders.bat Src/IBLGenerator.cpp
git commit -m "feat: GGX importance-sampled specular prefilter for env cube mips

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 5: CS3 DFG LUT + 对拍校验

**Files:**
- Modify: `shaders/ibl.hlsl`(追加 DfgLutCS)
- Modify: `shaders/compile_shaders.bat`
- Modify: `Src/IBLGenerator.cpp`(LUT 资源 + dispatch + 开发期 readback 校验)

**Interfaces:**
- Consumes: Task 4 的 `IBL_Hammersley`/`IBL_ImportanceSampleGGX`。
- Produces: `IBLResources.dfgLut/dfgLutMemory/dfgLutView`(Task 7 绑定);`shaders/ibl_dfglut.cs.spv`。

- [ ] **Step 1: ibl.hlsl 追加**

```hlsl
// ---- DFG LUT(Karis split-sum 第二步;uv = (NdotV, roughness) → (scale, bias)) ----
float IBL_GeometrySchlickGGX(float ndv, float roughness)
{
    float a = roughness * roughness;
    float k = a / 2.0; // IBL 的 k(= roughness²/2,与本项目 lighting.hlsl evaluateBRDF 的 alpha/2 一致)
    return ndv / (ndv * (1.0 - k) + k);
}

float IBL_GeometrySmith(float3 n, float3 v, float3 l, float roughness)
{
    float ndv = saturate(dot(n, v));
    float ndl = saturate(dot(n, l));
    return IBL_GeometrySchlickGGX(ndv, roughness) * IBL_GeometrySchlickGGX(ndl, roughness);
}

VK_BINDING(0,0) RWTexture2D<float2> outputLUT;

[numthreads(8, 8, 1)]
void DfgLutCS(uint3 tid : SV_DispatchThreadID)
{
    uint w, h;
    outputLUT.GetDimensions(w, h);
    if (tid.x >= w || tid.y >= h) return;

    float ndv = (tid.x + 0.5) / (float)w;
    float roughness = (tid.y + 0.5) / (float)h;

    float3 v;
    v.x = sqrt(1.0 - ndv * ndv);
    v.y = 0.0;
    v.z = ndv;
    float3 n = float3(0.0, 0.0, 1.0);

    float a = 0.0;
    float b = 0.0;
    const uint SAMPLE_COUNT = 128;
    for (uint i = 0; i < SAMPLE_COUNT; ++i)
    {
        float2 xi = IBL_Hammersley(i, SAMPLE_COUNT);
        float3 hv = IBL_ImportanceSampleGGX(xi, n, roughness);
        float3 l = 2.0 * dot(v, hv) * hv - v;
        float ndl = saturate(l.z);
        if (ndl > 0.0)
        {
            float ndh = saturate(hv.z);
            float vdh = saturate(dot(v, hv));
            float g = IBL_GeometrySmith(n, v, l, roughness);
            float gVis = (g * vdh) / max(ndh * ndv, 1e-4); // Review Focus #3
            float fc = pow(1.0 - vdh, 5.0);
            a += (1.0 - fc) * gVis;
            b += fc * gVis;
        }
    }
    outputLUT[tid.xy] = float2(a, b) / (float)SAMPLE_COUNT;
}
```

注意:binding 0 与 CS1 的 equirect 同号但类型不同(STORAGE_IMAGE)——CS3 用自己的 set layout,无冲突。

- [ ] **Step 2: compile_shaders.bat 追加 + 编译**

```bat
D:\VulkanSDK\1.3.296.0\Bin\dxc.exe -spirv -T cs_6_2 ibl.hlsl -E DfgLutCS -Fo ibl_dfglut.cs.spv
```

- [ ] **Step 3: IBLGenerator.cpp 加 LUT 生成**

CS2 资源创建附近加 CS3 pipeline(set layout 单 binding 0 = STORAGE_IMAGE,无 push constants——pushConstantRangeCount=0),dispatch 插在 Task 5 锚点;LUT image 在函数前部创建:

```cpp
  createImage2D(device, VK_FORMAT_R16G16_SFLOAT, 256, 256, 1, 1, 0,
                VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                out.dfgLut, out.dfgLutMemory);

  VkImageViewCreateInfo lvi{};
  lvi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  lvi.image = out.dfgLut;
  lvi.viewType = VK_IMAGE_VIEW_TYPE_2D;
  lvi.format = VK_FORMAT_R16G16_SFLOAT;
  lvi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  vkCreateImageView(dev, &lvi, nullptr, &out.dfgLutView);
```

录制段:UNDEFINED→GENERAL barrier → dispatch (32,32,1) → GENERAL→SHADER_READ_ONLY barrier(与 env cube 的最终 transition 并列)。

TRANSFER_SRC 用于下面的 readback 校验。

- [ ] **Step 4: 开发期对拍校验(readback vs Apple DFGLUT.ktx)**

`Src/IBLGenerator.cpp` 顶部 include 加 `<spdlog/spdlog.h>`;匿名命名空间加两个助手,`generate()` 末尾(endSingleTimeCommands、临时对象清理之后)调用一次。完整代码:

```cpp
namespace {

float halfToFloat(uint16_t h) {
  uint32_t sign = (uint32_t)(h & 0x8000u) << 16;
  uint32_t exp = (h >> 10) & 0x1F;
  uint32_t mant = h & 0x3FF;
  uint32_t f;
  if (exp == 0) {
    // 次正规:value = mant/1024 * 2^-14
    float v = (float)mant / 1024.0f * 6.103515625e-05f;
    memcpy(&f, &v, 4);
    f |= sign;
  } else if (exp == 31) {
    f = sign | 0x7F800000u | (mant << 13);
  } else {
    f = sign | ((exp + 112) << 23) | (mant << 13);
  }
  float out;
  memcpy(&out, &f, 4);
  return out;
}

void createHostBuffer(VulkanDevice& device, VkDeviceSize size, VkBufferUsageFlags usage,
                      VkBuffer& buffer, VkDeviceMemory& memory) {
  VkBufferCreateInfo bi{};
  bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  bi.size = size;
  bi.usage = usage;
  bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  if (vkCreateBuffer(device.getLogicalDevice(), &bi, nullptr, &buffer) != VK_SUCCESS)
    throw std::runtime_error("IBL: failed to create buffer");
  VkMemoryRequirements mr;
  vkGetBufferMemoryRequirements(device.getLogicalDevice(), buffer, &mr);
  VkMemoryAllocateInfo ai{};
  ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  ai.allocationSize = mr.size;
  ai.memoryTypeIndex = device.findMemoryType(
      mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  if (vkAllocateMemory(device.getLogicalDevice(), &ai, nullptr, &memory) != VK_SUCCESS)
    throw std::runtime_error("IBL: failed to allocate buffer memory");
  vkBindBufferMemory(device.getLogicalDevice(), buffer, memory, 0);
}

// 把 GPU 生成的 LUT 读回,与 Apple 烘焙的 DFGLUT.ktx 逐 texel 对比(Review Focus #3)。
void validateDfgAgainstReference(VulkanDevice& device, VkImage dfgImage,
                                 const std::filesystem::path& refPath) {
  VkDevice dev = device.getLogicalDevice();
  constexpr uint32_t kSize = 256;
  constexpr VkDeviceSize kBytes = (VkDeviceSize)kSize * kSize * 4; // RG16F = 4B/texel

  // 1) readback:SHADER_READ_ONLY → TRANSFER_SRC,copy → staging,转回
  VkBuffer staging; VkDeviceMemory stagingMem;
  createHostBuffer(device, kBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, staging, stagingMem);
  VkCommandBuffer cmd = device.beginSingleTimeCommands();
  transitionRange(cmd, dfgImage, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                  VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 0, 1, 1,
                  VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                  VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
  VkBufferImageCopy region{};
  region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  region.imageExtent = {kSize, kSize, 1}; // bufferOffset/rowLength 默认 0 = 紧密(1024B 行距,满足对齐)
  vkCmdCopyImageToBuffer(cmd, dfgImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging, 1, &region);
  transitionRange(cmd, dfgImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1, 1,
                  VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT,
                  VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
  device.endSingleTimeCommands(cmd);

  uint16_t* gpu = nullptr;
  vkMapMemory(dev, stagingMem, 0, kBytes, 0, (void**)&gpu);

  // 2) 解析 KTX1:64B 头(12B identifier + 13×uint32)→ keyValue → per-mip(uint32 size + data)
  // uint32 字段序:[0]endianness [1]glType [2]glTypeSize [3]glFormat [4]glInternalFormat
  //               [5]glBaseInternalFormat [6]width [7]height [8]depth [9]arrayElements
  //               [10]faces [11]mipLevels [12]bytesOfKeyValueData
  std::vector<char> ktx = readFile(refPath.generic_string());
  const uint32_t* header = reinterpret_cast<const uint32_t*>(ktx.data() + 12);
  uint32_t glInternalFormat = header[4]; // 期望 0x822F (GL_RG16F)
  uint32_t width = header[6];
  uint32_t kvBytes = header[12];
  if (glInternalFormat != 0x822F || width != kSize) {
    spdlog::warn("IBL: reference KTX format unexpected (internal=0x{:x}, w={}), skip validation",
                 glInternalFormat, width);
  } else {
    const uint16_t* ref = reinterpret_cast<const uint16_t*>(ktx.data() + 64 + kvBytes + 4);
    double mae = 0.0;
    uint32_t nanCount = 0;
    for (uint32_t i = 0; i < kSize * kSize * 2; ++i) {
      float a = halfToFloat(gpu[i]);
      float b = halfToFloat(ref[i]);
      if (std::isnan(a)) ++nanCount;
      mae += std::fabs(a - b);
    }
    mae /= (double)(kSize * kSize * 2);
    spdlog::info("IBL: DFG LUT vs Apple reference MAE={:.5f}, NaN count={}", mae, nanCount);
    if (mae > 0.02 || nanCount > 0)
      spdlog::warn("IBL: DFG LUT validation out of tolerance — check k=(a^2)/2 in IBL_GeometrySchlickGGX, Hammersley sequence, sample count");
  }

  vkUnmapMemory(dev, stagingMem);
  vkDestroyBuffer(dev, staging, nullptr);
  vkFreeMemory(dev, stagingMem, nullptr);
}

} // namespace
```

`generate()` 末尾(`return out;` 之前)加:

```cpp
  const std::filesystem::path dfgRef = R"(D:\ModernRenderingWithMetal\Assets\DFGLUT.ktx)";
  if (std::filesystem::exists(dfgRef))
    validateDfgAgainstReference(device, out.dfgLut, dfgRef);
```

(`std::isnan` 需要 `<cmath>`;transitionRange 的参数序见 Task 3 定义。)

- [ ] **Step 5: 构建运行,确认校验输出**

构建运行,日志应出现 `IBL: DFG LUT vs Apple reference MAE=... NaN count=0`。MAE 应 < 0.02(128 samples 与 Apple 烘焙的差异量级)。不达标则按 Step 4 提示排查,达标前不要进 Task 6。

- [ ] **Step 6: Commit**

```bash
git add shaders/ibl.hlsl shaders/compile_shaders.bat Src/IBLGenerator.cpp
git commit -m "feat: Karis split-sum DFG LUT generation with reference validation

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 6: FrameConstants 扩展 + ImGui + DX12 默认值

**Files:**
- Modify: `Src/Include/Common.h:72-103`
- Modify: `shaders/commonstruct.hlsl:74-94`
- Modify: `Src/Include/GpuScene.h`(默认值区,~970-977)
- Modify: `Src/DX12/DX12GpuScene.cpp`(~119-120,默认值区)
- Modify: `Src/GpuScene.cpp`(ImGui 区,8556 起)

**Interfaces:**
- Consumes: 无。
- Produces: `FrameConstants::iblScale`(offset 128)、`FrameConstants::iblSpecularScale`(offset 132),HLSL 侧同名;Task 7/8 的 shader 代码读这两个字段;ImGui 拉条写这两个字段。

- [ ] **Step 1: Common.h**

`FrameConstants` 末尾(`noiseSpeed` 之后)追加:

```cpp
  float iblScale;          // IBL 总强度(0 = 关闭);ImGui 可调,默认 1.0
  float iblSpecularScale;  // 仅 IBL 高光额外倍率;默认 4.0(对齐 Metal)
  float _padIbl0;
  float _padIbl1;
```

static_assert 更新:

```cpp
static_assert(sizeof(FrameConstants) == 144, "FrameConstants size mismatch vs AAPLFrameConstants (ibl fields appended)");
static_assert(offsetof(FrameConstants, iblScale)         == 128, "iblScale offset mismatch vs HLSL Offset(128)");
static_assert(offsetof(FrameConstants, iblSpecularScale) == 132, "iblSpecularScale offset mismatch vs HLSL Offset(132)");
```

(sizeof==128 的旧断言同步改为 144;其余 offset 断言保留。)

- [ ] **Step 2: commonstruct.hlsl**

`AAPLFrameConstants` 末尾(`noiseSpeed` 之后)追加:

```hlsl
    float iblScale;         // offset 128
    float iblSpecularScale; // offset 132
    float _padIbl0;
    float _padIbl1;
```

- [ ] **Step 3: 默认值只写一次(Review Focus #5)**

在 GpuScene.h 的 `_frameConstants` 默认初始化区(~970-977,与 `skyColor` 默认同处)加:

```cpp
  _frameConstants.iblScale = 1.0f;
  _frameConstants.iblSpecularScale = 4.0f;
```

(若该区域是构造初始化而非成员默认,照其形式加;**不要**在 GpuScene.cpp:3423-3454 的 per-frame 填充里加这两行。)

`Src/DX12/DX12GpuScene.cpp` 默认值区(~119-120,`_frameConstants.skyColor = ...` 旁)加同样的两行(DX12 shader 不含 IBL 代码,值不影响画面,仅为避免未初始化读取)。

- [ ] **Step 4: ImGui(GpuScene.cpp:8556 起的 "Culling Stats" 窗口内)**

```cpp
  if (ImGui::CollapsingHeader("IBL")) {
    ImGui::SliderFloat("IBL Scale", &_frameConstants.iblScale, 0.0f, 2.0f);
    ImGui::SliderFloat("IBL Specular Scale", &_frameConstants.iblSpecularScale, 0.0f, 8.0f);
    if (!_iblAvailable) ImGui::TextDisabled("env map missing - using black fallback");
  }
```

- [ ] **Step 5: 双后端重编译验证**

```powershell
cmd /c "cd /d F:\AdvancedVulkanRendering\shaders && .\compile_shaders.bat"
cmd /c "cd /d F:\AdvancedVulkanRendering\shaders && .\compile_shaders_dx12.bat"
cmake --build F:\AdvancedVulkanRendering --config Debug --target AdvancedVulkanRendering
```

Expected: 两个 bat 全部零错误(dxc 对未使用的新字段无告警);C++ 编译零错误。
运行 Vulkan 与 `.\Bin\AdvancedVulkanRendering.exe --dx12`:画面与改动前一致,ImGui 出现 IBL 分组(拉条暂时光学无效果,因为 shader 还没消费)。

- [ ] **Step 6: Commit**

```bash
git add Src/Include/Common.h shaders/commonstruct.hlsl Src/Include/GpuScene.h Src/DX12/DX12GpuScene.cpp Src/GpuScene.cpp
git commit -m "feat: add iblScale/iblSpecularScale to FrameConstants with imgui controls

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 7: ibl_common.hlsl + deferred pass 集成

**Files:**
- Create: `shaders/ibl_common.hlsl`
- Modify: `shaders/deferredlighting.hlsl`
- Modify: `Src/GpuScene.cpp`(deferred set layout :1217-1348、pool :1352-1356、writes :1382-1441)

**Interfaces:**
- Consumes: Task 2-5 产出的 `_iblEnvCubeView/_iblDfgLutView/_iblSHBuffer/_iblSampler`;Task 6 的 `frameConstants.iblScale/iblSpecularScale`。
- Produces: `IBL()`/`evaluateShCoefficients()`(Task 8 复用,签名见代码);deferred 画面出现 IBL。

- [ ] **Step 1: shaders/ibl_common.hlsl**

```hlsl
// ibl_common.hlsl — IBL 采样(deferred/forward 共用;后端中立,
// 调用方自行用 #ifndef DX12_BACKEND 保护,DX12 移植完成后移除 guard)。
// 数学与 Apple AAPLLightingCommon.h 的 evaluateShCoefficients + IBL() 一致,
// 差异:env cube 直接存线性 HDR,无 RGBM 6.0*rgb*a 解码。

float3 evaluateShCoefficients(float3 n, float4 sh[9])
{
    return sh[0].rgb
         + sh[1].rgb * (n.y)
         + sh[2].rgb * (n.z)
         + sh[3].rgb * (n.x)
         + sh[4].rgb * (n.y * n.x)
         + sh[5].rgb * (n.y * n.z)
         + sh[6].rgb * (3.0 * n.z * n.z - 1.0)
         + sh[7].rgb * (n.z * n.x)
         + sh[8].rgb * (n.x * n.x - n.y * n.y);
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
    float3 diffuseIBL = evaluateShCoefficients((float3)surface.normal, sh) * (float3)surface.albedo;

    float perceptualRoughness = (float)surface.roughness;
    float NoV = max(dot((float3)surface.normal, viewDir), 0.0);
    float3 r = reflect(-viewDir, (float3)surface.normal);

    const float mipLevels = 8.0; // 9 mip 链的最后一级(256..1)
    float lod = perceptualRoughness * mipLevels;
    float3 indirectSpecular = envMap.SampleLevel(samp, r, lod).rgb;
    indirectSpecular = min(indirectSpecular, 8192.0); // fp16 RT 上限 65504:8192*1.15*4≈37.6k(见 Task 7 勘误)

    float2 dfg = dfgLut.SampleLevel(samp, float2(NoV, perceptualRoughness), 0);
    float3 specularColor = (float3)surface.F0 * dfg.x + dfg.y;

    float3 specularIBL = indirectSpecular * specularColor;
    return (diffuseIBL + specularIBL * specularScale) * scale;
}
```

> **勘误(实现后补录):** 本节清单相对初版计划有三处修正,已直接体现在上方代码中:
> ① `IBL()` 调 `evaluateShCoefficients` 必须传 `sh` 参数(初版漏写,dxc 编译即报,实现期修正,Ruling 15);
> ② SH 求值方向为 **+N**——初版照抄 Apple 的 `-N` 调用,但那只对 Apple 反号约定的硬编码系数成立;本实现系数是标准辐射投影(Task 1 单测钉死 top-heavy ⇒ c[1]>0),`-N` 会把奇次带(上下不对称项)镜像反转(全分支终审发现,fix commit cccedbc);
> ③ 新增 `min(indirectSpecular, 8192.0)` 钳制——deferred HDR RT 是 fp16(上限 65504),未钳制时太阳像素(60000)×specularColor×iblSpecularScale(4) 可溢出为 inf,经 ACES 变 NaN 并被 TAA 历史扩散(终审 Important)。

- [ ] **Step 2: deferredlighting.hlsl 修改**

顶部 include 区(`#include "lighting.hlsl"` 之后)加 `#include "ibl_common.hlsl"`。

binding 区(scatter volume binding 17 之后)加:

```hlsl
// IBL(Vulkan;DX12 移植前用 guard 隔离)
#ifndef DX12_BACKEND
VK_BINDING(18,1) Texture2D<float2> dfgLutTex;
VK_BINDING(19,1) TextureCube envMap;
VK_BINDING(20,1) SamplerState iblSampler;
VK_BINDING(21,1) cbuffer SHCoefficients { float4 shCoefs[9]; };
#endif
```

调用点(现 :139-141)改为:

```hlsl
    float ao = aoTexture.SampleLevel(_NearestClampSampler, input.TextureUV, 0);

    half3 result = lightingShader(surfaceData, depth, worldPosition, frameConstants, cameraParams) * shadow;

#ifndef DX12_BACKEND
    // IBL:(sun*shadow + IBL) * AO —— 对齐 Apple AAPLLightingCommon.h 的顺序。
    // sky 像素(depth≈0)不加:随后的 sky 分支会整体覆盖 result。
    if (frameConstants.iblScale > 0.0f && depth >= 0.0001f)
    {
        float3 camPosIBL = float3(cameraParams.invViewMatrix._m03,
                                  cameraParams.invViewMatrix._m13,
                                  cameraParams.invViewMatrix._m23);
        float3 viewDirIBL = normalize(camPosIBL - worldPosition.xyz);
        result += (half3)IBL(surfaceData, envMap, dfgLutTex, iblSampler, shCoefs,
                             viewDirIBL, frameConstants.iblScale, frameConstants.iblSpecularScale);
    }
#endif

    result *= (half)ao;
```

- [ ] **Step 3: GpuScene.cpp deferred set layout(:1317-1334 的 bindings 数组)**

新增 4 个 binding 定义并追加进数组:

```cpp
  // IBL bindings(18-21,see deferredlighting.hlsl)
  VkDescriptorSetLayoutBinding dfgLutBinding = {};
  dfgLutBinding.binding = 18;
  dfgLutBinding.descriptorCount = 1;
  dfgLutBinding.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  dfgLutBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

  VkDescriptorSetLayoutBinding envCubeBinding = dfgLutBinding;
  envCubeBinding.binding = 19;

  VkDescriptorSetLayoutBinding iblSamplerBinding = {};
  iblSamplerBinding.binding = 20;
  iblSamplerBinding.descriptorCount = 1;
  iblSamplerBinding.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
  iblSamplerBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

  VkDescriptorSetLayoutBinding shUboBinding = {};
  shUboBinding.binding = 21;
  shUboBinding.descriptorCount = 1;
  shUboBinding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
  shUboBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
```

数组末尾追加 `dfgLutBinding, envCubeBinding, iblSamplerBinding, shUboBinding`。

- [ ] **Step 4: pool 扩容(:1352-1356)**

```cpp
  std::vector<VkDescriptorPoolSize> sizes = {
      {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 16 * framesInFlight}, // 0-4,6,10,13,16,18,19 + headroom
      {VK_DESCRIPTOR_TYPE_SAMPLER, 5 * framesInFlight},        // 5,7,14,17,20
      {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 6 * framesInFlight}, // 8,9,11,12,15 + headroom
      {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1 * framesInFlight}, // 21
  };
```

- [ ] **Step 5: descriptor writes(:1382-1441 的 per-frame 循环尾部)**

`writes` 数组由 6 扩为 10,新增 4 条(成员赋值风格,与既有代码一致):

```cpp
  VkDescriptorImageInfo dfgInfo{};
  dfgInfo.imageView = _iblDfgLutView;
  dfgInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  VkWriteDescriptorSet setDfg = {};
  setDfg.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  setDfg.dstSet = deferredLightingDescriptorSet[f];
  setDfg.dstBinding = 18;
  setDfg.dstArrayElement = 0;
  setDfg.descriptorCount = 1;
  setDfg.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  setDfg.pImageInfo = &dfgInfo;

  VkDescriptorImageInfo envInfo{};
  envInfo.imageView = _iblEnvCubeView;
  envInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  VkWriteDescriptorSet setEnv = {};
  setEnv.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  setEnv.dstSet = deferredLightingDescriptorSet[f];
  setEnv.dstBinding = 19;
  setEnv.dstArrayElement = 0;
  setEnv.descriptorCount = 1;
  setEnv.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  setEnv.pImageInfo = &envInfo;

  VkDescriptorImageInfo iblSampInfo{};
  iblSampInfo.sampler = _iblSampler;
  VkWriteDescriptorSet setIblSamp = {};
  setIblSamp.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  setIblSamp.dstSet = deferredLightingDescriptorSet[f];
  setIblSamp.dstBinding = 20;
  setIblSamp.dstArrayElement = 0;
  setIblSamp.descriptorCount = 1;
  setIblSamp.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
  setIblSamp.pImageInfo = &iblSampInfo;

  VkDescriptorBufferInfo shInfo{};
  shInfo.buffer = _iblSHBuffer;
  shInfo.offset = 0;
  shInfo.range = 9 * 16;
  VkWriteDescriptorSet setSH = {};
  setSH.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  setSH.dstSet = deferredLightingDescriptorSet[f];
  setSH.dstBinding = 21;
  setSH.dstArrayElement = 0;
  setSH.descriptorCount = 1;
  setSH.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
  setSH.pBufferInfo = &shInfo;

  std::array<VkWriteDescriptorSet, 10> writes = {setWriteTexture[0], setWriteTexture[1],
                                                 setWriteTexture[2], setWriteTexture[3],
                                                 setWriteDepth,      setSampler,
                                                 setDfg,             setEnv,
                                                 setIblSamp,         setSH};
```

- [ ] **Step 6: 编译运行 + 视觉验证**

```powershell
cmd /c "cd /d F:\AdvancedVulkanRendering\shaders && .\compile_shaders.bat"
cmake --build F:\AdvancedVulkanRendering --config Debug --target AdvancedVulkanRendering
.\Bin\AdvancedVulkanRendering.exe
```

Expected: 画面明显变化——阴影区不再是纯黑/纯 AO 色,呈现环境光(蓝天光/环境反射);金属/低粗糙度表面出现环境反射。ImGui 把 `IBL Scale` 拉到 0 应恢复改动前画面;`IBL Specular Scale` 拉高只增强反射。截图留存(on/off 各一张)。

- [ ] **Step 7: Commit**

```bash
git add shaders/ibl_common.hlsl shaders/deferredlighting.hlsl Src/GpuScene.cpp
git commit -m "feat: IBL in deferred lighting pass (SH diffuse + prefiltered specular)

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 8: forward pass 集成

**Files:**
- Modify: `shaders/drawcluster.hlsl`
- Modify: `Src/GpuScene.cpp`(`init_appl_descriptors` :1661-1768 及后续 writes)

**Interfaces:**
- Consumes: Task 7 的 `IBL()`;同 Task 7 的 4 个资源。
- Produces: forward(透明)pass 的 IBL。

- [ ] **Step 1: drawcluster.hlsl 修改**

include 区(`#include "lighting.hlsl"` 之后)加 `#include "ibl_common.hlsl"`。

binding 区(:63 chunkIndex 之后)加:

```hlsl
// IBL(Vulkan;与 deferred 同资源,appl set 的空闲槽位)
#ifndef DX12_BACKEND
VK_BINDING(5,1) Texture2D<float2> dfgLutTex;
VK_BINDING(6,1) TextureCube envMap;
VK_BINDING(7,1) SamplerState iblSampler;
VK_BINDING(8,1) cbuffer SHCoefficients { float4 shCoefs[9]; };
#endif
```

`RenderSceneForwardPS`(:290 附近,`half3 res = lightingShader(...)` 之后)与 `RenderSceneForwardPSIndirect`(:374 附近,同位置)各加:

```hlsl
#ifndef DX12_BACKEND
    if (frameConstants.iblScale > 0.0f)
    {
        float3 camPosIBL = float3(cameraParams.invViewMatrix._m03,
                                  cameraParams.invViewMatrix._m13,
                                  cameraParams.invViewMatrix._m23);
        float3 viewDirIBL = normalize(camPosIBL - input.wsPosition);
        res += (half3)IBL(surfaceData, envMap, dfgLutTex, iblSampler, shCoefs,
                          viewDirIBL, frameConstants.iblScale, frameConstants.iblSpecularScale);
    }
#endif
```

(实现时先确认两个函数内 surface 数据变量名与 `input.wsPosition` 字段名——以 :290/:374 上下文为准;base/shadow 等其它 entry 不引用,dxc 自动消除未用 binding。)

- [ ] **Step 2: GpuScene.cpp appl set layout(:1709-1716)**

新增 4 个 binding(5=SAMPLED_IMAGE,6=SAMPLED_IMAGE,7=SAMPLER,8=UNIFORM_BUFFER,均 FRAGMENT),追加进 `bindings[]`;`bindingFlags` 的 std::array 长度随 `bindingcount` 自动正确(补 4 个 `0`)。pool(:1741-1745)无需改动(4096 SAMPLED_IMAGE、10*N 其余类型均有足够余量)。

- [ ] **Step 3: appl descriptor writes(:1770 起的 per-frame 循环)**

与 Task 7 Step 5 同构的 4 条 write,binding 改为 5/6/7/8,dstSet 为 `applDescriptorSets[f]`。

- [ ] **Step 4: 编译运行 + 验证**

编译 shader + 构建 + 运行:场景中透明物件( Bistro 的树叶/玻璃等 alpha 材质)应呈现与 deferred 一致的环境光;IBL Scale 拉 0 时两者同时关闭。
RenderDoc:找到 forward pass 的 draw(`get_draw_calls` marker 过滤 forward),`get_shader_details`(pixel stage)确认 binding 5-8 已绑定。

- [ ] **Step 5: Commit**

```bash
git add shaders/drawcluster.hlsl Src/GpuScene.cpp
git commit -m "feat: IBL in forward transparent pass

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 9: 资源清理 + 全量验证

**Files:**
- Modify: `Src/GpuScene.cpp`(cleanup/析构区)

**Interfaces:**
- Consumes: 全部前置任务。
- Produces: 无泄漏的最终状态 + 验证记录。

- [ ] **Step 1: cleanup 补销毁**

在 GpuScene.cpp 中 `grep -n "vkDestroyImage(device"` 找到现有纹理销毁区(如 cleanup()),为以下成员补销毁(注意 destroy 顺序:view 先于 image;先 `vkDeviceWaitIdle` 的既有语义不变):

```cpp
  if (_equirectView)   vkDestroyImageView(dev, _equirectView, nullptr);
  if (_equirectImage)  { vkDestroyImage(dev, _equirectImage, nullptr); vkFreeMemory(dev, _equirectMemory, nullptr); }
  if (_iblEnvCubeView) vkDestroyImageView(dev, _iblEnvCubeView, nullptr);
  if (_iblEnvCube)     { vkDestroyImage(dev, _iblEnvCube, nullptr); vkFreeMemory(dev, _iblEnvCubeMemory, nullptr); }
  if (_iblDfgLutView)  vkDestroyImageView(dev, _iblDfgLutView, nullptr);
  if (_iblDfgLut)      { vkDestroyImage(dev, _iblDfgLut, nullptr); vkFreeMemory(dev, _iblDfgLutMemory, nullptr); }
  if (_iblSHBuffer)    { vkDestroyBuffer(dev, _iblSHBuffer, nullptr); vkFreeMemory(dev, _iblSHMemory, nullptr); }
  if (_iblSampler)     vkDestroySampler(dev, _iblSampler, nullptr);
```

(dev 替换为该函数内实际的 device 获取方式。)

- [ ] **Step 2: RenderDoc 全量检查单**

抓帧并用 MCP 逐项确认:
- env cube:mip0-8 逐级变糊,mip0 内容与 equirect 一致,无 inf/NaN(全黑/全白块);
- deferred draw 的 pixel stage:binding 18-21 绑定正确(`get_shader_details`),SH UBO 内容非零(第 0 项 ≈ (1.6, 1.5, 1.6) 量级——与 Metal 常数同环境对比);
- forward draw:binding 5-8 绑定正确。

- [ ] **Step 3: DX12 回归**

`.\Bin\AdvancedVulkanRendering.exe --dx12` 正常运行,画面与 `ibl` 分支起点一致(无 IBL)。`compile_shaders_dx12.bat` 重跑零错误。

- [ ] **Step 4: Vulkan 既有功能回归**

依次开关 ImGui 中的 TAA / Ray Tracing / scatter volume / spot lights,确认无验证层报错、画面无异常(重点关注 descriptor pool 相关的 OUT_OF_POOL/validation 错误)。

- [ ] **Step 5: 收尾 Commit**

```bash
git add Src/GpuScene.cpp
git commit -m "feat: destroy IBL resources in cleanup

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

## Self-Review 记录

- Spec 覆盖:spec §1 资产(Task 2)、§2 生成链(Task 3-5)、§3 shader(Task 6-8)、§4 C++(Task 2/7/8/9)、§5 验证(Task 1/5/9)—— 逐项有主。
- 占位符扫描:Task 5 Step 4 的 readback 校验给出数据布局/对齐/判定阈值,实现注释已内联;无 TBD。
- 类型一致:`SH9.c[9][3]`(T1)→ `shGpu[9][4]`(T2)→ `float4 shCoefs[9]`(T7/8)一致;`IBLResources` 字段与 GpuScene 成员逐一对应。
- Review Focus 5 项均已钉入对应 Task 的步骤(见各项括号)。
