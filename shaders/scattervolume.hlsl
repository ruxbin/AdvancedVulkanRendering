// scattervolume.hlsl  –  Froxel-based volumetric scattering
// Reference: ModernRenderingWithMetal / AAPLScatterVolume.metal
//
// Compiled TWICE (Vulkan):
//   dxc -enable-16bit-types -spirv -T cs_6_2 scattervolume.hlsl -E ScatterVolume       -Fo scattervolume.cs.spv
//   dxc -enable-16bit-types -spirv -T cs_6_2 scattervolume.hlsl -E AccumulateScattering -Fo accumscatter.cs.spv
//
// Compiled TWICE (DX12):
//   dxc -D DX12_BACKEND -T cs_6_2 scattervolume.hlsl -E ScatterVolume       -Fo scattervolume.cs.cso
//   dxc -D DX12_BACKEND -T cs_6_2 scattervolume.hlsl -E AccumulateScattering -Fo accumulatescatter.cs.cso
//
// ScatterVolume kernel  (set 0, bindings 0-14):
//   0: RWTexture3D scatterOut,  1: UBO,  2: shadowMaps,  3: shadowSampler
//   4: Texture3D  scatterPrev (history),  5: Texture2D blueNoise
//   6: pointLightData,  7: pointLightIndices (per-frame)
//   8: spotLightData,   9: spotLightIndices  (per-frame)
//  10: spotShadowMaps, 11: spotShadowSampler, 12: spotViewProjMatrices
//  13: Texture3D perlinNoise, 14: SamplerState linearSampler
//
// AccumulateScattering kernel  (set 0, bindings 0-1):
//   0: Texture3D scatterIn,  1: RWTexture3D accumOut

#include "shadercompat.hlsl"
#include "commonstruct.hlsl"

// ---- compile-time constants ------------------------------------------------
#define SCATTERING_TILE_SIZE     8
#define SCATTERING_VOLUME_DEPTH 64
#define SCATTERING_RANGE       100.0f

// ============================================================
//  ScatterVolume bindings  (set 0)
//  Only included when NOT compiling the AccumulateScattering entry point
// ============================================================
#ifndef ACCUM_PASS
VK_BINDING( 0,0) [[vk::image_format("rgba16f")]] RWTexture3D<float4> scatterOut REGISTER_UAV(0,0);
VK_BINDING( 1,0) cbuffer ScatterUBO REGISTER_CBV(1,0) {
    CameraParamsBufferFull cameraParams;
    AAPLFrameConstants     frameConstants;
}
VK_BINDING( 2,0) Texture2DArray<float>                  shadowMaps           REGISTER_SRV(2,0);
VK_BINDING( 3,0) SamplerComparisonState                 shadowSampler        REGISTER_SAMPLER_CMP(3,0);
VK_BINDING( 4,0) Texture3D<float4>                      scatterPrev          REGISTER_SRV(4,0);   // history
VK_BINDING( 5,0) Texture2D<float>                       blueNoiseTex         REGISTER_SRV(5,0);
VK_BINDING( 6,0) StructuredBuffer<AAPLPointLightCullingData> pointLightData  REGISTER_SRV(6,0);
VK_BINDING( 7,0) StructuredBuffer<uint>                 pointLightIndices    REGISTER_SRV(7,0);
VK_BINDING( 8,0) StructuredBuffer<AAPLSpotLightCullingData>  spotLightData   REGISTER_SRV(8,0);
VK_BINDING( 9,0) StructuredBuffer<uint>                 spotLightIndices     REGISTER_SRV(9,0);
VK_BINDING(10,0) Texture2DArray<float>                  spotShadowMaps       REGISTER_SRV(10,0);
VK_BINDING(11,0) SamplerComparisonState                 spotShadowSampler    REGISTER_SAMPLER_CMP(11,0);
VK_BINDING(12,0) StructuredBuffer<float4x4>             spotViewProjMatrices REGISTER_SRV(12,0);
VK_BINDING(13,0) Texture3D<float>                       perlinNoiseTex       REGISTER_SRV(13,0);
VK_BINDING(14,0) SamplerState                           linearSampler        REGISTER_SAMPLER(14,0);
#endif // !ACCUM_PASS

// ============================================================
//  AccumulateScattering bindings  (set 0, separate compilation)
//  Only included when compiling the AccumulateScattering entry point
// ============================================================
#ifdef ACCUM_PASS
VK_BINDING(0,0) Texture3D<float4>   scatterIn  REGISTER_SRV(0,0);
VK_BINDING(1,0) [[vk::image_format("rgba16f")]] RWTexture3D<float4> accumOut REGISTER_UAV(1,0);
#endif // ACCUM_PASS

