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

---

## 第二轮修复：RT 路径首次运行 validation 报错

第一轮实施完成后实际跑起来，遇到两类 Vulkan validation error：

### Bug A: render-pass 不兼容（VUID-vkCmdDrawIndexed-renderPass-02684）

```
pAttachments[0].format (VK_FORMAT_B8G8R8A8_UNORM)
   != pAttachments[0].format (VK_FORMAT_R16G16B16A16_SFLOAT)
```

**根因**：ImGui pipeline 在初始化时绑定到 `_forwardLightingPass`（color = R16G16B16A16_SFLOAT），但 `_rtImguiPass` 用 `_device.getSwapChainImageFormat()`（BGRA8）。Render-pass-compatibility 要求 attachment format 必须一致 → 不兼容 → 用 ImGui pipeline draw 在 `_rtImguiPass` 内时报错。

### Bug B: depth layout 不一致（DrawState-InvalidImageLayout）

```
expects layout DEPTH_STENCIL_ATTACHMENT_OPTIMAL
instead, current layout is DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL
```

**根因**：上一帧 raster path 跑完后 forward pass 把 depth 转成 `DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL`（其 finalLayout）。下一帧 RT path 启动 `_rtImguiPass` 时 initialLayout 写的是 `DEPTH_STENCIL_ATTACHMENT_OPTIMAL` → 不匹配。

### 修复策略

让 `_rtImguiPass` 与 `_forwardLightingPass` **真正 render-pass-compatible**，并把 ImGui 画到 `_hdrLightingBuffer`（HDR R16）而不是 swapchain。最后多一步 hdr → swapchain blit。

#### 1. `_rtImguiPass` attachment 修改

```cpp
colorAtt.format = VK_FORMAT_R16G16B16A16_SFLOAT;       // 改：原 swapchain format
colorAtt.initialLayout = COLOR_ATTACHMENT_OPTIMAL;
colorAtt.finalLayout   = COLOR_ATTACHMENT_OPTIMAL;     // 改：ImGui 画完后保持

depthAtt.loadOp = LOAD_OP_DONT_CARE;                   // 改：ImGui 不读 depth
depthAtt.initialLayout = UNDEFINED;                    // 改：接受任何 prior layout
depthAtt.finalLayout = DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
```

#### 2. Framebuffer 改成绑定 hdrLightingBuffer

```cpp
std::array<VkImageView, 2> views = {
    _scene._hdrLightingBufferView,                     // 改：原 swapchain view
    _device.getWindowDepthImageView(f)
};
```

`createSizeDependentResources()` 中 framebuffer 创建同样改。

#### 3. `RecordBlitToSwapchain` 重写为"blit 到 hdr"

不再 blit 到 swapchain，改成：
- `_rtLitImage` GENERAL → TRANSFER_SRC
- `_hdrLightingBuffer` UNDEFINED → TRANSFER_DST
- blit
- `_hdrLightingBuffer` TRANSFER_DST → COLOR_ATTACHMENT_OPTIMAL（为 ImGui pass 准备）

#### 4. 新增 `RecordHdrToSwapchain`

ImGui 画完后调用：
- `_hdrLightingBuffer` COLOR_ATTACHMENT → TRANSFER_SRC
- swapchain UNDEFINED → TRANSFER_DST
- blit hdr → swapchain
- swapchain TRANSFER_DST → PRESENT_SRC_KHR
- **`_hdrLightingBuffer` TRANSFER_SRC → COLOR_ATTACHMENT_OPTIMAL**（关键：让用户 toggle RT off 后下一帧 forward pass 仍能 LOAD）

#### 5. GpuScene RT 调用链

```cpp
_raytracing->RecordTraceRays(...);
_raytracing->RecordBlitToSwapchain(...);   // 现在是 blit 到 hdr
_raytracing->BeginImGuiCompositePass(...);
renderImGuiOverlay(...);                    // ImGui 画到 hdr
_raytracing->EndImGuiCompositePass(...);
_raytracing->RecordHdrToSwapchain(...);    // 新增：hdr → swap + transitions
```

### 关键设计点

让 `_rtImguiPass` 与 `_forwardLightingPass` **format-level 兼容**比"做一份独立的 RT-only ImGui pipeline"简单得多。代价是 RT 路径多一次 blit（rt → hdr → swap 而不是 rt → swap），但 1080p 量级几乎可以忽略。

最后一步把 hdr 转回 `COLOR_ATTACHMENT_OPTIMAL` 是关键：raster path 的 `_forwardLightingPass.color.initialLayout = COLOR_ATTACHMENT_OPTIMAL`，所以 RT→raster 切换不会因 layout 不一致再报错。

### 修改文件

| 文件 | 改动 |
|------|------|
| `Src/Raytracing.cpp` | `_rtImguiPass` attachment 改 R16 + UNDEFINED depth；framebuffer 绑 hdr view（两处：one-time init 和 resize）；`RecordBlitToSwapchain` 改写；新增 `RecordHdrToSwapchain` |
| `Src/Include/Raytracing.h` | 加 `RecordHdrToSwapchain` 声明 |
| `Src/GpuScene.cpp:3613-3625` | 调用链末尾加 `RecordHdrToSwapchain` |

