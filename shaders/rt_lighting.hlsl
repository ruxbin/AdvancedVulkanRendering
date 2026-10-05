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

#include "rt_path_common.hlsl"

// --- Bindings ---
[[vk::binding(1,1)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4>             outLitColor;   // tone-mapped display output
[[vk::binding(12,1)]] RWTexture2D<float4> outAccumColor;  // persistent accumulation buffer

// Per-dispatch knobs — must stay exactly 32 bytes (matches Raytracing.cpp RTPC).
struct RTPushConsts {
    uint  pointLightCount;
    uint  accumCount;       // samples accumulated so far (0 = first sample)
    float sunConeRadius;    // tan(sun angular half-radius), e.g. tan(0.5deg)
    float pixelSpreadAngle; // 2*tan(fovY/2)/screenHeight
    uint  frameSeed;        // per-frame jitter seed
    uint  maxBounces;       // max path depth
    uint  resetAccum;       // 1 = camera moved, start fresh
    uint  spotLightCount;
};
[[vk::push_constant]] RTPushConsts pc;

// --- Constants ---
#define FIREFLY_CLAMP  20.0f

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
            float3 sdir = sampleSunDir(rng, pc.sunConeRadius);
            if (dot(N, sdir) > 0.0f) {
                if (traceShadowRay(wsP, sdir, 1e30f) > 0.0f) {
                    float3 sunLight = frameConstants.sunColor * PI;
                    float3 contrib  = throughput * evalBRDF(N, V, sdir, p.albedo, p.F0, p.roughness) * sunLight;
                    radiance += contrib;
                    radiance  = min(radiance, FIREFLY_CLAMP);
                }
            }
        }

        // --- NEE: point + spot lights (only at primary hit to keep cost
        // bounded; secondary bounces still get sun NEE + emissive). ---
        if (bounce == 0) {
            float3 localContrib = evalLocalLightsNEE(wsP, N, V,
                                                     p.albedo, p.F0, p.roughness,
                                                     pc.pointLightCount, pc.spotLightCount);
            radiance += throughput * localContrib;
            radiance  = min(radiance, FIREFLY_CLAMP);
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
