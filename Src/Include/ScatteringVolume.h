#pragma once
#include "vulkan/vulkan.h"
#include "VulkanSetup.h"
#include <filesystem>
#include <vector>

// ScatteringVolume
// Froxel-based volumetric scattering, ported from ModernRenderingWithMetal.
//
// Two compute passes per frame:
//   1. ScatterVolume       – fills each froxel (8×8 pixels × 1 depth slice)
//                            with inscattered radiance and extinction coefficient.
//   2. AccumulateScattering – front-to-back Beer–Lambert integration through
//                            the volume; outputs a 3D texture ready for the
//                            deferred lighting pass.
//
// The accumulated 3D texture (accumVolumView) is bound at binding 11 in the
// deferred lighting descriptor set.

#define SCATTER_TILE_SIZE    8     // froxel covers 8×8 screen pixels
#define SCATTER_VOLUME_DEPTH 64    // depth slices along the view axis

class ScatteringVolume {
public:
    ScatteringVolume() = default;

    // Call once after shadow maps and uniform buffers are ready.
    // rootPath            – project root to locate compiled SPIR-V
    // framesInFlight      – number of simultaneous frames (for per-frame UBO sets)
    // uniformBuffers      – per-frame UBO containing CameraParamsBufferFull+FrameConstants
    // shadowMapView       – VkImageView of the full cascade shadow map array
    // shadowSampler       – VkSampler with comparison (VK_COMPARE_OP_LESS)
    void create(const VulkanDevice&              device,
                const std::filesystem::path&    rootPath,
                uint32_t                        screenW,
                uint32_t                        screenH,
                uint32_t                        framesInFlight,
                const std::vector<VkBuffer>&    uniformBuffers,
                VkImageView                     shadowMapView,
                VkSampler                       shadowSampler);

    // Dispatch both scatter and accumulate compute passes.
    void dispatch(VkCommandBuffer cmd, uint32_t frameIndex);

    // View of the accumulated 3D RGBA16F texture.
    // Bind at deferred-lighting binding 11 as SHADER_READ_ONLY_OPTIMAL.
    VkImageView accumVolumeView() const { return _accumView; }

    void destroy(const VulkanDevice& device);

private:
    uint32_t _volumeW = 0, _volumeH = 0;

    // ---- froxel scatter texture (per-froxel: rgb=light, a=extinction) ------
    VkImage        _scatterTex    = VK_NULL_HANDLE;
    VkDeviceMemory _scatterMem    = VK_NULL_HANDLE;
    VkImageView    _scatterView   = VK_NULL_HANDLE;  // GENERAL (write)

    // ---- accumulated 3D result texture (rgb=gathered light, a=transmittance) --
    VkImage        _accumTex      = VK_NULL_HANDLE;
    VkDeviceMemory _accumMem      = VK_NULL_HANDLE;
    VkImageView    _accumView     = VK_NULL_HANDLE;  // SHADER_READ_ONLY_OPTIMAL (final)

    // ---- pipelines ---------------------------------------------------------
    VkPipelineLayout _scatterLayout = VK_NULL_HANDLE;
    VkPipeline       _scatterPipe   = VK_NULL_HANDLE;

    VkPipelineLayout _accumLayout   = VK_NULL_HANDLE;
    VkPipeline       _accumPipe     = VK_NULL_HANDLE;

    // ---- descriptor sets ---------------------------------------------------
    // scatterSets[i]  – one per frame (needs the per-frame UBO)
    //   binding 0: RWTexture3D scatterOut (STORAGE_IMAGE)
    //   binding 1: UBO (camera + frame constants)
    //   binding 2: shadowMaps (SAMPLED_IMAGE array)
    //   binding 3: shadowSampler (SAMPLER)
    VkDescriptorPool      _scatterPool   = VK_NULL_HANDLE;
    VkDescriptorSetLayout _scatterSetLayout = VK_NULL_HANDLE;
    std::vector<VkDescriptorSet> _scatterSets; // per frame

    // accumSet  – one set shared across frames
    //   binding 0: Texture3D scatterIn (SAMPLED_IMAGE)
    //   binding 1: RWTexture3D accumOut (STORAGE_IMAGE)
    VkDescriptorPool      _accumPool     = VK_NULL_HANDLE;
    VkDescriptorSetLayout _accumSetLayout  = VK_NULL_HANDLE;
    VkDescriptorSet       _accumSet      = VK_NULL_HANDLE;

    // ---- helpers -----------------------------------------------------------
    void createTextures(const VulkanDevice& device);
    void createScatterDescriptors(const VulkanDevice& device,
                                  const std::vector<VkBuffer>& uniformBuffers,
                                  VkImageView shadowMapView,
                                  VkSampler   shadowSampler,
                                  uint32_t    framesInFlight);
    void createAccumDescriptors(const VulkanDevice& device);
    void createPipelines(const VulkanDevice& device, const std::filesystem::path& rootPath);

    static VkShaderModule loadSpirV(const VulkanDevice& device, const std::string& path);
};
