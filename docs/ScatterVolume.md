# Scatter Volume (Froxel-Based Volumetric Scattering)

Ported from **`D:\ModernRenderingWithMetal`** (`AAPLScatterVolume.metal/.mm/.h`).

---

## Overview

Implements volumetric fog and light scattering using a **froxel** (frustum-aligned voxel) volume.  
Each froxel covers **8×8 screen pixels** and one logarithmic depth slice, giving a volume of:

```
volumeW = ceil(screenW / 8)
volumeH = ceil(screenH / 8)
volumeD = 64 slices  (covers 0–100 m view depth, logarithmically distributed)
```

The result is a **3D RGBA16F texture** sampled by the deferred lighting fragment shader to apply fog and in-scattered light.

---

## Render Pipeline Position

```
RenderShadowMap()
  ↓
[ScatterVolume dispatch]    ← NEW (needs shadow maps + camera UBO)
  ↓
DrawOccluders + generateHiZPyramid
  ↓
GPU Culling (gpucull.cs)
  ↓
GBuffer pass (drawcluster.base.*)
  ↓
SAO
  ↓
Deferred lighting pass      ← samples accumVolumeView (binding 11)
```

---

## Two Compute Passes

### Pass 1 – `ScatterVolume`  (`scattervolume.cs.spv`)
Fills each froxel with in-scattered radiance and extinction.

| Thread group | Size |
|---|---|
| Threads per group | 4 × 4 × 4 |
| Groups dispatched | ceil(W/4) × ceil(H/4) × ceil(64/4) |

**Per froxel:**
1. Reconstruct world-space position from froxel `(x, y, z)` using the inverse projection + inverse view matrices.
2. Compute fog density = `scatterScale` (base) + exponential height fog.
3. Evaluate **Schlick phase function** (`g = 0.3`) for sun–viewer angle.
4. Sample cascaded shadow maps to determine sun visibility.
5. Accumulate sun scatter + ambient sky scatter.
6. Write `float4(scatter.rgb, extinction)`.

**Output texture:** `_scatterTex` (RGBA16F 3D, GENERAL layout)

---

### Pass 2 – `AccumulateScattering`  (`accumscatter.cs.spv`)
Front-to-back Beer–Lambert integration through the 64 depth slices.

| Thread group | Size |
|---|---|
| Threads per group | 8 × 8 × 1 |
| Groups dispatched | ceil(W/8) × ceil(H/8) × 1 |

**Per column (x, y):**
```
accum = float4(0, 0, 0, 1)   // rgb = accumulated light, a = transmittance
for z in 0..63:
    thickness = sliceToViewZ(z+1) - sliceToViewZ(z)
    sliceTrans = exp(-extinction * thickness)
    accum.rgb += scatter.rgb * accum.a
    accum.a   *= sliceTrans
    write accumOut[x,y,z] = accum
```

**Output texture:** `_accumTex` (RGBA16F 3D) → transitioned to `SHADER_READ_ONLY_OPTIMAL`.

---

## Depth Encoding (Logarithmic)

```hlsl
// Slice index [0..63] → view-space distance (metres)
float sliceToViewZ(float s) {
    return (exp2(s / 64.0 * 3.0) - 1.0) / 7.0 * 100.0;
}
```

| Slice | Approx view depth |
|---|---|
| 0 | 0.00 m |
| 16 | 2.0 m |
| 32 | 10.5 m |
| 48 | 40 m |
| 63 | 100 m |

---

## Apply in Deferred Lighting

`deferredlighting.hlsl` samples the accumulated volume after the PBR lighting step:

```hlsl
// Linearise reverse-Z depth → view-space distance
float viewZ = -viewPos.z / viewPos.w;
float sliceF = log2(viewZ / 100.0 * 7.0 + 1.0) / 3.0;  // [0,1]

float4 scatter = scatterAccumVolume.SampleLevel(linearClampSampler,
    float3(uv.x, uv.y, sliceF), 0);

// Compose: surface colour × transmittance + in-scattered light
result = result * scatter.a + scatter.rgb;
```

---

## New Descriptor Bindings

### `deferredlighting.hlsl` – Set 1

