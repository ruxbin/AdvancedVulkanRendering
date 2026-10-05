# GPU-Driven Rendering Optimization — AdvancedVulkanRendering

## 问题分析

`GpuScene::Draw()` 性能瓶颈的根因：

| 问题 | 位置 | 影响 |
|------|------|------|
| `#define USE_CPU_ENCODE_DRAWPARAM` 强制走 CPU 路径 | `GpuScene.cpp:22` | 所有 chunk 逐个发 `vkCmdDrawIndexed`，CPU 开销巨大 |
| Shadow 渲染无任何剔除 | `Shadow.cpp:567-607` | 3 cascade × N chunks = 3N 次 draw call，无 frustum cull |
| Occluder Z-Pass 深度未被利用 | `GpuScene.cpp:3299-3337` | 画了 occluder 深度但没有生成 Hi-Z 做遮挡剔除 |
| 透明物体 CPU 逐个绘制 | `GpuScene.cpp:3649-3666` | CPU 循环 + push constants + 单独 draw call |

## 当前渲染管线

```
CPU: UpdateShadowMatrices
CPU: Map/Unmap uniform buffers (每帧 3 次)
GPU: DrawOccluders (Z-Pass)          ← 深度数据未被复用
GPU: RenderShadowMap (3 cascades)    ← CPU 逐 chunk 循环，无剔除
GPU: Base Pass (G-Buffer)            ← CPU 逐 chunk 循环 + frustum cull
GPU: Light Culling (Compute)
GPU: Deferred Lighting Pass
GPU: Forward Pass (透明物体)         ← CPU 逐 chunk 循环
```

## 优化后渲染管线

```
CPU: UpdateShadowMatrices
CPU: Map uniform buffers (持久映射)
GPU: DrawOccluders (Z-Pass)
GPU: GenerateHiZ (Compute, mipmap chain)      ← 新增
GPU: ShadowCull (Compute, per cascade)         ← 新增
GPU: EncodeDrawBuffer (Compute, frustum + Hi-Z) ← 已有但被禁用，增强
  ├→ Opaque indirect commands
  ├→ AlphaMask indirect commands
  └→ Transparent indirect commands
GPU: RenderShadowMap (Indirect Draw)           ← 改造
GPU: Base Pass (2x IndirectCount)              ← 改造
GPU: Light Culling (Compute)
GPU: Deferred Lighting Pass
GPU: Forward Pass (1x IndirectCount)           ← 改造
```

---

## Stage 1: 启用 GPU Indirect Draw（Base Pass）

### 目标
移除 `USE_CPU_ENCODE_DRAWPARAM`，修复 GPU 路径的 opaque/alpha-masked 分流问题。

### 核心改动

#### 1. `shaders/gpucull.hlsl` — 按材质类型分流输出

当前：所有 visible chunk 写入同一个 buffer 区域。

改为：
- `writeIndex[0]` = opaque 可见数量
- `writeIndex[1]` = alpha-masked 可见数量
- Opaque 写入 `drawParams[0..opaqueChunkCount-1]`
- AlphaMask 写入 `drawParams[opaqueChunkCount..opaqueChunkCount+alphaMaskedChunkCount-1]`

```hlsl
if (chunkIndex < opaqueChunkCount) {
    InterlockedAdd(writeIndex[0], 1, insertIndex);
    drawParams[insertIndex] = { indexCount, 1, indexBegin, 0, insertIndex };
    chunkIndices[insertIndex] = chunkIndex;
    instanceToDrawIDMap[insertIndex] = insertIndex;
} else if (chunkIndex < opaqueChunkCount + alphaMaskedChunkCount) {
    InterlockedAdd(writeIndex[1], 1, insertIndex);
    uint offset = opaqueChunkCount;
    drawParams[offset + insertIndex] = { indexCount, 1, indexBegin, 0, offset + insertIndex };
    chunkIndices[offset + insertIndex] = chunkIndex;
    instanceToDrawIDMap[offset + insertIndex] = offset + insertIndex;
}
```

#### 2. `shaders/drawcluster.hlsl` — 新增 alpha-mask base pass 入口

