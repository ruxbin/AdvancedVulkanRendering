# DX12 Scatter Volume and Spot-Light Parity Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Complete DX12 parity for ScatterVolume noise inputs, spot-light loading/culling/deferred lighting, and static spot shadows.

**Architecture:** Keep GPU ownership and command recording in `DX12GpuScene`, while placing deterministic noise generation, spot-light derivation, dispatch policy, and descriptor offsets in the device-independent `DX12ScenePolicy` layer. Reuse the shared HLSL register contract, add dedicated DX12 spot-cull and spot-shadow PSOs, and use explicit combined pixel/non-pixel resource states for data consumed by both compute and graphics.

**Tech Stack:** C++20, Direct3D 12, HLSL Shader Model 6.0/6.2, DXC, CMake/MSVC, standalone C++ policy tests.

**Spec:** `docs/superpowers/specs/2026-08-29-dx12-scatter-spot-parity-design.md`

## Global Constraints

- Blue noise is exactly 64x64 R8 and Perlin noise is exactly 32x32x32 R8.
- Spot shadows are exactly 256x256 with at most 32 array layers used for shadow-casting lights.
- The current HLSL resource registers do not change.
- Spot lights after index 31 still illuminate and scatter but are treated as unshadowed.
- Empty point-light or spot-light categories use valid minimum-size resources and zero shader counts.
- Spot shadows are static and render at most once per scene lifetime.
- Resize recreates only screen/tile-sized resources.
- No Vulkan runtime behavior or Vulkan binding is intentionally changed.
- Preserve unrelated user changes and stage only files named by the current task.

---

## File Structure

- `Src/DX12/DX12ScenePolicy.h`: device-independent noise, spot derivation, readiness, dispatch, and descriptor-contract functions.
- `Tests/DX12ScenePolicyTests.cpp`: deterministic regression tests for all device-independent behavior.
- `Src/DX12/DX12ResourceHelper.h`: D3D12 3D texture construction and tightly packed R8 upload support.
- `Src/DX12/DX12GpuScene.h`: resource ownership, PSO members, constants, state flags, and helper declarations.
- `Src/DX12/DX12GpuScene.cpp`: resource creation/upload, scene ingestion, culling dispatch, spot-shadow recording, bindings, and state transitions.
- `shaders/drawclusterShadowSpot.hlsl`: dual-backend spot-shadow vertex and alpha-mask pixel entries.
- `shaders/compile_shaders_dx12.bat`: DXIL builds for spot culling and spot shadow entries.

### Task 1: Lock the CPU Policy Contract with Tests

**Files:**
- Modify: `Tests/DX12ScenePolicyTests.cpp`
- Modify: `Src/DX12/DX12ScenePolicy.h`

**Interfaces:**
- Consumes: `vec3`, `vec4`, `mat4`, `AAPLSpotLightCullingData`, `SPOT_LIGHT_INNER_SCALE`, `invLookAt()`, and `perspective()` from the existing math/common headers.
- Produces: `BuildBlueNoise() -> std::array<uint8_t, 4096>`, `BuildPerlinNoise() -> std::vector<uint8_t>`, `TryBuildSpotLight(const SpotLightInput&) -> std::optional<DerivedSpotLight>`, `ShouldDispatchLightCulling(uint32_t, uint32_t) -> bool`, expanded `ScatterAvailability`, and named descriptor offsets.

- [ ] **Step 1: Add failing noise and dispatch-policy tests**

Add these checks to `main()` and add `expectEqual()`/`expectSize()` helpers beside the existing assertions:

```cpp
void expectEqual(uint8_t actual, uint8_t expected, const char* message) {
  if (actual != expected) {
    std::cerr << message << ": expected " << static_cast<uint32_t>(expected)
              << ", got " << static_cast<uint32_t>(actual) << '\n';
    ++failures;
  }
}

void expectSize(size_t actual, size_t expected, const char* message) {
  if (actual != expected) {
    std::cerr << message << ": expected " << expected << ", got " << actual << '\n';
    ++failures;
  }
}

const auto blue = DX12ScenePolicy::BuildBlueNoise();
expectSize(blue.size(), 64u * 64u, "blue-noise size");
expectEqual(blue[0], 0, "blue-noise (0,0)");
expectEqual(blue[1], 128, "blue-noise (1,0)");
expectEqual(blue[8], 0, "blue-noise repeats every eight texels");

const auto perlin = DX12ScenePolicy::BuildPerlinNoise();
expectSize(perlin.size(), 32u * 32u * 32u, "Perlin size");
auto perlinAt = [&](uint32_t x, uint32_t y, uint32_t z) {
  return perlin[(z * 32u + y) * 32u + x];
};
expectEqual(perlinAt(0, 0, 0), 127, "Perlin origin");
expectEqual(perlinAt(1, 0, 0), 139, "Perlin x sample");
expectEqual(perlinAt(1, 2, 3), 145, "Perlin interior sample");
expectEqual(perlinAt(31, 31, 31), 139, "Perlin far sample");
expectTrue(blue == DX12ScenePolicy::BuildBlueNoise(), "blue noise is deterministic");
expectTrue(perlin == DX12ScenePolicy::BuildPerlinNoise(), "Perlin is deterministic");

expectTrue(DX12ScenePolicy::ShouldDispatchLightCulling(1, 0), "point-only dispatch");
expectTrue(DX12ScenePolicy::ShouldDispatchLightCulling(0, 1), "spot-only dispatch");
expectTrue(DX12ScenePolicy::ShouldDispatchLightCulling(1, 1), "mixed dispatch");
expectFalse(DX12ScenePolicy::ShouldDispatchLightCulling(0, 0), "empty scene skips culling");
```

