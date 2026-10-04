// rt_path_common.hlsl — 相机 PT(rt_lighting.hlsl)与 VLM 烘焙(vlm_bake.hlsl)
// 共享的命中/材质/BRDF/NEE 工具。仅 Vulkan RT shader 使用(dxc lib_6_3)。
// 本文件包含 shader 入口:ClosestHitPrimary / AnyHitAlpha / AnyHitAlphaShadow /
// MissShadow,include 本文件的模块会直接获得这些入口。
// 注意:SBT 约定在两个模块间必须一致——miss[0]=primary, miss[1]=shadow;
// hit group 每 geometry 2 条:[g*2+0]=primary(chit[+ahit]), [g*2+1]=shadow([ahit])。

#include "commonstruct.hlsl"

// --- Bindings(set0 全局相机/帧常量;set1 RT 场景资源,与 Raytracing.cpp 布局一致)---
[[vk::binding(0,0)]] cbuffer cam {
    CameraParamsBufferFull cameraParams;
    AAPLFrameConstants     frameConstants;
};

[[vk::binding(0,1)]] RaytracingAccelerationStructure tlas;

// vbPositions/Normals/Tangents 为紧凑 float3(stride=12),用 ByteAddressBuffer
// 避免 StructuredBuffer<float3> 的 16B stride  padding 与 BLAS vertexStride=12 错位。
[[vk::binding(2,1)]] ByteAddressBuffer               vbPositions;
[[vk::binding(3,1)]] ByteAddressBuffer               vbNormals;
[[vk::binding(4,1)]] ByteAddressBuffer               vbTangents;
[[vk::binding(5,1)]] StructuredBuffer<float2>        vbUVs;
[[vk::binding(6,1)]] ByteAddressBuffer               ibIndices;

[[vk::binding(7,1)]]  StructuredBuffer<AAPLMeshChunk>             meshChunksRT;
[[vk::binding(9,1)]]  StructuredBuffer<AAPLPointLightCullingData> pointLightsRT;

[[vk::binding(10,1)]] Texture2D<half4>    _Textures[];
[[vk::binding(11,1)]] SamplerState        _LinearRepeatSampler;
[[vk::binding(13,1)]] StructuredBuffer<AAPLSpotLightCullingData> spotLightsRT;

// AAPLShaderMaterial layout matches GpuScene.h (alignas(16)).
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

// --- Constants ---
#define ALPHA_CUTOUT   0.1f
#define PI             3.14159265358979f
#define TWO_PI         6.28318530717959f
#define INV_PI         0.31830988618379f

// --- Payloads ---
struct PrimaryPayload {
    float3 wsPos;
    float3 normal;
    float3 albedo;
    float3 F0;
    float  roughness;
    float3 emissive;
    float  alpha;
    bool   hit;
    float  hitT;
};
struct ShadowPayload { uint visible; };

// --- PCG random number generator ---
uint pcgNext(inout uint s) {
    uint old = s * 747796405u + 2891336453u;
    uint w   = ((old >> ((old >> 28u) + 4u)) ^ old) * 277803737u;
    s = old;
    return (w >> 22u) ^ w;
}
float rngF(inout uint s) { return float(pcgNext(s)) * 2.3283064365e-10f; }
float2 rng2F(inout uint s) { return float2(rngF(s), rngF(s)); }

// --- Orthonormal basis ---
void buildBasis(float3 N, out float3 T, out float3 B) {
    float3 up = abs(N.z) < 0.9999f ? float3(0,0,1) : float3(1,0,0);
    T = normalize(cross(up, N));
    B = cross(N, T);
}
float3 localToWorld(float3 v, float3 N) {
    float3 T, B;
    buildBasis(N, T, B);
    return v.x * T + v.y * B + v.z * N;
}

