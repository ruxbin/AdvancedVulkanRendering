#include "commonstruct.hlsl"
#include "lighting.hlsl"
#include "ibl_common.hlsl"
#ifndef DX12_BACKEND
#include "vlm_common.hlsl"
#endif
#include "shadercompat.hlsl"


struct GeometyBuffer
{
    Buffer<float3> positions;
    Buffer<float3> normals;
    Buffer<float2> uvs;
    Buffer<float3> tangents;
    Buffer<uint> indices;
};



struct PushConstants
{
    uint materialIndex;
    uint shadowIndex;
#ifdef DX12_BACKEND
    // Written per-draw by the [CONSTANT(b1), DRAW_INDEXED] command signature.
    // Vulkan gets the same value through firstInstance/gl_InstanceIndex, so the
    // field only exists on the DX12 side (keeps the VK push-constant block size
    // unchanged).
    uint drawSlot;
#endif
};



struct AAPLShaderMaterial
{
    uint albedo_texture_index;
    uint roughness_texture_index;
    uint normal_texture_index;
    uint emissive_texture_index;
    float alpha;
    uint hasMetallicRoughness;
    uint hasEmissive;
    uint padding;
// #if SUPPORT_SPARSE_TEXTURES //TODO:
//     uint baseColorMip;
//     uint metallicRoughnessMip;
//     uint normalMip;
//     uint emissiveMip;
// #endif
};

[[vk::constant_id(0)]] const bool  specAlphaMask  = false;
//[[vk::constant_id(1)]] const bool specTransparent = false;

VK_BINDING(0,0)
cbuffer cam REGISTER_CBV(0,0)
{
    CameraParamsBufferFull cameraParams;
    AAPLFrameConstants frameConstants;
}
VK_BINDING(0,1) StructuredBuffer<AAPLShaderMaterial> materials REGISTER_SRV(0,1);
VK_BINDING(1,1) SamplerState _LinearRepeatSampler REGISTER_SAMPLER(1,1);
VK_BINDING(2,1) Texture2D<half4> _Textures[] REGISTER_SRV(0,2);  //bindless textures (DX12: own space to avoid overlap)
VK_BINDING(3,1) StructuredBuffer<AAPLMeshChunk> meshChunks REGISTER_SRV(3,1);
VK_BINDING(4,1) StructuredBuffer<uint> chunkIndex REGISTER_SRV(4,1);

// IBL (Vulkan; same resources as the deferred pass, free slots in the appl set)
#ifndef DX12_BACKEND
VK_BINDING(5,1) Texture2D<float2> dfgLutTex;
VK_BINDING(6,1) TextureCube envMap;
VK_BINDING(7,1) SamplerState iblSampler;
VK_BINDING(8,1) cbuffer SHCoefficients { float4 shCoefs[9]; };
VK_BINDING(9,1) cbuffer VlmParamsCB { VlmParams vlmParams; };
VK_BINDING(10,1) StructuredBuffer<float4> vlmShData;
VK_BINDING(11,1) cbuffer VlmSkySH { float4 vlmSkySh[9]; };
#endif

// SVT set2（设计 §8.1；仅 Vulkan 不透明 SVT 变体，DX12 分支不引用新增资源）。
// 布局：0 资产元数据；1 材质 side table；2 页表；3 atlas 数组（每格式池一个）；
//      4 tail 数组；5 atlas sampler（bilinear/clamp，无 mip）；6 tail sampler
//      （bilinear/repeat/trilinear）；7 池元数据；8 SVT frame constants；
//      9 feedback UAV（M3）。
#ifdef SVT_ENABLED
#ifndef DX12_BACKEND
#include "svt_common.hlsl"
VK_BINDING(0,2) StructuredBuffer<SvtAssetGpuDesc> svtAssets REGISTER_SRV(0,2);
VK_BINDING(1,2) StructuredBuffer<SvtMaterialBinding> svtMaterialBindings REGISTER_SRV(1,2);
VK_BINDING(2,2) StructuredBuffer<uint> svtPageTable REGISTER_SRV(2,2);
VK_BINDING(3,2) Texture2D<half4> svtAtlases[SVT_MAX_POOLS] REGISTER_SRV(3,2);
VK_BINDING(4,2) Texture2D<half4> svtTails[SVT_MAX_TAILS] REGISTER_SRV(4,2);
VK_BINDING(5,2) SamplerState svtAtlasSampler REGISTER_SAMPLER(5,2);
VK_BINDING(6,2) SamplerState svtTailSampler REGISTER_SAMPLER(6,2);
VK_BINDING(7,2) StructuredBuffer<SvtPoolGpuDesc> svtPools REGISTER_SRV(7,2);

