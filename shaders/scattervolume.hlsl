// scattervolume.hlsl  –  Froxel-based volumetric scattering
// Reference: ModernRenderingWithMetal / AAPLScatterVolume.metal
//
// Compiled TWICE:
//   dxc -enable-16bit-types -spirv -T cs_6_2 scattervolume.hlsl -E ScatterVolume       -Fo scattervolume.cs.spv
//   dxc -enable-16bit-types -spirv -T cs_6_2 scattervolume.hlsl -E AccumulateScattering -Fo accumscatter.cs.spv
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

#include "commonstruct.hlsl"

// ---- compile-time constants ------------------------------------------------
#define SCATTERING_TILE_SIZE     8
#define SCATTERING_VOLUME_DEPTH 64
#define SCATTERING_RANGE       100.0f

// ============================================================
//  ScatterVolume bindings  (set 0)
// ============================================================
[[vk::binding( 0,0)]] RWTexture3D<float4>                    scatterOut;
[[vk::binding( 1,0)]] cbuffer ScatterUBO {
    CameraParamsBufferFull cameraParams;
    AAPLFrameConstants     frameConstants;
}
[[vk::binding( 2,0)]] Texture2DArray<float>                  shadowMaps;
[[vk::binding( 3,0)]] SamplerComparisonState                 shadowSampler;
[[vk::binding( 4,0)]] Texture3D<float4>                      scatterPrev;   // history
[[vk::binding( 5,0)]] Texture2D<float>                       blueNoiseTex;
[[vk::binding( 6,0)]] StructuredBuffer<AAPLPointLightCullingData> pointLightData;
[[vk::binding( 7,0)]] StructuredBuffer<uint>                 pointLightIndices;
[[vk::binding( 8,0)]] StructuredBuffer<AAPLSpotLightCullingData>  spotLightData;
[[vk::binding( 9,0)]] StructuredBuffer<uint>                 spotLightIndices;
[[vk::binding(10,0)]] Texture2DArray<float>                  spotShadowMaps;
[[vk::binding(11,0)]] SamplerComparisonState                 spotShadowSampler;
[[vk::binding(12,0)]] StructuredBuffer<float4x4>             spotViewProjMatrices;
[[vk::binding(13,0)]] Texture3D<float>                       perlinNoiseTex;
[[vk::binding(14,0)]] SamplerState                          linearSampler;

// ============================================================
//  AccumulateScattering bindings  (set 0, separate compilation)
// ============================================================
[[vk::binding(0,0)]] Texture3D<float4>   scatterIn;
[[vk::binding(1,0)]] RWTexture3D<float4> accumOut;

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
[[vk::push_constant]] PushConstants pc;

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

// Schlick phase function (g = forward-scattering anisotropy)
float schlickPhase(float cosTheta, float g) {
    float k     = 1.55f * g - 0.55f * g * g * g;
    float denom = 1.0f - k * cosTheta;
    return (1.0f - k * k) / (4.0f * M_PI_F * denom * denom);
}

// Exponential height fog density
float heightFogDensity(float worldY, float offset, float falloff) {
    return exp(-max(worldY - offset, 0.0f) * falloff);
}

// Reconstruct world-space position of a froxel at the given view-space depth
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

// 3D Perlin noise modulation for fog density variation (Phase E)
float applyGlobalNoise(float3 worldPos, float baseDensity) {
    float3 noiseUVW = (worldPos + frameConstants.globalNoiseOffset) / 16.0f;
    float n = perlinNoiseTex.SampleLevel(linearSampler, noiseUVW, 0);
    // Remap: smoothstep centering + squaring (matches Metal applyGlobalNoise)
    n = smoothstep(0.3f, 0.7f, n) * n;
    // Fade noise out beyond 20-30m to keep distant fog smooth
    float3 camPos = float3(cameraParams.invViewMatrix._m03,
                           cameraParams.invViewMatrix._m13,
                           cameraParams.invViewMatrix._m23);
    float dist = length(worldPos - camPos);
    float noiseFade = 1.0f - saturate((dist - 20.0f) / 10.0f);
    // 2x multiplier to maintain average density (matching Metal)
    return baseDensity * (1.0f + n * noiseFade * 2.0f);
}

// Phase C: point light inscattering contribution
float3 calculateLocalLightScattering(float3 worldPos, float3 camPos,
                                     float4 posAndRadius, float3 color,
                                     float scatterCoeff, float phase) {
    float3 toLight = posAndRadius.xyz - worldPos;
    float sqrDist = dot(toLight, toLight);
    if (sqrDist > posAndRadius.w) return 0.0f;

    float3 L = normalize(toLight);
    float3 V = normalize(camPos - worldPos);
    float cosT = dot(V, L);
    float lightPhase = schlickPhase(cosT, 0.3f);

    // Inverse-square attenuation with smooth cutoff
    float atten = 1.0f / max(sqrDist, 0.01f * 0.01f);
    float factor = sqrDist * (1.0f / posAndRadius.w);
    float smoothAtt = factor * factor; smoothAtt = 1.0f - smoothAtt * smoothAtt;
    smoothAtt = smoothAtt * smoothAtt;
    atten *= smoothAtt;

    return color * M_PI_F * frameConstants.localLightIntensity * atten * lightPhase * scatterCoeff;
}

// Phase D: spot light inscattering (with optional shadow)
float3 calculateLocalSpotLightScattering(float3 worldPos, float3 camPos,
                                         AAPLSpotLightCullingData spot,
                                         uint lightIdx,
                                         float scatterCoeff) {
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
           * atten * angleAtt * shadow * lightPhase * scatterCoeff;
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

    // ---- Phase E: fog density with Perlin noise modulation ----
    float baseDensity  = frameConstants.scatterScale;
    baseDensity += 0.008f * heightFogDensity(worldPos.y, 2.0f, 0.35f);
    float density = applyGlobalNoise(worldPos, baseDensity);

    float scatterCoeff    = density * 0.5f;
    float absorptionCoeff = density * 0.5f;
    float extinction      = scatterCoeff + absorptionCoeff;

    // ---- Directional sun light ----
    float3 camPos   = float3(cameraParams.invViewMatrix._m03,
                             cameraParams.invViewMatrix._m13,
                             cameraParams.invViewMatrix._m23);
    float3 viewDir  = normalize(camPos - worldPos);
    float  cosTheta = dot(viewDir, normalize(frameConstants.sunDirection));
    float  phase    = schlickPhase(cosTheta, 0.3f);
    float  shadow   = evalShadow(float4(worldPos, 1.0f));

    float3 totalScatter = frameConstants.sunColor * shadow * phase * scatterCoeff;

    // ---- Ambient sky ----
    totalScatter += frameConstants.skyColor * scatterCoeff * 0.08f;

    // ---- Phase C: local point lights ----
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
            totalScatter += calculateLocalLightScattering(worldPos, camPos,
                                posR, pl.color.xyz, scatterCoeff, phase);
        }
    }

    // ---- Phase D: local spot lights ----
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
            totalScatter += calculateLocalSpotLightScattering(worldPos, camPos,
                                spot, spotIdx, scatterCoeff);
        }
    }

    float4 current = float4(totalScatter, extinction);

    // ---- Phase A: temporal reprojection ----
    if (pc.resetHistory == 0u) {
        // Reproject world position into previous-frame's volume.
        // cameraParams.prevViewProjectionMatrix stores (CPU: view*proj) which maps
        // world → clip space of the previous frame. prevClipH.w = prev view-space Z.
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

// ============================================================
//  AccumulateScattering kernel  (unchanged from original)
// ============================================================
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
