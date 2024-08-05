
#include "commonstruct.hlsl"


struct VSInput
{
    [[vk::location(0)]] float3 position:POSITION;
};

struct VSOutput
{
    float4 Position   : SV_POSITION; 
    //float4 Diffuse    : COLOR0;
};

[[vk::binding(0,0)]]
cbuffer cam
{
    //CameraParamsBuffer ub;
    AAPLFrameConstants frameConstants;
    CameraParamsBuffer cameraParams;
}

VSOutput RenderSceneVS( VSInput input)
{
    VSOutput Output;
    float4x4 finalMatrix = mul(cameraParams.projectionMatrix, cameraParams.viewMatrix);
    Output.Position = mul(finalMatrix ,float4(input.position,1.0));
    //Output.Diffuse = float4(input.uv,0,0);
    return Output;    
}