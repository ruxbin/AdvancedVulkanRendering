// scattervolume.hlsl
// Froxel-based volumetric scattering.
// Reference: ModernRenderingWithMetal / AAPLScatterVolume.metal
//
// Compiled TWICE with different entry points → two SPIR-V modules:
//   dxc -T cs_6_0 -E ScatterVolume      → scattervolume.cs.spv
//   dxc -T cs_6_0 -E AccumulateScattering → accumscatter.cs.spv
//
// ScatterVolume kernel      – fills each froxel with in-scattered radiance + extinction
//   Set 0: { scatterOut, UBO, shadowMaps, shadowSampler }
//
// AccumulateScattering kernel – front-to-back Beer–Lambert integration
//   Set 0: { scatterIn, accumOut }
//
// Both kernels share a push-constant block { volumeW, volumeH, screenW, screenH }.

#include "commonstruct.hlsl"

// ---- compile-time constants -------------------------------------------------
#define SCATTERING_TILE_SIZE    8
#define SCATTERING_VOLUME_DEPTH 64
#define SCATTERING_RANGE        100.0f   // view-space depth range (metres)

// =============================================================================
//  ScatterVolume bindings  (set 0)
// =============================================================================
[[vk::binding(0,0)]] RWTexture3D<float4> scatterOut;  // per-froxel: rgb=light, a=extinction

[[vk::binding(1,0)]] cbuffer ScatterUBO {
    CameraParamsBufferFull cameraParams;
    AAPLFrameConstants     frameConstants;
}

[[vk::binding(2,0)]] Texture2DArray<float> shadowMaps;
[[vk::binding(3,0)]] SamplerComparisonState shadowSampler;

// =============================================================================
//  AccumulateScattering bindings  (set 0, separate compilation)
// =============================================================================
[[vk::binding(0,0)]] Texture3D<float4>    scatterIn;   // output of ScatterVolume
[[vk::binding(1,0)]] RWTexture3D<float4>  accumOut;    // accumulated result

// =============================================================================
//  Shared push constants
// =============================================================================
struct PushConstants {
    uint  volumeWidth;
    uint  volumeHeight;
    float screenWidth;
    float screenHeight;
};
[[vk::push_constant]] PushConstants pc;

// =============================================================================
//  Shared helpers
// =============================================================================

// Slice index → view-space distance (metres)  [logarithmic depth]
float sliceToViewZ(float slice) {
    float t = slice / float(SCATTERING_VOLUME_DEPTH);
    return (exp2(t * 3.0f) - 1.0f) / 7.0f * SCATTERING_RANGE;
}

// Schlick phase function  (g=0 → isotropic, g>0 → forward scattering)
float schlickPhase(float cosTheta, float g) {
    float k     = 1.55f * g - 0.55f * g * g * g;
    float denom = 1.0f - k * cosTheta;
    return (1.0f - k * k) / (4.0f * M_PI_F * denom * denom);
}

// Exponential height fog density  (density falls off with altitude)
float heightFogDensity(float worldY, float offset, float falloff) {
    return exp(-max(worldY - offset, 0.0f) * falloff);
}

// Reconstruct world-space position of a froxel
float3 froxelToWorldPos(uint3 coord, float viewZ) {
    // Froxel (x,y) → UV [0,1]
    float2 uv = (float2(coord.xy) + 0.5f) / float2(pc.volumeWidth, pc.volumeHeight);

    // Unproject through the camera's inverse projection to get a view-space ray
    float4 ndcRay = float4(uv * 2.0f - 1.0f, 0.0f, 1.0f);
    float4 viewRay = mul(cameraParams.invProjectionMatrix, ndcRay);
    viewRay /= viewRay.w;
    // Scale ray to reach the requested view-space depth (-Z forward)
    float scale = viewZ / (-viewRay.z);
    float4 worldPos = mul(cameraParams.invViewMatrix, float4(viewRay.xyz * scale, 1.0f));
    return worldPos.xyz;
}

