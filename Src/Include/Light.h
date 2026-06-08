#pragma once

#include "Common.h"
#include "Matrix.h"
#include "VulkanSetup.h"
#include <vector>


class GpuScene;
class LightCuller;

class Light {
public:
  virtual void Draw(VkCommandBuffer &, const GpuScene &) = 0;
  // virtual void InitRHI(const VulkanDevice&, const GpuScene&)=0;
};

class PointLight : public Light {
private:
  uint32_t _dynamicOffset = 0;
  const PointLightData *_pointLightData = nullptr;

public:
  virtual void Draw(VkCommandBuffer &, const GpuScene &);
  static void InitRHI(const VulkanDevice &, const GpuScene &);
  PointLight(uint32_t dynamic_offset, const PointLightData *pld)
      : _dynamicOffset(dynamic_offset), _pointLightData(pld) {}
  const PointLightData *getPointLightData() const { return _pointLightData; }

  static VkBuffer pointLightDynamicUniformBuffer;

  static VkPipelineLayout drawPointLightPipelineLayout;
  static VkPipeline drawPointLightPipeline;
  static VkPipeline drawPointLightPipelineStencil;

  static VkDescriptorPool pointLightingDescriptorPool;
  static VkDescriptorSetLayout drawPointLightDescriptorSetLayout;
  static std::vector<VkDescriptorSet> drawPointLightDescriptorSets; // per-frame
  static std::vector<PointLightData> pointLightData;

  static void CommonDrawSetup(VkCommandBuffer &);

  friend class LightCuller;
};

class SpotLight : public Light {
private:
  // VkBuffer coneBuffer;
  // VkBuffer coneIndexBuffer;
  const SpotLightData *_spotLightData = nullptr;
  uint32_t _dynamicOffset = 0;

public:
  virtual void Draw(VkCommandBuffer &, const GpuScene &) {}
  virtual void InitRHI(const VulkanDevice &, const GpuScene &) {}

  SpotLight(uint32_t dynamic_offset, const SpotLightData *sld)
      : _dynamicOffset(dynamic_offset), _spotLightData(sld) {}

  static std::vector<SpotLightData> spotLightData;

  friend class LightCuller;
  friend class PbrtExporter;
};

#define DEFAULT_LIGHT_CULLING_TILE_SIZE (32)
#define LIGHT_CLUSTER_DEPTH (64)

#define MAX_LIGHTS_PER_TILE (64)

#define MAX_LIGHTS_PER_CLUSTER (16)

// Apple parity (AAPLConfig.h:109): max number of spot lights that get an
// individual depth-array slice. Spots beyond this index are lit without a
// shadow term.
#define SPOT_SHADOW_MAX_COUNT (32)
#define SPOT_SHADOW_RESOLUTION (256)

class LightCuller {
public:
  void ClusterLightForScreen(VkCommandBuffer &, const VulkanDevice &device,
                             const GpuScene &gpuScene, uint32_t screen_width,
                             uint32_t screen_heigt);
  void InitRHI(const VulkanDevice &, const GpuScene &gpuScene,
               uint32_t screen_width, uint32_t screen_heigt);
  LightCuller();
  VkImage GetXZDebugImage() { return _xzDebugImage; }
  VkImage GetTraditionalDebugImage() { return _traditionalCullDebugImage; }
  VkBuffer GetPointLightCullingDataBuffer() const {
    return _pointLightCullingDataBuffer;
  }
  VkBuffer GetPointLightIndicesBuffer(uint32_t frame) const {
    return _lightIndicesBuffer[frame];
  }
  VkBuffer GetSpotLightCullingDataBuffer() const {
    return _spotLightCullingDataBuffer;
  }
  VkBuffer GetSpotLightIndicesBuffer(uint32_t frame) const {
    return _spotLightIndicesBuffer[frame];
  }
  VkBuffer GetSpotViewProjBuffer() const { return _spotViewProjBuffer; }

private:
  VkBuffer _pointLightCullingDataBuffer;

  std::vector<VkBuffer> _xzRangeBuffer;
  std::vector<VkBuffer> _lightIndicesBuffer;
  std::vector<VkBuffer> _lightIndicesTransparentBuffer;
  VkImage _xzDebugImage;
  VkImageView _xzDebugImageView;

  VkImage _traditionalCullDebugImage;
  VkImageView _traditionalCullDebugImageView;

  // Spot light culling buffers, parallel to the point light set above.
  VkBuffer _spotLightCullingDataBuffer = VK_NULL_HANDLE;
  std::vector<VkBuffer> _spotXZRangeBuffer;
  std::vector<VkBuffer> _spotLightIndicesBuffer;
  std::vector<VkBuffer> _spotLightIndicesTransparentBuffer;

  // One mat4 per spot light — view-proj matrices for shadow sampling in the
  // deferred lighting shader (binding 15).
  VkBuffer _spotViewProjBuffer = VK_NULL_HANDLE;
  VkDeviceMemory _spotViewProjMemory = VK_NULL_HANDLE;

  VkDescriptorSetLayout coarseCullSetLayout;
  VkDescriptorPool coarseCullDescriptorPool;
  std::vector<VkDescriptorSet> coarseCullDescriptorSet; // per-frame

  VkPipelineLayout coarseCullPipelineLayout;
  VkPipeline coarseCullPipeline;
  VkPipeline clearDebugViewPipeline;
  VkPipeline traditionalCullPipeline;
  VkPipeline clearIndicesPipeline;

  // Spot light cull pipelines (share coarseCullPipelineLayout).
  VkPipeline coarseCullSpotPipeline = VK_NULL_HANDLE;
  VkPipeline traditionalCullSpotPipeline = VK_NULL_HANDLE;
  VkPipeline clearIndicesSpotPipeline = VK_NULL_HANDLE;
};
