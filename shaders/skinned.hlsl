#include "commonstruct.hlsl"
#include "lighting.hlsl"

// Set 0: global (camera + frame constants, shared with existing pipeline)
[[vk::binding(0,0)]]
cbuffer cam
{
    CameraParamsBufferFull cameraParams;
    AAPLFrameConstants frameConstants;
}

// Set 1: skinned mesh resources
[[vk::binding(0,1)]] StructuredBuffer<float4x4> boneMatrices;
[[vk::binding(1,1)]] SamplerState _LinearRepeatSampler;
[[vk::binding(2,1)]] Texture2D<half4> diffuseTexture;

struct SkinnedPushConstants
{
    float4x4 worldMatrix;
    uint materialFlags; // bit 0: has diffuse texture
};

[[vk::push_constant]] SkinnedPushConstants pushConstants;

struct SkinnedVSInput
{
    [[vk::location(0)]] float3 position : POSITION;
    [[vk::location(1)]] float3 normal   : NORMAL;
    [[vk::location(2)]] float3 tangent  : TANGENT;
    [[vk::location(3)]] float2 uv       : TEXCOORD0;
    [[vk::location(4)]] uint4  boneIDs  : BLENDINDICES;
    [[vk::location(5)]] float4 boneWeights : BLENDWEIGHT;
};

struct SkinnedVSOutput
{
    float4 Position   : SV_POSITION;
    float2 TextureUV  : TEXCOORD0;
    half3  viewDir    : TEXCOORD2;
    half3  normal     : TEXCOORD3;
    half3  tangent    : TEXCOORD4;
    float4 wsPosition : TEXCOORD5;
};

struct PSOutput
{
    half4 albedo      : SV_Target0;
    half4 normals     : SV_Target1;
    half4 emissive    : SV_Target2;
    half4 F0Roughness : SV_Target3;
};

SkinnedVSOutput SkinnedVS(SkinnedVSInput input)
{
    SkinnedVSOutput output;

    // Compute skinned position via bone matrix palette
    float4x4 skinMatrix =
        boneMatrices[input.boneIDs.x] * input.boneWeights.x +
        boneMatrices[input.boneIDs.y] * input.boneWeights.y +
        boneMatrices[input.boneIDs.z] * input.boneWeights.z +
        boneMatrices[input.boneIDs.w] * input.boneWeights.w;

    float4 skinnedPos = mul(skinMatrix, float4(input.position, 1.0));
    float3 skinnedNormal = normalize(mul((float3x3)skinMatrix, input.normal));
    float3 skinnedTangent = normalize(mul((float3x3)skinMatrix, input.tangent));

    // Apply world transform
    float4 worldPos = mul(pushConstants.worldMatrix, skinnedPos);

    float4x4 viewProj = mul(cameraParams.projectionMatrix, cameraParams.viewMatrix);
    output.Position = mul(viewProj, worldPos);
    output.TextureUV = input.uv;
    output.wsPosition = worldPos;

    // World-space normal/tangent (world matrix assumed uniform scale for now)
    float3x3 worldMat3 = (float3x3)pushConstants.worldMatrix;
    output.normal = (half3)normalize(mul(worldMat3, skinnedNormal));
    output.tangent = (half3)normalize(mul(worldMat3, skinnedTangent));

    // View direction
    float3 camPos = float3(cameraParams.invViewMatrix._m03,
                           cameraParams.invViewMatrix._m13,
                           cameraParams.invViewMatrix._m23);
    output.viewDir = (half3)normalize(camPos - worldPos.xyz);

    return output;
}

// Base pass: write to GBuffer (deferred)
PSOutput SkinnedBasePS(SkinnedVSOutput input)
{
    PSOutput output;

    half4 baseColor = (half4)1.0;
    if (pushConstants.materialFlags & 1u)
    {
        baseColor = diffuseTexture.Sample(_LinearRepeatSampler, input.TextureUV);
    }

    half3 normal = normalize(input.normal);

    output.albedo      = half4(baseColor.rgb, baseColor.a);
    output.normals     = half4(normal, 0.0);
    output.emissive    = (half4)0;
    output.F0Roughness = half4((half3)0.04, (half)0.5);

    return output;
}

// Forward pass: evaluate lighting directly
half4 SkinnedForwardPS(SkinnedVSOutput input) : SV_Target
{
    half4 baseColor = (half4)1.0;
    if (pushConstants.materialFlags & 1u)
    {
        baseColor = diffuseTexture.Sample(_LinearRepeatSampler, input.TextureUV);
    }

    AAPLPixelSurfaceData surfaceData;
    surfaceData.normal = normalize(input.normal);
    surfaceData.albedo = baseColor.rgb;
    surfaceData.F0 = (half3)0.04;
    surfaceData.roughness = (half)0.5;
    surfaceData.alpha = baseColor.a;
    surfaceData.emissive = (half3)0;

    half3 res = lightingShader(surfaceData, 0, input.wsPosition,
                               frameConstants, cameraParams);
    return half4(res, surfaceData.alpha);
}
