#pragma once
#include <vulkan/vulkan_core.h>
#include <stdexcept>
#include <vector>

inline VkSurfaceFormatKHR ChooseSrgbSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& formats) {
  // VK_FORMAT_UNDEFINED in the legacy single-entry list allows any image format.
  if (formats.size() == 1 && formats[0].format == VK_FORMAT_UNDEFINED &&
      formats[0].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
    return {VK_FORMAT_B8G8R8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
  for (VkFormat preferred : {VK_FORMAT_B8G8R8A8_SRGB, VK_FORMAT_R8G8B8A8_SRGB})
    for (const auto& format : formats)
      if (format.format == preferred && format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
        return format;
  // Resolve writes linear colors. Silently accepting UNORM would omit encoding.
  throw std::runtime_error("No sRGB swapchain format available; linear resolve output requires an sRGB attachment");
}
