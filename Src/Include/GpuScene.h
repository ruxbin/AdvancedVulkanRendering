#pragma once
#include "Matrix.h"
#include "VulkanSetup.h"
#include "vulkan/vulkan.h"
#include <stdexcept>
#include <string_view>
#include <vector>

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

VkPipelineLayout pipelineLayout;
VkPipeline graphicsPipeline;

VkPipelineLayout epipelineLayout;
VkPipeline egraphicsPipeline;

VkCommandBuffer commandBuffer;

VkShaderModule createShaderModule(const std::vector<char>& code);
void createGraphicsPipeline(VkRenderPass renderPass);

void createCommandBuffer(VkCommandPool commandPool) {
        VkCommandBufferAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocInfo.commandPool = commandPool;
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandBufferCount = 1;

        if (vkAllocateCommandBuffers(device.getLogicalDevice(), &allocInfo, &commandBuffer) != VK_SUCCESS) {
            throw std::runtime_error("failed to allocate command buffers!");
        }
    }
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
};
