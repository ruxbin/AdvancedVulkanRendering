#include "commonstruct.hlsl"

// Set 0: global uniform buffer
[[vk::binding(0,0)]]
cbuffer cam
{
    CameraParamsBufferFull cameraParams;
    AAPLFrameConstants frameConstants;
};

// Set 1: resolve pass textures and samplers
[[vk::binding(0,1)]] Texture2D<float4> hdrBuffer;
[[vk::binding(1,1)]] Texture2D<float4> historyTex;
[[vk::binding(2,1)]] Texture2D<float> depthTex;
[[vk::binding(3,1)]] SamplerState _NearestClampSampler;
[[vk::binding(4,1)]] SamplerState _LinearClampSampler;

struct VSOutput
{
    float4 Position : SV_POSITION;
    float2 TextureUV : TEXCOORD0;
};

// ACES filmic tone mapping
float3 ToneMapACES(float3 x)
{
    float a = 2.51f;
    float b = 0.03f;
    float c = 2.43f;
    float d = 0.59f;
    float e = 0.14f;
    return saturate((x*(a*x+b))/(x*(c*x+d)+e));
}

// Catmull-Rom 5-tap bicubic sampling using bilinear hardware filtering
float3 sampleCatmullRom(float2 uv, float2 texSize, float2 invTexSize)
{
    float2 position = texSize * uv;
    float2 centerPosition = floor(position - 0.5f) + 0.5f;

    float2 f  = position - centerPosition;
    float2 f2 = f * f;
    float2 f3 = f * f2;

    float cc = 0.5f;

    float2 w0 = -cc * f3 + 2.0f * cc * f2 - cc * f;
    float2 w1 = (2.0f - cc) * f3 - (3.0f - cc) * f2 + 1.0f;
    float2 w2 = -(2.0f - cc) * f3 + (3.0f - 2.0f * cc) * f2 + cc * f;
    float2 w3 = cc * f3 - cc * f2;

    float2 w12 = w1 + w2;
    float2 tc12 = (centerPosition + w2 / w12) * invTexSize;
    float2 tc0 = (centerPosition - 1.0f) * invTexSize;
    float2 tc3 = (centerPosition + 2.0f) * invTexSize;

    float3 centerColor = historyTex.SampleLevel(_LinearClampSampler, float2(tc12.x, tc12.y), 0).rgb;
    float3 s0 = historyTex.SampleLevel(_LinearClampSampler, float2(tc12.x, tc0.y), 0).rgb;
    float3 s1 = historyTex.SampleLevel(_LinearClampSampler, float2(tc0.x, tc12.y), 0).rgb;
    float3 s2 = historyTex.SampleLevel(_LinearClampSampler, float2(tc3.x, tc12.y), 0).rgb;
    float3 s3 = historyTex.SampleLevel(_LinearClampSampler, float2(tc12.x, tc3.y), 0).rgb;

    float4 color = float4(s0, 1.0f) * (w12.x * w0.y)
                 + float4(s1, 1.0f) * (w0.x * w12.y)
                 + float4(centerColor, 1.0f) * (w12.x * w12.y)
                 + float4(s2, 1.0f) * (w3.x * w12.y)
                 + float4(s3, 1.0f) * (w12.x * w3.y);

    return color.rgb / color.a;
}

// Full-screen triangle vertex shader (same as deferred lighting)
VSOutput ResolveVS(uint vid : SV_VertexID)
{
    VSOutput output;
    output.TextureUV = float2((vid << 1) & 2, vid & 2);
    output.Position = float4(output.TextureUV * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 1.0f, 1.0f);
    output.TextureUV.y = 1.0f - output.TextureUV.y;
    return output;
}

struct ResolveOutput
{
    float4 color : SV_Target0;    // swapchain
    float4 history : SV_Target1;  // history buffer
};