- [ ] **Step 2: Add failing spot-light derivation tests**

Construct inputs on both culling-sphere branches and assert the shader-facing values:

```cpp
DX12ScenePolicy::SpotLightInput narrow{};
narrow.position = vec3(1.0f, 2.0f, 3.0f);
narrow.color = vec3(4.0f, 5.0f, 6.0f);
narrow.direction = vec3(0.0f, -2.0f, 0.0f);
narrow.outerAngle = 0.5f;
narrow.height = 8.0f;
narrow.forTransparent = true;
const auto narrowResult = DX12ScenePolicy::TryBuildSpotLight(narrow);
expectTrue(narrowResult.has_value(), "valid narrow spot");
const float narrowRadius = 8.0f / (2.0f * std::cos(0.5f) * std::cos(0.5f));
expectNear(narrowResult->culling.posRadius.y, 2.0f - narrowRadius, "narrow sphere center");
expectNear(narrowResult->culling.posRadius.w, narrowRadius, "narrow sphere radius");
expectNear(narrowResult->culling.dirAndOuterAngle.y, -1.0f, "normalized direction");
expectNear(narrowResult->culling.dirAndOuterAngle.w, std::cos(0.5f), "outer cosine");
expectNear(narrowResult->culling.cosInnerAngle,
           std::cos(0.5f * SPOT_LIGHT_INNER_SCALE), "inner cosine");
expectNear(narrowResult->culling.color.w, -1.0f, "transparent sign");
expectTrue(DX12ScenePolicy::IsFinite(narrowResult->viewProj), "finite narrow VP");

DX12ScenePolicy::SpotLightInput wide = narrow;
wide.direction = vec3(0.0f, 0.0f, 3.0f);
wide.outerAngle = 1.0f;
wide.forTransparent = false;
const auto wideResult = DX12ScenePolicy::TryBuildSpotLight(wide);
expectNear(wideResult->culling.posRadius.z, 11.0f, "wide sphere center");
expectNear(wideResult->culling.posRadius.w, 8.0f * std::tan(1.0f), "wide sphere radius");
expectNear(wideResult->culling.color.w, 1.0f, "opaque sign");

narrow.direction = vec3(0.0f);
expectFalse(DX12ScenePolicy::TryBuildSpotLight(narrow).has_value(), "zero direction rejected");
narrow.direction = vec3(0.0f, -1.0f, 0.0f);
narrow.height = 0.0f;
expectFalse(DX12ScenePolicy::TryBuildSpotLight(narrow).has_value(), "zero height rejected");
narrow.height = 8.0f;
narrow.outerAngle = 0.0f;
expectFalse(DX12ScenePolicy::TryBuildSpotLight(narrow).has_value(), "zero cone rejected");
```

- [ ] **Step 3: Extend the readiness test with mandatory noise dependencies**

Add `blueNoise` and `perlinNoise` to the all-true setup and dependency-member table. Add compile-time assertions for the descriptor offsets:

```cpp
static_assert(DX12ScenePolicy::ScatterSrvOffset::BlueNoise == 4);
static_assert(DX12ScenePolicy::ScatterSrvOffset::SpotData == 7);
static_assert(DX12ScenePolicy::ScatterSrvOffset::SpotIndices == 8);
static_assert(DX12ScenePolicy::ScatterSrvOffset::SpotShadowArray == 9);
static_assert(DX12ScenePolicy::ScatterSrvOffset::SpotMatrices == 11);
static_assert(DX12ScenePolicy::ScatterSrvOffset::PerlinNoise == 12);
static_assert(DX12ScenePolicy::DeferredSrvOffset::SpotData == 11);
static_assert(DX12ScenePolicy::DeferredSrvOffset::SpotIndices == 12);
static_assert(DX12ScenePolicy::DeferredSrvOffset::SpotShadowArray == 13);
static_assert(DX12ScenePolicy::DeferredSrvOffset::SpotMatrices == 15);
```

- [ ] **Step 4: Compile and run the policy test to verify it fails**

Run from the repository root:

```powershell
g++.exe -std=c++20 -I Src/DX12 -I Src/Include Tests/DX12ScenePolicyTests.cpp -o build-msvc/DX12ScenePolicyTests.exe
```

Expected: compilation fails because the new policy types/functions and readiness members do not exist.

- [ ] **Step 5: Implement the policy interfaces**

Add the following public shapes to `DX12ScenePolicy.h`, then port the Bayer table, fixed Perlin permutation, `fade/grad/perlin3/fbm`, and Vulkan spot formulas verbatim from `Src/ScatteringVolume.cpp` and `Src/GpuScene.cpp`:

