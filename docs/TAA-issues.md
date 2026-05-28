# TAA Known Issues & Improvement Notes

Compared against `D:\ModernRenderingWithMetal\Renderer\Shaders\AAPLResolve.metal` (Apple's reference TAA implementation).

## Fixed

### 1. `prevViewProjectionMatrix` missing jitter (2026-05-28)

**Problem:** `_prevViewProjectionMatrix` was `cleanView * cleanProj` (unjittered VP). The history lookup did not compensate for the previous frame's sub-pixel jitter offset, so temporal accumulation was sampling from a slightly wrong screen position each frame.

**Fix:** Changed to `cleanView * jitteredProj` in `GpuScene.cpp:3404`. Now the reprojection in `resolve.hlsl:123` correctly maps world positions to where they were on screen in the previous frame, jitter and all.

## Remaining

### 2. History buffer 8-bit sRGB precision

**Current:** `VK_FORMAT_R8G8B8A8_SRGB` (`GpuScene.cpp:5774`).

**Issue:** With 95%/5% blend, each frame only contributes 5% new information. 8-bit quantization error accumulates over many frames, causing:
- Visible banding in slowly-changing dark regions (sRGB gamma compounds precision loss in shadows)
- Color drift over hundreds of frames

**Both projects have this issue** — Metal uses `MTLPixelFormatBGRA8Unorm_sRGB`.

**Possible fix:** Change to `VK_FORMAT_R16G16B16A16_SFLOAT` (same as HDR buffer). Trade-off: ~2x memory bandwidth and VRAM for history. A cheaper alternative is `VK_FORMAT_R8G8B8A8_UNORM` instead of SRGB — linear storage avoids the gamma precision loss at the cost of slightly worse dark-tone quantization.

### 3. No velocity buffer / dynamic object support

**Current:** Only camera reprojection. World position reconstructed from depth, then projected with `prevViewProjectionMatrix`. This handles static geometry + moving camera correctly, but any object that moves within the scene will ghost.

**3x3 neighborhood clamp** (`resolve.hlsl:113-116`) mitigates ghosting by clamping history to the current frame's local color bounding box, but it's a heuristic that fails when:
- Objects move more than 1 pixel/frame (clamp range is too tight)
- Disocclusion reveals new background (clamp range contains only foreground colors)

**Both projects have this limitation** — Metal also uses only camera reprojection.

**Possible fix:** Render a velocity (motion vector) buffer during the GBuffer pass. Use `currentClip - prevClip` in screen space to offset the history lookup per-pixel. Combined with a disocclusion mask (`|velocity| > threshold`) to suppress history blending in those regions.

### 4. No YCoCg color space or variance clipping

**Current:** Neighborhood bounding box is computed in RGB space (`resolve.hlsl:113-114`), and history is clamped to strict `minC`/`maxC` (`resolve.hlsl:132`).

**Issue:** 
- RGB min/max clamping creates a box-shaped acceptable region that doesn't match perceptual color distribution. Scenarios with high chroma variance (geometry edges, specular highlights) get box-clipped to an overly conservative region, causing excessive blur.
- Strict min/max uses 100% of the neighborhood range; a single outlier pixel (e.g., specular sparkle, firefly) expands the clamp window to include colors the history pixel should never take.

**Metal has the same limitation.**

**Possible fix:**
1. Convert to YCoCg before computing min/max and clamping — better perceptual uniformity
2. Use variance clipping: compute neighborhood mean μ and standard deviation σ, then clamp history to `μ ± γ·σ` (γ ≈ 1.5–2.0). This tightens the clamp window and rejects outliers naturally.

### 5. Fixed blend factor

**Current:** `blendFactor = 0.95` always, except when `prevClip.xy` is outside NDC (set to 0).

**Issue:** No reaction to reprojection quality. When the reprojected UV is far from the current UV (indicating fast camera movement or disocclusion), the blend should favor the current frame more heavily. The binary NDC check is too coarse — a pixel at `prevUV = (0.99, 0.5)` gets full history even though it's near the screen edge where data is less reliable.

**Both projects use a fixed 0.95 blend factor** with the same binary NDC rejection.

**Possible fix:** Compute a reactive blend factor:
```
reprojectionError = length((prevUV - currentUV) * physicalSize)  // in pixels
blendFactor = 0.95 * exp(-reprojectionError / sigma)
```
Or use a simpler clamped linear ramp based on reprojection distance. Also, add a small neighborhood-based confidence term: if the history sample required heavy clamping, reduce blend factor proportionally.
