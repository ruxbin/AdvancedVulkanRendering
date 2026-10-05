#pragma once
#include "VlmRuntimeData.h"
#include "VulkanSetup.h"
#include <string>

// Startup / pre-record only: callers must rewrite descriptors after loading.
class VlmRuntime {
public:
  VlmRuntime(VulkanDevice& device, const SH9& physicalSky);
  ~VlmRuntime();
  VlmRuntime(const VlmRuntime&) = delete;
  VlmRuntime& operator=(const VlmRuntime&) = delete;
  bool LoadFromAsset(const VlmAssetData& asset);
  bool LoadFromFile(const std::string& path, uint64_t expectedSceneHash);
  bool IsActive() const { return _active; }
  uint32_t ProbeCount() const { return _probeCount; }
  void WriteDeferredDescriptors(const std::vector<VkDescriptorSet>& sets) const { writeSets(sets,22); }
  void WriteApplDescriptors(const std::vector<VkDescriptorSet>& sets) const { writeSets(sets,9); }
private:
  struct Buffer { VkBuffer buffer=VK_NULL_HANDLE; VkDeviceMemory memory=VK_NULL_HANDLE; };
  VulkanDevice& _device;
  Buffer _params, _sh, _fallbackParams, _fallbackSh, _sky;
  bool _active=false;
  uint32_t _probeCount=0;
  Buffer upload(const void* data, VkDeviceSize bytes, VkBufferUsageFlags usage);
  void release(Buffer& b);
  void writeSets(const std::vector<VkDescriptorSet>& sets, uint32_t firstBinding) const;
};
