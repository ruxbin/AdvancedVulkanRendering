// Hardware path tracing. Progressive accumulation with BRDF importance sampling,
// iterative bounce loop, and NEE for direct sun lighting.
//
// Keep RTPushConsts in sync with Src/Raytracing.cpp local RTPC struct (32 bytes).
//
// Compile with:
//   dxc -spirv -T lib_6_3 rt_lighting.hlsl -fspv-target-env=vulkan1.2
//        -fspv-extension=SPV_KHR_ray_tracing
//        -fspv-extension=SPV_KHR_physical_storage_buffer
//        -fspv-extension=SPV_KHR_non_semantic_info
//        -fspv-extension=SPV_EXT_descriptor_indexing
//        -Fo rt_lighting.lib.spv

#include "commonstruct.hlsl"

// --- Bindings ---
[[vk::binding(0,0)]] cbuffer cam {
    CameraParamsBufferFull cameraParams;
    AAPLFrameConstants     frameConstants;
};

[[vk::binding(0,1)]] RaytracingAccelerationStructure tlas;
[[vk::binding(1,1)]] RWTexture2D<float4>             outLitColor;   // tone-mapped display output

// vbPositions/Normals/Tangents are tightly packed float3 (stride=12). Use
// ByteAddressBuffer + Load3 to avoid DXC's StructuredBuffer<float3> 16-byte
// stride padding, which would mis-read against the BLAS vertexStride=12 layout.
[[vk::binding(2,1)]] ByteAddressBuffer               vbPositions;
[[vk::binding(3,1)]] ByteAddressBuffer               vbNormals;
[[vk::binding(4,1)]] ByteAddressBuffer               vbTangents;
[[vk::binding(5,1)]] StructuredBuffer<float2>        vbUVs;
[[vk::binding(6,1)]] ByteAddressBuffer               ibIndices;

[[vk::binding(7,1)]]  StructuredBuffer<AAPLMeshChunk>            meshChunksRT;
[[vk::binding(9,1)]]  StructuredBuffer<AAPLPointLightCullingData> pointLightsRT;

[[vk::binding(10,1)]] Texture2D<half4>    _Textures[];
[[vk::binding(11,1)]] SamplerState        _LinearRepeatSampler;
[[vk::binding(12,1)]] RWTexture2D<float4> outAccumColor;  // persistent accumulation buffer

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

// Per-dispatch knobs — must stay exactly 32 bytes (matches Raytracing.cpp RTPC).
struct RTPushConsts {
    uint  pointLightCount;
    uint  accumCount;       // samples accumulated so far (0 = first sample)
    float sunConeRadius;    // tan(sun angular half-radius), e.g. tan(0.5deg)
    float pixelSpreadAngle; // 2*tan(fovY/2)/screenHeight
    uint  frameSeed;        // per-frame jitter seed
    uint  maxBounces;       // max path depth
    uint  resetAccum;       // 1 = camera moved, start fresh
    uint  _pad;
};
[[vk::push_constant]] RTPushConsts pc;

// --- Constants ---
#define ALPHA_CUTOUT   0.1f
#define PI             3.14159265358979f
#define TWO_PI         6.28318530717959f
#define INV_PI         0.31830988618379f
#define FIREFLY_CLAMP  20.0f

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

// --- Sky model: matches deferredlighting.hlsl horizon-zenith blend ---
float3 skyColor(float3 dir) {
    float t = saturate(dir.y * 0.5f + 0.5f);
    return lerp(frameConstants.skyColor * 0.6f, frameConstants.skyColor, t * t);
}

