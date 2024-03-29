#pragma once
#include "Matrix.h"
#include "VulkanSetup.h"
#include "vulkan/vulkan.h"
#include "Camera.h"
#include <stdexcept>
#include <string_view>
#include <vector>
#include <stdio.h>

struct AAPLTextureData
{
  std::string _path;
  unsigned long long _width;
  unsigned long long _height;
  unsigned long long _mipmapLevelCount;
  unsigned long _pixelFormat;
  unsigned long long _pixelDataOffset;
  unsigned long long _pixelDataLength;
  std::vector<unsigned long>  _mipOffsets;
  std::vector<unsigned long> _mipLengths;
  AAPLTextureData()=delete;
  AAPLTextureData(FILE* f);
  AAPLTextureData(const AAPLTextureData&)=delete;
  AAPLTextureData(AAPLTextureData&& rhs);
};

struct AAPLMeshData
{
  unsigned long long _vertexCount,_indexCount,_indexType,_chunkCount,_meshCount,_opaqueChunkCount,_opaqueMeshCount,_alphaMaskedChunkCount,_alphaMaskedMeshCount,_transparentChunkCount,_transparentMeshCount,_materialCount;
  void * _vertexData;
  void * _normalData;
  void * _tangentData;
  void * _uvData;
  void * _indexData;
  void * _chunkData;
  void * _meshData;
  void * _materialData;
  std::vector<AAPLTextureData> _textures;
  void * _textureData;
  AAPLMeshData(const char * filepath);
  AAPLMeshData()=delete;
  AAPLMeshData(const AAPLMeshData&)=delete;
  ~AAPLMeshData();
};

class GpuScene {
private:
  VkBuffer vb;
  VkBuffer ib;
  VkBuffer texcoordBuffer;
  VkBuffer normalBuffer;
  // VkDevice device;
  const VulkanDevice &device;

  VkDescriptorSetLayout globalSetLayout;
  VkDescriptorPool descriptorPool;
  VkDescriptorSet globalDescriptor;

  VkBuffer uniformBuffer;
  VkDeviceMemory uniformBufferMemory;

  VkBuffer vertexBuffer;
  VkDeviceMemory vertexBufferMemory;

  VkBuffer indexBuffer;
  VkDeviceMemory indexBufferMemory;

  VkPipelineLayout pipelineLayout;
  VkPipeline graphicsPipeline;

  VkPipelineLayout epipelineLayout;
  VkPipeline egraphicsPipeline;

  VkCommandBuffer commandBuffer;
  float modelScale;

  Camera *maincamera;

  VkSemaphore imageAvailableSemaphore;
  VkSemaphore renderFinishedSemaphore;
  VkFence inFlightFence;

  VkShaderModule createShaderModule(const std::vector<char> &code);
  void createGraphicsPipeline(VkRenderPass renderPass);

  void createCommandBuffer(VkCommandPool commandPool) {
    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool = commandPool;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = 1;

    if (vkAllocateCommandBuffers(device.getLogicalDevice(), &allocInfo,
                                 &commandBuffer) != VK_SUCCESS) {
      throw std::runtime_error("failed to allocate command buffers!");
    }
    }

    void createSyncObjects();

  public:
    GpuScene(std::string_view &filepath, const VulkanDevice &deviceref);
    GpuScene() = delete;
    GpuScene(const GpuScene &) = delete;
    void Draw();
    void init_descriptors();

    struct uniformBufferData {
      mat4 projectionMatrix;
    };

    uint32_t findMemoryType(uint32_t typeFilter,
                            VkMemoryPropertyFlags properties) {
      VkPhysicalDeviceMemoryProperties memProperties;
      vkGetPhysicalDeviceMemoryProperties(device.getPhysicalDevice(),
                                          &memProperties);
      for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++) {
        if ((typeFilter & (1 << i)) &&
            (memProperties.memoryTypes[i].propertyFlags & properties) ==
                properties) {
          return i;
        }
      }

      throw std::runtime_error("failed to find suitable memory type!");
    }

    void createUniformBuffer() {
      VkBufferCreateInfo bufferInfo{};
      bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
      bufferInfo.size = sizeof(uniformBufferData);
      bufferInfo.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
      bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
      bufferInfo.flags = 0;
      if (vkCreateBuffer(device.getLogicalDevice(), &bufferInfo, nullptr,
                         &uniformBuffer) != VK_SUCCESS) {
        throw std::runtime_error("failed to create uniform buffer!");
      }
      VkMemoryRequirements memRequirements;
      vkGetBufferMemoryRequirements(device.getLogicalDevice(), uniformBuffer,
                                    &memRequirements);

      VkMemoryAllocateInfo allocInfo{};
      allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
      allocInfo.allocationSize = memRequirements.size;
      allocInfo.memoryTypeIndex =
          findMemoryType(memRequirements.memoryTypeBits,
                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                             VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

      if (vkAllocateMemory(device.getLogicalDevice(), &allocInfo, nullptr,
                           &uniformBufferMemory) != VK_SUCCESS) {
        throw std::runtime_error("failed to allocate uniform buffer memory!");
      }
      vkBindBufferMemory(device.getLogicalDevice(), uniformBuffer,
                         uniformBufferMemory, 0);
    }

    void createVertexBuffer();
    void createIndexBuffer();
    void recordCommandBuffer(int frameindex);
};