// --- BRDF evaluation (Smith GGX + Lambertian) ---
// Returns (diffuse + specular) * NdL — matches lighting.hlsl convention.
float3 evalBRDF(float3 N, float3 V, float3 L,
                float3 albedo, float3 F0, float roughness) {
    float NdL = saturate(dot(N, L));
    float NdV = saturate(dot(N, V));
    if (NdL <= 0.0f || NdV <= 0.0f) return (float3)0;

    float3 H   = normalize(V + L);
    float NdH  = saturate(dot(N, H));
    float LdH  = saturate(dot(L, H));
    float alpha = roughness * roughness;
    float alpha2 = max(alpha * alpha, 1e-6f);
    float k      = alpha / 2.0f;

    float3 F = F0 + (1.0f - F0) * pow(1.0f - LdH, 5.0f);
    float G  = (1.0f / max(NdL * (1.0f - k) + k, 1e-6f)) *
               (1.0f / max(NdV * (1.0f - k) + k, 1e-6f));
    float denom = NdH * NdH * (alpha2 - 1.0f) + 1.0f;
    float D     = alpha2 / max(PI * denom * denom, 1e-6f);

    float3 specular = F * G * D / 4.0f;
    float3 diffuse  = albedo * INV_PI * (1.0f - F);
    return (diffuse + specular) * NdL;
}

// --- Diffuse sampling: cosine-weighted hemisphere ---
float3 sampleDiffuse(float3 N, float2 xi) {
    float cosTheta = sqrt(max(xi.x, 0.0f));
    float sinTheta = sqrt(max(1.0f - xi.x, 0.0f));
    float phi      = TWO_PI * xi.y;
    return localToWorld(float3(cos(phi) * sinTheta, sin(phi) * sinTheta, cosTheta), N);
}
float diffusePdf(float3 N, float3 wi) {
    return max(dot(N, wi), 0.0f) * INV_PI;
}

// --- Specular sampling: GGX NDF importance sampling ---
float3 sampleGGX(float3 V, float3 N, float roughness, float2 xi, out bool valid) {
    float alpha  = roughness * roughness;
    float alpha2 = max(alpha * alpha, 1e-6f);
    float phi    = TWO_PI * xi.y;
    float cosTheta2 = (1.0f - xi.x) / max(1.0f + (alpha2 - 1.0f) * xi.x, 1e-6f);
    float cosTheta  = sqrt(max(cosTheta2, 0.0f));
    float sinTheta  = sqrt(max(1.0f - cosTheta2, 0.0f));
    float3 H = localToWorld(float3(cos(phi) * sinTheta, sin(phi) * sinTheta, cosTheta), N);
    float3 wi = reflect(-V, H);
    valid = (dot(wi, N) > 0.0f);
    return wi;
}
float specularPdf(float3 N, float3 V, float3 wi, float roughness) {
    float3 H    = normalize(V + wi);
    float NdH   = saturate(dot(N, H));
    float VdH   = saturate(dot(V, H));
    float alpha  = roughness * roughness;
    float alpha2 = max(alpha * alpha, 1e-6f);
    float denom  = NdH * NdH * (alpha2 - 1.0f) + 1.0f;
    float D      = alpha2 / max(PI * denom * denom, 1e-6f);
    return D * NdH / max(4.0f * VdH, 1e-6f);
}

// Sample BRDF direction; returns throughput weight = brdf(wi)*NdL / pdf_total.
float3 sampleBRDF(float3 N, float3 V, float3 albedo, float3 F0, float roughness,
                  inout uint rng, out float3 wi) {
    // Fresnel at view angle determines specular probability.
    float NdV   = max(dot(N, V), 0.0f);
    float3 F_at = F0 + (1.0f - F0) * pow(1.0f - NdV, 5.0f);
    float pSpec = clamp(dot(float3(0.2126f, 0.7152f, 0.0722f), F_at), 0.05f, 0.95f);

    float2 xi    = rng2F(rng);
    float chooser = rngF(rng);

    if (chooser < pSpec) {
        bool valid;
        wi = sampleGGX(V, N, roughness, xi, valid);
        if (!valid || dot(wi, N) <= 0.0f) return (float3)0;
        float pdf = specularPdf(N, V, wi, roughness) * pSpec;
        if (pdf < 1e-6f) return (float3)0;
        return evalBRDF(N, V, wi, albedo, F0, roughness) / pdf;
    } else {
        wi = sampleDiffuse(N, xi);
        if (dot(wi, N) <= 0.0f) return (float3)0;
        float pdf = diffusePdf(N, wi) * (1.0f - pSpec);
        if (pdf < 1e-6f) return (float3)0;
        return evalBRDF(N, V, wi, albedo, F0, roughness) / pdf;
    }
}

