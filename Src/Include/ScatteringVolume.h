#pragma once
#pragma execution_character_set("utf-8")
#include "VulkanSetup.h"
#include <filesystem>
#include <vector>

// ScatteringVolume – froxel-based volumetric scattering ported from ModernRenderingWithMetal.
//
// Two compute passes per frame:
//   1. ScatterVolume       – fills each froxel with in-scattered radiance + extinction.
//                            Includes temporal reprojection (85 % history blend),
//                            blue-noise depth jitter, local point/spot light contributions,
//                            3D Perlin noise density variation.
//   2. AccumulateScattering – Beer–Lambert front-to-back integration.

#define SCATTER_TILE_SIZE    8   // froxel covers 8×8 screen pixels
#define SCATTER_VOLUME_DEPTH 64  // depth slices

// Light resources passed to create() so the scatter shader can evaluate local lights.
struct ScatterLightResources {
    VkBuffer              pointLightDataBuffer   = VK_NULL_HANDLE;
    std::vector<VkBuffer> pointLightIndexBuffers; // per frame-in-flight
    VkBuffer              spotLightDataBuffer    = VK_NULL_HANDLE;
    std::vector<VkBuffer> spotLightIndexBuffers;  // per frame-in-flight
    VkImageView           spotShadowMapView      = VK_NULL_HANDLE;
    VkSampler             spotShadowSampler      = VK_NULL_HANDLE;
    VkBuffer              spotViewProjBuffer     = VK_NULL_HANDLE;
};

class ScatteringVolume {
public:
    ScatteringVolume() = default;

    // Call once after Shadow and LightCuller are ready.
    void create(const VulkanDevice&           device,
                const std::filesystem::path&  rootPath,
                uint32_t                      screenW,
                uint32_t                      screenH,
                uint32_t                      framesInFlight,
                const std::vector<VkBuffer>&  uniformBuffers,
                VkImageView                   shadowMapView,
                VkSampler                     shadowSampler,
                const ScatterLightResources&  lightRes);

    // Dispatch both compute passes.  Call once per frame before the G-buffer.
    void dispatch(VkCommandBuffer cmd, uint32_t frameIndex);

    // Accumulated 3D RGBA16F result — bind at deferred-lighting binding 16.
    VkImageView accumVolumeView() const { return _accumView; }

    void destroy(const VulkanDevice& device);

private:
    uint32_t _volumeW = 0, _volumeH = 0;
    uint32_t _framesInFlight = 0;
    bool     _firstDispatch = true;

    // Current-frame scatter output (written by ScatterVolume, read by Accumulate).
    VkImage        _scatterTex    = VK_NULL_HANDLE;
    VkDeviceMemory _scatterMem    = VK_NULL_HANDLE;
    VkImageView    _scatterView   = VK_NULL_HANDLE;

    // Previous-frame history (read by ScatterVolume for temporal reprojection).
    VkImage        _scatterHistoryTex = VK_NULL_HANDLE;
    VkDeviceMemory _scatterHistoryMem = VK_NULL_HANDLE;
    VkImageView    _scatterHistoryView = VK_NULL_HANDLE;

    // Accumulated result (rgb=gathered light, a=transmittance).
    VkImage        _accumTex  = VK_NULL_HANDLE;
    VkDeviceMemory _accumMem  = VK_NULL_HANDLE;
    VkImageView    _accumView = VK_NULL_HANDLE;

    // 64×64 blue noise (white noise used as cheaper substitute) for jitter.
    VkImage        _blueNoiseTex  = VK_NULL_HANDLE;
    VkDeviceMemory _blueNoiseMem  = VK_NULL_HANDLE;
    VkImageView    _blueNoiseView = VK_NULL_HANDLE;

    // 32×32×32 Perlin noise for fog density variation.
    VkImage        _perlinTex  = VK_NULL_HANDLE;
    VkDeviceMemory _perlinMem  = VK_NULL_HANDLE;
    VkImageView    _perlinView = VK_NULL_HANDLE;

    // Linear clamp sampler shared by history + Perlin.
    VkSampler _linearSampler = VK_NULL_HANDLE;

    // Scatter pipeline (bindings 0-14, per-frame descriptor sets).
    VkDescriptorPool      _scatterPool      = VK_NULL_HANDLE;
    VkDescriptorSetLayout _scatterSetLayout = VK_NULL_HANDLE;
    std::vector<VkDescriptorSet> _scatterSets; // per frame-in-flight

    VkPipelineLayout _scatterLayout = VK_NULL_HANDLE;
    VkPipeline       _scatterPipe   = VK_NULL_HANDLE;

    // Accumulate pipeline (2 bindings, single descriptor set).
    VkDescriptorPool      _accumPool      = VK_NULL_HANDLE;
    VkDescriptorSetLayout _accumSetLayout = VK_NULL_HANDLE;
    VkDescriptorSet       _accumSet       = VK_NULL_HANDLE;

    VkPipelineLayout _accumLayout = VK_NULL_HANDLE;
    VkPipeline       _accumPipe   = VK_NULL_HANDLE;

    void createTextures(const VulkanDevice& device);
    void uploadBlueNoise(const VulkanDevice& device);
    void uploadPerlinNoise(const VulkanDevice& device);
    void createSampler(const VulkanDevice& device);
    void createScatterDescriptors(const VulkanDevice& device,
                                  const std::vector<VkBuffer>& uniformBuffers,
                                  VkImageView shadowMapView, VkSampler shadowSampler,
                                  const ScatterLightResources& lightRes,
                                  uint32_t framesInFlight);
    void createAccumDescriptors(const VulkanDevice& device);
    void createPipelines(const VulkanDevice& device, const std::filesystem::path& rootPath);

    static VkShaderModule loadSpirV(const VulkanDevice& device, const std::string& path);
};
