#pragma once
#include "SkinnedMesh.h"
#include <functional>
#include <string>

class FbxLoader {
public:
  static SkinnedMeshData *
  loadFbx(const std::string &filepath, VkDevice device,
          VkPhysicalDevice physicalDevice, VkCommandPool commandPool,
          VkQueue graphicsQueue,
          uint32_t (*findMemoryType)(VkPhysicalDevice, uint32_t,
                                     VkMemoryPropertyFlags));
};
