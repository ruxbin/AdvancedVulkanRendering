# Ray Tracing 改造记录

> 本文件持续记录 raytracing 分支上为引入硬件光追所做的所有改动，方便回溯。

## 总体目标

加入一条**完整的 RT 路径**（VK_KHR_ray_tracing_pipeline）：
- 直接从相机发射 primary ray，命中后在 closest-hit 取材质 / 法线 / UV
- raygen 算 BRDF（复用 `lighting.hlsl::lightingShader`）+ N-tap sun shadow + 本地 point/spot 灯遍历
- 与原 deferred 路径运行时 toggle，二选一
- Alpha-mask 用 any-hit 处理；纹理 LOD 用 ray cone 推导 mip
- 透明物体、cluster point/spot 灯、SAO、cascade shadow 在 RT 路径下都跳过

## 帧图

```
useRayTracing == true:
    [TraceRays]  → _rtLitImage[frame]    (HDR R16G16B16A16_SFLOAT, STORAGE)
    [Blit]       → swapchain[imageIndex]
    [ImGui]      → swapchain
useRayTracing == false:
    保持原 deferred pipeline
```

## SBT / Hit-Group 布局

| 区域 | 记录数 | 内容 |
|---|---|---|
| raygen | 1 | RayGen |
| miss   | 2 | MissPrimary, MissShadow |
| hit    | `(opaqueChunks + alphaChunks) * 2` | 每 geometry 两条：[primary HG, shadow HG] |

每 geometry：
- record `g*2 + 0` → 主路径 hit-group（opaque 或 alpha 取决于 `g < _opaqueChunkCount`）
- record `g*2 + 1` → 阴影路径 hit-group

`hitShaderStride = 2`；`TraceRay(primary)` 传 `hitGroupOffset=0, missIndex=0`，`TraceRay(shadow)` 传 `hitGroupOffset=1, missIndex=1`。

4 个 hit-group：
- `HG_PRI_OPAQUE` = closest-hit-primary
- `HG_PRI_ALPHA`  = closest-hit-primary + any-hit-alpha
- `HG_SHA_OPAQUE` = （空，shadow ray 用 SKIP_CLOSEST_HIT）
- `HG_SHA_ALPHA`  = any-hit-alpha

## 关键设计细节

- **TLAS = 1 个 instance**（identity）；BLAS 内每 chunk 一个 geometry，`_opaqueChunkCount` 个标 OPAQUE，剩余 `_alphaMaskedChunkCount` 个不标
- **payload**：primary 64 字节量级（wsPos / normal / albedo / F0 / roughness / emissive / alpha / hit / hitT），shadow 4 字节（visible）
- **N-tap 软阴影**：默认 4 ray，Hammersley + sun cone（半角 0.5°），origin offset `normal * 1e-3`
- **法线公式**与 base pass 完全一致：`normal = texnormal.b*geoN - texnormal.g*geoT + texnormal.r*geoB`
- **ray cone**：`coneSpread = 2 * tan(fovY/2) / screenHeight`（C++ 端预算），closest-hit 中 `lambda = log2(coneSpread * RayTCurrent() / texelSize)`
- **point/spot 灯本地遍历**：raygen 内对全量灯 buffer 跑 `lightingShaderPointSpot`，跳过 cluster culling（v1 数量 64 量级，无压力）

## 已知 v1 限制

- 不渲染透明物体（玻璃 / 水）
- 仅 sun 投射 RT 阴影；point/spot 无阴影（与原 raster 一致）
- 没有 AO（用户已确认可省）
- Mip 用 ray cone 一阶近似，无各向异性

---

# 改动逐项记录

> 按提交顺序追加。每项标 [stage] / [文件] / [说明]。

(尚未开始 — 实施期间逐步追加)

## [stage 1] VulkanSetup —— 扩展 + features + RT properties

