# DX12 Scatter Volume and Spot-Light Parity Design

## Context

The Vulkan backend already supplies ScatterVolume with procedural blue noise,
procedural 3D Perlin noise, point lights, spot lights, spot-light tile indices,
spot shadow maps, and spot view-projection matrices. The DX12 backend has the
ScatterVolume and accumulation compute passes, but several descriptor slots are
still bound to null resources. DX12 also has spot-light culling shader support
and buffer members, yet it does not load spot lights or render spot shadows.
The deferred pass consequently receives no spot-light data either.

This change completes the existing DX12 lighting pipeline rather than adding a
ScatterVolume-only workaround. The implementation will follow the Vulkan
backend's scene interpretation and shader-visible data formats while retaining
the current DX12 resource and command-recording structure.

## Goals

- Generate and upload the same deterministic 64x64 R8 blue-noise pattern and
  32x32x32 R8 Perlin/fBm volume used by Vulkan.
- Load spot lights from `scene.scene` and derive matching culling, lighting, and
  shadow-matrix data.
- Run spot-light coarse and tiled culling for point-only, spot-only, and mixed
  scenes.
- Render up to 32 spot shadow maps into a 256x256 depth texture array, including
  alpha-masked geometry.
- Bind the completed point/spot/noise/shadow data to both ScatterVolume and
  deferred lighting at their existing HLSL registers.
- Keep empty-light and partial-resource cases valid and diagnosable.
- Preserve the current DX12 backend architecture and avoid unrelated Vulkan or
  renderer refactors.

## Non-goals

- Dynamic editing or animation of spot lights and their shadow maps.
- More than 32 shadow-casting spot lights.
- Replacing the existing tiled light culler or cascaded-shadow implementation.
- Extracting a new cross-backend renderer abstraction.
- Changing the ScatterVolume froxel dimensions, temporal accumulation model,
  or public controls.

## Chosen Approach

Implement Vulkan-equivalent behavior directly inside the existing DX12 scene
path. CPU-side policy helpers will isolate deterministic data generation and
spot-light derivation so those rules can be unit tested without a D3D12 device.
DX12GpuScene will own the GPU resources, descriptors, PSOs, and command
recording, consistent with its current point-light and cascaded-shadow code.

This approach has a smaller compatibility surface than introducing a shared
backend abstraction, while still avoiding untestable calculations embedded in
descriptor setup and command-list code.

## CPU Data Preparation

### Noise

Blue noise is generated once as a 64x64 byte array using the same Bayer-derived
formula as Vulkan. Perlin noise is generated once as a 32-cubed byte array using
the same fixed permutation table, three-octave fBm calculation, and `[-1, 1]`
to `[0, 1]` remapping. Both generators are deterministic and return complete
upload-ready byte arrays.

### Spot lights

`CreateLights()` parses every entry in `scene.scene["spot_lights"]`. For each
valid entry it reads position, RGB intensity, outer cone angle, height,
direction, and the transparent-light flag. The direction is normalized before
any derived data is computed.

The culling sphere matches Vulkan:

- For an outer angle greater than pi/4, the center is
  `position + direction * height` and the radius is
  `height * tan(outerAngle)`.
- Otherwise, the radius is `height / (2 * cos(outerAngle)^2)` and the center is
  `position + direction * radius`.

Each GPU culling record is an 80-byte `AAPLSpotLightCullingData`:

- `posRadius`: bounding-sphere center and radius.
- `posAndHeight`: light position and linear range.
- `dirAndOuterAngle`: normalized direction and cosine of the outer angle.
- `color`: RGB intensity and a sign encoding the transparent flag.
- `cosInnerAngle`: cosine of `outerAngle * SPOT_LIGHT_INNER_SCALE`.

The light view uses the Vulkan up-vector rule: world Y unless the direction is
nearly vertical, then world Z. The projection uses a two-times-outer-angle
field of view, aspect one, near 0.1, and far equal to the light height. The
stored combined matrix uses the convention already consumed by the shared
HLSL shaders.

Malformed entries with a near-zero direction, non-positive height, invalid
cone angle, or a non-finite derived matrix are skipped with a warning. A skipped
entry is removed from both the culling-data and matrix arrays so indices remain
stable across all consumers.

## GPU Resources and Lifetime

The following scene-sized resources are created once and survive resize:

- Structured spot-light buffer, with at least one dummy element for an empty
  scene.
- Structured spot view-projection matrix buffer, with 32 entries. Unused entries
  are initialized to identity.
- 64x64 `DXGI_FORMAT_R8_UNORM` blue-noise texture.
- 32x32x32 `DXGI_FORMAT_R8_UNORM` Perlin texture.
- 256x256x32 typeless spot-shadow texture array with D32 DSV views and an
  R32-float array SRV.
- A DSV descriptor heap containing one slice view per shadow-capable light.
- A spot-shadow root signature and opaque/alpha-masked PSOs.

Noise upload resources are retained until the initialization command list has
finished, then released. Noise textures transition from copy destination to
non-pixel shader resource. Light and matrix buffers are uploaded as immutable
default-heap resources.

Resize recreates only tile-resolution-dependent XZ range, light-index, and
debug resources. It does not recreate noise, spot data, matrices, or spot
shadows.

## Spot Shadow Rendering

Spot shadows are static for the current scene model and render once, before the
first ScatterVolume or deferred-lighting read. At most
`SPOT_SHADOW_MAX_COUNT` (32) lights render shadows; later lights remain fully
lit in the existing shader fallback while still contributing lighting and
volumetrics.

The spot-shadow root signature contains:

- Root constants for the current spot view-projection matrix and material
  index.