// Cascaded shadow evaluation
float evalShadow(float4 worldPos) {
    for (int c = 0; c < SHADOW_CASCADE_COUNT; ++c) {
        float4x4 shadowVP = mul(cameraParams.shadowMatrix[c].shadowProjectionMatrix,
                                cameraParams.shadowMatrix[c].shadowViewMatrix);
        float4 lsPos = mul(shadowVP, worldPos);
        lsPos /= lsPos.w;

        if (all(lsPos.xyz < 1.0f) && all(lsPos.xyz > float3(-1.0f, -1.0f, 0.0f))) {
            float bias = 0.001f;
            float3 uv  = float3(lsPos.xy * 0.5f + 0.5f, (float)c);
            return shadowMaps.SampleCmpLevelZero(shadowSampler, uv, lsPos.z - bias);
        }
    }
    return 1.0f; // outside all cascades → fully lit
}

// =============================================================================
//  ScatterVolume kernel
//  One thread per froxel.
//  Dispatch groups: ceil(volumeW/4) × ceil(volumeH/4) × ceil(DEPTH/4)
// =============================================================================
[numthreads(4, 4, 4)]
void ScatterVolume(uint3 DTid : SV_DispatchThreadID) {
    if (DTid.x >= pc.volumeWidth ||
        DTid.y >= pc.volumeHeight ||
        DTid.z >= SCATTERING_VOLUME_DEPTH)
        return;

    // View-space depth at the centre of this slice
    float viewZ = sliceToViewZ(float(DTid.z) + 0.5f);

    float3 worldPos = froxelToWorldPos(DTid, viewZ);

    // ---- fog density --------------------------------------------------------
    float density = frameConstants.scatterScale;
    density += 0.008f * heightFogDensity(worldPos.y, 2.0f, 0.35f);

    // Simple single-coefficient model: absorption ≈ scattering ≈ density/2
    float scatterCoeff  = density * 0.5f;
    float absorptionCoeff = density * 0.5f;
    float extinction    = scatterCoeff + absorptionCoeff;

    // ---- directional light (sun) --------------------------------------------
    float3 camPos   = mul(cameraParams.invViewMatrix, float4(0,0,0,1)).xyz;
    float3 viewDir  = -normalize(worldPos - camPos);
    float  cosTheta = dot(viewDir, normalize(frameConstants.sunDirection));
    float  phase    = schlickPhase(cosTheta, 0.3f);
    float  shadow   = evalShadow(float4(worldPos, 1.0f));

    float3 sunScatter = frameConstants.sunColor * shadow * phase * scatterCoeff;

    // ---- ambient / sky ------------------------------------------------------
    float3 skyScatter = frameConstants.skyColor * scatterCoeff * 0.08f;

    float3 totalScatter = sunScatter + skyScatter;

    // ---- store: rgb = in-scattered light, a = extinction -------------------
    scatterOut[DTid] = float4(totalScatter, extinction);
}

// =============================================================================
//  AccumulateScattering kernel
//  One thread per froxel column (x,y), marches front→back through all Z slices.
//  Dispatch groups: ceil(volumeW/8) × ceil(volumeH/8) × 1
// =============================================================================
[numthreads(8, 8, 1)]
void AccumulateScattering(uint3 DTid : SV_DispatchThreadID) {
    if (DTid.x >= pc.volumeWidth || DTid.y >= pc.volumeHeight)
        return;

    // accum.rgb = accumulated in-scattered light,  accum.a = remaining transmittance
    float4 accum = float4(0.0f, 0.0f, 0.0f, 1.0f);

    for (uint z = 0; z < SCATTERING_VOLUME_DEPTH; ++z) {
        float4 s = scatterIn[uint3(DTid.xy, z)];

        float3 inscatter = s.rgb;
        float  extinction = s.a;

        // Thickness of this depth slice
        float zNear    = sliceToViewZ((float)z);
        float zFar     = sliceToViewZ((float)z + 1.0f);
        float thickness = max(zFar - zNear, 0.0001f);

        // Beer–Lambert transmittance across the slice
        float sliceTrans = exp(-extinction * thickness);

        // Integrate: inscattered light weighted by current transmittance
        accum.rgb += inscatter * accum.a;
        accum.a   *= sliceTrans;

        // Write accumulated result for this (x,y,z)
        // rgb = total light gathered from near plane to this slice
        // a   = remaining transmittance (1 = fully transparent, 0 = opaque)
        accumOut[uint3(DTid.xy, z)] = accum;
    }
}
