# Per-Frame-In-Flight vs Per-Swapchain-Image 索引重构

## 问题背景

Vulkan 渲染中有两个**容易混淆**的索引：

1. **`currentFrame`**（per-frame-in-flight）—— CPU 端的"帧槽"计数器，cycles `0..framesInFlight-1`，一帧一进位。用来给 cmd buffer / semaphore / fence 这种**反复复用**的同步对象做轮转，避免 CPU 写正在被 GPU 用的资源。
2. **`imageIndex`**（per-swapchain-image）—— `vkAcquireNextImageKHR` 返回的 swapchain image 索引。**驱动可以乱序返回**（例如 `0,1,2,1,0,2,...`），不一定按 `currentFrame` 的顺序。

很多 Vulkan tutorial 简单情况下两者**数值相同**所以教程代码看起来"能跑"，但严格按 spec 它们是两个独立概念。

## 本项目原有的 bug

- `framesInFlight = swapChainImageCount` —— 数组大小相同，访问不会越界
- 但**索引混用**：
  - `_basePassFrameBuffer[currentFrame]` 用 frame 索引，但里面绑的 `depth[currentFrame]` 是 swapchain-scoped 资源
  - `_deferredFrameBuffer[imageIndex]` 用 swapchain 索引，里面绑 `depth[imageIndex]`
  - 当 `currentFrame != imageIndex` 时，base pass 写到 `depth[currentFrame]`，deferred 读 `depth[imageIndex]` —— **不同物理 image**

正常 FIFO 三缓冲且驱动按顺序返回 imageIndex 时，两者数值相同，bug 不显现。但驱动**完全有权**乱序返回，此时：
- 视觉上：deferred lighting 读到的 depth 是其他帧的（轻微不一致，可能不易察觉）
- Validation 层：`vkCmdPipelineBarrier` 报 layout mismatch（barrier 的 oldLayout 和 image 实际 layout 不一致）

```
Validation Error: vkQueueSubmit(): pSubmits[0].pCommandBuffers[0] command buffer
expects VkImage to be in layout VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL
-- instead, current layout is VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL.
```

## 资源分类

按"该用哪个索引"划分：

| 类型 | 例子 | 索引 |
|---|---|---|
| 同步对象 | command buffer, fence, semaphore | `_syncSlot`（per-frame-in-flight，独立轮转） |
| Swapchain-scoped image | `swapChainImages`, `depthImage`, `_gbuffers`, `_aoTexture`, `_saoDepthPyramid` | `imageIndex` |
| 引用 swapchain-scoped image 的 framebuffer | `_basePassFrameBuffer`, `_deferredFrameBuffer`, `_forwardFrameBuffer`, `_decalFramebuffers` | `imageIndex` |
| 含 swapchain-scoped image view 的 descriptor set | `deferredLightingDescriptorSet`（绑 depth + gbuffer view）, `_decalDescriptorSets`, `coarseCullDescriptorSet`, `_saoDescriptorSets`, `drawPointLightDescriptorSets` | `imageIndex` |
| CPU 写的 per-frame UBO/SSBO | `uniformBuffers`, `cullParamsBuffers`, `writeIndexBuffers`, `_decalDataBuffers` | `imageIndex`（对齐 descriptor 索引） |
| 仅含 per-frame buffer 的 descriptor | `globalDescriptorSets`, `applDescriptorSets`, `gpuCullDescriptorSets` | `imageIndex`（对齐其他 set） |

## 重构方案

**思路**：让 `currentFrame == imageIndex` 永远成立。这样原有 record 路径所有 `currentFrame` 引用都自动指向"本次 swapchain image 对应的资源槽"，无需逐处修改。

**实现**（标准 Vulkan tutorial 的 `imagesInFlight` 模式）：