---

## 第三轮修复：texture streaming 导致 RT descriptor stale → device lost

### 现象

修完 render pass 兼容、补好 hdr buffer 的 TRANSFER_SRC/DST usage 之后，运行时**第一帧能看到正确的 albedo，但接着立刻 device lost**（`vkQueueSubmit` 返回 `-4`，nvlddmkm Event ID 153）。

### 根因

`texture_streaming` 分支后台线程会动态替换 `textures[i]` 中的 `VkImageView`（升 / 降 mip 时新建 view + 销毁旧 view）。

raster path 通过 `streamingDescriptorsDirtyMask`（`Src/GpuScene.cpp:3386-3404`）每帧检查并 patch 自己的 `applDescriptorSets[currentFrame]`，但 **RT 的 `_rtDescriptorSets` 在 `CreateOutputImagesAndDescriptorSet()` 写入一次后就再也没刷新**。

时间线：
1. 用户开 RT toggle → lazy init → RT descriptor binding 10 写入当下的 textures view 数组
2. 几帧后 streaming 完成一次 swap → 旧 VkImageView 销毁
3. RT 路径下一次 dispatch → AnyHit / ClosestHit 通过 `_Textures[i].SampleLevel(...)` 访问已销毁的 view → GPU page fault → device lost

完美解释了"第一帧 OK，立刻就崩"——descriptor 写入瞬间还有效，等 streaming 一触发 swap 立刻全废。

### 修复

#### 1. 在 `RayTracing` 中加刷新方法

```cpp
// Src/Raytracing.cpp
void RayTracing::RefreshTextureDescriptors(uint32_t imageIndex) {
  if (_rtDescriptorSets.empty() || imageIndex >= _rtDescriptorSets.size()) return;
  const uint32_t bindlessCount = (uint32_t)_scene.textures.size();
  if (bindlessCount == 0) return;

  std::vector<VkDescriptorImageInfo> texImgs(bindlessCount);
  for (uint32_t i = 0; i < bindlessCount; ++i) {
    texImgs[i].imageView   = _scene.textures[i].second;
    texImgs[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    texImgs[i].sampler     = VK_NULL_HANDLE;
  }
  VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
  w.dstSet          = _rtDescriptorSets[imageIndex];
  w.dstBinding      = 10;
  w.descriptorCount = bindlessCount;
  w.descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  w.pImageInfo      = texImgs.data();
  vkUpdateDescriptorSets(_device.getLogicalDevice(), 1, &w, 0, nullptr);
}
```

binding 10 已经设了 `VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT_EXT`，所以可以热更新。

#### 2. 加独立的 RT dirty mask（按 swapchain image 索引）

`streamingDescriptorsDirtyMask` 是按 `framesInFlight`（通常 2）维度，但 RT descriptor sets 是按 swapchain image count（通常 3）。索引域不一样，必须分开。

```cpp
// Src/Include/GpuScene.h
uint32_t streamingDescriptorsDirtyMask = 0;            // 已存在，raster 用
uint32_t rtStreamingDescriptorsDirtyMask = 0;          // 新增，RT 用
```

#### 3. swap 完成后同时设两个 mask 全部位

```cpp
// Src/GpuScene.cpp:5741-5745（streaming swap 完成处）
streamingDescriptorsDirtyMask = (framesInFlight >= 32) ? ~0u : ((1u << framesInFlight) - 1u);
const uint32_t rtN = device.getSwapChainImageCount();
rtStreamingDescriptorsDirtyMask = (rtN >= 32) ? ~0u : ((1u << rtN) - 1u);
```

#### 4. RT 路径首步检查 + 刷新对应 imageIndex

```cpp
// Src/GpuScene.cpp:3613-3621
if (useRayTracing) {
    if (rtStreamingDescriptorsDirtyMask & (1u << imageIndex)) {
      _raytracing->RefreshTextureDescriptors((uint32_t)imageIndex);
      rtStreamingDescriptorsDirtyMask &= ~(1u << imageIndex);
    }
    _raytracing->RecordTraceRays(...);
    ...
}
```

### 关键设计点

raster 和 RT 的描述符**索引域不同**（frame-in-flight vs swapchain-image-index），所以**dirty mask 必须分两份**。raster 那个不能直接复用——raster 已经"自己刷新过"会清掉 mask 位，但 RT 这边可能还没轮到。

### 修改文件

| 文件 | 改动 |
|------|------|
| `Src/Include/Raytracing.h` | 加 `RefreshTextureDescriptors(imageIndex)` 声明 |
| `Src/Raytracing.cpp` | 实现 `RefreshTextureDescriptors`（重写 binding 10） |
| `Src/Include/GpuScene.h:347-349` | 加 `rtStreamingDescriptorsDirtyMask` |
| `Src/GpuScene.cpp:3613-3621` | RT 路径首步条件刷新 |
| `Src/GpuScene.cpp:5741-5745` | streaming swap 完成处同时设 RT mask |

