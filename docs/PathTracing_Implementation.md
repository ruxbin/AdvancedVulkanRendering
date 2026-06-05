# Path Tracing 升级与修复

记录从 `pathtracing` 分支 merge 到 `texture_streaming` 后，发现的不一致问题与对应的修复实施。

---

## 起因：合并冲突保留 ours 导致 C++ / shader 严重错位

把 `pathtracing` 分支合并到 `texture_streaming` 时，3 个冲突文件保留了 ours：

| 文件 | 保留版本 |
|------|----------|
| `shaders/rt_lighting.hlsl` | 旧的单次 ray tracing（无 accumulation、无多 bounce） |
| `shaders/rt_lighting.lib.spv` | 编译自上面这个旧 shader |
| `shaders/compile_shaders.bat` | 用 VulkanSDK 的 dxc 路径 |

但 C++ 端（`Src/Raytracing.cpp`、`Src/Include/Raytracing.h`、`Src/GpuScene.cpp`）已经完整切换为 path tracing 版（accumulation buffer、camera dirty 检测、ImGui PT 控件、PT 风格 32-byte push constants）。

### 后果

| 问题 | 影响 |
|------|------|
| Push constant 字段错位（C++ 写 `accumCount` ↔ shader 读 `shadowTaps`） | 相机静止越久 shadow ray 数线性爆炸 |
| Shader 没声明 binding 12 但 C++ 写了 outAccumColor descriptor | accumulation buffer 完全无人使用，C++ PT 代码全部死代码 |
| `Src/GpuScene.cpp:3518-3521` 的 `Init/BuildAS/CreateOutput/CreatePipelineAndSBT` 全部注释掉 | RT 路径事实上从未跑过；打开 `useRayTracing` toggle 立刻 crash |
| 硬编码 fovY=60° (`Raytracing.cpp:961`)，camera 实际 65° | mip 计算偏差 |
| Swapchain resize 不重建 RT 资源 | resize 后越界写 |

---

## 解决方向

**方向 A：升级到完整 path tracing**——多 bounce + 渐进累积 + BRDF importance sampling + NEE + ACES tone map。让现有 C++ 端代码真正能跑，并补齐 shader 替换、resize、FOV、camera dirty 等漏洞。

---

## Phase 0：取消 RT init 注释

**文件**：`Src/GpuScene.cpp:3516-3522`

取消 `_raytracing->Init()`、`BuildAccelerationStructures()`、`CreateOutputImagesAndDescriptorSet()`、`CreatePipelineAndSBT()` 四行注释。守卫 `if (!_raytracing)` 保证只跑一次。顺序在 `_lightCuller` 之后，descriptor 写入需要的 `GetPointLightCullingDataBuffer` 已可用。

---

## Phase 1：替换 path tracing shader

**文件**：`shaders/rt_lighting.hlsl`（整文件覆盖）

### 1.1 用 pathtracing 分支版本作为基础

13 binding + RTPC + 6 个 stage（RayGen / MissPrimary / MissShadow / ClosestHitPrimary / AnyHitAlpha / AnyHitAlphaShadow）与 C++ 端完全对齐。

新增组件：
- PCG 随机数生成器
- Smith GGX + Lambertian BRDF（`evalBRDF`）
- BRDF importance sampling（按 Fresnel 概率二选一：cosine-weighted diffuse vs GGX specular）
- 多 bounce 迭代循环（在 RayGen 内）
- 太阳 NEE（每 bounce 一条 shadow ray）
- Russian Roulette（≥2 bounce 启动）
- Progressive accumulation（lerp 1/(N+1)）
- ACES filmic tone map
- Sky horizon-zenith 渐变

### 1.2 vbPositions/Normals/Tangents 改回 ByteAddressBuffer

**理由**：pathtracing 分支用 `StructuredBuffer<float3>`，DXC SPIRV 后端默认对 vec3 padding 到 16-byte stride；而 BLAS 用 `vertexStride=12` 紧凑布局（`Raytracing.cpp:162`），会让 `gatherHit` 读到错位法线/切线（黑斑/firefly/NaN）。

```hlsl
[[vk::binding(2,1)]] ByteAddressBuffer vbPositions;
[[vk::binding(3,1)]] ByteAddressBuffer vbNormals;
[[vk::binding(4,1)]] ByteAddressBuffer vbTangents;
[[vk::binding(5,1)]] StructuredBuffer<float2> vbUVs;  // float2 不会被 padding，保留
```

`gatherHit` 中 6 行采样改为 `asfloat(vbNormals.Load3(idx.x*12))` 等。

