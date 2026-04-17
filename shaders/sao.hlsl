// Scalable Ambient Obscurance (SAO) Compute Shader
// Ported from AAPLAmbientObscurance.metal (ModernRenderingWithMetal)
// Adapted for Vulkan reverse-Z (near=1, far=0)

#include "commonstruct.hlsl"

// --- Bindings ---
[[vk::binding(0,0)]] Texture2D<float> depthTexture;        // Full-resolution scene depth
[[vk::binding(1,0)]] Texture2D<float> depthMipTexture;     // Mipped depth pyramid (half-res start)
[[vk::binding(2,0)]] cbuffer cam {
    CameraParamsBufferFull cameraParams;
    AAPLFrameConstants frameData;
};
[[vk::binding(3,0)]] 
[[vk::image_format("r8")]] 
RWTexture2D<float> aoOutput;          // Output AO texture (R8_UNORM)

struct SAOPushConstants {
    uint2 screenSize;
};
[[vk::push_constant]] SAOPushConstants pushConstants;

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
    ndc.y *= -1.0; // Vulkan Y flip

    float4x4 invProj = cameraParams.invProjectionMatrix;
    float4 cameraSpacePosition = float4(
        ndc.x * invProj[0][0] + invProj[3][0],
        ndc.y * invProj[1][1] + invProj[3][1],
        1.0,
        depth * invProj[2][3] + invProj[3][3]
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
    ndc.y *= -1.0;

    float4x4 invProj = cameraParams.invProjectionMatrix;
    return float2(
        ndc.x * invProj[0][0] + invProj[3][0],
        ndc.y * invProj[1][1] + invProj[3][1]
    );
}

// Reconstruct camera-space position from base + offset + depth
float3 GetOffsetCameraSpacePosition(float2 baseInCameraSpace, float2 offsetInScreenSpace, float depth)
{
    float4x4 invProj = cameraParams.invProjectionMatrix;
    float2 ndcScale = float2(2.0, -2.0) / float2(pushConstants.screenSize);
    float2 cameraSpaceScale = ndcScale * float2(invProj[0][0], invProj[1][1]);

    float4 cameraSpacePosition = float4(
        baseInCameraSpace + offsetInScreenSpace * cameraSpaceScale,
        1.0,
        depth * invProj[2][3] + invProj[3][3]
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
    float depth = depthTexture.Load(int3(coordinates, 0));
    // Reverse-Z: 0 = far plane, skip sky pixels
    if (depth <= 0.0001)
    {
        aoOutput[coordinates] = 1.0;
        return;
    }

    float3 cameraPosition = GetCameraSpacePositionFromDepth(coordinates, depth);
    float2 cameraBasePosition = GetCameraSpaceBasePosition(coordinates);

    // --- Reconstruct normal from 4-neighbor cross product ---
    float depthd = depthTexture.Load(int3(coordinates + uint2(0, 1), 0));
    float depthr = depthTexture.Load(int3(coordinates + uint2(1, 0), 0));
    float depthu = depthTexture.Load(int3(coordinates - uint2(0, 1), 0));
    float depthl = depthTexture.Load(int3(coordinates - uint2(1, 0), 0));

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

        // Adaptive mip selection: far samples use coarser mips for cache efficiency
        int mipLevel = int(log2(max(abs(offset.x), abs(offset.y)))) - 3;
        mipLevel = clamp(mipLevel, 0, 6);

        // Read from depth pyramid (half-res start, so shift by mipLevel+1)
        float depth2 = depthMipTexture.Load(int3(xy >> (mipLevel + 1), mipLevel));

        // Skip far-plane pixels (reverse-Z: 0 = far)
        if (depth2 <= 0.0001)
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
