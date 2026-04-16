// Hi-Z Pyramid Generation Compute Shaders
#include "shadercompat.hlsl"

// --- Copy Depth to Hi-Z (Mip 0) ---
VK_BINDING(0,0) Texture2D<float> depthTexture REGISTER_SRV(0,0);
VK_BINDING(1,0) RWTexture2D<float> hizMip0 REGISTER_UAV(1,0);

struct PushConstants {
    uint2 srcSize;   // source mip dimensions
    uint2 dstSize;   // output mip dimensions
};

DECLARE_PUSH_CONSTANTS(PushConstants, pushConstants, 0);

[numthreads(8, 8, 1)]
void CopyDepthToHiZ(uint3 DTid : SV_DispatchThreadID)
{
    if (DTid.x >= pushConstants.dstSize.x || DTid.y >= pushConstants.dstSize.y)
        return;
    hizMip0[DTid.xy] = depthTexture.Load(int3(DTid.xy, 0));
}

// --- Downsample Hi-Z (MIN for reverse-Z, with edge handling for odd sizes) ---
VK_BINDING(0,1) Texture2D<float> prevMip REGISTER_SRV(0,1);
VK_BINDING(1,1) RWTexture2D<float> currentMip REGISTER_UAV(1,1);

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
