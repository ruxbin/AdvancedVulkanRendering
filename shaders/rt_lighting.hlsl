// Hardware ray tracing path. Replaces the entire raster deferred pipeline.
// Primary rays from the camera, BRDF + N-tap sun shadow + full point-light loop
// all in raygen. Closest-hit fills surface payload (vertex pull + bindless
// material), any-hit handles alpha mask. See docs/RayTracing_Changes.md.
//
// Compile with:
//   dxc -spirv -T lib_6_3 rt_lighting.hlsl -fspv-target-env=vulkan1.2
//        -fspv-extension=SPV_KHR_ray_tracing
//        -fspv-extension=SPV_KHR_physical_storage_buffer
//        -fspv-extension=SPV_KHR_shader_non_semantic_info
//        -Fo rt_lighting.lib.spv

#include "commonstruct.hlsl"
#include "lighting.hlsl"

// --- Bindings -------------------------------------------------------------
// set 0 binding 0: same camera+frame UBO as deferredlighting (re-use)
[[vk::binding(0,0)]]
cbuffer cam {
    CameraParamsBufferFull cameraParams;
    AAPLFrameConstants     frameConstants;
};

// set 1: RT-specific resources (TLAS, output, geometry pull, bindless textures)
[[vk::binding(0,1)]] RaytracingAccelerationStructure tlas;
[[vk::binding(1,1)]] RWTexture2D<float4>             outLitColor;

[[vk::binding(2,1)]] StructuredBuffer<float3>        vbPositions;
[[vk::binding(3,1)]] StructuredBuffer<float3>        vbNormals;
[[vk::binding(4,1)]] StructuredBuffer<float3>        vbTangents;
[[vk::binding(5,1)]] StructuredBuffer<float2>        vbUVs;
[[vk::binding(6,1)]] ByteAddressBuffer               ibIndices;

[[vk::binding(7,1)]] StructuredBuffer<AAPLMeshChunk> meshChunksRT;

// AAPLShaderMaterial layout matches Src/Include/GpuScene.h (alignas(16)).
struct AAPLShaderMaterial {
    uint   albedo_texture_index;
    uint   roughness_texture_index;
    uint   normal_texture_index;
    uint   emissive_texture_index;
    float  alpha;
    uint   hasMetallicRoughness;
    uint   hasEmissive;
    uint   _pad;
};
[[vk::binding(8,1)]] StructuredBuffer<AAPLShaderMaterial> materialsRT;

// Full unculled point light list (AAPLPointLightCullingData = vec4 posRadius + vec4 color).
[[vk::binding(9,1)]] StructuredBuffer<AAPLPointLightCullingData> pointLightsRT;

[[vk::binding(10,1)]] Texture2D<half4>     _Textures[];
[[vk::binding(11,1)]] SamplerState         _LinearRepeatSampler;

// Per-dispatch knobs from C++ side
struct RTPushConsts {
    uint  pointLightCount;
    uint  shadowTaps;        // e.g. 4 or 8
    float sunConeRadius;     // tan(sun angular half-radius), e.g. tan(0.5deg)
    float pixelSpreadAngle;  // 2*tan(fovY/2)/screenHeight, used for ray cone mip
    uint  frameSeed;         // optional jitter seed
    uint  _pad0, _pad1, _pad2;
};
[[vk::push_constant]] RTPushConsts pc;

// --- Payloads -------------------------------------------------------------
struct PrimaryPayload {
    float3 wsPos;
    float3 normal;     // tangent-space-perturbed
    float3 albedo;
    float3 F0;
    float  roughness;
    float3 emissive;
    float  alpha;
    bool   hit;
    float  hitT;
};
struct ShadowPayload { uint visible; };

// --- Helpers --------------------------------------------------------------
#define ALPHA_CUTOUT 0.1f

// Hammersley low-discrepancy 2D
float radicalInverse_VdC(uint bits) {
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return float(bits) * 2.3283064365386963e-10;
}
float2 hammersley(uint i, uint N) {
    return float2(float(i) / float(N), radicalInverse_VdC(i));
}

// Rotate `dir` into a cone of half-angle `coneRadius` (radians, small) using xi
// xi.x ∈ [0,1) → radial cosine, xi.y ∈ [0,1) → azimuth
float3 sampleConeDir(float3 axis, float coneRadius, float2 xi) {
    float cosThetaMax = cos(coneRadius);
    float cosTheta    = lerp(cosThetaMax, 1.0f, xi.x);
    float sinTheta    = sqrt(saturate(1.0f - cosTheta * cosTheta));
    float phi         = xi.y * 6.2831853f;
    float3 local      = float3(cos(phi) * sinTheta, sin(phi) * sinTheta, cosTheta);

    // Build orthonormal basis around axis
    float3 up = abs(axis.z) < 0.999f ? float3(0,0,1) : float3(1,0,0);
    float3 t  = normalize(cross(up, axis));
    float3 b  = cross(axis, t);
    return normalize(local.x * t + local.y * b + local.z * axis);
}

