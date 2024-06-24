#include "commonstruct.hlsl"
#include "lighting.hlsl"



[[vk::binding(0,0)]] Texture2D<half4> albedoeTex;
[[vk::binding(1,0)]] Texture2D<half4> normalTex;
[[vk::binding(2,0)]] Texture2D<half4> emissiveTex;
[[vk::binding(3,0)]] Texture2D<half4> F0RoughnessTex;
[[vk::binding(4,0)]] Texture2D<half> inDepth;
[[vk::binding(5,0)]] SamplerState _NearestClampSampler;

[[vk::binding(6,0)]] 
cbuffer frameData
{
    AAPLFrameConstants frameConstants;
    CameraParamsBuffer cameraParams;
}

struct VSOutput
{
    float4 Position : SV_POSITION;
   
    float2 TextureUV : TEXCOORD0;
};



float4 worldPositionForTexcoord(float2 texCoord, float depth, CameraParamsBuffer cameraParams)
{
    float4 ndc;
    ndc.xy = texCoord.xy * 2 - 1;
    //ndc.y *= -1;
    ndc.z =depth;
    ndc.w = 1;

    float4 worldPosition = mul(cameraParams.invViewProjectionMatrix, ndc);
    worldPosition.xyz /= worldPosition.w;
    return  worldPosition;
}






VSOutput AAPLSimpleTexVertexOutFSQuadVertexShader(
uint vid : SV_VertexID)
{
    VSOutput output;
    output.TextureUV = float2((vid << 1) & 2, vid & 2);
    output.Position = float4(output.TextureUV * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 1.0f, 1.0f);
    output.TextureUV = float2(output.TextureUV.x, output.TextureUV.y * -1 + 1);
    return output;
}

half4 DeferredLighting(VSOutput input) : SV_Target
{
    
    
    half4 albedoSample;
    half4 normalSample;
    half4 emissiveSample;
    half4 F0RoughnessSample;
    
    albedoSample = albedoeTex.SampleLevel(_NearestClampSampler, input.TextureUV,0);
    normalSample = normalTex.SampleLevel(_NearestClampSampler, input.TextureUV,0);
    emissiveSample = emissiveTex.SampleLevel(_NearestClampSampler, input.TextureUV,0);
    F0RoughnessSample = F0RoughnessTex.SampleLevel(_NearestClampSampler, input.TextureUV,0);
    
    
    AAPLPixelSurfaceData surfaceData;
    surfaceData.normal = (half3) normalize((float3) normalSample.xyz); // normalizing half3 normal causes banding
    surfaceData.albedo = albedoSample.xyz;
    surfaceData.F0 = lerp(F0RoughnessSample.xyz, (half) 0.02, (half) frameConstants.wetness);
    surfaceData.roughness = lerp(F0RoughnessSample.w, (half) 0.1, (half) frameConstants.wetness);
    surfaceData.alpha = 1.0f;
    surfaceData.emissive = emissiveSample.rgb;
    
    
    float depth = inDepth.SampleLevel(_NearestClampSampler, input.TextureUV, 0);
    float4 worldPosition = worldPositionForTexcoord(input.TextureUV, depth, cameraParams);
    
    
    half3 result = lightingShader(surfaceData, depth, worldPosition, frameConstants, cameraParams);
    
    return half4(result, 1.f);
    
}