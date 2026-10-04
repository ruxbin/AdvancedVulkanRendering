#include "KtxTexture.h"

#include <cstring>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <limits>

namespace {
constexpr uint8_t kKtx1Identifier[12] = {0xAB, 0x4B, 0x54, 0x58, 0x20, 0x31,
                                         0x31, 0xBB, 0x0D, 0x0A, 0x1A, 0x0A};
constexpr uint32_t kLittleEndian = 0x04030201;

inline uint32_t readU32(const char* p) {
  const auto* b = reinterpret_cast<const uint8_t*>(p);
  return uint32_t(b[0]) | (uint32_t(b[1]) << 8) |
         (uint32_t(b[2]) << 16) | (uint32_t(b[3]) << 24);
}
} // namespace

bool KtxTexture::load(const std::filesystem::path& path, std::string& err) {
  *this = KtxTexture{};
  err.clear();
  KtxTexture parsed;
  if (!parsed.loadFile(path, err)) return false;
  *this = std::move(parsed);
  return true;
}

bool KtxTexture::loadFile(const std::filesystem::path& path, std::string& err) {
  std::ifstream f(path, std::ios::binary | std::ios::ate);
  if (!f) {
    err = "cannot open " + path.generic_string();
    return false;
  }
  const std::streamsize fileSize = f.tellg();
  if (fileSize < 64 || uint64_t(fileSize) > (std::numeric_limits<size_t>::max)()) {
    err = "invalid file size for KTX1";
    return false;
  }
  f.seekg(0);
  _bytes.resize((size_t)fileSize);
  if (!f.read(_bytes.data(), fileSize)) {
    err = "cannot read " + path.generic_string();
    return false;
  }

  if (fileSize < 64) {
    err = "file too small for KTX1 header";
    return false;
  }
  if (memcmp(_bytes.data(), kKtx1Identifier, 12) != 0) {
    err = "bad KTX1 identifier";
    return false;
  }
  if (readU32(_bytes.data() + 12) != kLittleEndian) {
    err = "big-endian KTX not supported";
    return false;
  }

  const char* h = _bytes.data() + 16; // endianness 之后 12 个 uint32
  _header.glType = readU32(h + 0);
  _header.glTypeSize = readU32(h + 4);
  _header.glFormat = readU32(h + 8);
  _header.glInternalFormat = readU32(h + 12);
  _header.glBaseInternalFormat = readU32(h + 16);
  _header.width = readU32(h + 20);
  _header.height = readU32(h + 24);
  _header.depth = readU32(h + 28);
  _header.arrayElements = readU32(h + 32);
  _header.faces = readU32(h + 36);
  _header.mipLevels = readU32(h + 40);
  _header.bytesOfKeyValueData = readU32(h + 44);

  if (_header.width == 0 || _header.height == 0 || _header.depth != 0 ||
      _header.arrayElements != 0 || (_header.faces != 1 && _header.faces != 6) ||
      (_header.faces == 6 && _header.width != _header.height)) {
    err = "expected a non-array 2D texture or square cubemap";
    return false;
  }
  uint32_t maxMips = 1;
  for (uint32_t size = (std::max)(_header.width, _header.height); size > 1; size >>= 1)
    ++maxMips;
  const uint32_t storedMips = (std::max)(1u, _header.mipLevels);
  if (storedMips > maxMips) {
    err = "too many mip levels for dimensions";
    return false;
  }
  if ((uint64_t)64 + _header.bytesOfKeyValueData > (uint64_t)fileSize) {
    err = "keyValue data overruns file";
    return false;
  }

  // --- key/value 对 ---
  size_t off = 64;
  const size_t kvEnd = off + _header.bytesOfKeyValueData;
  while (off < kvEnd) {
    if (kvEnd - off < 4) { err = "truncated keyValue size"; return false; }
    const uint32_t kvSize = readU32(_bytes.data() + off);
    off += 4;
    const uint64_t paddedSize = (uint64_t(kvSize) + 3) & ~uint64_t(3);
    if (kvSize == 0 || paddedSize > kvEnd - off) {
      err = "keyValue entry overruns metadata";
      return false;
    }
    const char* kv = _bytes.data() + off;
    // key 到第一个 NUL;value 为其余字节(可能含文本 NUL?取到 kvSize 为止再去尾部 NUL)
    const char* nul = (const char*)memchr(kv, 0, kvSize);
    if (!nul || nul == kv) { err = "invalid keyValue key"; return false; }
    {
      std::string key(kv, nul - kv);
      size_t vlen = kvSize - (nul - kv) - 1;
      // 文本值:去掉末尾的 NUL/padding
      while (vlen > 0 && kv[(nul - kv) + 1 + vlen - 1] == 0) --vlen;
      _keyValues.emplace_back(key, std::string(nul + 1, vlen));
    }
    off += size_t(paddedSize);
  }
  off = kvEnd;

  // --- mip/face 偏移 ---
  _mips.clear();
  _mips.reserve(storedMips);
  for (uint32_t m = 0; m < storedMips; ++m) {
    if ((size_t)fileSize - off < 4) {
      err = "truncated before imageSize of mip " + std::to_string(m);
      return false;
    }
    MipInfo info;
    info.imageSize = readU32(_bytes.data() + off);
    off += 4;
    if (info.imageSize == 0) { err = "empty mip image"; return false; }
    const uint64_t paddedSize = (uint64_t(info.imageSize) + 3) & ~uint64_t(3);
    // KTX1:imageSize 是单 face 的字节数(cube);非 cube 也按单 face 处理。
    // arrayElements>0 的纹理这里不支持(本项目用不到)。
    const uint32_t faceCount = _header.faces == 6 ? 6 : 1;
    for (uint32_t face = 0; face < faceCount; ++face) {
      if (paddedSize > (size_t)fileSize - off) {
        err = "truncated face data at mip " + std::to_string(m);
        return false;
      }
      info.faceOffsets.push_back(off);
      off += size_t(paddedSize); // cubePadding / mipPadding for non-array 2D
    }
    _mips.push_back(std::move(info));
  }
  return true;
}