// --- ACES filmic tone mapping ---
float3 aces(float3 x) {
    x *= 0.6f;
    return saturate((x * (2.51f * x + 0.03f)) / (x * (2.43f * x + 0.59f) + 0.14f));
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
float3 sampleSunDir(inout uint rng) {
    float3 sunAxis = normalize(frameConstants.sunDirection);
    float cosThetaMax = cos(pc.sunConeRadius);
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

// Reconstruct primary ray from pixel with sub-pixel jitter.
void cameraRayFromPixel(uint2 px, uint2 dim, float2 jitter,
                        out float3 origin, out float3 dir) {
    float2 ndc = ((float2(px) + 0.5f + jitter) / float2(dim)) * 2.0f - 1.0f;
    float4 clip  = float4(ndc, 1.0f, 1.0f);
    float4 world = mul(cameraParams.invViewProjectionMatrix, clip);
    world /= world.w;
    origin = float3(cameraParams.invViewMatrix._m03,
                    cameraParams.invViewMatrix._m13,
                    cameraParams.invViewMatrix._m23);
    dir = normalize(world.xyz - origin);
}

// ============================================================
// Shaders
// ============================================================

[shader("raygeneration")]
void RayGen() {
    uint2 px  = DispatchRaysIndex().xy;
    uint2 dim = DispatchRaysDimensions().xy;

    // Per-pixel RNG: mix pixel coords (resolution-agnostic), frame seed, accum count.
    uint rng = (px.x * dim.y + px.y) ^ (pc.frameSeed * 2654435761u)
                                     ^ (pc.accumCount * 805459861u);
    pcgNext(rng); pcgNext(rng);  // warm up

    float2 jitter = rng2F(rng) - 0.5f;
    float3 ro, rd;
    cameraRayFromPixel(px, dim, jitter, ro, rd);

    // --- Path tracing loop ---
    float3 throughput = float3(1.0f, 1.0f, 1.0f);
    float3 radiance   = float3(0.0f, 0.0f, 0.0f);

    RayDesc ray;
    ray.Origin    = ro;
    ray.Direction = rd;
    ray.TMin      = 1e-3f;
    ray.TMax      = 1e30f;

    uint maxBounces = max(1u, pc.maxBounces);

    for (uint bounce = 0; bounce < maxBounces; ++bounce) {
        PrimaryPayload p;
        p.hit = false; p.wsPos = 0; p.normal = 0; p.albedo = 0;
        p.F0 = 0.04f; p.roughness = 1.0f; p.emissive = 0; p.alpha = 1; p.hitT = 0;

        // hgOffset=0, hgStride=2, missIdx=0
        TraceRay(tlas, RAY_FLAG_CULL_BACK_FACING_TRIANGLES, 0xFF, 0, 2, 0, ray, p);

        if (!p.hit) {
            radiance += throughput * skyColor(ray.Direction);
            break;
        }

        // Emissive contribution
        radiance += throughput * p.emissive * frameConstants.emissiveScale;
        radiance  = min(radiance, FIREFLY_CLAMP);

        float3 N = normalize(p.normal);
        float3 V = -ray.Direction;
        // Ensure normal faces the incoming ray (two-sided surfaces)
        if (dot(N, V) < 0.0f) N = -N;
        float3 wsP = p.wsPos + N * 2e-3f;  // offset for shadow/bounce rays

        // --- NEE: direct sun lighting ---
        {
            float3 sdir = sampleSunDir(rng);
            if (dot(N, sdir) > 0.0f) {
                RayDesc sr;
                sr.Origin    = wsP;
                sr.Direction = sdir;
                sr.TMin      = 1e-3f;
                sr.TMax      = 1e30f;
                ShadowPayload sp; sp.visible = 0;
                // hgOffset=1 (shadow), hgStride=2, missIdx=1
                TraceRay(tlas,
                         RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH |
                         RAY_FLAG_SKIP_CLOSEST_HIT_SHADER,
                         0xFF, 1, 2, 1, sr, sp);
                if (sp.visible) {
                    float3 sunLight = frameConstants.sunColor * PI;
                    float3 contrib  = throughput * evalBRDF(N, V, sdir, p.albedo, p.F0, p.roughness) * sunLight;
                    radiance += contrib;
                    radiance  = min(radiance, FIREFLY_CLAMP);
                }
            }
        }

        // --- Sample BRDF for next bounce ---
        float3 wi;
        float3 weight = sampleBRDF(N, V, p.albedo, p.F0, p.roughness, rng, wi);
        if (dot(wi, N) <= 0.0f || !any(weight > 0.0f)) break;

        throughput *= weight;
        throughput  = min(throughput, FIREFLY_CLAMP);

        // Russian Roulette (start after bounce 2)
        if (bounce >= 2) {
            float q = clamp(max(throughput.r, max(throughput.g, throughput.b)), 0.01f, 0.95f);
            if (rngF(rng) > q) break;
            throughput /= q;
        }

        ray.Origin    = wsP;
        ray.Direction = wi;
        ray.TMin      = 1e-3f;
        ray.TMax      = 1e30f;
    }

    // --- Progressive accumulation ---
    float3 accumulated;
    if (pc.resetAccum != 0u || pc.accumCount == 0u) {
        accumulated = radiance;
    } else {
        float3 prev = outAccumColor[px].rgb;
        float  t    = 1.0f / float(pc.accumCount + 1u);
        accumulated = lerp(prev, radiance, t);
    }
    outAccumColor[px] = float4(accumulated, 1.0f);

    // Tone map to display image
    outLitColor[px] = float4(aces(accumulated), 1.0f);
}

[shader("miss")]
void MissPrimary(inout PrimaryPayload p) {
    p.hit = false;
}

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
