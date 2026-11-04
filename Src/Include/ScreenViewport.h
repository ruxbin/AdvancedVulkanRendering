#pragma once
#include <vulkan/vulkan.h>

// Screen-sized pipelines must not bake the initial Android surface extent:
// immersive mode can resize it after pipeline creation.
inline const VkPipelineDynamicStateCreateInfo* ScreenViewportDynamicState() {
  static const VkDynamicState states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
  static const VkPipelineDynamicStateCreateInfo info = {
      VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, nullptr, 0, 2, states};
  return &info;
}

inline void SetScreenViewport(VkCommandBuffer cmd, VkExtent2D extent) {
  const VkViewport viewport{0.f, 0.f, float(extent.width), float(extent.height), 0.f, 1.f};
  const VkRect2D scissor{{0, 0}, extent};
  vkCmdSetViewport(cmd, 0, 1, &viewport);
  vkCmdSetScissor(cmd, 0, 1, &scissor);
}
