#include "commonstruct.hlsl"


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

#define M_PI_F 3.1415926f


// Standard Smith geometric shadowing function.
static float G1V(float NdV, float k)
{
    return 1.0f / (NdV * (1.0f - k) + k);
}

// Standard GGX normal distribution function.
static float GGX_NDF(float NdH, float alpha)
{
    float alpha2 = alpha * alpha;

    float denom = NdH * NdH * (alpha2 - 1.0f) + 1.0f;

    denom = max(denom, 1e-3);

    return alpha2 / (M_PI_F * denom * denom);
}

float3 Fresnel_Schlick(half3 F0, float LdH)
{
    return (float3) (F0 + (1.0f - F0) * pow(1.0f - LdH, 5));
}

static half3 evaluateBRDF(AAPLPixelSurfaceData surface,
                           half3 viewDir,
                           half3 lightDirection)
{
    float3 H = normalize((float3) viewDir + (float3) lightDirection);

    float NdL = saturate(dot(surface.normal, lightDirection));
    float LdH = saturate(dot((float3) lightDirection, H));
    float NdH = saturate(dot((float3) surface.normal, H));
    float NdV = saturate(dot(surface.normal, viewDir));

    float alpha = surface.roughness * surface.roughness;
    float k = alpha / 2.0f;

    float3 diffuse = (float3) surface.albedo / M_PI_F;
    float3 F = Fresnel_Schlick(surface.F0, LdH);
    float G = G1V(NdL, k) * G1V(NdV, k);

    float3 specular = F * GGX_NDF(NdH, alpha) * G / 4.0f;

    return (half3) (diffuse * (1 - F) + specular) * NdL;
}

half3 lightingShader(AAPLPixelSurfaceData surfaceData,
                             
                             float depth,
                             float4 worldPosition,
                             AAPLFrameConstants frameData,
                             CameraParamsBuffer cameraParams
                            )
{
    //tocamera should use the second one!!
    //float3 tocamera = cameraParams.invViewMatrix[3].xyz - worldPosition.xyz;
    float3 tocamera2 = float3(cameraParams.invViewMatrix._m03, cameraParams.invViewMatrix._m13, cameraParams.invViewMatrix._m23) - worldPosition.xyz;
    half3 viewDir = (half3) normalize(tocamera2);//TODO:m03,m13,m23
    half3 lightDirection = (half3) frameData.sunDirection;
    half3 light = (half3) (frameData.sunColor * M_PI_F);
    
    
    half3 result = evaluateBRDF(surfaceData, viewDir, lightDirection) * light;
    
    result += surfaceData.emissive * frameData.emissiveScale;

    return result;
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