// SVT 离线切片核心实现。
// 设计依据：docs/superpowers/specs/2026-10-16-streaming-virtual-texture-design.md §6.1、§6.2。
//
// BC 路径按 4x4 block 复制内部与邻居块（Repeat 外边界按源 block 坐标取模）；
// RGBA8 路径按 texel 执行相同规则。不修改源 mip 链（保留原有 BC blocks）。

#include "SvtCooker.h"

#include "ThirdParty/lzfse.h"
#include "nlohmann/json.hpp"

#include <algorithm>
#include <cstring>

namespace svt {

namespace {

// ---------------------------------------------------------------
// 小端磁盘写入（与 SvtAsset.cpp 的读取端逐字段对应）
// ---------------------------------------------------------------

#ifdef _WIN32
inline int seek64(FILE *f, long long off, int whence) { return _fseeki64(f, off, whence); }
inline long long tell64(FILE *f) { return _ftelli64(f); }
#else
inline int seek64(FILE *f, long long off, int whence) { return fseeko(f, off, whence); }
inline long long tell64(FILE *f) { return ftello(f); }
#endif

bool writeExact(FILE *f, const void *src, size_t n) { return fwrite(src, 1, n, f) == n; }
bool writeU32(FILE *f, uint32_t v) { return writeExact(f, &v, sizeof(v)); }
bool writeU64(FILE *f, uint64_t v) { return writeExact(f, &v, sizeof(v)); }

std::string cookFail(std::string *error, const std::string &msg) {
  if (error)
    *error = msg;
  return msg;
}

// ---------------------------------------------------------------
// LZFSE 压缩（压不小则回退 raw）
// ---------------------------------------------------------------

class LzfseCompressor {
public:
  LzfseCompressor() {
    const size_t scratch = lzfse_encode_scratch_size();
    if (scratch)
      scratch_.resize(scratch);
    else
      scratch_.resize(1);
  }

