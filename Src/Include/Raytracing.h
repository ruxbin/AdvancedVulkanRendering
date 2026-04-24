#pragma once

#include "VulkanSetup.h"
#include <vector>

class GpuScene;

// Hardware ray tracing path. One BLAS (multi-geometry, one geometry per chunk)
// + one identity TLAS instance over the static scene mesh.
//
// Frame-graph role: when GpuScene::useRayTracing == true, replace the entire
// raster path (cascade shadow / GBuffer / SAO / deferred lighting / ...) with
// a single TraceRays + Blit-to-swapchain. Primary rays from the camera, BRDF +
// N-tap sun shadow + point/spot lighting all evaluated in raygen.
class RayTracing {
public:
  RayTracing(VulkanDevice &device, GpuScene &scene);
  RayTracing() = delete;
  RayTracing(const RayTracing &) = delete;
  ~RayTracing();

  // One-time setup, called from GpuScene constructor after meshes/materials
  // are uploaded and textures are created.
  void Init();
  void BuildAccelerationStructures();
  void CreatePipelineAndSBT();             // stub for stage 4
  void CreateOutputImagesAndDescriptorSet(); // stub for stage 4

  // Per-frame recording (stub for stage 4/5).
  void RecordTraceRays(VkCommandBuffer cb, uint32_t imageIndex,
                       VkExtent2D extent);
  void RecordBlitToSwapchain(VkCommandBuffer cb, uint32_t imageIndex);
  // Begin/end a load-only render pass that lets ImGui draw on top of the
  // blitted RT output and transitions swapchain to PRESENT_SRC_KHR.
  void BeginImGuiCompositePass(VkCommandBuffer cb, uint32_t imageIndex,
                               VkExtent2D extent);
  void EndImGuiCompositePass(VkCommandBuffer cb);

  VkRenderPass GetImGuiCompositeRenderPass() const { return _rtImguiPass; }

  bool IsBuilt() const { return _tlas != VK_NULL_HANDLE; }

private:
  // helpers
  VkDeviceAddress getBufferDeviceAddress(VkBuffer buffer) const;
  void allocateBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                      VkMemoryPropertyFlags memProps,
                      VkBuffer &outBuffer,
                      VkDeviceMemory &outMemory,
                      bool deviceAddress = true);
  void loadFunctionPointers();

  void buildBLAS();
  void buildTLAS();

private:
  VulkanDevice &_device;
  GpuScene &_scene;

  // BLAS
  VkAccelerationStructureKHR _blas = VK_NULL_HANDLE;
  VkBuffer _blasBuffer = VK_NULL_HANDLE;
  VkDeviceMemory _blasBufferMemory = VK_NULL_HANDLE;
  VkDeviceAddress _blasAddress = 0;

  // TLAS
  VkAccelerationStructureKHR _tlas = VK_NULL_HANDLE;
  VkBuffer _tlasBuffer = VK_NULL_HANDLE;
  VkDeviceMemory _tlasBufferMemory = VK_NULL_HANDLE;
  VkBuffer _instancesBuffer = VK_NULL_HANDLE;
  VkDeviceMemory _instancesBufferMemory = VK_NULL_HANDLE;

  // Function pointers (loaded once via vkGetDeviceProcAddr).
  PFN_vkGetBufferDeviceAddressKHR pfnGetBufferDeviceAddress = nullptr;
  PFN_vkCreateAccelerationStructureKHR pfnCreateAccelerationStructure = nullptr;
  PFN_vkDestroyAccelerationStructureKHR pfnDestroyAccelerationStructure = nullptr;
  PFN_vkGetAccelerationStructureBuildSizesKHR pfnGetAccelerationStructureBuildSizes = nullptr;
  PFN_vkCmdBuildAccelerationStructuresKHR pfnCmdBuildAccelerationStructures = nullptr;
  PFN_vkGetAccelerationStructureDeviceAddressKHR pfnGetAccelerationStructureDeviceAddress = nullptr;
  PFN_vkCreateRayTracingPipelinesKHR pfnCreateRayTracingPipelines = nullptr;
  PFN_vkGetRayTracingShaderGroupHandlesKHR pfnGetRayTracingShaderGroupHandles = nullptr;
  PFN_vkCmdTraceRaysKHR pfnCmdTraceRays = nullptr;

  // RT pipeline / SBT
  VkDescriptorSetLayout _rtSetLayout = VK_NULL_HANDLE;
  VkDescriptorPool _rtDescriptorPool = VK_NULL_HANDLE;
  std::vector<VkDescriptorSet> _rtDescriptorSets;  // per-frame
  VkPipelineLayout _rtPipelineLayout = VK_NULL_HANDLE;
  VkPipeline _rtPipeline = VK_NULL_HANDLE;
  VkShaderModule _rtShaderModule = VK_NULL_HANDLE;
  VkBuffer _sbtBuffer = VK_NULL_HANDLE;
  VkDeviceMemory _sbtMemory = VK_NULL_HANDLE;
  VkStridedDeviceAddressRegionKHR _rgenRegion{};
  VkStridedDeviceAddressRegionKHR _missRegion{};
  VkStridedDeviceAddressRegionKHR _hitRegion{};
  VkStridedDeviceAddressRegionKHR _callRegion{};

  // Output images (per-frame; trace writes here, then blit to swapchain)
  std::vector<VkImage> _rtLitImage;
  std::vector<VkDeviceMemory> _rtLitMemory;
  std::vector<VkImageView> _rtLitImageView;
  VkExtent2D _rtExtent{};

  // Composite render pass (load swapchain, draw ImGui, present)
  VkRenderPass _rtImguiPass = VK_NULL_HANDLE;
  std::vector<VkFramebuffer> _rtImguiFrameBuffer;
};
