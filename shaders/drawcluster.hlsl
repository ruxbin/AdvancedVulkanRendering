#include "commonstruct.hlsl"

struct UniformBuffer
{
    float4x4 projectionMatrix;
    float4x4 viewMatrix;
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
	uint drawcallid: SV_InstanceID;
};

struct VSOutput
{
    float4 Position   : SV_POSITION; 
    //float4 Diffuse    : COLOR0;
    float2 TextureUV  : TEXCOORD0;
    uint drawcallid : TEXCOORD1;
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
    return Output;    
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
