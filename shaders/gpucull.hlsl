#include "commonstruct.hlsl"




[[vk::binding(0,0)]] RWStructuredBuffer<DrawIndexedIndirectCommand> drawParams;
[[vk::binding(1,0)]] cbuffer cullParams {
    uint totalChunks;
    uint totalPointLights;
    uint totalSpotLights;
    Frustum frustum;
}

[[vk::binding(2,0)]] StructuredBuffer<AAPLMeshChunk> meshChunks;
[[vk::binding(3,0)]] RWStructuredBuffer<uint> writeIndex;
[[vk::binding(4,0)]] RWStructuredBuffer<uint> chunkIndices;

[[vk::binding(5,0)]] RWStructuredBuffer<uint> instanceToDrawIDMap;

groupshared uint visible[128];
//TODO: 合批

[numthreads(128, 1, 1)]
void EncodeDrawBuffer(uint3 DTid : SV_DispatchThreadID,uint3 GTid:SV_GroupThreadID)
{
    uint chunkIndex = DTid.x;
    uint groupThreadIndex = GTid.x;
   visible[groupThreadIndex] = 0;
   if(chunkIndex<totalChunks)
   {
    if (!FrustumCull(frustum, meshChunks[chunkIndex].boundingBox))
    {
    visible[groupThreadIndex]=1;
    uint insertIndex = 0;
	InterlockedAdd(writeIndex[0],1,insertIndex);
            DrawIndexedIndirectCommand drawParam = { meshChunks[chunkIndex].indexCount, 1, meshChunks[chunkIndex].indexBegin, 0, insertIndex };
    drawParams[insertIndex] = drawParam;
	chunkIndices[insertIndex] = chunkIndex;
            instanceToDrawIDMap[insertIndex] = insertIndex;
	//{
            //.indexCount = meshChunks[chunkIndex].indexCount,.instanceCount = 1, .firstIndex = meshChunks[chunkIndex].indexBegin, vertexOffset = 0, firstInstance = 0

        //}

    }
	}
}
