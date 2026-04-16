#include "commonstruct.hlsl"
#include "lighting.hlsl"
#include "shadercompat.hlsl"


struct GeometyBuffer
{
    Buffer<float3> positions;
    Buffer<float3> normals;
    Buffer<float2> uvs;
    Buffer<float3> tangents;
    Buffer<uint> indices;
};



struct PushConstants
{
    uint materialIndex;
    uint shadowIndex;
};



struct AAPLShaderMaterial
{
    uint albedo_texture_index;
    uint roughness_texture_index;
    uint normal_texture_index;
    uint emissive_texture_index;
    float alpha;
    uint hasMetallicRoughness;
    uint hasEmissive;
    uint padding;
// #if SUPPORT_SPARSE_TEXTURES //TODO:
//     uint baseColorMip;
//     uint metallicRoughnessMip;
//     uint normalMip;
//     uint emissiveMip;
// #endif
};

[[vk::constant_id(0)]] const bool  specAlphaMask  = false;
//[[vk::constant_id(1)]] const bool specTransparent = false;

VK_BINDING(0,0)
cbuffer cam REGISTER_CBV(0,0)
{
    CameraParamsBufferFull cameraParams;
    AAPLFrameConstants frameConstants;
}
VK_BINDING(0,1) StructuredBuffer<AAPLShaderMaterial> materials REGISTER_SRV(0,1);
VK_BINDING(1,1) SamplerState _LinearRepeatSampler REGISTER_SAMPLER(1,1);
VK_BINDING(2,1) Texture2D<half4> _Textures[] REGISTER_SRV(0,2);  //bindless textures (DX12: own space to avoid overlap)
VK_BINDING(3,1) StructuredBuffer<AAPLMeshChunk> meshChunks REGISTER_SRV(3,1);
VK_BINDING(4,1) StructuredBuffer<uint> chunkIndex REGISTER_SRV(4,1);



DECLARE_PUSH_CONSTANTS(PushConstants, pushConstants, 1);


struct VSInput
{
    [[vk::location(0)]] float3 position:POSITION;
    [[vk::location(1)]] float3 normal:NORMAL;
    [[vk::location(2)]] float3 tangent:Tangent;
    [[vk::location(3)]] float2 uv:TEXCOORD0;
    //[[vk::location(4)]] uint drawcallid : BLENDINDICES;
	uint instancid: SV_InstanceID;
};

struct VSOutput
{
    float4 Position   : SV_POSITION; 
    //float4 Diffuse    : COLOR0;
    float2 TextureUV  : TEXCOORD0;
    uint chunkid : TEXCOORD1;

	half3 viewDir : TEXCOORD2;
    half3 normal : TEXCOORD3;
    half3 tangent : TEXCOORD4;
    float4 wsPosition : TEXCOORD5;
};

struct PSOutput
{
	half4 albedo : SV_Target0;
	half4 normals : SV_Target1;
	half4 emissive : SV_Target2;
	half4 F0Roughness : SV_Target3;

};


//vkCmdDrawIndexed indexcount firstindex
VSOutput RenderSceneVS( VSInput input)
{
    VSOutput Output;
    float4x4 finalMatrix = mul(cameraParams.projectionMatrix, cameraParams.viewMatrix);
    Output.Position = mul(finalMatrix ,float4(input.position,1.0));
    //Output.Diffuse = float4(input.uv,0,0);
    Output.TextureUV = input.uv;
    Output.chunkid = chunkIndex[input.instancid];

    float4x4 invViewMatrix = cameraParams.invViewMatrix;

    Output.viewDir = normalize(float3(invViewMatrix._m03, invViewMatrix._m13, invViewMatrix._m23) - input.position);
    Output.wsPosition = float4(input.position, 1);

	Output.normal = normalize(input.normal);
	Output.tangent = normalize(input.tangent);

    return Output;    
}