// ============================================================
//  Shared push constants
// ============================================================
struct PushConstants {
    uint  volumeWidth;
    uint  volumeHeight;
    float screenWidth;
    float screenHeight;
    uint  resetHistory; // 1 = first frame, skip history blend
};
DECLARE_PUSH_CONSTANTS(PushConstants, pc, 0);

// Light culling constants (match Light.h / commonstruct.hlsl)
#define SCATTER_LIGHT_TILE_SIZE 32
// MAX_LIGHTS_PER_TILE is defined in commonstruct.hlsl
#define SPOT_SHADOW_MAX_COUNT   32
#define SPOT_SHADOW_DEPTH_BIAS  0.001f

// Temporal blend factor (85 % history, 15 % current — matches Metal)
#define TEMPORAL_BLEND 0.85f
// Golden ratio for per-frame offset (temporal jitter)
#define GOLDEN_RATIO   0.6180339887f

// ============================================================
//  Shared helpers
// ============================================================

// Logarithmic depth: slice index → view-space Z (metres)
float sliceToViewZ(float slice) {
    float t = slice / float(SCATTERING_VOLUME_DEPTH);
    return (exp2(t * 3.0f) - 1.0f) / 7.0f * SCATTERING_RANGE;
}

// View-space Z → normalised scatter depth [0,1]
float viewZToScatterDepth(float viewZ) {
    return log2(clamp(viewZ / SCATTERING_RANGE * 7.0f + 1.0f, 1.0f, 8.0f)) / 3.0f;
}

// Schlick phase function — Metal uses k = g directly (no H-G remapping)
float schlickPhase(float cosTheta, float g) {
    float denom = 1.0f + g * cosTheta;
    return (1.0f - g * g) / (4.0f * M_PI_F * denom * denom);
}

// Exponential height fog: Metal manual saturate(exp(-f * max(0, y + o))).
// Fog is densest below -offset, fades exponentially with altitude.
float heightFogDensity(float worldY, float offset, float falloff) {
    return saturate(exp(-falloff * max(0.0f, worldY + offset)));
}

// Reconstruct world-space position of a froxel at the given view-space depth
#ifndef ACCUM_PASS
float3 froxelToWorldPos(uint3 coord, float viewZ) {
    float2 uv = (float2(coord.xy) + 0.5f) / float2(pc.volumeWidth, pc.volumeHeight);
    float4 ndcRay = float4(uv * 2.0f - 1.0f, 0.0f, 1.0f);
    float4 viewRay = mul(cameraParams.invProjectionMatrix, ndcRay);
    viewRay /= viewRay.w;
    float scale = viewZ / (-viewRay.z);
    float4 worldPos = mul(cameraParams.invViewMatrix, float4(viewRay.xyz * scale, 1.0f));
    return worldPos.xyz;
}

// Cascaded shadow evaluation
float evalShadow(float4 worldPos) {
    for (int c = 0; c < SHADOW_CASCADE_COUNT; ++c) {
        float4x4 shadowVP = mul(cameraParams.shadowMatrix[c].shadowProjectionMatrix,
                                cameraParams.shadowMatrix[c].shadowViewMatrix);
        float4 lsp = mul(shadowVP, worldPos);
        lsp /= lsp.w;
        if (all(lsp.xyz < 1.0f) && all(lsp.xyz > float3(-1.0f,-1.0f,0.0f))) {
            float3 uv = float3(lsp.xy * 0.5f + 0.5f, (float)c);
            return shadowMaps.SampleCmpLevelZero(shadowSampler, uv, lsp.z - 0.001f);
        }
    }
    return 1.0f;
}

// 3D Perlin noise modulation — EXACT Metal match (AAPLScatterVolume.metal:26-60)
float applyGlobalNoise(float density, float3 worldPos, float depth) {
    float3 noisePos = worldPos + frameConstants.globalNoiseOffset;
    const float baseScale   = 1.0f / 16.0f;
    const float detailScale = 1.0f / 2.0f;

    float n = perlinNoiseTex.SampleLevel(linearSampler, noisePos * baseScale, 0);
    // Detail noise (blends two scales equally)
    n += perlinNoiseTex.SampleLevel(linearSampler, noisePos * detailScale, 0);
    n *= 0.5f;

    n = smoothstep(0.3f, 0.7f, n);
    n *= n;

    const float noiseFadeStart  = 20.0f;
    const float noiseFadeLength = 10.0f;
    const float noiseMult       = 2.0f;

    density *= lerp(n * noiseMult, 1.0f,
                    saturate(max(0.0f, depth - noiseFadeStart) / noiseFadeLength));
    return density;
}