```cpp
struct SpotLightInput {
  vec3 position{};
  vec3 color{};
  vec3 direction{};
  float outerAngle = 0.0f;
  float height = 0.0f;
  bool forTransparent = false;
};

struct DerivedSpotLight {
  AAPLSpotLightCullingData culling{};
  mat4 viewProj{};
};

namespace ScatterSrvOffset {
inline constexpr uint32_t BlueNoise = 4;
inline constexpr uint32_t PointData = 5;
inline constexpr uint32_t PointIndices = 6;
inline constexpr uint32_t SpotData = 7;
inline constexpr uint32_t SpotIndices = 8;
inline constexpr uint32_t SpotShadowArray = 9;
inline constexpr uint32_t SpotMatrices = 11;
inline constexpr uint32_t PerlinNoise = 12;
}
namespace DeferredSrvOffset {
inline constexpr uint32_t SpotData = 11;
inline constexpr uint32_t SpotIndices = 12;
inline constexpr uint32_t SpotShadowArray = 13;
inline constexpr uint32_t SpotMatrices = 15;
}

std::array<uint8_t, 64u * 64u> BuildBlueNoise();
std::vector<uint8_t> BuildPerlinNoise();
bool IsFinite(const mat4& matrix);
std::optional<DerivedSpotLight> TryBuildSpotLight(const SpotLightInput& input);
inline bool ShouldDispatchLightCulling(uint32_t pointCount, uint32_t spotCount) {
  return pointCount != 0 || spotCount != 0;
}
```

Implement `TryBuildSpotLight()` with the exact validation domain
`length(direction) > 1e-6`, `height > 0.1`, and
`0 < outerAngle < pi/2`, and return `std::nullopt` when validation or the
finite-matrix check fails. Extend `ScatterAvailability` and
`IsScatterReady()` with `blueNoise` and `perlinNoise`.

- [ ] **Step 6: Rebuild and run the policy test**

```powershell
g++.exe -std=c++20 -I Src/DX12 -I Src/Include Tests/DX12ScenePolicyTests.cpp -o build-msvc/DX12ScenePolicyTests.exe
build-msvc/DX12ScenePolicyTests.exe
```

Expected: exit code 0 with no stderr output.

- [ ] **Step 7: Commit the policy layer**

```powershell
git.exe add -- Src/DX12/DX12ScenePolicy.h Tests/DX12ScenePolicyTests.cpp
git.exe commit -m "test: define DX12 scatter and spot policies"
```

### Task 2: Add R8 Texture Upload Support and Scatter Noise Resources

**Files:**
- Modify: `Src/DX12/DX12ResourceHelper.h`
- Modify: `Src/DX12/DX12GpuScene.h`
- Modify: `Src/DX12/DX12GpuScene.cpp`

**Interfaces:**
- Consumes: `BuildBlueNoise()`, `BuildPerlinNoise()`, existing DX12 device/command-list access, and `FlushCommandQueue()`.
- Produces: `DX12Util::CreateTexture3D()`, `DX12Util::UploadR8Texture()`, `_blueNoiseTexture`, `_perlinNoiseTexture`, and `CreateScatterNoiseResources()`.

- [ ] **Step 1: Make readiness tests represent the missing GPU resources**

Confirm Task 1's test fails if either of these lines is removed from the all-true setup:

```cpp
availability.blueNoise = true;
availability.perlinNoise = true;
```

Run the policy test once with `blueNoise = false`, then restore it. Expected:
the test reports that the missing blue-noise dependency did not disable
scatter. This establishes the integration invariant before GPU work.

- [ ] **Step 2: Add focused texture helpers**

Add `CreateTexture3D()` beside `CreateTexture2D()`:

```cpp
inline ComPtr<ID3D12Resource> CreateTexture3D(
    ID3D12Device* device, uint32_t width, uint32_t height, uint16_t depth,
    DXGI_FORMAT format, D3D12_RESOURCE_FLAGS flags,
    D3D12_RESOURCE_STATES initialState) {
  D3D12_HEAP_PROPERTIES heap{};
  heap.Type = D3D12_HEAP_TYPE_DEFAULT;
  D3D12_RESOURCE_DESC desc{};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
  desc.Width = width;
  desc.Height = height;
  desc.DepthOrArraySize = depth;
  desc.MipLevels = 1;
  desc.Format = format;
  desc.SampleDesc.Count = 1;
  desc.Flags = flags;
  ComPtr<ID3D12Resource> texture;
  ThrowIfFailed(device->CreateCommittedResource(
      &heap, D3D12_HEAP_FLAG_NONE, &desc, initialState, nullptr,
      IID_PPV_ARGS(&texture)), "CreateTexture3D");
  return texture;
}
```

Add `UploadR8Texture()` with this exact contract:

```cpp
ComPtr<ID3D12Resource> UploadR8Texture(
    ID3D12Device* device, ID3D12GraphicsCommandList* cmdList,
    ID3D12Resource* destination, const uint8_t* source,
    uint32_t width, uint32_t height, uint32_t depth,
    D3D12_RESOURCE_STATES finalState);
```

Use `GetCopyableFootprints()` for subresource 0, allocate an upload buffer of
the returned required size, copy each row into `Footprint.RowPitch`, advance
each Z slice by `Footprint.RowPitch * height`, call `CopyTextureRegion()`, and
transition `COPY_DEST -> finalState`. Return the upload resource so the caller
keeps it alive through queue completion.