**`Src/Include/VulkanSetup.h`**
- `deviceExtensionNames[]` 在 `#ifndef __ANDROID__` 内追加：
  - `VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME`
  - `VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME`
  - `VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME`
  - `VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME`
  - `VK_KHR_SPIRV_1_4_EXTENSION_NAME`
  - `VK_KHR_SHADER_FLOAT_CONTROLS_EXTENSION_NAME`
- 私有成员新增（`#ifndef __ANDROID__`）：
  - `VkPhysicalDeviceRayTracingPipelinePropertiesKHR rtPipelineProperties{}` —— SBT 对齐 / handle size
  - `VkPhysicalDeviceAccelerationStructurePropertiesKHR asProperties{}` —— AS build size 上限
- 公有 getter：`getRTPipelineProperties()`、`getASProperties()`

**`Src/VulkanSetup.cpp`**（`createLogicalDevice`）
- 在拿到 `float16_features` 之后显式 enable：`bufferDeviceAddress`、`descriptorIndexing`、`runtimeDescriptorArray`、`descriptorBindingPartiallyBound`、`descriptorBindingVariableDescriptorCount`、`shaderSampledImageArrayNonUniformIndexing`、`scalarBlockLayout`
- 探测并记录 `VkPhysicalDeviceAccelerationStructureFeaturesKHR.accelerationStructure` 和 `VkPhysicalDeviceRayTracingPipelineFeaturesKHR.rayTracingPipeline`（不支持仅 warn，不阻塞）
- 用 `vkGetPhysicalDeviceProperties2` 把 `rtPipelineProperties` + `asProperties` 一次性查回
- 在 `vkCreateDevice` 之前的 pNext 链尾追加 `asFeatures → rtPipelineFeatures`

**未做**：函数指针加载 —— 决定放到 `Raytracing.cpp` 一次性加载（避免污染 VulkanSetup 接口）。

## [stage 2] Buffer usage / memory allocate flag

`Src/GpuScene.cpp` 中以下 buffer 加 `SHADER_DEVICE_ADDRESS_BIT` + 让 `vkAllocateMemory` 挂 `VkMemoryAllocateFlagsInfo{ VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT_KHR }`：

| Buffer | usage 增加项 |
|---|---|
| `applVertexBuffer` | `+ ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR + STORAGE_BUFFER_BIT + SHADER_DEVICE_ADDRESS_BIT` |
| `applIndexBuffer` | `+ ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR + STORAGE_BUFFER_BIT + SHADER_DEVICE_ADDRESS_BIT` |
| `applNormalBuffer` | `+ STORAGE_BUFFER_BIT + SHADER_DEVICE_ADDRESS_BIT` |
| `applTangentBuffer` | `+ STORAGE_BUFFER_BIT + SHADER_DEVICE_ADDRESS_BIT` |
| `applUVBuffer` | `+ STORAGE_BUFFER_BIT + SHADER_DEVICE_ADDRESS_BIT` |

`meshChunksBuffer` 已是 STORAGE_BUFFER（无变化），`applMaterialBuffer` 同。

**回归性**：原 vertex assembly / index draw 都不受影响；BDA flag 仅是额外能力。原 raster 路径渲染像素一致。

## [stage 3] `RayTracing` 类骨架 + AS 构建

**新增**：
- `Src/Include/Raytracing.h` —— 类声明、AS / pipeline / SBT / output image / 描述符 set 成员、9 个 PFN_* 函数指针、`Init / BuildAccelerationStructures / CreatePipelineAndSBT / CreateOutputImagesAndDescriptorSet / RecordTraceRays / RecordBlitToSwapchain` 接口
- `Src/Raytracing.cpp` —— 函数指针加载（用 `vkGetDeviceProcAddr` 一次性查 9 个）、`buildBLAS()` / `buildTLAS()`、辅助 `allocateBuffer()` / `getBufferDeviceAddress()`、stage 4/5 stub