bool KtxTexture::findKeyValue(const std::string& key, std::string& valueOut) const {
  for (const auto& kv : _keyValues) {
    if (kv.first == key) {
      valueOut = kv.second;
      return true;
    }
  }
  return false;
}

KtxTexture::FaceData KtxTexture::faceImage(uint32_t mip, uint32_t face) const {
  FaceData out;
  if (mip >= _mips.size() || face >= _mips[mip].faceOffsets.size()) return out;
  out.data = reinterpret_cast<const uint8_t*>(_bytes.data() + _mips[mip].faceOffsets[face]);
  out.size = _mips[mip].imageSize;
  return out;
}

bool KtxTexture::parseShCoefficients(float out[9][3]) const {
  std::string sh;
  if (!findKeyValue("sh", sh)) return false;
  std::istringstream ss(sh);
  for (int i = 0; i < 9; ++i)
    for (int c = 0; c < 3; ++c) {
      if (!(ss >> out[i][c])) return false;
    }
  return true;
}

bool KtxTexture::decodeFaceRGBM(uint32_t mip, uint32_t face, std::vector<float>& out,
                                uint32_t& w, uint32_t& h) const {
  // 仅支持本项目校验用到的 RGBA8 cube(glType=UBYTE, glFormat=RGBA)。
  if (_header.glType != 0x1401 || _header.glTypeSize != 1 ||
      _header.glFormat != 0x1908 || _header.glInternalFormat != 0x8058 ||
      _header.glBaseInternalFormat != 0x1908) return false;
  FaceData fd = faceImage(mip, face);
  if (!fd.data) return false;
  w = (std::max)(1u, _header.width >> mip);
  h = (std::max)(1u, _header.height >> mip);
  if ((uint64_t)w * h > fd.size / 4u || (uint64_t)w * h * 4 != fd.size) return false;
  out.resize((size_t)w * h * 3);
  for (size_t i = 0; i < (size_t)w * h; ++i) {
    const float r = fd.data[i * 4 + 0] / 255.0f;
    const float g = fd.data[i * 4 + 1] / 255.0f;
    const float b = fd.data[i * 4 + 2] / 255.0f;
    const float a = fd.data[i * 4 + 3] / 255.0f;
    out[i * 3 + 0] = 6.0f * r * a;
    out[i * 3 + 1] = 6.0f * g * a;
    out[i * 3 + 2] = 6.0f * b * a;
  }
  return true;
}
