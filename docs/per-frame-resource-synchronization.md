# Per-Frame Resource Synchronization（多帧并行资源竞争修复）

## 问题描述

当 `framesInFlight > 1` 时（通常等于 swapchain image count，即 2-3），Vulkan 的多帧并行机制允许 CPU 录制第 N+1 帧的 command buffer，同时 GPU 仍在执行第 N 帧的 command buffer。

### 原始同步机制

```
Frame 0: WaitFence[0] → Record CB[0] → Submit(signal Fence[0])
Frame 1: WaitFence[1] → Record CB[1] → Submit(signal Fence[1])
```

`inFlightFences[currentFrame]` 保证的是：**同一个 frame index 的 command buffer 执行完毕后才能重新录制**。

但它 **不会阻止** frame 0 和 frame 1 的 command buffer 在 GPU 上同时执行。

### 竞争场景

以下资源在每帧都被 CPU 写入或 GPU compute 写入，当两帧的 command buffer 同时在 GPU 执行时会发生数据竞争：

| 资源 | 写入方 | 竞争类型 |
|------|--------|----------|
| `uniformBuffer` | CPU（相机矩阵、阴影矩阵） | Frame N GPU 读取 vs Frame N+1 CPU 写入 |
| `cullParamsBuffer` | CPU（视锥体、HiZ 参数） | 同上 |
| `writeIndexBuffer` | CPU 重置 + GPU compute 原子递增 | Frame N GPU 读取 vs Frame N+1 CPU 重置 |
| `drawParamsBuffer` | GPU compute 写入 | Frame N indirect draw 读取 vs Frame N+1 compute 覆写 |
| `chunkIndicesBuffer` | GPU compute 写入 | 同上 |
| Shadow 系列 buffer | 同上各类 | 同上 |

### 为什么之前"没出问题"

1. GPU 执行速度快，fence wait 使得并发窗口很小
2. 如果 uniform buffer 每帧写入相同值（相机未移动），竞争不会表现为可见错误
3. 某些驱动可能内部做了额外同步

但在相机快速移动、GPU 负载高、或 validation layer 开启时，问题可能会暴露为画面撕裂、闪烁或 validation error。

## 解决方案

采用 **方案 A：Per-Frame Resource Duplication**，为每帧维护独立的 buffer 和 descriptor set 副本。

### 原则

- **只读资源不复制**：vertex buffer、index buffer、texture、sampler、meshChunksBuffer（初始化后不再写入）等可以安全共享
- **每帧写入的资源必须复制**：任何被 CPU map/write 或 GPU compute 每帧写入的 buffer

### 修改的文件

#### `Src/Include/GpuScene.h`

单份资源改为 `std::vector`，大小 = `framesInFlight`：

```cpp
// Before
VkBuffer uniformBuffer;
VkDescriptorSet globalDescriptorSet;

// After
std::vector<VkBuffer> uniformBuffers;          // per-frame
std::vector<VkDescriptorSet> globalDescriptorSets; // per-frame
```

完整列表：

| 旧名称 | 新名称 |
|--------|--------|
| `uniformBuffer` / `uniformBufferMemory` | `uniformBuffers` / `uniformBufferMemories` |
| `drawParamsBuffer` / `drawParamsBufferMemory` | `drawParamsBuffers` / `drawParamsBufferMemories` |
| `cullParamsBuffer` / `cullParamsBufferMemory` | `cullParamsBuffers` / `cullParamsBufferMemories` |
| `writeIndexBuffer` / `writeIndexBufferMemory` | `writeIndexBuffers` / `writeIndexBufferMemories` |
| `chunkIndicesBuffer` / `chunkIndicesBufferMemory` | `chunkIndicesBuffers` / `chunkIndicesBufferMemories` |
| `globalDescriptorSet` | `globalDescriptorSets` |
| `gpuCullDescriptorSet` | `gpuCullDescriptorSets` |
| `applDescriptorSet` | `applDescriptorSets` |

#### `Src/Include/Shadow.h`

```cpp
// Before
VkBuffer _shadowDrawParamsBuffer;
VkDescriptorSet _shadowCullDescriptorSet;

// After
std::vector<VkBuffer> _shadowDrawParamsBuffers;
std::vector<VkDescriptorSet> _shadowCullDescriptorSets;
```

#### `Src/Include/Light.h`

```cpp
// Before
static VkDescriptorSet drawPointLightDescriptorSet;

// After
static std::vector<VkDescriptorSet> drawPointLightDescriptorSets;
```

#### `Src/GpuScene.cpp`

1. **Buffer 创建**：`createUniformBuffer()` 及各 buffer 创建块内加 `for (f = 0; f < framesInFlight; ++f)` 循环
2. **Descriptor Set 分配**：`init_GlobaldescriptorSet()`、`init_drawparams_descriptors()`、`init_appl_descriptors()` 分配 `framesInFlight` 个 descriptor set，每个绑定对应帧的 buffer
3. **录制/绘制**：`recordCommandBuffer()` 中使用 `uniformBufferMemories[currentFrame]`、`cullParamsBufferMemories[currentFrame]`、`writeIndexBufferMemories[currentFrame]` 进行 map/write；所有 `vkCmdBindDescriptorSets` 和 `vkCmdDrawIndexedIndirectCount` 使用 `[currentFrame]` 索引
4. **HiZ 更新**：`createHiZResources()` 中循环更新所有帧的 `gpuCullDescriptorSets`
5. **更新 Sampler**：`updateSamplerInDescriptors()` 循环更新所有帧的 `globalDescriptorSets`

#### `Src/Shadow.cpp`

1. `InitGPUShadowResources()`：为每帧创建独立的 shadow buffer 和 descriptor set
2. `RenderShadowMap()`：通过 `gpuScene.currentFrame` 索引使用对应帧的 buffer 和 descriptor set

#### `Src/Light.cpp`

1. `PointLight::InitRHI()`：为每帧创建独立的 `drawPointLightDescriptorSets`
2. `PointLight::Draw()`：通过 `gpuScene.currentFrame` 绑定对应帧的 descriptor set

### 未修改（共享安全）

以下资源在初始化后不再写入，多帧共享是安全的：

- `vertexBuffer`、`indexBuffer`（applVertexBuffer 等）
- `meshChunksBuffer`（初始化时一次性上传）
- `applMaterialBuffer`
- 所有 texture image/view
- sampler
- pipeline / pipeline layout
- render pass / framebuffer
- `deferredLightingDescriptorSet`（绑定的全是 GBuffer image view 和 sampler）

### 已知 TODO

- `LightCuller` 的 `coarseCullDescriptorSet` 目前暂用 `uniformBuffers[0]` 和 `cullParamsBuffers[0]`，当 cluster lighting 启用时应也做 per-frame 化

## 内存开销

每增加一帧，额外分配：

| Buffer | 大小（典型值） |
|--------|----------------|
| uniformBuffer | ~512 bytes |
| cullParamsBuffer | ~192 bytes |
| writeIndexBuffer | 12 bytes |
| drawParamsBuffer | chunkCount * 20 bytes |
| chunkIndicesBuffer | chunkCount * cascadeCount * 8 bytes |
| shadowDrawParamsBuffer | totalSlots * 20 bytes |
| shadowWriteIndexBuffer | cascadeCount * 8 bytes |
| shadowCullParamsBuffer | ~112 bytes |

对于典型的 2-3 帧并行，总额外内存开销在 MB 级别以下，完全可以接受。
