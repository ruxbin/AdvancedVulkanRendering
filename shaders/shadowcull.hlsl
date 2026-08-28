// Shadow Cascade Culling Compute Shader
#include "commonstruct.hlsl"
#include "shadercompat.hlsl"

VK_BINDING(0,0) RWStructuredBuffer<DrawIndexedIndirectCommand> shadowDrawParams REGISTER_UAV(0,0);
VK_BINDING(1,0) cbuffer shadowCullParams REGISTER_CBV(1,0) {
    uint opaqueChunkCount;
    uint alphaMaskedChunkCount;
    uint cascadeMaxChunks; // max chunks per category per cascade
    uint cascadeCount;
    // .x = min boundingSphere radius below which the chunk is skipped
    // for this cascade. Lets far cascades drop sub-texel props.
    float4 cascadeCullThreshold[SHADOW_CASCADE_COUNT];
    Frustum cascadeFrustum[SHADOW_CASCADE_COUNT];
};
VK_BINDING(2,0) StructuredBuffer<AAPLMeshChunk> meshChunks REGISTER_SRV(2,0);
VK_BINDING(3,0) RWStructuredBuffer<uint> shadowWriteIndex REGISTER_UAV(3,0);
// shadowWriteIndex[cascadeIndex * 2 + 0] = opaque count for this cascade
// shadowWriteIndex[cascadeIndex * 2 + 1] = alpha-masked count for this cascade
VK_BINDING(4,0) RWStructuredBuffer<uint> shadowChunkIndices REGISTER_UAV(4,0);

#define SHADOW_CULL_TG_SIZE 128
#define SHADOW_BUCKETS_PER_CASCADE 2   // 0 = opaque, 1 = alpha-masked
#define SHADOW_SLOTS_PER_THREAD (SHADOW_CASCADE_COUNT * SHADOW_BUCKETS_PER_CASCADE)

// Threadgroup memory for intra-TG draw-call merging (Apple-style).
// g_visibleBits[t]: bit (c*2 + b) set => thread t's chunk is visible in (cascade c, bucket b).
// g_indexBegin/Count/Material/Bucket: shadow copies of the chunk fields so we don't re-touch SSBO in phase 2.
// g_absorbed[t * SLOTS + c*2 + b]: this chunk's indexCount when the chunk is absorbed by t-1 in (c, b); 0 otherwise.
groupshared uint g_visibleBits   [SHADOW_CULL_TG_SIZE];
groupshared uint g_indexBegin    [SHADOW_CULL_TG_SIZE];
groupshared uint g_indexCount    [SHADOW_CULL_TG_SIZE];
groupshared uint g_materialIndex [SHADOW_CULL_TG_SIZE];
groupshared uint g_bucket        [SHADOW_CULL_TG_SIZE];
groupshared uint g_absorbed      [SHADOW_CULL_TG_SIZE * SHADOW_SLOTS_PER_THREAD];

