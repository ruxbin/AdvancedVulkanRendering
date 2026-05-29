#include "commonstruct.hlsl"

// Spot-light shadow-map vertex shader.
// Reads the combined view-proj matrix from a push constant (64 bytes).
// Uses SV_InstanceID directly as the global chunk index (firstInstance = chunk index
// in the caller's vkCmdDrawIndexed, no chunkIndex[] indirection needed).

struct SpotShadowPushConstants {
    float4x4 viewProj;
};

[[vk::push_constant]] SpotShadowPushConstants push;

// applSetLayout binding 3 – needed so the FS (RenderSceneShadowDepthIndirect)
// can read meshChunks[chunkid].materialIndex for alpha-mask tests.
[[vk::binding(3,1)]] StructuredBuffer<AAPLMeshChunk> meshChunks;

struct VSInput {
    [[vk::location(0)]] float3 position : POSITION;
    [[vk::location(1)]] float3 normal   : NORMAL;
    [[vk::location(2)]] float3 tangent  : Tangent;
    [[vk::location(3)]] float2 uv       : TEXCOORD0;
    uint instancid : SV_InstanceID;
};

struct VSOutput {
    float4 Position   : SV_POSITION;
    float2 TextureUV  : TEXCOORD0;
    uint   chunkid    : TEXCOORD1;
    half3  viewDir    : TEXCOORD2;
    half3  normal     : TEXCOORD3;
    half3  tangent    : TEXCOORD4;
    float4 wsPosition : TEXCOORD5;
};

VSOutput RenderSceneVSShadowSpot(VSInput input)
{
    VSOutput Output;
    Output.Position   = mul(push.viewProj, float4(input.position, 1.0));
    Output.TextureUV  = input.uv;
    Output.chunkid    = input.instancid;   // direct global chunk index
    Output.viewDir    = (half3)0;
    Output.normal     = (half3)0;
    Output.tangent    = (half3)0;
    Output.wsPosition = float4(input.position, 1.0);
    return Output;
}