---

## 第四轮修复：accumulation buffer 误建成 per-swapchain-image → 画面抖动

### 现象

texture stale 修完后能稳定不崩了，但**画面剧烈抖动，看不到 progressive accumulation 收敛过程**。理论上相机静止时 PT 输出应该越来越平滑，实际却像每帧独立 stochastic 渲染，从未"积累"。

### 根因

```cpp
// 误：N 张 image，每张独立累积
std::vector<VkImage>        _accumImage;       // size = swapChainImageCount
std::vector<VkImageView>    _accumImageView;
std::vector<VkDeviceMemory> _accumMemory;
```

descriptor binding 12（outAccumColor）按 imageIndex 各绑各的：
- 第 N 个 swap image 的 RT descriptor → 第 N 张 accum image
- 第 N+1 个 swap image 的 RT descriptor → 第 N+1 张 accum image

所以 frame N 写 `_accumImage[2]`，frame N+1 写 `_accumImage[0]`，frame N+2 写 `_accumImage[1]`——**三张完全独立累积**！

更糟的是，`_accumCount` 是单一全局计数器：

```hlsl
float t = 1.0f / float(pc.accumCount + 1u);
accumulated = lerp(prev, radiance, t);
```

frame N+1 读 `_accumImage[0]`（包含 3 帧前的内容），但 lerp 权重用的是 `_accumCount`（已经 = 3）→ 权重和 prev 内容**完全不匹配**，画面在 N 张 image 之间疯狂跳变。

### 修复

把 accumulation buffer 改成**单张共享**：

```cpp
// Src/Include/Raytracing.h
VkImage        _accumImage     = VK_NULL_HANDLE;
VkDeviceMemory _accumMemory    = VK_NULL_HANDLE;
VkImageView    _accumImageView = VK_NULL_HANDLE;
```

所有 N 个 RT descriptor set 的 binding 12 全指向同一个 view：

```cpp
// Src/Raytracing.cpp:CreateOutputImagesAndDescriptorSet
VkDescriptorImageInfo accumImg{};
accumImg.imageView   = _accumImageView;          // 不再是 _accumImageView[f]
accumImg.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
```

`RecordTraceRays` 起始处的 RT_SHADER → RT_SHADER barrier 直接用单张 image：

```cpp
accumBarrier.image = _accumImage;                // 不再是 _accumImage[imageIndex]
```

### 跨帧同步说明

单张 image 跨所有 frame 共享，需要保证 frame N+1 不和 frame N 同时读写。两层保护已经够：

1. **CPU 端**：每帧开始 `vkWaitForFences` 等本 frame slot 的 in-flight fence。framesInFlight=2 时，frame N+2 启动前 frame N 必已完成
2. **GPU 端**：`RecordTraceRays` 入口的 image barrier `oldLayout=GENERAL → newLayout=GENERAL, srcAccess=SHADER_WRITE, dstAccess=SHADER_READ|SHADER_WRITE, srcStage=RT_SHADER, dstStage=RT_SHADER` 把上一帧 RT 写入对当前帧可见

### 关键设计点

**accumulation buffer 必须是逻辑单一资源**——它的语义是"全程累积同一画面"。per-frame-in-flight 拆分是 *output* 资源（每帧渲染独立结果用）的标准模式，但对 accumulation 这种**跨帧累积**资源是反模式。

类似教训：output / framebuffer / 临时 attachment 通常按 frame-in-flight 或 swapchain-image 拆；持久状态（accumulation, history buffer for TAA, persistent g-buffer 等）必须单张共享并配合显式跨帧 barrier。

### 修改文件

| 文件 | 改动 |
|------|------|
| `Src/Include/Raytracing.h` | `_accumImage / _accumMemory / _accumImageView` 从 vector 改成单一 handle |
| `Src/Raytracing.cpp` 析构函数 | 销毁单张 |
| `Src/Raytracing.cpp` `CreateOutputImagesAndDescriptorSet` | 创建单张 + pre-transition GENERAL；descriptor 写 binding 12 时用单 view |
| `Src/Raytracing.cpp` `destroy/createSizeDependentResources` | 销毁/重建单张；resize descriptor rewrite 同样用单 view |
| `Src/Raytracing.cpp` `RecordTraceRays` | accumBarrier image 改单张 |

---

## 关于 sun direction 符号

第四轮调试期间还修了一个独立的 shader bug：`sampleSunDir` 用了 `-frameConstants.sunDirection`，导致 NEE shadow ray 朝远离太阳方向打 → 大部分被地面挡住 → 阴影区一片漆黑（只剩 emissive 物体亮）。

参照 `lighting.hlsl:63` 的 raster 用法：

```hlsl
half3 lightDirection = (half3) frameData.sunDirection;
```

raster 直接把 `sunDirection` 当 wi 用，证明它已经是 surface→sun 方向。**PT 不应再加负号**：

```hlsl
// shaders/rt_lighting.hlsl
float3 sunAxis = normalize(frameConstants.sunDirection);  // 去掉了 '-'
```
