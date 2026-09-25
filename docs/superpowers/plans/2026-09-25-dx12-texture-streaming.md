# DX12 Texture Streaming Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 为 DX12 后端实现真正的 mip-level texture streaming（显存降到 permanent-mip 水位 + 按需双向流动），逐结构对齐 Vulkan 端现有实现。

**Architecture:** 五件套镜像 Vulkan(`initTextureStreaming` / `shutdownTextureStreaming` / `UpdateTextureStreaming` / `dispatchStreamingRequest`+后台线程 / `processStreamingWork`)。后台线程做解压/建 committed resource/填 staging(CPU only,D3D12 device 方法 free-threaded)；主线程在 swap 时 `WaitForGpu`、录 copy、原地重写 static SRV slot，旧资源进按帧 fence 释放的 retention ring。纯逻辑（mip 公式、copy region、屏幕面积）抽成无 D3D12 依赖的 policy 头文件以便单测。

**Tech Stack:** C++20,D3D12,MSVC 2022(`D:\Program Files\Microsoft Visual Studio\2022\Community`)，无框架手写 main 单测（沿用 `Tests/DX12ScenePolicyTests.cpp` 模式）。

**Spec:** `docs/superpowers/specs/2026-09-25-dx12-texture-streaming-design.md`

## Global Constraints

- 结构与命名对齐 Vulkan 端 `Src/GpuScene.cpp` 的 streaming 路径；`PERMANENT_TEXTURE_SIZE = 64`,`MAX_TEXTURE_SIZE = 4096`。
- 不改 shader、不改材质系统、不改根签名；bindless SRV slot 保持 `SRV_BINDLESS_START + textureIndex` 稳定。
- 主项目构建（仓库根目录）：`cmake --build build-msvc --config Debug --target AdvancedVulkanRendering`，必须 0 error。
- 运行：`.\Bin\AdvancedVulkanRendering.exe --dx12`（从仓库根目录启动，资源按相对路径读取）。
- 单测编译运行（PowerShell，仓库根目录）：
  ```powershell
  cmd /c '"D:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >NUL && cl /std:c++20 /EHsc /nologo /I Src\Include /I Src\DX12 Tests\DX12TextureStreamingPolicyTests.cpp /Fe:Bin\DX12TextureStreamingPolicyTests.exe && Bin\DX12TextureStreamingPolicyTests.exe'
  ```
  退出码 0 = 全过；1 = 有失败（逐条打印）。
- 只在 `texture_streaming` 分支提交；提交信息沿用仓库 conventional-commit 风格（小写，如 `feat: ...`)。

## Review Focus

以下输入/失效模式 spec 隐含但没有任何任务的自动化测试覆盖，按最可能咬人排序；每条在所属任务里有对应的验证步骤：

1. **退出时后台线程仍持有 `_applMesh->_textureData`** — `shutdownTextureStreaming()`（停线程+join+清队列）必须先于 `delete _applMesh`，否则 use-after-free。属 Task 4（运行时 alt-F4 检查）。
2. **非 4 倍数尺寸的纹理** — policy 与 Vulkan 同用 floor 块数公式，`CreateTextures` 现有上传用 ceil;4 的倍数时两者一致，非倍数时 streaming 路径必须逐字节对齐 Vulkan 的 floor 口径（避免 staging 布局与源数据错位）。属 Task 4（代码评审点；真实资源全为 4 的倍数，无法运行时触发）。
3. **entry 在 work in-flight 期间 `requiredMip` 再次变化** — 期望行为：按 dispatch 时的旧 target 完成 swap，下一帧自然重新 dispatch；不得 crash/泄漏/重复 dispatch。属 Task 4（运行时快速推拉相机检查）。
4. **streaming 中的 `WaitForGpu` 与 `MoveToNextFrame` 的 fence 值交互** — 额外 Signal/Wait 抬高当前槽 fence 后，三帧 in-flight 语义不得破坏（无挂死、无 fence 错乱）。属 Task 4（运行时连续 swap 检查）。
5. **`OnResize` 发生在 work in-flight / retired 未释放时** — retired ring 与 SRV 不属于 swapchain，resize 后 streaming 应无感继续。属 Task 5（运行时拖窗口检查）。

---

### Task 1: 纯逻辑 policy 头文件 + 单测（TDD)

**Files:**
- Create: `Src/DX12/DX12TextureStreamingPolicy.h`
- Test: `Tests/DX12TextureStreamingPolicyTests.cpp`

**Interfaces:**
- Consumes: 无（仅 `<cmath>` `<cstdint>` `<vector>`)。
- Produces（后续任务依赖的确切名字）:
  - 常量：`DX12TextureStreamingPolicy::PERMANENT_TEXTURE_SIZE`(64)、`MAX_TEXTURE_SIZE`(4096)
  - `void GetPixelFormatBlockDesc(uint32_t pixelFormat, uint64_t& blockSize, uint64_t& bytesPerBlock)`
  - `uint64_t CalculateMipSizeInBlocks(uint64_t size, uint64_t blockSize, uint32_t mip)`
  - `uint32_t CalculateMinMip(uint64_t width, uint64_t height, uint64_t mipCount, unsigned int maxSize)`
  - `uint32_t CalculateRequiredMip(uint64_t width, uint64_t height, uint64_t mipCount, float screenArea)`
  - `float ComputeScreenArea(float viewX, float viewY, float viewZ, float radius, float focalLengthSquared, float viewW, float viewH)`
  - 结构：`StreamingBufferCopy { uint64_t stagingOffset; uint32_t width, height; uint32_t rowPitch; uint32_t srcRowPitch; uint32_t blockRows; uint32_t dstSubresource; }`
  - 结构：`StreamingImageCopy { uint32_t srcSubresource, dstSubresource; uint32_t width, height; }`
  - 结构：`StreamingCopyPlan { std::vector<StreamingBufferCopy> bufferCopies; std::vector<StreamingImageCopy> imageCopies; uint64_t stagingSize; uint32_t newMipCount, oldMipCount; }`
  - `StreamingCopyPlan BuildStreamingCopyPlan(uint64_t width, uint64_t height, uint64_t mipCount, uint32_t pixelFormat, uint32_t targetMip, uint32_t currentMip)`

- [ ] **Step 1: 写失败测试**

创建 `Tests/DX12TextureStreamingPolicyTests.cpp`（沿用 `DX12ScenePolicyTests.cpp` 的无框架模式）:

