## Task 7 Report: HDR Buffer + Tone Mapping + TAA

**Status:** DONE_WITH_CONCERNS

**Commit SHA:** a8f149f

**One-line summary:** Added R16G16B16A16_FLOAT HDR intermediate buffer, ACES tone mapping resolve pass, variance-clamp TAA with ping-pong history, and Halton jitter for DX12 backend.

---

### What was done

**shader/resolve.hlsl**
- Added `#include "shadercompat.hlsl"` and replaced bare `[[vk::binding()]]` annotations with the `VK_BINDING` / `REGISTER_*` dual-backend macros.
- Entry points used: `ResolveVS` and `ResolvePS` (the brief incorrectly named `AAPLSimpleTexVertexOutFSQuadVertexShader` / `ToneMapAndResolve`; the actual shader has `ResolveVS` / `ResolvePS`).
- Both VS and PS compile clean under `-D DX12_BACKEND`.

**shaders/compile_shaders_dx12.bat**
- Added two entries after the SAO block:
  `%DXC% %DXFLAGS% -T vs_6_0 resolve.hlsl -E ResolveVS -Fo resolve.vs.cso`
  `%DXC% %DXFLAGS% -T ps_6_0 resolve.hlsl -E ResolvePS -Fo resolve.ps.cso`
- Both `resolve.vs.cso` and `resolve.ps.cso` produced and committed.

**Src/DX12/DX12GpuScene.h**
- Added `_hdrBuffer`, `_taaHistory[2]`, `_taaHistoryIndex`, `_hdrRtvHeap`, `_hdrRtvSize`, `_resolveRootSig`, `_resolvePSO`, `_taaEnabled`, `SRV_HDR_BUFFER`, `SRV_TAA_HISTORY`.
- Added `CreateHDRResources()` declaration.

**Src/DX12/DX12GpuScene.cpp**
- Static heap bumped from 2048 → 2100 to fit HDR+TAA SRV slots.
- `_frameConstants` default-initialized in constructor: `sunDirection=(0.3,1,0.5)`, `exposure=1.0`, `emissiveScale=1`, `localLightIntensity=1`, `sunColor`, `skyColor` — these were zero-initialized before, which would have caused NaN in shadow matrix computation and black output after tone mapping.
- `CreateHDRResources()`: creates HDR buffer (RT|UAV) + 2 TAA history buffers (SRV initial state), 2-slot RTV heap, static SRVs at slots 1015 (HDR) and 1016 (TAA history).
- `CreateRootSignatures()`: resolve root sig with CBV(b0,s0) + 3-SRV descriptor table (t0-t2, space1) + 2 static samplers (s3=nearest clamp, s4=linear clamp, both space1).
- `CreatePipelineStates()`: resolve PSO with 2 RTVs (swapchain format + R16G16B16A16_FLOAT for history write), no IA layout, CULL_NONE.
- `Draw()`:
  - Deferred lighting `OMSetRenderTargets` redirected to `_hdrRtvHeap` slot [0] (HDR buffer).
  - After deferred draw: transition HDR → PSR, update TAA history write RTV at slot [1], call `OMSetRenderTargets(2, ...)` with swapchain + history targets, bind 3-SRV dynamic block (HDR, history-read, scene depth), draw full-screen triangle, ping-pong history index.
  - Depth buffer stays in `DEPTH_READ` throughout (valid for SRV read in resolve).
  - After resolve: HDR and history write target transitioned back to their rest states.
- `UpdateUniforms()`:
  - Added `prevViewProjectionMatrix`: captures `objectToCamera * projectionMatrix` (= mathematical VP via the mat4 quirk), transposes for HLSL, saves as `sPrevVP`.
  - Added Halton jitter (base-2/base-3, 16-frame sequence) written to `_frameConstants.taaJitter`.
  - Added `taaEnabled` and `invPhysicalSize` updates.
