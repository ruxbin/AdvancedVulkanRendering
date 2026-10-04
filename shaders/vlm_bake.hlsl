// vlm_bake.hlsl — VLM 探针离线烘焙(规格 §6/§7)。
// 每 lane 一个 probe:批量低差异方向 × 迭代路径追踪,首段方向做 SH9 投影,
// FP32 累加进 shAccum[probe*27 .. +27)(lane 独占 probe,无需原子)。
// 与相机 PT 的差异(规格 §2/§6):
//   - 首段为均匀球面低差异序列(pdf=1/(4π)),探针无接收法线,不用余弦半球;
//   - miss 采样原始线性 HDR equirect(非程序化天空);
//   - 局部灯 NEE 在所有命中执行(相机 PT 仅 bounce==0);
//   - 不继承 FIREFLY_CLAMP;NaN/Inf 置 errorFlags 并丢弃样本,CPU 判整批失败;
//   - 无累积平均:GPU 只存 Σ,均值由 CPU finalize。
//
// Keep VlmBakePushConsts in sync with Src/VlmBaker.cpp VlmBakePC struct (112 bytes).
//
// Compile with(与 rt_lighting 同参数):
//   dxc -spirv -T lib_6_3 vlm_bake.hlsl -fspv-target-env=vulkan1.2
//        -fspv-extension=SPV_KHR_ray_tracing -fspv-extension=SPV_KHR_non_semantic_info
//        -fspv-extension=SPV_EXT_descriptor_indexing -Fo vlm_bake.lib.spv

#include "rt_path_common.hlsl"

// --- VLM 自有绑定(set1 追加)---
[[vk::binding(14,1)]] Texture2D<float4>       envTex;
[[vk::binding(15,1)]] SamplerState            envSampler;
[[vk::binding(16,1)]] RWStructuredBuffer<float> shAccum;    // probeCount × 27
[[vk::binding(17,1)]] RWStructuredBuffer<uint>  errorFlags; // 1 uint

// --- Push constants(112 B,与 C++ VlmBakePC 逐字段一致)---
struct VlmBakePushConsts {
    uint  probeCount; uint  samplesThisBatch; uint  batchSeed; uint  maxBounces; // 0..15
    uint  pointLightCount; uint spotLightCount; uint flags; uint cellsX;         // 16..31
    uint  cellsY; uint cellsZ; float sunConeRadius; float sunScale;              // 32..47
    float envScale; float localLightScale; float pad0; float pad1;               // 48..63
    float3 constEnvRGB; float pad2;   // 64..79
    float3 bmin; float pad3;          // 80..95
    float3 step; float pad4;          // 96..111
};
[[vk::push_constant]] VlmBakePushConsts pc;

// flags 位(与 C++ 一致)
#define VLM_BAKE_SKY_ONLY          1u
#define VLM_BAKE_CONST_ENV         2u
#define VLM_BAKE_SUN_IS_ENVIRONMENT 4u

// 方向→equirect uv:必须与 Src/SphericalHarmonics.h 注释及 ibl.hlsl:14-19 逐字一致
// (-d.x 方位角镜像是对 Apple 烘焙 KTX 的实证校准)。
float2 DirectionToEquirectUV(float3 d)
{
    float phi = atan2(-d.x, d.z);
    float theta = acos(clamp(d.y, -1.0, 1.0));
    return float2(phi / (2.0 * PI) + 0.5, theta / PI);
}

float3 envRadiance(float3 dir) {
    float3 e = (pc.flags & VLM_BAKE_CONST_ENV)
        ? pc.constEnvRGB
        : envTex.SampleLevel(envSampler, DirectionToEquirectUV(dir), 0).rgb;
    return e * pc.envScale;
}

// Fibonacci 球面 + Cranley-Patterson 旋转(按 probeId/batchId 扰乱,规格 §6)。
// 与 Tests/VlmShTests.cpp 的 FibonacciDir 同基序列;均匀球面 pdf 恒 1/(4π)。
// CP 旋转:r1/r2 每个 (probe,batch) 只抽一次、整批共享同一随机平移,保持
// 低差异序列的分层性质(逐样本各自抖动会退化成 √N 白噪声,常量天空下
// 4096 样本的 maxRelErr 会从 ~1e-3 恶化到 ~7e-2)。
float3 sampleSphereDir(uint s, uint n, float r1, float r2) {
    float u = (float(s) + r1) / float(n);
    float z = 1.0f - 2.0f * u;
    float phi = TWO_PI * frac(float(s) * 0.61803398875f + r2);
    float r = sqrt(max(0.0f, 1.0f - z * z));
    return float3(r * cos(phi), z, r * sin(phi));
}

// 物理 SH 基(kY 已折入),与 EvalSh9Basis(SphericalHarmonics.h)逐字一致。
void evalSh9Basis(float3 d, out float b[9]) {
    b[0] = 0.282095f;
    b[1] = 0.488603f * d.y;
    b[2] = 0.488603f * d.z;
    b[3] = 0.488603f * d.x;
    b[4] = 1.092548f * d.y * d.x;
    b[5] = 1.092548f * d.y * d.z;
    b[6] = 0.315392f * (3.0f * d.z * d.z - 1.0f);
    b[7] = 1.092548f * d.z * d.x;
    b[8] = 0.546274f * (d.x * d.x - d.y * d.y);
}