  // 返回 codec；stored 为实际落盘字节。
  uint32_t compress(const uint8_t *payload, uint32_t n, bool allowCompress,
                    std::vector<uint8_t> &stored) {
    if (allowCompress && n > 0) {
      stored.resize(n);
      const size_t r =
          lzfse_encode_buffer(stored.data(), stored.size(), payload, n, scratch_.data());
      if (r > 0 && r < n) {
        stored.resize(r);
        return kSvtCodecLzfse;
      }
    }
    stored.assign(payload, payload + n);
    return kSvtCodecRaw;
  }

private:
  std::vector<uint8_t> scratch_;
};

// ---------------------------------------------------------------
// 含边框 tile 的构造（设计 §6.2 block-copy）
// ---------------------------------------------------------------

void buildTilePayload(const SvtSourceTexture &src, uint32_t mip, uint32_t tx, uint32_t ty,
                      const SvtCookParams &p, std::vector<uint8_t> &out) {
  const SvtFormat fmt = src.format;
  const uint32_t mw = svtMipDim(src.width, mip);
  const uint32_t mh = svtMipDim(src.height, mip);
  const uint8_t *mipData = src.mips[mip].data();
  const uint32_t padded = p.tileSize + 2 * p.border;

  if (svtFormatIsBlockCompressed(fmt)) {
    const uint32_t blockSize = svtFormatBlockSize(fmt);
    const uint32_t bytesPerBlock = svtFormatBytesPerUnit(fmt);
    const uint32_t blocksWide = static_cast<uint32_t>(std::max<uint64_t>(1, (mw + blockSize - 1) / blockSize));
    const uint32_t blocksHigh = static_cast<uint32_t>(std::max<uint64_t>(1, (mh + blockSize - 1) / blockSize));
    const uint32_t blocksPerSide = padded / blockSize;

    out.assign(static_cast<size_t>(blocksPerSide) * blocksPerSide * bytesPerBlock, 0);
    for (uint32_t by = 0; by < blocksPerSide; ++by) {
      // padded block 覆盖的源 texel 起点（负值表示左侧/上侧边框）。
      const int64_t y0 = static_cast<int64_t>(ty) * p.tileSize +
                         static_cast<int64_t>(by) * blockSize - p.border;
      const uint32_t sby = svtWrapRepeat(y0 / blockSize, blocksHigh);
      for (uint32_t bx = 0; bx < blocksPerSide; ++bx) {
        const int64_t x0 = static_cast<int64_t>(tx) * p.tileSize +
                           static_cast<int64_t>(bx) * blockSize - p.border;
        const uint32_t sbx = svtWrapRepeat(x0 / blockSize, blocksWide);
        const size_t srcBlock = (static_cast<size_t>(sby) * blocksWide + sbx) * bytesPerBlock;
        const size_t dstBlock = (static_cast<size_t>(by) * blocksPerSide + bx) * bytesPerBlock;
        memcpy(out.data() + dstBlock, mipData + srcBlock, bytesPerBlock);
      }
    }
  } else {
    const uint32_t bytesPerTexel = svtFormatBytesPerUnit(fmt);
    out.assign(static_cast<size_t>(padded) * padded * bytesPerTexel, 0);
    for (uint32_t py = 0; py < padded; ++py) {
      const int64_t sy = static_cast<int64_t>(ty) * p.tileSize + py - p.border;
      const uint32_t wsy = svtWrapRepeat(sy, mh);
      for (uint32_t px = 0; px < padded; ++px) {
        const int64_t sx = static_cast<int64_t>(tx) * p.tileSize + px - p.border;
        const uint32_t wsx = svtWrapRepeat(sx, mw);
        memcpy(out.data() + (static_cast<size_t>(py) * padded + px) * bytesPerTexel,
               mipData + (static_cast<size_t>(wsy) * mw + wsx) * bytesPerTexel, bytesPerTexel);
      }
    }
  }
}

// ---------------------------------------------------------------
// 诊断纹理生成
// ---------------------------------------------------------------

struct Lcg64 {
  uint64_t s;
  explicit Lcg64(uint64_t seed) : s(seed ? seed : 0x9E3779B97F4A7C15ull) {}
  uint32_t next32() {
    s = s * 6364136223846793005ull + 1442695040888963407ull;
    return static_cast<uint32_t>(s >> 33);
  }
  uint8_t nextByte() { return static_cast<uint8_t>(next32() & 0xFFu); }
};

// 2x2 box filter 生成下一层 mip（RGBA8，逐通道平均，四舍五入）。
// 短边为 1 的 mip（如 2x1）按 clamp 读取，避免越过源行/列。
void downsampleRgba8(const std::vector<uint8_t> &src, uint32_t w, uint32_t h,
                     std::vector<uint8_t> &dst) {
  const uint32_t dw = std::max(1u, w / 2);
  const uint32_t dh = std::max(1u, h / 2);
  dst.assign(static_cast<size_t>(dw) * dh * 4, 0);
  for (uint32_t y = 0; y < dh; ++y)
    for (uint32_t x = 0; x < dw; ++x) {
      const uint32_t y0 = y * 2, y1 = std::min(y * 2 + 1, h - 1);
      const uint32_t x0 = x * 2, x1 = std::min(x * 2 + 1, w - 1);
      for (uint32_t c = 0; c < 4; ++c) {
        const uint32_t sum = src[(static_cast<size_t>(y0) * w + x0) * 4 + c] +
                             src[(static_cast<size_t>(y0) * w + x1) * 4 + c] +
                             src[(static_cast<size_t>(y1) * w + x0) * 4 + c] +
                             src[(static_cast<size_t>(y1) * w + x1) * 4 + c];
        dst[(static_cast<size_t>(y) * dw + x) * 4 + c] = static_cast<uint8_t>((sum + 2) / 4);
      }
    }
}

SvtSourceTexture makeRgba8Chain(uint32_t width, uint32_t height,
                                const std::vector<uint8_t> &mip0) {
  SvtSourceTexture t;
  t.width = width;
  t.height = height;
  t.format = SvtFormat::RGBA8;
  t.mipCount = svtFullMipCount(width, height);
  t.mips.resize(t.mipCount);
  t.mips[0] = mip0;
  uint32_t w = width, h = height;
  for (uint32_t m = 1; m < t.mipCount; ++m) {
    downsampleRgba8(t.mips[m - 1], w, h, t.mips[m]);
    w = svtMipDim(width, m);
    h = svtMipDim(height, m);
  }
  return t;
}

} // namespace

// ---------------------------------------------------------------------------
// 源校验与内容 hash
// ---------------------------------------------------------------------------

bool svtValidateSource(const SvtSourceTexture &src, const SvtCookParams &params,
                       std::string *error) {
  if (!svtFormatIsValid(static_cast<uint32_t>(src.format)))
    return cookFail(error, "unknown source format"), false;
  if (src.width == 0 || src.height == 0 || src.width > kSvtMaxTextureDim ||
      src.height > kSvtMaxTextureDim)
    return cookFail(error, "source dimensions out of range"), false;
  if ((src.width & (src.width - 1)) != 0 || (src.height & (src.height - 1)) != 0)
    return cookFail(error, "source dimensions must be power of two"), false;
  if (params.tileSize == 0 || params.tileSize > kSvtMaxTileSize || params.border > kSvtMaxBorder ||
      params.border * 2 >= params.tileSize)
    return cookFail(error, "invalid tileSize/border"), false;
  if (svtFormatIsBlockCompressed(src.format) &&
      (params.tileSize % 4 != 0 || params.border % 4 != 0))
    return cookFail(error, "tileSize/border must be block aligned for BC formats"), false;
  const uint32_t fullChain = svtFullMipCount(src.width, src.height);
  if (src.mipCount != fullChain || src.mips.size() != src.mipCount)
    return cookFail(error, "source mip chain incomplete"), false;
  for (uint32_t m = 0; m < src.mipCount; ++m) {
    const uint64_t expected = svtMipByteSize(src.format, svtMipDim(src.width, m),
                                             svtMipDim(src.height, m));
    if (src.mips[m].size() != expected)
      return cookFail(error, "source mip byte size mismatch"), false;
  }
  return true;
}

uint64_t svtComputeContentHash(const SvtSourceTexture &src) {
  uint64_t h = 14695981039346656037ull;
  const auto mixU32 = [&](uint32_t v) {
    uint8_t b[4] = {static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8),
                    static_cast<uint8_t>(v >> 16), static_cast<uint8_t>(v >> 24)};
    h = svtFnv1a64(b, 4, h);
  };
  mixU32(src.width);
  mixU32(src.height);
  mixU32(src.mipCount);
  mixU32(static_cast<uint32_t>(src.format));
  for (uint32_t m = 0; m < src.mipCount; ++m) {
    const uint64_t n = src.mips[m].size();
    uint8_t b[8];
    for (int i = 0; i < 8; ++i)
      b[i] = static_cast<uint8_t>(n >> (8 * i));
    h = svtFnv1a64(b, 8, h);
    h = svtFnv1a64(src.mips[m].data(), src.mips[m].size(), h);
  }
  return h;
}