```cpp
#include "DX12TextureStreamingPolicy.h"

#include <cmath>
#include <cstdint>
#include <iostream>

namespace {

int failures = 0;

void expectU64(uint64_t actual, uint64_t expected, const char* message) {
  if (actual != expected) {
    std::cerr << message << ": expected " << expected << ", got " << actual << '\n';
    ++failures;
  }
}

void expectU32(uint32_t actual, uint32_t expected, const char* message) {
  expectU64(actual, expected, message);
}

void expectSize(size_t actual, size_t expected, const char* message) {
  expectU64((uint64_t)actual, (uint64_t)expected, message);
}

void expectNear(float actual, float expected, float eps, const char* message) {
  if (std::fabs(actual - expected) > eps) {
    std::cerr << message << ": expected " << expected << ", got " << actual << '\n';
    ++failures;
  }
}

void expectTrue(bool actual, const char* message) {
  if (!actual) {
    std::cerr << message << ": expected true\n";
    ++failures;
  }
}

} // namespace

using namespace DX12TextureStreamingPolicy;

int main() {
  // --- GetPixelFormatBlockDesc（镜像 GpuScene.cpp::getPixelFormatBlockDesc 含默认分支)---
  uint64_t bs = 0, bpb = 0;
  GetPixelFormatBlockDesc(130, bs, bpb); // BC1_RGBA
  expectU64(bs, 4, "BC1 block size");
  expectU64(bpb, 8, "BC1 bytes per block");
  GetPixelFormatBlockDesc(134, bs, bpb); // BC3_RGBA
  expectU64(bpb, 16, "BC3 bytes per block");
  GetPixelFormatBlockDesc(140, bs, bpb); // BC4_RUnorm
  expectU64(bpb, 8, "BC4 bytes per block");
  GetPixelFormatBlockDesc(142, bs, bpb); // BC5_RGUnorm
  expectU64(bpb, 16, "BC5 bytes per block");
  GetPixelFormatBlockDesc(152, bs, bpb); // BC7 — 走默认分支
  expectU64(bpb, 16, "BC7 bytes per block");

  // --- CalculateMipSizeInBlocks（floor 口径,与 Vulkan 一致)---
  expectU64(CalculateMipSizeInBlocks(4096, 4, 0), 1024, "4096 mip0 blocks");
  expectU64(CalculateMipSizeInBlocks(4096, 4, 6), 16, "4096 mip6 blocks");
  expectU64(CalculateMipSizeInBlocks(4, 4, 10), 1, "tiny size clamps to 1 block");

  // --- CalculateMinMip ---
  expectU32(CalculateMinMip(4096, 4096, 13, 4096), 0, "4096 tex top mip");
  expectU32(CalculateMinMip(4096, 4096, 13, 64), 6, "4096 tex permanent mip");
  expectU32(CalculateMinMip(8192, 8192, 14, 4096), 1, "8192 tex top mip");
  expectU32(CalculateMinMip(8192, 8192, 14, 64), 7, "8192 tex permanent mip");
  expectU32(CalculateMinMip(2048, 1024, 12, 64), 5, "2048x1024 uses largest dimension");
  expectU32(CalculateMinMip(32, 32, 6, 64), 0, "small texture clamps ratio to 1");
  expectU32(CalculateMinMip(128, 128, 8, 64), 1, "128 tex permanent mip");

  // --- CalculateRequiredMip ---
  expectU32(CalculateRequiredMip(4096, 4096, 13, 0.0f), 6, "no coverage -> bot mip");
  expectU32(CalculateRequiredMip(4096, 4096, 13, 4096.0f * 4096.0f), 0,
            "full coverage -> top mip");
  expectU32(CalculateRequiredMip(4096, 4096, 13, 1024.0f * 1024.0f), 2,
            "1/16 area -> mip 2");
  expectU32(CalculateRequiredMip(4096, 4096, 13, 1.0f), 6, "tiny area clamps to bot mip");
  expectU32(CalculateRequiredMip(8192, 8192, 14, 8192.0f * 8192.0f), 1,
            "huge texture clamps to top mip (MAX_TEXTURE_SIZE)");

  // --- ComputeScreenArea ---
  // 轴上远球:focal=1,1000x1000,center(0,0,100),r=1
  // area = pi/9999 * 1000*1000*0.25 ≈ 78.555
  float area = ComputeScreenArea(0.0f, 0.0f, 100.0f, 1.0f, 1.0f, 1000.0f, 1000.0f);
  expectNear(area, 78.555f, 0.01f, "far on-axis sphere area");
  // 球心距 <= 半径:全覆盖
  expectNear(ComputeScreenArea(0.0f, 0.0f, 0.5f, 1.0f, 1.0f, 1280.0f, 720.0f),
             1280.0f * 720.0f, 1e-3f, "enclosing sphere covers screen");

  // --- BuildStreamingCopyPlan: 升级 6 -> 4,4096 BC7(16 B/block)---
  {
    auto plan = BuildStreamingCopyPlan(4096, 4096, 13, 152, 4, 6);
    expectU32(plan.newMipCount, 9, "upgrade: new texture mip count");
    expectU32(plan.oldMipCount, 7, "upgrade: old texture mip count");
    expectSize(plan.bufferCopies.size(), 2, "upgrade: mips 4,5 uploaded");
    // mip 4:256x256 texels = 64x64 blocks * 16 B = 1024 B 行(已 256 对齐)
    expectU32(plan.bufferCopies[0].width, 256, "mip4 width");
    expectU32(plan.bufferCopies[0].height, 256, "mip4 height");
    expectU32(plan.bufferCopies[0].srcRowPitch, 1024, "mip4 tight pitch");
    expectU32(plan.bufferCopies[0].rowPitch, 1024, "mip4 aligned pitch");
    expectU32(plan.bufferCopies[0].blockRows, 64, "mip4 block rows");
    expectU32(plan.bufferCopies[0].dstSubresource, 0, "mip4 -> new sub 0");
    expectU64(plan.bufferCopies[0].stagingOffset, 0, "mip4 at staging start");
    // mip 5:128x128 = 32x32 blocks * 16 = 512 B 行
    expectU32(plan.bufferCopies[1].dstSubresource, 1, "mip5 -> new sub 1");
    expectU64(plan.bufferCopies[1].stagingOffset, 64ull * 1024, "mip5 staging offset");
    expectU64(plan.stagingSize, 64ull * 1024 + 32ull * 512, "upgrade staging total");
    // 共享:old 7 mips 落到 new subresources [2..8]
    expectSize(plan.imageCopies.size(), 7, "upgrade: all old mips copied");
    expectU32(plan.imageCopies[0].srcSubresource, 0, "upgrade: old sub 0");
    expectU32(plan.imageCopies[0].dstSubresource, 2, "upgrade: -> new sub (6-4)=2");
    expectU32(plan.imageCopies[0].width, 64, "upgrade: shared mip size");
    expectU32(plan.imageCopies[6].dstSubresource, 8, "upgrade: last old mip -> new sub 8");
  }

  // --- BuildStreamingCopyPlan: 驱逐 2 -> 6,4096 BC1(8 B/block)---
  {
    auto plan = BuildStreamingCopyPlan(4096, 4096, 13, 130, 6, 2);
    expectU32(plan.newMipCount, 7, "evict: new mip count");
    expectU32(plan.oldMipCount, 11, "evict: old mip count");
    expectTrue(plan.bufferCopies.empty(), "evict: no staging uploads");
    expectU64(plan.stagingSize, 0, "evict: staging size zero");
    expectSize(plan.imageCopies.size(), 7, "evict: new mips come from old");
    expectU32(plan.imageCopies[0].srcSubresource, 4, "evict: old sub (6-2)=4 -> new 0");
    expectU32(plan.imageCopies[0].dstSubresource, 0, "evict: dst 0");
    expectU32(plan.imageCopies[0].width, 64, "evict: mip6 width");
  }

  // --- BuildStreamingCopyPlan: 退化输入 ---
  {
    auto same = BuildStreamingCopyPlan(1024, 1024, 11, 134, 3, 3);
    expectTrue(same.bufferCopies.empty() && same.imageCopies.empty() &&
               same.stagingSize == 0, "same mip -> empty plan");
  }

  // --- 小节距对齐:升级 7 -> 6,4096 BC1,mip6 = 64 tex = 16 blocks * 8 B = 128 B -> 256 ---
  {
    auto plan = BuildStreamingCopyPlan(4096, 4096, 13, 130, 6, 7);
    expectSize(plan.bufferCopies.size(), 1, "one new mip");
    expectU32(plan.bufferCopies[0].srcRowPitch, 128, "tight pitch 128");
    expectU32(plan.bufferCopies[0].rowPitch, 256, "pitch aligns up to 256");
    expectU32(plan.bufferCopies[0].blockRows, 16, "16 block rows");
    expectU64(plan.stagingSize, 256ull * 16, "staging uses aligned pitch");
    // 共享:old 6 mips 落到 new subs [1..6]
    expectSize(plan.imageCopies.size(), 6, "old mips copied");
    expectU32(plan.imageCopies[0].dstSubresource, 1, "old sub 0 -> new sub 1");
  }

  return failures == 0 ? 0 : 1;
}
```

