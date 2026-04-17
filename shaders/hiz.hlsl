// Hi-Z Pyramid Generation Compute Shaders

// --- Copy Depth to Hi-Z (Mip 0) ---
[[vk::binding(0,0)]] Texture2D<float> depthTexture;
[[vk::binding(1,0)]] RWTexture2D<float> hizMip0;

struct PushConstants {
    uint2 srcSize;   // source mip dimensions
    uint2 dstSize;   // output mip dimensions
};

[[vk::push_constant]] PushConstants pushConstants;

[numthreads(8, 8, 1)]
void CopyDepthToHiZ(uint3 DTid : SV_DispatchThreadID)
{
    if (DTid.x >= pushConstants.dstSize.x || DTid.y >= pushConstants.dstSize.y)
        return;
    float d = depthTexture.Load(int3(DTid.xy, 0));
    // In reverse-Z, the depth buffer is cleared to 0.0 (far plane = background/sky).
    // 0.0 is the minimum possible value, so it contaminates MIN downsampling and
    // makes the entire HIZ pyramid 0.0, preventing any occlusion culling.
    // Remap background (d == 0.0) to a sentinel value (2.0) that lies outside the
    // valid NDC depth range [0,1], so MIN downsampling ignores it.
    hizMip0[DTid.xy] = (d == 0.0) ? 2.0 : d;
}

// --- Downsample Hi-Z (MIN for reverse-Z, with edge handling for odd sizes) ---
[[vk::binding(0,1)]] Texture2D<float> prevMip;
[[vk::binding(1,1)]] RWTexture2D<float> currentMip;

[numthreads(8, 8, 1)]
void DownsampleHiZ(uint3 DTid : SV_DispatchThreadID)
{
    if (DTid.x >= pushConstants.dstSize.x || DTid.y >= pushConstants.dstSize.y)
        return;

    uint2 src = DTid.xy * 2;

    // Standard 2x2 block - MIN keeps farthest visible surface in reverse-Z
    float d = prevMip.Load(int3(src, 0));
    d = min(d, prevMip.Load(int3(src + uint2(1, 0), 0)));
    d = min(d, prevMip.Load(int3(src + uint2(0, 1), 0)));
    d = min(d, prevMip.Load(int3(src + uint2(1, 1), 0)));

    // Edge handling: when source has odd dimensions, the last output texel
    // covers a 3-pixel span. Sample the extra column/row/corner to avoid
    // depth leaks at pyramid edges.
    bool edge_x = (DTid.x * 2 == pushConstants.srcSize.x - 3);
    bool edge_y = (DTid.y * 2 == pushConstants.srcSize.y - 3);

    if (edge_x) {
        d = min(d, prevMip.Load(int3(src + uint2(2, 0), 0)));
        d = min(d, prevMip.Load(int3(src + uint2(2, 1), 0)));
    }
    if (edge_y) {
        d = min(d, prevMip.Load(int3(src + uint2(0, 2), 0)));
        d = min(d, prevMip.Load(int3(src + uint2(1, 2), 0)));
    }
    if (edge_x && edge_y)
        d = min(d, prevMip.Load(int3(src + uint2(2, 2), 0)));

    currentMip[DTid.xy] = d;
}
