# TAA 分支合并到 raytracing 分支

把 `taa` 分支的 Resolve Pass (TAA + ACES Tone Mapping) 功能移植到 `raytracing` 分支，并通过 ImGui 添加开关，默认关闭。

## 为什么没用 `git merge`

`taa` 分支和 `raytracing` 分支共同祖先是 `1201f55`，两边后续都改了大量代码：

- `taa` 比 `raytracing` 多 1 个 commit：`11ad7af taa&resolve code generate by AI`
- `raytracing` 比 `taa` 多 25+ commits：raytracing、screen-space decal、SAO、HZB cull、shadow opt 等

直接 `git merge taa` 会在 `Src/GpuScene.cpp`、`Src/Include/GpuScene.h`、`shaders/commonstruct.hlsl` 等核心文件产生大面积冲突。改成手工把 `11ad7af` 这一个 commit 的功能性改动按 raytracing 分支的当前结构重新落地。

## 文件改动

### `Src/Include/Common.h`

- `uniformBufferData` 末尾加 `mat4 prevViewProjectionMatrix`（TAA reprojection 用）
- `FrameConstants` 末尾加 `invPhysicalSize / taaJitter / exposure / taaEnabled`，并给 `physicalSize` 补 `alignas(16)` 让新加的 `vec2` 字段对齐

### `shaders/commonstruct.hlsl`

镜像 C++ 改动：`AAPLFrameConstants` 加四个新字段，`CameraParamsBufferFull` 加 `prevViewProjectionMatrix`。

### `Src/Include/GpuScene.h`

新增成员：

```cpp
mat4 _prevViewProjectionMatrix;
bool _taaFirstFrame = true;
uint32_t _taaFrameIndex = 0;
bool _taaEnabled = false;  // ImGui toggle, default OFF

VkImage / VkDeviceMemory / VkImageView _hdrLightingBuffer*;
VkImage / VkDeviceMemory / VkImageView _taaHistoryBuffer*;
VkRenderPass _resolvePass;
std::vector<VkFramebuffer> _resolveFrameBuffer;
VkDescriptorSetLayout _resolveSetLayout;
VkDescriptorPool _resolveDescriptorPool;
std::vector<VkDescriptorSet> _resolveDescriptorSets;  // per-frame
VkPipelineLayout _resolvePipelineLayout;
VkPipeline _resolvePipeline;
VkSampler _linearClampSampler;
```

新增方法声明：`createHDRLightingBuffer / createTAAHistoryBuffer / createResolvePass / createResolveFrameBuffer / createResolveDescriptors / createResolvePipeline / createLinearClampSampler`。

### `Src/GpuScene.cpp`

#### Render pass 格式

| 函数 | 改动 |
|------|------|
| `CreateDeferredLightingPass` | 颜色 attachment format: `swapChainFormat` → `R16G16B16A16_SFLOAT` |
| `CreateForwardLightingPass`  | format 同上；finalLayout: `PRESENT_SRC_KHR` → `SHADER_READ_ONLY_OPTIMAL` |
| `CreateDeferredLightingFrameBuffer` | 颜色 view: `swapChainImageView(i)` → `_hdrLightingBufferView` |
| `CreateForwardLightingFrameBuffer`  | 同上 |

#### Constructor 初始化顺序

在已有的创建调用前后插入：

```cpp
CreateGBuffers();
createHDRLightingBuffer();        // NEW
createTAAHistoryBuffer();         // NEW
...
CreateForwardLightingPass();
createResolvePass();              // NEW
CreateBasePassFrameBuffer();
CreateDeferredLightingFrameBuffer(...);
CreateForwardLightingFrameBuffer(...);
createResolveFrameBuffer(...);    // NEW
createTextureSampler();
createNearestClampSampler();
createLinearClampSampler();       // NEW
...
createSAOResources();
createDecalResources();
createResolveDescriptors();       // NEW
createGraphicsPipeline(...);
createRenderOccludersPipeline(...);
createOccluderWireframePipeline();
createResolvePipeline();          // NEW
```

#### `recordCommandBuffer` 改动

- Halton (base 2,3) jitter，8 sample 循环；仅当 `_taaEnabled` 为 true 才计算并应用到 projection 矩阵
- 缓存一份 clean (未 jitter) 的 view/proj/inv 矩阵
- 上传 jittered projection 给 vertex shading 用；inverse VP 用 clean 矩阵
- 新增上传 `prevViewProjectionMatrix`
- 上传完 frame constants 后存当帧的 clean VP 到 `_prevViewProjectionMatrix` 给下一帧用
- TAA 开启时 `_taaFrameIndex++` 并清掉 `_taaFirstFrame`；TAA 关闭时重置两者，避免再次开启时混到陈旧 history

forward pass 结束、`vkEndCommandBuffer` 之前插入 resolve pass：