VSOutput RenderSceneVSShadow( VSInput input)
{
    VSOutput Output;
    float4x4 finalMatrix = mul(cameraParams.shadowMatrix[pushConstants.shadowIndex].shadowProjectionMatrix, cameraParams.shadowMatrix[pushConstants.shadowIndex].shadowViewMatrix);
    Output.Position = mul(finalMatrix ,float4(input.position,1.0));
    //Output.Diffuse = float4(input.uv,0,0);
    Output.TextureUV = input.uv;
    Output.chunkid = chunkIndex[input.instancid];

    float4x4 invViewMatrix = cameraParams.invViewMatrix;

    //Output.viewDir = normalize(float3(invViewMatrix._m03, invViewMatrix._m13, invViewMatrix._m23) - input.position);
    Output.wsPosition = float4(input.position, 1);

	//Output.normal = normalize(input.normal);
	Output.tangent = normalize(input.tangent);

    return Output;    
}



#define ALPHA_CUTOUT 0.1

PSOutput RenderSceneBasePass(VSOutput input)
{
	PSOutput output;
    uint chunkindex = input.chunkid;
    uint materialIndex = meshChunks[chunkindex].materialIndex;
    AAPLShaderMaterial material = materials[materialIndex];
    half4 baseColor = _Textures[material.albedo_texture_index].Sample(_LinearRepeatSampler, input.TextureUV);
	half4 materialData = half4(0,0,0,0);
	half4 emissive = 0;

	if(material.hasMetallicRoughness>0)
        materialData = _Textures[material.roughness_texture_index].Sample(_LinearRepeatSampler, input.TextureUV);

	if(material.hasEmissive>0)
        emissive = _Textures[material.emissive_texture_index].Sample(_LinearRepeatSampler, input.TextureUV);

	half3 geonormal = normalize(input.normal);
	half3 geotan = normalize(input.tangent);
	half3 geobinormal = normalize(cross(geotan,geonormal));
    half4 texnormal = _Textures[material.normal_texture_index].Sample(_LinearRepeatSampler, input.TextureUV);
	texnormal.xy = 2*texnormal.xy-1;
	
	texnormal.z = sqrt(saturate(1.0f - dot(texnormal.xy, texnormal.xy)));

    half3 normal = texnormal.b * geonormal - texnormal.g * geotan + texnormal.r * geobinormal;
	AAPLPixelSurfaceData surfaceData;
	surfaceData.normal = normal;
	surfaceData.albedo = lerp(baseColor.rgb, 0.0f, materialData.b);
	surfaceData.F0=lerp((half)0.04, baseColor.rgb, materialData.b);
	surfaceData.roughness=max((half)0.08, materialData.g);
	surfaceData.alpha=baseColor.a * material.alpha;
	surfaceData.emissive=emissive.xyz;
	output.albedo      = half4(surfaceData.albedo, surfaceData.alpha);
    output.normals     = half4(surfaceData.normal, 0.0f);
    output.emissive    = half4(surfaceData.emissive, 0.0f);
    output.F0Roughness = half4(surfaceData.F0, surfaceData.roughness);
	return output;
}

//deprecated - cpu push constant version
PSOutput RenderSceneBasePS(VSOutput input)
{
    PSOutput output;
    
    uint materialIndex = pushConstants.materialIndex;
    AAPLShaderMaterial material = materials[materialIndex];
    half4 baseColor = _Textures[material.albedo_texture_index].Sample(_LinearRepeatSampler, input.TextureUV);
    half4 materialData = half4(0, 0, 0, 0);
    half4 emissive = 0;

    if (material.hasMetallicRoughness > 0)
        materialData = _Textures[material.roughness_texture_index].Sample(_LinearRepeatSampler, input.TextureUV);

    if (material.hasEmissive > 0)
        emissive = _Textures[material.emissive_texture_index].Sample(_LinearRepeatSampler, input.TextureUV);

    half3 geonormal = normalize(input.normal);
    half3 geotan = normalize(input.tangent);
    half3 geobinormal = normalize(cross(geotan, geonormal));
    half4 texnormal = _Textures[material.normal_texture_index].Sample(_LinearRepeatSampler, input.TextureUV);
    //half4 texnormalmip = _Textures[material.normal_texture_index].SampleLevel(_LinearRepeatSampler,input.TextureUV,3.5);
    //texnormalmip = abs(texnormal-texnormalmip);
    texnormal.xy = 2 * texnormal.xy - 1;
    half dotproduct = dot(texnormal.xy, texnormal.xy);
    half oneminusdotproduct = saturate(1.0f - dotproduct);
    half zzz = sqrt(oneminusdotproduct);

    half3 normal = zzz * geonormal - texnormal.y * geotan + texnormal.x * geobinormal;
    
    AAPLPixelSurfaceData surfaceData;
    surfaceData.normal = normal;
    surfaceData.albedo = lerp(baseColor.rgb, 0.0f, materialData.b);
    surfaceData.F0 = lerp((half) 0.04, baseColor.rgb, materialData.b);
    surfaceData.roughness = max((half) 0.08, materialData.g);
    surfaceData.alpha = baseColor.a * material.alpha;
    surfaceData.emissive = emissive.xyz;

	if(specAlphaMask)
    {
        clip(surfaceData.alpha - ALPHA_CUTOUT);
        surfaceData.alpha = 1;

    }
    output.albedo = half4(surfaceData.albedo, surfaceData.alpha);
    output.normals = half4(surfaceData.normal, 0.0f);
    output.emissive = half4(surfaceData.emissive, 0.0f);
//output.emissive = texnormalmip;
    output.F0Roughness = half4(surfaceData.F0, surfaceData.roughness);
//output.F0Roughness = half4(ddx(input.TextureUV),ddy(input.TextureUV));
    return output;
}


