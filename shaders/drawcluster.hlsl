#include "commonstruct.hlsl"

struct UniformBuffer
{
    float4x4 projectionMatrix;
    float4x4 viewMatrix;
    float4x4 invViewMatrix;
};

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



struct AAPLPixelSurfaceData
{
    half3 normal;
    half3 albedo;
    half3 F0;
    half  roughness;
    half  alpha;
    half3 emissive;
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

[[vk::binding(0,0)]] cbuffer cam {UniformBuffer ub;}
[[vk::binding(1,0)]] StructuredBuffer<AAPLShaderMaterial> materials;
[[vk::binding(2,0)]] SamplerState _LinearClampSampler;
[[vk::binding(3,0)]] Texture2D<half> _Textures[];  //bindless textures
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
    float4x4 finalMatrix = mul(ub.projectionMatrix,ub.viewMatrix);
    Output.Position = mul(finalMatrix ,float4(input.position,1.0));
    //Output.Diffuse = float4(input.uv,0,0);
    Output.TextureUV = input.uv;
    Output.drawcallid = input.drawcallid;


	Output.viewDir = normalize(float3(ub.invViewMatrix._m03,ub.invViewMatrix._m13,ub.invViewMatrix._m23)-input.position);
	Output.wsPosition = input.position;

	Output.normal = normalize(input.normal);
	Output.tangent = normalize(input.tangent);

    return Output;    
}


PSOutput RenderSceneBasePass(VSOutput input)
{
	PSOutput output;
    uint chunkindex = chunkIndex[input.drawcallid];
    uint materialIndex = meshChunks[chunkindex].materialIndex;
    AAPLShaderMaterial material = materials[materialIndex];
         half4 baseColor = _Textures[material.albedo_texture_index].SampleLevel(_LinearClampSampler,input.TextureUV,0);
	half4 materialData = half4(0,0,0,0);
	half3 emissive = 0;

	if(material.hasMetallicRoughness>0)
		materialData = _Textures[material.roughness_texture_index].SampleLevel(_LinearClampSampler,input.TextureUV,0);

	if(material.hasEmissive>0)
		emissive = _Textures[material.emissive_texture_index].SampleLevel(_LinearClampSampler,input.TextureUV,0);

	half3 geonormal = normalize(input.normal);
	half3 geotan = normalize(input.tangent);
	half3 geobinormal = normalize(cross(geotan,geonormal));
	half3 texnormal = _Textures[material.normal_texture_index].SampleLevel(_LinearClampSampler,input.TextureUV,0);
	texnormal.xy = 2*texnormal.xy-1;
	
	texnormal.z = sqrt(saturate(1.0f - dot(texnormal.xy, texnormal.xy)));

    	half3 normal = texnormal.b * geonormal - texnormal.g * geotan + texnormal.r * geobinormal;
	AAPLPixelSurfaceData surfaceData;
	surfaceData.normal = normal;
	surfaceData.albedo = lerp(baseColor.rgb, 0.0f, materialData.b);
	surfaceData.F0=lerp((half)0.04, baseColor.rgb, materialData.b);
	surfaceData.roughness=max((half)0.08, materialData.g);
	surfaceData.alpha=baseColor.a * material.alpha;
	surfaceData.emissive=emissive;
	output.albedo      = half4(surfaceData.albedo, surfaceData.alpha);
    	output.normals     = half4(surfaceData.normal, 0.0f);
    	output.emissive    = half4(surfaceData.emissive, 0.0f);
    	output.F0Roughness = half4(surfaceData.F0, surfaceData.roughness);
	return output;
}

float4 RenderScenePS(VSOutput input ) : SV_Target
{
    //half4 col = _Textures[materials[pushConstants.materialIndex].albedo_texture_index].SampleLevel(_LinearClampSampler,input.TextureUV,0);
    //return float4(0.5,0.5,0.5,1);


    float4 col = float4(input.TextureUV,0,1);
    uint chunkindex = chunkIndex[input.drawcallid];
    uint materialIndex = meshChunks[chunkindex].materialIndex;
    if(materials[materialIndex].albedo_texture_index!=0xffffffff){
        col = _Textures[materials[materialIndex].albedo_texture_index].SampleLevel(_LinearClampSampler,input.TextureUV,0);
        col.a = 1;
    }

    //float ddx_z = abs(ddx(input.Position.z))*10000;
    //float ddy_z = abs(ddy(input.Position.z))*10000;

    //float4 col = float4(ddx_z,ddy_z,0,1);

    return col;

    //return float4(col.xyz,1);
}