- [ ] **Step 2: 运行测试，确认失败**

用 Global Constraints 里的 PowerShell 单测命令。
Expected：编译失败，`Cannot open include file: 'DX12TextureStreamingPolicy.h'`。

- [ ] **Step 3: 写 policy 头文件**

创建 `Src/DX12/DX12TextureStreamingPolicy.h`:

```cpp
#pragma once

#include <cmath>
#include <cstdint>
#include <vector>

// DX12 texture streaming 的纯逻辑内核:不依赖 D3D12,可独立单测。
// 公式逐行镜像 Vulkan 路径(Src/GpuScene.cpp 的 streaming 段,本身镜像
// Metal AAPLTextureManager)。与 Vulkan 的唯一刻意差异:staging 行距按
// D3D12_TEXTURE_DATA_PITCH_ALIGNMENT(256)对齐——PLACED_FOOTPRINT 要求,
// Vulkan 的 tightly-packed staging 无此约束。
namespace DX12TextureStreamingPolicy {

inline constexpr unsigned int PERMANENT_TEXTURE_SIZE = 64;
inline constexpr unsigned int MAX_TEXTURE_SIZE = 4096;

// 资源中出现的 MTLPixelFormat 数值(与 GpuScene.cpp enum / MapMTLToDXGI 同值)。
inline constexpr uint32_t MTLBC1_RGBA = 130;
inline constexpr uint32_t MTLBC1_RGBA_sRGB = 131;
inline constexpr uint32_t MTLBC4_RUnorm = 140;
inline constexpr uint32_t MTLBC4_RSnorm = 141;

// BC 块边长(texels)与块字节数。镜像 GpuScene.cpp::getPixelFormatBlockDesc
// 及其默认分支:非 BC1/BC4 一律 (4,16)——对 BC2/BC3/BC5/BC6H/BC7 均正确。
inline void GetPixelFormatBlockDesc(uint32_t pixelFormat,
                                    uint64_t& blockSize,
                                    uint64_t& bytesPerBlock) {
  blockSize = 4;
  bytesPerBlock = (pixelFormat == MTLBC1_RGBA || pixelFormat == MTLBC1_RGBA_sRGB ||
                   pixelFormat == MTLBC4_RUnorm || pixelFormat == MTLBC4_RSnorm)
                      ? 8 : 16;
}

// 镜像 GpuScene.cpp::calculateMipSizeInBlocks(floor 口径,至少 1 块)。
// 注意:DX12 CreateTextures 现有上传用 ceil((dim+3)/4);4 的倍数尺寸两者一致,
// streaming 路径必须与 Vulkan 的 floor 口径逐字节一致(源数据按此布局)。
inline uint64_t CalculateMipSizeInBlocks(uint64_t size, uint64_t blockSize, uint32_t mip) {
  uint64_t blocksWide = (size / blockSize) > 1 ? (size / blockSize) : 1;
  return (blocksWide >> mip) > 1 ? (blocksWide >> mip) : 1;
}

// 镜像 GpuScene.cpp::calculateMinMip(整数比、截断 log2、clamp 到已有 mip)。
inline uint32_t CalculateMinMip(uint64_t width, uint64_t height, uint64_t mipCount,
                                unsigned int maxSize) {
  uint64_t texSize = width > height ? width : height;
  uint64_t ratio = texSize / maxSize;
  if (ratio < 1) ratio = 1;
  int minMip = static_cast<int>(std::log2(static_cast<float>(ratio)));
  if (minMip >= static_cast<int>(mipCount))
    minMip = static_cast<int>(mipCount) - 1;
  return static_cast<uint32_t>(minMip);
}

// 镜像 GpuScene.cpp::calculateRequiredMip(clamp [topMip, botMip])。
inline uint32_t CalculateRequiredMip(uint64_t width, uint64_t height, uint64_t mipCount,
                                     float screenArea) {
  float topMipTexelArea = static_cast<float>(width * height);
  uint32_t topMip = CalculateMinMip(width, height, mipCount, MAX_TEXTURE_SIZE);
  uint32_t botMip = CalculateMinMip(width, height, mipCount, PERMANENT_TEXTURE_SIZE);

  if (screenArea <= 0.0f) return botMip;
  int mipLevel = static_cast<int>(0.5f * std::log2(topMipTexelArea / screenArea));
  if (mipLevel < static_cast<int>(topMip)) mipLevel = static_cast<int>(topMip);
  if (mipLevel > static_cast<int>(botMip)) mipLevel = static_cast<int>(botMip);
  return static_cast<uint32_t>(mipLevel);
}

// Metal 包围球屏幕面积投影(GpuScene.cpp:5806-5817)。
// viewX/viewY/viewZ: 球心 view 空间坐标;radius: 球半径。
// 只用 x 缩放与 view z,与 NDC Y / 负高度 viewport 无关。
inline float ComputeScreenArea(float viewX, float viewY, float viewZ, float radius,
                               float focalLengthSquared, float viewW, float viewH) {
  if (viewZ <= radius) return viewW * viewH;
  float radiusSquared = radius * radius;
  float z2 = viewZ * viewZ;
  float l2 = viewX * viewX + viewY * viewY + viewZ * viewZ;
  float area = -3.1415926535897932f * focalLengthSquared * radiusSquared *
               std::sqrt(std::fabs((l2 - radiusSquared) / (radiusSquared - z2))) /
               (radiusSquared - z2);
  return area * viewW * viewH * 0.25f;
}

// staging→texture 单条 copy(新 mip),D3D12 PLACED_FOOTPRINT 口径。
struct StreamingBufferCopy {
  uint64_t stagingOffset;  // work item upload buffer 内字节偏移
  uint32_t width, height;  // mip 尺寸(texel)
  uint32_t rowPitch;       // 256 对齐后的行距(staging 内布局)
  uint32_t srcRowPitch;    // 解压数据的紧密行距(拷贝源)
  uint32_t blockRows;      // 块行数
  uint32_t dstSubresource; // 新纹理的 subresource
};

// texture→texture 单条 copy(共享 mip),D3D12 SUBRESOURCE_INDEX 口径。
struct StreamingImageCopy {
  uint32_t srcSubresource; // 旧纹理 subresource
  uint32_t dstSubresource; // 新纹理 subresource
  uint32_t width, height;  // mip 尺寸(texel)
};

struct StreamingCopyPlan {
  std::vector<StreamingBufferCopy> bufferCopies;
  std::vector<StreamingImageCopy> imageCopies;
  uint64_t stagingSize = 0;
  uint32_t newMipCount = 0;
  uint32_t oldMipCount = 0;
};

// 常驻从 currentMip 迁移到 targetMip 的 copy 计划。
// 镜像 GpuScene.cpp::blitThreadFunc 的两个 region 分支(GpuScene.cpp:5584-5649):
// 升级(target<current):上传 [target, current) 段 + old sub m -> new sub m+(current-target)
// 驱逐(target>current):无上传 + old sub m+(target-current) -> new sub m
inline StreamingCopyPlan BuildStreamingCopyPlan(uint64_t width, uint64_t height,
                                                uint64_t mipCount, uint32_t pixelFormat,
                                                uint32_t targetMip, uint32_t currentMip) {
  StreamingCopyPlan plan;
  if (targetMip == currentMip || targetMip >= mipCount || currentMip >= mipCount)
    return plan;

  uint64_t blockSize, bytesPerBlock;
  GetPixelFormatBlockDesc(pixelFormat, blockSize, bytesPerBlock);

  plan.newMipCount = static_cast<uint32_t>(mipCount) - targetMip;
  plan.oldMipCount = static_cast<uint32_t>(mipCount) - currentMip;
  const bool addingMips = targetMip < currentMip;

  if (addingMips) {
    uint64_t offset = 0;
    for (uint32_t m = targetMip; m < currentMip; ++m) {
      uint64_t bw = CalculateMipSizeInBlocks(width, blockSize, m);
      uint64_t bh = CalculateMipSizeInBlocks(height, blockSize, m);
      uint32_t mipW = static_cast<uint32_t>((width >> m) > 0 ? (width >> m) : 1);
      uint32_t mipH = static_cast<uint32_t>((height >> m) > 0 ? (height >> m) : 1);
      uint32_t srcRowPitch = static_cast<uint32_t>(bw * bytesPerBlock);
      uint32_t rowPitch = (srcRowPitch + 255u) & ~255u; // D3D12 pitch alignment
      plan.bufferCopies.push_back({offset, mipW, mipH, rowPitch, srcRowPitch,
                                   static_cast<uint32_t>(bh), m - targetMip});
      offset += static_cast<uint64_t>(rowPitch) * bh;
    }
    plan.stagingSize = offset;

    uint32_t sharedOffset = currentMip - targetMip;
    for (uint32_t m = 0; m < plan.oldMipCount; ++m) {
      uint32_t srcMip = m + currentMip;
      plan.imageCopies.push_back({m, m + sharedOffset,
          static_cast<uint32_t>((width >> srcMip) > 0 ? (width >> srcMip) : 1),
          static_cast<uint32_t>((height >> srcMip) > 0 ? (height >> srcMip) : 1)});
    }
  } else {
    uint32_t dstOffset = targetMip - currentMip;
    for (uint32_t m = 0; m < plan.newMipCount; ++m) {
      uint32_t srcMip = m + targetMip;
      plan.imageCopies.push_back({m + dstOffset, m,
          static_cast<uint32_t>((width >> srcMip) > 0 ? (width >> srcMip) : 1),
          static_cast<uint32_t>((height >> srcMip) > 0 ? (height >> srcMip) : 1)});
    }
  }
  return plan;
}

} // namespace DX12TextureStreamingPolicy
```