void RenderSceneDepthOnly(VSOutput input)
{
    PSOutput output;
    
    uint materialIndex = pushConstants.materialIndex;
    AAPLShaderMaterial material = materials[materialIndex];
    half4 baseColor = _Textures[material.albedo_texture_index].Sample(_LinearRepeatSampler, input.TextureUV);
   
    if (specAlphaMask)
    {
        clip(baseColor.w - ALPHA_CUTOUT);

    }
    
}

half4 RenderSceneForwardPS(VSOutput input) : SV_Target
{
    uint materialIndex = pushConstants.materialIndex;
    AAPLShaderMaterial material = materials[materialIndex];
    half4 baseColor = _Textures[material.albedo_texture_index].Sample(_LinearRepeatSampler, input.TextureUV);
    half4 materialData = half4(0, 0, 0, 0);
    half4 emissive = 0;

    if (material.hasMetallicRoughness > 0)
        materialData = _Textures[material.roughness_texture_index].Sample(_LinearRepeatSampler, input.TextureUV);

    if (material.hasEmissive > 0)
        emissive = _Textures[material.emissive_texture_index].Sample(_LinearRepeatSampler, input.TextureUV);

    half3 geonormal = normalize(input.normal);
    half3 geotan = normalize(input.tangent);
    half3 geobinormal = normalize(cross(geotan, geonormal));
    half4 texnormal = _Textures[material.normal_texture_index].Sample(_LinearRepeatSampler, input.TextureUV);
    texnormal.xy = 2 * texnormal.xy - 1;
    half dotproduct = dot(texnormal.xy, texnormal.xy);
    half oneminusdotproduct = saturate(1.0f - dotproduct);
    half zzz = sqrt(oneminusdotproduct);

    half3 normal = zzz * geonormal - texnormal.y * geotan + texnormal.x * geobinormal;

    AAPLPixelSurfaceData surfaceData;
    surfaceData.normal = normal;
    surfaceData.albedo = lerp(baseColor.rgb, 0.0f, materialData.b);
    surfaceData.F0 = lerp((half) 0.04, baseColor.rgb, materialData.b);
    surfaceData.roughness = max((half) 0.08, materialData.g);
    surfaceData.alpha = baseColor.a * material.alpha;
    surfaceData.emissive = emissive.xyz;

    half3 res = lightingShader(surfaceData, 0, input.wsPosition, frameConstants, cameraParams);
    return half4(res, surfaceData.alpha);

}

