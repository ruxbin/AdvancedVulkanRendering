// ibl.hlsl — IBL 运行时生成内核(Vulkan only,DX12 批处理不编译本文件)。
//
// equirect 方向约定(必须与 Src/SphericalHarmonics.h 注释逐字一致):
//   u = atan2(dir.x, dir.z) / (2π) + 0.5
//   v = acos(dir.y) / π            (v=0 端 = +Y)
// 逆映射:φ=(u-0.5)*2π,θ=v*π;dir=(sinθ sinφ, cosθ, sinθ cosφ)
#include "shadercompat.hlsl"

static const float IBL_PI = 3.1415926535897932f;

float2 DirectionToEquirectUV(float3 d)
{
    float phi = atan2(d.x, d.z);
    float theta = acos(clamp(d.y, -1.0, 1.0));
    return float2(phi / (2.0 * IBL_PI) + 0.5, theta / IBL_PI);
}

// Vulkan/GL cube 面约定:face == array layer(0:+X 1:-X 2:+Y 3:-Y 4:+Z 5:-Z)。
// uv01: 面内 [0,1] 坐标,y 向下(与 storage 写坐标一致)。
float3 CubeFaceDirection(uint face, float2 uv01)
{
    float sc = uv01.x * 2.0 - 1.0;
    float tc = uv01.y * 2.0 - 1.0;
    switch (face)
    {
    case 0:  return normalize(float3( 1.0, -tc, -sc));
    case 1:  return normalize(float3(-1.0, -tc,  sc));
    case 2:  return normalize(float3( sc,  1.0,  tc));
    case 3:  return normalize(float3( sc, -1.0, -tc));
    case 4:  return normalize(float3( sc, -tc,  1.0));
    default: return normalize(float3(-sc, -tc, -1.0));
    }
}

VK_BINDING(0,0) Texture2D<float4> equirectTex;
VK_BINDING(1,0) SamplerState iblLinearSampler;
VK_BINDING(2,0) RWTexture2DArray<float4> outputFace;

// CS1/CS2 共用一个 push constant block(dxc 每个编译单元只允许一个 [[vk::push_constant]])。
// CS1 只用 mipSize;CS2 用 mipSize + roughness。两者 C++ 侧都是 16 字节。
struct IblPushConstants { uint mipSize; float roughness; uint _pad0; uint _pad1; };
DECLARE_PUSH_CONSTANTS(IblPushConstants, iblPc, 0);

[numthreads(8, 8, 1)]
void EquirectToCubeCS(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= iblPc.mipSize || tid.y >= iblPc.mipSize || tid.z >= 6) return;
    float2 uv = (tid.xy + 0.5) / (float)iblPc.mipSize;
    float3 dir = CubeFaceDirection(tid.z, uv);
    float3 color = equirectTex.SampleLevel(iblLinearSampler, DirectionToEquirectUV(dir), 0).rgb;
    // R16G16B16A16_SFLOAT 上限 65504;超高动态像素(太阳)clamp 防 inf(Review Focus #2)
    outputFace[tid] = float4(min(color, 60000.0), 1.0);
}

// ---- 预过滤(GGX 重要性采样,split-sum 第一步;Epic/Karis) ----
float IBL_RadicalInverse(uint bits)
{
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return float(bits) * 2.3283064365386963e-10;
}

float2 IBL_Hammersley(uint i, uint n)
{
    return float2(float(i) / float(n), IBL_RadicalInverse(i));
}

float3 IBL_ImportanceSampleGGX(float2 xi, float3 n, float roughness)
{
    float a = roughness * roughness;
    float phi = 2.0 * IBL_PI * xi.x;
    float cosTheta = sqrt((1.0 - xi.y) / (1.0 + (a * a - 1.0) * xi.y));
    float sinTheta = sqrt(1.0 - cosTheta * cosTheta);
    float3 h = float3(cos(phi) * sinTheta, sin(phi) * sinTheta, cosTheta);
    float3 up = abs(n.z) < 0.999 ? float3(0, 0, 1) : float3(1, 0, 0);
    float3 tx = normalize(cross(up, n));
    float3 ty = cross(n, tx);
    return normalize(tx * h.x + ty * h.y + n * h.z);
}

// CS2 输入:整个 env cube(采样 mip0);与 CS1 不同,CS2 set layout 只含 binding 1/2/3
VK_BINDING(3,0) TextureCube prefilterInput;

[numthreads(8, 8, 1)]
void PrefilterSpecularCS(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= iblPc.mipSize || tid.y >= iblPc.mipSize || tid.z >= 6) return;
    float2 uv = (tid.xy + 0.5) / (float)iblPc.mipSize;
    float3 N = CubeFaceDirection(tid.z, uv);
    float3 V = N;

    const uint SAMPLE_COUNT = 128;
    float totalWeight = 0.0;
    float3 prefiltered = 0.0;
    for (uint i = 0; i < SAMPLE_COUNT; ++i)
    {
        float2 xi = IBL_Hammersley(i, SAMPLE_COUNT);
        float3 H = IBL_ImportanceSampleGGX(xi, N, iblPc.roughness);
        float3 L = 2.0 * dot(V, H) * H - V;
        float ndl = saturate(dot(N, L));
        if (ndl > 0.0)
        {
            prefiltered += prefilterInput.SampleLevel(iblLinearSampler, L, 0).rgb * ndl;
            totalWeight += ndl;
        }
    }
    outputFace[tid] = float4(prefiltered / max(totalWeight, 1e-4), 1.0);
}
