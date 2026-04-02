// Hi-Z Pyramid Generation Compute Shaders

// --- Copy Depth to Hi-Z (Mip 0) ---
[[vk::binding(0,0)]] Texture2D<float> depthTexture;
[[vk::binding(1,0)]] RWTexture2D<float> hizMip0;

[[vk::push_constant]] cbuffer PushConstants {
    uint2 mipSize;
};

[numthreads(8, 8, 1)]
void CopyDepthToHiZ(uint3 DTid : SV_DispatchThreadID)
{
    if (DTid.x >= mipSize.x || DTid.y >= mipSize.y)
        return;
    hizMip0[DTid.xy] = depthTexture.Load(int3(DTid.xy, 0));
}

// --- Downsample Hi-Z (MIN for reverse-Z) ---
[[vk::binding(0,1)]] Texture2D<float> prevMip;
[[vk::binding(1,1)]] RWTexture2D<float> currentMip;

[numthreads(8, 8, 1)]
void DownsampleHiZ(uint3 DTid : SV_DispatchThreadID)
{
    if (DTid.x >= mipSize.x || DTid.y >= mipSize.y)
        return;

    float d00 = prevMip.Load(int3(DTid.xy * 2 + uint2(0, 0), 0));
    float d10 = prevMip.Load(int3(DTid.xy * 2 + uint2(1, 0), 0));
    float d01 = prevMip.Load(int3(DTid.xy * 2 + uint2(0, 1), 0));
    float d11 = prevMip.Load(int3(DTid.xy * 2 + uint2(1, 1), 0));

    // MIN: keep farthest visible surface (smallest depth in reverse-Z)
    currentMip[DTid.xy] = min(min(d00, d10), min(d01, d11));
}
