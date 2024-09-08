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

[[vk::binding(3,0)]] Texture2D<float> inDepth; //Texture2D<half> -->  generated SPIR-V is invalid: [VUID-StandaloneSpirv-OpTypeImage-04656] Expected Sampled Type to be a 32-bit int, 64-bit int or 32-bit float scalar type for Vulkan environment
                                                                        //%type_2d_image = OpTypeImage % half2 D2 0 0 1 Unknown

[[vk::binding(4,0)]] RWStructuredBuffer<uint16_t4> lightXZRange; //-enable-16bit-types
[[vk::binding(5,0)]] RWTexture2D<uint> lightDebug;            //uint16_t on VK_FORMAT_R8_UINT actuarry --- interlockecadd only supports uint or int
[[vk::binding(6,0)]] RWStructuredBuffer<uint> lightIndices;
[[vk::binding(7,0)]] RWTexture2D<float4> tradtionDebug;


//calculate each light's xzrange
[numthreads(128, 1, 1)]
void CoarseCull(uint3 DTid : SV_DispatchThreadID)
{
    if (DTid.x < totalPointLights )
    {
        uint16_t4 result = 0;
        if (!FrustumCull(frustum, pointLightCullingData[DTid.x]))
        {
            float4 lightPosView = mul(cameraParams.viewMatrix, float4(pointLightCullingData[DTid.x].posRadius.xyz, 1));
            float r = pointLightCullingData[DTid.x].posRadius.w;
            lightPosView.xyz /= lightPosView.w;
            float2 tileDims = float2(frameConstants.physicalSize) / gLightCullingTileSize;
        
       
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

                    result.x = (uint16_t)(boxMin.x * tileDims.x);
                    result.y = (uint16_t)(ceil(boxMax.x * tileDims.x) - result.x);
                    result.z = (uint16_t)(boxMin.y * tileDims.y);
                    result.w = (uint16_t)(ceil(boxMax.y * tileDims.y) - result.z);
                
                
                //debug purpose
                    for (int row = 0; row < result.y; ++row)
                    {
                        for (int col = 0; col < result.w; ++col)
                        {
                            InterlockedAdd(lightDebug[uint2(result.x + row, result.z + col)], 1);
                        }

                    }
                
                }
            }
           
        }

        lightXZRange[DTid.x] = result;
    }
}

struct AAPLTileFrustum
{
    float tileMinZ;
    float tileMaxZ;
    float4 minZFrustumXY;
    float4 maxZFrustumXY;
    float4 tileBoundingSphere;
    float4 tileBoundingSphereTransparent;
};

static bool intersectsFrustumTile( float3 lightPosView, float r, AAPLTileFrustum frustum, bool transparent)
{
    float4 boundingSphere = transparent ? frustum.tileBoundingSphereTransparent : frustum.tileBoundingSphere;
    float3 tileCenter = boundingSphere.xyz;
    float tileMinZ = transparent ? 0.0f : frustum.tileMinZ;
    float4 minZFrustumXY = transparent ? 0.0f : frustum.minZFrustumXY;

    float3 normal = normalize(tileCenter - lightPosView.xyz);

    //Separate Axis Theorem Test - frustum OBB vs light bounding sphere
    float min_d1 = -dot(normal, lightPosView.xyz);
    float min_d2 = min_d1;
    min_d1 += min(normal.x * minZFrustumXY.x, normal.x * minZFrustumXY.y);
    min_d1 += min(normal.y * minZFrustumXY.z, normal.y * minZFrustumXY.w);
    min_d1 += normal.z * tileMinZ;
    min_d2 += min(normal.x * frustum.maxZFrustumXY.x, normal.x * frustum.maxZFrustumXY.y);
    min_d2 += min(normal.y * frustum.maxZFrustumXY.z, normal.y * frustum.maxZFrustumXY.w);
    min_d2 += normal.z * frustum.tileMaxZ;
    float min_d = min(min_d1, min_d2);

    return (min_d <= r);// && isLightVisibleFine(light, boundingSphere);
}