// --- Geometry helpers ---
uint3 fetchTriangleIndices(AAPLMeshChunk chunk, uint primIdx) {
    uint base = (chunk.indexBegin + primIdx * 3) * 4;
    return ibIndices.Load3(base);
}

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
    uint3 idx    = fetchTriangleIndices(chunk, primIdx);

    // Tightly packed float3 (stride=12) — see binding declarations.
    float3 n0 = asfloat(vbNormals.Load3(idx.x * 12));
    float3 n1 = asfloat(vbNormals.Load3(idx.y * 12));
    float3 n2 = asfloat(vbNormals.Load3(idx.z * 12));
    float3 t0 = asfloat(vbTangents.Load3(idx.x * 12));
    float3 t1 = asfloat(vbTangents.Load3(idx.y * 12));
    float3 t2 = asfloat(vbTangents.Load3(idx.z * 12));
    float2 u0 = vbUVs[idx.x];
    float2 u1 = vbUVs[idx.y];
    float2 u2 = vbUVs[idx.z];

    float3 bary = float3(1.0f - attribs.barycentrics.x - attribs.barycentrics.y,
                          attribs.barycentrics.x, attribs.barycentrics.y);
    h.geoN = normalize(bary.x * n0 + bary.y * n1 + bary.z * n2);
    h.geoT = normalize(bary.x * t0 + bary.y * t1 + bary.z * t2);
    h.uv   =           bary.x * u0 + bary.y * u1 + bary.z * u2;
    h.materialIndex = chunk.materialIndex;
    h.hitT  = RayTCurrent();
    h.wsPos = WorldRayOrigin() + WorldRayDirection() * h.hitT;
    return h;
}

// Sample a direction toward the sun disk cone.
// Note: frameConstants.sunDirection is already surface→sun (matches raster's
// lighting.hlsl:63 which uses it directly as the light direction). Don't negate.
// coneRadius 原为 pc.sunConeRadius;调用方传入,rt 传 pc.sunConeRadius,
// vlm_bake 传 pc.sunConeRadius(烘焙 push const 同名字段)。
float3 sampleSunDir(inout uint rng, float coneRadius) {
    float3 sunAxis = normalize(frameConstants.sunDirection);
    float cosThetaMax = cos(coneRadius);
    float2 xi = rng2F(rng);
    float cosTheta = lerp(cosThetaMax, 1.0f, xi.x);
    float sinTheta = sqrt(saturate(1.0f - cosTheta * cosTheta));
    float phi = TWO_PI * xi.y;
    float3 local = float3(cos(phi) * sinTheta, sin(phi) * sinTheta, cosTheta);
    float3 up = abs(sunAxis.z) < 0.999f ? float3(0,0,1) : float3(1,0,0);
    float3 t  = normalize(cross(up, sunAxis));
    float3 b  = cross(sunAxis, t);
    return normalize(local.x * t + local.y * b + local.z * sunAxis);
}

// Smooth distance attenuation matching raster lighting.hlsl:84.
float distanceAttenuation(float3 unormLightVec, float invSqrAttRadius) {
    float sqrDist = dot(unormLightVec, unormLightVec);
    float att = 1.0f / max(sqrDist, 1e-4f);
    float factor = sqrDist * invSqrAttRadius;
    float smoothFactor = saturate(1.0f - factor * factor);
    att *= smoothFactor * smoothFactor;
    return att;
}

// Trace a shadow ray to a finite-distance light. Returns 1 if visible, 0 if occluded.
float traceShadowRay(float3 origin, float3 dir, float tMax) {
    RayDesc sr;
    sr.Origin    = origin;
    sr.Direction = dir;
    sr.TMin      = 1e-3f;
    sr.TMax      = tMax;
    ShadowPayload sp; sp.visible = 0;
    TraceRay(tlas,
             RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH |
             RAY_FLAG_SKIP_CLOSEST_HIT_SHADER,
             0xFF, 1, 2, 1, sr, sp);
    return float(sp.visible);
}

