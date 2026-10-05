# VLM phase 1 review and validation

2026-10-05, branch `vlm`; continues implementation base `e969685`.

Uniform-grid baking, versioned asset load/save, and Vulkan deferred/forward runtime are implemented. Automated checks cover numerical energy, interpolation, actual baking, resource validation and loading. This does not certify thin-wall quality, complete indirect-path convergence or image parity.

## Review fixes

| Finding | Resolution |
|---|---|
| Full BRDF divided by selected-lobe PDF doubled bounce energy | VLM sampler uses the complete mixture PDF; camera sampler remains unchanged. Independent GPU hemisphere quadrature catches the original 2× error. |
| Last scattering did not trace its outgoing segment | Trace the terminal segment for sky/emission. |
| Readback lacked explicit memory visibility | RT shader-write barrier includes host-read; submit bounded probe/sample tiles. |
| Zero/negative/overflow/nonfinite CLI values | Reject before baking; failures have nonzero application status. |
| Same-size scene edits left assets usable | Stream content hashes of packed scene/HDR plus effective lighting; canonical settings replace truncated/rounded text. |
| Valid CRC could carry invalid SH/metadata | Reject NaN/Inf half coefficients, padding, validity, unsupported flags/integrator, invalid layout and file budgets. |
| Grid rounded down and exceeded requested spacing | Use ceil with tolerance around integer cell ratios. |
| Environment sun duplicated runtime analytic direct sun | Active VLM flags carry sun ownership. Bundled HDR defaults to environment mode. |
| First frame streaming made bake snapshot nondeterministic | Skip streaming until the first bake completes; identity records `permanent64-frozen-cutout`. |
| Uniform first-direction sampling missed narrow HDR sun | Mix 50% uniform sphere and 50% environment importance directions; divide by full mixed PDF. |

Independent review reproduced the old uniform sampler against HDR texels. Only 18 texels brighter than 1000 carry 47.76% of red-channel energy; 65536 uniform directions expect just 0.775 hits in that region. CPU reproduction agrees with the old GPU probe c0 values within approximately 0.30%. The large error was variance, not a direction convention change.

## Runtime contract

- File format remains fixed-layout `.vlm` v1, `IrradiancePolynomialSH9_v1`; current integrator is **3**. Older estimator assets must be rebaked.
- Uniform grid has `(cellsX+1)*(cellsY+1)*(cellsZ+1)` nodes. Runtime manually gathers eight nodes and respects validity; maximum boundary uses the last cell with fraction 1.
- Diffuse receiver computes `albedo * (1-F0) * E / pi`; material albedo already includes metallic suppression. Specular IBL remains separate.
- Deferred VLM diffuse bypasses SSAO; specular retains existing AO behavior. Forward uses the same sampler.
- Boundary mixes physical sky irradiance with VLM. Interior invalid holes return zero under coverage; they are not replaced with bright sky.
- Normal bias currently defaults to zero. Geometry/shading normals remain separate inputs.
- Disk storage: 132-byte header + 57 bytes/probe. Runtime FP32 SH: 112 bytes/probe, plus small parameter/sky buffers. Probe budget is 1M.
- Deferred bindings 22/23/24, forward bindings 9/10/11. Fallback resources are valid even when VLM is disabled.
- Loading is supported at startup; bake activation appears on the next frame because frame constants were already uploaded for the baking frame.

## Reproduce

Build the app and both shader backends using the repository's normal build. Build standalone CPU/GPU regressions:

```powershell
cmake -S Tests -B build-vlm-tests -DVLM_GPU_TESTS=ON
cmake --build build-vlm-tests --config Debug
ctest --test-dir build-vlm-tests -C Debug --output-on-failure
```

GPU tests need Vulkan, DXC and Python; leave `VLM_GPU_TESTS=OFF` for CPU-only tests. Existing local KTX/IBL tests are included only when their source files are present.

Run actual renderer acceptance with the packed Bistro scene and bundled HDR available:

```powershell
& ./Tests/run_vlm_smoke.ps1 -PythonExe python
```

The script starts hidden test processes, keeps outputs in `.cache/vlm-validation`, and fails on timeouts, unexpected exits, missing diagnostics, thresholds or Vulkan VUID/errors. It runs:

1. Constant sky: 8 nodes, 4096 samples.
2. Bundled HDR sky-only: 8 nodes, 65536 samples, batches of 256; maximum errors over all nodes.
3. Geometry dark/environment/sun/both fixtures: 8192 samples, two surface scatterings, constant disk-free sky, explicit analytic sun, local light scale zero. Compare **both = env + sun − dark** over all packed coefficients.
4. HDR geometry bake with environment sun.
5. Matching asset load, sun-mode mismatch, CRC corruption: each draws 30 frames and exits normally. Rejected/stale assets stay disabled.
6. Invalid zero sample argument: application returns 2. Invalid zero volume with bounded frame exit returns 1 and publishes no asset.

Manual validators are also available:

```powershell
python Tests/validate_vlm_log.py .cache/vlm-validation/const.log --mode const --asset .cache/vlm-validation/const.vlm
python Tests/validate_vlm_log.py .cache/vlm-validation/direction.log --mode direction --asset .cache/vlm-validation/direction.vlm
python Tests/validate_vlm_log.py --linearity .cache/vlm-validation/dark.vlm .cache/vlm-validation/env.vlm .cache/vlm-validation/sun.vlm .cache/vlm-validation/both.vlm
```

## Measurements

| Check | Measured | Gate |
|---|---:|---:|
| Local CPU/GPU/validator suite | 13 passed; standalone CPU mirror 7 passed | No failures/skipped GPU checks |
| Constant sky maximum E error | 0.3500% | ≤1% |
| HDR maximum c0 coefficient error, all 8 nodes | 0.7954% | ≤1% |
| HDR maximum weighted coefficient error, all 8 nodes | 1.6488% | ≤5% |
| HDR FP16 reconstruction maximum error | 0.1586% | ≤1% |
| Geometry coefficient superposition, final frozen snapshot | 0.6293% | ≤1% |
| HDR sky-only dispatch, 65536 samples/8 nodes | 0.59s | Diagnostic, not receiver-frame timing |
| Asset matching/stale/corrupt | 30 rendered frames, normal exit per case | No VUID/Validation Error, correct activation |
| Application and Vulkan/DX12 shaders | Build/compile passed | Zero errors |

Debug startup includes hashing the approximately 839 MB packed scene. Treat bake dispatch time separately from scene loading, hashing, CPU environment setup and renderer startup. Existing spdlog deprecation/linker warnings are not new VLM failures.

## Use

```powershell
./Bin/AdvancedVulkanRendering.exe --vlm-bake out_bistro.vlm --vlm-bake-volume camera,8,6,8 --vlm-bake-spacing 1 --vlm-bake-samples 8192
./Bin/AdvancedVulkanRendering.exe --vlm out_bistro.vlm
```

Default environment mode matches the bundled sun-disk HDR. Use `--vlm-sun-mode analytic` only with disk-free sky (for example a controlled `--vlm-bake-const-env` fixture). No automatic sun-disk removal is implemented.

Assets baked with nondefault environment/light scales, constant RGB or sky-only diagnostics require the same effective options when loading; otherwise they are stale. Sample count, batch size and volume need not be repeated on load. ImGui VLM Scale adjusts the active asset; zero restores legacy lighting.

## Remaining quality work

Geometric validity classification still initializes all nodes as valid. There is no relocation, visibility distance field, deringing, adaptive sample stopping, checkpointing or sparse bricks. Phase 1 uses frozen coarse permanent texture mips; alpha cutouts and indirect material response can differ from full-resolution rendering. Glass retains the engine's alpha-cutout policy.

First-direction importance sampling does not solve high variance when later BSDF bounces hit a narrow solar disk. Geometry bakes above establish implementation/resource and superposition behavior, not converged indoor lighting. Emissive geometry co-located with a proxy analytic light remains unsupported because both can contribute.

Thin-wall/doorway leakage, boundary image A/B, material/camera PT comparisons, DX12 visual parity, RenderDoc descriptor captures and GPU receiver-frame cost have not been certified by these numerical/smoke checks. DX12 shader compilation is verified; VLM runtime itself is Vulkan-only.