```hlsl
PSOutput RenderSceneBasePassAlphaMask(VSOutput input) {
    // 与 RenderSceneBasePass 相同的材质查找（从 SSBO 读取）
    // 增加 alpha clip
    clip(baseColor.w - ALPHA_CUTOUT);
    // ... G-Buffer 输出
}
```

#### 3. `Src/GpuScene.cpp` — 主要修改

- 移除 `#define USE_CPU_ENCODE_DRAWPARAM`
- `writeIndexBuffer` 扩展为 `3 * sizeof(uint32_t)`
- 每帧重置 3 个 write counter
- 创建 `drawclusterBasePipelineAlphaMask` pipeline
- `DrawChunksBasePass()` 改为两次 `vkCmdDrawIndexedIndirectCount`

### 预期效果
- 消除 Base Pass 中 N 次 CPU draw call → 2 次 GPU indirect draw
- GPU 端并行 frustum culling

---

## Stage 2: GPU-Driven Shadow Maps

### 目标
Shadow 渲染从 CPU 逐 chunk 循环改为 GPU compute cull + indirect draw。

### 核心改动

#### 1. `shaders/shadowcull.hlsl` — 新建 shadow 剔除 compute shader

```hlsl
// 每个 cascade 独立 dispatch
// 对 cascade 的正交投影视锥体做 frustum cull
// 输出 per-cascade indirect draw commands
[numthreads(128, 1, 1)]
void ShadowCull(uint3 DTid : SV_DispatchThreadID) {
    if (!FrustumCull(cascadeFrustum, meshChunks[chunkIndex].boundingBox)) {
        // Opaque: write to region [cascadeIndex * maxChunks, ...]
        // AlphaMask: write to separate offset
    }
}
```

#### 2. `Src/Shadow.cpp` — RenderShadowMap 改造

```cpp
for (int cascade = 0; cascade < SHADOW_CASCADE_COUNT; ++cascade) {
    // 1. 上传 cascade frustum
    // 2. 重置 write counter
    // 3. Dispatch ShadowCull compute
    // 4. Pipeline barrier (compute → indirect)
    // 5. vkCmdDrawIndexedIndirectCount (opaque)
    // 6. vkCmdDrawIndexedIndirectCount (alpha-masked)
}
```

### 预期效果
- 3N 次 CPU draw call → 6 次 GPU indirect draw (2 per cascade)
- 每个 cascade 自动 frustum cull，跳过不可见 chunk

---

## Stage 3: Hi-Z Occlusion Culling

### 目标
利用 Occluder Z-Pass 深度生成 Hi-Z pyramid，在 GPU cull 中增加遮挡剔除。

### 核心改动

#### 1. `shaders/hiz.hlsl` — Hi-Z 生成 compute shader

```hlsl
// 入口 1: 从 depth texture 拷贝到 R32_FLOAT Hi-Z mip 0
[numthreads(8, 8, 1)]
void CopyDepthToHiZ(uint3 DTid : SV_DispatchThreadID) {
    hizMip0[DTid.xy] = depthTexture.Load(int3(DTid.xy, 0));
}

// 入口 2: 逐级 downsample（MIN，reverse-Z 下保留最远可见深度）
[numthreads(8, 8, 1)]
void DownsampleHiZ(uint3 DTid : SV_DispatchThreadID) {
    float d00 = prevMip.Load(int3(DTid.xy * 2 + uint2(0,0), 0));
    float d10 = prevMip.Load(int3(DTid.xy * 2 + uint2(1,0), 0));
    float d01 = prevMip.Load(int3(DTid.xy * 2 + uint2(0,1), 0));
    float d11 = prevMip.Load(int3(DTid.xy * 2 + uint2(1,1), 0));
    currentMip[DTid.xy] = min(min(d00, d10), min(d01, d11));
}
```

#### 2. `shaders/gpucull.hlsl` — 增加 Hi-Z 遮挡测试