// NEE for all point + spot lights at a hit point. Each light: distance / cone
// cull cheaply, then if its contribution would be non-zero fire a shadow ray
// and add BRDF*light*attenuation. Returns the total radiance contribution
// (already multiplied by throughput in the caller's accumulation).
// pointCount/spotCount 原为 pc.pointLightCount/spotLightCount;调用方传入。
float3 evalLocalLightsNEE(float3 wsP, float3 N, float3 V,
                          float3 albedo, float3 F0, float roughness,
                          uint pointCount, uint spotCount) {
    float3 result = (float3)0;

    // --- Point lights ---
    for (uint i = 0; i < pointCount; ++i) {
        AAPLPointLightCullingData L = pointLightsRT[i];
        float3 toLight = L.posRadius.xyz - wsP;
        float  d2      = dot(toLight, toLight);
        float  radius  = L.posRadius.w;
        if (d2 > radius * radius) continue;

        float  dist    = sqrt(d2);
        float3 Ldir    = toLight / max(dist, 1e-4f);
        if (dot(N, Ldir) <= 0.0f) continue;

        float invSqrR  = 1.0f / max(radius * radius, 1e-4f);
        float distAtt  = distanceAttenuation(toLight, invSqrR);
        if (distAtt <= 0.0f) continue;

        // Light visibility (shadow ray)
        float vis = traceShadowRay(wsP, Ldir, dist - 2e-3f);
        if (vis <= 0.0f) continue;

        float3 lightCol = L.color.xyz * PI * distAtt * frameConstants.localLightIntensity;
        result += evalBRDF(N, V, Ldir, albedo, F0, roughness) * lightCol;
    }

    // --- Spot lights ---
    for (uint j = 0; j < spotCount; ++j) {
        AAPLSpotLightCullingData S = spotLightsRT[j];
        float3 toLight = S.posAndHeight.xyz - wsP;
        float  dist    = length(toLight);
        if (dist > S.posAndHeight.w) continue;

        float3 Ldir    = toLight / max(dist, 1e-4f);
        // Cone test: cos(angle) between -L and spot dir vs cos(outerAngle)
        float cosTheta = dot(-Ldir, S.dirAndOuterAngle.xyz);
        if (cosTheta < S.dirAndOuterAngle.w) continue;
        if (dot(N, Ldir) <= 0.0f) continue;

        float invSqrR  = 1.0f / max(S.posAndHeight.w * S.posAndHeight.w, 1e-4f);
        float distAtt  = distanceAttenuation(toLight, invSqrR);
        float angRange = max(S.cosInnerAngle - S.dirAndOuterAngle.w, 1e-4f);
        float t        = saturate((cosTheta - S.dirAndOuterAngle.w) / angRange);
        float angAtt   = t * t;
        if (distAtt * angAtt <= 0.0f) continue;

        float vis = traceShadowRay(wsP, Ldir, dist - 2e-3f);
        if (vis <= 0.0f) continue;

        float3 lightCol = S.color.xyz * PI * (distAtt * angAtt)
                          * frameConstants.localLightIntensity;
        result += evalBRDF(N, V, Ldir, albedo, F0, roughness) * lightCol;
    }
    return result;
}

// ============================================================
// Shader entries (shared by rt_lighting.hlsl and vlm_bake.hlsl)
// ============================================================

[shader("miss")]
void MissShadow(inout ShadowPayload sp) {
    sp.visible = 1;
}

