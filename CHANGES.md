# Recent Changes Summary

## Round 1: HiZ Occlusion Culling Fix + Culling Stats UI

### 1.1 HiZ Downsample Edge Handling

**Problem**: 当源 mip 尺寸为奇数（如 1920→960→480→240→120→60→30→15→...）时，2x2 downsample 会丢失最后一列/行的深度信息，导致 depth pyramid 边缘产生深度泄漏。

**参考**: `D:\ModernRenderingWithMetal\Renderer\Shaders\AAPLDepthPyramid.metal:29-42`

**修改文件**:

- **`shaders/hiz.hlsl`**
  - Push constant 从 `uint2 mipSize` 扩展为 `uint4 {srcSize, dstSize}`
  - `DownsampleHiZ` 新增边缘检测逻辑：当 `tid.x * 2 == srcWidth - 3` 时额外采样第3列，`tid.y * 2 == srcHeight - 3` 时额外采样第3行，两者同时成立时采样角落像素
  - 使用 `min()`（reverse-Z 下最远表面 = 最小深度值）

- **`Src/GpuScene.cpp`**
  - Push constant range 从 `sizeof(uint32_t) * 2` 改为 `sizeof(uint32_t) * 4`
  - Copy pass: push `{screenW, screenH, hizW, hizH}`
  - Downsample pass: push `{prevMipW, prevMipH, mipW, mipH}`

### 1.2 启用 HiZ Occlusion Culling

**Problem**: `gpucull.hlsl` 的 `IsOccludedByHiZ()` 被硬编码 `return false;` 禁用。

**修改文件**:

- **`shaders/gpucull.hlsl:31-32`**
  - 移除 `return false;`，恢复为 `if (hizMipLevels == 0) return false;`
  - 已有的遮挡测试逻辑已适配 Vulkan reverse-Z：投影8个AABB角点到NDC，选取mip level via `ceil(log2(max(pixelSize)))`, 4角采样取 `min()`，比较 `maxZ <= hizDepth`

### 1.3 ImGui Culling Stats UI

**Problem**: 无屏幕上实时显示剔除统计，仅有 spdlog 控制台输出。

**修改文件**:

- **`thirdparty/imgui/`** — 新增：clone ImGui 库（含 Vulkan + SDL2 backends）

- **`Src/CMakeLists.txt`** — 添加 ImGui 源文件和 include 路径

- **`Src/Include/VulkanSetup.h`** — 新增 `getInstance()` accessor

- **`Src/Include/GpuScene.h`** — 新增成员：
  - `VkDescriptorPool _imguiDescriptorPool`
  - `CullingStats` 结构体（visibleOpaque/AlphaMask/Transparent + total 计数）
  - `initImGui()`, `readbackCullingStats()`, `renderImGuiOverlay()`, `ProcessImGuiEvent()`

- **`Src/GpuScene.cpp`**
  - `initImGui()`: 创建 ImGui descriptor pool，初始化 `ImGui_ImplVulkan_Init` + `ImGui_ImplSDL2_InitForVulkan`，render pass 使用 `_forwardLightingPass`
  - `readbackCullingStats()`: 从上一帧的 `writeIndexBuffers`（HOST_VISIBLE）读取可见 chunk 计数
  - `renderImGuiOverlay()`: 在 forward pass 末尾渲染半透明统计窗口，显示 Total/Visible/Culled 及每类别明细
  - `Draw()` 中插入 `readbackCullingStats(previousFrame)`

- **`Src/Window.cpp`**
  - 构造 GpuScene 后调用 `gpuScene.InitImGui(window)`
  - SDL 事件循环中调用 `gpuScene.ProcessImGuiEvent(&e)`

---

## Round 2: Scalable Ambient Obscurance (SAO)

**参考**: `D:\ModernRenderingWithMetal\Renderer\Shaders\AAPLAmbientObscurance.metal`

### 2.1 Uniform 数据扩展

**修改文件**:

- **`Src/Include/Common.h`**
  - `uniformBufferData` 新增 `mat4 invProjectionMatrix`（用于从深度重建相机空间坐标）
  - `FrameConstants` 新增 `uint32_t frameCounter`（用于时域抖动）

- **`shaders/commonstruct.hlsl`**
  - `CameraParamsBuffer` 和 `CameraParamsBufferFull` 新增 `float4x4 invProjectionMatrix`
  - `AAPLFrameConstants` 新增 `uint frameCounter`

- **`Src/GpuScene.cpp` (uniform upload)**
  - 计算 `invProj = inverse(projectionMatrix)` 并上传
  - 使用 `static uint32_t sFrameCounter` 递增并写入 `frameConstants.frameCounter`

### 2.2 SAO Compute Shader

**新建文件**: **`shaders/sao.hlsl`**

从 Metal 移植的 Scalable Ambient Obscurance 算法：

| 参数 | 值 | 说明 |
|------|-----|------|
| tapCount | 36 / 4 = 9 | 每帧9次采样 |
| temporalFrames | 4 | 4帧轮换，等效36次采样 |
| radius | 0.5 * height * proj[1][1] | ≈1米投影到屏幕像素 |
| numSpirals | 11 | 螺旋采样避免混叠 |
| bias | 0.001 | 防止自遮蔽 |
| intensity | 1.0 | AO 强度 |

