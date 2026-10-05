#pragma once

#include "VlmAsset.h"
#include "VlmLayout.h"
#include "VulkanSetup.h"
#include <filesystem>
#include <vector>

class GpuScene;

// VLM GPU 烘焙器(阶段 1:均匀网格,首帧一次性同步执行)。
// 复用 RayTracing 的静态 TLAS 与 GpuScene 的几何/材质/灯光/纹理 buffer;
// 自有 pipeline/SBT/descriptor,不复制相机渲染流程(规格 §2)。
class VlmBaker {
public:
  struct Settings {
    VlmUniformLayout layout{};
    uint32_t samplesPerProbe = 2048;
    uint32_t batchSamples = 32;
    uint32_t maxBounces = 6;
    bool skyOnly = false;
    bool constEnv = false;
    float constEnvRGB[3] = {1, 1, 1};
    bool sunIsEnvironment = true;
    float sunScale = 1.0f;
    float envScale = 1.0f;
    float localLightScale = 1.0f;
    std::filesystem::path hdrPathForReference; // 方向约定 CPU 参考用
  };

  VlmBaker(VulkanDevice& device, GpuScene& scene);
  VlmBaker() = delete;
  ~VlmBaker();

  // equirectView 可为 VK_NULL_HANDLE(仅 constEnv 模式允许;此时内部建 1×1 占位图)。
  void Init(VkImageView equirectView);

  // 完整烘焙:分批 dispatch → readback → finalize → FP16 打包填入 out.shFp16。
  // 返回 false = 失败(NaN/Inf、FP16 溢出、env 缺失),调用方不得发布资产。
  bool Bake(const Settings& s, VlmAssetData& out);

private:
  VulkanDevice& _device;
  GpuScene& _scene;

  void ensureHostBuffer(VkDeviceSize bytes, VkBuffer& buffer, VkDeviceMemory& memory);
  void writeAccumDescriptors(); // binding 16/17(ensureHostBuffer 重建后必须重绑)

  VkDescriptorSetLayout _setLayout = VK_NULL_HANDLE;
  VkDescriptorPool _pool = VK_NULL_HANDLE;
  VkDescriptorSet _set = VK_NULL_HANDLE;
  VkPipelineLayout _pipelineLayout = VK_NULL_HANDLE;
  VkPipeline _pipeline = VK_NULL_HANDLE;
  VkShaderModule _shaderModule = VK_NULL_HANDLE;
  VkBuffer _sbtBuffer = VK_NULL_HANDLE;
  VkDeviceMemory _sbtMemory = VK_NULL_HANDLE;
  VkStridedDeviceAddressRegionKHR _rgenRegion{};
  VkStridedDeviceAddressRegionKHR _missRegion{};
  VkStridedDeviceAddressRegionKHR _hitRegion{};
  VkStridedDeviceAddressRegionKHR _callRegion{};

  VkBuffer _shAccum = VK_NULL_HANDLE;
  VkDeviceMemory _shAccumMemory = VK_NULL_HANDLE;
  VkBuffer _errorFlags = VK_NULL_HANDLE;
  VkDeviceMemory _errorFlagsMemory = VK_NULL_HANDLE;
  VkBuffer _envCdf = VK_NULL_HANDLE;
  VkDeviceMemory _envCdfMemory = VK_NULL_HANDLE;
  VkSampler _envSampler = VK_NULL_HANDLE;
  VkImage _envFallback = VK_NULL_HANDLE;
  VkDeviceMemory _envFallbackMemory = VK_NULL_HANDLE;
  VkImageView _envFallbackView = VK_NULL_HANDLE;

  PFN_vkGetBufferDeviceAddressKHR pfnGetBufferDeviceAddress = nullptr;
  PFN_vkCreateRayTracingPipelinesKHR pfnCreateRayTracingPipelines = nullptr;
  PFN_vkGetRayTracingShaderGroupHandlesKHR pfnGetRayTracingShaderGroupHandles = nullptr;
  PFN_vkCmdTraceRaysKHR pfnCmdTraceRays = nullptr;
};
