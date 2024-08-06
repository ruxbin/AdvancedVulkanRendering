#include "commonstruct.hlsl"
#include "lighting.hlsl"

#include "geom.hlsl"

[[vk::binding(0,0)]] 
cbuffer frameData
{
    CameraParamsBufferFull cameraParams;
    AAPLFrameConstants frameConstants;
}

[[vk::binding(1,0)]] cbuffer cullParams
{

uint totalPointLights;
uint totalSpotLights;
Frustum frustum;
}

[[vk::binding(2,0)]]
StructuredBuffer<AAPLPointLightCullingData> pointLightCullingData;  //world position

[[vk::binding(3,0)]] Texture2D<half> inDepth;

[[vk::binding(4,0)]] RWStructuredBuffer<uint16_t4> lightXZRange; //-enable-16bit-types
[[vk::binding(5,0)]] RWTexture2D<uint> lightDebug;            //uint16_t on VK_FORMAT_R8_UINT actuarry --- interlockecadd only supports uint or int

//calculate each light's xzrange
[numthreads(128, 1, 1)]
void CoarseCull(uint3 DTid : SV_DispatchThreadID)
{
    if (DTid.x < totalPointLights  &&
        !FrustumCull(frustum, pointLightCullingData[DTid.x]))
    {
        float4 lightPosView = mul(cameraParams.viewMatrix, float4(pointLightCullingData[DTid.x].posRadius.xyz, 1));
        float r = pointLightCullingData[DTid.x].posRadius.w;
        lightPosView.xyz /= lightPosView.w;
        float2 tileDims = float2(frameConstants.physicalSize) / gLightCullingTileSize;
        uint16_t4 result = 0;
       
        {
            AAPLBox2D projectedBounds = getBoundingBox(lightPosView.xyz, r, frameConstants.nearPlane, cameraParams.projectionMatrix);

            float2 boxMin = projectedBounds.min();
            float2 boxMax = projectedBounds.max();

            if (boxMin.x < boxMax.x && boxMin.y < boxMax.y
               && boxMin.x < 1.0f
               && boxMin.y < 1.0f
               && boxMax.x > -1.0f
               && boxMax.y > -1.0f
               )
            {
                boxMin = saturate(boxMin * 0.5f + 0.5f);
                boxMax = saturate(boxMax * 0.5f + 0.5f);

                result.x = boxMin.x * tileDims.x;
                result.y = ceil(boxMax.x * tileDims.x) - result.x;
                result.z = boxMin.y * tileDims.y;
                result.w = ceil(boxMax.y * tileDims.y) - result.z;
                
                
                //debug purpose
                for (int row = 0; row < result.y;++row)
                {
                    for (int col = 0; col < result.w;++col)
                    {
                        //lightDebug[uint2(row, col)];
                        uint useless = 0;
                        InterlockedAdd(lightDebug[uint2(row, col)], 1, useless);
                    }

                }
                
            }
        }

        lightXZRange[DTid.x] = result;
    }
}

[numthreads(16, 16, 1)]
void TraditionalCull(uint3 tid : SV_DispatchThreadID,uint3 gid:SV_GroupID)
{
    
}