// SVT frame constants（每帧由 CPU 更新的 UBO）。
struct SvtFrameParams
{
    uint feedbackCapacity;   // uint2 条目容量（不含 header）
    uint frameIndex;         // 采样点轮转
    uint feedbackFactor;     // 屏幕块因子（默认 8）
    uint feedbackEnabled;    // 0/1（ImGui 冻结开关，设计 §11）
    float mipBias;           // M2 固定 0；M4 起自适应（设计 §9）
    uint generationBase;     // assetIndex -> generation 的读取暂用 side table
    uint _pad0;
    uint _pad1;
};
VK_BINDING(8,2) cbuffer SvtFrameParamsCB REGISTER_CBV(8,2) { SvtFrameParams svtFrameParams; };

// 反馈 UAV：RWStructuredBuffer<uint>，[0]=attemptCount，[4 + i*2..] = uint2 条目。
VK_BINDING(9,2) RWStructuredBuffer<uint> svtFeedbackBuffer REGISTER_UAV(9,2);

// 统一材质采样 helper（设计 §11）：SVT 资产走虚拟采样，否则回退旧 bindless 路径。
// slot: 0=albedo 1=roughness 2=normal 3=emissive。
// 导数 du/dv 必须在调用前、任何非一致分支之外取得（设计 §8.2）。
// pixelPos：屏幕像素坐标（反馈采样点选择）。
half4 SampleMaterialTexture(uint materialIndex, uint slot, uint legacyTextureIndex,
                            float2 uv, float2 du, float2 dv, float2 pixelPos)
{
    uint assetIndex = svtMaterialBindings[NonUniformResourceIndex(materialIndex)].assets[slot];
    if (assetIndex != SVT_INVALID_INDEX)
    {
        // 采样点反馈：每 factor x factor 块选 1 像素，m0/m1 两级期望 mip
        // 都反馈（命中也反馈，设计 §8.3）。generation 编码进反馈条目
        //（资产侧 generation 由 CPU 保证 <= 65535；首版资产数少，恒 0）。
        if (svtFrameParams.feedbackEnabled &&
            svtFeedbackSamplePoint(uint2(pixelPos), svtFrameParams.feedbackFactor,
                                   svtFrameParams.frameIndex))
        {
            // 期望 mip 的 LOD 由导数推（与 SvtSampleTexture 内一致）。
            SvtAssetGpuDesc fa = svtAssets[NonUniformResourceIndex(assetIndex)];
            SvtPoolGpuDesc fp = svtPools[NonUniformResourceIndex(fa.poolIndex)];
            float2 duT = du * float2(fa.width, fa.height);
            float2 dvT = dv * float2(fa.width, fa.height);
            float maxLen = max(max(length(duT), length(dvT)), 1e-6);
            float lambda = clamp(log2(maxLen) + svtFrameParams.mipBias,
                                 0.0, float(fa.mipCount - 1));
            uint m0 = (uint)floor(lambda);
            uint m1 = min(m0 + 1, fa.mipCount - 1);
            svtAppendFeedback(svtFeedbackBuffer, svtFrameParams.feedbackCapacity,
                              assetIndex, 0 /*generation16*/, fa, fp, m0, uv);
            if (m1 != m0)
                svtAppendFeedback(svtFeedbackBuffer, svtFrameParams.feedbackCapacity,
                                  assetIndex, 0 /*generation16*/, fa, fp, m1, uv);
        }
        return SvtSampleTexture(uv, du, dv, assetIndex, svtFrameParams.mipBias,
                                svtAssets, svtPools, svtPageTable,
                                svtAtlases, svtTails, svtAtlasSampler, svtTailSampler);
    }
    return _Textures[NonUniformResourceIndex(legacyTextureIndex)].Sample(_LinearRepeatSampler, uv);
}
#endif
#endif



DECLARE_PUSH_CONSTANTS(PushConstants, pushConstants, 1);


