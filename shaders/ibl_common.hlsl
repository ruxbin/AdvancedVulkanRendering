// ibl_common.hlsl — IBL 采样(deferred/forward 共用;后端中立,
// 调用方自行用 #ifndef DX12_BACKEND 保护,DX12 移植完成后移除 guard)。
// 数学与 Apple AAPLLightingCommon.h 的 evaluateShCoefficients + IBL() 一致,
// 差异:env cube 直接存线性 HDR,无 RGBM 6.0*rgb*a 解码;
// 且本移植的 SH 系数由标准辐亮度投影得到(ComputeSH9FromEquirect),
// 因此按 +N 求值——Apple 硬编码系数是取反约定才需要 -N。

float3 evaluateShCoefficients(float3 n, float4 sh[9])
{
    return sh[0].rgb
         + sh[1].rgb * (n.y)
         + sh[2].rgb * (n.z)
         + sh[3].rgb * (n.x)
         + sh[4].rgb * (n.y * n.x)
         + sh[5].rgb * (n.y * n.z)
         + sh[6].rgb * (3.0 * n.z * n.z - 1.0)
         + sh[7].rgb * (n.z * n.x)
         + sh[8].rgb * (n.x * n.x - n.y * n.y);
}

float3 IBL(AAPLPixelSurfaceData surface,
           TextureCube envMap,
           Texture2D<float2> dfgLut,
           SamplerState samp,
           float4 sh[9],
           float3 viewDir,
           float scale,
           float specularScale)
{
    float3 diffuseIBL = evaluateShCoefficients((float3)surface.normal, sh) * (float3)surface.albedo;

    float perceptualRoughness = (float)surface.roughness;
    float NoV = max(dot((float3)surface.normal, viewDir), 0.0);
    float3 r = reflect(-viewDir, (float3)surface.normal);

    const float mipLevels = 8.0; // 9 mip 链的最后一级(256..1)
    float lod = perceptualRoughness * mipLevels;
    float3 indirectSpecular = envMap.SampleLevel(samp, r, lod).rgb;
    // fp16 RT 上限 65504:8192 × specularColor(≈1.15) × iblSpecularScale(4) ≈ 37.6k,留有余量;
    // 不夹的话太阳 texel 会在 half3 写入时变 inf,ACES inf/inf=NaN 并被 TAA 扩散。
    indirectSpecular = min(indirectSpecular, 8192.0);

    float2 dfg = dfgLut.SampleLevel(samp, float2(NoV, perceptualRoughness), 0);
    float3 specularColor = (float3)surface.F0 * dfg.x + dfg.y;

    float3 specularIBL = indirectSpecular * specularColor;
    return (diffuseIBL + specularIBL * specularScale) * scale;
}