- [ ] **Step 3: Add scene-owned noise resources**

Declare:

```cpp
ComPtr<ID3D12Resource> _blueNoiseTexture;
ComPtr<ID3D12Resource> _perlinNoiseTexture;
void CreateScatterNoiseResources();
```

Call `CreateScatterNoiseResources()` immediately before
`CreateScatterResources()` in the constructor. In the implementation, reset the
current allocator/list, create a 64x64 R8 2D texture and 32-cubed R8 3D texture
in `COPY_DEST`, upload policy bytes with `UploadR8Texture()`, transition both to
`NON_PIXEL_SHADER_RESOURCE`, execute, flush, and release the two returned upload
resources after the flush. Catch `std::exception` at the helper boundary, reset
both texture members, and log
`spdlog::warn("DX12: scatter noise unavailable: {}", error.what())` so
the readiness policy disables ScatterVolume without aborting the renderer.

- [ ] **Step 4: Bind noise at the existing scatter offsets**

Replace the null descriptors at offsets `BlueNoise` and `PerlinNoise` with:

```cpp
D3D12_SHADER_RESOURCE_VIEW_DESC blue{};
blue.Format = DXGI_FORMAT_R8_UNORM;
blue.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
blue.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
blue.Texture2D.MipLevels = 1;
dev->CreateShaderResourceView(_blueNoiseTexture.Get(), &blue, blueHandle);

D3D12_SHADER_RESOURCE_VIEW_DESC perlin{};
perlin.Format = DXGI_FORMAT_R8_UNORM;
perlin.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
perlin.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
perlin.Texture3D.MipLevels = 1;
dev->CreateShaderResourceView(_perlinNoiseTexture.Get(), &perlin, perlinHandle);
```

Update `IsScatterReady()` to pass both resource-presence booleans into
`ScatterAvailability`.

- [ ] **Step 5: Build the DX12 target and run policy tests**

```powershell
cmake.exe --build build-msvc --config Debug --target AdvancedVulkanRendering
build-msvc/DX12ScenePolicyTests.exe
```

Expected: build succeeds and the policy test exits 0.

- [ ] **Step 6: Commit noise resource support**

```powershell
git.exe add -- Src/DX12/DX12ResourceHelper.h Src/DX12/DX12GpuScene.h Src/DX12/DX12GpuScene.cpp
git.exe commit -m "feat: upload DX12 scatter noise textures"
```

### Task 3: Load and Upload Spot Lights

**Files:**
- Modify: `Src/DX12/DX12GpuScene.h`
- Modify: `Src/DX12/DX12GpuScene.cpp`

**Interfaces:**
- Consumes: `TryBuildSpotLight()` and `AAPLSpotLightCullingData` from Task 1.
- Produces: populated `_spotLights`, `_spotViewProjMatrices`, `_spotLightBuffer`, and `_spotViewProjBuffer` with identical indices.

- [ ] **Step 1: Add a scene fixture assertion to the policy test**

Add a JSON-independent loop that builds three valid inputs and one invalid
input, appending only successful results. Assert both output arrays have three
entries and matching positions at every index. This test fails if the caller
advances one array independently:

```cpp
DX12ScenePolicy::SpotLightInput a{};
a.position = vec3(1.0f, 2.0f, 3.0f);
a.color = vec3(2.0f, 1.0f, 0.5f);
a.direction = vec3(0.0f, -1.0f, 0.0f);
a.outerAngle = 0.4f;
a.height = 10.0f;
DX12ScenePolicy::SpotLightInput b = a;
b.position = vec3(-2.0f, 3.0f, 4.0f);
b.direction = vec3(1.0f, -1.0f, 0.0f);
DX12ScenePolicy::SpotLightInput c = a;
c.position = vec3(5.0f, 6.0f, 7.0f);
c.direction = vec3(0.0f, 0.0f, -1.0f);
DX12ScenePolicy::SpotLightInput invalid = a;
invalid.direction = vec3(0.0f);
const DX12ScenePolicy::SpotLightInput inputs[] = {a, b, invalid, c};

std::vector<AAPLSpotLightCullingData> culling;
std::vector<mat4> matrices;
for (const auto& input : inputs) {
  if (auto derived = DX12ScenePolicy::TryBuildSpotLight(input)) {
    culling.push_back(derived->culling);
    matrices.push_back(derived->viewProj);
  }
}
expectSize(culling.size(), 3, "valid spot count");
expectSize(matrices.size(), culling.size(), "spot arrays remain index-aligned");
```

- [ ] **Step 2: Run the policy test**

```powershell
g++.exe -std=c++20 -I Src/DX12 -I Src/Include Tests/DX12ScenePolicyTests.cpp -o build-msvc/DX12ScenePolicyTests.exe
build-msvc/DX12ScenePolicyTests.exe
```

Expected: exit code 0; the fixture documents the ingestion rule used below.

- [ ] **Step 3: Parse spot lights through the policy layer**