```hlsl
if (passedFrustumCull) {
    // 1. 将 AABB 8 顶点投影到屏幕空间
    // 2. 找到投影矩形的 min/max UV 和 maxDepth（reverse-Z 下最近点）
    // 3. 根据投影像素尺寸选择 mip level
    // 4. 在 Hi-Z 的 4 个角采样取 MIN
    // 5. 如果 maxDepth <= hizMinDepth → 被遮挡
    if (maxAabbDepth <= hizDepth) visible = false;
}
```

#### 3. 执行顺序

```
DrawOccluders()
  ↓
Transition depth → SHADER_READ_ONLY
  ↓
Dispatch CopyDepthToHiZ
  ↓
For each mip 1..N:
    Barrier (compute write → compute read)
    Dispatch DownsampleHiZ
  ↓
Transition Hi-Z → SHADER_READ_ONLY
  ↓
Dispatch EncodeDrawBuffer (frustum + Hi-Z cull)
```

### 预期效果
- 被大型遮挡体挡住的 chunk 不再生成 draw command
- 减少 GPU overdraw 和三角形处理量

---

## Stage 4: GPU-Driven 透明物体

### 目标
Forward pass 透明物体渲染从 CPU 循环改为 GPU indirect draw。

### 核心改动

#### 1. `shaders/gpucull.hlsl` — 增加透明物体区域

```hlsl
else if (chunkIndex < opaqueChunkCount + alphaMaskedChunkCount + transparentChunkCount) {
    InterlockedAdd(writeIndex[2], 1, insertIndex);
    uint offset = opaqueChunkCount + alphaMaskedChunkCount;
    drawParams[offset + insertIndex] = { indexCount, 1, indexBegin, 0, offset + insertIndex };
    chunkIndices[offset + insertIndex] = chunkIndex;
    instanceToDrawIDMap[offset + insertIndex] = offset + insertIndex;
}
```

#### 2. `shaders/drawcluster.hlsl` — 新增 indirect forward 入口

```hlsl
half4 RenderSceneForwardPSIndirect(VSOutput input) : SV_Target {
    uint chunkindex = chunkIndex[input.drawcallid];
    uint materialIndex = meshChunks[chunkindex].materialIndex;
    // ... 与 RenderSceneForwardPS 相同的光照计算
}
```

#### 3. `Src/GpuScene.cpp` — Forward pass 改造

```cpp
// 替换 CPU 循环
vkCmdBindPipeline(commandBuffer, ..., drawclusterForwardPipelineIndirect);
vkCmdDrawIndexedIndirectCount(commandBuffer, drawParamsBuffer,
    (opaqueChunkCount + alphaMaskedChunkCount) * stride,
    writeIndexBuffer, 2 * sizeof(uint32_t),
    transparentChunkCount, stride);
```

### 预期效果
- 消除 Forward pass CPU draw call 循环
- 透明物体也享受 GPU frustum + Hi-Z 剔除

---

## 文件修改清单

| 文件 | 改动类型 | 涉及 Stage |
|------|----------|-----------|
| `shaders/gpucull.hlsl` | 重写 | 1, 3, 4 |
| `shaders/drawcluster.hlsl` | 新增入口点 | 1, 2, 4 |
| `shaders/shadowcull.hlsl` | 新建 | 2 |
| `shaders/hiz.hlsl` | 新建 | 3 |
| `shaders/compile_shaders.bat` | 新增编译命令 | 1, 2, 3, 4 |
| `Src/GpuScene.cpp` | 大量修改 | 1, 3, 4 |
| `Src/Include/GpuScene.h` | 新增成员 | 1, 3, 4 |
| `Src/Shadow.cpp` | 重写 RenderShadowMap + 新增 InitGPUShadowResources | 2 |
| `Src/Include/Shadow.h` | 新增成员 | 2 |

---

## 实现详情

### Stage 1 — GPU Indirect Draw (Base Pass)

#### `Src/GpuScene.cpp`

1. **移除 `#define USE_CPU_ENCODE_DRAWPARAM`**（原第 22 行），GPU indirect 路径成为默认路径。

2. **`writeIndexBuffer` 扩展**：从 `sizeof(uint32_t)` 扩展到 `3 * sizeof(uint32_t)`，存储 `[opaqueCount, alphaMaskCount, transparentCount]`。

