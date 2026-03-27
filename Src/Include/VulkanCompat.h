#pragma once

// Compatibility shims for Vulkan functions not available in Android's libvulkan.so (API 28 = Vulkan 1.1)
// vkCmdPipelineBarrier2 is Vulkan 1.3, vkCmdDrawIndexedIndirectCount is Vulkan 1.2

#ifdef __ANDROID__
#include "vulkan/vulkan.h"
#include <vector>

// Replace vkCmdPipelineBarrier2 with vkCmdPipelineBarrier (Vulkan 1.0)
inline void vkCmdPipelineBarrier2(VkCommandBuffer commandBuffer,
                                  const VkDependencyInfo *pDependencyInfo) {
  // Convert VkMemoryBarrier2 to legacy barrier call
  VkPipelineStageFlags srcStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
  VkPipelineStageFlags dstStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;

  // Convert memory barriers
  std::vector<VkMemoryBarrier> memoryBarriers(pDependencyInfo->memoryBarrierCount);
  for (uint32_t i = 0; i < pDependencyInfo->memoryBarrierCount; i++) {
    const auto &b2 = pDependencyInfo->pMemoryBarriers[i];
    memoryBarriers[i].sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    memoryBarriers[i].pNext = nullptr;
    memoryBarriers[i].srcAccessMask = static_cast<VkAccessFlags>(b2.srcAccessMask);
    memoryBarriers[i].dstAccessMask = static_cast<VkAccessFlags>(b2.dstAccessMask);
    srcStage = static_cast<VkPipelineStageFlags>(b2.srcStageMask);
    dstStage = static_cast<VkPipelineStageFlags>(b2.dstStageMask);
  }

  // Convert image memory barriers
  std::vector<VkImageMemoryBarrier> imageBarriers(pDependencyInfo->imageMemoryBarrierCount);
  for (uint32_t i = 0; i < pDependencyInfo->imageMemoryBarrierCount; i++) {
    const auto &ib2 = pDependencyInfo->pImageMemoryBarriers[i];
    imageBarriers[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    imageBarriers[i].pNext = nullptr;
    imageBarriers[i].srcAccessMask = static_cast<VkAccessFlags>(ib2.srcAccessMask);
    imageBarriers[i].dstAccessMask = static_cast<VkAccessFlags>(ib2.dstAccessMask);
    imageBarriers[i].oldLayout = ib2.oldLayout;
    imageBarriers[i].newLayout = ib2.newLayout;
    imageBarriers[i].srcQueueFamilyIndex = ib2.srcQueueFamilyIndex;
    imageBarriers[i].dstQueueFamilyIndex = ib2.dstQueueFamilyIndex;
    imageBarriers[i].image = ib2.image;
    imageBarriers[i].subresourceRange = ib2.subresourceRange;
    srcStage = static_cast<VkPipelineStageFlags>(ib2.srcStageMask);
    dstStage = static_cast<VkPipelineStageFlags>(ib2.dstStageMask);
  }

  vkCmdPipelineBarrier(commandBuffer, srcStage, dstStage, 0,
                       pDependencyInfo->memoryBarrierCount,
                       memoryBarriers.empty() ? nullptr : memoryBarriers.data(),
                       0, nullptr,
                       pDependencyInfo->imageMemoryBarrierCount,
                       imageBarriers.empty() ? nullptr : imageBarriers.data());
}

// Replace vkCmdDrawIndexedIndirectCount with vkCmdDrawIndexedIndirect (no count buffer)
inline void vkCmdDrawIndexedIndirectCount(VkCommandBuffer commandBuffer,
                                          VkBuffer buffer, VkDeviceSize offset,
                                          VkBuffer countBuffer, VkDeviceSize countBufferOffset,
                                          uint32_t maxDrawCount, uint32_t stride) {
  // Fallback: just use maxDrawCount as the draw count (no GPU-driven count)
  vkCmdDrawIndexedIndirect(commandBuffer, buffer, offset, maxDrawCount, stride);
}

#endif // __ANDROID__
