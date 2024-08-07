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
uint totalChunks;
uint totalPointLights;
uint totalSpotLights;
Frustum frustum;
}

[[vk::binding(2,0)]]
StructuredBuffer<AAPLPointLightCullingData> pointLightCullingData;  //world position

[[vk::binding(3,0)]] Texture2D<half> inDepth;

[[vk::binding(4,0)]] RWStructuredBuffer<uint16_t4> lightXZRange; //-enable-16bit-types
[[vk::binding(5,0)]] RWTexture2D<uint> lightDebug;            //uint16_t on VK_FORMAT_R8_UINT actuarry --- interlockecadd only supports uint or int
[[vk::binding(6,0)]] RWStructuredBuffer<uint16_t> lightIndices;
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
                        InterlockedAdd(lightDebug[uint2(result.x+row, result.z+col)], 1);
                    }

                }
                
            }
        }

        lightXZRange[DTid.x] = result;
    }
}


groupshared float minDepth[16][16];
groupshared float maxDepth[16][16];
groupshared uint nearZ;
groupshared uint farZ;
[numthreads(16, 16, 1)]
void TraditionalCull(uint3 tid : SV_DispatchThreadID,uint3 gtid:SV_GroupThreadID, uint3 gid:SV_GroupID)
{
    //cull against each tile frustum -- view space
    //1. calculate tile frustum with (near & far depth)
    uint2 basecoord = uint2(gid.x * gLightCullingTileSize + gtid.x *2, gid.y * gLightCullingTileSize + gtid.y*2);
    basecoord = min(basecoord, uint2(frameConstants.physicalSize));
    uint2 basecoord1 = basecoord + uint2(1, 0);
    basecoord1 = min(basecoord1, uint2(frameConstants.physicalSize));
    uint2 basecoord2 = basecoord + uint2(1, 1);
    basecoord2 = min(basecoord2, uint2(frameConstants.physicalSize));
    uint2 basecoord3 = basecoord + uint2(0, 1);
    basecoord3 = min(basecoord3, uint2(frameConstants.physicalSize));
    float mindepth = inDepth.Load(uint3(basecoord, 0));
    float mindepth1 = inDepth.Load(uint3(basecoord1, 0));
    float mindepth2 = inDepth.Load(uint3(basecoord2, 0));
    float mindepth3 = inDepth.Load(uint3(basecoord3, 0));
    minDepth[gtid.x][gtid.y] = min(min(mindepth, mindepth1), min(mindepth2, mindepth3));
    maxDepth[gtid.x][gtid.y] = max(max(mindepth, mindepth1), max(mindepth2, mindepth3));
    if(gtid.xy=uint2(0,0))
    {
        uint orgval = 0;
        InterlockedExchange(nearZ, 0x7fffffff, orgval);
        InterlockedExchange(farZ, 0x0, orgval);
    }
    GroupMemoryBarrier();
    InterlockedMin(nearZ, asuint(minDepth[gtid.x][gtid.y]));
    InterlockedMax(farZ, asuint(maxDepth[gtid.x][gtid.y]));
    GroupMemoryBarrier();
    //2. test each light
    float zNear = nearZ * (frameConstants.nearPlane-frameConstants.farPlane)/frameConstants.nearPlane+frameConstants.farPlane;
    float zFar = farZ * (frameConstants.nearPlane - frameConstants.farPlane) / frameConstants.nearPlane + frameConstants.farPlane;
    float2 xS = (min(float2(gid.x, gid.x + 1) * gLightCullingTileSize, frameConstants.physicalSize.xx) / frameConstants.physicalSize.xx) * 2 - 1;
    float2 yS = (min(float2(gid.y, gid.y + 1) * gLightCullingTileSize, frameConstants.physicalSize.yy) / frameConstants.physicalSize.yy) * 2 - 1;
    float2 xNearS = xS * zNear * cameraParams.projectionMatrix._m00 * 0.5;
    float2 yNearS = yS * zNear * cameraParams.projectionMatrix._m11 * 0.5;
    float2 xFarS = xS * zFar * cameraParams.projectionMatrix._m00 * 0.5;
    float2 yFarS = yS * zFar * cameraParams.projectionMatrix._m11 * 0.5;
    for (int i = gtid.x; i < totalPointLights;i+=16*16)
    {
        float4 lightPosView = mul(cameraParams.viewMatrix, float4(pointLightCullingData[i].posRadius.xyz, 1));
        float r = pointLightCullingData[i].posRadius.w;
    }

}

[numthreads(16, 16, 1)]
void ClearDebugView(uint3 tid : SV_DispatchThreadID, uint3 gid : SV_GroupID)
{
    uint2 tileDims = ceil(float2(frameConstants.physicalSize) / gLightCullingTileSize);
    if (tid.x < tileDims.x && tid.y < tileDims.y)
        lightDebug[tid.xy] = 0;
}