// Reconstruct primary ray from pixel (jittered to pixel center).
void cameraRayFromPixel(uint2 px, uint2 dim, out float3 origin, out float3 dir) {
    float2 ndc = ((float2(px) + 0.5f) / float2(dim)) * 2.0f - 1.0f;
    ndc.y = -ndc.y;  // matches worldPositionForTexcoord convention
    float4 clip  = float4(ndc, 1.0f, 1.0f);
    float4 world = mul(cameraParams.invViewProjectionMatrix, clip);
    world /= world.w;
    origin = float3(cameraParams.invViewMatrix._m03,
                    cameraParams.invViewMatrix._m13,
                    cameraParams.invViewMatrix._m23);
    dir = normalize(world.xyz - origin);
}

// Pull triangle indices for hit (chunk-local, indexBegin already added).
uint3 fetchTriangleIndices(AAPLMeshChunk chunk, uint primIdx) {
    uint base = (chunk.indexBegin + primIdx * 3) * 4;
    return ibIndices.Load3(base);
}

// --- Shaders --------------------------------------------------------------

[shader("raygeneration")]
void RayGen() {
    uint2 px  = DispatchRaysIndex().xy;
    uint2 dim = DispatchRaysDimensions().xy;

    float3 ro, rd;
    cameraRayFromPixel(px, dim, ro, rd);

    RayDesc r;
    r.Origin    = ro;
    r.Direction = rd;
    r.TMin      = 1e-3f;
    r.TMax      = 1e30f;

    PrimaryPayload p;
    p.hit = false;
    p.wsPos = float3(0,0,0);
    p.normal = float3(0,0,1);
    p.albedo = float3(0,0,0);
    p.F0 = float3(0.04,0.04,0.04);
    p.roughness = 1.0f;
    p.emissive  = float3(0,0,0);
    p.alpha     = 1.0f;
    p.hitT      = 0.0f;

    // Primary: hgOffset=0, hgStride=2, missIdx=0
    TraceRay(tlas, RAY_FLAG_NONE, 0xFF, 0, 2, 0, r, p);

    if (!p.hit) {
        outLitColor[px] = float4(0, 0, 0, 1);
        return;
    }

    AAPLPixelSurfaceData surfaceData;
    surfaceData.normal    = (half3)p.normal;
    surfaceData.albedo    = (half3)p.albedo;
    surfaceData.F0        = (half3)lerp(p.F0, (float3)0.02f, frameConstants.wetness);
    surfaceData.roughness = (half) lerp(p.roughness, 0.1f, frameConstants.wetness);
    surfaceData.alpha     = (half) p.alpha;
    surfaceData.emissive  = (half3)p.emissive;

    // --- N-tap sun shadow ---
    float visibility = 0.0f;
    uint  N = max(1u, pc.shadowTaps);
    {
        float3 sunAxis = normalize(-frameConstants.sunDirection);
        float3 origin  = p.wsPos + p.normal * 1e-3f;
        for (uint i = 0; i < N; ++i) {
            float2 xi = hammersley(i + pc.frameSeed, N);
            float3 sdir = sampleConeDir(sunAxis, pc.sunConeRadius, xi);
            RayDesc sr;
            sr.Origin    = origin;
            sr.Direction = sdir;
            sr.TMin      = 1e-3f;
            sr.TMax      = 1e30f;
            ShadowPayload sp;
            sp.visible = 0;
            // Shadow: hgOffset=1, hgStride=2, missIdx=1
            TraceRay(tlas,
                     RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER,
                     0xFF, 1, 2, 1, sr, sp);
            visibility += float(sp.visible);
        }
        visibility /= float(N);
    }

    // --- Direct lighting (sun) — same formula as deferredlighting.hlsl ---
    half3 result = lightingShader(surfaceData, 0, float4(p.wsPos, 1.0),
                                  frameConstants, cameraParams) * (half)visibility;

    // --- Point lights — full unculled loop, no shadow (matches raster cluster path) ---
    for (uint i = 0; i < pc.pointLightCount; ++i) {
        AAPLPointLightCullingData L = pointLightsRT[i];
        // posRadius.w in this buffer is RADIUS (linear), but lightingShaderPointSpot expects
        // posSqrRadius (w = radius^2). Match the encoding used by deferredlighting cluster path.
        float r2 = L.posRadius.w * L.posRadius.w;
        float4 posSqrR = float4(L.posRadius.xyz, r2);
        result += lightingShaderPointSpot(surfaceData, 0, float4(p.wsPos, 1.0),
                                           frameConstants, cameraParams,
                                           posSqrR, L.color.xyz);
    }

    outLitColor[px] = float4((float3)result, 1.0f);
}

[shader("miss")]
void MissPrimary(inout PrimaryPayload p) {
    p.hit = false;
}

[shader("miss")]
void MissShadow(inout ShadowPayload sp) {
    sp.visible = 1;
}

// Closest-hit shared logic: vertex pull + interpolate + bindless material lookup.
struct HitInputs {
    float3 wsPos;
    float3 geoN;
    float3 geoT;
    float2 uv;
    uint   materialIndex;
    float  hitT;
};