// Phase C: point light inscattering contribution (Metal: returns scattering color, no scatterCoeff)
float3 calculateLocalLightScattering(float3 worldPos, float3 camPos,
                                     float4 posAndRadius, float3 color) {
    float3 toLight = posAndRadius.xyz - worldPos;
    float sqrDist = dot(toLight, toLight);
    if (sqrDist > posAndRadius.w) return 0.0f;

    float3 L = normalize(toLight);
    float3 V = normalize(camPos - worldPos);
    float cosT = dot(V, L);
    float lightPhase = schlickPhase(cosT, 0.3f);

    float atten = 1.0f / max(sqrDist, 0.01f * 0.01f);
    float factor = sqrDist * (1.0f / posAndRadius.w);
    float smoothAtt = factor * factor; smoothAtt = 1.0f - smoothAtt * smoothAtt;
    smoothAtt = smoothAtt * smoothAtt;
    atten *= smoothAtt;

    return color * M_PI_F * frameConstants.localLightIntensity * atten * lightPhase;
}

// Phase D: spot light inscattering (with optional shadow)
float3 calculateLocalSpotLightScattering(float3 worldPos, float3 camPos,
                                         AAPLSpotLightCullingData spot,
                                         uint lightIdx) {
    float3 toLight = spot.posAndHeight.xyz - worldPos;
    float dist = length(toLight);
    if (dist > spot.posAndHeight.w) return 0.0f;

    float3 L = normalize(toLight);
    float cosTheta = dot(-L, spot.dirAndOuterAngle.xyz);
    if (cosTheta < spot.dirAndOuterAngle.w) return 0.0f;

    float invSqrRadius = 1.0f / (spot.posAndHeight.w * spot.posAndHeight.w);
    float3 toL = spot.posAndHeight.xyz - worldPos;
    float sqrDist = dot(toL, toL);
    float atten = 1.0f / max(sqrDist, 0.01f * 0.01f);
    float factor = sqrDist * invSqrRadius;
    float smoothAtt = factor * factor; smoothAtt = 1.0f - smoothAtt * smoothAtt;
    smoothAtt = smoothAtt * smoothAtt;
    atten *= smoothAtt;

    float angleRange = max(spot.cosInnerAngle - spot.dirAndOuterAngle.w, 1e-4f);
    float t = saturate((cosTheta - spot.dirAndOuterAngle.w) / angleRange);
    float angleAtt = t * t;

    float3 V = normalize(camPos - worldPos);
    float lightPhase = schlickPhase(dot(V, L), 0.3f);

    float shadow = 1.0f;
    if (lightIdx < SPOT_SHADOW_MAX_COUNT) {
        float4 lsp = mul(spotViewProjMatrices[lightIdx], float4(worldPos, 1.0f));
        lsp /= lsp.w;
        if (all(lsp.xyz < 1.0f) && all(lsp.xyz > float3(-1.0f,-1.0f,0.0f))) {
            float3 suv = float3(lsp.xy * 0.5f + 0.5f, (float)lightIdx);
            shadow = spotShadowMaps.SampleCmpLevelZero(spotShadowSampler, suv, lsp.z - SPOT_SHADOW_DEPTH_BIAS);
        }
    }

    // Frostbite convention: spot intensity = 4x point intensity
    return spot.color.xyz * M_PI_F * 4.0f * frameConstants.localLightIntensity
           * atten * angleAtt * shadow * lightPhase;
}