struct VSInput
{
    [[vk::location(0)]] float3 position:POSITION;
    [[vk::location(1)]] float3 normal:NORMAL;
    [[vk::location(2)]] float3 tangent:Tangent;
    [[vk::location(3)]] float2 uv:TEXCOORD0;
    //[[vk::location(4)]] uint drawcallid : BLENDINDICES;
	DECLARE_CHUNK_ID_INPUT
};

struct VSOutput
{
    float4 Position   : SV_POSITION; 
    //float4 Diffuse    : COLOR0;
    float2 TextureUV  : TEXCOORD0;
    uint chunkid : TEXCOORD1;

	half3 viewDir : TEXCOORD2;
    half3 normal : TEXCOORD3;
    half3 tangent : TEXCOORD4;
    float4 wsPosition : TEXCOORD5;
};

struct PSOutput
{
	half4 albedo : SV_Target0;
	half4 normals : SV_Target1;
	half4 emissive : SV_Target2;
	half4 F0Roughness : SV_Target3;

};


//vkCmdDrawIndexed indexcount firstindex
VSOutput RenderSceneVS( VSInput input)
{
    VSOutput Output;
    float4x4 finalMatrix = mul(cameraParams.projectionMatrix, cameraParams.viewMatrix);
    Output.Position = mul(finalMatrix ,float4(input.position,1.0));
    //Output.Diffuse = float4(input.uv,0,0);
    Output.TextureUV = input.uv;
    Output.chunkid = GET_CHUNK_ID(input);

    float4x4 invViewMatrix = cameraParams.invViewMatrix;

    Output.viewDir = normalize(float3(invViewMatrix._m03, invViewMatrix._m13, invViewMatrix._m23) - input.position);
    Output.wsPosition = float4(input.position, 1);

	Output.normal = normalize(input.normal);
	Output.tangent = normalize(input.tangent);

    return Output;    
}

VSOutput RenderSceneVSShadow( VSInput input)
{
    VSOutput Output;
    float4x4 finalMatrix = mul(cameraParams.shadowMatrix[pushConstants.shadowIndex].shadowProjectionMatrix, cameraParams.shadowMatrix[pushConstants.shadowIndex].shadowViewMatrix);
    Output.Position = mul(finalMatrix ,float4(input.position,1.0));
    //Output.Diffuse = float4(input.uv,0,0);
    Output.TextureUV = input.uv;
    Output.chunkid = GET_CHUNK_ID(input);

    float4x4 invViewMatrix = cameraParams.invViewMatrix;

    //Output.viewDir = normalize(float3(invViewMatrix._m03, invViewMatrix._m13, invViewMatrix._m23) - input.position);
    Output.wsPosition = float4(input.position, 1);

	//Output.normal = normalize(input.normal);
	Output.tangent = normalize(input.tangent);

    return Output;    
}



#define ALPHA_CUTOUT 0.1

PSOutput RenderSceneBasePass(VSOutput input)
{
	PSOutput output;
    uint chunkindex = input.chunkid;
    uint materialIndex = meshChunks[chunkindex].materialIndex;
    AAPLShaderMaterial material = materials[materialIndex];
    half4 baseColor = _Textures[NonUniformResourceIndex(material.albedo_texture_index)].Sample(_LinearRepeatSampler, input.TextureUV);
	half4 materialData = half4(0,0,0,0);
	half4 emissive = 0;

	if(material.hasMetallicRoughness>0)
        materialData = _Textures[NonUniformResourceIndex(material.roughness_texture_index)].Sample(_LinearRepeatSampler, input.TextureUV);

	if(material.hasEmissive>0)
        emissive = _Textures[NonUniformResourceIndex(material.emissive_texture_index)].Sample(_LinearRepeatSampler, input.TextureUV);

	half3 geonormal = normalize(input.normal);
	half3 geotan = normalize(input.tangent);
	half3 geobinormal = normalize(cross(geotan,geonormal));
    half4 texnormal = _Textures[NonUniformResourceIndex(material.normal_texture_index)].Sample(_LinearRepeatSampler, input.TextureUV);
	texnormal.xy = 2*texnormal.xy-1;
	
	texnormal.z = sqrt(saturate(1.0f - dot(texnormal.xy, texnormal.xy)));

    half3 normal = texnormal.b * geonormal - texnormal.g * geotan + texnormal.r * geobinormal;
	AAPLPixelSurfaceData surfaceData;
	surfaceData.normal = normal;
	surfaceData.albedo = lerp(baseColor.rgb, 0.0f, materialData.b);
	surfaceData.F0=lerp((half)0.04, baseColor.rgb, materialData.b);
	surfaceData.roughness=max((half)0.08, materialData.g);
	surfaceData.alpha=baseColor.a * material.alpha;
	surfaceData.emissive=emissive.xyz;
	output.albedo      = half4(surfaceData.albedo, surfaceData.alpha);
    output.normals     = half4(surfaceData.normal, 0.0f);
    output.emissive    = half4(surfaceData.emissive, 0.0f);
    output.F0Roughness = half4(surfaceData.F0, surfaceData.roughness);
	return output;
}

