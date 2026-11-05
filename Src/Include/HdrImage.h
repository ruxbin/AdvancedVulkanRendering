#pragma once
#include "AssetLoader.h"
#include "stb_image.h"
#include <limits>
#include <stdexcept>

// Use the same decoder for filesystem files and Android APK assets. The caller
// owns the returned linear HDR RGBA pixels and releases them with stbi_image_free.
inline float* LoadHdrImage(const std::string& path, int* width, int* height, int* channels) {
  *width = *height = *channels = 0;
  std::vector<unsigned char> bytes;
  try {
    bytes = AssetLoader::loadAssetBytes(path);
  } catch (const std::runtime_error&) {
    return nullptr; // Preserve the existing missing-environment fallback.
  }
  if (bytes.empty() || bytes.size() > static_cast<size_t>(std::numeric_limits<int>::max()))
    return nullptr;
  return stbi_loadf_from_memory(bytes.data(), static_cast<int>(bytes.size()),
                                width, height, channels, STBI_rgb_alpha);
}