- [ ] **Step 4: 运行测试，确认全过**

用 Global Constraints 里的 PowerShell 单测命令。
Expected：退出码 0，无输出（全过）。

- [ ] **Step 5: Commit**

```bash
git add Src/DX12/DX12TextureStreamingPolicy.h Tests/DX12TextureStreamingPolicyTests.cpp
git commit -m "test: add DX12 texture streaming policy kernel"
```

---

### Task 2: CreateTextures 只加载 permanent mip + 拆除 stub

**Files:**
- Modify: `Src/DX12/DX12GpuScene.h:106-115`(entry struct、map、删 staging 桩）
- Modify: `Src/DX12/DX12GpuScene.cpp:412-579`(`CreateTextures`)、`Src/DX12/DX12GpuScene.cpp:1787-1837`（旧 `UpdateTextureStreaming` 桩改为 no-op)

**Interfaces:**
- Consumes: `DX12TextureStreamingPolicy::{CalculateMinMip, PERMANENT_TEXTURE_SIZE}`(Task 1)。
- Produces:
  - `TextureStreamEntry { const AAPLTextureData* desc; uint32_t totalMips, currentMip, requiredMip; bool inFlight; }`
  - `_streamEntries`（与 `_textures` 平行）、`_streamEntryMap`(`pathHash → index`)
  - 成员 `_streamWantUpgrade`(Task 3 起用）

- [ ] **Step 1: 改头文件**

`Src/DX12/DX12GpuScene.h`:
- 顶部前向声明处（现有 `struct AAPLMeshData;` 旁）加 `struct AAPLTextureData;`。
- 替换 `// Texture streaming state` 段（现 106-115 行）为：

```cpp
  // Texture streaming state (parallel to _textures)
  struct TextureStreamEntry {
    const AAPLTextureData* desc = nullptr; // 指向 _applMesh->_textures[k]
    uint32_t totalMips  = 1;  // 完整纹理的总 mip 数
    uint32_t currentMip = 0;  // 当前常驻的最细 mip(0 = 满分辨率)
    uint32_t requiredMip = 0; // 覆盖度计算的本帧需求(MIN 累积)
    bool inFlight = false;    // 已有 streaming work item 在途
  };
  std::vector<TextureStreamEntry> _streamEntries;
  std::unordered_map<uint32_t, size_t> _streamEntryMap; // pathHash -> entry index
  uint32_t _streamWantUpgrade = 0; // 本帧 requiredMip != currentMip 的条目数(观测)
```

（即：删除 `_streamingStagingBuffer`、`_streamingStagingMapped`、`STREAMING_STAGING_SIZE`。)

- [ ] **Step 2: 改造 CreateTextures 的上传循环**

`Src/DX12/DX12GpuScene.cpp` 的 `CreateTextures()`（现 412-521 行区域）：在逐纹理循环内，把「创建全尺寸全 mip 纹理并上传 `[0, mipCount)`」改为「创建 permanent 段纹理并上传 `[permanentMip, mipCount)`」。循环体开头（现 `DXGI_FORMAT format = ...` 之后）插入：

```cpp
    uint32_t permanentMip = DX12TextureStreamingPolicy::CalculateMinMip(
        w, h, mipCount, DX12TextureStreamingPolicy::PERMANENT_TEXTURE_SIZE);
    uint32_t loadedMipCount = mipCount - permanentMip;
    uint32_t baseW = (w >> permanentMip) > 0 ? (w >> permanentMip) : 1;
    uint32_t baseH = (h >> permanentMip) > 0 ? (h >> permanentMip) : 1;
```

并做三处对应修改：
- 纹理创建（现 430-431 行）:`DX12Util::CreateTexture2D(dev, baseW, baseH, format, D3D12_RESOURCE_FLAG_NONE, loadedMipCount, 1, D3D12_RESOURCE_STATE_COPY_DEST)`。
- mip 上传循环（现 434 行）:`for (uint32_t srcMip = permanentMip; srcMip < mipCount; ++srcMip)`,mip 尺寸/偏移全部按 `srcMip` 取，拷贝目标 `dst.SubresourceIndex = srcMip - permanentMip`（循环内原 `mip` 变量重命名，逻辑不变——沿用现有 ceil 行距口径与 `decompressToHeap`)。
- SRV(现 509 行）:`srvDesc.Texture2D.MipLevels = loadedMipCount;`。

文件头部（现 `#include "GpuScene.h"` 附近）加 `#include "DX12TextureStreamingPolicy.h"`。

- [ ] **Step 3: 替换 streaming entry 初始化，删 staging 桩**

`CreateTextures()` 尾部（现 567-576 行）替换为：

```cpp
  // Initialize texture streaming entries (textures start at permanent mip;
  // finer mips stream in via UpdateTextureStreaming, mirroring the Vulkan path)
  _streamEntries.resize(_textures.size());
  _streamEntryMap.clear();
  for (size_t k = 0; k < _textures.size(); ++k) {
    auto& e = _streamEntries[k];
    e.desc = &_applMesh->_textures[k];
    e.totalMips = (uint32_t)e.desc->_mipmapLevelCount;
    e.currentMip = DX12TextureStreamingPolicy::CalculateMinMip(
        e.desc->_width, e.desc->_height, e.totalMips,
        DX12TextureStreamingPolicy::PERMANENT_TEXTURE_SIZE);
    e.requiredMip = e.currentMip;
    e.inFlight = false;
    _streamEntryMap[e.desc->_pathHash] = k;
  }
```

删除紧随其后的 `_streamingStagingBuffer = DX12Util::CreateUploadBuffer(...)` 两行。

- [ ] **Step 4: 旧 UpdateTextureStreaming 桩改 no-op**

把 `UpdateTextureStreaming()`（现 1787-1837 行）整个函数体替换为：

```cpp
// ---- Update Texture Streaming (mip LOD selection) ----
void DX12GpuScene::UpdateTextureStreaming() {
  // 覆盖度计算(Task 3)与 streaming pipeline(Task 4)在此填充。
}
```

（旧的 camDist 启发式 + `WaitForGpu` + `MostDetailedMip` 重写全部删除——它在新的小纹理上会错，且本就被 Task 3/4 取代。)

