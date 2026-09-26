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

struct EquirectToCubeParams { uint mipSize; uint _pad0; uint _pad1; uint _pad2; };
DECLARE_PUSH_CONSTANTS(EquirectToCubeParams, epc, 0);

[numthreads(8, 8, 1)]
void EquirectToCubeCS(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= epc.mipSize || tid.y >= epc.mipSize || tid.z >= 6) return;
    float2 uv = (tid.xy + 0.5) / (float)epc.mipSize;
    float3 dir = CubeFaceDirection(tid.z, uv);
    float3 color = equirectTex.SampleLevel(iblLinearSampler, DirectionToEquirectUV(dir), 0).rgb;
    // R16G16B16A16_SFLOAT 上限 65504;超高动态像素(太阳)clamp 防 inf(Review Focus #2)
    outputFace[tid] = float4(min(color, 60000.0), 1.0);
}
