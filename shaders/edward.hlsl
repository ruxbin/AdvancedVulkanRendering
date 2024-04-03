
struct VSInput
{
    float3 position:POSITION;
    float3 normal:NORMAL;
    float2 uv:TEXCOORD0;
};

struct VSOutput
{
    float4 Position   : SV_POSITION; 
    float4 Diffuse    : COLOR0;
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

[[vk::push_constant]] PushConstants pushConstants;

VSOutput RenderSceneVS( VSInput input)
{
    VSOutput Output;
    float4x4 finalMatrix = mul(ub.projectionMatrix,pushConstants.objectToCameraMatrix);
    Output.Position = mul(finalMatrix ,float4(input.position,1.0));
    Output.Diffuse = float4(input.uv,0,0);
    Output.TextureUV = input.uv;
    return Output;    
}

float4 RenderScenePS(VSOutput input) : SV_Target
{
    return float4(input.TextureUV,0,1);
}