#include "commonstruct.hlsl"



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

[[vk::binding(0,0)]]
cbuffer cam
{
    //CameraParamsBuffer ub;
    AAPLFrameConstants frameConstants;
    CameraParamsBuffer cameraParams;
}
[[vk::binding(1,0)]] StructuredBuffer<AAPLShaderMaterial> materials;
[[vk::binding(2,0)]] SamplerState _LinearRepeatSampler;
[[vk::binding(3,0)]] Texture2D<half4> _Textures[];  //bindless textures
[[vk::binding(4,0)]] StructuredBuffer<AAPLMeshChunk> meshChunks; 
[[vk::binding(5,0)]] StructuredBuffer<uint> chunkIndex;

// [[vk::binding(0,1)]] Buffer<float3> positions;
// [[vk::binding(1,1)]] Buffer<float3> normals;
// [[vk::binding(2,1)]] Buffer<float3> uvs;
// [[vk::binding(3,1)]] Buffer<float3> tangents;
// [[vk::binding(4,1)]] Buffer<uint> indices;

// [[vk::binding(0,2)]] StructuredBuffer<AAPLMeshChunk> meshChunks;



[[vk::push_constant]] PushConstants pushConstants;


struct VSInput
{
    [[vk::location(0)]] float3 position:POSITION;
    [[vk::location(1)]] float3 normal:NORMAL;
    [[vk::location(2)]] float3 tangent:Tangent;
    [[vk::location(3)]] float2 uv:TEXCOORD0;
    [[vk::location(4)]] uint drawcallid : BLENDINDICES;
	//uint instancid: SV_InstanceID;
};

struct VSOutput
{
    float4 Position   : SV_POSITION; 
    //float4 Diffuse    : COLOR0;
    float2 TextureUV  : TEXCOORD0;
    uint drawcallid : TEXCOORD1;

	half3 viewDir : TEXCOORD2;
    half3 normal : TEXCOORD3;
    half3 tangent : TEXCOORD4;
    float3 wsPosition : TEXCOORD5;
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
    Output.drawcallid = input.drawcallid;


    Output.viewDir = normalize(float3(cameraParams.invViewMatrix._m03, cameraParams.invViewMatrix._m13, cameraParams.invViewMatrix._m23) - input.position);
	Output.wsPosition = input.position;

	Output.normal = normalize(input.normal);
	Output.tangent = normalize(input.tangent);

    return Output;    
}

#define ALPHA_CUTOUT 0.1

PSOutput RenderSceneBasePass(VSOutput input)
{
	PSOutput output;
    uint chunkindex = chunkIndex[input.drawcallid];
    uint materialIndex = meshChunks[chunkindex].materialIndex;
    AAPLShaderMaterial material = materials[materialIndex];
    half4 baseColor = _Textures[material.albedo_texture_index].SampleLevel(_LinearRepeatSampler, input.TextureUV, 0);
	half4 materialData = half4(0,0,0,0);
	half4 emissive = 0;

	if(material.hasMetallicRoughness>0)
        materialData = _Textures[material.roughness_texture_index].SampleLevel(_LinearRepeatSampler, input.TextureUV, 0);

	if(material.hasEmissive>0)
        emissive = _Textures[material.emissive_texture_index].SampleLevel(_LinearRepeatSampler, input.TextureUV, 0);

	half3 geonormal = normalize(input.normal);
	half3 geotan = normalize(input.tangent);
	half3 geobinormal = normalize(cross(geotan,geonormal));
    half4 texnormal = _Textures[material.normal_texture_index].SampleLevel(_LinearRepeatSampler, input.TextureUV, 0);
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


PSOutput RenderSceneBasePS(VSOutput input)
{
    PSOutput output;
    
    uint materialIndex = pushConstants.materialIndex;
    AAPLShaderMaterial material = materials[materialIndex];
    half4 baseColor = _Textures[material.albedo_texture_index].SampleLevel(_LinearRepeatSampler, input.TextureUV, 10);
    half4 materialData = half4(0, 0, 0, 0);
    half4 emissive = 0;

    if (material.hasMetallicRoughness > 0)
        materialData = _Textures[material.roughness_texture_index].SampleLevel(_LinearRepeatSampler, input.TextureUV, 10);

    if (material.hasEmissive > 0)
        emissive = _Textures[material.emissive_texture_index].SampleLevel(_LinearRepeatSampler, input.TextureUV, 10);

    half3 geonormal = normalize(input.normal);
    half3 geotan = normalize(input.tangent);
    half3 geobinormal = normalize(cross(geotan, geonormal));
    half4 texnormal = _Textures[material.normal_texture_index].SampleLevel(_LinearRepeatSampler, input.TextureUV, 10);
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
    		clip(surfaceData.alpha - ALPHA_CUTOUT);
    output.albedo = half4(surfaceData.albedo, surfaceData.alpha);
    output.normals = half4(surfaceData.normal, 0.0f);
    output.emissive = half4(surfaceData.emissive, 0.0f);
    output.F0Roughness = half4(surfaceData.F0, surfaceData.roughness);
    return output;
}