### 1.3 Sky color 改用 `frameConstants.skyColor`

与光栅 deferred path（`shaders/deferredlighting.hlsl:139`）一致：

```hlsl
float3 skyColor(float3 dir) {
    float t = saturate(dir.y * 0.5f + 0.5f);
    return lerp(frameConstants.skyColor * 0.6f, frameConstants.skyColor, t * t);
}
```

### 1.4 NonUniformResourceIndex

pathtracing 版省略，加回 4 处 bindless 贴图采样（albedo / roughness / normal / emissive），与 raster 路径一致避开 spec UB。

### 1.5 Gram-Schmidt 切线正交化

保留旧 shader 的处理，避免顶点 tangent 不严格正交导致 TBN 矩阵非正交：

```hlsl
half3 geotan = (half3)normalize(h.geoT - dot(h.geoT, h.geoN) * h.geoN);
```

### 1.6 RNG 种子去掉硬编码 1920

```hlsl
uint rng = (px.x * dim.y + px.y) ^ (pc.frameSeed * 2654435761u)
                                 ^ (pc.accumCount * 805459861u);
```

避免不同分辨率下抖动周期不一致。

### 1.7 Push constant 同步注释

shader 顶部加 "Keep RTPushConsts in sync with Src/Raytracing.cpp local RTPC struct (32 bytes)"。

---

## Phase 2：重新编译 .spv

**命令**：

```bat
dxc -spirv -T lib_6_3 rt_lighting.hlsl -fspv-target-env=vulkan1.2
    -fspv-extension=SPV_KHR_ray_tracing
    -fspv-extension=SPV_KHR_physical_storage_buffer
    -fspv-extension=SPV_KHR_non_semantic_info
    -fspv-extension=SPV_EXT_descriptor_indexing
    -Fo rt_lighting.lib.spv
```

成功，spv 文件 12992 → 28228 bytes（PT 路径有更多代码，符合预期）。

---

## Phase 3：Swapchain resize 支持

**文件**：`Src/Include/Raytracing.h`、`Src/Raytracing.cpp`、`Src/GpuScene.cpp`

### 3.1 策略

- **保留**（与分辨率无关）：BLAS / TLAS / pipeline / SBT / `_rtSetLayout` / `_rtDescriptorPool` / `_rtImguiPass`
- **重建**（尺寸相关）：`_rtLitImage*` / `_accumImage*` / `_rtImguiFrameBuffer`
- **重写**（不重新分配）：descriptor set binding 1（outLitColor）和 binding 12（outAccumColor）

### 3.2 新增方法

```cpp
class RayTracing {
public:
    void destroySizeDependentResources();
    void createSizeDependentResources();
};
```

`destroySizeDependentResources()` 释放 `_rtLitImage*` / `_accumImage*` / framebuffers，把 handle 置为 `VK_NULL_HANDLE`（避免析构函数二次释放）。

`createSizeDependentResources()` 重新分配三组资源、把 accum image 预转 GENERAL、用现有 `_rtImguiPass` 创建新 framebuffer、重写 binding 1+12、`_accumCount = 0`。

### 3.3 GpuScene 接入

- `cleanupSwapChainResources()`（行 4189）末尾：
  ```cpp
  if (_raytracing && _raytracing->IsBuilt()) {
      _raytracing->destroySizeDependentResources();
  }
  ```
- `recreateSwapChainResources()`（行 4271）：同样的 hook，调 `createSizeDependentResources()`。

`IsBuilt()` 守卫：第一次 resize 在 lazy init 之前就触发时，TLAS 还没建好，跳过即可。

---

## Phase 4：review 暴露的小问题修复

### 4.1 Hardcoded fovY=60° → 真实 FOV

**改动**：

```cpp
// Src/Include/Camera.h
float _fov;  // vertical FOV in radians
float Fov() const { return _fov; }

// Src/Camera.cpp
Camera::Camera(float fov, ...) {
    _fov = fov;  // 新增
    // ...
}

// Src/Raytracing.cpp:973
pc.pixelSpreadAngle = 2.0f * std::tan(0.5f * _scene.maincamera->Fov())
                      / float(extent.height);
```

对 raster 路径零影响（raster 直接读 projection matrix，不读 `_fov`）。

### 4.2 Camera dirty 含 projection

**改动**：

```cpp
// Raytracing.h
mat4 _prevViewProjMatrix{};   // 改名

// Raytracing.cpp:898-908
mat4 view = _scene.maincamera->getObjectToCamera();
mat4 proj = _scene.maincamera->getProjectMatrix();
mat4 currentVP = view * proj;  // mat4 quirk: A*B 实际算 B*A
bool cameraMoved = (std::memcmp(&currentVP, &_prevViewProjMatrix, sizeof(mat4)) != 0);
if (cameraMoved) {
    _accumCount = 0;
    _prevViewProjMatrix = currentVP;
}
```

