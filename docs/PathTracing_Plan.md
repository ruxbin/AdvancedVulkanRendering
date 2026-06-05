# Hardware Path Tracing — Implementation Plan

## 当前状态

- 已有完整 RT 基础设施：BLAS/TLAS、RT Pipeline、SBT、Bindless 材质系统
- `rt_lighting.hlsl` 已有 RayGen / ClosestHit / AnyHit / Miss 框架，但只做了单次弹射，输出原始 albedo
- 无 accumulation buffer，无 BRDF 重要性采样，无多次弹射

---

## Phase 1 — Progressive Accumulation 框架

**目标：** 逐帧累积采样，相机静止时持续收敛

### C++ 侧（Raytracing.h / Raytracing.cpp）

1. **添加 Accumulation Buffer**
   - `_accumImage[]`, `_accumMemory[]`, `_accumImageView[]`（per-frame, `R32G32B32A32_SFLOAT`）
   - 在 `CreateOutputImagesAndDescriptorSet()` 中创建，并用 single-time command 预转换为 `GENERAL` 布局

2. **修改 Descriptor Set（binding 12 = accumImage）**
   - `VK_DESCRIPTOR_TYPE_STORAGE_IMAGE`，`RWTexture2D<float4>` 可读写
   - Binding count: 12 → 13
   - Pool sizes: STORAGE_IMAGE × 2×N

3. **相机 Dirty 检测**
   - `_prevViewMatrix`（mat4）存储上一帧的 view matrix
   - `RecordTraceRays()` 中 `memcmp` 比较，变化时 `_accumCount = 0`

4. **Push Constants 更新（仍 32 bytes）**
   - `shadowTaps` → `accumCount`
   - `_pad0` → `maxBounces`
   - `_pad1` → `resetAccum`

### Shader 侧（rt_lighting.hlsl）

5. **Progressive 混合公式**
   ```hlsl
   // 写入 outAccumColor：
   if (resetAccum || accumCount == 0)
       accumulated = radiance;
   else
       accumulated = lerp(outAccumColor[px].rgb, radiance, 1.0 / (accumCount + 1));
   outAccumColor[px] = float4(accumulated, 1.0);
   outLitColor[px]   = float4(aces(accumulated), 1.0);
   ```

---

## Phase 2 — Path Tracing Bounce Loop

**目标：** 多次弹射，取代现有单次直接光照

### 随机数生成（PCG Hash）

```hlsl
uint pcgNext(inout uint state) {
    uint old = state * 747796405u + 2891336453u;
    uint w = ((old >> ((old >> 28u) + 4u)) ^ old) * 277803737u;
    return (w >> 22u) ^ w;
}
// 初始化：(px.x * W + px.y) ^ (frameSeed * 2654435761u) ^ (accumCount * 805459861u)
```

### BRDF 重要性采样

- **Diffuse**: cosine-weighted hemisphere sampling  
  `wi = sqrt(xi.x) * cosTheta + ... ; pdf = cosTheta / PI`
- **Specular**: GGX NDF importance sampling（Walter et al.）  
  `H = sampleGGX(xi); wi = reflect(-V, H); pdf = D * NdH / (4 * VdH)`
- **混合**: 按 Fresnel 权重随机选择  
  `pSpec = luminance(F0_at_N), chooser ~ U(0,1)`

### 迭代式 Bounce Loop（在 RayGen 中）

```hlsl
float3 throughput = 1.0, radiance = 0.0;
for (uint bounce = 0; bounce < maxBounces; ++bounce) {
    TraceRay(tlas, ..., primaryRay, payload);     // primary / secondary ray
    if (!hit) { radiance += throughput * skyColor(dir); break; }
    radiance += throughput * emissive;             // emissive contribution
    doNEE(wsPos, N, ...);                          // → Phase 3
    float3 wi; float3 weight = sampleBRDF(..., wi);
    throughput *= weight;
    if (bounce >= 3) { RussianRoulette(); }
    ray = makeRay(wsPos + N*eps, wi);
}
```

### Russian Roulette

```hlsl
if (bounce >= 3) {
    float q = clamp(max(throughput.r, max(throughput.g, throughput.b)), 0.01, 0.95);
    if (rng() > q) break;
    throughput /= q;
}
```

---

## Phase 3 — NEE + Environment Light

**目标：** Next Event Estimation，减少方差

### 太阳光 NEE

```hlsl
// 在每次弹射时发射一条 shadow ray 到太阳盘
float3 sdir = sampleConeDir(sunAxis, sunConeRadius, rng2());
ShadowPayload sp;
TraceRay(tlas, ACCEPT_FIRST_HIT | SKIP_CLOSEST_HIT, ..., shadowRay, sp);
if (sp.visible)
    radiance += throughput * evalBRDF(N, V, sdir, ...) * sunLight;
```

### Sky Gradient（Miss Shader）

```hlsl
float3 skyColor(float3 dir) {
    float t  = saturate(dir.y * 0.5 + 0.5);
    return lerp(float3(0.8, 0.9, 1.0), float3(0.1, 0.3, 0.8), t*t) * 2.0;
}
```

### Firefly Clamp

```hlsl
radiance = min(radiance, FIREFLY_CLAMP);  // FIREFLY_CLAMP = 20.0
```

### ACES Filmic Tone Mapping

```hlsl
float3 aces(float3 x) {
    x *= 0.6;
    return saturate((x*(2.51*x+0.03)) / (x*(2.43*x+0.59)+0.14));
}
```

---

## Phase 4 — 降噪与打磨（可选 / 未实现）

| 技术 | 说明 |
|---|---|
| A-Trous Wavelet 滤波 | 低延迟空间滤波，适合实时 PT 预览 |
| Temporal Reprojection | 复用上一帧已收敛区域 |
| Sobol / Blue Noise | 替换 PCG，低差异序列收敛更快 |
| VK_KHR_ray_query | 在 compute 内联 shadow 查询 |

---

## 文件改动清单

| 文件 | 改动 |
|---|---|
| `docs/PathTracing_Plan.md` | 本文档 |
| `Src/Include/Raytracing.h` | 增加 accumImage 组、_accumCount、_prevViewMatrix、maxBounces |
| `Src/Raytracing.cpp` | 创建 accum buffer、更新 descriptor、camera dirty 检测 |
| `shaders/rt_lighting.hlsl` | PCG、BRDF 采样、bounce loop、NEE、accum blend、tone map |

---

## 关键参数

| 参数 | 默认值 | 说明 |
|---|---|---|
| `maxBounces` | 4 | 最大路径深度，建议 3-6 |
| `sunConeRadius` | 0.0087 rad | 太阳盘半角，~0.5° |
| `FIREFLY_CLAMP` | 20.0 | 辐亮度上限，防止萤火虫噪声 |
| `ALPHA_CUTOUT` | 0.1 | Alpha mask 阈值 |