| Binding | Type | Name |
|---|---|---|
| 11 | `Texture3D<float4>` (SAMPLED_IMAGE) | `scatterAccumVolume` |
| 12 | `SamplerState` (SAMPLER) | `linearClampSampler` |

### `scattervolume.hlsl` – ScatterVolume kernel, Set 0

| Binding | Type | Name |
|---|---|---|
| 0 | `RWTexture3D<float4>` (STORAGE_IMAGE) | `scatterOut` |
| 1 | `cbuffer` (UNIFORM_BUFFER) | camera + frame constants |
| 2 | `Texture2DArray<float>` (SAMPLED_IMAGE) | cascade shadow maps |
| 3 | `SamplerComparisonState` (SAMPLER) | shadow comparison sampler |

### `scattervolume.hlsl` – AccumulateScattering kernel, Set 0

| Binding | Type | Name |
|---|---|---|
| 0 | `Texture3D<float4>` (SAMPLED_IMAGE) | `scatterIn` |
| 1 | `RWTexture3D<float4>` (STORAGE_IMAGE) | `accumOut` |

---

## New / Modified Files

| File | Change |
|---|---|
| `shaders/scattervolume.hlsl` | **New** – both compute kernels |
| `shaders/commonstruct.hlsl` | Added `skyColor`, `scatterScale` to `AAPLFrameConstants` |
| `shaders/deferredlighting.hlsl` | Added bindings 11/12; apply scatter after PBR |
| `Src/Include/Common.h` | Added `skyColor`, `scatterScale` to `FrameConstants` |
| `Src/Include/GpuScene.h` | `#include "ScatteringVolume.h"`, added `_scatterVolume`, `_linearClampSampler`, `createScatterVolume()`, `createLinearClampSampler()` |
| `Src/Include/ScatteringVolume.h` | **New** – class declaration |
| `Src/ScatteringVolume.cpp` | **New** – class implementation |
| `Src/GpuScene.cpp` | `init_deferredlighting_descriptors`: added bindings 11/12; init: call `createLinearClampSampler()` + `createScatterVolume()`; `recordCommandBuffer`: `_scatterVolume.dispatch()` after shadow pass |

---

## Build – Compiling Shaders

The same HLSL file is compiled twice with different entry points:

```bat
dxc -spirv -T cs_6_0 -E ScatterVolume        shaders/scattervolume.hlsl -Fo shaders/scattervolume.cs.spv
dxc -spirv -T cs_6_0 -E AccumulateScattering shaders/scattervolume.hlsl -Fo shaders/accumscatter.cs.spv
```

---

## Tuning Parameters

| Parameter | Location | Default | Effect |
|---|---|---|---|
| `scatterScale` | `GpuScene.h` → `frameConstants` | `0.02` | Global fog density |
| `skyColor` | `GpuScene.h` → `frameConstants` | `(0.4, 0.6, 1.0)` | Ambient sky inscatter colour |
| `SCATTERING_RANGE` | `scattervolume.hlsl` | `100.0 m` | Far depth of volume |
| `SCATTER_VOLUME_DEPTH` | `ScatteringVolume.h` | `64` | Depth slice count |
| Height fog offset | `scattervolume.hlsl` `froxelToWorldPos` | `2.0 m` | Fog floor altitude |
| Height fog falloff | same | `0.35` | Exponential decay rate |
| Schlick anisotropy `g` | same | `0.3` | Forward/backward scattering bias |

---

## Differences from Metal Reference

| Aspect | Metal (`AAPLScatterVolume`) | This (Vulkan) |
|---|---|---|
| Temporal reprojection | ✅ 85% blend with previous frame | ❌ Not implemented (no history buffer) |
| 3D Perlin noise | ✅ detail noise modulation | ❌ Not implemented |
| Local point/spot lights | ✅ with cluster culling | ❌ Not implemented |
| Rasterization rate map | ✅ variable-rate | ❌ N/A (Vulkan VRS differs) |
| Double-buffered volume | ✅ (temporal) | ❌ Single buffer |

Future work: add temporal reprojection (store previous-frame accum volume and blend with `0.85` factor each frame) and 3D noise modulation for detail.