- [ ] **Step 5: 构建 + 运行验证**

```powershell
cmake --build build-msvc --config Debug --target AdvancedVulkanRendering
```
Expected:0 error。
运行 `.\Bin\AdvancedVulkanRendering.exe --dx12`:
- 场景正常渲染但**全部糊**(64px 水位）——这是本任务的预期中间态，streaming 尚未接线。
- 日志有 `DX12: N textures loaded into bindless heap`，无 d3d12 错误，无 stall（相机移动不再触发 `WaitForGpu`)。

- [ ] **Step 6: Commit**

```bash
git add Src/DX12/DX12GpuScene.h Src/DX12/DX12GpuScene.cpp
git commit -m "feat: load only permanent mips at DX12 texture init"
```

---

### Task 3: 覆盖度计算（requiredMip)+ CPU 侧场景数据

**Files:**
- Modify: `Src/DX12/DX12GpuScene.h`（加 `_subMeshes` / `_cpuMaterials` 成员与前向声明）
- Modify: `Src/DX12/DX12GpuScene.cpp:212-219`（析构）、`Src/DX12/DX12GpuScene.cpp:237-243`(`LoadMeshData`)、`Src/DX12/DX12GpuScene.cpp:298-318`(`CreateBuffers` 材质段）、`UpdateTextureStreaming()`（整体重写）、`RenderImGuiOverlay()` streaming 块（现 3686-3696 行）

**Interfaces:**
- Consumes: Task 1 全部 policy 函数；Task 2 的 `_streamEntries` / `_streamEntryMap`。
- Produces:
  - `AAPLSubMesh* _subMeshes`、`AAPLMaterial* _cpuMaterials`（成员，析构释放）
  - `UpdateTextureStreaming()` 的 reset + coverage + MIN 累积段（Task 4 在其尾部追加 dispatch/process 步骤）

- [ ] **Step 1: 头文件加成员**

`Src/DX12/DX12GpuScene.h`：前向声明区加 `struct AAPLSubMesh; struct AAPLMaterial;`;`_streamEntryMap` 声明之后加：

```cpp
  // CPU 侧场景数据(streaming 覆盖度用;对齐 Vulkan 的 m_SubMeshes / cpuMaterials)
  AAPLSubMesh* _subMeshes = nullptr;     // 解压自 _applMesh->_meshData
  AAPLMaterial* _cpuMaterials = nullptr; // 解压自 _applMesh->_materialData
```

- [ ] **Step 2: LoadMeshData 解压 submeshes**

`LoadMeshData()`(237-243 行）尾部加：

```cpp
  // 保留 submesh 包围球供 streaming 覆盖度计算(对齐 GpuScene.cpp:2773)
  _subMeshes = (AAPLSubMesh*)decompressToHeap(_applMesh->_meshData,
      _applMesh->compressedMeshDataLength).first;
```

- [ ] **Step 3: CreateBuffers 保留 CPU 材质**

`CreateBuffers()` 材质段（现 301-315 行）：把 `free(matData);` 改为 `_cpuMaterials = (AAPLMaterial*)matData; // 保留供 streaming hash 查询,析构释放`。

- [ ] **Step 4: 析构释放**

`~DX12GpuScene()`(212-219 行）在 `delete _applMesh;` 之前加：

```cpp
  free(_subMeshes);
  free(_cpuMaterials);
```

- [ ] **Step 5: 重写 UpdateTextureStreaming 的覆盖度段**

函数体（Task 2 留的 no-op）替换为：

```cpp
// ---- Update Texture Streaming (mip LOD selection) ----
void DX12GpuScene::UpdateTextureStreaming() {
  if (_streamEntries.empty()) return;

  namespace TSP = DX12TextureStreamingPolicy;

  // 1. reset requiredMip 到 permanent(允许驱逐回去的最粗 mip)
  for (auto& e : _streamEntries) {
    e.requiredMip = TSP::CalculateMinMip(e.desc->_width, e.desc->_height,
                                         e.totalMips, TSP::PERMANENT_TEXTURE_SIZE);
  }

  // 2. 逐 submesh 覆盖度(镜像 GpuScene.cpp:5784-5832;只取 proj[0][0] 与
  //    view z,与 NDC Y / 负高度 viewport / mat4 存储约定无关)
  Camera* cam = _mainCamera;
  if (cam && _subMeshes && _applMesh && _cpuMaterials) {
    mat4 viewMatrix = cam->getObjectToCamera();
    float focalLength = cam->getProjectMatrix()[0][0];
    float focalLengthSquared = focalLength * focalLength;
    float viewW = (float)_device.GetWidth();
    float viewH = (float)_device.GetHeight();
    Frustum frustum = cam->getFrustum();

    for (unsigned int i = 0; i < _applMesh->_meshCount; ++i) {
      const AAPLSubMesh& mesh = _subMeshes[i];
      const AAPLSphere& sphere = mesh.boundingSphere;
      if (!SphereInFrustum(frustum, sphere)) continue;

      vec4 viewPos = viewMatrix * vec4(sphere.data.x, sphere.data.y,
                                       sphere.data.z, 1.0f);
      float area = TSP::ComputeScreenArea(viewPos.x, viewPos.y, viewPos.z,
          sphere.data.w, focalLengthSquared, viewW, viewH);

      uint32_t matIndex = mesh.materialIndex;
      if (matIndex >= _applMesh->_materialCount) continue;
      const AAPLMaterial& cpuMat = _cpuMaterials[matIndex];

      // MIN 累积:最近的 submesh 赢(对齐 GpuScene.cpp::setRequiredMip)
      auto require = [&](uint32_t hash) {
        auto it = _streamEntryMap.find(hash);
        if (it == _streamEntryMap.end()) return;
        auto& e = _streamEntries[it->second];
        uint32_t mip = TSP::CalculateRequiredMip(e.desc->_width, e.desc->_height,
                                                 e.totalMips, area);
        if (mip < e.requiredMip) e.requiredMip = mip;
      };
      if (cpuMat.hasBaseColorTexture) require(cpuMat.baseColorTextureHash);
      if (cpuMat.hasNormalMap) require(cpuMat.normalMapHash);
      if (cpuMat.hasMetallicRoughnessTexture) require(cpuMat.metallicRoughnessHash);
      if (cpuMat.hasEmissiveTexture) require(cpuMat.emissiveTextureHash);
    }
  }

  _streamWantUpgrade = 0;
  for (auto& e : _streamEntries)
    if (e.requiredMip != e.currentMip) ++_streamWantUpgrade;
}
```

文件内（`UpdateTextureStreaming` 之前）加 file-static（直抄 `GpuScene.cpp:5351`):

```cpp
// 球-AABB 视锥测试(直抄 GpuScene.cpp:5351 的 file-static)
static bool SphereInFrustum(const Frustum& frustum, const AAPLSphere& sphere) {
  AAPLBoundingBox3 aabb;
  aabb.min.x = sphere.data.x - sphere.data.w;
  aabb.min.y = sphere.data.y - sphere.data.w;
  aabb.min.z = sphere.data.z - sphere.data.w;
  aabb.max.x = sphere.data.x + sphere.data.w;
  aabb.max.y = sphere.data.y + sphere.data.w;
  aabb.max.z = sphere.data.z + sphere.data.w;
  return !frustum.FrustumCull(aabb);
}
```

- [ ] **Step 6: ImGui 加观测行**

`RenderImGuiOverlay()` 的 streaming 块（现 `ImGui::Text("Textures: %u full | %u half | %u low", ...)` 之后）加：

```cpp
    ImGui::Text("Stream want-swap: %u", _streamWantUpgrade);
```

- [ ] **Step 7: 构建 + 运行验证**

```powershell
cmake --build build-msvc --config Debug --target AdvancedVulkanRendering
```
Expected:0 error。
运行 `.\Bin\AdvancedVulkanRendering.exe --dx12`：画面仍全糊（符合预期）；相机走近大型建筑时 `Stream want-swap` 从 0 涨到几百，走远回到 0（全都已在 permanent，远处无新增需求）。

- [ ] **Step 8: Commit**

```bash
git add Src/DX12/DX12GpuScene.h Src/DX12/DX12GpuScene.cpp
git commit -m "feat: compute DX12 streaming mip demand from submesh coverage"
```

---

### Task 4: streaming pipeline（后台线程 + swap + retention)

**Files:**
- Modify: `Src/DX12/DX12GpuScene.h`（方法声明、work struct、线程/同步/retention/streaming cmdlist 成员、includes)
- Modify: `Src/DX12/DX12GpuScene.cpp:79-210`（构造函数尾）、`Src/DX12/DX12GpuScene.cpp:212-219`（析构）、`UpdateTextureStreaming()`（尾部追加 §3-5 步）、新增 4 个函数

**Interfaces:**
- Consumes: Task 1 的 `BuildStreamingCopyPlan` / `StreamingCopyPlan` / `StreamingBufferCopy` / `StreamingImageCopy`;Task 2 的 entry `inFlight`;Task 3 的 `requiredMip`。
- Produces（本计划内最终接口，Task 5 直接读）:
  - `void initTextureStreaming()` / `void shutdownTextureStreaming()` / `void dispatchStreamingRequest(size_t entryIndex)` / `void blitThreadFunc()` / `void processStreamingWork()`
  - `_pendingWorks`(`std::vector<PendingStreamWork>`,护于 `_pendingMutex`)、`_cpuCompletedWorks`(`std::vector<CpuStreamingWork>`,护于 `_cpuWorkMutex`)——Task 5 ImGui 统计用

- [ ] **Step 1: 头文件加声明与成员**

`Src/DX12/DX12GpuScene.h`:
- includes 加 `<thread>` `<mutex>` `<condition_variable>` 与 `"DX12TextureStreamingPolicy.h"`。
- `UpdateTextureStreaming();` 声明（现 288 行）之后加：

```cpp
  // --- Texture streaming pipeline(镜像 Vulkan 五件套;spec §4)---
  void initTextureStreaming();
  void shutdownTextureStreaming();
  void dispatchStreamingRequest(size_t entryIndex);
  void blitThreadFunc();
  void processStreamingWork();

  struct PendingStreamWork {
    size_t entryIndex;
    uint32_t targetMip;
    uint32_t snapshotCurrentMip;           // dispatch 时快照,防与主线程竞态
    ComPtr<ID3D12Resource> snapshotSource; // 旧纹理,保活到 copy 用完
  };

  struct CpuStreamingWork {
    size_t entryIndex = 0;
    ComPtr<ID3D12Resource> newTexture;    // 后台失败时为 nullptr
    ComPtr<ID3D12Resource> staging;       // 纯驱逐时为 nullptr
    ComPtr<ID3D12Resource> sourceTexture; // copy 源(旧纹理)
    uint32_t targetMip = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    DX12TextureStreamingPolicy::StreamingCopyPlan plan;
  };

  std::thread _streamingThread;
  std::mutex _pendingMutex;
  std::condition_variable _pendingCv;
  bool _streamingThreadRunning = false;
  std::vector<PendingStreamWork> _pendingWorks;
  std::vector<CpuStreamingWork> _cpuCompletedWorks;
  std::mutex _cpuWorkMutex;

  // 退役资源(旧纹理 + staging),每帧槽一份;Draw 开头清空——
  // 该槽上一轮 fence 已由 MoveToNextFrame 等过(spec §4)
  std::vector<std::vector<ComPtr<ID3D12Resource>>> _retiredResources;

  ComPtr<ID3D12CommandAllocator> _streamingCmdAllocator;
  ComPtr<ID3D12GraphicsCommandList> _streamingCmdList;
```

- [ ] **Step 2: 实现 init / shutdown / dispatch**

`DX12GpuScene.cpp`(`FlushCommandQueue()` 之后）新增：

```cpp
// ---- Texture streaming pipeline(镜像 GpuScene.cpp 的 Vulkan 路径)----
void DX12GpuScene::initTextureStreaming() {
  _retiredResources.resize(DX12Device::FRAME_COUNT);
  auto* dev = _device.GetDevice();
  DX12Util::ThrowIfFailed(
      dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
          IID_PPV_ARGS(&_streamingCmdAllocator)), "streaming cmd allocator");
  DX12Util::ThrowIfFailed(
      dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
          _streamingCmdAllocator.Get(), nullptr,
          IID_PPV_ARGS(&_streamingCmdList)), "streaming cmd list");
  _streamingCmdList->Close();
  _streamingThreadRunning = true;
  _streamingThread = std::thread(&DX12GpuScene::blitThreadFunc, this);
}

void DX12GpuScene::shutdownTextureStreaming() {
  _streamingThreadRunning = false;
  _pendingCv.notify_all();
  if (_streamingThread.joinable()) _streamingThread.join();
  _pendingWorks.clear();
  _cpuCompletedWorks.clear(); // ComPtr 自动释放
  for (auto& slot : _retiredResources) slot.clear();
}

void DX12GpuScene::dispatchStreamingRequest(size_t entryIndex) {
  auto& entry = _streamEntries[entryIndex];
  PendingStreamWork work;
  work.entryIndex = entryIndex;
  work.targetMip = entry.requiredMip;
  work.snapshotCurrentMip = entry.currentMip;
  work.snapshotSource = _textures[entryIndex];
  // 先入队前置位,防下一帧重复 dispatch(对齐 GpuScene.cpp:5442)
  entry.inFlight = true;
  {
    std::lock_guard<std::mutex> lock(_pendingMutex);
    _pendingWorks.push_back(std::move(work));
  }
  _pendingCv.notify_one();
}
```

构造函数（现 207 行 `CreateScatterResources();` 之后、`spdlog::info("DX12GpuScene initialized");` 之前）加 `initTextureStreaming();`。

析构（`delete _mainCamera;` 之前）加 `shutdownTextureStreaming();`——**必须在 `delete _applMesh;` 之前**（后台线程读 `_applMesh->_textureData`)。

- [ ] **Step 3: 实现后台线程 blitThreadFunc**

紧接其后新增：

```cpp
void DX12GpuScene::blitThreadFunc() {
  // CPU-only:建替代纹理、解压新 mip 进 staging。GPU 命令在主线程录制。
  // (D3D12 device 方法 free-threaded;命令录制/Execute/SRV 重写在主线程。)
  namespace TSP = DX12TextureStreamingPolicy;
  while (_streamingThreadRunning) {
    std::unique_lock<std::mutex> lock(_pendingMutex);
    _pendingCv.wait(lock, [this] {
      return !_pendingWorks.empty() || !_streamingThreadRunning;
    });
    if (!_streamingThreadRunning) break;

    std::vector<PendingStreamWork> workItems;
    std::swap(workItems, _pendingWorks);
    lock.unlock();

    for (auto& work : workItems) {
      auto& entry = _streamEntries[work.entryIndex];
      const AAPLTextureData& desc = *entry.desc;

      CpuStreamingWork cpuWork;
      cpuWork.entryIndex = work.entryIndex;
      cpuWork.targetMip = work.targetMip;
      cpuWork.sourceTexture = work.snapshotSource;
      cpuWork.format = MapMTLToDXGI(desc._pixelFormat);
      cpuWork.plan = TSP::BuildStreamingCopyPlan(desc._width, desc._height,
          desc._mipmapLevelCount, desc._pixelFormat, work.targetMip,
          work.snapshotCurrentMip);

      try {
        uint32_t baseW = (uint32_t)((desc._width >> work.targetMip) > 0
            ? (desc._width >> work.targetMip) : 1);
        uint32_t baseH = (uint32_t)((desc._height >> work.targetMip) > 0
            ? (desc._height >> work.targetMip) : 1);

        D3D12_HEAP_PROPERTIES heapProps = {};
        heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC texDesc = {};
        texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        texDesc.Width = baseW;
        texDesc.Height = baseH;
        texDesc.DepthOrArraySize = 1;
        texDesc.MipLevels = (UINT16)cpuWork.plan.newMipCount;
        texDesc.Format = cpuWork.format;
        texDesc.SampleDesc.Count = 1;
        DX12Util::ThrowIfFailed(
            _device.GetDevice()->CreateCommittedResource(&heapProps,
                D3D12_HEAP_FLAG_NONE, &texDesc, D3D12_RESOURCE_STATE_COPY_DEST,
                nullptr, IID_PPV_ARGS(&cpuWork.newTexture)),
            "streaming: create texture");

        if (cpuWork.plan.stagingSize > 0) {
          cpuWork.staging = DX12Util::CreateUploadBuffer(_device.GetDevice(),
              cpuWork.plan.stagingSize, nullptr);
          void* mapped = nullptr;
          cpuWork.staging->Map(0, nullptr, &mapped);
          if (!mapped) throw std::runtime_error("streaming: staging map failed");
          for (const auto& copy : cpuWork.plan.bufferCopies) {
            uint32_t srcMip = work.targetMip + copy.dstSubresource;
            uint8_t* compressedSrc = (uint8_t*)_applMesh->_textureData +
                desc._pixelDataOffset + desc._mipOffsets[srcMip];
            auto [mipData, mipSize] = decompressToHeap(compressedSrc,
                desc._mipLengths[srcMip]);
            // 紧密行距 -> 256 对齐行距
            for (uint32_t row = 0; row < copy.blockRows; ++row) {
              memcpy((uint8_t*)mapped + copy.stagingOffset + row * copy.rowPitch,
                     (uint8_t*)mipData + row * copy.srcRowPitch, copy.srcRowPitch);
            }
            free(mipData);
          }
          cpuWork.staging->Unmap(0, nullptr);
        }
      } catch (const std::exception& e) {
        spdlog::warn("DX12 streaming: work failed: {}", e.what());
        cpuWork.newTexture.Reset(); // 主线程按失败 work 处理:只复位 inFlight
        cpuWork.staging.Reset();
      }
      {
        std::lock_guard<std::mutex> clock(_cpuWorkMutex);
        _cpuCompletedWorks.push_back(std::move(cpuWork));
      }
    }
  }
}
```

（注意：`plan.bufferCopies` 为空即纯驱逐，`stagingSize == 0`,**不建** upload buffer——Review Focus 第 5 条。)

