
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
    float3 skyColor;          // ambient sky colour used by scatter volume
    float wetness;
    float emissiveScale;
    float localLightIntensity;
    float nearPlane;
    float farPlane;
    float scatterScale;       // global fog density multiplier (scatter volume)
    uint frameCounter;
    float2 physicalSize;
    float2 invPhysicalSize;
    float2 taaJitter;
    float exposure;
    uint taaEnabled;
    // Wind-animated noise offset for volumetric scatter detail.
    float3 globalNoiseOffset;
    float  noiseSpeed;
};

struct CameraParamsBuffer
{
    float4x4 projectionMatrix;
    float4x4 viewMatrix;
    float4x4 invViewMatrix;
    float4x4 invViewProjectionMatrix;
    float4x4 invProjectionMatrix;
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
    float4x4 invProjectionMatrix;
    float4x4 prevViewProjectionMatrix;
};




struct AAPLPointLightCullingData
{
    float4 posRadius; // Bounding sphere position in XYZ and radius of sphere in W.
                                        // Sign of radius:
                                        //  positive - transparency affecting light
                                        //  negative - light does not affect transparency
	float4 color;
};

// Spot light culling/lighting data. cos(outer) and cos(inner) are precomputed on
// the CPU so the shader never calls cos(). color.w sign reuses the point light
// convention: >= 0 = opaque-only, < 0 = also affects transparents.
struct AAPLSpotLightCullingData
{
    float4 posRadius;          // xyz = bounding sphere center (world), w = sphere radius
    float4 posAndHeight;       // xyz = spot position (world), w = height (= linear distance cutoff)
    float4 dirAndOuterAngle;   // xyz = light direction (world, unit), w = cos(outerAngle)
    float4 color;              // xyz = RGB intensity, w = sign(transparent): >=0 opaque, <0 transparent
    float  cosInnerAngle;
    float  _padSpot0, _padSpot1, _padSpot2;
};

#define M_PI_F 3.1415926535897932f


// Reconstruct world-space position from a screen-space texCoord and depth.
//
// Note on the Y-flip convention (Fix #13): the commented-out `ndc.y *= -1`
// is intentionally not applied here because the project's projectionMatrix
// already encodes Vulkan's Y-down NDC (negative Y scale in the projection),
// so its inverse undoes the flip when we multiply. If you ever switch to a
// projection without the implicit Y-flip, re-enable that line.
float4 worldPositionForTexcoord(float2 texCoord, float depth, CameraParamsBufferFull cameraParams, bool isdecal=false)
{
    float4 ndc;
    ndc.xy = texCoord.xy * 2 - 1;
    if(isdecal)
        ndc.y *= -1;
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

// True iff every corner of aabb sits on the inside half-space of every plane.
// Uses the n-vertex (negative-vertex) trick: for each plane, pick the AABB
// corner with the SMALLEST signed distance and test it. If that worst corner
// is inside, all 8 corners are inside.
bool FrustumContains(Frustum frustum, AAPLBoundingBox3 aabb)
{
    [unroll]
    for (int i = 0; i < 6; i++)
    {
        Plane p = frustum.borders[i];
        float3 nVertex;
        nVertex.x = (p.normal.x >= 0.0f) ? aabb.min.x : aabb.max.x;
        nVertex.y = (p.normal.y >= 0.0f) ? aabb.min.y : aabb.max.y;
        nVertex.z = (p.normal.z >= 0.0f) ? aabb.min.z : aabb.max.z;
        if (dot(nVertex, p.normal) - p.w < 0.0f)
            return false;
    }
    return true;
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