// ---------------------------------------------------------------------------
// cook 主流程
// ---------------------------------------------------------------------------

bool svtCookTexture(const SvtSourceTexture &src, const SvtCookParams &params,
                    const std::filesystem::path &outPath, SvtCookStats *stats,
                    std::string *error) {
  if (!svtValidateSource(src, params, error))
    return false;

  const uint32_t tailFirstMip =
      svtComputeTailFirstMip(src.width, src.height, src.mipCount, params.tileSize);
  const uint64_t tileCount = svtPageTableEntryCount(src.width, src.height, tailFirstMip, params.tileSize);
  if (tileCount > kSvtMaxTileEntries)
    return cookFail(error, "tile count exceeds cap"), false;

#ifdef _WIN32
  FILE *f = nullptr;
  if (_wfopen_s(&f, outPath.wstring().c_str(), L"wb") != 0 || !f)
#else
  FILE *f = fopen(outPath.string().c_str(), "wb");
  if (!f)
#endif
    return cookFail(error, "cannot create output file"), false;

  LzfseCompressor lzfse;
  uint64_t cursor = kSvtHeaderSize;
  std::vector<SvtAsset::Entry> tileEntries, tailEntries;

  // 占位 header；最终在文件末尾回写。
  {
    std::vector<uint8_t> zero(kSvtHeaderSize, 0);
    if (!writeExact(f, zero.data(), zero.size())) {
      fclose(f);
      return cookFail(error, "write header placeholder failed"), false;
    }
  }

  // ---- tile payloads（按 mip/y/x 顺序，与目录排序一致）
  std::vector<uint8_t> payload, stored;
  for (uint32_t m = 0; m < tailFirstMip; ++m) {
    const uint32_t tilesX = svtTilesPerAxis(svtMipDim(src.width, m), params.tileSize);
    const uint32_t tilesY = svtTilesPerAxis(svtMipDim(src.height, m), params.tileSize);
    for (uint32_t ty = 0; ty < tilesY; ++ty)
      for (uint32_t tx = 0; tx < tilesX; ++tx) {
        buildTilePayload(src, m, tx, ty, params, payload);
        SvtAsset::Entry e{};
        e.decodedSize = static_cast<uint32_t>(payload.size());
        e.crc32 = svtCrc32(payload.data(), payload.size());
        e.codec = lzfse.compress(payload.data(), e.decodedSize, params.compress, stored);
        e.storedSize = static_cast<uint32_t>(stored.size());
        e.fileOffset = cursor;
        if (!writeExact(f, stored.data(), stored.size())) {
          fclose(f);
          return cookFail(error, "write tile payload failed"), false;
        }
        cursor += stored.size();
        tileEntries.push_back(e);
      }
  }

  // ---- tail payloads（tailFirstMip .. 1x1 的完整 mip）
  for (uint32_t m = tailFirstMip; m < src.mipCount; ++m) {
    const std::vector<uint8_t> &mip = src.mips[m];
    SvtAsset::Entry e{};
    e.decodedSize = static_cast<uint32_t>(mip.size());
    e.crc32 = svtCrc32(mip.data(), mip.size());
    e.codec = lzfse.compress(mip.data(), e.decodedSize, params.compress, stored);
    e.storedSize = static_cast<uint32_t>(stored.size());
    e.fileOffset = cursor;
    if (!writeExact(f, stored.data(), stored.size())) {
      fclose(f);
      return cookFail(error, "write tail payload failed"), false;
    }
    cursor += stored.size();
    tailEntries.push_back(e);
  }

  // ---- directories
  const uint64_t mipDirOffset = cursor;
  for (uint32_t m = 0; m < src.mipCount; ++m) {
    const bool tiled = m < tailFirstMip;
    const uint32_t tilesX = tiled ? svtTilesPerAxis(svtMipDim(src.width, m), params.tileSize) : 0;
    const uint32_t tilesY = tiled ? svtTilesPerAxis(svtMipDim(src.height, m), params.tileSize) : 0;
    uint32_t firstTile = 0;
    if (tiled)
      for (uint32_t mm = 0; mm < m; ++mm)
        firstTile += svtTilesPerAxis(svtMipDim(src.width, mm), params.tileSize) *
                     svtTilesPerAxis(svtMipDim(src.height, mm), params.tileSize);
    if (!writeU32(f, svtMipDim(src.width, m)) || !writeU32(f, svtMipDim(src.height, m)) ||
        !writeU32(f, tilesX) || !writeU32(f, tilesY) || !writeU32(f, firstTile)) {
      fclose(f);
      return cookFail(error, "write mip directory failed"), false;
    }
    cursor += 20;
  }

  const uint64_t tileDirOffset = cursor;
  for (const SvtAsset::Entry &e : tileEntries) {
    if (!writeU64(f, e.fileOffset) || !writeU32(f, e.storedSize) || !writeU32(f, e.decodedSize) ||
        !writeU32(f, e.codec) || !writeU32(f, e.crc32)) {
      fclose(f);
      return cookFail(error, "write tile directory failed"), false;
    }
    cursor += 24;
  }

  const uint64_t tailDirOffset = cursor;
  for (const SvtAsset::Entry &e : tailEntries) {
    if (!writeU64(f, e.fileOffset) || !writeU32(f, e.storedSize) || !writeU32(f, e.decodedSize) ||
        !writeU32(f, e.codec) || !writeU32(f, e.crc32)) {
      fclose(f);
      return cookFail(error, "write tail directory failed"), false;
    }
    cursor += 24;
  }

  // ---- header 回写
  const bool okHeader =
      seek64(f, 0, SEEK_SET) == 0 && writeU32(f, kSvtMagic) && writeU32(f, kSvtVersion) &&
      writeU32(f, kSvtHeaderSize) && writeU32(f, kSvtFlagLittleEndian) &&
      writeU32(f, src.width) && writeU32(f, src.height) && writeU32(f, src.mipCount) &&
      writeU32(f, params.tileSize) && writeU32(f, params.border) &&
      writeU32(f, static_cast<uint32_t>(src.format)) &&
      writeU32(f, svtFormatIsSrgb(src.format) ? 1u : 0u) && writeU32(f, kSvtAddressRepeat) &&
      writeU32(f, tailFirstMip) && writeU32(f, src.mipCount) &&
      writeU32(f, static_cast<uint32_t>(tileEntries.size())) &&
      writeU32(f, static_cast<uint32_t>(tailEntries.size())) && writeU64(f, mipDirOffset) &&
      writeU64(f, tileDirOffset) && writeU64(f, tailDirOffset) && writeU64(f, cursor) &&
      writeU64(f, svtComputeContentHash(src)) && writeU32(f, 0) && writeU32(f, 0) &&
      writeU32(f, 0) && writeU32(f, 0);
  if (!okHeader) {
    fclose(f);
    return cookFail(error, "write header failed"), false;
  }

  if (fflush(f) != 0) {
    fclose(f);
    return cookFail(error, "flush failed"), false;
  }
  fclose(f);

  if (stats) {
    stats->tileCount = tileEntries.size();
    stats->tailMipCount = tailEntries.size();
    stats->fileBytes = cursor;
    stats->rawPayloadBytes = 0;
    for (const auto &e : tileEntries)
      stats->rawPayloadBytes += e.decodedSize;
    for (const auto &e : tailEntries)
      stats->rawPayloadBytes += e.decodedSize;
  }
  return true;
}