- [ ] **Step 4: 实现主线程 processStreamingWork**

紧接其后新增：

```cpp
void DX12GpuScene::processStreamingWork() {
  std::vector<CpuStreamingWork> workItems;
  {
    std::lock_guard<std::mutex> lock(_cpuWorkMutex);
    std::swap(workItems, _cpuCompletedWorks);
  }
  if (workItems.empty()) return;

  auto* dev = _device.GetDevice();

  // 动 static(shader-visible)SRV slot 或 transition 旧纹理前必须 GPU 空闲:
  // 所有 in-flight 帧共享这份描述符堆。对等于 Vulkan processStreamingWork
  // 内 endSingleTimeCommands 的 vkQueueWaitIdle——只在真有 work 完成时发生。
  _device.WaitForGpu();

  _streamingCmdAllocator->Reset();
  _streamingCmdList->Reset(_streamingCmdAllocator.Get(), nullptr);

  for (auto& work : workItems) {
    if (!work.newTexture) continue; // 后台失败;inFlight 在下面统一复位

    if (!work.plan.imageCopies.empty() && work.sourceTexture) {
      DX12Util::TransitionBarrier(_streamingCmdList.Get(), work.sourceTexture.Get(),
          D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
    }
    // 共享 mip:old -> new
    for (const auto& copy : work.plan.imageCopies) {
      D3D12_TEXTURE_COPY_LOCATION dst = {};
      dst.pResource = work.newTexture.Get();
      dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
      dst.SubresourceIndex = copy.dstSubresource;
      D3D12_TEXTURE_COPY_LOCATION src = {};
      src.pResource = work.sourceTexture.Get();
      src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
      src.SubresourceIndex = copy.srcSubresource;
      D3D12_BOX box = {};
      box.right = copy.width; box.bottom = copy.height; box.back = 1;
      _streamingCmdList->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
    }
    // 新 mip:staging -> new
    for (const auto& copy : work.plan.bufferCopies) {
      D3D12_TEXTURE_COPY_LOCATION dst = {};
      dst.pResource = work.newTexture.Get();
      dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
      dst.SubresourceIndex = copy.dstSubresource;
      D3D12_TEXTURE_COPY_LOCATION src = {};
      src.pResource = work.staging.Get();
      src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
      src.PlacedFootprint.Offset = copy.stagingOffset;
      src.PlacedFootprint.Footprint.Format = work.format;
      src.PlacedFootprint.Footprint.Width = copy.width;
      src.PlacedFootprint.Footprint.Height = copy.height;
      src.PlacedFootprint.Footprint.Depth = 1;
      src.PlacedFootprint.Footprint.RowPitch = copy.rowPitch;
      _streamingCmdList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    }
    // 新纹理出生即 COPY_DEST,无需进入 barrier;旧纹理不转回(即将退役)
    DX12Util::TransitionBarrier(_streamingCmdList.Get(), work.newTexture.Get(),
        D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
  }

  _streamingCmdList->Close();
  ID3D12CommandList* lists[] = { _streamingCmdList.Get() };
  _device.GetCommandQueue()->ExecuteCommandLists(1, lists);

  // CPU 侧 swap。GPU 跑 copy 时重写 SRV slot 合法:copy 不读描述符;
  // 之后录制的命令列表见到的已是新描述符(spec §4)。
  for (auto& work : workItems) {
    auto& entry = _streamEntries[work.entryIndex];
    entry.inFlight = false;
    if (!work.newTexture) continue;

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = work.format;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MostDetailedMip = 0;
    srvDesc.Texture2D.MipLevels = work.plan.newMipCount;
    dev->CreateShaderResourceView(work.newTexture.Get(), &srvDesc,
        _cbvSrvUavHeap.GetStaticCPU(SRV_BINDLESS_START + (uint32_t)work.entryIndex));

    // 旧纹理 + staging 退役:Draw 开头随帧槽 fence 清空,无需二次 stall
    if (work.sourceTexture)
      _retiredResources[_currentFrame].push_back(work.sourceTexture);
    if (work.staging)
      _retiredResources[_currentFrame].push_back(work.staging);

    _textures[work.entryIndex] = work.newTexture;
    entry.currentMip = work.targetMip;
  }
}
```

