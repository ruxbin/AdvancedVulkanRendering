// Reuse the exact vertex transform, indirect-ID bindings and material sampler.
// Unused lighting resources are eliminated by DXC from these minimal PS entries.
#include "drawcluster.hlsl"

VSOutput RenderScenePickingVS(VSInput input)
{
    VSOutput output = (VSOutput)0;
    float4x4 finalMatrix = mul(cameraParams.projectionMatrix, cameraParams.viewMatrix);
    output.Position = mul(finalMatrix, float4(input.position, 1.0));
    output.TextureUV = input.uv;
    output.chunkid = GET_CHUNK_ID(input);
    return output;
}

uint PickOpaque(VSOutput input) : SV_Target0 {
    return input.chunkid;
}
uint PickAlphaMask(VSOutput input) : SV_Target0 {
    uint materialId = meshChunks[input.chunkid].materialIndex;
    AAPLShaderMaterial material = materials[materialId];
    float alpha = _Textures[NonUniformResourceIndex(material.albedo_texture_index)]
        .Sample(_LinearRepeatSampler, input.TextureUV).a;
    clip(alpha - ALPHA_CUTOUT);
    return input.chunkid;
}
uint PickTransparent(VSOutput input) : SV_Target0 {
    uint materialId = meshChunks[input.chunkid].materialIndex;
    AAPLShaderMaterial material = materials[materialId];
    float alpha = _Textures[NonUniformResourceIndex(material.albedo_texture_index)]
        .Sample(_LinearRepeatSampler, input.TextureUV).a * material.alpha;
    clip(alpha - 0.1);
    return input.chunkid;
}