// ---------------------------------------------------------------------------
// 诊断纹理
// ---------------------------------------------------------------------------

SvtSourceTexture svtMakeDiagnosticTexture(uint32_t width, uint32_t height, uint32_t tileSize) {
  std::vector<uint8_t> mip0(static_cast<size_t>(width) * height * 4);
  const uint32_t checker = 32;
  for (uint32_t y = 0; y < height; ++y)
    for (uint32_t x = 0; x < width; ++x) {
      uint8_t *px = &mip0[(static_cast<size_t>(y) * width + x) * 4];
      const bool grid = tileSize > 0 && (x % tileSize < 2 || y % tileSize < 2);
      const bool dark = ((x / checker) + (y / checker)) % 2 == 0;
      if (grid) {
        px[0] = 50;
        px[1] = 50;
        px[2] = 60;
      } else {
        px[0] = static_cast<uint8_t>(dark ? 225 : 175);
        px[1] = static_cast<uint8_t>((dark ? 225 : 175) - y * 60 / (height ? height : 1));
        px[2] = static_cast<uint8_t>((dark ? 225 : 175) - x * 60 / (width ? width : 1));
      }
      px[3] = 255;
    }
  SvtSourceTexture t = makeRgba8Chain(width, height, mip0);
  // 每 mip 轻微色调标记（mip 越深越蓝），便于视觉回归区分层号。
  for (uint32_t m = 0; m < t.mipCount; ++m) {
    const uint32_t gScale = 100 - std::min<uint32_t>(m * 6, 60);
    const uint32_t bScale = 100 + std::min<uint32_t>(m * 10, 100);
    for (size_t i = 0; i < t.mips[m].size(); i += 4) {
      t.mips[m][i + 1] = static_cast<uint8_t>(t.mips[m][i + 1] * gScale / 100);
      t.mips[m][i + 2] = static_cast<uint8_t>(std::min<uint32_t>(
          255, t.mips[m][i + 2] * bScale / 100 + m * 8));
    }
  }
  return t;
}

