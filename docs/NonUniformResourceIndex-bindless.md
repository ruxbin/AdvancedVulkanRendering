# Bindless 纹理 NonUniformResourceIndex 问题

## 现象

Linux 上 RT 输出的画面中，纹理（albedo / normal / etc.）出现**整块整块的错色**：屏幕被切成 ~8×4 / 16×2 / 32×1 的小矩形，每块内部使用同一张纹理，但相邻块用的纹理不同。

Windows 上同一份 SPIR-V 完全正常。

参考截图：`F:\albedo.jpg` 中红框内部。

## 根因

着色器对 bindless 纹理数组（`Texture2D _Textures[]`）做索引时，索引值在一个 subgroup（wave）内不是常量：

```hlsl
// rt_lighting.hlsl, ClosestHit
AAPLShaderMaterial mat = materialsRT[h.materialIndex];   // 不同光线 → 不同 chunk → 不同 material
half4 baseColor = _Textures[mat.albedo_texture_index].SampleLevel(...);
//                          ^^^^^^^^^^^^^^^^^^^^^^^^ subgroup 内每条 lane 取值不同
```

**SPIR-V / Vulkan 规范**：访问 descriptor array 时，如果索引在 subgroup 内非一致（"non-uniform"），HLSL 必须用 `NonUniformResourceIndex(...)` 显式标注，否则行为未定义。对应 SPIR-V 会带：

- `OpCapability ShaderNonUniform`
- `OpCapability SampledImageArrayNonUniformIndexing`
- 索引、采样指令上的 `OpDecorate %X NonUniform`

**驱动差异**：

| 驱动 | 未标 `NonUniform` 的行为 |
|---|---|
| Windows NVIDIA / AMD | 大多数情况自动处理，看起来正常 |
| Linux RADV / Mesa / 部分 NVIDIA Linux | 严格按 spec —— 假设索引 subgroup-uniform，整个 wave 共用第 0 lane 的 index → 一块块的色块（块大小 = subgroup 在屏幕上的 tile） |

## 修复

在所有 bindless 数组访问的 index 表达式上包 `NonUniformResourceIndex(...)`：

```hlsl
// 修复前
half4 baseColor = _Textures[mat.albedo_texture_index].SampleLevel(_LinearRepeatSampler, h.uv, lod);

// 修复后
half4 baseColor = _Textures[NonUniformResourceIndex(mat.albedo_texture_index)].SampleLevel(_LinearRepeatSampler, h.uv, lod);
```

注意：

- 只对**descriptor array 的索引**包，不需要对普通 `StructuredBuffer<T>` 的索引包。
- `materialsRT[h.materialIndex]` 是普通 SSBO 索引 → **不需要**。
- `_Textures[mat.xxx_texture_index]` 是 sampled image array 索引 → **需要**。
- 验证 SPIR-V 是否带：
  ```
  spirv-dis foo.spv | grep -E "NonUniform|SampledImageArrayNonUniformIndexing"
  ```

## 已修复

`shaders/rt_lighting.hlsl`：
- `ClosestHitPrimary` 中 4 处采样（albedo / roughness / emissive / normal）
- `AnyHitAlpha` 中 1 处（alpha 测试用 albedo）
- `AnyHitAlphaShadow` 中 1 处（alpha 测试用 albedo）

`shaders/drawcluster.hlsl` GPU indirect 路径（material 来自 `meshChunks[chunkindex].materialIndex`，跨 chunk 边界时索引非一致）：
- `RenderSceneBasePass`（line 148）— 4 处（albedo / roughness / emissive / normal）
- `RenderSceneBasePassAlphaMask`（line 297）— 4 处
- `RenderSceneForwardPSIndirect`（line 340）— 4 处
- `RenderSceneShadowDepthIndirect`（line 380）— 1 处（仅 albedo for alpha mask）

**为何 raster 之前看起来正常？**

- 一个 subgroup（典型 32 像素，8×4 排列）通常落在同一个三角形 → 同一 chunk → 同一 material → 索引天然 uniform
- 只在三角形/chunk 边界出现非一致索引，受影响像素数远少于 RT
- 视觉上不显眼，但严格说仍是 UB；改完更稳

## 不需要修复

`shaders/drawcluster.hlsl` 的 push constant 路径（`materialIndex = pushConstants.materialIndex` 全 draw 共享，必然 uniform）：
- `RenderSceneBasePS`（已废弃）
- `RenderSceneDepthOnly`
- `RenderSceneForwardPS`

## 检查清单

新写带 bindless 资源的 shader 时：

1. 索引值是常量 / push constant / 整 dispatch 共享 → 不需要 `NonUniformResourceIndex`
2. 索引值来自 `StructuredBuffer` / 顶点属性 / 光线命中数据 → **必须** `NonUniformResourceIndex`
3. 在 Linux/RADV 上跑一次 — Windows 通过不代表代码正确
4. `spirv-dis` 检查输出是否带 `NonUniform` 装饰

## 参考

- HLSL: [`NonUniformResourceIndex` intrinsic](https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/dx-graphics-hlsl-intrinsic-functions)
- Vulkan spec: "Shader Resource Interface" → "Non-Uniform Indexing"
- VK_EXT_descriptor_indexing