ResolveOutput ResolvePS(VSOutput input)
{
    ResolveOutput output;

    float2 texelSize = frameConstants.invPhysicalSize;

    // Sample current HDR pixel
    float3 center = hdrBuffer.SampleLevel(_NearestClampSampler, input.TextureUV, 0).rgb;

    // Apply ACES tone mapping
    float3 result = ToneMapACES(center * frameConstants.exposure);

    if (frameConstants.taaEnabled)
    {
        // 3x3 neighborhood sampling for color clamping
        float3 n0 = hdrBuffer.SampleLevel(_NearestClampSampler, input.TextureUV + float2(-texelSize.x, -texelSize.y), 0).rgb;
        float3 n1 = hdrBuffer.SampleLevel(_NearestClampSampler, input.TextureUV + float2(0, -texelSize.y), 0).rgb;
        float3 n2 = hdrBuffer.SampleLevel(_NearestClampSampler, input.TextureUV + float2(texelSize.x, -texelSize.y), 0).rgb;
        float3 n3 = hdrBuffer.SampleLevel(_NearestClampSampler, input.TextureUV + float2(-texelSize.x, 0), 0).rgb;
        float3 n5 = hdrBuffer.SampleLevel(_NearestClampSampler, input.TextureUV + float2(texelSize.x, 0), 0).rgb;
        float3 n6 = hdrBuffer.SampleLevel(_NearestClampSampler, input.TextureUV + float2(-texelSize.x, texelSize.y), 0).rgb;
        float3 n7 = hdrBuffer.SampleLevel(_NearestClampSampler, input.TextureUV + float2(0, texelSize.y), 0).rgb;
        float3 n8 = hdrBuffer.SampleLevel(_NearestClampSampler, input.TextureUV + float2(texelSize.x, texelSize.y), 0).rgb;

        // Compute 3x3 neighborhood mean and variance in tone-mapped space.
        // Variance clamping avoids the tight min/max box that kills history in
        // uniform shadow regions (where all 9 taps have nearly the same dark
        // value and any history from a lit frame gets hard-clamped to shadow).
        float3 taps[9] = {
            ToneMapACES(n0 * frameConstants.exposure),
            ToneMapACES(n1 * frameConstants.exposure),
            ToneMapACES(n2 * frameConstants.exposure),
            ToneMapACES(n3 * frameConstants.exposure),
            ToneMapACES(center * frameConstants.exposure),
            ToneMapACES(n5 * frameConstants.exposure),
            ToneMapACES(n6 * frameConstants.exposure),
            ToneMapACES(n7 * frameConstants.exposure),
            ToneMapACES(n8 * frameConstants.exposure)
        };
        float3 m1 = 0.0f, m2 = 0.0f;
        [unroll] for (int i = 0; i < 9; i++) { m1 += taps[i]; m2 += taps[i] * taps[i]; }
        m1 /= 9.0f;
        m2 /= 9.0f;
        float3 sigma = sqrt(max(m2 - m1 * m1, 0.0f));
        float k = 1.25f;

        // Reconstruct world position from depth
        float depth = depthTex.SampleLevel(_NearestClampSampler, input.TextureUV, 0);
        float4 worldPos = worldPositionForTexcoord(input.TextureUV, depth, cameraParams);

        // Reproject to previous frame
        float4 prevClip = mul(cameraParams.prevViewProjectionMatrix, float4(worldPos.xyz, 1.0f));
        prevClip.xyz /= prevClip.w;

        float2 prevUV = prevClip.xy * 0.5f + 0.5f;

        // Sample history with Catmull-Rom
        float3 historySample = sampleCatmullRom(prevUV, frameConstants.physicalSize, frameConstants.invPhysicalSize);

        // Clamp history to variance neighbourhood (mean ± k*sigma).
        historySample = clamp(historySample, m1 - k * sigma, m1 + k * sigma);

        float blendFactor = 0.95f;

        // Reject history for off-screen reprojection
        if (any(abs(prevClip.xy) > 1.0f))
            blendFactor = 0.0f;

        result = lerp(result, historySample, blendFactor);
    }

    output.color = float4(result, 1.0f);
    output.history = float4(result, 1.0f);
    return output;
}
