#pragma once
#include <cstdint>
#include <vulkan/vulkan_core.h>

namespace bistro {
// Serialized values are MTLPixelFormat, not VkFormat. Only implement formats
// whose block layout is also supported by the legacy mip streaming code.
inline VkFormat mapTextureFormat(uint32_t metalFormat) {
  switch (metalFormat) {
  case 131: return VK_FORMAT_BC1_RGBA_SRGB_BLOCK;
  case 135: return VK_FORMAT_BC3_SRGB_BLOCK;
  case 142: return VK_FORMAT_BC5_UNORM_BLOCK;
  case 186: return VK_FORMAT_ASTC_4x4_SRGB_BLOCK;
  case 204: return VK_FORMAT_ASTC_4x4_UNORM_BLOCK;
  default: return VK_FORMAT_UNDEFINED;
  }
}

inline const char* textureFormatError(VkFormat format,
    const VkPhysicalDeviceFeatures& features, const VkFormatProperties& properties) {
  switch (format) {
  case VK_FORMAT_ASTC_4x4_SRGB_BLOCK:
  case VK_FORMAT_ASTC_4x4_UNORM_BLOCK:
    if (!features.textureCompressionASTC_LDR)
      return "textureCompressionASTC_LDR is unavailable (ASTC 4x4 required)";
    break;
  case VK_FORMAT_BC1_RGBA_SRGB_BLOCK:
  case VK_FORMAT_BC3_SRGB_BLOCK:
  case VK_FORMAT_BC5_UNORM_BLOCK:
    if (!features.textureCompressionBC)
      return "textureCompressionBC is unavailable";
    break;
  default:
    return "unsupported Bistro texture format";
  }
  // Streaming both uploads new mips and copies resident mips to replacement images.
  constexpr VkFormatFeatureFlags required = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
      VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT |
      VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
  if ((properties.optimalTilingFeatures & required) != required)
    return "texture format lacks optimal-tiling sampling, linear filtering, or transfer support";
  return nullptr;
}
} // namespace bistro