**关键设计**：
- BLAS 一个，包含 `_opaqueChunkCount + _alphaMaskedChunkCount` 个 geometry。
  - 前 `_opaqueChunkCount` 个标 `VK_GEOMETRY_OPAQUE_BIT_KHR`，alpha 区段 flags=0
  - 每 geometry：vertexFormat=R32G32B32_SFLOAT，stride=12，indexData=`ibAddr + chunk.indexBegin*4`
  - 用 PREFER_FAST_TRACE，DEVICE_LOCAL AS storage + scratch
- TLAS 一个，1 个 instance，identity transform，mask=0xFF，sbtRecordOffset=0，CULL_DISABLE，引用 BLAS device address
- 用 `_device.beginSingleTimeCommands()` 一次性 build；scratch buffer 临时申请、build 后立即释放
- 完成后 `_blasAddress` 缓存（TLAS instance 引用）、`_tlas` 留作 descriptor 写入

**配套改动**：
- `Src/Include/GpuScene.h` —— `friend class RayTracing;`（让 RT 直接访问 buffer 句柄、`m_Chunks`、`applMesh` 等私有成员）
- `Src/Include/VulkanSetup.h` —— 把 `beginSingleTimeCommands` / `endSingleTimeCommands` 从 private 提到 public
- `Src/CMakeLists.txt` —— SOURCES 加 `Raytracing.cpp`

**编译验证**：MSVC build 通过，无 error。运行时 AS 构建尚未在 GpuScene 接入（stage 6 才打通）。

## [stage 5] `rt_lighting.hlsl` + 编译命令

**新增** `shaders/rt_lighting.hlsl` —— 单 .hlsl 多 entry，target `lib_6_3`：
- `RayGen` —— 反投相机射线，TraceRay 主光线，命中后计算 visibility（N-tap Hammersley + sun cone），调 `lightingShader`，全量遍历 point lights 调 `lightingShaderPointSpot`，写 `outLitColor[px]`
- `MissPrimary` / `MissShadow` —— 设置 payload `hit=false` / `visible=1`
- `ClosestHitPrimary` —— `gatherHit()` 顶点拉取 + 重心插值，bindless 采样 4 张材质纹理，复刻 drawcluster.hlsl 的 TBN 法线公式与 surfaceData
- `AnyHitAlpha` / `AnyHitAlphaShadow` —— 同样 gatherHit，alpha < 0.1 → `IgnoreHit()`

**Bindings 设计**：
- set 0 binding 0 —— 复用 deferredlighting 的相机 UBO（`CameraParamsBufferFull` + `AAPLFrameConstants`）
- set 1 binding 0..11 —— RT 专属：tlas、RW outLitColor、4 个顶点 SSBO、ByteAddressBuffer ibIndices、meshChunksRT、materialsRT、pointLightsRT、bindless `_Textures[]`、Linear sampler
- push constant `RTPushConsts` —— pointLightCount / shadowTaps / sunConeRadius / pixelSpreadAngle / frameSeed

**SBT/Hit Group 设计**（在 RT pipeline 里实现）：
- TraceRay 主：hgOff=0, hgStride=2, missIdx=0
- TraceRay 阴影：hgOff=1, hgStride=2, missIdx=1
- 4 hit-group：HG_PRI_OPAQUE(chit) / HG_PRI_ALPHA(chit+ahit) / HG_SHA_OPAQUE(空) / HG_SHA_ALPHA(ahit only)

**Ray cone mip**：`coneMipLOD(t) = log2(pixelSpreadAngle * t / texelWorldSize)`，texelWorldSize 当前用 1/2048 的常数近似。

**修改** `shaders/compile_shaders.bat` —— 末尾追加 dxc lib_6_3 编译命令：
```
dxc.exe -spirv -T lib_6_3 rt_lighting.hlsl
  -fspv-target-env=vulkan1.2
  -fspv-extension=SPV_KHR_ray_tracing
  -fspv-extension=SPV_KHR_physical_storage_buffer
  -fspv-extension=SPV_KHR_non_semantic_info
  -fspv-extension=SPV_EXT_descriptor_indexing  (← 因为 _Textures[] 用了 runtime array)
  -Fo rt_lighting.lib.spv
```

