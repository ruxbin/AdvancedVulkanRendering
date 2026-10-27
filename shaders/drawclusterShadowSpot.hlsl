#include "shadercompat.hlsl"
#include "commonstruct.hlsl"

// Spot-light shadow-map vertex shader.
// Reads the combined view-proj matrix from a push constant (64 bytes).
// Vulkan: SV_InstanceID/gl_InstanceIndex includes firstInstance, which the
// caller sets to the global chunk index — no chunkIndex[] indirection needed.
// DX12: SV_InstanceID is 0-based per draw and ignores StartInstanceLocation,
// so the chunk index is delivered by the [CONSTANT(b2), DRAW_INDEXED] command
// signature instead: it patches push.chunkId per draw (same trick as the CSM
// shadow path's drawSlot).

struct SpotShadowPushConstants {
    float4x4 viewProj;
#ifdef DX12_BACKEND
    uint chunkId;   // patched per-draw by the command signature (root const b2)
#endif
};

DECLARE_PUSH_CONSTANTS(SpotShadowPushConstants, push, 2);

// applSetLayout binding 3 – needed so the FS (RenderSceneShadowDepthIndirect)
// can read meshChunks[chunkid].materialIndex for alpha-mask tests.
VK_BINDING(3,1) StructuredBuffer<AAPLMeshChunk> meshChunks REGISTER_SRV(3,1);

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
#ifdef DX12_BACKEND
    Output.chunkid    = push.chunkId;      // patched by command signature
#else
    Output.chunkid    = input.instancid;   // direct global chunk index
#endif
    Output.viewDir    = (half3)0;
    Output.normal     = (half3)0;
    Output.tangent    = (half3)0;
    Output.wsPosition = float4(input.position, 1.0);
    return Output;
}