HitInputs gatherHit(BuiltInTriangleIntersectionAttributes attribs) {
    HitInputs h;
    uint geomIdx = GeometryIndex();
    AAPLMeshChunk chunk = meshChunksRT[geomIdx];
    uint primIdx = PrimitiveIndex();
    uint3 idx = fetchTriangleIndices(chunk, primIdx);

    float3 n0 = vbNormals[idx.x];
    float3 n1 = vbNormals[idx.y];
    float3 n2 = vbNormals[idx.z];
    float3 t0 = vbTangents[idx.x];
    float3 t1 = vbTangents[idx.y];
    float3 t2 = vbTangents[idx.z];
    float2 u0 = vbUVs[idx.x];
    float2 u1 = vbUVs[idx.y];
    float2 u2 = vbUVs[idx.z];

    float3 bary = float3(1.0f - attribs.barycentrics.x - attribs.barycentrics.y,
                          attribs.barycentrics.x, attribs.barycentrics.y);

    h.geoN = normalize(bary.x * n0 + bary.y * n1 + bary.z * n2);
    h.geoT = normalize(bary.x * t0 + bary.y * t1 + bary.z * t2);
    h.uv   =          bary.x * u0 + bary.y * u1 + bary.z * u2;
    h.materialIndex = chunk.materialIndex;
    h.hitT = RayTCurrent();
    h.wsPos = WorldRayOrigin() + WorldRayDirection() * h.hitT;
    return h;
}

// Ray cone first-order mip estimate: cone radius at hit / approximate texel size.
// We cheat texel size with a constant — close enough for v1.
float coneMipLOD(float hitT) {
    float coneWidth = pc.pixelSpreadAngle * hitT;
    // Assume textures are roughly 2048 sized in world (eyeballed).
    const float texelWorldSize = 1.0f / 2048.0f;
    return max(0.0f, log2(max(coneWidth / texelWorldSize, 1.0f)));
}

[shader("closesthit")]
void ClosestHitPrimary(inout PrimaryPayload p,
                       BuiltInTriangleIntersectionAttributes attribs) {
    HitInputs h = gatherHit(attribs);
    AAPLShaderMaterial mat = materialsRT[h.materialIndex];

    float lod = coneMipLOD(h.hitT);

    half4 baseColor = _Textures[mat.albedo_texture_index].SampleLevel(
        _LinearRepeatSampler, h.uv, lod);

    half4 materialData = half4(0,0,0,0);
    if (mat.hasMetallicRoughness > 0)
        materialData = _Textures[mat.roughness_texture_index].SampleLevel(
            _LinearRepeatSampler, h.uv, lod);

    half4 emissive = half4(0,0,0,0);
    if (mat.hasEmissive > 0)
        emissive = _Textures[mat.emissive_texture_index].SampleLevel(
            _LinearRepeatSampler, h.uv, lod);

    half4 texnormal = _Textures[mat.normal_texture_index].SampleLevel(
        _LinearRepeatSampler, h.uv, lod);
    texnormal.xy = (half)2 * texnormal.xy - (half)1;
    texnormal.z  = sqrt(saturate(1.0f - dot(texnormal.xy, texnormal.xy)));

    // Same TBN combination as drawcluster.hlsl base pass.
    half3 geonormal = (half3)normalize(h.geoN);
    half3 geotan    = (half3)normalize(h.geoT);
    half3 geobinorm = normalize(cross(geotan, geonormal));
    half3 normal    = texnormal.b * geonormal - texnormal.g * geotan + texnormal.r * geobinorm;

    p.wsPos     = h.wsPos;
    p.normal    = (float3)normal;
    p.albedo    = (float3)lerp(baseColor.rgb, (half3)0.0h, materialData.b);
    p.F0        = (float3)lerp((half3)0.04h, baseColor.rgb, materialData.b);
    p.roughness = (float)max((half)0.08, materialData.g);
    p.alpha     = (float)(baseColor.a * mat.alpha);
    p.emissive  = (float3)emissive.rgb;
    p.hit       = true;
    p.hitT      = h.hitT;
}

[shader("anyhit")]
void AnyHitAlpha(inout PrimaryPayload p,
                 BuiltInTriangleIntersectionAttributes attribs) {
    HitInputs h = gatherHit(attribs);
    AAPLShaderMaterial mat = materialsRT[h.materialIndex];
    half4 baseColor = _Textures[mat.albedo_texture_index].SampleLevel(
        _LinearRepeatSampler, h.uv, 0);
    if (baseColor.a < (half)ALPHA_CUTOUT)
        IgnoreHit();
}

// Shadow-ray any-hit needs its own payload type but shares the alpha test.
[shader("anyhit")]
void AnyHitAlphaShadow(inout ShadowPayload sp,
                       BuiltInTriangleIntersectionAttributes attribs) {
    HitInputs h = gatherHit(attribs);
    AAPLShaderMaterial mat = materialsRT[h.materialIndex];
    half4 baseColor = _Textures[mat.albedo_texture_index].SampleLevel(
        _LinearRepeatSampler, h.uv, 0);
    if (baseColor.a < (half)ALPHA_CUTOUT)
        IgnoreHit();
}