//deprecated - cpu push constant version
PSOutput RenderSceneBasePS(VSOutput input)
{
    PSOutput output;
    
    uint materialIndex = pushConstants.materialIndex;
    AAPLShaderMaterial material = materials[materialIndex];
    half4 baseColor = _Textures[material.albedo_texture_index].Sample(_LinearRepeatSampler, input.TextureUV);
    half4 materialData = half4(0, 0, 0, 0);
    half4 emissive = 0;

    if (material.hasMetallicRoughness > 0)
        materialData = _Textures[material.roughness_texture_index].Sample(_LinearRepeatSampler, input.TextureUV);

    if (material.hasEmissive > 0)
        emissive = _Textures[material.emissive_texture_index].Sample(_LinearRepeatSampler, input.TextureUV);

    half3 geonormal = normalize(input.normal);
    half3 geotan = normalize(input.tangent);
    half3 geobinormal = normalize(cross(geotan, geonormal));
    half4 texnormal = _Textures[material.normal_texture_index].Sample(_LinearRepeatSampler, input.TextureUV);
    //half4 texnormalmip = _Textures[material.normal_texture_index].SampleLevel(_LinearRepeatSampler,input.TextureUV,3.5);
    //texnormalmip = abs(texnormal-texnormalmip);
    texnormal.xy = 2 * texnormal.xy - 1;
    half dotproduct = dot(texnormal.xy, texnormal.xy);
    half oneminusdotproduct = saturate(1.0f - dotproduct);
    half zzz = sqrt(oneminusdotproduct);

    half3 normal = zzz * geonormal - texnormal.y * geotan + texnormal.x * geobinormal;
    
    AAPLPixelSurfaceData surfaceData;
    surfaceData.normal = normal;
    surfaceData.albedo = lerp(baseColor.rgb, 0.0f, materialData.b);
    surfaceData.F0 = lerp((half) 0.04, baseColor.rgb, materialData.b);
    surfaceData.roughness = max((half) 0.08, materialData.g);
    surfaceData.alpha = baseColor.a * material.alpha;
    surfaceData.emissive = emissive.xyz;

	if(specAlphaMask)
    {
        clip(surfaceData.alpha - ALPHA_CUTOUT);
        surfaceData.alpha = 1;

    }
    output.albedo = half4(surfaceData.albedo, surfaceData.alpha);
    output.normals = half4(surfaceData.normal, 0.0f);
    output.emissive = half4(surfaceData.emissive, 0.0f);
//output.emissive = texnormalmip;
    output.F0Roughness = half4(surfaceData.F0, surfaceData.roughness);
//output.F0Roughness = half4(ddx(input.TextureUV),ddy(input.TextureUV));
    return output;
}


void RenderSceneDepthOnly(VSOutput input)
{
    PSOutput output;
    
    uint materialIndex = pushConstants.materialIndex;
    AAPLShaderMaterial material = materials[materialIndex];
    half4 baseColor = _Textures[material.albedo_texture_index].Sample(_LinearRepeatSampler, input.TextureUV);
   
    if (specAlphaMask)
    {
        clip(baseColor.w - ALPHA_CUTOUT);

    }
    
}

