#include "commonstruct.hlsl"

[[vk::binding(0, 0)]]
cbuffer cam {
    CameraParamsBufferFull cameraParams;
    AAPLFrameConstants frameConstants;
}

[[vk::binding(0, 1)]] Texture2D<float> depthTex;
[[vk::binding(1, 1)]] SamplerState nearestSampler;

struct DecalData {
    float4x4 worldToLocal;
    float4x4 localToWorld;
    float4 albedoTint;
    uint hasTexture;
    uint _pad0, _pad1, _pad2;
};

// SSBO array of all decals (CPU updates once per frame, no race condition).
[[vk::binding(2, 1)]] StructuredBuffer<DecalData> decalArray;

[[vk::binding(3, 1)]] Texture2D<float4> decalAlbedoTex;

// Push constant: which decal in the array this draw refers to.
struct DecalPushConsts { uint decalIndex; };
[[vk::push_constant]] DecalPushConsts pc;

struct VSInput {
    float3 position : POSITION;
};

struct VSOutput {
    float4 position : SV_POSITION;
    float3 localPos : TEXCOORD0;
};

VSOutput DecalVS(VSInput input) {
    DecalData d = decalArray[pc.decalIndex];
    VSOutput output;
    float4 worldPos = mul(d.localToWorld, float4(input.position, 1.0));
    output.position = mul(cameraParams.projectionMatrix, mul(cameraParams.viewMatrix, worldPos));
    output.localPos = input.position;
    return output;
}

half4 DecalPS(VSOutput input) : SV_Target0
{
    DecalData d = decalArray[pc.decalIndex];

    float2 texCoord = input.position.xy / frameConstants.physicalSize;
    float depth = depthTex.SampleLevel(nearestSampler, texCoord, 0);

    float4 worldPosition = worldPositionForTexcoord(texCoord, depth, cameraParams,true);
    float4 localPos = mul(d.worldToLocal, float4(worldPosition.xyz, 1.0));
    localPos /= localPos.w;

    if (any(abs(localPos) > 1.0))
        discard;

    half4 albedo;
    if (d.hasTexture) {
        float2 uv = localPos.xy * 0.5 + 0.5;
        albedo = (half4)decalAlbedoTex.SampleLevel(nearestSampler, uv, 0);
        albedo.rgb *= d.albedoTint.rgb;
    } else {
        albedo = half4(d.albedoTint.rgb, d.albedoTint.a);
    }

    return half4(albedo.rgb, albedo.a * d.albedoTint.a);
}
