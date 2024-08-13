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

[[vk::binding(7,0)]] Texture2DArray<float> shadowMaps;
[[vk::binding(8,0)]] SamplerComparisonState shadowSampler;//TODO : initializer 
//{
   // sampler state
//    Filter = COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
//    AddressU = MIRROR;
//    AddressV = MIRROR;

   // sampler comparison state
//    ComparisonFunc = LESS;
//};
[[vk::binding(9,0)]]
StructuredBuffer<AAPLPointLightCullingData> pointLightCullingData;  //world position
[[vk::binding(10,0)]] StructuredBuffer<uint> lightIndices;

struct VSOutput
{
    float4 Position : SV_POSITION;
   
    float2 TextureUV : TEXCOORD0;
};


[[vk::constant_id(0)]] const bool  useClusterLighting  = false;



float evaluateCascadeShadows(CameraParamsBufferFull cameraParams,
                                    float4 worldPosition,
                                    bool useFilter)//TODO: output cascade index for debug
{
    float shadow = 0;
    for (int cascadeIndex = 0; cascadeIndex < SHADOW_CASCADE_COUNT; cascadeIndex++)
    {
        float4x4 finalMatrix = mul(cameraParams.shadowMatrix[cascadeIndex].shadowProjectionMatrix, cameraParams.shadowMatrix[cascadeIndex].shadowViewMatrix);
        float4 lightSpacePos = mul(finalMatrix, worldPosition);
        lightSpacePos /= lightSpacePos.w;

        if (all(lightSpacePos.xyz < 1.0) && all(lightSpacePos.xyz > float3(-1, -1, 0)))
        {
            shadow = 0.0f;
            float lightSpaceDepth = lightSpacePos.z - 0.0001f;
            float3 shadowUv = float3(lightSpacePos.xy * float2(0.5, 0.5) + 0.5, cascadeIndex);

            if (!useFilter)
                return shadowMaps.SampleCmpLevelZero(shadowSampler, shadowUv, lightSpaceDepth);

            for (int j = -1; j <= 1; ++j)
            {
                for (int i = -1; i <= 1; ++i)
                {
                    shadow += shadowMaps.SampleCmpLevelZero(shadowSampler, shadowUv, lightSpaceDepth, int2(i, j));
                }
            }
            shadow /= 9;
            break;
        }

    }
    return shadow;
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
    
    float shadow = evaluateCascadeShadows(cameraParams, worldPosition, false);
    
    half3 result = lightingShader(surfaceData, depth, worldPosition, frameConstants, cameraParams) * shadow;
    if(useClusterLighting)
    {
	//get the cluster index
	uint xClusterCount = (uint(frameConstants.physicalSize.x) + gLightCullingTileSize - 1) / gLightCullingTileSize;
 
	uint clusterindex = uint(input.Position.x/gLightCullingTileSize)+xClusterCount*uint(input.Position.y/gLightCullingTileSize);

	//lighting
	uint lightCount = lightIndices[clusterindex*MAX_LIGHTS_PER_TILE];
	for(int lightindex = 0;lightindex<lightCount;++lightindex)
	{
		float lightradius = pointLightCullingData[lightIndices[clusterindex*MAX_LIGHTS_PER_TILE+lightindex+1]].posRadius.w;
		float4 posRadiusSqr = float4(pointLightCullingData[lightIndices[clusterindex*MAX_LIGHTS_PER_TILE+lightindex+1]].posRadius.xyz,lightradius*lightradius);

		result += lightingShaderPointSpot(surfaceData,depth,worldPosition,frameConstants,cameraParams,posRadiusSqr,pointLightCullingData[lightIndices[clusterindex*MAX_LIGHTS_PER_TILE+lightindex+1]].color.xyz);
	}
    }
    
    return half4(result, 1.f);
    
}
