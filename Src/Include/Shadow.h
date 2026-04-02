#pragma once
#include "GpuScene.h"
#include "Matrix.h"
#include "VulkanSetup.h"
#include <array>
#include <vector>


#define SHADOW_CASCADE_COUNT 3
// VK_FORMAT_R32_SFLOAT won't work
#define SHADOW_FORMAT VK_FORMAT_D32_SFLOAT_S8_UINT

class GpuScene;

class Shadow {
private:
  VkImage _shadowSlices;
  std::array<mat4, SHADOW_CASCADE_COUNT> _shadowViewMatrices;
  std::array<mat4, SHADOW_CASCADE_COUNT> _shadowProjectionMatrices;
  uint32_t _shadowResolution;
  VkImage _shadowMaps;
  std::array<VkImageView, SHADOW_CASCADE_COUNT> _shadowSliceViews;
  std::array<VkFramebuffer, SHADOW_CASCADE_COUNT> _shadowFrameBuffers;
  VkImageView _shadowSliceViewFull;
  VkRenderPass _shadowPass;
  VkPipelineLayout _shadowPassPipelineLayout;
  VkPipeline _shadowPassPipeline;
  VkPipeline _shadowPassPipelineAlphaMask;
  VkSampler _shadowMapSampler;
  void InitRHI(const VulkanDevice &, const GpuScene &);

  // GPU-Driven Shadow (Stage 2)
  VkBuffer _shadowDrawParamsBuffer = VK_NULL_HANDLE;
  VkDeviceMemory _shadowDrawParamsMemory = VK_NULL_HANDLE;
  VkBuffer _shadowWriteIndexBuffer = VK_NULL_HANDLE;
  VkDeviceMemory _shadowWriteIndexMemory = VK_NULL_HANDLE;
  VkBuffer _shadowChunkIndicesBuffer = VK_NULL_HANDLE;
  VkDeviceMemory _shadowChunkIndicesMemory = VK_NULL_HANDLE;
  VkBuffer _shadowInstanceBuffer = VK_NULL_HANDLE;
  VkDeviceMemory _shadowInstanceMemory = VK_NULL_HANDLE;
  VkBuffer _shadowCullParamsBuffer = VK_NULL_HANDLE;
  VkDeviceMemory _shadowCullParamsMemory = VK_NULL_HANDLE;

  VkDescriptorSetLayout _shadowCullSetLayout = VK_NULL_HANDLE;
  VkDescriptorPool _shadowCullDescriptorPool = VK_NULL_HANDLE;
  VkDescriptorSet _shadowCullDescriptorSet = VK_NULL_HANDLE;

  VkPipelineLayout _shadowCullPipelineLayout = VK_NULL_HANDLE;
  VkPipeline _shadowCullPipeline = VK_NULL_HANDLE;

  // Indirect shadow rendering pipelines (reads material from SSBO, no push constants)
  VkPipeline _shadowPassPipelineIndirect = VK_NULL_HANDLE;
  VkPipeline _shadowPassPipelineAlphaMaskIndirect = VK_NULL_HANDLE;

  bool _gpuShadowInitialized = false;
  void InitGPUShadowResources(const VulkanDevice &, const GpuScene &);

  struct ShadowCullParams {
    uint32_t opaqueChunkCount;
    uint32_t alphaMaskedChunkCount;
    uint32_t cascadeMaxChunks;
    uint32_t cascadeIndex;
    Frustum cascadeFrustum;
  };

public:
  Shadow(const VulkanDevice& device, const GpuScene& gpuscene, uint32_t shadowResolution) : _shadowResolution(shadowResolution) 
  {
    InitRHI(device, gpuscene);
  }

  void CreateShadowSlices(const VulkanDevice &);
  void RenderShadowMap(VkCommandBuffer &, const GpuScene &,
                       const VulkanDevice &);
  void UpdateShadowMatrices(const GpuScene &);
  friend class GpuScene;
};