3. **`GPUCullParams` 结构体重新布局**（`GpuScene.h`）：
   ```
   offset 0:  opaqueChunkCount      (uint32)
   offset 4:  alphaMaskedChunkCount  (uint32)
   offset 8:  transparentChunkCount  (uint32)
   offset 12: totalPointLights       (uint32)
   offset 16: totalSpotLights        (uint32)
   offset 20: hizMipLevels           (uint32)
   offset 24: screenSize             (float2)
   offset 32: viewProjMatrix         (float4x4, 64 bytes)
   offset 96: frustum                (Frustum, 96 bytes)
   ```

4. **每帧 cull params 上传**：使用 memcpy 按偏移量写入所有字段，包括 viewProjMatrix（用于 Hi-Z AABB 投影）和 hizMipLevels。

5. **每帧重置 3 个 write counter** 到 0。

6. **`DrawChunksBasePass()` 重写**：
   - Opaque: `vkCmdDrawIndexedIndirectCount` 从 `drawParamsBuffer` offset 0 读取，count 从 `writeIndexBuffer` offset 0 读取。
   - AlphaMask: 从 `drawParamsBuffer` offset `opaqueChunkCount * stride` 读取，count 从 `writeIndexBuffer` offset `sizeof(uint32_t)` 读取。

7. **新增 pipeline**：
   - `drawclusterBasePipelineAlphaMask`：使用 `drawcluster.base.alphamask.ps.spv`（`RenderSceneBasePassAlphaMask` 入口），使用 `drawclusterBasePipelineLayout`（无 push constants），从 SSBO 读材质并做 alpha clip。
   - `drawclusterForwardPipelineIndirect`：使用 `drawcluster.forward.indirect.ps.spv`（`RenderSceneForwardPSIndirect` 入口），同样无 push constants。

8. **`gpuCullDescriptorSet` 扩展**：新增 binding 6（Hi-Z sampled image）和 binding 7（Hi-Z sampler），descriptor pool 增加对应类型。

9. **compute dispatch 调整**：`groupx` 改为 `(totalChunks + 127) / 128`（totalChunks = opaque + alphaMask + transparent），barrier 的 dstStageMask 增加了 `VERTEX_SHADER_BIT`。

10. **渲染顺序调整**：`DrawOccluders()` 移到 compute dispatch 之前（原来在 `#ifdef` 中间），以便 Hi-Z 先生成。

#### `shaders/gpucull.hlsl`

- 完全重写。cbuffer 扩展为包含 `opaqueChunkCount`、`alphaMaskedChunkCount`、`transparentChunkCount`、`viewProjMatrix`、`screenSize`、`hizMipLevels`。
- 新增 `IsOccludedByHiZ()` 函数：投影 AABB 8 顶点到屏幕空间，选择对应 mip level，采样 Hi-Z 4 角取 MIN，reverse-Z 下判断是否被遮挡。
- 按 chunk index 范围分流写入 opaque / alphaMask / transparent 三个区域。

#### `shaders/drawcluster.hlsl`

- 新增 `RenderSceneBasePassAlphaMask`：与 `RenderSceneBasePass` 相同的 SSBO 材质读取，增加 `clip(baseColor.w - ALPHA_CUTOUT)`。
- 新增 `RenderSceneForwardPSIndirect`：与 `RenderSceneForwardPS` 相同的光照计算，但从 `chunkIndex[input.drawcallid]` 读材质而非 push constants。
- 新增 `RenderSceneShadowDepthIndirect`：用于 Stage 2 shadow indirect，根据 `specAlphaMask` 特化常量决定是否做 alpha test。

---

### Stage 2 — GPU-Driven Shadow Maps

#### `Src/Include/Shadow.h`

