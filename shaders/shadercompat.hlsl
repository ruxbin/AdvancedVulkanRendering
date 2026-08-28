// shadercompat.hlsl - Dual backend binding macros for Vulkan (SPIR-V) and DX12 (DXIL)
//
// Vulkan path: dxc -spirv ...            (uses [[vk::binding(n,s)]])
// DX12 path:   dxc -D DX12_BACKEND ...   (uses register(xN, spaceS))

#ifdef DX12_BACKEND
  #define VK_BINDING(binding_idx, set_idx)
  #define REGISTER_CBV(binding_idx, set_idx)     : register(b##binding_idx, space##set_idx)
  #define REGISTER_SRV(binding_idx, set_idx)     : register(t##binding_idx, space##set_idx)
  #define REGISTER_UAV(binding_idx, set_idx)     : register(u##binding_idx, space##set_idx)
  #define REGISTER_SAMPLER(binding_idx, set_idx) : register(s##binding_idx, space##set_idx)
  #define REGISTER_SAMPLER_CMP(binding_idx, set_idx) : register(s##binding_idx, space##set_idx)
  // DX12: push constants become a cbuffer with root constant binding
  #define DECLARE_PUSH_CONSTANTS(type_name, var_name, reg_idx) \
    cbuffer var_name##_cb : register(b##reg_idx) { type_name var_name; }
  #define NDC_Y_FLIP 1.0
  // Chunk id delivery for GPU-culled indirect draws.
  //
  // D3D12's SV_InstanceID is 0-based *per draw* and does NOT include
  // StartInstanceLocation, so Vulkan's trick of smuggling the draw slot through
  // firstInstance and looking it up as chunkIndex[gl_InstanceIndex] is
  // unavailable here — SV_InstanceID would be 0 for every indirect draw
  // (InstanceCount is always 1), collapsing the whole scene onto one material.
  //
  // Instead the command signature is [CONSTANT(b1), DRAW_INDEXED]: the cull
  // shader writes the draw slot as the first uint of each argument record and
  // ExecuteIndirect patches it into pushConstants.drawSlot before the draw.
  #define DECLARE_CHUNK_ID_INPUT
  #define GET_CHUNK_ID(input)     (chunkIndex[pushConstants.drawSlot])
#else
  #define VK_BINDING(binding_idx, set_idx)       [[vk::binding(binding_idx,set_idx)]]
  #define REGISTER_CBV(binding_idx, set_idx)
  #define REGISTER_SRV(binding_idx, set_idx)
  #define REGISTER_UAV(binding_idx, set_idx)
  #define REGISTER_SAMPLER(binding_idx, set_idx)
  #define REGISTER_SAMPLER_CMP(binding_idx, set_idx)
  // Vulkan: push constants use [[vk::push_constant]]
  #define DECLARE_PUSH_CONSTANTS(type_name, var_name, reg_idx) \
    [[vk::push_constant]] type_name var_name
  #define NDC_Y_FLIP -1.0
  // Vulkan's gl_InstanceIndex includes firstInstance, which the cull shader
  // sets to the draw slot — so the chunk id comes from chunkIndex[slot].
  #define DECLARE_CHUNK_ID_INPUT  uint instancid : SV_InstanceID;
  #define GET_CHUNK_ID(input)     (chunkIndex[input.instancid])
#endif
