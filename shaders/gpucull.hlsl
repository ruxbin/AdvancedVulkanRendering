#include "commonstruct.hlsl"

// --- Bindings ---
[[vk::binding(0,0)]] RWStructuredBuffer<DrawIndexedIndirectCommand> drawParams;
[[vk::binding(1,0)]] cbuffer cullParams {
    uint opaqueChunkCount;
    uint alphaMaskedChunkCount;
    uint transparentChunkCount;
    uint totalPointLights;
    uint totalSpotLights;
    uint hizMipLevels;
    float2 screenSize;
    float4x4 viewProjMatrix;
    Frustum frustum;
};

[[vk::binding(2,0)]] StructuredBuffer<AAPLMeshChunk> meshChunks;
[[vk::binding(3,0)]] RWStructuredBuffer<uint> writeIndex;
// writeIndex[0] = opaque visible count
// writeIndex[1] = alpha-masked visible count
// writeIndex[2] = transparent visible count

[[vk::binding(4,0)]] RWStructuredBuffer<uint> chunkIndices;

[[vk::binding(6,0)]] Texture2D<float> hizTexture;
[[vk::binding(7,0)]] SamplerState hizSampler;

// --- Hi-Z Occlusion Test ---
bool IsOccludedByHiZ(AAPLBoundingBox3 aabb)
{
    //if (hizMipLevels == 0)
        return false;

    float minX = 1.0, minY = 1.0, maxX = 0.0, maxY = 0.0;
    float maxZ = 0.0; // nearest point in reverse-Z (highest depth value)

    // All 8 AABB corners
    float3 corners[8] = {
        float3(aabb.min.x, aabb.min.y, aabb.min.z),
        float3(aabb.max.x, aabb.min.y, aabb.min.z),
        float3(aabb.min.x, aabb.max.y, aabb.min.z),
        float3(aabb.max.x, aabb.max.y, aabb.min.z),
        float3(aabb.min.x, aabb.min.y, aabb.max.z),
        float3(aabb.max.x, aabb.min.y, aabb.max.z),
        float3(aabb.min.x, aabb.max.y, aabb.max.z),
        float3(aabb.max.x, aabb.max.y, aabb.max.z)
    };

    bool anyBehindCamera = false;
    [unroll]
    for (int i = 0; i < 8; i++)
    {
        float4 clip = mul(viewProjMatrix, float4(corners[i], 1.0));
        if (clip.w <= 0.0)
        {
            anyBehindCamera = true;
            continue;
        }
        float3 ndc = clip.xyz / clip.w;
        float2 uv = ndc.xy * 0.5 + 0.5;
        uv.y = 1.0 - uv.y; // Vulkan Y flip

        minX = min(minX, uv.x);
        maxX = max(maxX, uv.x);
        minY = min(minY, uv.y);
        maxY = max(maxY, uv.y);
        maxZ = max(maxZ, ndc.z); // nearest point = highest depth in reverse-Z
    }

    // If any corner is behind camera, skip Hi-Z (frustum cull already passed)
    if (anyBehindCamera)
        return false;

    // Clamp to screen
    minX = max(minX, 0.0);
    maxX = min(maxX, 1.0);
    minY = max(minY, 0.0);
    maxY = min(maxY, 1.0);

    if (minX >= maxX || minY >= maxY)
        return false;

    // Choose mip level based on projected pixel size
    float2 pixelSize = float2(maxX - minX, maxY - minY) * screenSize;
    float mipLevel = ceil(log2(max(pixelSize.x, pixelSize.y)));
    mipLevel = clamp(mipLevel, 0.0, (float)(hizMipLevels - 1));

    // Sample Hi-Z at 4 corners of projected rect, take MIN (farthest visible surface)
    float hiz00 = hizTexture.SampleLevel(hizSampler, float2(minX, minY), mipLevel);
    float hiz10 = hizTexture.SampleLevel(hizSampler, float2(maxX, minY), mipLevel);
    float hiz01 = hizTexture.SampleLevel(hizSampler, float2(minX, maxY), mipLevel);
    float hiz11 = hizTexture.SampleLevel(hizSampler, float2(maxX, maxY), mipLevel);
    float hizDepth = min(min(hiz00, hiz10), min(hiz01, hiz11));

    // Reverse-Z: occluded if nearest AABB point (maxZ) <= farthest visible surface (hizDepth)
    return maxZ <= hizDepth;
}

// --- Main Compute Shader ---
[numthreads(128, 1, 1)]
void EncodeDrawBuffer(uint3 DTid : SV_DispatchThreadID)
{
    uint chunkIndex = DTid.x;
    uint totalChunks = opaqueChunkCount + alphaMaskedChunkCount + transparentChunkCount;

    if (chunkIndex >= totalChunks)
        return;

    // Frustum cull
    if (FrustumCull(frustum, meshChunks[chunkIndex].boundingBox))
        return;

    // Hi-Z occlusion cull
    if (IsOccludedByHiZ(meshChunks[chunkIndex].boundingBox))
        return;

    // Visible — determine which category and write to appropriate region
    uint indexCount = meshChunks[chunkIndex].indexCount;
    uint indexBegin = meshChunks[chunkIndex].indexBegin;

    if (chunkIndex < opaqueChunkCount)
    {
        // Opaque region: [0, opaqueChunkCount)
        uint insertIndex;
        InterlockedAdd(writeIndex[0], 1, insertIndex);
        DrawIndexedIndirectCommand cmd = { indexCount, 1, indexBegin, 0, insertIndex };
        drawParams[insertIndex] = cmd;
        chunkIndices[insertIndex] = chunkIndex;
    }
    else if (chunkIndex < opaqueChunkCount + alphaMaskedChunkCount)
    {
        // Alpha-masked region: [opaqueChunkCount, opaqueChunkCount + alphaMaskedChunkCount)
        uint insertIndex;
        InterlockedAdd(writeIndex[1], 1, insertIndex);
        uint offset = opaqueChunkCount;
        DrawIndexedIndirectCommand cmd = { indexCount, 1, indexBegin, 0, offset + insertIndex };
        drawParams[offset + insertIndex] = cmd;
        chunkIndices[offset + insertIndex] = chunkIndex;
    }
    else
    {
        // Transparent region: [opaqueChunkCount + alphaMaskedChunkCount, totalChunks)
        uint insertIndex;
        InterlockedAdd(writeIndex[2], 1, insertIndex);
        uint offset = opaqueChunkCount + alphaMaskedChunkCount;
        DrawIndexedIndirectCommand cmd = { indexCount, 1, indexBegin, 0, offset + insertIndex };
        drawParams[offset + insertIndex] = cmd;
        chunkIndices[offset + insertIndex] = chunkIndex;
    }
}