**算法流程**:
1. 读取中心像素深度，通过 `invProjectionMatrix` 重建相机空间坐标
2. 从4邻域深度重建法线（cross product，选择较短边缘避免深度不连续伪影）
3. Wang hash 生成 per-pixel 时域抖动种子（`seed += frameCounter`）
4. 螺旋采样：每个 tap 根据偏移距离自适应选择 mip level（`log2(offset) - 3`），从 depth pyramid 读取深度
5. 累积 horizon-based 遮蔽：`max((vn - bias) / (epsilon + vv), 0)`
6. 输出：`max(0, 1 - sum * intensity / taps)`

**Vulkan reverse-Z 适配**: 远平面 depth=0（Metal 为 1），跳过 `depth <= 0.0001` 的天空像素。

### 2.3 SAO 深度金字塔

**修改文件**:

- **`Src/Include/GpuScene.h`** — 新增成员：
  - `_saoDepthPyramid` / `_saoDepthPyramidView` / `_saoMipViews[]`（R32_SFLOAT，全屏分辨率，完整 mip chain）
  - `_aoTexture` / `_aoTextureView`（R8_UNORM，全屏分辨率，SAO 输出）
  - SAO compute pipeline / descriptor set 相关成员
  - `createSAOResources()`, `generateSAODepthPyramid()`, `dispatchSAO()`

- **`Src/GpuScene.cpp`**
  - `createSAOResources()`:
    - 分配 SAO depth pyramid（全屏 R32_SFLOAT + 完整 mip chain）
    - 分配 AO 输出纹理（R8_UNORM）
    - 创建 descriptor sets 复用 HiZ 的 copy/downsample pipeline
    - 创建 SAO compute pipeline（sao.cs.spv）
    - 将 AO 纹理写入 deferred lighting descriptor set（binding 10）
  - `generateSAODepthPyramid()`: 与 HiZ pyramid 相同模式，但输入为窗口深度（全场景），而非 occluder depth
  - `dispatchSAO()`: 绑定 pipeline，dispatch `(width+7)/8 × (height+7)/8`，完成后 barrier 到 `SHADER_READ_ONLY_OPTIMAL`

### 2.4 Deferred Lighting 集成

**修改文件**:

- **`shaders/deferredlighting.hlsl`**
  - 新增 `[[vk::binding(10,1)]] Texture2D<float> aoTexture`
  - 在 `DeferredLighting()` 中采样 AO 并乘入最终光照：
    ```hlsl
    float ao = aoTexture.SampleLevel(_NearestClampSampler, input.TextureUV, 0);
    half3 result = lightingShader(...) * shadow * ao;
    ```

- **`Src/GpuScene.cpp` (descriptor set)**
  - `init_deferredlighting_descriptors()` 新增 binding 10（SAMPLED_IMAGE）
  - `createSAOResources()` 末尾将 `_aoTextureView` 写入该 binding

- **`Src/GpuScene.cpp` (recordCommandBuffer)**
  - 在 base pass 深度转换之后、deferred lighting pass 之前插入：
    ```
    generateSAODepthPyramid(commandBuffer);  // 从全场景深度构建 pyramid
    dispatchSAO(commandBuffer);               // 计算 AO → _aoTexture
    ```

### 2.5 重编译的 Shader

由于 `commonstruct.hlsl` 结构体变化（新增 `invProjectionMatrix` 和 `frameCounter`），所有引用它的 shader 全部重编译：

- `hiz_copy.cs.spv`, `hiz_downsample.cs.spv`
- `gpucull.cs.spv`, `shadowcull.cs.spv`
- `sao.cs.spv`（新）
- `deferredlighting.vs.spv`, `deferredlighting.ps.spv`
- `deferredPointLighting.vs.spv`, `deferredPointLighting.ps.spv`
- `drawcluster.vs.spv`, `drawcluster.ps.spv`, `drawcluster.base.ps.spv`, `drawcluster.depth.ps.spv`, `drawcluster.forward.ps.spv`
- `drawcluster.base.alphamask.ps.spv`, `drawcluster.forward.indirect.ps.spv`, `drawcluster.shadow.indirect.ps.spv`
- `drawclusterShadow.vs.spv`, `occluders.vs.spv`

---

## 渲染管线执行顺序（最终）

```
1. Shadow Rendering
2. Draw Occluders → occluder depth
3. Generate HiZ Pyramid (compute, from occluder depth)
4. GPU Frustum + HiZ Culling (compute)
5. Base Pass → G-Buffers + window depth
6. [Optional] Light Culling (compute)
7. Depth Transition → DEPTH_READ_ONLY
8. ★ Generate SAO Depth Pyramid (compute, from window depth)
9. ★ Dispatch SAO Compute → _aoTexture
10. Deferred Lighting (reads G-Buffers + depth + shadow + AO)
11. Forward Pass (transparent objects)
12. ImGui Overlay (culling stats)
13. Present
```