In `CreateLights()`, map each JSON entry to `SpotLightInput` using keys
`position_x/y/z`, `color_r/g/b`, `direction_x/y/z`, `coneRad`, `height`, and
`for_transparent`. For every successful result, append both outputs together:

```cpp
if (auto derived = DX12ScenePolicy::TryBuildSpotLight(input)) {
  _spotLights.push_back(derived->culling);
  _spotViewProjMatrices.push_back(derived->viewProj);
} else {
  spdlog::warn("DX12: skipping invalid spot light {}", sceneIndex);
}
```

Declare `std::vector<mat4> _spotViewProjMatrices;` and
`ComPtr<ID3D12Resource> _spotViewProjBuffer;`. Log both the valid and rejected
counts. Remove the existing four-light debug fallback for an empty
`point_lights` array; neither light category receives synthetic scene data.

- [ ] **Step 4: Upload valid-or-dummy spot data and 32 matrices**

Use a single initialization command list. Upload one zeroed
`AAPLPointLightCullingData` when `_pointLights` is empty and one zeroed
`AAPLSpotLightCullingData` when `_spotLights` is empty; otherwise upload each
real vector. Use `pointCapacity = max(pointCount, 1u)` and
`spotCapacity = max(spotCount, 1u)` for XZ-range resources and descriptors,
while preserving the real zero counts in shader constants. Build a 32-element
matrix array initialized to identity and copy
`min(_spotViewProjMatrices.size(), 32)` matrices into it before uploading. Keep
both upload resources alive until `FlushCommandQueue()` returns.

Add static assertions next to the upload:

```cpp
static_assert(sizeof(AAPLSpotLightCullingData) == 80);
static_assert(sizeof(mat4) == 64);
```

- [ ] **Step 5: Build and inspect the current scene count**

```powershell
cmake.exe --build build-msvc --config Debug --target AdvancedVulkanRendering
```

Expected: build succeeds. On the next application run, the initialization log
must say `DX12: Loaded 31 spot lights` and zero rejected entries.

- [ ] **Step 6: Commit spot ingestion**

```powershell
git.exe add -- Src/DX12/DX12GpuScene.h Src/DX12/DX12GpuScene.cpp
git.exe commit -m "feat: load DX12 spot lights"
```

### Task 4: Execute the Existing Spot-Light Culling Kernels

**Files:**
- Modify: `shaders/compile_shaders_dx12.bat`
- Modify: `Src/DX12/DX12GpuScene.h`
- Modify: `Src/DX12/DX12GpuScene.cpp`

**Interfaces:**
- Consumes: spot buffers from Task 3, `ShouldDispatchLightCulling()` from Task 1, and existing `CoarseCullSpot`, `TraditionalCullSpot`, `ClearLightIndicesSpot` HLSL entries.
- Produces: `_coarseCullSpotPSO`, `_traditionalCullSpotPSO`, `_clearIndicesSpotPSO`, and readable `_spotLightIndicesBuffer` output each frame.

- [ ] **Step 1: Add failing shader artifact checks**

Run in `shaders/`:

```powershell
Test-Path CoarseCullSpot.cs.cso,TraditionalCullSpot.cs.cso,ClearIndicesSpot.cs.cso
```

Expected before editing the batch file: three `False` results.

- [ ] **Step 2: Compile all three spot kernels**

Add:

```bat
%DXC% %DXFLAGS% -enable-16bit-types -T cs_6_2 lightculling.hlsl -E CoarseCullSpot -Fo CoarseCullSpot.cs.cso
%DXC% %DXFLAGS% -enable-16bit-types -T cs_6_2 lightculling.hlsl -E TraditionalCullSpot -Fo TraditionalCullSpot.cs.cso
%DXC% %DXFLAGS% -enable-16bit-types -T cs_6_2 lightculling.hlsl -E ClearLightIndicesSpot -Fo ClearIndicesSpot.cs.cso
```

Re-run the command from Step 1. Expected: DXC exits 0 and all three paths are
`True`.

- [ ] **Step 3: Create spot-cull PSOs**

Declare three PSO members and load the new CSOs through the existing `loadCS`
lambda in `CreateLightCullPipelines()`:

```cpp
loadCS("CoarseCullSpot.cs.cso", _coarseCullSpotPSO);
loadCS("TraditionalCullSpot.cs.cso", _traditionalCullSpotPSO);
loadCS("ClearIndicesSpot.cs.cso", _clearIndicesSpotPSO);
```

- [ ] **Step 4: Make culling category-independent**

Replace the point-only early return with:

```cpp
const uint32_t pointCount = static_cast<uint32_t>(_pointLights.size());
const uint32_t spotCount = static_cast<uint32_t>(_spotLights.size());
if (!DX12ScenePolicy::ShouldDispatchLightCulling(pointCount, spotCount)) return;
```

Keep descriptor allocations at a minimum element count of one, while putting
the real counts into `LightCullCBData`.

- [ ] **Step 5: Dispatch point and spot kernels conditionally**

Clear and dispatch point kernels only when `pointCount > 0`; clear and dispatch
spot kernels only when `spotCount > 0`. The spot sequence is:

