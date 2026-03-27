#pragma once
#include <cstddef>
#include <cstdio>
#include <string>
#include <vector>

#ifdef __ANDROID__
#include <android/asset_manager.h>
#endif

namespace AssetLoader {

// Read an entire file into a byte vector (replaces readFile for shaders)
std::vector<char> readFileAsset(const std::string &path);

// Load raw bytes for use with stbi_load_from_memory, json::parse, etc.
std::vector<unsigned char> loadAssetBytes(const std::string &path);

#ifdef __ANDROID__
void setAndroidAssetManager(AAssetManager *mgr);
AAssetManager *getAndroidAssetManager();
#endif

// Platform-agnostic binary file reader (wraps fopen/AAsset)
class BinaryFileReader {
public:
  BinaryFileReader(const char *path);
  ~BinaryFileReader();

  bool isOpen() const;
  size_t read(void *dst, size_t size, size_t count);
  void close();

private:
#ifdef __ANDROID__
  AAsset *asset = nullptr;
#else
  FILE *file = nullptr;
#endif
};

} // namespace AssetLoader