SvtSourceTexture svtMakeNoiseTexture(uint32_t width, uint32_t height, uint64_t seed) {
  std::vector<uint8_t> mip0(static_cast<size_t>(width) * height * 4);
  Lcg64 rng(seed);
  for (size_t i = 0; i < mip0.size(); ++i)
    mip0[i] = rng.nextByte();
  return makeRgba8Chain(width, height, mip0);
}

SvtSourceTexture svtMakeNoiseBcTexture(SvtFormat format, uint32_t width, uint32_t height,
                                       uint64_t seed, bool compressible) {
  SvtSourceTexture t;
  t.width = width;
  t.height = height;
  t.format = format;
  t.mipCount = svtFullMipCount(width, height);
  t.mips.resize(t.mipCount);
  const uint32_t bytesPerBlock = svtFormatBytesPerUnit(format);
  Lcg64 rng(seed);
  for (uint32_t m = 0; m < t.mipCount; ++m) {
    const uint32_t mw = svtMipDim(width, m), mh = svtMipDim(height, m);
    const uint64_t bw = std::max<uint64_t>(1, (mw + 3) / 4);
    const uint64_t bh = std::max<uint64_t>(1, (mh + 3) / 4);
    t.mips[m].assign(bw * bh * bytesPerBlock, 0);
    for (size_t b = 0; b < t.mips[m].size(); b += bytesPerBlock) {
      if (compressible) {
        // 常量模板 + 低字节序号：字节级确定且高度可压缩。
        for (uint32_t i = 0; i < bytesPerBlock; ++i)
          t.mips[m][b + i] = static_cast<uint8_t>(0x40 + i * 8);
        t.mips[m][b] = static_cast<uint8_t>(b / bytesPerBlock);
      } else {
        for (uint32_t i = 0; i < bytesPerBlock; ++i)
          t.mips[m][b + i] = rng.nextByte();
      }
    }
  }
  return t;
}

// ---------------------------------------------------------------------------
// bistro.dxt.bin 解析（与 AAPLMeshData/AAPLTextureData 的磁盘布局逐字段对应）
// ---------------------------------------------------------------------------

namespace {

bool readExactImpl(FILE *f, void *dst, size_t n) { return fread(dst, 1, n, f) == n; }
bool readU64F(FILE *f, uint64_t &v) { return readExactImpl(f, &v, sizeof(v)); }
bool readU32F(FILE *f, uint32_t &v) { return readExactImpl(f, &v, sizeof(v)); }

// 与 GpuScene.cpp 的 AAPLCompressionHeader 相同的 24 字节磁盘布局：
// u32 mode | 4 字节 padding | u64 uncompressedSize | u64 compressedSize
struct AppleChunkHeader {
  uint32_t mode = 0;
  uint64_t uncompressedSize = 0;
  uint64_t compressedSize = 0;
};

bool readAppleChunkHeader(FILE *f, AppleChunkHeader &h) {
  uint32_t pad = 0;
  return readU32F(f, h.mode) && readU32F(f, pad) &&
         readU64F(f, h.uncompressedSize) && readU64F(f, h.compressedSize);
}

} // namespace