- The material structured buffer.
- The existing bindless texture table.
- A linear-repeat static sampler for alpha testing.

A DX12-compatible spot-shadow vertex entry point transforms world-space
vertices by the pushed matrix. The opaque PSO writes depth without a pixel
shader. The alpha-masked PSO uses a dedicated pixel entry point to sample the
chunk material's albedo and apply the existing alpha cutoff. Direct indexed
draws iterate opaque chunks followed by alpha-masked chunks, matching the
Vulkan spot-shadow path and avoiding a second indirect-culling layout.

Every array slice is cleared to depth 1.0. After the one-time render, the full
array transitions from depth-write to the combined pixel and non-pixel shader
resource state. It stays readable for the remainder of the scene lifetime.

If the spot-shadow shader or PSO is unavailable, the array remains cleared to
1.0 and transitions to the same readable state. Spot lights continue to work
without shadows and a warning identifies the degraded path.

## Light Culling

Light culling returns early only when both point-light and spot-light counts are
zero. A zero count in either individual category does not suppress the other.
All structured buffers allocate at least one element so descriptors remain
valid, while the count constants ensure dummy elements are never processed.

The existing spot coarse-cull and traditional tiled-cull shader paths are used.
After compute writes complete, point and spot tile-index buffers transition to
the combined pixel/non-pixel shader resource state because deferred lighting
reads them in a pixel shader and ScatterVolume reads them in a compute shader.
They return to unordered-access state before the next frame's culling dispatch.

## Descriptor Contract

The existing HLSL register layout remains unchanged.

ScatterVolume receives:

| Register | Resource |
| --- | --- |
| `t5` | Blue-noise `Texture2D` |
| `t6` | Point-light structured buffer |
| `t7` | Point-light tile indices |
| `t8` | Spot-light structured buffer |
| `t9` | Spot-light tile indices |
| `t10` | Spot-shadow `Texture2DArray` |
| `t12` | Spot view-projection matrices |
| `t13` | Perlin `Texture3D` |

Deferred lighting receives:

| Register | Resource |
| --- | --- |
| `t11` | Spot-light structured buffer |
| `t12` | Spot-light tile indices |
| `t13` | Spot-shadow `Texture2DArray` |
| `t15` | Spot view-projection matrices |

The existing comparison static samplers remain at `s11` for ScatterVolume and
`s14` for deferred lighting. Descriptor construction will use small typed
helpers so empty structured buffers, 2D textures, 3D textures, and texture
arrays receive descriptors of the correct dimension rather than a generic
placeholder descriptor.

## Command Ordering

The relevant dependency order is:

1. GBuffer, depth/Hi-Z, and SAO work completes.
2. Point and spot light culling produces tile-index buffers.
3. The first frame renders static spot shadows, or prepares a cleared unshadowed
   array when shadow rendering is unavailable.
4. ScatterVolume reads both light categories, their tile indices, cascade and
   spot shadows, blue noise, Perlin noise, and history.
5. The accumulation pass writes the filtered scatter volume.
6. Deferred lighting reads the same spot data and shadow resources plus the
   accumulated scatter volume.

The one-time spot-shadow pass may be recorded earlier in the frame if command
organization makes that simpler, provided its final transition completes before
steps 4 and 6.

## Readiness and Failure Behavior

ScatterVolume readiness requires both compute PSOs, both root signatures, both
volume textures, blue noise, and Perlin noise. A missing mandatory resource
causes the effective scatter scale written to the frame constants to become
zero. Empty spot-light data does not disable ScatterVolume.

Spot shadow failure is non-fatal and degrades to unshadowed spot lighting.
Invalid scene entries are skipped individually. Resource-creation failures that
leave no type-correct readable fallback are logged and prevent only the
dependent optional pass; they do not create a descriptor pointing at a resource
of the wrong dimension.

## Testing

CPU policy tests cover:

- Blue-noise and Perlin dimensions, byte range, determinism, and fixed sample
  values.
- Spot culling spheres on both sides of the pi/4 branch.
- Direction normalization, inner/outer cosine values, transparent flag encoding,
  up-vector selection, and finite view-projection matrices.
- Rejection of malformed spot entries.
- Light-culling dispatch policy for point-only, spot-only, mixed, and empty
  scenes.
- Scatter readiness with every mandatory resource present and with each class
  of mandatory resource absent.

Build verification recompiles all DX12 shaders, including the spot-shadow vertex
and alpha-mask pixel entries, then builds the DX12 Debug target and runs the
existing and new policy tests.

Runtime verification uses the current scene as the integration case:

- All 31 valid spot lights load.
- ScatterVolume and deferred descriptor tables contain real resources in every
  register listed above.
- Enabling scatter shows point and spot volumetric contribution with blue-noise
  dithering, Perlin modulation, and spot-shadow occlusion.
- Deferred spot lighting and spot shadows are visible independently of scatter.
- The D3D12 debug layer reports no descriptor-dimension or resource-state
  violations.

## Expected Files

- `Src/DX12/DX12GpuScene.h`: new resource members, constants, and helper methods.
- `Src/DX12/DX12GpuScene.cpp`: parsing, uploads, descriptors, PSOs, command
  recording, state transitions, and readiness updates.
- `Src/DX12/DX12ScenePolicy.h`: deterministic and device-independent policy
  functions used by tests and the DX12 backend.
- `shaders/drawclusterShadowSpot.hlsl`: dual-backend spot-shadow entries.
- `shaders/compile_shaders_dx12.bat`: DXIL compilation commands.
- `Tests/DX12ScenePolicyTests.cpp`: expanded CPU regression coverage.

No Vulkan shader binding or runtime behavior is intentionally changed.
