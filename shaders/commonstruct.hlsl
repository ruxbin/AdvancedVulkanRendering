
struct AAPLBoundingBox3
{
    float3 min;
    float3 max;
};

struct AAPLSphere
{
    float4 data; //xyz center, w radius
};



struct AAPLMeshChunk
{
    AAPLBoundingBox3 boundingBox;
    float4 normalDistribution;
    float4 cluterMean;

    AAPLSphere boundingSphere;

    unsigned int materialIndex;
    unsigned int indexBegin;
    unsigned int indexCount;
};

//keep it the same as VkDrawIndexedIndirectCommand
struct DrawIndexedIndirectCommand
{
    uint indexCount;
    uint instanceCount;
    uint firstIndex;
    int vertexOffset;
    uint firstInstance;
};


struct AAPLPixelSurfaceData
{
    half3 normal;
    half3 albedo;
    half3 F0;
    half roughness;
    half alpha;
    half3 emissive;
};


struct AAPLFrameConstants
{
    float3 sunDirection;
    float3 sunColor;
    float wetness;
    float emissiveScale;
    float localLightIntensity;
    float nearPlane;
    float farPlane;
    float2 physicalSize;
};

struct CameraParamsBuffer
{
    float4x4 projectionMatrix;
    float4x4 viewMatrix;
    float4x4 invViewMatrix;
    float4x4 invViewProjectionMatrix;
};

#define SHADOW_CASCADE_COUNT 3

struct ShadowMatrix
{
    float4x4 shadowProjectionMatrix;
    float4x4 shadowViewMatrix;
};

struct CameraParamsBufferFull
{
    ShadowMatrix shadowMatrix[SHADOW_CASCADE_COUNT];
    
    float4x4 projectionMatrix;
    float4x4 viewMatrix;
    float4x4 invViewMatrix;
    float4x4 invViewProjectionMatrix;
};


// Point light information for culling.

struct AAPLPointLightCullingData
{
    float4 posRadius; // Bounding sphere position in XYZ and radius of sphere in W.
                                        // Sign of radius:
                                        //  positive - transparency affecting light
                                        //  negative - light does not affect transparency
};

#define M_PI_F 3.1415926535897932f


float4 worldPositionForTexcoord(float2 texCoord, float depth, CameraParamsBufferFull cameraParams)
{
    float4 ndc;
    ndc.xy = texCoord.xy * 2 - 1;
    //ndc.y *= -1;
    ndc.z = depth;
    ndc.w = 1;

    float4 worldPosition = mul(cameraParams.invViewProjectionMatrix, ndc);
    worldPosition /= worldPosition.w;
    return worldPosition;
}


//---------------------- frustrum related begin
/*
hlsl 不支持成员函数，不支持reference parameter，语法层面比metal差
*/
struct Plane
{
    //float nx, ny, nz, w;
    float3 normal;
    float w;
};


bool IsInside(Plane p, AAPLBoundingBox3 aabb)
{

    float maxD = -1e9; // , minD = std::numeric_limits<float>::max;
    //unroll掉，不需要loop带来的branch--不太确定编译器是否会自动处理，x,y,z每个值的循环次数是固定的
    [unroll]
    for (int z_z = 0; z_z < 2; z_z++)
    {
            [unroll]
        for (int y_y = 0; y_y < 2; y_y++)
        {
                    [unroll]
            for (int x_x = 0; x_x < 2; x_x++)
            {
                float3 cornor_i = float3(x_x == 0 ? aabb.min.x : aabb.max.x, y_y == 0 ? aabb.min.y : aabb.max.y, z_z == 0 ? aabb.min.z : aabb.max.z);
                float d1 = dot(cornor_i, p.normal) - p.w;
                if (d1 > maxD)
                    maxD = d1;
                            //if (d1 < minD)
                            //	minD = d1;
            }
        }
    }
    return maxD > 0;
}

bool IsInside(Plane p, AAPLPointLightCullingData sphere)
{
    float d1 = dot(p.normal, sphere.posRadius.xyz) - p.w;
    if (d1 < 0 && ((d1 * d1) > sphere.posRadius.w * sphere.posRadius.w))
        return false;
    else
        return true;
}

struct Frustum
{
    Plane borders[6];

};

bool FrustumCull(Frustum frustum, AAPLBoundingBox3 aabb)
{
    //return false;
    [unroll]
    for (int i = 0; i < 6; i++)
    {
        if (!IsInside(frustum.borders[i], aabb))
            return true;
    }
    return false;
}

bool FrustumCull(Frustum frustum, AAPLPointLightCullingData sphere)
{
    [unroll]
    for (int i = 0; i < 6; i++)
    {
        if (!IsInside(frustum.borders[i], sphere))
            return true;
    }
    return false;
}

//-----------------------------frustrum realted end

//variable 'gLightCullingTileSize' will be placed in $Globals so initializer ignored
//const uint gLightCullingTileSize = 32;
#define gLightCullingTileSize 32

#define MAX_LIGHTS_PER_TILE                 (64)

#define MAX_LIGHTS_PER_CLUSTER              (16)

#define LIGHT_CLUSTER_DEPTH                 (64)