bool svtReadBistroInfo(const std::filesystem::path &binPath, SvtBistroInfo &out,
                       std::string *error) {
  out = SvtBistroInfo{};
#ifdef _WIN32
  FILE *f = nullptr;
  if (_wfopen_s(&f, binPath.wstring().c_str(), L"rb") != 0 || !f)
#else
  FILE *f = fopen(binPath.string().c_str(), "rb");
  if (!f)
#endif
    return cookFail(error, "cannot open bistro bin"), false;

  // AAPLMeshData 头部：12 个 u64 计数字段
  // （vertexCount, indexCount, indexType, chunkCount, meshCount, opaqueChunkCount,
  //  opaqueMeshCount, alphaMaskedChunkCount, alphaMaskedMeshCount,
  //  transparentChunkCount, transparentMeshCount, materialCount）。
  uint64_t counts[12] = {};
  for (int i = 0; i < 12; ++i)
    if (!readU64F(f, counts[i])) {
      fclose(f);
      return cookFail(error, "truncated counts"), false;
    }

  // 布局自校验：chunk/mesh 分类计数总和必须等于总数。
  // 若此处失败，说明 .dxt.bin 头部布局与本解析器不再一致。
  if (counts[5] + counts[7] + counts[9] != counts[3] ||
      counts[6] + counts[8] + counts[10] != counts[4]) {
    fclose(f);
    return cookFail(error, "bistro header layout mismatch (chunk/mesh counts)"), false;
  }

  // 8 个压缩数据段：u64 length + data，全部跳过。
  for (int i = 0; i < 8; ++i) {
    uint64_t len = 0;
    if (!readU64F(f, len) || seek64(f, static_cast<long long>(len), SEEK_CUR) != 0) {
      fclose(f);
      return cookFail(error, "truncated data section"), false;
    }
  }

  uint64_t textureCount = 0;
  if (!readU64F(f, textureCount) || textureCount > 65536) {
    fclose(f);
    return cookFail(error, "bad texture count"), false;
  }

  out.textures.resize(static_cast<size_t>(textureCount));
  for (uint64_t i = 0; i < textureCount; ++i) {
    SvtBistroTextureInfo &tex = out.textures[i];
    uint32_t pathLength = 0;
    if (!readU32F(f, pathLength) || pathLength == 0 || pathLength > 4096) {
      fclose(f);
      return cookFail(error, "bad texture path length"), false;
    }
    std::vector<char> path(pathLength + 1, 0);
    if (!readExactImpl(f, path.data(), pathLength)) {
      fclose(f);
      return cookFail(error, "truncated texture path"), false;
    }
    tex.path.assign(path.data(), pathLength);
    uint64_t w = 0, h = 0, mips = 0, pixelOffset = 0, pixelLength = 0;
    if (!readU32F(f, tex.pathHash) || !readU64F(f, w) || !readU64F(f, h) ||
        !readU64F(f, mips) || !readU32F(f, tex.pixelFormat) || !readU64F(f, pixelOffset) ||
        !readU64F(f, pixelLength) || mips == 0 || mips > 32 || w == 0 || h == 0 ||
        w > 0xFFFFFFFFull || h > 0xFFFFFFFFull) {
      fclose(f);
      return cookFail(error, "bad texture descriptor"), false;
    }
    tex.width = static_cast<uint32_t>(w);
    tex.height = static_cast<uint32_t>(h);
    tex.mipCount = static_cast<uint32_t>(mips);
    tex.pixelDataOffset = pixelOffset;
    tex.pixelDataLength = pixelLength;
    tex.mipOffsets.resize(mips);
    tex.mipLengths.resize(mips);
    for (uint64_t m = 0; m < mips; ++m)
      if (!readU64F(f, tex.mipOffsets[m])) {
        fclose(f);
        return cookFail(error, "truncated mip offsets"), false;
      }
    for (uint64_t m = 0; m < mips; ++m)
      if (!readU64F(f, tex.mipLengths[m])) {
        fclose(f);
        return cookFail(error, "truncated mip lengths"), false;
      }
  }

  uint64_t blobLength = 0;
  if (!readU64F(f, blobLength)) {
    fclose(f);
    return cookFail(error, "truncated blob length"), false;
  }
  out.textureBlobLength = blobLength;
  out.textureBlobOffset = static_cast<uint64_t>(tell64(f));
  fclose(f);
  return true;
}