[numthreads(SHADOW_CULL_TG_SIZE, 1, 1)]
void ShadowCull(uint3 DTid : SV_DispatchThreadID, uint3 GTid : SV_GroupThreadID)
{
    uint chunkIndex = DTid.x;
    uint tg         = GTid.x;
    uint totalChunks = opaqueChunkCount + alphaMaskedChunkCount;
    bool valid = (chunkIndex < totalChunks);

    // ------------------------------------------------------------------
    // Phase 1: per-cascade visibility + cascade-de-duplication.
    // Walk cascades from fine (0) to coarse (N-1). After a cascade passes
    // and fully contains the chunk's AABB, skip all coarser cascades: any
    // pixel covered by them inside this cascade's frustum will sample the
    // finer cascade first (per evaluateCascadeShadows' first-hit rule), so
    // a duplicate caster on the coarser map would never be sampled.
    // ------------------------------------------------------------------
    uint visibleBits     = 0;
    uint myIndexBegin    = 0;
    uint myIndexCount    = 0;
    uint myMaterialIndex = 0;
    uint myBucket        = 0;

    if (valid)
    {
        AAPLMeshChunk chunk = meshChunks[chunkIndex];
        myIndexCount    = chunk.indexCount;
        myIndexBegin    = chunk.indexBegin;
        myMaterialIndex = chunk.materialIndex;
        myBucket        = (chunkIndex < opaqueChunkCount) ? 0u : 1u;

        float chunkRadius = chunk.boundingSphere.data.w;
        AAPLBoundingBox3 chunkAABB = chunk.boundingBox;

        [unroll]
        for (uint c = 0; c < SHADOW_CASCADE_COUNT; c++)
        {
            if (c >= cascadeCount) break;
            if (FrustumCull(cascadeFrustum[c], chunkAABB))    continue;
            if (chunkRadius < cascadeCullThreshold[c].x)      continue;

            visibleBits |= (1u << (c * 2u + myBucket));

            // Cascade-de-dup: stop at the first cascade that fully covers us.
            if (FrustumContains(cascadeFrustum[c], chunkAABB))
                break;
        }
    }

    // Publish phase-1 results to TG memory.
    g_visibleBits  [tg] = visibleBits;
    g_indexBegin   [tg] = myIndexBegin;
    g_indexCount   [tg] = myIndexCount;
    g_materialIndex[tg] = myMaterialIndex;
    g_bucket       [tg] = myBucket;

    [unroll]
    for (uint s = 0; s < SHADOW_SLOTS_PER_THREAD; s++)
        g_absorbed[tg * SHADOW_SLOTS_PER_THREAD + s] = 0;

    GroupMemoryBarrierWithGroupSync();

    // ------------------------------------------------------------------
    // Phase 2: per-(cascade,bucket) "can previous chunk eat me?" check.
    // Mergeable iff: same bucket, contiguous indexBegin, both visible in
    // (c, bucket), and for alpha-mask additionally same material. (Opaque
    // shadow pass is depth-only; material is irrelevant there.)
    // ------------------------------------------------------------------
    if (valid && tg > 0)
    {
        uint prevVisible  = g_visibleBits  [tg - 1];
        uint prevIndexBegin = g_indexBegin [tg - 1];
        uint prevIndexCount = g_indexCount [tg - 1];
        uint prevMaterial = g_materialIndex[tg - 1];
        uint prevBucket   = g_bucket       [tg - 1];

        bool sameBucket  = (prevBucket == myBucket);
        bool contiguous  = (myIndexBegin == prevIndexBegin + prevIndexCount);
        bool materialOk  = (myBucket == 0u) || (myMaterialIndex == prevMaterial);
        bool baseMerge   = sameBucket && contiguous && materialOk;

        if (baseMerge)
        {
            [unroll]
            for (uint c = 0; c < SHADOW_CASCADE_COUNT; c++)
            {
                if (c >= cascadeCount) break;
                uint bit = 1u << (c * 2u + myBucket);
                if ((prevVisible & bit) && (visibleBits & bit))
                    g_absorbed[tg * SHADOW_SLOTS_PER_THREAD + c * 2u + myBucket] = myIndexCount;
            }
        }
    }

    GroupMemoryBarrierWithGroupSync();

    if (!valid) return;

    // ------------------------------------------------------------------
    // Phase 3: emit at most one draw per (cascade,bucket) this thread owns.
    // A thread "owns" the merged draw when it's visible in (c,b) but NOT
    // absorbed by its predecessor. It then walks forward in the TG eating
    // absorbed neighbours until the chain breaks.
    // ------------------------------------------------------------------
    uint groupStart = DTid.x - tg;
    uint endTG = min((uint)SHADOW_CULL_TG_SIZE, totalChunks - groupStart);

    [unroll]
    for (uint c = 0; c < SHADOW_CASCADE_COUNT; c++)
    {
        if (c >= cascadeCount) break;
        uint slot = c * 2u + myBucket;
        uint bit  = 1u << slot;
        if ((visibleBits & bit) == 0u) continue;
        if (g_absorbed[tg * SHADOW_SLOTS_PER_THREAD + slot] != 0u) continue;

        uint totalIndexCount = myIndexCount;
        for (uint j = tg + 1u; j < endTG; j++)
        {
            uint extra = g_absorbed[j * SHADOW_SLOTS_PER_THREAD + slot];
            if (extra == 0u) break;
            totalIndexCount += extra;
        }

        uint cascadeBase = c * cascadeMaxChunks * 2u
                         + ((myBucket == 0u) ? 0u : cascadeMaxChunks);

        uint insertIndex;
        InterlockedAdd(shadowWriteIndex[slot], 1, insertIndex);
        uint globalIdx = cascadeBase + insertIndex;

        DrawIndexedIndirectCommand cmd = MAKE_DRAW_CMD(globalIdx, totalIndexCount, myIndexBegin);
        shadowDrawParams [globalIdx] = cmd;
        shadowChunkIndices[globalIdx] = chunkIndex;
    }
}