```cpp
vkCmdBeginRenderPass(cmd, &resolvePassInfo, ...);
vkCmdBindPipeline(cmd, GRAPHICS, _resolvePipeline);
VkDescriptorSet sets[] = {globalDescriptorSets[currentFrame],
                          _resolveDescriptorSets[currentFrame]};
vkCmdBindDescriptorSets(cmd, ..., 0, 2, sets, 0, nullptr);
vkCmdDraw(cmd, 3, 1, 0, 0);  // fullscreen triangle
vkCmdEndRenderPass(cmd);
```

#### Cleanup / Recreate

- `cleanupSwapChainResources`：销毁 resolve framebuffers / HDR buffer / TAA history buffer，重置 `_taaFirstFrame` / `_taaFrameIndex`
- `recreateSwapChainResources`：重建 HDR/history/resolve framebuffers，按新 view 重写 resolve descriptor set 的 binding 0/1/2

#### Resolve Pass 设计

| Attachment | Image | Layout | LoadOp / StoreOp |
|------------|-------|--------|------------------|
| 0 | swapchain image | UNDEFINED → PRESENT_SRC_KHR | DONT_CARE / STORE |
| 1 | `_taaHistoryBuffer` (R8G8B8A8_SRGB) | UNDEFINED → SHADER_READ_ONLY | DONT_CARE / STORE |

MRT 双输出，避免单独的 history copy。

#### Resolve Descriptor Set (set 1, per-frame)

| Binding | Type | 内容 |
|---------|------|------|
| 0 | SAMPLED_IMAGE | `_hdrLightingBufferView` |
| 1 | SAMPLED_IMAGE | `_taaHistoryBufferView` |
| 2 | SAMPLED_IMAGE | `device.getWindowDepthOnlyImageView(frame)` (per-frame) |
| 3 | SAMPLER | `nearestClampSampler` |
| 4 | SAMPLER | `_linearClampSampler` |

Set 0 复用 `globalSetLayout` (camera + frame constants UBO)。

#### ImGui

`renderImGuiOverlay` 加一行：

```cpp
ImGui::Checkbox("TAA", &_taaEnabled);
```

放在已有的 "Ray Tracing" checkbox 后面。

### `shaders/resolve.hlsl` (新文件)

从 `taa` 分支直接拷贝。算法：

1. 采当前像素的 HDR 值
2. ACES tone mapping (`exposure * pixel`)
3. 如果 `taaEnabled`：
   - 3×3 邻域 min/max bounding box（tone-mapped 空间）
   - 用 `depthTex` 重建世界坐标 (`worldPositionForTexcoord`)
   - 用 `prevViewProjectionMatrix` 重投影到上一帧 UV
   - Catmull-Rom 5-tap 采 history
   - 把 history clamp 到邻域 bounding box
   - blend：5% current + 95% history（屏外重投影 blend=0）
4. 同时输出到 `SV_Target0` (swapchain) 和 `SV_Target1` (history)

### `shaders/compile_shaders.bat`

末尾追加：

```bat
REM Resolve/PostFX pass (TAA + ACES Tone Mapping)
D:\VulkanSDK\1.3.296.0\Bin\dxc.exe -spirv -T ps_6_0 resolve.hlsl ^
  -fspv-debug=vulkan-with-source -E ResolvePS -Fo resolve.ps.spv
```

dxc 路径换成 raytracing 分支已经在用的 `D:\VulkanSDK\1.3.296.0\Bin\dxc.exe`（taa 分支用的是 UE5 路径）。

## 适配 raytracing 分支的两处关键调整

### 1. 深度 view 从 single 变 per-frame

taa 分支：`device.getWindowDepthOnlyImageView()`（单例）
raytracing 分支：`device.getWindowDepthOnlyImageView(frameIndex)`（per-frame）

因此 `_resolveDescriptorSet` 从单个 `VkDescriptorSet` 改成 `std::vector<VkDescriptorSet>` per-frame，descriptor pool 按 `framesInFlight` 倍数分配，每个 set 绑对应帧的深度 view。

### 2. prevVP 矩阵组合顺序

taa 分支原版：

```cpp
_prevViewProjectionMatrix = cleanProj * cleanView;
```

raytracing 分支这里改成：

```cpp
_prevViewProjectionMatrix = cleanView * cleanProj;
```

**原因**：项目自带 `mat4::operator*` 实现里 `A * B`（代码）实际算的是数学 `B * A`，跟 GLM 反着的。要得到数学 `P*V`，代码要写 `view * proj`。`Camera.cpp:74` 算 `viewprojmatrix` 就是这种写法。Shader 里 `mul(prevVP, worldPos)` 期望 prevVP 是数学 `P*V`，所以代码必须写 `view * proj`。

## 验证

- MSBuild Debug 构建通过：`Bin/AdvancedVulkanRendering.exe` 生成成功
- `resolve.ps.spv` dxc 编译通过（约 40 KB）
- 没做实际运行验证，建议跑一下确认默认关闭/打开 TAA 的画面表现
