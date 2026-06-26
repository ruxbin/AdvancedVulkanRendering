#include "shadercompat.hlsl"
#include "commonstruct.hlsl"
#include "lighting.hlsl"



VK_BINDING(0,0)
cbuffer cam REGISTER_CBV(0,0)
{
    CameraParamsBufferFull cameraParams;
    AAPLFrameConstants frameConstants;
}

VK_BINDING(0,1) Texture2D<float4> albedoeTex REGISTER_SRV(0,1);
VK_BINDING(1,1) Texture2D<float4> normalTex REGISTER_SRV(1,1);
VK_BINDING(2,1) Texture2D<float4> emissiveTex REGISTER_SRV(2,1);
VK_BINDING(3,1) Texture2D<float4> F0RoughnessTex REGISTER_SRV(3,1);
VK_BINDING(4,1) Texture2D<float> inDepth REGISTER_SRV(4,1);
VK_BINDING(5,1) SamplerState _NearestClampSampler REGISTER_SAMPLER(5,1);

VK_BINDING(6,1) Texture2DArray<float> shadowMaps REGISTER_SRV(6,1);
VK_BINDING(7,1) SamplerComparisonState shadowSampler REGISTER_SAMPLER_CMP(7,1);
VK_BINDING(10,1) Texture2D<float> aoTexture REGISTER_SRV(10,1);
VK_BINDING(8,1) StructuredBuffer<AAPLPointLightCullingData> pointLightCullingData REGISTER_SRV(8,1);
VK_BINDING(9,1) StructuredBuffer<uint> lightIndices REGISTER_SRV(9,1);

// Spot light bindings.
VK_BINDING(11,1) StructuredBuffer<AAPLSpotLightCullingData> spotLightCullingData REGISTER_SRV(11,1);
VK_BINDING(12,1) StructuredBuffer<uint> spotLightIndices REGISTER_SRV(12,1);

// Spot shadow resources.
#define SPOT_SHADOW_MAX_COUNT 32
#define SPOT_SHADOW_DEPTH_BIAS 0.001f
VK_BINDING(13,1) Texture2DArray<float> spotShadowMaps REGISTER_SRV(13,1);
VK_BINDING(14,1) SamplerComparisonState spotShadowSampler REGISTER_SAMPLER_CMP(14,1);
VK_BINDING(15,1) StructuredBuffer<float4x4> spotViewProjMatrices REGISTER_SRV(15,1);

// Scatter volume (froxel volumetrics, bound after spot shadow slots).
VK_BINDING(16,1) Texture3D<float4> scatterAccumVolume REGISTER_SRV(16,1);
VK_BINDING(17,1) SamplerState linearClampSampler REGISTER_SAMPLER(17,1);

struct VSOutput
{
    float4 Position : SV_POSITION;
   
    float2 TextureUV : TEXCOORD0;
};


[[vk::constant_id(0)]] const bool  useClusterLighting  = false;




