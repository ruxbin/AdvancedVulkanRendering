
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

#define M_PI_F 3.1415926f