half4 RenderSceneForwardPS(VSOutput input) : SV_Target
{
    uint materialIndex = pushConstants.materialIndex;
    AAPLShaderMaterial material = materials[materialIndex];
    half4 baseColor = _Textures[material.albedo_texture_index].Sample(_LinearRepeatSampler, input.TextureUV);
    half4 materialData = half4(0, 0, 0, 0);
    half4 emissive = 0;

    if (material.hasMetallicRoughness > 0)
        materialData = _Textures[material.roughness_texture_index].Sample(_LinearRepeatSampler, input.TextureUV);

    if (material.hasEmissive > 0)
        emissive = _Textures[material.emissive_texture_index].Sample(_LinearRepeatSampler, input.TextureUV);

    half3 geonormal = normalize(input.normal);
    half3 geotan = normalize(input.tangent);
    half3 geobinormal = normalize(cross(geotan, geonormal));
    half4 texnormal = _Textures[material.normal_texture_index].Sample(_LinearRepeatSampler, input.TextureUV);
    texnormal.xy = 2 * texnormal.xy - 1;
    half dotproduct = dot(texnormal.xy, texnormal.xy);
    half oneminusdotproduct = saturate(1.0f - dotproduct);
    half zzz = sqrt(oneminusdotproduct);

    half3 normal = zzz * geonormal - texnormal.y * geotan + texnormal.x * geobinormal;

    AAPLPixelSurfaceData surfaceData;
    surfaceData.normal = normal;
    surfaceData.albedo = lerp(baseColor.rgb, 0.0f, materialData.b);
    surfaceData.F0 = lerp((half) 0.04, baseColor.rgb, materialData.b);
    surfaceData.roughness = max((half) 0.08, materialData.g);
    surfaceData.alpha = baseColor.a * material.alpha;
    surfaceData.emissive = emissive.xyz;

    half3 res = lightingShader(surfaceData, 0, input.wsPosition, frameConstants, cameraParams);
#ifndef DX12_BACKEND
    if (frameConstants.iblScale > 0.0f || (frameConstants.vlmFlags > 0.5f && frameConstants.vlmScale > 0))
    {
        float3 camPosIBL = float3(cameraParams.invViewMatrix._m03,
                                  cameraParams.invViewMatrix._m13,
                                  cameraParams.invViewMatrix._m23);
        float3 viewDirIBL = normalize(camPosIBL - input.wsPosition.xyz);
        if(frameConstants.vlmFlags>0.5f && frameConstants.vlmScale>0)
            res+=(half3)VlmIndirect(surfaceData,input.wsPosition.xyz,normalize(float3(input.normal)),
                viewDirIBL,vlmParams,vlmShData,vlmSkySh,envMap,dfgLutTex,iblSampler,
                frameConstants.vlmScale,frameConstants.iblScale,frameConstants.iblSpecularScale);
        else
            res+=(half3)IBL(surfaceData,envMap,dfgLutTex,iblSampler,shCoefs,viewDirIBL,
                frameConstants.iblScale,frameConstants.iblSpecularScale);
    }
#endif
    // Emissive is no longer added inside lightingShader (Apple parity: it must
    // stay unshadowed/unoccluded); the forward path applies no shadow/AO, so
    // add it directly.
    res += surfaceData.emissive * frameConstants.emissiveScale;
    return half4(res, surfaceData.alpha);

}