```cpp
cmdList->SetPipelineState(_clearIndicesSpotPSO.Get());
cmdList->Dispatch(tileW, tileH, 1);
DX12Util::UAVBarrier(cmdList, _spotLightIndicesBuffer.Get());

cmdList->SetPipelineState(_coarseCullSpotPSO.Get());
cmdList->Dispatch((spotCount + 127) / 128, 1, 1);
DX12Util::UAVBarrier(cmdList, _spotXZRangeBuffer.Get());

cmdList->SetPipelineState(_traditionalCullSpotPSO.Get());
cmdList->Dispatch(tileW, tileH, 1);
DX12Util::UAVBarrier(cmdList, _spotLightIndicesBuffer.Get());
```

Transition both point and spot index buffers from UAV to
`PIXEL_SHADER_RESOURCE | NON_PIXEL_SHADER_RESOURCE` after dispatch. At the
start of the next frame, transition each active category from that combined
read state back to UAV. Keep XZ/debug/transparent buffers in the existing
COMMON-to-UAV-to-COMMON cycle unless a consumer actually reads them.

- [ ] **Step 6: Compile shaders, build, and run policy tests**

```powershell
Push-Location shaders
cmd.exe /c compile_shaders_dx12.bat
Pop-Location
cmake.exe --build build-msvc --config Debug --target AdvancedVulkanRendering
build-msvc/DX12ScenePolicyTests.exe
```

Expected: DXC and the C++ build succeed; policy tests exit 0.

- [ ] **Step 7: Commit spot culling**

```powershell
git.exe add -- shaders/compile_shaders_dx12.bat shaders/CoarseCullSpot.cs.cso shaders/TraditionalCullSpot.cs.cso shaders/ClearIndicesSpot.cs.cso Src/DX12/DX12GpuScene.h Src/DX12/DX12GpuScene.cpp
git.exe commit -m "feat: cull DX12 spot lights"
```

### Task 5: Add Static Spot Shadow Rendering

**Files:**
- Modify: `shaders/drawclusterShadowSpot.hlsl`
- Modify: `shaders/compile_shaders_dx12.bat`
- Modify: `Src/DX12/DX12GpuScene.h`
- Modify: `Src/DX12/DX12GpuScene.cpp`

**Interfaces:**
- Consumes: `_spotViewProjMatrices`, mesh buffers/chunks/materials/textures, and the 32-light limit.
- Produces: `_spotShadowMapArray`, one DSV per layer, `_spotShadowRootSig`, opaque/alpha PSOs, and `RecordSpotShadowMaps()`.

- [ ] **Step 1: Add failing DXIL compilation commands manually**

Run in `shaders/` before editing the shader:

```powershell
D:\VulkanSDK\1.3.296.0\Bin\dxc.exe -D DX12_BACKEND -T vs_6_0 drawclusterShadowSpot.hlsl -E RenderSceneVSShadowSpot -Fo drawclusterShadowSpot.vs.cso
D:\VulkanSDK\1.3.296.0\Bin\dxc.exe -D DX12_BACKEND -T ps_6_0 drawclusterShadowSpot.hlsl -E RenderSceneShadowSpotAlpha -Fo drawclusterShadowSpot.alpha.ps.cso
```

Expected: the alpha command fails because `RenderSceneShadowSpotAlpha` does not
exist; the vertex command may also fail on the Vulkan-only declarations.

- [ ] **Step 2: Make the spot-shadow shader dual-backend**

Include `shadercompat.hlsl`. Use one b0 root-constant block containing a matrix
and material index, bind materials at `t0,space1`, bind textures at
`t0,space2`, and use the existing linear sampler at `s1,space1`:

```hlsl
struct AAPLShaderMaterial {
    uint albedo_texture_index;
    uint roughness_texture_index;
    uint normal_texture_index;
    uint emissive_texture_index;
    float alpha;
    uint hasMetallicRoughness;
    uint hasEmissive;
    uint padding;
};

struct SpotShadowConstants {
    float4x4 viewProj;
    uint materialIndex;
};
DECLARE_PUSH_CONSTANTS(SpotShadowConstants, spotShadow, 0);
VK_BINDING(0,1) StructuredBuffer<AAPLShaderMaterial> materials REGISTER_SRV(0,1);
VK_BINDING(2,1) Texture2D<half4> spotTextures[] REGISTER_SRV(0,2);
VK_BINDING(1,1) SamplerState spotLinearRepeat REGISTER_SAMPLER(1,1);

void RenderSceneShadowSpotAlpha(VSOutput input) {
    AAPLShaderMaterial material = materials[spotShadow.materialIndex];
    half alpha = spotTextures[NonUniformResourceIndex(material.albedo_texture_index)]
        .Sample(spotLinearRepeat, input.TextureUV).a;
    clip(alpha - 0.1h);
}
```

For DX12, do not depend on `SV_InstanceID` carrying StartInstanceLocation;
material identity comes from the root constant. Retain Vulkan compilation by
keeping the same `RenderSceneVSShadowSpot` entry and compatibility macros.

- [ ] **Step 3: Add the two shader builds to the batch file**

```bat
%DXC% %DXFLAGS% -T vs_6_0 drawclusterShadowSpot.hlsl -E RenderSceneVSShadowSpot -Fo drawclusterShadowSpot.vs.cso
%DXC% %DXFLAGS% -T ps_6_0 drawclusterShadowSpot.hlsl -E RenderSceneShadowSpotAlpha -Fo drawclusterShadowSpot.alpha.ps.cso
```

