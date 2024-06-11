
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
