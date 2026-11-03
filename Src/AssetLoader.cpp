#include "AssetLoader.h"
#include <fstream>
#include <algorithm>
#include <stdexcept>

#ifdef __ANDROID__
#include <android/asset_manager.h>
#include <android/log.h>

static AAssetManager *g_assetManager = nullptr;

void AssetLoader::setAndroidAssetManager(AAssetManager *mgr) {
  g_assetManager = mgr;
}

AAssetManager *AssetLoader::getAndroidAssetManager() { return g_assetManager; }
#endif

std::vector<char> AssetLoader::readFileAsset(const std::string &path) {
#ifdef __ANDROID__
  if (!g_assetManager) {
    throw std::runtime_error("AAssetManager not initialized!");
  }
  AAsset *asset =
      AAssetManager_open(g_assetManager, path.c_str(), AASSET_MODE_BUFFER);
  if (!asset) {
    throw std::runtime_error("failed to open asset: " + path);
  }
  size_t size = AAsset_getLength(asset);
  std::vector<char> buffer(size);
  AAsset_read(asset, buffer.data(), size);
  AAsset_close(asset);
  return buffer;
#else
  std::ifstream file(path, std::ios::ate | std::ios::binary);
  if (!file.is_open()) {
    throw std::runtime_error("failed to open file: " + path);
  }
  size_t fileSize = (size_t)file.tellg();
  std::vector<char> buffer(fileSize);
  file.seekg(0);
  file.read(buffer.data(), fileSize);
  file.close();
  return buffer;
#endif
}

std::vector<unsigned char> AssetLoader::loadAssetBytes(const std::string &path) {
#ifdef __ANDROID__
  if (!g_assetManager) {
    throw std::runtime_error("AAssetManager not initialized!");
  }
  AAsset *asset =
      AAssetManager_open(g_assetManager, path.c_str(), AASSET_MODE_BUFFER);
  if (!asset) {
    throw std::runtime_error("failed to open asset: " + path);
  }
  size_t size = AAsset_getLength(asset);
  std::vector<unsigned char> buffer(size);
  AAsset_read(asset, buffer.data(), size);
  AAsset_close(asset);
  return buffer;
#else
  std::ifstream file(path, std::ios::ate | std::ios::binary);
  if (!file.is_open()) {
    throw std::runtime_error("failed to open file: " + path);
  }
  size_t fileSize = (size_t)file.tellg();
  std::vector<unsigned char> buffer(fileSize);
  file.seekg(0);
  file.read(reinterpret_cast<char *>(buffer.data()), fileSize);
  file.close();
  return buffer;
#endif
}

// --- BinaryFileReader ---

AssetLoader::BinaryFileReader::BinaryFileReader(const char *path) {
#ifdef __ANDROID__
  if (g_assetManager) {
    asset = AAssetManager_open(g_assetManager, path, AASSET_MODE_STREAMING);
  }
#else
  file = fopen(path, "rb");
#endif
}

AssetLoader::BinaryFileReader::~BinaryFileReader() { close(); }

bool AssetLoader::BinaryFileReader::isOpen() const {
#ifdef __ANDROID__
  return asset != nullptr;
#else
  return file != nullptr;
#endif
}

size_t AssetLoader::BinaryFileReader::read(void *dst, size_t size,
                                           size_t count) {
#ifdef __ANDROID__
  if (!asset)
    return 0;
  int bytesRead = AAsset_read(asset, dst, size * count);
  return bytesRead > 0 ? (size_t)bytesRead / size : 0;
#else
  if (!file)
    return 0;
  return fread(dst, size, count, file);
#endif
}

void AssetLoader::BinaryFileReader::close() {
#ifdef __ANDROID__
  if (asset) {
    AAsset_close(asset);
    asset = nullptr;
  }
#else
  if (file) {
    fclose(file);
    file = nullptr;
  }
#endif
}

uint64_t AssetLoader::BinaryFileReader::remaining() const {
  if (!isOpen())
    throw std::runtime_error("Binary asset is not open");
#ifdef __ANDROID__
  const auto size = AAsset_getRemainingLength64(asset);
  if (size < 0) throw std::runtime_error("Cannot query binary asset size");
  return static_cast<uint64_t>(size);
#else
#ifdef _WIN32
  const auto position = _ftelli64(file);
  if (position < 0 || _fseeki64(file, 0, SEEK_END) != 0)
    throw std::runtime_error("Cannot query binary asset size");
  const auto end = _ftelli64(file);
  const auto restored = _fseeki64(file, position, SEEK_SET);
#else
  const auto position = ftello(file);
  if (position < 0 || fseeko(file, 0, SEEK_END) != 0)
    throw std::runtime_error("Cannot query binary asset size");
  const auto end = ftello(file);
  const auto restored = fseeko(file, position, SEEK_SET);
#endif
  if (end < position || restored != 0)
    throw std::runtime_error("Cannot seek binary asset");
  return static_cast<uint64_t>(end - position);
#endif
}

void AssetLoader::BinaryFileReader::readExact(void *dst, size_t bytes) {
  if (bytes > remaining())
    throw std::runtime_error("Truncated binary asset");
  if (bytes && !dst)
    throw std::runtime_error("Null binary asset destination");
  auto *out = static_cast<unsigned char*>(dst);
  while (bytes) {
    // AAsset_read returns int; bounded requests also support short reads.
    const size_t request = std::min(bytes, size_t(1024 * 1024));
    const size_t got = read(out, 1, request);
    if (!got) throw std::runtime_error("Short binary asset read");
    bytes -= got;
    out += got;
  }
}