bool svtLoadBistroTexture(const std::filesystem::path &binPath, const SvtBistroInfo &info,
                          size_t textureIndex, SvtSourceTexture &out, std::string *error) {
  out = SvtSourceTexture{};
  if (textureIndex >= info.textures.size())
    return cookFail(error, "texture index out of range"), false;
  const SvtBistroTextureInfo &tex = info.textures[textureIndex];
  SvtFormat fmt;
  if (!svtFormatFromMtlPixelFormat(tex.pixelFormat, fmt))
    return cookFail(error, "unsupported pixel format " + std::to_string(tex.pixelFormat)), false;

#ifdef _WIN32
  FILE *f = nullptr;
  if (_wfopen_s(&f, binPath.wstring().c_str(), L"rb") != 0 || !f)
#else
  FILE *f = fopen(binPath.string().c_str(), "rb");
  if (!f)
#endif
    return cookFail(error, "cannot open bistro bin"), false;

  out.width = tex.width;
  out.height = tex.height;
  out.mipCount = tex.mipCount;
  out.format = fmt;
  out.mips.resize(tex.mipCount);

  const uint64_t base = info.textureBlobOffset + tex.pixelDataOffset;
  for (uint32_t m = 0; m < tex.mipCount; ++m) {
    const uint64_t chunkOffset = base + tex.mipOffsets[m];
    const uint64_t chunkLength = tex.mipLengths[m];
    // 基址 + 段内偏移 + 长度都必须落在 blob 范围内（有界检查）。
    if (chunkOffset < info.textureBlobOffset ||
        chunkOffset - info.textureBlobOffset > info.textureBlobLength ||
        chunkLength > info.textureBlobLength - (chunkOffset - info.textureBlobOffset)) {
      fclose(f);
      return cookFail(error, "mip chunk out of blob range"), false;
    }
    if (seek64(f, static_cast<long long>(chunkOffset), SEEK_SET) != 0) {
      fclose(f);
      return cookFail(error, "seek failed"), false;
    }
    AppleChunkHeader ch;
    if (!readAppleChunkHeader(f, ch) || ch.mode != 2049 ||
        24 + ch.compressedSize != chunkLength) {
      fclose(f);
      return cookFail(error, "bad apple compression header"), false;
    }
    const uint64_t expected =
        svtMipByteSize(fmt, svtMipDim(tex.width, m), svtMipDim(tex.height, m));
    if (ch.uncompressedSize != expected) {
      fclose(f);
      return cookFail(error, "mip uncompressed size mismatch"), false;
    }
    std::vector<uint8_t> compressed(static_cast<size_t>(ch.compressedSize));
    if (!readExactImpl(f, compressed.data(), compressed.size())) {
      fclose(f);
      return cookFail(error, "truncated mip payload"), false;
    }
    out.mips[m].resize(static_cast<size_t>(expected));
    const size_t decoded = lzfse_decode_buffer(out.mips[m].data(), out.mips[m].size(),
                                               compressed.data(), compressed.size(), nullptr);
    if (decoded != out.mips[m].size()) {
      fclose(f);
      return cookFail(error, "lzfse decode failed"), false;
    }
  }
  fclose(f);
  return true;
}

bool svtFormatFromMtlPixelFormat(uint32_t mtlPixelFormat, SvtFormat &out) {
  switch (mtlPixelFormat) {
  case 130: out = SvtFormat::BC1; return true;
  case 131: out = SvtFormat::BC1_SRGB; return true;
  case 134: out = SvtFormat::BC3; return true;
  case 135: out = SvtFormat::BC3_SRGB; return true;
  case 140: out = SvtFormat::BC4; return true;
  case 142: out = SvtFormat::BC5; return true;
  case 143: out = SvtFormat::BC5_SNORM; return true;
  case 152: out = SvtFormat::BC7; return true;
  case 153: out = SvtFormat::BC7_SRGB; return true;
  default: return false;
  }
}

// ---------------------------------------------------------------------------
// manifest（nlohmann/json；与渲染器现有依赖一致）
// ---------------------------------------------------------------------------

bool svtWriteManifest(const std::filesystem::path &manifestPath,
                      const std::vector<SvtManifestEntry> &entries, std::string *error) {
  nlohmann::json j;
  j["version"] = 1;
  j["entries"] = nlohmann::json::array();
  for (const SvtManifestEntry &e : entries) {
    nlohmann::json je;
    je["sourcePath"] = e.sourcePath;
    je["pathHash"] = e.pathHash;
    je["contentHash"] = e.contentHash; // json 以 uint64 精确往返
    je["svtFile"] = e.svtFile;
    je["width"] = e.width;
    je["height"] = e.height;
    je["mipCount"] = e.mipCount;
    je["format"] = static_cast<uint32_t>(e.format);
    je["tailFirstMip"] = e.tailFirstMip;
    je["tileCount"] = e.tileCount;
    j["entries"].push_back(je);
  }
#ifdef _WIN32
  FILE *f = nullptr;
  if (_wfopen_s(&f, manifestPath.wstring().c_str(), L"wb") != 0 || !f)
#else
  FILE *f = fopen(manifestPath.string().c_str(), "wb");
  if (!f)
#endif
    return cookFail(error, "cannot create manifest"), false;
  const std::string text = j.dump(2);
  const bool ok = writeExact(f, text.data(), text.size()) && fflush(f) == 0;
  fclose(f);
  if (!ok)
    return cookFail(error, "write manifest failed"), false;
  return true;
}