[shader("closesthit")]
void ClosestHitPrimary(inout PrimaryPayload p,
                       BuiltInTriangleIntersectionAttributes attribs) {
    HitInputs h = gatherHit(attribs);
    AAPLShaderMaterial mat = materialsRT[h.materialIndex];

    float lod = 0.0f;  // use base mip; cone-based LOD can be added later

    half4 baseColor = _Textures[NonUniformResourceIndex(mat.albedo_texture_index)].SampleLevel(
        _LinearRepeatSampler, h.uv, lod);

    half4 materialData = (half4)0;
    if (mat.hasMetallicRoughness > 0)
        materialData = _Textures[NonUniformResourceIndex(mat.roughness_texture_index)].SampleLevel(
            _LinearRepeatSampler, h.uv, lod);

    half4 emissive = (half4)0;
    if (mat.hasEmissive > 0)
        emissive = _Textures[NonUniformResourceIndex(mat.emissive_texture_index)].SampleLevel(
            _LinearRepeatSampler, h.uv, lod);

    half4 texnormal = _Textures[NonUniformResourceIndex(mat.normal_texture_index)].SampleLevel(
        _LinearRepeatSampler, h.uv, lod);
    texnormal.xy = (half)2 * texnormal.xy - (half)1;
    texnormal.z  = sqrt(saturate(1.0f - dot(texnormal.xy, texnormal.xy)));

    // Gram-Schmidt: re-orthogonalize tangent against normal for orthonormal TBN.
    half3 geonormal = (half3)normalize(h.geoN);
    half3 geotan    = (half3)normalize(h.geoT - dot(h.geoT, h.geoN) * h.geoN);
    half3 geobinorm = normalize(cross(geotan, geonormal));
    half3 normal    = normalize(texnormal.b * geonormal - texnormal.g * geotan + texnormal.r * geobinorm);

    p.wsPos    = h.wsPos;
    p.normal   = (float3)normal;
    p.albedo   = (float3)lerp(baseColor.rgb, (half3)0.0h, materialData.b);
    p.F0       = (float3)lerp((half3)0.04h, baseColor.rgb, materialData.b);
    p.roughness = (float)max((half)0.08h, materialData.g);
    p.alpha    = (float)(baseColor.a * mat.alpha);
    p.emissive = (float3)emissive.rgb;
    p.hit      = true;
    p.hitT     = h.hitT;
}

// Slim AnyHit helper: fetch ONLY UV (skip 6 Load3 of normals/tangents).
// AnyHit fires many times per ray on alpha-masked foliage; cutting per-call
// work is the biggest win for keeping the path tracer under TDR.
float2 fetchHitUV(BuiltInTriangleIntersectionAttributes attribs, out uint matIdx) {
    uint geomIdx = GeometryIndex();
    AAPLMeshChunk chunk = meshChunksRT[geomIdx];
    matIdx = chunk.materialIndex;
    uint primIdx = PrimitiveIndex();
    uint3 idx = fetchTriangleIndices(chunk, primIdx);
    float2 u0 = vbUVs[idx.x];
    float2 u1 = vbUVs[idx.y];
    float2 u2 = vbUVs[idx.z];
    float3 bary = float3(1.0f - attribs.barycentrics.x - attribs.barycentrics.y,
                          attribs.barycentrics.x, attribs.barycentrics.y);
    return bary.x * u0 + bary.y * u1 + bary.z * u2;
}

[shader("anyhit")]
void AnyHitAlpha(inout PrimaryPayload p,
                 BuiltInTriangleIntersectionAttributes attribs) {
    uint matIdx;
    float2 uv = fetchHitUV(attribs, matIdx);
    AAPLShaderMaterial mat = materialsRT[matIdx];
    half4 baseColor = _Textures[NonUniformResourceIndex(mat.albedo_texture_index)].SampleLevel(
        _LinearRepeatSampler, uv, 0);
    if (baseColor.a < (half)ALPHA_CUTOUT)
        IgnoreHit();
}

[shader("anyhit")]
void AnyHitAlphaShadow(inout ShadowPayload sp,
                       BuiltInTriangleIntersectionAttributes attribs) {
    uint matIdx;
    float2 uv = fetchHitUV(attribs, matIdx);
    AAPLShaderMaterial mat = materialsRT[matIdx];
    half4 baseColor = _Textures[NonUniformResourceIndex(mat.albedo_texture_index)].SampleLevel(
        _LinearRepeatSampler, uv, 0);
    if (baseColor.a < (half)ALPHA_CUTOUT)
        IgnoreHit();
}