新增成员：
- 6 个 buffer（`_shadowDrawParamsBuffer`、`_shadowWriteIndexBuffer`、`_shadowChunkIndicesBuffer`、`_shadowInstanceBuffer`、`_shadowCullParamsBuffer` 及对应 memory）
- descriptor set / pool / layout（`_shadowCullSetLayout`、`_shadowCullDescriptorPool`、`_shadowCullDescriptorSet`）
- compute pipeline（`_shadowCullPipelineLayout`、`_shadowCullPipeline`）
- `ShadowCullParams` 结构体：`{opaqueChunkCount, alphaMaskedChunkCount, cascadeMaxChunks, cascadeIndex, Frustum}`
- `InitGPUShadowResources()` 方法

#### `Src/Shadow.cpp`

1. **`InitGPUShadowResources()`**：
   - Buffer 布局：`totalSlots = SHADOW_CASCADE_COUNT * cascadeMaxChunks * 2`（每个 cascade 有 opaque 和 alphaMask 两个区域）。
   - 使用 lambda `createBuf` 简化 buffer 创建。
   - 6 个 descriptor set binding 与 `shadowcull.hlsl` 一一对应。
   - 加载 `shadowcull.cs.spv`，创建 compute pipeline（入口 `ShadowCull`）。

2. **`RenderShadowMap()` 重写**：
   - 首次调用时触发 `InitGPUShadowResources()`。
   - 每个 cascade 循环：上传 cascade frustum → 重置 write counter → dispatch ShadowCull → barrier → begin render pass → 2x `vkCmdDrawIndexedIndirectCount`（opaque + alphaMask）→ end render pass。
   - 使用 `_shadowInstanceBuffer` 作为 vertex buffer（替代 `applInstanceBuffer`），因为 shadow cull shader 写入自己的 instance-to-draw mapping。
   - descriptor set 绑定使用 `drawclusterBasePipelineLayout`（无 push constants / 无 dynamic offset）。

#### `shaders/shadowcull.hlsl`

- 每个 cascade 独立 dispatch。
- 按 `cascadeIndex` 计算 buffer 偏移：`cascadeBaseOpaque = cascadeIndex * cascadeMaxChunks * 2`。
- 对 opaque 和 alphaMask 分别写入独立区域，使用独立的原子计数器 `shadowWriteIndex[cascadeIndex * 2 + 0/1]`。

---

### Stage 3 — Hi-Z Occlusion Culling

#### `Src/Include/GpuScene.h`

新增成员：
- `_hizTexture`（VK_FORMAT_R32_SFLOAT，带 mip chain）、`_hizMemory`
- `_hizTextureView`（全 mip chain，用于 cull shader 采样）
- `_hizMipViews`（per-mip view，用于 compute shader storage write）
- `_hizSampler`（nearest, clamp）
- `_hizCopyPipeline`、`_hizDownsamplePipeline`、`_hizPipelineLayout`
- `_hizCopySetLayout`、`_hizDownsampleSetLayout`（相同布局：binding 0 = sampled image, binding 1 = storage image）
- `_hizCopyDescriptorSet`、`_hizDownsampleDescriptorSets`（per-mip）
- `_hizMipLevels`、`_hizWidth`、`_hizHeight`

#### `Src/GpuScene.cpp` — `createHiZResources()`

1. **Hi-Z texture 创建**：`VK_FORMAT_R32_SFLOAT`，`STORAGE_BIT | SAMPLED_BIT`，mip levels = `floor(log2(max(w,h))) + 1`。

2. **Descriptor set layout**：两个相同的 layout（set 0 for CopyDepthToHiZ, set 1 for DownsampleHiZ），每个包含 binding 0（sampled image）+ binding 1（storage image）。

3. **Pipeline layout**：两个 set layout + push constant `{uint2 mipSize}`（8 bytes）。

4. **Descriptor set 分配**：
   - `_hizCopyDescriptorSet`（set 0）：`_depthTextureView`（occluder depth, D32_SFLOAT depth aspect view）→ `_hizMipViews[0]`。
   - `_hizDownsampleDescriptorSets[m-1]`（set 1）：`_hizMipViews[m-1]` → `_hizMipViews[m]`，共 `mipLevels - 1` 个。

5. **Compute pipeline 创建**：
   - `_hizCopyPipeline`：加载 `hiz_copy.cs.spv`，入口 `CopyDepthToHiZ`。
   - `_hizDownsamplePipeline`：加载 `hiz_downsample.cs.spv`，入口 `DownsampleHiZ`。