- [ ] **Step 5: UpdateTextureStreaming 尾部接 pipeline**

`UpdateTextureStreaming()` 末尾（`_streamWantUpgrade` 统计之后）追加：

```cpp
  // 3. 清空本帧槽的退役资源——必须先于 dispatch/processStreamingWork:
  //    后者会往同一槽压新退役资源(copy 刚提交仍在途)。安全性:Draw 开头
  //    _currentFrame 槽的上轮 fence 已被 MoveToNextFrame 等过(spec §3 步 3)。
  _retiredResources[_currentFrame].clear();

  // 4. dispatch(跳过在途条目)
  for (size_t i = 0; i < _streamEntries.size(); ++i) {
    auto& e = _streamEntries[i];
    if (e.currentMip != e.requiredMip && !e.inFlight)
      dispatchStreamingRequest(i);
  }

  // 5. 消费后台完成的 work(GPU copy + swap,本线程)
  processStreamingWork();
```

- [ ] **Step 6: 构建 + 运行验证（含 Review Focus 检查）**

```powershell
cmake --build build-msvc --config Debug --target AdvancedVulkanRendering
```
Expected:0 error。
运行 `.\Bin\AdvancedVulkanRendering.exe --dx12`，逐项检查：
- 开局糊 → 相机走近建筑，纹理在 1-2 秒内变清晰（升级）；走远重新变糊（驱逐）。ImGui `Textures: full|half|low` 分布随移动变化。
- 快速连续推拉相机（触发 Review Focus #3、#4)：无 crash、无挂死、`want-swap` 计数最终回稳。
- 移动中直接关窗退出（Review Focus #1)：干净退出，无异常对话框。
- 代码评审（Review Focus #2)：确认 `blitThreadFunc` 的 staging 布局全部走 `plan.srcRowPitch/blockRows`(policy 的 floor 口径），与 `decompressToHeap` 输出逐字节一致。