// --- GPU Indirect Base Pass with Alpha Mask (Stage 1) ---
// Reads material from SSBO via chunkIndex, clips alpha
PSOutput RenderSceneBasePassAlphaMask(VSOutput input)
{
    PSOutput output;
    uint chunkindex = input.chunkid;
    uint materialIndex = meshChunks[chunkindex].materialIndex;
    AAPLShaderMaterial material = materials[materialIndex];
    half4 baseColor = _Textures[NonUniformResourceIndex(material.albedo_texture_index)].Sample(_LinearRepeatSampler, input.TextureUV);

    clip(baseColor.w - ALPHA_CUTOUT);

    half4 materialData = half4(0,0,0,0);
    half4 emissive = 0;

    if(material.hasMetallicRoughness>0)
        materialData = _Textures[NonUniformResourceIndex(material.roughness_texture_index)].Sample(_LinearRepeatSampler, input.TextureUV);

    if(material.hasEmissive>0)
        emissive = _Textures[NonUniformResourceIndex(material.emissive_texture_index)].Sample(_LinearRepeatSampler, input.TextureUV);

    half3 geonormal = normalize(input.normal);
    half3 geotan = normalize(input.tangent);
    half3 geobinormal = normalize(cross(geotan,geonormal));
    half4 texnormal = _Textures[NonUniformResourceIndex(material.normal_texture_index)].Sample(_LinearRepeatSampler, input.TextureUV);
    texnormal.xy = 2*texnormal.xy-1;
    texnormal.z = sqrt(saturate(1.0f - dot(texnormal.xy, texnormal.xy)));

    half3 normal = texnormal.b * geonormal - texnormal.g * geotan + texnormal.r * geobinormal;
    AAPLPixelSurfaceData surfaceData;
    surfaceData.normal = normal;
    surfaceData.albedo = lerp(baseColor.rgb, 0.0f, materialData.b);
    surfaceData.F0=lerp((half)0.04, baseColor.rgb, materialData.b);
    surfaceData.roughness=max((half)0.08, materialData.g);
    surfaceData.alpha=baseColor.a * material.alpha;
    surfaceData.emissive=emissive.xyz;
    output.albedo      = half4(surfaceData.albedo, surfaceData.alpha);
    output.normals     = half4(surfaceData.normal, 0.0f);
    output.emissive    = half4(surfaceData.emissive, 0.0f);
    output.F0Roughness = half4(surfaceData.F0, surfaceData.roughness);
    return output;
}

// --- GPU Indirect Forward Pass (Stage 4) ---
// Reads material from SSBO via chunkIndex, no push constants
half4 RenderSceneForwardPSIndirect(VSOutput input) : SV_Target
{
    uint chunkindex = input.chunkid;
    uint materialIndex = meshChunks[chunkindex].materialIndex;
    AAPLShaderMaterial material = materials[materialIndex];
    half4 baseColor = _Textures[NonUniformResourceIndex(material.albedo_texture_index)].Sample(_LinearRepeatSampler, input.TextureUV);
    half4 materialData = half4(0, 0, 0, 0);
    half4 emissive = 0;

    if (material.hasMetallicRoughness > 0)
        materialData = _Textures[NonUniformResourceIndex(material.roughness_texture_index)].Sample(_LinearRepeatSampler, input.TextureUV);

    if (material.hasEmissive > 0)
        emissive = _Textures[NonUniformResourceIndex(material.emissive_texture_index)].Sample(_LinearRepeatSampler, input.TextureUV);

    half3 geonormal = normalize(input.normal);
    half3 geotan = normalize(input.tangent);
    half3 geobinormal = normalize(cross(geotan, geonormal));
    half4 texnormal = _Textures[NonUniformResourceIndex(material.normal_texture_index)].Sample(_LinearRepeatSampler, input.TextureUV);
    texnormal.xy = 2 * texnormal.xy - 1;
    half dotproduct = dot(texnormal.xy, texnormal.xy);
    half oneminusdotproduct = saturate(1.0f - dotproduct);
    half zzz = sqrt(oneminusdotproduct);

    half3 normal = zzz * geonormal - texnormal.y * geotan + texnormal.x * geobinormal;

    AAPLPixelSurfaceData surfaceData;
    surfaceData.normal = normal;
    surfaceData.albedo = lerp(baseColor.rgb, 0.0f, materialData.b);
    surfaceData.F0 = lerp((half) 0.04, baseColor.rgb, materialData.b);
    surfaceData.roughness = max((half) 0.08, materialData.g);
    surfaceData.alpha = baseColor.a * material.alpha;
    surfaceData.emissive = emissive.xyz;

    half3 res = lightingShader(surfaceData, 0, input.wsPosition, frameConstants, cameraParams);
#ifndef DX12_BACKEND
    if (frameConstants.iblScale > 0.0f || (frameConstants.vlmFlags > 0.5f && frameConstants.vlmScale > 0))
    {
        float3 camPosIBL = float3(cameraParams.invViewMatrix._m03,
                                  cameraParams.invViewMatrix._m13,
                                  cameraParams.invViewMatrix._m23);
        float3 viewDirIBL = normalize(camPosIBL - input.wsPosition.xyz);
        if(frameConstants.vlmFlags>0.5f && frameConstants.vlmScale>0)
            res+=(half3)VlmIndirect(surfaceData,input.wsPosition.xyz,normalize(float3(input.normal)),
                viewDirIBL,vlmParams,vlmShData,vlmSkySh,envMap,dfgLutTex,iblSampler,
                frameConstants.vlmScale,frameConstants.iblScale,frameConstants.iblSpecularScale);
        else
            res+=(half3)IBL(surfaceData,envMap,dfgLutTex,iblSampler,shCoefs,viewDirIBL,
                frameConstants.iblScale,frameConstants.iblSpecularScale);
    }
#endif
    // Emissive is no longer added inside lightingShader (Apple parity: it must
    // stay unshadowed/unoccluded); the forward path applies no shadow/AO, so
    // add it directly.
    res += surfaceData.emissive * frameConstants.emissiveScale;
    return half4(res, surfaceData.alpha);
}