float3 probeWorldPos(uint probeIdx) {
    uint PX = pc.cellsX + 1u;
    uint PY = pc.cellsY + 1u;
    uint x = probeIdx % PX;
    uint y = (probeIdx / PX) % PY;
    uint z = probeIdx / (PX * PY);
    return pc.bmin + pc.step * float3(float(x), float(y), float(z));
}

[shader("raygeneration")]
void VlmProbeRayGen() {
    uint probeIdx = DispatchRaysIndex().x;
    if (probeIdx >= pc.probeCount) return;

    float3 probePos = probeWorldPos(probeIdx);
    uint rng = (probeIdx * 2654435761u) ^ (pc.batchSeed * 805459861u);
    pcgNext(rng); pcgNext(rng); // warm up

    const float invPdf = 4.0f * PI; // 1/pdf,pdf=1/(4π)
    // Cranley-Patterson 旋转量:每个 (probe,batch) 一对,整批样本共享。
    const float cpR1 = rngF(rng);
    const float cpR2 = rngF(rng);
    float3 acc[9];
    [unroll] for (int j = 0; j < 9; ++j) acc[j] = (float3)0;

    for (uint s = 0; s < pc.samplesThisBatch; ++s) {
        float3 dir0 = sampleSphereDir(s, pc.samplesThisBatch, cpR1, cpR2);
        float3 Li;

        if (pc.flags & VLM_BAKE_SKY_ONLY) {
            // 无几何模式(常量/方向验收):不追射线,直接采样环境。
            Li = envRadiance(dir0);
        } else {
            Li = (float3)0;
            float3 throughput = float3(1.0f, 1.0f, 1.0f);

            RayDesc ray;
            ray.Origin    = probePos;
            ray.Direction = dir0;
            ray.TMin      = 1e-3f;
            ray.TMax      = 1e30f;

            for (uint bounce = 0; bounce < max(1u, pc.maxBounces); ++bounce) {
                PrimaryPayload p;
                p.hit = false; p.wsPos = 0; p.normal = 0; p.albedo = 0;
                p.F0 = 0.04f; p.roughness = 1.0f; p.emissive = 0; p.alpha = 1; p.hitT = 0;

                TraceRay(tlas, RAY_FLAG_CULL_BACK_FACING_TRIANGLES, 0xFF, 0, 2, 0, ray, p);

                if (!p.hit) {
                    Li += throughput * envRadiance(ray.Direction);
                    break;
                }

                // 自发光出射(规格 §3:命中发光表面记录其出射 emission)
                Li += throughput * p.emissive * frameConstants.emissiveScale;

                float3 N = normalize(p.normal);
                float3 V = -ray.Direction;
                if (dot(N, V) < 0.0f) N = -N;
                float3 wsP = p.wsPos + N * 2e-3f;

                // 解析太阳 NEE(sunIsEnvironment 时跳过,避免与环境太阳盘双重计能,规格 §3)
                if ((pc.flags & VLM_BAKE_SUN_IS_ENVIRONMENT) == 0u) {
                    float3 sdir = sampleSunDir(rng, pc.sunConeRadius);
                    if (dot(N, sdir) > 0.0f &&
                        traceShadowRay(wsP, sdir, 1e30f) > 0.0f) {
                        Li += throughput *
                              evalBRDF(N, V, sdir, p.albedo, p.F0, p.roughness) *
                              (frameConstants.sunColor * PI) * pc.sunScale;
                    }
                }

                // 局部灯 NEE:所有命中都执行(规格 §6;相机 PT 仅 bounce==0)
                Li += throughput *
                      evalLocalLightsNEE(wsP, N, V, p.albedo, p.F0, p.roughness,
                                         pc.pointLightCount, pc.spotLightCount) *
                      pc.localLightScale;

                // BSDF 采样下一跳(完整混合 pdf,与返回权重一致)
                float3 wi;
                float3 weight = sampleBRDF(N, V, p.albedo, p.F0, p.roughness, rng, wi);
                if (dot(wi, N) <= 0.0f || !any(weight > 0.0f)) break;

                throughput *= weight;

                // Russian Roulette(第三次表面散射后,与相机 PT 同阈值)
                if (bounce >= 2) {
                    float q = clamp(max(throughput.r, max(throughput.g, throughput.b)), 0.01f, 0.95f);
                    if (rngF(rng) > q) break;
                    throughput /= q;
                }

                ray.Origin    = wsP;
                ray.Direction = wi;
            }
        }

        // NaN/Inf:置错误标志并丢弃样本;CPU 判整批失败(规格 §6,不当零样本计入)
        if (!(isfinite(Li.x) && isfinite(Li.y) && isfinite(Li.z))) {
            InterlockedOr(errorFlags[0], 1u);
            continue;
        }

        // 首段方向 SH 投影:acc_j += Li · Y_j(dir0) / pdf
        float basis[9];
        evalSh9Basis(dir0, basis);
        [unroll] for (int j = 0; j < 9; ++j) acc[j] += Li * (basis[j] * invPdf);
    }

    // lane 独占 probe,跨 dispatch 由 host 端 barrier 保证顺序,直接 RMW。
    uint base = probeIdx * 27u;
    [unroll] for (int j = 0; j < 9; ++j) {
        shAccum[base + j * 3 + 0] += acc[j].x;
        shAccum[base + j * 3 + 1] += acc[j].y;
        shAccum[base + j * 3 + 2] += acc[j].z;
    }
}

[shader("miss")]
void VlmMissPrimary(inout PrimaryPayload p) {
    p.hit = false;
}