Run Step 1 again. Expected: both commands exit 0.

- [ ] **Step 4: Create resources and root signature**

Declare constants `SPOT_SHADOW_SIZE = 256` and `SPOT_SHADOW_COUNT = 32`, the
texture array, 32-entry DSV heap, root signature, two PSOs, and
`bool _spotShadowsRendered = false`.

Create a typeless R32 2D array with `ALLOW_DEPTH_STENCIL`, initial
`DEPTH_WRITE`, and clear depth 1.0. Create 32 `D32_FLOAT`
`TEXTURE2DARRAY` DSVs with `FirstArraySlice = i`, `ArraySize = 1`.

The root signature has:

```text
parameter 0: 17 x 32-bit root constants at b0 space0, ALL
parameter 1: descriptor table t0 space1, one SRV, PS
parameter 2: descriptor table t0 space2, unbounded SRVs, PS
static sampler: s1 space1, linear, wrap, PS
```

Create an opaque depth-only PSO and an alpha PSO using the compiled pixel
shader. Both use `LESS`, depth writes enabled, no render targets,
`DXGI_FORMAT_D32_FLOAT`, 256x256 viewport/scissor, and the existing four vertex
streams. Put spot-shadow creation in its own `CreateSpotShadowResources()`
boundary. If resource or PSO creation throws, catch it at that boundary, reset
the affected PSO, log the exception, and retain the depth array whenever it was
successfully created so the clear-to-unshadowed fallback remains available.

- [ ] **Step 5: Record the one-time direct draw pass**

Implement `RecordSpotShadowMaps(ID3D12GraphicsCommandList*)`:

```cpp
if (_spotShadowsRendered || !_spotShadowMapArray) return;
const uint32_t lightCount = std::min<uint32_t>(_spotLights.size(), SPOT_SHADOW_COUNT);
```

Bind vertex/index buffers and the materials/bindless descriptor tables once.
Obtain CPU chunk data by decompressing `_applMesh->_chunkData` to an
`AAPLMeshChunk` array for the duration of this one-time pass, then free it after
recording; do not assume `DX12GpuScene` owns Vulkan's `m_Chunks` pointer.
For each of the 32 layers, clear depth to 1.0. For layers below `lightCount`,
set the layer DSV and matrix constants, draw opaque chunks with the opaque PSO,
then draw alpha-masked chunks with the alpha PSO while setting the chunk's
material index as root constant DWORD 16 before each draw. Draw calls use each
chunk's `indexCount` and `indexBegin` with base vertex zero.

Transition the entire array to
`PIXEL_SHADER_RESOURCE | NON_PIXEL_SHADER_RESOURCE`, set
`_spotShadowsRendered = true`, and restore the screen viewport/scissor. Call
this function after light culling and before ScatterVolume.

If either PSO is absent, clear all 32 slices, perform the readable transition,
set the rendered flag, and log that spot lighting is running unshadowed.

- [ ] **Step 6: Compile and build**

```powershell
Push-Location shaders
cmd.exe /c compile_shaders_dx12.bat
Pop-Location
cmake.exe --build build-msvc --config Debug --target AdvancedVulkanRendering
```

Expected: both spot-shadow CSOs compile and the DX12 target builds.

- [ ] **Step 7: Commit spot shadows**

```powershell
git.exe add -- shaders/drawclusterShadowSpot.hlsl shaders/compile_shaders_dx12.bat shaders/drawclusterShadowSpot.vs.cso shaders/drawclusterShadowSpot.alpha.ps.cso Src/DX12/DX12GpuScene.h Src/DX12/DX12GpuScene.cpp
git.exe commit -m "feat: render DX12 spot shadows"
```

### Task 6: Complete Scatter and Deferred Descriptor Bindings

**Files:**
- Modify: `Src/DX12/DX12GpuScene.cpp`

**Interfaces:**
- Consumes: noise resources from Task 2, light/matrix buffers from Task 3,
  readable tile indices from Task 4, and the shadow array from Task 5.
- Produces: type-correct real descriptors at every spot/noise register used by
  `scattervolume.hlsl` and `deferredlighting.hlsl`.

- [ ] **Step 1: Add a descriptor-contract regression scan**

Before changing bindings, run:

```powershell
rg -n "slot4 = t5|spot light culling data \(null|spotShadowMaps SRV \(null|spotViewProjMatrices SRV \(null|perlinNoiseTex SRV \(null" Src/DX12/DX12GpuScene.cpp
```

Expected: matches identify every remaining placeholder called out by the spec.

- [ ] **Step 2: Add typed descriptor writers**

Inside `RecordCommandBuffer()`, define focused lambdas that accept an explicit
destination handle: `writeStructuredSrv(resource, count, stride, handle)`,
`writeTexture2DSrv(resource, format, handle)`,
`writeTexture3DSrv(resource, format, handle)`, and
`writeTexture2DArraySrv(resource, format, layers, handle)`. Each lambda sets the
matching `ViewDimension`; it may receive null only when creating a null view of
that same type.