// Cascade split distances — must match Shadow.cpp:86-88 (world-space metres).
static const float CASCADE_SPLIT_0 = 3.0f;
static const float CASCADE_SPLIT_1 = 10.0f;

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

    // Cascade selection from raw depth (jitter-independent).
    // The frustum test in evaluateCascadeShadows uses worldPosition which
    // shifts by ~½ px per frame; near a cascade boundary (3 m / 10 m) that
    // is enough to flip between cascades and produce dramatically different
    // shadow-map content on alternating frames.
    //
    // Split distances mirror Shadow.cpp:86-88 (3 / far, 10 / far, 50 / far).
    // Reverse-Z depth → eye-space Z:  eyeZ ≈ near / depth.
    float eyeZ = frameConstants.nearPlane / max(depth, 0.0001f);
    int ci = 0;
    if (eyeZ > CASCADE_SPLIT_0) ci = 1;
    if (eyeZ > CASCADE_SPLIT_1) ci = 2;

    // Evaluate only the selected cascade.
    float4x4 sm = mul(cameraParams.shadowMatrix[ci].shadowProjectionMatrix,
                      cameraParams.shadowMatrix[ci].shadowViewMatrix);
    float4 lsp = mul(sm, worldPosition);
    lsp /= lsp.w;
    float shadow = 1.0f;
    if (all(lsp.xyz < 1.0) && all(lsp.xyz > float3(-1.0f, -1.0f, 0.0f)))
    {
        float lightSpaceDepth = lsp.z - 0.0001f;
        float3 shadowUv = float3(lsp.xy * 0.5f + 0.5f, (float)ci);
        shadow = 0.0f;
        float smW, smH, smLayers, smLevels;
        shadowMaps.GetDimensions(0, smW, smH, smLayers, smLevels);
        float2 smTexelSize = float2(1.0f / smW, 1.0f / smH);
        for (int j = -2; j <= 2; ++j)
        {
            for (int i = -2; i <= 2; ++i)
            {
                float3 uv = shadowUv;
                uv.xy += float2(i, j) * smTexelSize;
                shadow += shadowMaps.SampleCmpLevelZero(shadowSampler, uv, lightSpaceDepth);
            }
        }
        shadow /= 25.0f;
    }

    float ao = aoTexture.SampleLevel(_NearestClampSampler, input.TextureUV, 0);

    half3 result = lightingShader(surfaceData, depth, worldPosition, frameConstants, cameraParams) * shadow * ao;

    // Phase F: for sky pixels (no geometry, depth == 0 in reverse-Z far plane)
    // use the skyColor as the base surface colour so the scatter volume composites
    // correctly against the background horizon.
    if (depth < 0.0001f) {
        float3 camPos2 = float3(cameraParams.invViewMatrix._m03,
                                cameraParams.invViewMatrix._m13,
                                cameraParams.invViewMatrix._m23);
        float3 viewDirWS = normalize(worldPosition.xyz - camPos2);
        float upBlend = saturate(viewDirWS.y * 0.5f + 0.5f);
        result = (half3)lerp((float3)frameConstants.skyColor * 0.5f,
                             (float3)frameConstants.skyColor, upBlend);
    }

    // --- Scatter volume application (froxel volumetrics) ---
    {
        const float SCATTERING_RANGE = 100.0;
        float4 ndcPos  = float4(input.TextureUV * 2.0 - 1.0, depth, 1.0);
        float4 viewPos = mul(cameraParams.invProjectionMatrix, ndcPos);
        float  viewZ   = -viewPos.z / viewPos.w;
        float sliceF = log2(clamp(viewZ, 0.001, SCATTERING_RANGE) / SCATTERING_RANGE * 7.0 + 1.0) / 3.0;
        float3 uvw = float3(input.TextureUV.x, input.TextureUV.y, sliceF);
        float4 scatter = scatterAccumVolume.SampleLevel(linearClampSampler, uvw, 0);
        result = result * (half) scatter.a + (half3) scatter.rgb;
    }

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

	// Spot lights with per-light PCF shadow lookup.
	// Lights with index >= SPOT_SHADOW_MAX_COUNT fall back to shadow=1.
	// Spot lights with per-light PCF shadow lookup.
	// Lights with index >= SPOT_SHADOW_MAX_COUNT fall back to shadow=1.
	uint spotCount = spotLightIndices[clusterindex * MAX_LIGHTS_PER_TILE];
	for (uint si = 0; si < spotCount; ++si)
	{
		uint spotIdx = spotLightIndices[clusterindex * MAX_LIGHTS_PER_TILE + si + 1];
		AAPLSpotLightCullingData spot = spotLightCullingData[spotIdx];

		float shadow = 1.0f;
		if (spotIdx < SPOT_SHADOW_MAX_COUNT)
		{
			float4 lsp = mul(spotViewProjMatrices[spotIdx], worldPosition);
			lsp /= lsp.w;
			if (all(lsp.xyz < 1.0f) && all(lsp.xyz > float3(-1.0f, -1.0f, 0.0f)))
			{
				float lightDepth = lsp.z - SPOT_SHADOW_DEPTH_BIAS;
				float2 suv = lsp.xy * 0.5f + 0.5f;
				float smW, smH, smL, smM;
				spotShadowMaps.GetDimensions(0, smW, smH, smL, smM);
				float2 texelSize = float2(1.0f / smW, 1.0f / smH);
				shadow = 0.0f;
				for (int pj = -1; pj <= 1; ++pj)
					for (int pk = -1; pk <= 1; ++pk)
					{
						float3 suvArr = float3(suv + float2(pk, pj) * texelSize, (float)spotIdx);
						shadow += spotShadowMaps.SampleCmpLevelZero(spotShadowSampler, suvArr, lightDepth);
					}
				shadow /= 9.0f;
			}
		}
		result += applySpotLight(surfaceData, worldPosition, frameConstants, cameraParams, spot, shadow);

		
	}
    }
    
    return half4(result, 1.f);
    
}
