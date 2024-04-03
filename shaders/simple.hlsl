
struct VSInput
{
    float3 position:POSITION;
    float3 normal:NORMAL;
    float2 uv:TEXCOORD0;
};

struct VSOutput
{
    float4 Position   : SV_POSITION; 
    float2 TextureUV  : TEXCOORD0;
};

struct UniformBuffer
{
    float4x4 projectionMatrix;
};

struct PushConstants
{
    float4x4 objectToCameraMatrix;
};

[[vk::binding(0,0)]] cbuffer cam {UniformBuffer ub;}
[[vk::binding(1,0)]] Texture2D<half> _Texture;
[[vk::binding(2,0)]] SamplerState _LinearClampSampler;

[[vk::push_constant]] PushConstants pushConstants;

VSOutput RenderSceneVS( VSInput input)
{
    VSOutput Output;
    float4x4 finalMatrix = mul(ub.projectionMatrix,pushConstants.objectToCameraMatrix);
    Output.Position = mul(finalMatrix ,float4(input.position,1.0));
    Output.TextureUV = input.uv;
    return Output;    
}

float4 RenderScenePS(VSOutput input) : SV_Target
{
    float4 res = _Texture.SampleLevel(_LinearClampSampler,input.TextureUV,0);
    res.a = 1;
    return res;
}