groupshared uint nearZ;
groupshared uint farZ;
[numthreads(16, 16, 1)]
void TraditionalCull(uint3 tid : SV_DispatchThreadID,uint3 gtid:SV_GroupThreadID, uint3 gid:SV_GroupID)
{
    //cull against each tile frustum -- view space
    //1. calculate tile frustum with (near & far depth)
    uint2 basecoord = uint2(gid.x * gLightCullingTileSize + gtid.x *2, gid.y * gLightCullingTileSize + gtid.y*2);
    basecoord = min(basecoord, uint2(frameConstants.physicalSize)-1);
    uint2 basecoord1 = basecoord + uint2(1, 0);
    basecoord1 = min(basecoord1, uint2(frameConstants.physicalSize)-1);
    uint2 basecoord2 = basecoord + uint2(1, 1);
    basecoord2 = min(basecoord2, uint2(frameConstants.physicalSize)-1);
    uint2 basecoord3 = basecoord + uint2(0, 1);
    basecoord3 = min(basecoord3, uint2(frameConstants.physicalSize)-1);
    float mindepth = inDepth.Load(uint3(basecoord, 0));
    float mindepth1 = inDepth.Load(uint3(basecoord1, 0));
    float mindepth2 = inDepth.Load(uint3(basecoord2, 0));
    float mindepth3 = inDepth.Load(uint3(basecoord3, 0));
    float minDepth = min(min(mindepth, mindepth1), min(mindepth2, mindepth3));
    float maxDepth = max(max(mindepth, mindepth1), max(mindepth2, mindepth3));
    maxDepth = frameConstants.nearPlane*frameConstants.farPlane/(frameConstants.nearPlane - maxDepth*(frameConstants.nearPlane-frameConstants.farPlane));
    minDepth = frameConstants.nearPlane*frameConstants.farPlane/(frameConstants.nearPlane - minDepth*(frameConstants.nearPlane-frameConstants.farPlane));
   // if(gtid.xy==uint2(0,0))
   float clampFar = frameConstants.farPlane+1;
   float clampNear = 0;
    //if(gtid.x==0 && gtid.y==0)
    {
        uint orgval = 0;
        InterlockedExchange(nearZ, asuint(clampFar), orgval);
        InterlockedExchange(farZ, asuint(clampNear), orgval);
    }
    GroupMemoryBarrier();
    InterlockedMin(nearZ, asuint(maxDepth));
    InterlockedMax(farZ, asuint(minDepth));
    GroupMemoryBarrierWithGroupSync();
    //2. test each light
    float asfloatnearZ = asfloat(nearZ); //because we use reverse z here
    float asfloatfarZ = asfloat(farZ);
   
    float zNear = asfloatnearZ;//frameConstants.nearPlane * frameConstants.farPlane / (frameConstants.nearPlane - asfloatnearZ * (frameConstants.nearPlane - frameConstants.farPlane));
    float zFar = asfloatfarZ;//frameConstants.nearPlane * frameConstants.farPlane / (frameConstants.nearPlane - asfloatfarZ * (frameConstants.nearPlane - frameConstants.farPlane));
   
    float2 xS = (min(float2(gid.x, gid.x + 1) * gLightCullingTileSize, frameConstants.physicalSize.xx) / frameConstants.physicalSize.xx) * 2 - 1;
    float2 yS = (min(float2(gid.y, gid.y + 1) * gLightCullingTileSize, frameConstants.physicalSize.yy) / frameConstants.physicalSize.yy) * 2 - 1;
    float2 xNearS = xS * zNear / cameraParams.projectionMatrix._m00;
    float2 yNearS = yS * zNear / cameraParams.projectionMatrix._m11;
    
    float2 xFarS = xS * zFar / cameraParams.projectionMatrix._m00;
    float2 yFarS = yS * zFar / cameraParams.projectionMatrix._m11;
    float2 NearCenter = float2((xNearS.x + xNearS.y) * 0.5, (yNearS.x+yNearS.y)*0.5);
    float2 FarCenter = float2((xFarS.x + xFarS.y) * 0.5, (yFarS.x + yFarS.y) * 0.5);
    float zCenter = (zNear + zFar) * 0.5;
    AAPLTileFrustum frustum;
    frustum.tileMinZ = zNear;
    frustum.tileMaxZ = zFar;
    frustum.minZFrustumXY = float4(xNearS,yNearS);
    frustum.maxZFrustumXY = float4(xFarS,yFarS);
    frustum.tileBoundingSphere = float4(float3((NearCenter + FarCenter)*0.5, zCenter), 0);
    
    uint xCluterCount = (uint(frameConstants.physicalSize.x) + gLightCullingTileSize - 1) / gLightCullingTileSize;
    uint outputIndex = (gid.x + gid.y * xCluterCount) * MAX_LIGHTS_PER_TILE;
    //if(gtid.x==0&&gtid.y==0)
    //	tradtionDebug[gid.xy] = float4(frustum.tileMinZ, frustum.tileMaxZ, asfloatnearZ, asfloatfarZ);
    for (int i = gtid.x+gtid.y*16; i < totalPointLights;i+=16*16)
    {
        float4 lightPosView = mul(cameraParams.viewMatrix, float4(pointLightCullingData[i].posRadius.xyz, 1));
        
        float r = pointLightCullingData[i].posRadius.w;
        
        bool inFrustumMinZ = (lightPosView.z + r) > frustum.tileMinZ;
        bool inFrustumMaxZ = (lightPosView.z - r) < frustum.tileMaxZ;
     	//if(i==64)
//	{
//		tradtionDebug[gid.xy] = float4(lightPosView.z,r,frustum.tileMinZ,lightPosView.z+r+frustum.tileMinZ);
//	}
        uint16_t4 xzRange = lightXZRange[i];
        //if(gid.x>=xzRange.x && gid.x<xzRange.x+xzRange.y && 
        //    gid.y>=xzRange.z && gid.y<xzRange.z+xzRange.w)
        if (uint(gid.x - xzRange.x) < xzRange.y &&
            uint(gid.y - xzRange.z) < xzRange.w
	    && inFrustumMinZ
	    && inFrustumMaxZ)
        {
            if (intersectsFrustumTile(lightPosView.xyz, r, frustum, false))
            {
                uint storeindex = 0;
                InterlockedAdd(lightIndices[outputIndex], 1, storeindex);
                lightIndices[storeindex + outputIndex + 1] = i;
                InterlockedAdd(lightDebug[gid.xy], 1);
            }
            
        }
        
        

    }

}

[numthreads(16, 16, 1)]
void ClearDebugView(uint3 tid : SV_DispatchThreadID, uint3 gid : SV_GroupID)
{
    uint2 tileDims = ceil(float2(frameConstants.physicalSize) / gLightCullingTileSize);
    if (tid.x < tileDims.x && tid.y < tileDims.y)
        lightDebug[tid.xy] = 0;
}


#define CLEAR_WIDTH 8
#define CLEAR_HEIGHT 8
[numthreads(CLEAR_WIDTH, CLEAR_HEIGHT, 1)]
void ClearLightIndices(uint3 gid : SV_GroupID, uint3 gtid : SV_GroupThreadID)
{
    uint2 tileDims = ceil(float2(frameConstants.physicalSize) / gLightCullingTileSize);
    
    uint outputIndex = (gid.x + gid.y * tileDims.x) * MAX_LIGHTS_PER_TILE;
    for (int i = gtid.x + gtid.y * CLEAR_WIDTH; i < MAX_LIGHTS_PER_TILE; i += CLEAR_WIDTH * CLEAR_HEIGHT)
        lightIndices[i+outputIndex] = 0;

}