- [ ] **Step 3: Fill all ScatterVolume descriptors**

Use the Task 1 offsets and these exact resource shapes:

```text
BlueNoise:        Texture2D, R8_UNORM
PointData:        StructuredBuffer, sizeof(AAPLPointLightCullingData)
PointIndices:     Buffer R32_UINT
SpotData:         StructuredBuffer, sizeof(AAPLSpotLightCullingData)
SpotIndices:      Buffer R32_UINT
SpotShadowArray:  Texture2DArray, R32_FLOAT, 32 layers
SpotMatrices:     StructuredBuffer, sizeof(mat4), 32 elements
PerlinNoise:      Texture3D, R8_UNORM
```

Bind the real minimum-size buffers even when a light count is zero; shader
counts prevent reads. Keep t3 and t11 as type-correct padding descriptors
because their samplers are static.

- [ ] **Step 4: Fill deferred spot descriptors**

Replace t11, t12, t13, and t15 with the same spot data, spot index, shadow
array, and matrix resources. Leave t14 as a type-correct padding descriptor for
the static comparison sampler. Fix the t16 empty fallback to be a null
`Texture3D`, not a null `Texture2D`.

- [ ] **Step 5: Close the per-frame state cycle**

At the end of deferred lighting, transition active point and spot index buffers
from the combined readable state back to `COMMON`. At the next culling dispatch,
transition `COMMON -> UAV`. Do not transition the static spot-shadow array or
noise textures back to writable states. Preserve the existing accumulation
volume history transitions.

- [ ] **Step 6: Verify the placeholder scan and build**

```powershell
rg -n "blueNoiseTex SRV \(null|spot light culling data \(null|spotShadowMaps SRV \(null|spotViewProjMatrices SRV \(null|perlinNoiseTex SRV \(null" Src/DX12/DX12GpuScene.cpp
cmake.exe --build build-msvc --config Debug --target AdvancedVulkanRendering
build-msvc/DX12ScenePolicyTests.exe
```

Expected: rg returns no matches, the DX12 build succeeds, and policy tests exit
0.

- [ ] **Step 7: Commit consumer bindings**

```powershell
git.exe add -- Src/DX12/DX12GpuScene.cpp
git.exe commit -m "feat: bind DX12 spot and scatter resources"
```

### Task 7: Full Verification and Runtime Acceptance

**Files:**
- Modify only if verification finds a defect: files already listed in Tasks 1-6.

**Interfaces:**
- Consumes: the complete implementation.
- Produces: evidence that CPU policy, shader compilation, C++ compilation, current-scene loading, and D3D12 validation all pass.

- [ ] **Step 1: Rebuild policy tests from source and run them**

```powershell
g++.exe -std=c++20 -I Src/DX12 -I Src/Include Tests/DX12ScenePolicyTests.cpp -o build-msvc/DX12ScenePolicyTests.exe
build-msvc/DX12ScenePolicyTests.exe
```

Expected: compilation succeeds and the executable exits 0 without stderr.

- [ ] **Step 2: Recompile every DX12 shader**

```powershell
Push-Location shaders
cmd.exe /c compile_shaders_dx12.bat
Pop-Location
```

Expected: every DXC invocation exits 0 and the five new CSOs exist:
`CoarseCullSpot.cs.cso`, `TraditionalCullSpot.cs.cso`,
`ClearIndicesSpot.cs.cso`, `drawclusterShadowSpot.vs.cso`, and
`drawclusterShadowSpot.alpha.ps.cso`.

- [ ] **Step 3: Build the complete DX12 Debug target**

```powershell
cmake.exe --build build-msvc --config Debug --target AdvancedVulkanRendering
```

Expected: exit code 0 and zero compiler errors. Existing unrelated warnings may
remain, but no new warning may originate from a file changed by this plan.

- [ ] **Step 4: Run the current scene under the D3D12 debug layer**

Run `Bin\AdvancedVulkanRendering.exe --dx12` from the repository root, enable
`scatterScale` in the renderer UI, and verify the initialization log reports 31
valid spot lights and zero rejected lights.
Inspect one frame and confirm:

```text
t5/t13 Scatter noise descriptors point to R8 2D/3D resources
t8/t9 Scatter spot descriptors point to the spot data/index buffers
t10/t12 Scatter shadow descriptors point to the 32-layer array/matrix buffer
t11/t12/t13/t15 Deferred descriptors point to the same spot resources
spot shadow resource state = PIXEL_SHADER_RESOURCE | NON_PIXEL_SHADER_RESOURCE
point and spot index state during consumers = PIXEL_SHADER_RESOURCE | NON_PIXEL_SHADER_RESOURCE
```

Expected: deferred spot lighting is visible with scatter disabled; enabling
scatter adds point/spot volumetric contribution with noise modulation and spot
occlusion; the debug layer reports no descriptor-dimension or resource-state
errors.

- [ ] **Step 5: Review the final diff for scope and generated artifacts**

```powershell
git.exe status --short
git.exe diff --check HEAD~6..HEAD
git.exe diff --stat HEAD~6..HEAD
```

Expected: only the files named in this plan plus the required CSOs are changed;
there is no whitespace error. Do not add unrelated untracked scene assets or
build outputs.