// --- GPU Indirect Shadow Depth Only (Stage 2) ---
// Reads material from SSBO for alpha test, no push constants
void RenderSceneShadowDepthIndirect(VSOutput input)
{
    if (specAlphaMask)
    {
        uint chunkindex = input.chunkid;
        uint materialIndex = meshChunks[chunkindex].materialIndex;
        AAPLShaderMaterial material = materials[materialIndex];
        half4 baseColor = _Textures[NonUniformResourceIndex(material.albedo_texture_index)].Sample(_LinearRepeatSampler, input.TextureUV);
        clip(baseColor.w - ALPHA_CUTOUT);
    }
}

// ===========================================================================
// SVT opaque variants (design §8.1: 仅不透明 base/forward 使用 SVT 采样；
// alpha-mask/透明/阴影维持上方旧入口)。
// ===========================================================================
#if defined(SVT_ENABLED) && !defined(DX12_BACKEND)

// --- SVT Base Pass (opaque, GPU indirect) ---
[earlydepthstencil]
PSOutput RenderSceneBasePassSvt(VSOutput input)
{
    PSOutput output;
    uint chunkindex = input.chunkid;
    uint materialIndex = meshChunks[chunkindex].materialIndex;
    AAPLShaderMaterial material = materials[materialIndex];

    // 导数在所有非一致分支之前取得（设计 §8.2）。
    float2 du = ddx(input.TextureUV);
    float2 dv = ddy(input.TextureUV);

    half4 baseColor = SampleMaterialTexture(materialIndex, 0, material.albedo_texture_index,
                                            input.TextureUV, du, dv, input.Position.xy);
    half4 materialData = half4(0,0,0,0);
    half4 emissive = 0;

    if(material.hasMetallicRoughness>0)
        materialData = SampleMaterialTexture(materialIndex, 1, material.roughness_texture_index,
                                            input.TextureUV, du, dv, input.Position.xy);

    if(material.hasEmissive>0)
        emissive = SampleMaterialTexture(materialIndex, 3, material.emissive_texture_index,
                                            input.TextureUV, du, dv, input.Position.xy);

    half3 geonormal = normalize(input.normal);
    half3 geotan = normalize(input.tangent);
    half3 geobinormal = normalize(cross(geotan,geonormal));
    half4 texnormal = SampleMaterialTexture(materialIndex, 2, material.normal_texture_index,
                                            input.TextureUV, du, dv, input.Position.xy);
    texnormal.xy = 2*texnormal.xy-1;

    texnormal.z = sqrt(saturate(1.0f - dot(texnormal.xy, texnormal.xy)));

    half3 normal = texnormal.b * geonormal - texnormal.g * geotan + texnormal.r * geobinormal;
    AAPLPixelSurfaceData surfaceData;
    surfaceData.normal = normal;
    surfaceData.albedo = lerp(baseColor.rgb, 0.0f, materialData.b);
    surfaceData.F0=lerp((half)0.04, baseColor.rgb, materialData.b);
    surfaceData.roughness=max((half)0.08, materialData.g);
    surfaceData.alpha=baseColor.a * material.alpha;
    surfaceData.emissive=emissive.xyz;
    output.albedo      = half4(surfaceData.albedo, surfaceData.alpha);
    output.normals     = half4(surfaceData.normal, 0.0f);
    output.emissive    = half4(surfaceData.emissive, 0.0f);
    output.F0Roughness = half4(surfaceData.F0, surfaceData.roughness);
    if (svtFrameParams._pad0 != 0) {
        uint asset = svtMaterialBindings[materialIndex].assets.x;
        float3 color = asset == SVT_INVALID_INDEX ? float3(0.1,0.1,0.1) :
            SvtDebugColor(input.TextureUV,du,dv,asset,svtFrameParams.mipBias,
                          svtFrameParams._pad0,svtAssets,svtPools,svtPageTable);
        output.albedo = half4(0,0,0,1);
        output.F0Roughness = half4(0,0,0,1);
        output.emissive = half4(color,0);
    }
    return output;
}