```cpp
class GpuScene {
  // ...
  std::vector<VkFence> imagesInFlight;  // size = swapChainImageCount, 初始全 NULL
  uint32_t _syncSlot = 0;               // 同步资源轮转用
  uint32_t currentFrame = 0;            // = imageIndex（acquire 后赋值）
};

void GpuScene::Draw() {
    // 1. 等当前 sync slot 的 fence（确保 cmd buffer / semaphore 可复用）
    vkWaitForFences(... inFlightFences[_syncSlot] ...);

    // 2. acquire image
    uint32_t imageIndex;
    vkAcquireNextImageKHR(..., imageAvailableSemaphores[_syncSlot], ..., &imageIndex);

    // 3. 这个 image 上一次可能由其他 sync slot 提交，工作可能未完。
    //    在 CPU 写它的 per-image 资源前要等 image 自己的 fence。
    if (imagesInFlight[imageIndex] != VK_NULL_HANDLE) {
        vkWaitForFences(... imagesInFlight[imageIndex] ...);
    }
    imagesInFlight[imageIndex] = inFlightFences[_syncSlot];

    vkResetFences(... inFlightFences[_syncSlot] ...);

    // 4. 关键步骤：让 currentFrame 与 imageIndex 同步
    currentFrame = imageIndex;

    // 5. record + submit + present 用 _syncSlot 做同步资源
    recordCommandBuffer(imageIndex, commandBuffers[_syncSlot]);
    // ... 用 imageAvailableSemaphores[_syncSlot] / renderFinishedSemaphores[_syncSlot]
    // ... fence = inFlightFences[_syncSlot]

    // 6. _syncSlot 独立轮转（currentFrame 下次 acquire 会被重赋值）
    _syncSlot = (_syncSlot + 1) % framesInFlight;
}
```

**为什么这样行**：
- record 路径所有 `currentFrame` 现在都等于 `imageIndex` → 自动正确
- per-frame buffer 在 CPU 写之前由 `imagesInFlight` 等到 GPU 不再用
- sync 资源（cmd buffer / semaphore / fence）按 `_syncSlot` 独立轮转，仍然每帧一进位

## 修改文件

| 文件 | 改动 |
|---|---|
| `Src/Include/GpuScene.h` | 加 `imagesInFlight` 数组、`_syncSlot` 计数器，`currentFrame` 注释更新 |
| `Src/GpuScene.cpp` `Draw()` | 重写：`_syncSlot` 用于同步资源；`currentFrame = imageIndex`；插入 `imagesInFlight` 等待 |
| `Src/GpuScene.cpp` `createSyncObjects()` | 初始化 `imagesInFlight = vector<VkFence>(N, VK_NULL_HANDLE)` |
| `Src/GpuScene.cpp` `recreateSwapChain()` | image count 不变时也清零 `imagesInFlight` |
| `Src/GpuScene.cpp` `getCurrentCommandBuffer()` | 从 `commandBuffers[currentFrame]` 改为 `commandBuffers[_syncSlot]` |

不需要改的文件（自动正确）：
- `Light.cpp`：`gpuScene.currentFrame` 等于 imageIndex，per-image 引用自动对齐
- `Shadow.cpp`：shadow map 是静态资源不依赖 swapchain index
- `Raytracing.cpp`：早就在用 `imageIndex` 参数

## 附带修复（Light.cpp pre-existing bug）

`Light.cpp:1378-1396` 的 image barrier 是错的：

```cpp
// 修复前
barrier.oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
barrier.newLayout = VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL;
barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;  // 单 aspect
```

问题：
1. **oldLayout 已经过时**：`drawDecals` / 我新加的 conditional transition 已经把 depth 转到了 `DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL`，这里再用 `DEPTH_STENCIL_ATTACHMENT_OPTIMAL` 作 oldLayout 就和实际状态对不上。
2. **aspectMask 单 aspect + combined-layout enum**：spec 上不太严格，但 VVL 的 per-aspect tracking 对 stencil aspect 也会做 enum-equality 检查，发现 `DEPTH_STENCIL_ATTACHMENT_OPTIMAL` 和实际的 `DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL` 不同 enum，报错：
   ```
   cannot transition the layout of aspect=2, level=0, layer=0 from
   VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL when the previous known
   layout is VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL.
   ```
3. **逻辑重复**：layout 已经被前面的 transition 转好了，这里没必要再做 image barrier。

```cpp
// 修复后：纯 memory barrier，不动 image layout
VkMemoryBarrier memBarrier{};
memBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
memBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
vkCmdPipelineBarrier(cmd, COMPUTE_SHADER, COMPUTE_SHADER, 0,
                     1, &memBarrier, 0, nullptr, 0, nullptr);
```

## 验证

修复后：
1. 多 decal 渲染正确（每个在自己位置）
2. validation layer 不再报 `oldLayout` mismatch
3. validation layer 不再报 `expects ... DEPTH_READ_ONLY_STENCIL_ATTACHMENT` 之类
4. `useClusterLighting=true` 路径里 `Light.cpp:1385` 那个 barrier 也自动指向正确 image

## 工程参考

- Vulkan tutorial: <https://vulkan-tutorial.com/Drawing_a_triangle/Drawing/Frames_in_flight>
- "Why imagesInFlight": <https://github.com/Overv/VulkanTutorial/blob/main/code/15_hello_triangle.cpp>
- 标准约定：sync 资源 per-frame-in-flight；GPU image 资源 per-swapchain-image