- `OnResize()`: releases `_hdrBuffer`, `_taaHistory[2]`, `_hdrRtvHeap`; resets `_taaHistoryIndex`; calls `CreateHDRResources()` after `CreateGBuffers()`.
- `RenderImGuiOverlay()`: `ImGui::Checkbox("TAA", &_taaEnabled)` added after culling stats.

---

### Concerns

1. **Entry-point names diverge from brief.** The brief specified `AAPLSimpleTexVertexOutFSQuadVertexShader` / `ToneMapAndResolve`, but `resolve.hlsl` actually has `ResolveVS` / `ResolvePS`. The bat file and PSO load use the correct shader names. The brief was probably written against a planned (not yet final) shader. No action needed unless the shader is later renamed to match the brief.

2. **Resolve PSO outputs `SV_Target1` (history) simultaneously with swapchain.** The swapchain's RTV format is likely `DXGI_FORMAT_B8G8R8A8_UNORM` or similar (non-HDR). The resolve PSO declares `RTVFormats[0] = swapchain format` and `RTVFormats[1] = R16G16B16A16_FLOAT`. This is correct — the `output.history` write goes to the TAA history buffer, not the swapchain. No concern here, but worth noting.

3. **`DEPTH_READ` state compatibility with SRV in resolve.** The depth buffer is transitioned to `D3D12_RESOURCE_STATE_DEPTH_READ` before the deferred pass and back to `DEPTH_WRITE` after the entire block (including resolve). D3D12_RESOURCE_STATE_DEPTH_READ is a combined state that includes pixel shader resource access, so reading the depth as SRV during the resolve draw is valid.

4. **SRV slot collision risk.** `SRV_BINDLESS_START = 15` and up to 1000 bindless textures would use slots 15–1014. `SRV_HDR_BUFFER = 1015`, `SRV_TAA_HISTORY = 1016`. If the scene has more than 1000 bindless textures the slots would overlap. The scene currently uses significantly fewer, but this should be guarded or the bindless range cap made explicit.

5. **`_frameConstants.sunDirection` was not initialized before this task.** Added a default of `(0.3, 1.0, 0.5)` in the constructor. This is cosmetic; the prior code would have produced NaN shadow matrices every frame, which presumably produced incorrect-but-not-crashing shadow output. The default is reasonable but not tuned to the actual scene.

6. **No runtime test was performed** (the DX12 binary was built successfully, but the renderer was not launched for visual verification). TAA correctness (anti-aliasing effect, ghosting behavior) requires runtime observation.

---

### Post-review fixes (2026-06-26)

**Fix 1 — SRV_HDR_BUFFER / SRV_TAA_HISTORY collision with bindless range (Critical)**

`SRV_HDR_BUFFER` was computed at runtime as `SRV_BINDLESS_START + 1000 = 1015`. With up to 1000 bindless textures occupying slots 15–1014, any scene exceeding 1000 textures would stomp these slots. Changed both to `static constexpr uint32_t` in `DX12GpuScene.h`:
```cpp
static constexpr uint32_t SRV_HDR_BUFFER  = 2050;
static constexpr uint32_t SRV_TAA_HISTORY = 2051;
```
Removed the runtime formula assignment from `CreateHDRResources()`. The static heap `staticCount` of 2100 already covers these slots; no heap resize needed.

**Fix 2 — Mid-frame write to static shader-visible heap descriptor (Important)**

In `Draw()`, the resolve pass previously called `CreateShaderResourceView(_taaHistory[histRead], ..., GetStaticCPU(SRV_TAA_HISTORY))` mid-frame — a D3D12 spec violation because the GPU may still be reading that static heap slot from the prior frame. The fix removes that call and instead writes the TAA history SRV directly into the dynamic allocation's second slot (`resolveDesc.cpu.ptr + ds`) using `CreateShaderResourceView` on the dynamic (non-shader-visible) CPU handle. The copy path for t1 (`CopyDescriptorsSimple` from the static slot) is also removed. The static `SRV_TAA_HISTORY` slot continues to be written once during `CreateHDRResources()` (initial state, at init time), which remains valid.