- [ ] **Step 7: Commit**

```bash
git add Src/DX12/DX12GpuScene.h Src/DX12/DX12GpuScene.cpp
git commit -m "feat: stream DX12 texture mips on a background thread"
```

---

### Task 5: 可观测性补全 + 端到端验收

**Files:**
- Modify: `Src/DX12/DX12GpuScene.cpp`(`RenderImGuiOverlay()` streaming 块、`processStreamingWork()` swap 段加日志）

**Interfaces:**
- Consumes: Task 4 的 `_pendingWorks` / `_cpuCompletedWorks` / `_streamEntries`;Task 1 的 `GetPixelFormatBlockDesc` / `CalculateMipSizeInBlocks`。

- [ ] **Step 1: ImGui streaming 块扩展**

`RenderImGuiOverlay()` 的 streaming 块（现 `if (!_streamEntries.empty()) { ... }`)，在 `Stream want-swap` 行之后加：

```cpp
    // 常驻显存估算(CPU 侧近似,不含 256 行距 padding)
    uint64_t residentBytes = 0;
    for (auto& e : _streamEntries) {
      uint64_t bs, bpb;
      DX12TextureStreamingPolicy::GetPixelFormatBlockDesc(e.desc->_pixelFormat, bs, bpb);
      for (uint32_t m = e.currentMip; m < e.totalMips; ++m) {
        residentBytes += DX12TextureStreamingPolicy::CalculateMipSizeInBlocks(
                             e.desc->_width, bs, m) *
                         DX12TextureStreamingPolicy::CalculateMipSizeInBlocks(
                             e.desc->_height, bs, m) * bpb;
      }
    }
    size_t pending = 0, completed = 0, inFlightCount = 0;
    { std::lock_guard<std::mutex> l(_pendingMutex); pending = _pendingWorks.size(); }
    { std::lock_guard<std::mutex> l(_cpuWorkMutex); completed = _cpuCompletedWorks.size(); }
    for (auto& e : _streamEntries) inFlightCount += e.inFlight ? 1 : 0;
    ImGui::Text("TexMem ~%.1f MB", (double)residentBytes / (1024.0 * 1024.0));
    ImGui::Text("Stream: %zu pend / %zu done / %zu inflight",
                pending, completed, inFlightCount);
```

- [ ] **Step 2: swap 日志**

`processStreamingWork()` 的 swap 循环内，在 `entry.currentMip = work.targetMip;` **之前**插入（先记旧值再赋值）:

```cpp
    spdlog::debug("DX12 streaming: tex[{}] mip {} -> {}", work.entryIndex,
                  entry.currentMip, work.targetMip);
```

- [ ] **Step 3: 构建 + 端到端验收清单**

```powershell
cmake --build build-msvc --config Debug --target AdvancedVulkanRendering
```
运行 `.\Bin\AdvancedVulkanRendering.exe --dx12`，逐条过：
- [ ] 开局 `TexMem` 在低位（几十 MB 量级，非全量 GB)；开局画面糊。
- [ ] 走近建筑：纹理变清晰，`TexMem` 上升；`full` 计数增加。
- [ ] 走远：`TexMem` 回落，`low` 计数增加（驱逐生效）。
- [ ] 静止数秒后 `pend/done/inflight` 全部归 0(quiesce)。
- [ ] 拖动改窗口大小（Review Focus #5):resize 后渲染正常，streaming 继续。
- [ ] 日志出现 `DX12 streaming: tex[k] mip a -> b` 行；无 warn/error。

- [ ] **Step 4: Commit**

```bash
git add Src/DX12/DX12GpuScene.cpp
git commit -m "feat: surface DX12 texture streaming stats in imgui"
```

---

## Self-Review 记录

**Spec 覆盖**:§1 五件套→Task 4;§2 启动路径→Task 2;§3 覆盖度+CPU 数据→Task 3;§4 swap/同步/retention→Task 4;§5 错误处理/关闭→Task 4(catch + shutdown);§6 观测/测试→Task 1 + Task 5;影响文件表 4 个文件→Task 1-5 全覆盖。无遗漏。

**Placeholder 扫描**：所有代码步骤含完整代码；无 TBD/TODO/“类似 Task N”。

**类型一致性**:`StreamingBufferCopy/StreamingImageCopy/StreamingCopyPlan` 字段名在 Task 1 定义、Task 4/5 使用一致（`stagingOffset/rowPitch/srcRowPitch/blockRows/dstSubresource`、`srcSubresource/dstSubresource/width/height`、`bufferCopies/imageCopies/stagingSize/newMipCount/oldMipCount`);entry 字段 `desc/totalMips/currentMip/requiredMip/inFlight` 在 Task 2 定义、Task 3/4/5 使用一致；`_streamWantUpgrade` Task 2 声明、Task 3 写入；`_subMeshes/_cpuMaterials` Task 3 定义并释放。
