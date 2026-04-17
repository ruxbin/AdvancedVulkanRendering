# TAA + ACES Tone Mapping Resolve Pass

## Overview

在 deferred lighting 和 forward pass 之后插入了一个 Resolve pass，实现 Temporal Anti-Aliasing (TAA) 和 ACES 色调映射。参考实现来自 `D:\ModernRenderingWithMetal\Renderer\Shaders\AAPLResolve.metal`。

## Pipeline 变化

```
Before:
  Shadow -> Cull -> GBuffer -> SAO -> Deferred [-> swapchain] -> Forward [-> swapchain] -> Present

After:
  Shadow -> Cull -> GBuffer -> SAO -> Deferred [-> HDR R16G16B16A16F] -> Forward [-> HDR]
       -> Resolve (TAA + ACES) [-> swapchain + history] -> Present
```

## 修改的文件

### 1. `Src/Include/Common.h`

**uniformBufferData** 末尾新增:
```cpp
mat4 prevViewProjectionMatrix;  // TAA reprojection 用
```

**FrameConstants** 末尾新增:
```cpp
vec2 invPhysicalSize;    // 1/screenSize, UV 计算用
vec2 taaJitter;          // 当前帧的 sub-pixel jitter (NDC 空间)
float exposure;          // 曝光系数 (tone mapping 用)
uint32_t taaEnabled;     // TAA 开关 (首帧为 0)
```

### 2. `shaders/commonstruct.hlsl`

与 C++ 侧对应，**CameraParamsBufferFull** 末尾加了 `prevViewProjectionMatrix`，**AAPLFrameConstants** 末尾加了 `invPhysicalSize`、`taaJitter`、`exposure`、`taaEnabled`。

### 3. `Src/Include/GpuScene.h`

新增成员变量:
```cpp
// TAA 状态
mat4 _prevViewProjectionMatrix;
bool _taaFirstFrame = true;
uint32_t _taaFrameIndex = 0;

// HDR lighting buffer (R16G16B16A16_SFLOAT)
VkImage _hdrLightingBuffer;
VkDeviceMemory _hdrLightingBufferMemory;
VkImageView _hdrLightingBufferView;

// TAA history buffer (R8G8B8A8_SRGB)
VkImage _taaHistoryBuffer;
VkDeviceMemory _taaHistoryBufferMemory;
VkImageView _taaHistoryBufferView;

// Resolve pass 资源
VkRenderPass _resolvePass;
std::vector<VkFramebuffer> _resolveFrameBuffer;
VkDescriptorSetLayout _resolveSetLayout;
VkDescriptorPool _resolveDescriptorPool;
VkDescriptorSet _resolveDescriptorSet;
VkPipelineLayout _resolvePipelineLayout;
VkPipeline _resolvePipeline;
VkSampler _linearClampSampler;
```

新增方法:
- `createHDRLightingBuffer()` / `createTAAHistoryBuffer()`
- `createResolvePass()` / `createResolveFrameBuffer()`
- `createResolveDescriptors()` / `createResolvePipeline()`
- `createLinearClampSampler()`

### 4. `Src/GpuScene.cpp`

#### 4.1 Render Pass 格式修改

| 函数 | 修改 |
|------|------|
| `CreateDeferredLightingPass()` | format: `swapChainFormat` -> `R16G16B16A16_SFLOAT` |
| `CreateForwardLightingPass()` | format: `swapChainFormat` -> `R16G16B16A16_SFLOAT`; finalLayout: `PRESENT_SRC_KHR` -> `SHADER_READ_ONLY_OPTIMAL` |
| `CreateDeferredLightingFrameBuffer()` | color attachment: `swapChainImageView` -> `_hdrLightingBufferView` |
| `CreateForwardLightingFrameBuffer()` | color attachment: `swapChainImageView` -> `_hdrLightingBufferView` |

#### 4.2 Uniform Upload (recordCommandBuffer)

- 计算 Halton jitter (base 2/3, 8 samples 循环)
- 对 projection matrix 施加 jitter 后上传 (vertex shading sub-pixel offset)
- invViewProjectionMatrix / invProjectionMatrix 使用 clean (无 jitter) 矩阵
- 新增 prevViewProjectionMatrix 上传
- 填充 invPhysicalSize / taaJitter / exposure / taaEnabled

#### 4.3 Resolve Pass 录制