// ============================================================
//  ScatterVolume kernel
// ============================================================
[numthreads(4, 4, 4)]
void ScatterVolume(uint3 DTid : SV_DispatchThreadID) {
    if (DTid.x >= pc.volumeWidth || DTid.y >= pc.volumeHeight || DTid.z >= SCATTERING_VOLUME_DEPTH)
        return;

    // ---- Phase B: blue-noise depth jitter for temporal anti-aliasing ----
    float2 noiseUV = float2(DTid.x & 63u, DTid.y & 63u) / 64.0f;
    float  noise   = blueNoiseTex.SampleLevel(linearSampler, noiseUV, 0);
    float  jitter  = frac(noise + frameConstants.frameCounter * GOLDEN_RATIO) - 0.5f;

    float viewZ = sliceToViewZ(float(DTid.z) + 0.5f + jitter * 0.5f);

    float3 worldPos = froxelToWorldPos(DTid, viewZ);

    // ---- coefficients: EXACT Metal match (AAPLScatterVolume.metal:190-198) ----
    // absorptionCoeff is a hard-coded constant, never modified.
    float absorptionCoeff = 0.01f;
    // scatteringCoeff starts at base + height fog, then noise, then scatterScale.
    float scatteringCoeff = 0.01f + heightFogDensity(worldPos.y, 10.0f, 0.5f) * 0.01f;
    scatteringCoeff = applyGlobalNoise(scatteringCoeff, worldPos, viewZ);
    scatteringCoeff *= frameConstants.scatterScale;
    float extinction = absorptionCoeff + scatteringCoeff;

    // ---- Directional scattering color: EXACT Metal match (lines 200-205) ----
    float3 camPos  = float3(cameraParams.invViewMatrix._m03,
                            cameraParams.invViewMatrix._m13,
                            cameraParams.invViewMatrix._m23);
    float3 viewDir = normalize(camPos - worldPos);
    float  cosSun  = -dot(normalize(frameConstants.sunDirection), viewDir);
    float  shadow  = evalShadow(float4(worldPos, 1.0f));

    float3 scattering = frameConstants.skyColor; // ambient
    scattering += frameConstants.sunColor * M_PI_F * shadow * schlickPhase(cosSun, 0.3f);

    // ---- Phase C: local point lights (adding to scattering) ----
    {
        uint tileCountX = (uint(pc.screenWidth)  + SCATTER_LIGHT_TILE_SIZE - 1) / SCATTER_LIGHT_TILE_SIZE;
        uint tileX      = DTid.x * SCATTERING_TILE_SIZE / SCATTER_LIGHT_TILE_SIZE;
        uint tileY      = DTid.y * SCATTERING_TILE_SIZE / SCATTER_LIGHT_TILE_SIZE;
        uint tileIdx    = tileX + tileY * tileCountX;
        uint base       = tileIdx * MAX_LIGHTS_PER_TILE;
        uint count      = pointLightIndices[base];
        for (uint li = 0; li < count; ++li) {
            uint idx = pointLightIndices[base + li + 1];
            AAPLPointLightCullingData pl = pointLightData[idx];
            float radius2 = pl.posRadius.w * pl.posRadius.w;
            float4 posR   = float4(pl.posRadius.xyz, radius2);
            scattering += calculateLocalLightScattering(worldPos, camPos,
                                posR, pl.color.xyz);
        }
    }

    // ---- Phase D: local spot lights (adding to scattering) ----
    {
        uint tileCountX = (uint(pc.screenWidth)  + SCATTER_LIGHT_TILE_SIZE - 1) / SCATTER_LIGHT_TILE_SIZE;
        uint tileX      = DTid.x * SCATTERING_TILE_SIZE / SCATTER_LIGHT_TILE_SIZE;
        uint tileY      = DTid.y * SCATTERING_TILE_SIZE / SCATTER_LIGHT_TILE_SIZE;
        uint tileIdx    = tileX + tileY * tileCountX;
        uint base       = tileIdx * MAX_LIGHTS_PER_TILE;
        uint count      = spotLightIndices[base];
        for (uint si = 0; si < count; ++si) {
            uint spotIdx = spotLightIndices[base + si + 1];
            AAPLSpotLightCullingData spot = spotLightData[spotIdx];
            scattering += calculateLocalSpotLightScattering(worldPos, camPos,
                                spot, spotIdx);
        }
    }

    float4 current = float4(scattering * scatteringCoeff, extinction);

    // ---- Phase A: temporal reprojection ----
    if (pc.resetHistory == 0u) {
        float4 prevClipH = mul(cameraParams.prevViewProjectionMatrix, float4(worldPos, 1.0f));
        float  prevViewZ = prevClipH.w;
        float4 prevClip  = prevClipH / prevViewZ;
        float2 prevUV    = prevClip.xy * 0.5f + 0.5f;
        float  prevSliceF = viewZToScatterDepth(prevViewZ);

        if (all(prevUV >= 0.0f) && all(prevUV <= 1.0f) && prevSliceF >= 0.0f && prevSliceF <= 1.0f) {
            float3 histUVW  = float3(prevUV, prevSliceF);
            float4 histSample = scatterPrev.SampleLevel(linearSampler, histUVW, 0);
            current = lerp(current, histSample, TEMPORAL_BLEND);
        }
    }

    scatterOut[DTid] = current;
}
#endif // !ACCUM_PASS

// ============================================================
//  AccumulateScattering kernel  (unchanged from original)
// ============================================================
#ifdef ACCUM_PASS
[numthreads(8, 8, 1)]
void AccumulateScattering(uint3 DTid : SV_DispatchThreadID) {
    if (DTid.x >= pc.volumeWidth || DTid.y >= pc.volumeHeight)
        return;

    float4 accum = float4(0.0f, 0.0f, 0.0f, 1.0f);

    for (uint z = 0; z < SCATTERING_VOLUME_DEPTH; ++z) {
        float4 s         = scatterIn[uint3(DTid.xy, z)];
        float3 inscatter = s.rgb;
        float  extinction = s.a;

        float zNear    = sliceToViewZ((float)z);
        float zFar     = sliceToViewZ((float)z + 1.0f);
        float thickness = max(zFar - zNear, 0.0001f);

        float sliceTrans = exp(-extinction * thickness);
        accum.rgb += inscatter * accum.a;
        accum.a   *= sliceTrans;

        accumOut[uint3(DTid.xy, z)] = accum;
    }
}
#endif // ACCUM_PASS
