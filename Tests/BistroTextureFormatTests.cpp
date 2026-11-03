#include "BistroTextureFormat.h"
#include <iostream>
#include <string>

int main() {
  int failures = 0;
  auto check = [&](bool ok, const char* message) {
    if (!ok) { std::cerr << message << '\n'; ++failures; }
  };
  check(bistro::mapTextureFormat(186) == VK_FORMAT_ASTC_4x4_SRGB_BLOCK, "ASTC color must be sRGB");
  check(bistro::mapTextureFormat(204) == VK_FORMAT_ASTC_4x4_UNORM_BLOCK, "ASTC normal must be linear");
  check(bistro::mapTextureFormat(131) == VK_FORMAT_BC1_RGBA_SRGB_BLOCK, "BC1 regression");
  check(bistro::mapTextureFormat(135) == VK_FORMAT_BC3_SRGB_BLOCK, "BC3 regression");
  check(bistro::mapTextureFormat(142) == VK_FORMAT_BC5_UNORM_BLOCK, "BC5 regression");
  check(bistro::mapTextureFormat(194) == VK_FORMAT_UNDEFINED, "unimplemented 8x8 must fail explicitly");

  VkPhysicalDeviceFeatures features{};
  VkFormatProperties properties{};
  properties.optimalTilingFeatures = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
      VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT |
      VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
  const auto astc = VK_FORMAT_ASTC_4x4_UNORM_BLOCK;
  const char* error = bistro::textureFormatError(astc, features, properties);
  check(error && std::string(error).find("textureCompressionASTC_LDR") != std::string::npos,
        "unsupported ASTC device must report missing feature");
  features.textureCompressionASTC_LDR = VK_TRUE;
  check(!bistro::textureFormatError(astc, features, properties), "supported ASTC rejected");
  check(!bistro::textureFormatError(VK_FORMAT_ASTC_4x4_SRGB_BLOCK, features, properties), "supported sRGB rejected");
  for (auto bit : {VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT,
                   VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT,
                   VK_FORMAT_FEATURE_TRANSFER_SRC_BIT, VK_FORMAT_FEATURE_TRANSFER_DST_BIT}) {
    auto missing = properties;
    missing.optimalTilingFeatures &= ~bit;
    check(bistro::textureFormatError(astc, features, missing) != nullptr, "missing upload/sampling capability accepted");
  }
  check(bistro::textureFormatError(VK_FORMAT_UNDEFINED, features, properties) != nullptr, "undefined format accepted");
  check(bistro::textureFormatError(VK_FORMAT_BC3_SRGB_BLOCK, features, properties) != nullptr, "BC support not checked");
  features.textureCompressionBC = VK_TRUE;
  check(!bistro::textureFormatError(VK_FORMAT_BC3_SRGB_BLOCK, features, properties), "BC support regression");
  return failures ? 1 : 0;
}