**编译验证**：单次 dxc 调用产出 `rt_lighting.lib.spv`（约 19.7 KB），无 error/warn。

## [stage 4] RT pipeline + SBT + output image + descriptor set

**修改** `Src/Include/Raytracing.h`：新增以下字段：
- `_rtSetLayout / _rtDescriptorPool / _rtDescriptorSets` —— set 1 描述符（per-frame）
- `_rtPipelineLayout / _rtPipeline / _rtShaderModule` —— RT pipeline + shader module
- `_sbtBuffer / _sbtMemory + _rgenRegion / _missRegion / _hitRegion / _callRegion` —— SBT
- `_rtLitImage[N] / _rtLitMemory[N] / _rtLitImageView[N] + _rtExtent` —— per-frame HDR 输出 image
- `_rtImguiPass + _rtImguiFrameBuffer[N]` —— ImGui composite render pass，保留 RT blit

**`Src/Raytracing.cpp::CreatePipelineAndSBT()`**
- 读 `shaders/rt_lighting.lib.spv` → `vkCreateShaderModule`
- 6 stage（RayGen / MissPrimary / MissShadow / ClosestHitPrimary / AnyHitAlpha / AnyHitAlphaShadow），entry 名通过 `pName` 指定
- 7 group：1 raygen + 2 miss + 4 hit-group（HG_PRI_OPAQUE / HG_PRI_ALPHA / HG_SHA_OPAQUE 空 / HG_SHA_ALPHA）
- pipeline layout：set 0 = `_scene.globalSetLayout`（复用 deferredlighting 的相机 UBO），set 1 = `_rtSetLayout`，push constant 32 字节
- `vkCreateRayTracingPipelinesKHR` + `vkGetRayTracingShaderGroupHandles`
- SBT 内存按 `shaderGroupBaseAlignment` / `shaderGroupHandleAlignment` 对齐，hit 区按 chunk 顺序写入 `(opaque|alpha)_PRIMARY` 和 `(opaque|alpha)_SHADOW` 两条 handle

**`Src/Raytracing.cpp::CreateOutputImagesAndDescriptorSet()`**
- 创建 `_rtLitImage[swapChainImageCount]`，`R16G16B16A16_SFLOAT`，DEVICE_LOCAL，usage `STORAGE | TRANSFER_SRC | SAMPLED`
- set 1 layout 12 个 binding（含 bindless `_Textures[]` 用 `UPDATE_AFTER_BIND`）
- pool 大小按 N 帧 × 各类型预留
- 一次性写入所有 binding：TLAS / RW image / vertex/index/material/chunk/pointLight SSBO / bindless 纹理 / sampler

**`Src/Raytracing.cpp::RecordTraceRays()`**
- 转换 `_rtLitImage[idx]` UNDEFINED → GENERAL（write barrier）
- 绑定 pipeline + 2 set + 推送 32 字节 push constant：`pointLightCount / shadowTaps=4 / sunConeRadius=0.0087 / pixelSpreadAngle (固定 fovY=60°估算) / frameSeed`
- `vkCmdTraceRaysKHR(width, height, 1)`

**`Src/Raytracing.cpp::RecordBlitToSwapchain()`**
- `_rtLitImage` GENERAL → TRANSFER_SRC，swapchain UNDEFINED → TRANSFER_DST
- `vkCmdBlitImage`（NEAREST 过滤）
- swapchain TRANSFER_DST → COLOR_ATTACHMENT（给 ImGui 用）

**`Src/Raytracing.cpp::BeginImGuiCompositePass / EndImGuiCompositePass`**
- 单 attachment（swapchain）的 render pass，loadOp=LOAD，finalLayout=PRESENT_SRC_KHR
- per-swapchain framebuffer 一次性建好