6. **gpuCullDescriptorSet 更新**：binding 6 绑定 `_hizTextureView`（全 mip chain），binding 7 绑定 `_hizSampler`。

#### `Src/GpuScene.cpp` — `generateHiZPyramid()`

每帧在 `DrawOccluders()` 之后、`EncodeDrawBuffer` compute dispatch 之前执行：

```
1. Barrier: _depthTexture DEPTH_ATTACHMENT → DEPTH_READ_ONLY
2. Barrier: _hizTexture (all mips) UNDEFINED → GENERAL
3. Bind _hizCopyPipeline + set 0, push {width, height}
   Dispatch CopyDepthToHiZ: (width+7)/8 × (height+7)/8
4. For mip = 1 to mipLevels-1:
   a. Barrier: _hizTexture mip[m-1] GENERAL → SHADER_READ_ONLY
   b. Bind _hizDownsamplePipeline + set 1, push {mipW, mipH}
   c. Dispatch DownsampleHiZ: (mipW+7)/8 × (mipH+7)/8
5. Barrier: _hizTexture last mip GENERAL → SHADER_READ_ONLY
6. Barrier: _depthTexture DEPTH_READ_ONLY → DEPTH_ATTACHMENT (恢复给后续 pass)
```

#### `shaders/hiz.hlsl`

- Push constant `{uint2 mipSize}` 用于边界检查。
- `CopyDepthToHiZ`（set 0）：从 `depthTexture`（occluder depth）Load 采样，写入 `hizMip0`。
- `DownsampleHiZ`（set 1）：从 `prevMip` Load 2×2 区域，取 MIN（reverse-Z 下保留最远可见表面），写入 `currentMip`。

#### `shaders/gpucull.hlsl` — `IsOccludedByHiZ()`

- 如果 `hizMipLevels == 0`，直接返回 false（Hi-Z 未就绪）。
- 将 AABB 8 个顶点用 `viewProjMatrix` 投影到 NDC，找到屏幕空间 min/max UV 和 max depth（reverse-Z 下最近点）。
- 任何顶点 `clip.w <= 0`（在摄像机后方）时跳过 Hi-Z test。
- 根据投影像素尺寸选择 mip level：`ceil(log2(max(pixelW, pixelH)))`。
- 在选定 mip 上采样 Hi-Z 的 4 个角取 MIN。
- **遮挡判定**：`maxAabbDepth <= hizMinDepth` → 被遮挡（AABB 最近点仍在 Hi-Z 最远可见表面之后）。

---

### Stage 4 — GPU-Driven 透明物体

#### `Src/GpuScene.cpp`

- Forward pass CPU 循环替换为 `vkCmdDrawIndexedIndirectCount`，从 `drawParamsBuffer` 的 transparent 区域读取。
- 使用 `drawclusterForwardPipelineIndirect`（`drawclusterBasePipelineLayout`，无 push constants）。
- count 从 `writeIndexBuffer` offset `2 * sizeof(uint32_t)` 读取。

#### `shaders/gpucull.hlsl`

- 当 `chunkIndex >= opaqueChunkCount + alphaMaskedChunkCount` 时写入 transparent 区域，使用 `writeIndex[2]`。

---

## 编译命令

`compile_shaders.bat` 新增：

```bat
REM Stage 1
dxc -spirv -T cs_6_2 gpucull.hlsl -E EncodeDrawBuffer -Fo gpucull.cs.spv
dxc -spirv -T ps_6_0 drawcluster.hlsl -E RenderSceneBasePassAlphaMask -Fo drawcluster.base.alphamask.ps.spv

REM Stage 2
dxc -spirv -T cs_6_2 shadowcull.hlsl -E ShadowCull -Fo shadowcull.cs.spv
dxc -spirv -T ps_6_0 drawcluster.hlsl -E RenderSceneShadowDepthIndirect -Fo drawcluster.shadow.indirect.ps.spv

REM Stage 3
dxc -spirv -T cs_6_2 hiz.hlsl -E CopyDepthToHiZ -Fo hiz_copy.cs.spv
dxc -spirv -T cs_6_2 hiz.hlsl -E DownsampleHiZ -Fo hiz_downsample.cs.spv

REM Stage 4
dxc -spirv -T ps_6_0 drawcluster.hlsl -E RenderSceneForwardPSIndirect -Fo drawcluster.forward.indirect.ps.spv
```