在 forward pass 结束后、`vkEndCommandBuffer` 之前插入:
```cpp
vkCmdBeginRenderPass(cmd, &resolvePassInfo, ...);
vkCmdBindPipeline(cmd, GRAPHICS, _resolvePipeline);
vkCmdBindDescriptorSets(cmd, GRAPHICS, _resolvePipelineLayout, 0, 2, {global, resolve}, ...);
vkCmdDraw(cmd, 3, 1, 0, 0);  // fullscreen triangle
vkCmdEndRenderPass(cmd);
```

#### 4.4 Resolve Render Pass 设计

MRT 双输出:
- Attachment 0: swapchain image -> `PRESENT_SRC_KHR`
- Attachment 1: TAA history buffer (R8G8B8A8_SRGB) -> `SHADER_READ_ONLY_OPTIMAL`

这样不需要单独的 history copy 步骤。

#### 4.5 Resolve Descriptor Set (set 1)

| Binding | 类型 | 内容 |
|---------|------|------|
| 0 | SAMPLED_IMAGE | HDR lighting buffer |
| 1 | SAMPLED_IMAGE | TAA history texture |
| 2 | SAMPLED_IMAGE | Depth texture |
| 3 | SAMPLER | Nearest clamp sampler |
| 4 | SAMPLER | Linear clamp sampler (Catmull-Rom 用) |

Set 0 复用 globalSetLayout (uniform buffer)。

#### 4.6 初始化顺序

```
CreateGBuffers()
createHDRLightingBuffer()        // NEW
createTAAHistoryBuffer()         // NEW
...
CreateForwardLightingPass()
createResolvePass()              // NEW
...
CreateForwardLightingFrameBuffer()
createResolveFrameBuffer()       // NEW
...
createNearestClampSampler()
createLinearClampSampler()       // NEW
...
createSAOResources()
createResolveDescriptors()       // NEW
...
createOccluderWireframePipeline()
createResolvePipeline()          // NEW
```

#### 4.7 Cleanup / Recreation

`cleanupSwapChainResources()` 新增销毁: resolve framebuffers、HDR buffer、history buffer，重置 `_taaFirstFrame = true`。

`recreateSwapChainResources()` 新增重建: HDR/history buffers、resolve framebuffers、更新 resolve descriptor set 的 image views。

### 5. `shaders/resolve.hlsl` (新文件)

核心算法:

```
1. 采样当前 HDR 像素
2. ACES tone mapping (exposure * pixel)
3. if (taaEnabled):
   a. 3x3 邻域采样，在 tone-mapped 空间计算 min/max bounding box
   b. 从 depth 重建世界坐标 (worldPositionForTexcoord)
   c. 用 prevViewProjectionMatrix 重投影到上一帧 UV
   d. Catmull-Rom 5-tap 采样 history texture
   e. clamp history 到邻域 bounding box
   f. blend: lerp(current, history, 0.95)  (off-screen 时 blend=0)
4. 输出到 SV_Target0 (swapchain) 和 SV_Target1 (history)
```

**ACES Tone Mapping**:
```hlsl
float3 ToneMapACES(float3 x) {
    float a=2.51, b=0.03, c=2.43, d=0.59, e=0.14;
    return saturate((x*(a*x+b))/(x*(c*x+d)+e));
}
```

**Halton Jitter** (CPU 端):
```cpp
halton(index, base=2) -> X jitter
halton(index, base=3) -> Y jitter
// 映射到 [-1/width, 1/width] 范围
// 8 samples 循环
```

### 6. `shaders/compile_shaders.bat`

末尾新增:
```bat
dxc.exe -spirv -T ps_6_0 resolve.hlsl -fspv-debug=vulkan-with-source -E ResolvePS -Fo resolve.ps.spv
```
VS 复用 `deferredlighting.vs.spv` (相同的 fullscreen triangle)。

## 关键设计决策

| 决策 | 选择 | 原因 |
|------|------|------|
| HDR buffer 格式 | R16G16B16A16_SFLOAT | 足够精度存储 HDR lighting 值 |
| History buffer 格式 | R8G8B8A8_SRGB | tone mapped 后的 LDR 数据，sRGB 编码 |
| History 写入方式 | MRT 双输出 | 避免额外 copy 和 swapchain layout 转换 |
| Jitter 序列 | Halton (2,3) 8 samples | 低差异序列，业界标准做法 |
| History 采样 | Catmull-Rom 5-tap | 比 bilinear 更锐利，减少 TAA 模糊 |
| Neighborhood clamping | 3x3 min/max in tone-mapped space | 防止 ghosting，标准 TAA 做法 |
| Blend ratio | 5% current / 95% history | 与 Metal 参考一致 |
| ImGui | 留在 forward pass | ACES 对 LDR 值近似线性，UI 外观影响极小 |
