// Shadow Cascade Culling Compute Shader
#include "commonstruct.hlsl"

[[vk::binding(0,0)]] RWStructuredBuffer<DrawIndexedIndirectCommand> shadowDrawParams;
[[vk::binding(1,0)]] cbuffer shadowCullParams {
    uint opaqueChunkCount;
    uint alphaMaskedChunkCount;
    uint cascadeMaxChunks; // max chunks per category per cascade
    uint cascadeCount;
    // .x = min boundingSphere radius below which the chunk is skipped
    // for this cascade. Lets far cascades drop sub-texel props.
    float4 cascadeCullThreshold[SHADOW_CASCADE_COUNT];
    Frustum cascadeFrustum[SHADOW_CASCADE_COUNT];
};
[[vk::binding(2,0)]] StructuredBuffer<AAPLMeshChunk> meshChunks;
[[vk::binding(3,0)]] RWStructuredBuffer<uint> shadowWriteIndex;
// shadowWriteIndex[cascadeIndex * 2 + 0] = opaque count for this cascade
// shadowWriteIndex[cascadeIndex * 2 + 1] = alpha-masked count for this cascade
[[vk::binding(4,0)]] RWStructuredBuffer<uint> shadowChunkIndices;

[numthreads(128, 1, 1)]
void ShadowCull(uint3 DTid : SV_DispatchThreadID)
{
    uint chunkIndex = DTid.x;
    uint totalChunks = opaqueChunkCount + alphaMaskedChunkCount;
    if (chunkIndex >= totalChunks)
        return;

    uint indexCount = meshChunks[chunkIndex].indexCount;
    uint indexBegin = meshChunks[chunkIndex].indexBegin;
    float chunkRadius = meshChunks[chunkIndex].boundingSphere.data.w;
    AAPLBoundingBox3 chunkAABB = meshChunks[chunkIndex].boundingBox;

    [unroll]
    for (uint cascadeIndex = 0; cascadeIndex < cascadeCount; cascadeIndex++)
    {
        // Per-cascade frustum cull (sun-aligned OBB → 6 inward planes)
        if (FrustumCull(cascadeFrustum[cascadeIndex], chunkAABB))
            continue;
        // Per-cascade distance/size cull: skip sub-texel props on far cascades
        if (chunkRadius < cascadeCullThreshold[cascadeIndex].x)
            continue;

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
        }
        else
        {
            uint insertIndex;
            InterlockedAdd(shadowWriteIndex[cascadeIndex * 2 + 1], 1, insertIndex);
            uint globalIdx = cascadeBaseAlphaMask + insertIndex;
            DrawIndexedIndirectCommand cmd = { indexCount, 1, indexBegin, 0, globalIdx };
            shadowDrawParams[globalIdx] = cmd;
            shadowChunkIndices[globalIdx] = chunkIndex;
        }
    }
}
