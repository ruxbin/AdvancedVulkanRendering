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
| `shaders/drawcluster.hlsl` | 新增入口点 | 1, 4 |
| `shaders/shadowcull.hlsl` | 新建 | 2 |
| `shaders/hiz.hlsl` | 新建 | 3 |
| `shaders/compile_shaders.bat` | 新增编译命令 | 1, 2, 3, 4 |
| `Src/GpuScene.cpp` | 大量修改 | 1, 3, 4 |
| `Src/Include/GpuScene.h` | 新增成员 | 1, 3, 4 |
| `Src/Shadow.cpp` | 重写 RenderShadowMap | 2 |
| `Src/Include/Shadow.h` | 新增成员 | 2 |
