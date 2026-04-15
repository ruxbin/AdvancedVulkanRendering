// Shadow Cascade Culling Compute Shader
#include "commonstruct.hlsl"

[[vk::binding(0,0)]] RWStructuredBuffer<DrawIndexedIndirectCommand> shadowDrawParams;
[[vk::binding(1,0)]] cbuffer shadowCullParams {
    uint opaqueChunkCount;
    uint alphaMaskedChunkCount;
    uint cascadeMaxChunks; // max chunks per category per cascade
    uint cascadeCount;
    Frustum cascadeFrustum;
};
[[vk::binding(2,0)]] StructuredBuffer<AAPLMeshChunk> meshChunks;
[[vk::binding(3,0)]] RWStructuredBuffer<uint> shadowWriteIndex;
// shadowWriteIndex[cascadeIndex * 2 + 0] = opaque count for this cascade
// shadowWriteIndex[cascadeIndex * 2 + 1] = alpha-masked count for this cascade
[[vk::binding(4,0)]] RWStructuredBuffer<uint> shadowChunkIndices;
//[[vk::binding(5,0)]] RWStructuredBuffer<uint> shadowInstanceMap;

[numthreads(128, 1, 1)]
void ShadowCull(uint3 DTid : SV_DispatchThreadID)
{
    uint chunkIndex = DTid.x;
    uint totalChunks = opaqueChunkCount + alphaMaskedChunkCount;

    if (chunkIndex >= totalChunks)
        return;

    // Frustum cull against cascade frustum
    //if (FrustumCull(cascadeFrustum, meshChunks[chunkIndex].boundingBox))
    //    return;

    uint indexCount = meshChunks[chunkIndex].indexCount;
    uint indexBegin = meshChunks[chunkIndex].indexBegin;

    // Base offset for this cascade in the shared buffer
    [unroll]
    for(uint cascadeIndex=0;cascadeIndex<cascadeCount;cascadeIndex++)
    {
        uint cascadeBaseOpaque = cascadeIndex * cascadeMaxChunks * 2;
        uint cascadeBaseAlphaMask = cascadeBaseOpaque + cascadeMaxChunks;

        if (chunkIndex < opaqueChunkCount)
        {
            uint insertIndex;
            InterlockedAdd(shadowWriteIndex[cascadeIndex * 2 + 0], 1, insertIndex);
            uint globalIdx = cascadeBaseOpaque + insertIndex;
            DrawIndexedIndirectCommand cmd = { indexCount, 1, indexBegin, 0, globalIdx };
            shadowDrawParams[globalIdx] = cmd;
            shadowChunkIndices[globalIdx] = chunkIndex;
//            shadowInstanceMap[globalIdx] = globalIdx;
        }
        else
        {
            uint insertIndex;
            InterlockedAdd(shadowWriteIndex[cascadeIndex * 2 + 1], 1, insertIndex);
            uint globalIdx = cascadeBaseAlphaMask + insertIndex;
            DrawIndexedIndirectCommand cmd = { indexCount, 1, indexBegin, 0, globalIdx };
            shadowDrawParams[globalIdx] = cmd;
            shadowChunkIndices[globalIdx] = chunkIndex;
//            shadowInstanceMap[globalIdx] = globalIdx;
        }
    }
    
}