// --- GPU Indirect Base Pass with Alpha Mask (Stage 1) ---
// Reads material from SSBO via chunkIndex, clips alpha
PSOutput RenderSceneBasePassAlphaMask(VSOutput input)
{
    PSOutput output;
    uint chunkindex = input.chunkid;
    uint materialIndex = meshChunks[chunkindex].materialIndex;
    AAPLShaderMaterial material = materials[materialIndex];
    half4 baseColor = _Textures[material.albedo_texture_index].Sample(_LinearRepeatSampler, input.TextureUV);

    clip(baseColor.w - ALPHA_CUTOUT);

    half4 materialData = half4(0,0,0,0);
    half4 emissive = 0;

    if(material.hasMetallicRoughness>0)
        materialData = _Textures[material.roughness_texture_index].Sample(_LinearRepeatSampler, input.TextureUV);

    if(material.hasEmissive>0)
        emissive = _Textures[material.emissive_texture_index].Sample(_LinearRepeatSampler, input.TextureUV);

    half3 geonormal = normalize(input.normal);
    half3 geotan = normalize(input.tangent);
    half3 geobinormal = normalize(cross(geotan,geonormal));
    half4 texnormal = _Textures[material.normal_texture_index].Sample(_LinearRepeatSampler, input.TextureUV);
    texnormal.xy = 2*texnormal.xy-1;
    texnormal.z = sqrt(saturate(1.0f - dot(texnormal.xy, texnormal.xy)));

    half3 normal = texnormal.b * geonormal - texnormal.g * geotan + texnormal.r * geobinormal;
    AAPLPixelSurfaceData surfaceData;
    surfaceData.normal = normal;
    surfaceData.albedo = lerp(baseColor.rgb, 0.0f, materialData.b);
    surfaceData.F0=lerp((half)0.04, baseColor.rgb, materialData.b);
    surfaceData.roughness=max((half)0.08, materialData.g);
    surfaceData.alpha=baseColor.a * material.alpha;
    surfaceData.emissive=emissive.xyz;
    output.albedo      = half4(surfaceData.albedo, surfaceData.alpha);
    output.normals     = half4(surfaceData.normal, 0.0f);
    output.emissive    = half4(surfaceData.emissive, 0.0f);
    output.F0Roughness = half4(surfaceData.F0, surfaceData.roughness);
    return output;
}

// --- GPU Indirect Forward Pass (Stage 4) ---
// Reads material from SSBO via chunkIndex, no push constants
half4 RenderSceneForwardPSIndirect(VSOutput input) : SV_Target
{
    uint chunkindex = input.chunkid;
    uint materialIndex = meshChunks[chunkindex].materialIndex;
    AAPLShaderMaterial material = materials[materialIndex];
    half4 baseColor = _Textures[material.albedo_texture_index].Sample(_LinearRepeatSampler, input.TextureUV);
    half4 materialData = half4(0, 0, 0, 0);
    half4 emissive = 0;

    if (material.hasMetallicRoughness > 0)
        materialData = _Textures[material.roughness_texture_index].Sample(_LinearRepeatSampler, input.TextureUV);

    if (material.hasEmissive > 0)
        emissive = _Textures[material.emissive_texture_index].Sample(_LinearRepeatSampler, input.TextureUV);

    half3 geonormal = normalize(input.normal);
    half3 geotan = normalize(input.tangent);
    half3 geobinormal = normalize(cross(geotan, geonormal));
    half4 texnormal = _Textures[material.normal_texture_index].Sample(_LinearRepeatSampler, input.TextureUV);
    texnormal.xy = 2 * texnormal.xy - 1;
    half dotproduct = dot(texnormal.xy, texnormal.xy);
    half oneminusdotproduct = saturate(1.0f - dotproduct);
    half zzz = sqrt(oneminusdotproduct);

    half3 normal = zzz * geonormal - texnormal.y * geotan + texnormal.x * geobinormal;

    AAPLPixelSurfaceData surfaceData;
    surfaceData.normal = normal;
    surfaceData.albedo = lerp(baseColor.rgb, 0.0f, materialData.b);
    surfaceData.F0 = lerp((half) 0.04, baseColor.rgb, materialData.b);
    surfaceData.roughness = max((half) 0.08, materialData.g);
    surfaceData.alpha = baseColor.a * material.alpha;
    surfaceData.emissive = emissive.xyz;

    half3 res = lightingShader(surfaceData, 0, input.wsPosition, frameConstants, cameraParams);
    return half4(res, surfaceData.alpha);
}

// --- GPU Indirect Shadow Depth Only (Stage 2) ---
// Reads material from SSBO for alpha test, no push constants
void RenderSceneShadowDepthIndirect(VSOutput input)
{
    if (specAlphaMask)
    {
        uint chunkindex = input.chunkid;
        uint materialIndex = meshChunks[chunkindex].materialIndex;
        AAPLShaderMaterial material = materials[materialIndex];
        half4 baseColor = _Textures[material.albedo_texture_index].Sample(_LinearRepeatSampler, input.TextureUV);
        clip(baseColor.w - ALPHA_CUTOUT);
    }
}
