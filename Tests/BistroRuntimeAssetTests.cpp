// Optional full-asset acceptance test: use the existing sequential parser and
// the actual LZFSE decoder to validate every mip in the converter's output.
#include "SvtCooker.h"
#include "AssetLoader.h"
#include "lzfse.h"
#include <algorithm>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>

static uint64_t decode(const std::vector<uint8_t>& chunk, uint64_t expected) {
  if (chunk.size() < 24) throw std::runtime_error("short chunk");
  uint32_t mode;
  uint64_t raw, compressed;
  memcpy(&mode, chunk.data(), 4);
  memcpy(&raw, chunk.data() + 8, 8);
  memcpy(&compressed, chunk.data() + 16, 8);
  if (mode != 2049 || raw != expected || compressed != chunk.size() - 24)
    throw std::runtime_error("invalid chunk header/size");
  std::vector<uint8_t> bytes(static_cast<size_t>(raw));
  if (lzfse_decode_buffer(bytes.data(), bytes.size(), chunk.data() + 24,
                          compressed, nullptr) != raw)
    throw std::runtime_error("LZFSE decode failure");
  return raw;
}

int main(int argc, char** argv) try {
  if (argc != 2) throw std::runtime_error("usage: BistroRuntimeAssetTests runtime.bin");
  AssetLoader::BinaryFileReader reader(argv[1]);
  uint64_t counts[12];
  reader.readExact(counts, sizeof(counts));
  if (counts[0] != 3963005 || counts[1] != 11605365 || counts[2] != 4 || counts[11] != 202)
    throw std::runtime_error("Bistro scene counts changed");
  const uint64_t sizes[] = {counts[0]*12, counts[0]*12, counts[0]*12, counts[0]*8,
                            counts[1]*4, counts[3]*96, counts[4]*80, counts[11]*96};
  for (auto size : sizes) {
    uint64_t length;
    reader.readExact(&length, 8);
    if (length > reader.remaining()) throw std::runtime_error("section outside file");
    std::vector<uint8_t> chunk(length);
    reader.readExact(chunk.data(), chunk.size());
    decode(chunk, size);
  }
  svt::SvtBistroInfo info;
  std::string error;
  if (!svt::svtReadBistroInfo(argv[1], info, &error)) throw std::runtime_error(error);
  if (info.textures.size() != 591) throw std::runtime_error("Bistro texture count changed");
  std::ifstream file(argv[1], std::ios::binary);
  uint64_t bytes = 0, mipCount = 0;
  unsigned srgb = 0, linear = 0;
  for (const auto& t : info.textures) {
    if (t.pixelFormat == 186) ++srgb;
    else if (t.pixelFormat == 204) ++linear;
    else throw std::runtime_error("unexpected ASTC format");
    for (uint32_t m = 0; m < t.mipCount; ++m) {
      if (t.pixelDataOffset + t.mipOffsets[m] + t.mipLengths[m] > info.textureBlobLength)
        throw std::runtime_error("mip outside blob");
      file.seekg(info.textureBlobOffset + t.pixelDataOffset + t.mipOffsets[m]);
      std::vector<uint8_t> chunk(t.mipLengths[m]);
      if (!file.read(reinterpret_cast<char*>(chunk.data()), chunk.size()))
        throw std::runtime_error("short mip read");
      const uint64_t w = std::max(1u, t.width >> m), h = std::max(1u, t.height >> m);
      bytes += decode(chunk, ((w + 3) / 4) * ((h + 3) / 4) * 16);
      ++mipCount;
    }
  }
  if (srgb != 398 || linear != 193 || mipCount != 4815 || bytes != 455918352)
    throw std::runtime_error("Bistro texture inventory changed");
  std::cout << "Decoded 8 geometry/material sections and " << mipCount
            << " ASTC mips (" << bytes << " block bytes), 398 sRGB + 193 linear textures\n";
  return 0;
} catch (const std::exception& e) {
  std::cerr << e.what() << '\n';
  return 1;
}
