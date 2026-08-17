// Scalable Ambient Obscurance (SAO) Compute Shader
// Ported from AAPLAmbientObscurance.metal (ModernRenderingWithMetal)
// Adapted for Vulkan reverse-Z (near=1, far=0)

#include "shadercompat.hlsl"
#include "commonstruct.hlsl"

// --- Bindings ---
// DX12 root signature (DX12GpuScene.cpp CreateRootSignatures):
//   [0] table: SRV t0-t1, UAV u3
//   [1] CBV b2 (camera params)
//   [2] root constants b3 (screenSize)
// SAO depth pyramid (R32_SFLOAT, full-res mip 0).  Serves both as
// the source of centre-pixel depth (mip 0 Load) and as the mip chain
// for coarse-level lookups.
VK_BINDING(0,0) Texture2D<float> depthMipTexture REGISTER_SRV(0,0);

VK_BINDING(1,0) cbuffer cam REGISTER_CBV(2,0) {
    CameraParamsBufferFull cameraParams;
    AAPLFrameConstants frameData;
};
VK_BINDING(2,0)
#if !defined(DX12_BACKEND)
[[vk::image_format("r8")]]
#endif
RWTexture2D<float> aoOutput REGISTER_UAV(3,0);    // Output AO texture (R8_UNORM)

struct SAOPushConstants {
    uint2 screenSize;
};
DECLARE_PUSH_CONSTANTS(SAOPushConstants, pushConstants, 3);

// Wang hash for per-pixel pseudo-random dithering
uint wang_hash(uint seed)
{
    seed = (seed ^ 61) ^ (seed >> 16);
    seed *= 9;
    seed = seed ^ (seed >> 4);
    seed *= 0x27d4eb2d;
    seed = seed ^ (seed >> 15);
    return seed;
}

// Reconstruct camera-space position from pixel coordinates and depth
float3 GetCameraSpacePositionFromDepth(uint2 coordinates, float depth)
{
    float2 ndc;
    ndc.xy = (float2(coordinates) + 0.5) / float2(pushConstants.screenSize);
    ndc.xy = ndc.xy * 2.0 - 1.0;
    // 不需要 Vulkan Y flip:窗口深度由正 viewport 渲染,
    // 跟 deferredlighting 的 worldPositionForTexcoord 一致(都不翻 Y)。

    // HLSL/SPIR-V 列向量约定:invProj[i][j] = row i, col j。
    // reverse-Z perspective 的逆矩阵第 3 行是 (0, 0, 1/d, -c/d),
    // 所以 depth 的系数是 invProj[3][2],不是 invProj[2][3]。
    // (从 .metal port 时少做了一次行列索引转置)
    float4x4 invProj = cameraParams.invProjectionMatrix;
    float4 cameraSpacePosition = float4(
        ndc.x * invProj[0][0] + invProj[0][3],
        ndc.y * invProj[1][1] + invProj[1][3],
        1.0,
        depth * invProj[3][2] + invProj[3][3]
    );
    cameraSpacePosition.xyz /= cameraSpacePosition.w;

    return cameraSpacePosition.xyz;
}

// Get XY camera-space base (without depth), for efficient offset calculation
float2 GetCameraSpaceBasePosition(uint2 coordinates)
{
    float2 ndc;
    ndc.xy = (float2(coordinates) + 0.5) / float2(pushConstants.screenSize);
    ndc.xy = ndc.xy * 2.0 - 1.0;

    float4x4 invProj = cameraParams.invProjectionMatrix;
    return float2(
        ndc.x * invProj[0][0] + invProj[0][3],
        ndc.y * invProj[1][1] + invProj[1][3]
    );
}

// Reconstruct camera-space position from base + offset + depth
float3 GetOffsetCameraSpacePosition(float2 baseInCameraSpace, float2 offsetInScreenSpace, float depth)
{
    float4x4 invProj = cameraParams.invProjectionMatrix;
    // ndcScale: 像素位移 → NDC 位移。Y 不翻(同上)。
    float2 ndcScale = float2(2.0, 2.0) / float2(pushConstants.screenSize);
    float2 cameraSpaceScale = ndcScale * float2(invProj[0][0], invProj[1][1]);

    float4 cameraSpacePosition = float4(
        baseInCameraSpace + offsetInScreenSpace * cameraSpaceScale,
        1.0,
        depth * invProj[3][2] + invProj[3][3]
    );
    cameraSpacePosition.xyz /= cameraSpacePosition.w;

    return cameraSpacePosition.xyz;
}

// Rotate 2D vector by (cos, sin) pair
float2 RotateVector(float2 v, float2 rotation)
{
    return float2(
        v.x * rotation.x - v.y * rotation.y,
        v.x * rotation.y + v.y * rotation.x
    );
}