bool svtReadManifest(const std::filesystem::path &manifestPath,
                     std::vector<SvtManifestEntry> &out, std::string *error) {
  out.clear();
#ifdef _WIN32
  FILE *f = nullptr;
  if (_wfopen_s(&f, manifestPath.wstring().c_str(), L"rb") != 0 || !f)
#else
  FILE *f = fopen(manifestPath.string().c_str(), "rb");
  if (!f)
#endif
    return cookFail(error, "cannot open manifest"), false;
  std::string text;
  {
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
      text.append(buf, n);
  }
  fclose(f);
  try {
    const nlohmann::json j = nlohmann::json::parse(text);
    if (!j.contains("version") || j["version"] != 1 || !j.contains("entries"))
      return cookFail(error, "manifest version/entries missing"), false;
    for (const auto &je : j["entries"]) {
      SvtManifestEntry e;
      e.sourcePath = je.at("sourcePath").get<std::string>();
      e.pathHash = je.at("pathHash").get<uint32_t>();
      e.contentHash = je.at("contentHash").get<uint64_t>();
      e.svtFile = je.at("svtFile").get<std::string>();
      e.width = je.at("width").get<uint32_t>();
      e.height = je.at("height").get<uint32_t>();
      e.mipCount = je.at("mipCount").get<uint32_t>();
      e.format = static_cast<SvtFormat>(je.at("format").get<uint32_t>());
      e.tailFirstMip = je.at("tailFirstMip").get<uint32_t>();
      e.tileCount = je.at("tileCount").get<uint64_t>();
      if (!svtFormatIsValid(static_cast<uint32_t>(e.format)))
        return cookFail(error, "manifest has unknown format"), false;
      out.push_back(e);
    }
  } catch (const std::exception &ex) {
    return cookFail(error, std::string("manifest parse error: ") + ex.what()), false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// 整包 cook
// ---------------------------------------------------------------------------

bool svtCookBistroBin(const std::filesystem::path &binPath, const std::filesystem::path &outDir,
                      const SvtCookParams &params, SvtBistroCookReport &report,
                      std::string *error) {
  report = SvtBistroCookReport{};
  SvtBistroInfo info;
  if (!svtReadBistroInfo(binPath, info, error))
    return false;

  std::error_code ec;
  std::filesystem::create_directories(outDir, ec);
  if (ec)
    return cookFail(error, "cannot create output dir: " + ec.message()), false;

  for (size_t i = 0; i < info.textures.size(); ++i) {
    const SvtBistroTextureInfo &tex = info.textures[i];
    SvtFormat fmt;
    if (!svtFormatFromMtlPixelFormat(tex.pixelFormat, fmt)) {
      report.skipped.emplace_back(tex.path, "unsupported pixel format " +
                                                 std::to_string(tex.pixelFormat));
      continue;
    }
    SvtSourceTexture src;
    std::string loadError;
    if (!svtLoadBistroTexture(binPath, info, i, src, &loadError)) {
      report.skipped.emplace_back(tex.path, loadError);
      continue;
    }
    SvtCookStats stats;
    char name[64];
    snprintf(name, sizeof(name), "%06zu_%08x.svt", i, tex.pathHash);
    const std::filesystem::path outPath = outDir / name;
    if (!svtCookTexture(src, params, outPath, &stats, &loadError)) {
      report.skipped.emplace_back(tex.path, loadError);
      continue;
    }
    report.totalFileBytes += stats.fileBytes;
    SvtManifestEntry e;
    e.sourcePath = tex.path;
    e.pathHash = tex.pathHash;
    e.contentHash = svtComputeContentHash(src);
    e.svtFile = name;
    e.width = src.width;
    e.height = src.height;
    e.mipCount = src.mipCount;
    e.format = src.format;
    e.tailFirstMip = svtComputeTailFirstMip(src.width, src.height, src.mipCount, params.tileSize);
    e.tileCount = stats.tileCount;
    report.cooked.push_back(e);
  }
  return true;
}

} // namespace svt