未来 FOV 滑动调整也能正确触发 reset。

### 4.3 frameSeed 与 accumCount 联动 hash

```cpp
pc.frameSeed = _scene.frameConstants.frameCounter * 2654435761u
             + _accumCount * 1597334677u;
```

shader 端已经做了 hash，C++ 加属于双保险。

### 4.4 RTPC 加 static_assert

```cpp
static_assert(sizeof(RTPC) == 32, "RTPC must match RTPushConsts in rt_lighting.hlsl");
static_assert(offsetof(RTPC, accumCount)       == 4,  "RTPC layout drift");
static_assert(offsetof(RTPC, sunConeRadius)    == 8,  "RTPC layout drift");
static_assert(offsetof(RTPC, pixelSpreadAngle) == 12, "RTPC layout drift");
static_assert(offsetof(RTPC, frameSeed)        == 16, "RTPC layout drift");
static_assert(offsetof(RTPC, maxBounces)       == 20, "RTPC layout drift");
static_assert(offsetof(RTPC, resetAccum)       == 24, "RTPC layout drift");
```

未来再有人修改 RTPC 字段就会立即编译失败，避免本次的合并惨案重演。

---

## 改动文件清单

| 文件 | Phase | 改动概要 |
|------|-------|---------|
| `Src/GpuScene.cpp:3516-3522` | 0 | 取消 4 行 init 注释 |
| `Src/GpuScene.cpp:4189, 4271` | 3 | 接入 RT cleanup / recreate hooks |
| `shaders/rt_lighting.hlsl` | 1 | 整体替换为 PT 版 + ByteAddressBuffer + sky color + NonUniformResourceIndex + Gram-Schmidt（+303 / -200 行） |
| `shaders/rt_lighting.lib.spv` | 2 | 重新编译（12992 → 28228 bytes） |
| `Src/Raytracing.cpp` | 3, 4 | viewProj dirty + 真实 FOV + frameSeed hash + 7 个 static_assert + 新增两个 size-dependent 资源管理方法（共 +200 行） |
| `Src/Include/Raytracing.h` | 3, 4 | 加 `destroy/createSizeDependentResources()`、`_prevViewProjMatrix` |
| `Src/Include/Camera.h` | 4.1 | 加 `_fov` 字段 + `Fov()` getter |
| `Src/Camera.cpp:23` | 4.1 | ctor 中保存 fov |

---

## 高层设计决策

1. **优先级**：Phase 0/1/2 是基本盘（让 RT 路径真能跑），Phase 3/4 是 robustness 补丁。Phase 0 没做完，后面全是空中楼阁。

2. **shader 替换 vs 重写**：直接用 pathtracing 分支版本，**仅修补 vbXxx 的 stride 问题**。pathtracing 分支用 `StructuredBuffer<float3>` 在 BLAS 紧凑布局下是潜在 bug，改回旧 shader 的 `ByteAddressBuffer + Load3(idx*12)` 模式（同样代码已验证可用），最稳妥。

3. **Resize 选择只重建尺寸相关资源**：BLAS/TLAS/pipeline/SBT/descriptor pool/render pass 都和分辨率无关，保留它们能避免 RT 整套重建。模式与现有 `cleanup/recreateSwapChainResources` 完全一致，不引入新 lifecycle 概念。

4. **Camera 加 `_fov` 字段**：比从 invProjection 反算更清晰，且 raster 路径零影响。

5. **static_assert 防御性编程**：根本原因是没有 C++/HLSL 共享 header；`RTPC` 是本地 struct。最低成本的防御就是 `offsetof + static_assert`，既不引入新文件也不重构 build system。

---

## 验证

- ✅ MSVC 编译通过（无报错，生成 `Bin/AdvancedVulkanRendering.exe`）
- ✅ DXC 编译 PT shader 无 error/warning
- ⏳ 运行验证（待手工跑）：
  - 勾选 ImGui 的 "Ray Tracing" toggle 不应 crash
  - 静止相机时 "Samples accumulated" 计数上升、画面逐帧变干净
  - 相机移动时 counter 重置 → 重新收敛
  - "Max Bounces" slider 1 → 8 时 GI 强度变化
  - 拖动 window resize 不应 crash，counter 重置
  - RenderDoc 检查 push constant 偏移 4 是 accumCount 小值（0~1000），不是百万级