[numthreads(8, 8, 1)]
void ScalableAmbientObscurance(uint3 DTid : SV_DispatchThreadID)
{
    uint2 coordinates = DTid.xy;
    if (coordinates.x >= pushConstants.screenSize.x || coordinates.y >= pushConstants.screenSize.y)
        return;

    // --- Parameters ---
    const bool temporal = true;
    const int temporalFrames = temporal ? 4 : 1;
    int tapCount = 36 / temporalFrames; // 9 taps per frame

    // Radius: ~1 meter projected to pixels = 0.5 * height * proj[1][1]
    const float radius = 0.5 * pushConstants.screenSize.y * cameraParams.projectionMatrix[1][1];
    const uint numSpirals = 11;
    const float bias = 0.001;
    const float epsilon = 0.01;
    const float intensity = 1.0;

    // --- Center pixel ---
    // Read from SAO depth pyramid mip 0 (R32_SFLOAT), not the raw depth
    // texture, so that Load() works on all Vulkan drivers.
    float depth = depthMipTexture.Load(int3(coordinates, 0));
    // Reverse-Z: depth == 0 = far plane = sky / not rendered. 严格用 0 判定;
    // 之前用 `<= 0.0001` 会误把 ~90m 远的合法几何当成 sky 剔除(reverse-Z
    // 在 n=0.1, f=100 下,depth=0.0001 对应 view.z ≈ 90m),Bistro 街景里
    // 这片几何很多。
    if (depth == 0.0)
    {
        aoOutput[coordinates] = 1.0;
        return;
    }

    float3 cameraPosition = GetCameraSpacePositionFromDepth(coordinates, depth);
    float2 cameraBasePosition = GetCameraSpaceBasePosition(coordinates);

    // --- Reconstruct normal from 4-neighbor cross product ---
    float depthd = depthMipTexture.Load(int3(coordinates + uint2(0, 1), 0));
    float depthr = depthMipTexture.Load(int3(coordinates + uint2(1, 0), 0));
    float depthu = depthMipTexture.Load(int3(coordinates - uint2(0, 1), 0));
    float depthl = depthMipTexture.Load(int3(coordinates - uint2(1, 0), 0));

    float3 cameraPositiond = GetOffsetCameraSpacePosition(cameraBasePosition, float2(0, 1), depthd);
    float3 cameraPositionr = GetOffsetCameraSpacePosition(cameraBasePosition, float2(1, 0), depthr);
    float3 cameraPositionu = GetOffsetCameraSpacePosition(cameraBasePosition, float2(0, -1), depthu);
    float3 cameraPositionl = GetOffsetCameraSpacePosition(cameraBasePosition, float2(-1, 0), depthl);

    // Use shorter edge to avoid depth discontinuity artifacts
    float3 dyu = cameraPositionu - cameraPosition;
    float3 dyd = cameraPositiond - cameraPosition;
    float3 dy = (dot(dyd, dyd) < dot(dyu, dyu)) ? dyd : -dyu;
    float3 dxr = cameraPositionr - cameraPosition;
    float3 dxl = cameraPositionl - cameraPosition;
    float3 dx = (dot(dxl, dxl) < dot(dxr, dxr)) ? -dxl : dxr;
    float3 normal = normalize(cross(dx, dy));

    // --- Sampling ---
    float sum = 0.0;
    int taps = 0;
    uint seed = ((coordinates.y << 16) | coordinates.x) * 100;

    if (temporal)
        seed += frameData.frameCounter;

    float dither = wang_hash(seed) / float(0xFFFFFFFF);

    float discSize = radius / cameraPosition.z;
    float angleIncrement = (numSpirals * M_PI_F * 2.0) / tapCount;
    float2 tapRotation = float2(cos(angleIncrement), sin(angleIncrement));

    float initialAngle = dither * M_PI_F * 2.0;
    float2 offsetDirection = float2(cos(initialAngle), sin(initialAngle));

    float alpha = dither / tapCount;

    for (int i = 0; i < tapCount; i++, alpha += 1.0 / tapCount, offsetDirection = RotateVector(offsetDirection, tapRotation))
    {
        float offsetScale = alpha * alpha; // Square to bias towards origin

        float2 offset = floor(offsetDirection * offsetScale * discSize);
        int2 xy = int2(coordinates) + int2(offset);
        if (any(xy < int2(0, 0)) || any(xy >= int2(pushConstants.screenSize)))
            continue;

        // Adaptive mip selection: far samples use coarser mips for cache efficiency.
        // Pyramid mip 0 is full-res (unlike Apple's half-res Metal variant), so
        // coordinates are shifted by mipLevel (not mipLevel+1).
        int mipLevel = int(log2(max(abs(offset.x), abs(offset.y)))) - 3;
        mipLevel = clamp(mipLevel, 0, 6);

        float depth2 = depthMipTexture.Load(int3(xy >> mipLevel, mipLevel));

        // Skip far-plane pixels (reverse-Z: 0 = far). 严格 0 判定,理由同 center pixel。
        if (depth2 == 0.0)
            continue;

        float3 cameraPosition2 = GetOffsetCameraSpacePosition(cameraBasePosition, offset, depth2);
        float3 v = cameraPosition2 - cameraPosition;

        float vv = dot(v, v);
        float vn = dot(v, normal);
        sum += max((vn - bias) / (epsilon + vv), 0.0);
        taps++;
    }

    float x = (taps > 0) ? max(0.0, 1.0 - sum * intensity * (1.0 / taps)) : 1.0;
    aoOutput[coordinates] = x;
}
