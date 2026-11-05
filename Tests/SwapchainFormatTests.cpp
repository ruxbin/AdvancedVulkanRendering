#include "SwapchainFormat.h"
#include <iostream>

int main() {
  const VkSurfaceFormatKHR bgra{VK_FORMAT_B8G8R8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
  const VkSurfaceFormatKHR rgba{VK_FORMAT_R8G8B8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
  const VkSurfaceFormatKHR unorm{VK_FORMAT_R8G8B8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
  if (ChooseSrgbSurfaceFormat({unorm, rgba}).format != rgba.format ||
      ChooseSrgbSurfaceFormat({rgba, bgra}).format != bgra.format ||
      ChooseSrgbSurfaceFormat({{VK_FORMAT_UNDEFINED, bgra.colorSpace}}).format != bgra.format)
    return 1;
  for (const auto& formats : {std::vector<VkSurfaceFormatKHR>{}, std::vector<VkSurfaceFormatKHR>{unorm}}) {
    try { ChooseSrgbSurfaceFormat(formats); return 1; }
    catch (const std::runtime_error&) {}
  }
}