**配套**：
- `Src/Include/VulkanSetup.h` —— 新增 `getSwapChainImage(int i)` 公共 getter
- `Src/GpuScene.cpp::init_GlobaldescriptorSet()` —— globalSetLayout 的相机 UBO binding stage flags 加上 RT 4 个 stage（RAYGEN / CHIT / AHIT / MISS），让 RT pipeline 复用同一 layout

**编译验证**：MSVC build 通过。

## [stage 6] GpuScene 接入 + ImGui toggle

**修改** `Src/Include/GpuScene.h`：
- 加 `class RayTracing *_raytracing = nullptr;`
- 加 `bool useRayTracing = false;`

**修改** `Src/GpuScene.cpp`：
- include `"Raytracing.h"`
- `recordCommandBuffer` 顶部（在帧 UBO 上传之后、`vkBeginCommandBuffer` 之前）做 LightCuller + RayTracing 的**一次性懒加载**：
  ```cpp
  if (!_lightCuller) { _lightCuller = new LightCuller(); _lightCuller->InitRHI(...); }
  if (!_raytracing) {
    _raytracing = new RayTracing(...);
    _raytracing->Init();
    _raytracing->BuildAccelerationStructures();
    _raytracing->CreatePipelineAndSBT();
    _raytracing->CreateOutputImagesAndDescriptorSet();
  }
  ```
  把 `_lightCuller` 的初始化提前到这里（原来挂在 cluster lighting 的懒加载里），保证 RT 路径下也有 point-light buffer。
- `vkBeginCommandBuffer` 之后立即分支：
  ```cpp
  if (useRayTracing) {
    RecordTraceRays → RecordBlitToSwapchain → BeginImGuiCompositePass → renderImGuiOverlay → EndImGuiCompositePass
    vkEndCommandBuffer; return;
  }
  // …existing raster pipeline…
  ```
- ImGui overlay 加 `Checkbox("Ray Tracing", &useRayTracing)` + 状态提示

**编译验证**：完整 MSVC build 通过，`Bin/AdvancedVulkanRendering.exe` 链接成功。

## 运行/调试要点 & 已知风险

1. **第一次 `useRayTracing=on` 切换**会触发 BLAS / TLAS / pipeline / SBT 一次性构建（已经在第 1 帧懒加载完毕），切换本身只是 toggle 一个 bool，零额外开销。
2. **DXC 编译命令**对应的 SPIR-V extension 命名细节：
   - 用 `SPV_KHR_non_semantic_info`（不是 `SPV_KHR_shader_non_semantic_info`）
   - bindless 必须显式加 `SPV_EXT_descriptor_indexing`
3. **`pixelSpreadAngle` 当前硬编码 fovY=60°**；如需更准确，把 `Camera::Fov()` 接到 push constant。
4. **`sunConeRadius` 目前 0.0087 (~0.5° 半角)**；可在 `RecordTraceRays` 调整。
5. **AS 是 host-visible 的 vertex/index buffer 喂的**（继承现有内存属性）。性能上不是最优；后续可改 staging upload + DEVICE_LOCAL。
6. **AS / pipeline / output image 在窗口缩放时不会重建**；`recreateSwapChain` 当前未触发 RT 资源刷新。如果改窗口尺寸，先关掉 RT toggle 再开（或后续补 `_raytracing->Recreate()`）。

## 验证 checklist

- [ ] 启动应用，原 raster 渲染像素未变（baseline 回归）
- [ ] ImGui 勾选 `Ray Tracing` —— 画面切到 RT 输出
- [ ] sun 直接光强度 / 色温与 raster 一致（同一 BRDF）
- [ ] 阴影边缘从 cascade PCF 变为 N-tap 软阴影
- [ ] Bistro 树叶通过 alpha-mask any-hit 漏光
- [ ] VK validation layer 无 RT 相关 error
