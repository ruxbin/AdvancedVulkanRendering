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
#endif
