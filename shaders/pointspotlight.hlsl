#include "commonstruct.hlsl"
#include "lighting.hlsl"



[[vk::binding(0,0)]] Texture2D<float4> albedoeTex;
[[vk::binding(1,0)]] Texture2D<float4> normalTex;
[[vk::binding(2,0)]] Texture2D<float4> emissiveTex;
[[vk::binding(3,0)]] Texture2D<float4> F0RoughnessTex;
[[vk::binding(4,0)]] Texture2D<float> inDepth;
[[vk::binding(5,0)]] SamplerState _NearestClampSampler;

[[vk::binding(6,0)]] 
cbuffer frameData
{
    CameraParamsBufferFull cameraParams;
    AAPLFrameConstants frameConstants;
}

[[vk::binding(7,0)]]
cbuffer pointLightData
{
    float4 posSqrRadius;
    float3 color;
}

struct VSInput
{
    [[vk::location(0)]] float3 position : POSITION;
};

struct VSOutput
{
    float4 Position : SV_POSITION;
    float2 uv : TEXCOORD0;
};


VSOutput RenderSceneVS(VSInput input)
{
    VSOutput Output;
    float radius = sqrt(posSqrRadius.w);
    float4x4 objectToWorldMatrix =
    {
        radius, 0, 0, posSqrRadius.x,
        0, radius, 0, posSqrRadius.y,
        0, 0, radius, posSqrRadius.z,
        0, 0, 0, 1
            
      };//row-major
    float4x4 finalMatrix = mul(cameraParams.projectionMatrix, cameraParams.viewMatrix);
    
    Output.Position = mul(finalMatrix, mul(objectToWorldMatrix, float4(input.position, 1.0)));
        Output.uv = Output.Position.xy / Output.Position.w * 0.5 + 0.5;
    return Output;
}



half4 DeferredLighting(VSOutput input) : SV_Target
{
    half4 albedoSample;
    half4 normalSample;
    half4 emissiveSample;
    half4 F0RoughnessSample;
    
    float2 uv = input.uv;
    
    albedoSample = albedoeTex.SampleLevel(_NearestClampSampler, uv, 0);
    normalSample = normalTex.SampleLevel(_NearestClampSampler, uv, 0);
    emissiveSample = emissiveTex.SampleLevel(_NearestClampSampler, uv, 0);
    F0RoughnessSample = F0RoughnessTex.SampleLevel(_NearestClampSampler, uv, 0);
    
    AAPLPixelSurfaceData surfaceData;
    surfaceData.normal = (half3) normalize((float3) normalSample.xyz); // normalizing half3 normal causes banding
    surfaceData.albedo = albedoSample.xyz;
    surfaceData.F0 = lerp(F0RoughnessSample.xyz, (half) 0.02, (half) frameConstants.wetness);
    surfaceData.roughness = lerp(F0RoughnessSample.w, (half) 0.1, (half) frameConstants.wetness);
    surfaceData.alpha = 1.0f;
    surfaceData.emissive = emissiveSample.rgb;
    
    
    float depth = inDepth.SampleLevel(_NearestClampSampler, uv, 0);
    float4 worldPosition = worldPositionForTexcoord(uv, depth, cameraParams);
    
    half3 result = lightingShaderPointSpot(surfaceData, depth, worldPosition, frameConstants, cameraParams,posSqrRadius,color);
    
    return half4(result, 1.f);
}