---

## Bug Fix: Shadow Acne 修复

### 问题描述

Shadow pass 渲染时出现自阴影（shadow acne）：表面无缘无故被自己的影子覆盖，产生条纹伪影。

### 根因分析

**`Src/Shadow.cpp` line 421**: 深度偏移（depth bias）被禁用
```cpp
rasterizer.depthBiasEnable = VK_FALSE;  // ❌ 禁用深度偏移
```

**问题机制**：
1. 阴影贴图以浮点精度渲染深度
2. 光照计算时，表面到光源的距离可能因浮点舍入误差与阴影贴图中记录的值完全相同或略近
3. 深度对比时（`VK_COMPARE_OP_LESS`），由于精度误差，表面可能被判定为在其自身阴影后
4. 无深度偏移时，无法抵消这种精度误差 → 自阴影

### 修复方案

在 shadow pass 的 rasterizer 配置中启用深度偏移：

**文件**: `Src/Shadow.cpp` lines 413-424

**改动**:
```cpp
VkPipelineRasterizationStateCreateInfo rasterizer{};
rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
rasterizer.depthClampEnable = VK_FALSE;
rasterizer.rasterizerDiscardEnable = VK_FALSE;
rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
rasterizer.lineWidth = 1.0f;
rasterizer.cullMode = VK_CULL_MODE_BACK_BIT;
rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
rasterizer.depthBiasEnable = VK_TRUE;              // ✅ 启用深度偏移
rasterizer.depthBiasConstantFactor = 1.25f;        // 固定偏移（防止自阴影）
rasterizer.depthBiasSlopeFactor = 1.75f;           // 斜率偏移（处理陡峭表面）
rasterizer.depthBiasClamp = 0.0f;                  // 无上限夹钳
```

### 深度偏移参数说明

| 参数 | 值 | 作用 |
|------|-----|------|
| `depthBiasConstantFactor` | 1.25f | 对所有像素添加固定深度偏移，单位为最小深度可表示间隔 |
| `depthBiasSlopeFactor` | 1.75f | 按表面斜率添加额外偏移：`bias += slope * depthBiasSlopeFactor`，处理掠射角表面 |
| `depthBiasClamp` | 0.0f | 不限制总偏移量（避免过度修正） |

### Vulkan 深度偏移公式

```
finalDepth = fragmentDepth + depthBiasConstantFactor * minimumResolvableDepthDifference
                           + depthBiasSlopeFactor * maxDepthSlope
```

其中：
- `minimumResolvableDepthDifference` = 1 / (2^24) 对于 D32_SFLOAT
- `maxDepthSlope` = max(|dZ/dX|, |dZ/dY|) 在像素内的最大深度梯度

### 效果验证

编译并运行后：
- ✅ Shadow map 自阴影消失
- ✅ 阴影轮廓清晰无条纹
- ✅ 投影阴影仍然准确（偏移量极小，不影响可见阴影质量）

### 微调指南（如需进一步调整）

若阴影仍有轻微伪影：
- **增加** `depthBiasConstantFactor`（如 1.5f）
- **增加** `depthBiasSlopeFactor`（如 2.0f）

若阴影出现**peter panning**（表面悬浮在阴影上）：
- **减少** `depthBiasConstantFactor`（如 1.0f）
- **减少** `depthBiasSlopeFactor`（如 1.5f）

### 影响范围

仅影响 shadow pass 的深度值生成（不影响主摄像机深度），修改：
- `_shadowPassPipeline`（opaque pass）
- `_shadowPassPipelineAlphaMask`（alpha-masked pass）

两个 pipeline 使用同一个 rasterizer 配置，修复适用于所有阴影级联。