// --- SVT Forward Pass (opaque, GPU indirect, no push constants) ---
half4 RenderSceneForwardPSIndirectSvt(VSOutput input) : SV_Target
{
    uint chunkindex = input.chunkid;
    uint materialIndex = meshChunks[chunkindex].materialIndex;
    AAPLShaderMaterial material = materials[materialIndex];

    float2 du = ddx(input.TextureUV);
    float2 dv = ddy(input.TextureUV);

    half4 baseColor = SampleMaterialTexture(materialIndex, 0, material.albedo_texture_index,
                                            input.TextureUV, du, dv, input.Position.xy);
    half4 materialData = half4(0, 0, 0, 0);
    half4 emissive = 0;

    if (material.hasMetallicRoughness > 0)
        materialData = SampleMaterialTexture(materialIndex, 1, material.roughness_texture_index,
                                            input.TextureUV, du, dv, input.Position.xy);

    if (material.hasEmissive > 0)
        emissive = SampleMaterialTexture(materialIndex, 3, material.emissive_texture_index,
                                            input.TextureUV, du, dv, input.Position.xy);

    half3 geonormal = normalize(input.normal);
    half3 geotan = normalize(input.tangent);
    half3 geobinormal = normalize(cross(geotan, geonormal));
    half4 texnormal = SampleMaterialTexture(materialIndex, 2, material.normal_texture_index,
                                            input.TextureUV, du, dv, input.Position.xy);
    texnormal.xy = 2 * texnormal.xy - 1;
    half dotproduct = dot(texnormal.xy, texnormal.xy);
    half oneminusdotproduct = saturate(1.0f - dotproduct);
    half zzz = sqrt(oneminusdotproduct);

    half3 normal = zzz * geonormal - texnormal.y * geotan + texnormal.x * geobinormal;

    AAPLPixelSurfaceData surfaceData;
    surfaceData.normal = normal;
    surfaceData.albedo = lerp(baseColor.rgb, 0.0f, materialData.b);
    surfaceData.F0 = lerp((half) 0.04, baseColor.rgb, materialData.b);
    surfaceData.roughness = max((half) 0.08, materialData.g);
    surfaceData.alpha = baseColor.a * material.alpha;
    surfaceData.emissive = emissive.xyz;

    half3 res = lightingShader(surfaceData, 0, input.wsPosition, frameConstants, cameraParams);
    if (frameConstants.iblScale > 0.0f || (frameConstants.vlmFlags > 0.5f && frameConstants.vlmScale > 0))
    {
        float3 camPosIBL = float3(cameraParams.invViewMatrix._m03,
                                  cameraParams.invViewMatrix._m13,
                                  cameraParams.invViewMatrix._m23);
        float3 viewDirIBL = normalize(camPosIBL - input.wsPosition.xyz);
        if(frameConstants.vlmFlags>0.5f && frameConstants.vlmScale>0)
            res+=(half3)VlmIndirect(surfaceData,input.wsPosition.xyz,normalize(float3(input.normal)),
                viewDirIBL,vlmParams,vlmShData,vlmSkySh,envMap,dfgLutTex,iblSampler,
                frameConstants.vlmScale,frameConstants.iblScale,frameConstants.iblSpecularScale);
        else
            res+=(half3)IBL(surfaceData,envMap,dfgLutTex,iblSampler,shCoefs,viewDirIBL,
                frameConstants.iblScale,frameConstants.iblSpecularScale);
    }
    // Emissive is no longer added inside lightingShader (Apple parity: it must
    // stay unshadowed/unoccluded); the forward path applies no shadow/AO, so
    // add it directly.
    res += surfaceData.emissive * frameConstants.emissiveScale;
    return half4(res, surfaceData.alpha);
}

#endif // SVT_ENABLED && !DX12_BACKEND
