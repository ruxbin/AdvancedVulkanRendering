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

[[vk::binding(2, 1)]] cbuffer decalBuf {
    DecalData decalData;
}

[[vk::binding(3, 1)]] Texture2D<float4> decalAlbedoTex;

struct VSInput {
    float3 position : POSITION;
};

struct VSOutput {
    float4 position : SV_POSITION;
    float3 localPos : TEXCOORD0;
};

VSOutput DecalVS(VSInput input) {
    VSOutput output;
    float4 worldPos = mul(decalData.localToWorld, float4(input.position, 1.0));
    output.position = mul(cameraParams.projectionMatrix, mul(cameraParams.viewMatrix, worldPos));
    output.localPos = input.position;
    return output;
}

half4 DecalPS(VSOutput input) : SV_Target0
{
    float2 texCoord = input.position.xy / frameConstants.physicalSize;
    float depth = depthTex.SampleLevel(nearestSampler, texCoord, 0);

    float4 worldPosition = worldPositionForTexcoord(texCoord, depth, cameraParams);
    float3 localPos = mul(decalData.worldToLocal, float4(worldPosition.xyz, 1.0)).xyz;

    if (any(abs(localPos) > 1.0))
        discard;

    half4 albedo;
    if (decalData.hasTexture) {
        float2 uv = localPos.xy * 0.5 + 0.5;
        albedo = (half4)decalAlbedoTex.SampleLevel(nearestSampler, uv, 0);
        albedo.rgb *= decalData.albedoTint.rgb;
    } else {
        albedo = half4(decalData.albedoTint.rgb, decalData.albedoTint.a);
    }

    return half4(albedo.rgb, albedo.a * decalData.albedoTint.a);
}
