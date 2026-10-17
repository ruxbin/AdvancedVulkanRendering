// SVT 资产文件读取与校验实现。
// 设计依据：docs/superpowers/specs/2026-10-16-streaming-virtual-texture-design.md §6.1、§7.1。

#include "SvtAsset.h"

#include "ThirdParty/lzfse.h"

#include <algorithm>
#include <cstring>

namespace svt {

// ---------------------------------------------------------------------------
// 格式辅助
// ---------------------------------------------------------------------------

bool svtFormatIsValid(uint32_t format) {
  switch (static_cast<SvtFormat>(format)) {
  case SvtFormat::RGBA8:
  case SvtFormat::RGBA8_SRGB:
  case SvtFormat::BC1:
  case SvtFormat::BC1_SRGB:
  case SvtFormat::BC3:
  case SvtFormat::BC3_SRGB:
  case SvtFormat::BC4:
  case SvtFormat::BC5:
  case SvtFormat::BC5_SNORM:
  case SvtFormat::BC7:
  case SvtFormat::BC7_SRGB:
    return true;
  }
  return false;
}

bool svtFormatIsBlockCompressed(SvtFormat format) {
  return format != SvtFormat::RGBA8 && format != SvtFormat::RGBA8_SRGB;
}

uint32_t svtFormatBlockSize(SvtFormat format) { return svtFormatIsBlockCompressed(format) ? 4 : 1; }

uint32_t svtFormatBytesPerUnit(SvtFormat format) {
  switch (format) {
  case SvtFormat::RGBA8:
  case SvtFormat::RGBA8_SRGB:
    return 4; // 每 texel 字节
  case SvtFormat::BC1:
  case SvtFormat::BC1_SRGB:
  case SvtFormat::BC4:
    return 8; // 每 block 字节
  case SvtFormat::BC3:
  case SvtFormat::BC3_SRGB:
  case SvtFormat::BC5:
  case SvtFormat::BC5_SNORM:
  case SvtFormat::BC7:
  case SvtFormat::BC7_SRGB:
    return 16;
  }
  return 0;
}

bool svtFormatIsSrgb(SvtFormat format) {
  switch (format) {
  case SvtFormat::RGBA8_SRGB:
  case SvtFormat::BC1_SRGB:
  case SvtFormat::BC3_SRGB:
  case SvtFormat::BC7_SRGB:
    return true;
  default:
    return false;
  }
}

// ---------------------------------------------------------------------------
// 地址数学
// ---------------------------------------------------------------------------

uint32_t svtMipDim(uint32_t dim, uint32_t mip) {
  return (mip >= 32 || (dim >> mip) == 0) ? 1 : (dim >> mip);
}

uint32_t svtTilesPerAxis(uint32_t dim, uint32_t tileSize) {
  return (dim + tileSize - 1) / tileSize;
}

uint32_t svtFullMipCount(uint32_t width, uint32_t height) {
  uint32_t maxDim = std::max(width, height);
  uint32_t count = 1;
  while (maxDim > 1) {
    maxDim >>= 1;
    ++count;
  }
  return count;
}

uint32_t svtComputeTailFirstMip(uint32_t width, uint32_t height, uint32_t mipCount,
                                uint32_t tileSize) {
  for (uint32_t m = 0; m < mipCount; ++m) {
    if (svtMipDim(width, m) <= tileSize && svtMipDim(height, m) <= tileSize)
      return m;
  }
  return mipCount; // 完整链最低是 1x1，正常不会到达
}

uint64_t svtPageTableEntryCount(uint32_t width, uint32_t height, uint32_t tailFirstMip,
                                uint32_t tileSize) {
  uint64_t total = 0;
  for (uint32_t m = 0; m < tailFirstMip; ++m) {
    uint64_t tx = svtTilesPerAxis(svtMipDim(width, m), tileSize);
    uint64_t ty = svtTilesPerAxis(svtMipDim(height, m), tileSize);
    total += tx * ty;
  }
  return total;
}

uint32_t svtWrapRepeat(int64_t coord, uint32_t dim) {
  if (dim == 0)
    return 0;
  int64_t m = coord % static_cast<int64_t>(dim);
  if (m < 0)
    m += dim;
  return static_cast<uint32_t>(m);
}

uint64_t svtMipByteSize(SvtFormat format, uint32_t mipWidth, uint32_t mipHeight) {
  if (!svtFormatIsBlockCompressed(format))
    return static_cast<uint64_t>(mipWidth) * mipHeight * 4;
  const uint32_t blockSize = svtFormatBlockSize(format);
  const uint64_t bw = std::max<uint64_t>(1, (static_cast<uint64_t>(mipWidth) + blockSize - 1) / blockSize);
  const uint64_t bh = std::max<uint64_t>(1, (static_cast<uint64_t>(mipHeight) + blockSize - 1) / blockSize);
  return bw * bh * svtFormatBytesPerUnit(format);
}

uint64_t svtTilePayloadBytes(SvtFormat format, uint32_t tileSize, uint32_t border) {
  const uint64_t padded = static_cast<uint64_t>(tileSize) + 2 * border;
  if (!svtFormatIsBlockCompressed(format))
    return padded * padded * svtFormatBytesPerUnit(format);
  const uint32_t blockSize = svtFormatBlockSize(format);
  const uint64_t blocksPerSide = padded / blockSize; // caller 保证对齐
  return blocksPerSide * blocksPerSide * svtFormatBytesPerUnit(format);
}

// ---------------------------------------------------------------------------
// CRC32 / FNV
// ---------------------------------------------------------------------------

namespace {

struct Crc32Table {
  uint32_t v[256];
  Crc32Table() {
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k)
        c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
      v[i] = c;
    }
  }
};
const Crc32Table kCrc32Table;

} // namespace

uint32_t svtCrc32(const uint8_t *data, size_t size) {
  uint32_t crc = 0xFFFFFFFFu;
  for (size_t i = 0; i < size; ++i)
    crc = kCrc32Table.v[(crc ^ data[i]) & 0xFFu] ^ (crc >> 8);
  return crc ^ 0xFFFFFFFFu;
}

uint64_t svtFnv1a64(const uint8_t *data, size_t size, uint64_t h) {
  for (size_t i = 0; i < size; ++i) {
    h ^= data[i];
    h *= 1099511628211ull;
  }
  return h;
}

// ---------------------------------------------------------------------------
// 磁盘 IO：字段按版本逐项小端读写（不序列化带 padding 的 struct，设计 §6.1）
// ---------------------------------------------------------------------------

namespace {

// 大文件（>2 GiB）安全的定位/取位。Windows 的 fseek/ftell 参数是 32 位 long。
#ifdef _WIN32
inline int seek64(FILE *f, long long off, int whence) { return _fseeki64(f, off, whence); }
inline long long tell64(FILE *f) { return _ftelli64(f); }
#else
inline int seek64(FILE *f, long long off, int whence) { return fseeko(f, off, whence); }
inline long long tell64(FILE *f) { return ftello(f); }
#endif

bool readExact(FILE *f, void *dst, size_t n) { return fread(dst, 1, n, f) == n; }

bool readU32(FILE *f, uint32_t &v) { return readExact(f, &v, sizeof(v)); }
bool readU64(FILE *f, uint64_t &v) { return readExact(f, &v, sizeof(v)); }

// 有界检查：offset + size <= fileLength（避免溢出）。
bool rangeInFile(uint64_t offset, uint64_t size, uint64_t fileLength) {
  return offset <= fileLength && size <= fileLength - offset;
}

std::string fail(std::string *error, const std::string &msg) {
  if (error)
    *error = msg;
  return msg;
}

} // namespace

// ---------------------------------------------------------------------------
// SvtAsset
// ---------------------------------------------------------------------------

SvtAsset::~SvtAsset() { close(); }

void SvtAsset::moveFrom(SvtAsset &rhs) noexcept {
  file_ = rhs.file_;
  openPath_ = std::move(rhs.openPath_);
  fileLength_ = rhs.fileLength_;
  width_ = rhs.width_;
  height_ = rhs.height_;
  mipCount_ = rhs.mipCount_;
  tileSize_ = rhs.tileSize_;
  border_ = rhs.border_;
  format_ = rhs.format_;
  tailFirstMip_ = rhs.tailFirstMip_;
  sourceContentHash_ = rhs.sourceContentHash_;
  mips_ = std::move(rhs.mips_);
  tiles_ = std::move(rhs.tiles_);
  tail_ = std::move(rhs.tail_);
  rhs.file_ = nullptr;
  rhs.openPath_.clear();
  rhs.fileLength_ = 0;
  rhs.mips_.clear();
  rhs.tiles_.clear();
  rhs.tail_.clear();
}

void SvtAsset::close() {
  closeFile();
  openPath_.clear();
  mips_.clear();
  tiles_.clear();
  tail_.clear();
}

bool SvtAsset::openFileHandle(std::string *error) {
#ifdef _WIN32
  FILE *f = nullptr;
  if (_wfopen_s(&f, openPath_.wstring().c_str(), L"rb") != 0 || !f)
#else
  FILE *f = fopen(openPath_.string().c_str(), "rb");
  if (!f)
#endif
    return fail(error, "cannot open file: " + openPath_.string()), false;
  file_ = f;
  return true;
}

void SvtAsset::closeFile() {
  if (file_) {
    fclose(file_);
    file_ = nullptr;
  }
}

bool SvtAsset::reopen(std::string *error) {
  if (file_)
    return true; // 已打开
  if (openPath_.empty())
    return fail(error, "reopen without prior open"), false;
  return openFileHandle(error);
}

bool SvtAsset::open(const std::filesystem::path &path, std::string *error) {
  close();
  openPath_ = path;

  FILE *f = nullptr;
#ifdef _WIN32
  if (_wfopen_s(&f, path.wstring().c_str(), L"rb") != 0 || !f)
#else
  f = fopen(path.string().c_str(), "rb");
  if (!f)
#endif
    return fail(error, "cannot open file: " + path.string()), false;

  // 取文件长度做所有 offset/size 的边界。
  if (seek64(f, 0, SEEK_END) != 0) {
    fclose(f);
    return fail(error, "seek failed"), false;
  }
  const long long len = tell64(f);
  if (len < 0) {
    fclose(f);
    return fail(error, "tell failed"), false;
  }
  if (static_cast<uint64_t>(len) > kSvtMaxFileSize) {
    fclose(f);
    return fail(error, "file exceeds size cap"), false;
  }
  fileLength_ = static_cast<uint64_t>(len);
  if (seek64(f, 0, SEEK_SET) != 0) {
    fclose(f);
    return fail(error, "seek failed"), false;
  }

  // ---- header（逐字段读取，kSvtHeaderSize 字节固定布局）
  uint32_t magic = 0, version = 0, headerSize = 0, flags = 0;
  uint32_t width = 0, height = 0, mipCount = 0;
  uint32_t tileSize = 0, border = 0;
  uint32_t format = 0, colorSpace = 0, addressMode = 0, tailFirstMip = 0;
  uint32_t mipDirCount = 0, tileDirCount = 0, tailDirCount = 0;
  uint64_t mipDirOffset = 0, tileDirOffset = 0, tailDirOffset = 0;
  uint64_t payloadEnd = 0, contentHash = 0;
  uint32_t reserved[4] = {};

  const bool hdrOk = readU32(f, magic) && readU32(f, version) && readU32(f, headerSize) &&
                     readU32(f, flags) && readU32(f, width) && readU32(f, height) &&
                     readU32(f, mipCount) && readU32(f, tileSize) && readU32(f, border) &&
                     readU32(f, format) && readU32(f, colorSpace) && readU32(f, addressMode) &&
                     readU32(f, tailFirstMip) && readU32(f, mipDirCount) &&
                     readU32(f, tileDirCount) && readU32(f, tailDirCount) &&
                     readU64(f, mipDirOffset) && readU64(f, tileDirOffset) &&
                     readU64(f, tailDirOffset) && readU64(f, payloadEnd) &&
                     readU64(f, contentHash) && readExact(f, reserved, sizeof(reserved));
  if (!hdrOk) {
    fclose(f);
    return fail(error, "truncated header"), false;
  }

  if (magic != kSvtMagic) {
    fclose(f);
    return fail(error, "bad magic"), false;
  }
  if (version != kSvtVersion) {
    fclose(f);
    return fail(error, "unsupported version"), false;
  }
  if (headerSize < kSvtHeaderSize || headerSize > fileLength_) {
    fclose(f);
    return fail(error, "bad headerSize"), false;
  }
  if ((flags & kSvtFlagLittleEndian) == 0) {
    fclose(f);
    return fail(error, "file not little-endian"), false;
  }
  if (addressMode != kSvtAddressRepeat) {
    fclose(f);
    return fail(error, "unsupported address mode"), false;
  }
  if (!svtFormatIsValid(format)) {
    fclose(f);
    return fail(error, "unknown format"), false;
  }
  if (colorSpace != (svtFormatIsSrgb(static_cast<SvtFormat>(format)) ? 1u : 0u)) {
    fclose(f);
    return fail(error, "colorSpace/format mismatch"), false;
  }
  if (width == 0 || height == 0 || width > kSvtMaxTextureDim || height > kSvtMaxTextureDim) {
    fclose(f);
    return fail(error, "bad dimensions"), false;
  }
  if ((width & (width - 1)) != 0 || (height & (height - 1)) != 0) {
    fclose(f);
    return fail(error, "dimensions must be power of two"), false;
  }
  if (tileSize == 0 || tileSize > kSvtMaxTileSize || border > kSvtMaxBorder ||
      border * 2 >= tileSize) {
    fclose(f);
    return fail(error, "bad tileSize/border"), false;
  }
  const SvtFormat fmt = static_cast<SvtFormat>(format);
  if (svtFormatIsBlockCompressed(fmt)) {
    // BC：T、B 与 padded 尺寸都必须按 4 texel block 对齐（设计 §6.2）。
    if (tileSize % 4 != 0 || border % 4 != 0 || (tileSize + 2 * border) % 4 != 0) {
      fclose(f);
      return fail(error, "tile/border not block aligned"), false;
    }
  }
  const uint32_t fullChain = svtFullMipCount(width, height);
  if (mipCount != fullChain || mipCount > kSvtMaxMipCount) {
    fclose(f);
    return fail(error, "mipCount does not match full chain"), false;
  }
  if (tailFirstMip != svtComputeTailFirstMip(width, height, mipCount, tileSize)) {
    fclose(f);
    return fail(error, "tailFirstMip inconsistent with dimensions"), false;
  }
  if (tailFirstMip >= mipCount) {
    fclose(f);
    return fail(error, "tail must contain at least the last mip"), false;
  }
  if (mipDirCount != mipCount) {
    fclose(f);
    return fail(error, "mip directory count mismatch"), false;
  }
  const uint64_t expectedTiles =
      svtPageTableEntryCount(width, height, tailFirstMip, tileSize);
  if (expectedTiles > kSvtMaxTileEntries) {
    fclose(f);
    return fail(error, "tile count exceeds cap"), false;
  }
  if (tileDirCount != expectedTiles) {
    fclose(f);
    return fail(error, "tile directory count mismatch"), false;
  }
  if (tailDirCount != mipCount - tailFirstMip) {
    fclose(f);
    return fail(error, "tail directory count mismatch"), false;
  }
  // 目录读取前的分配上界：count*entrySize 不能超过文件长度。
  const auto dirFits = [&](uint64_t dirOffset, uint32_t count, size_t entrySize) {
    return rangeInFile(dirOffset, static_cast<uint64_t>(count) * entrySize, fileLength_);
  };
  if (!dirFits(mipDirOffset, mipDirCount, 20) || !dirFits(tileDirOffset, tileDirCount, 24) ||
      !dirFits(tailDirOffset, tailDirCount, 24) || payloadEnd != fileLength_) {
    fclose(f);
    return fail(error, "directory offset/size out of range"), false;
  }

  // ---- mip directory（按 header 中的 offset 定位，而非紧跟 header）
  if (seek64(f, static_cast<long long>(mipDirOffset), SEEK_SET) != 0) {
    fclose(f);
    return fail(error, "seek to mip directory failed"), false;
  }
  mips_.resize(mipCount);
  uint32_t runningFirstTile = 0;
  for (uint32_t m = 0; m < mipCount; ++m) {
    MipInfo &mi = mips_[m];
    if (!readU32(f, mi.texelWidth) || !readU32(f, mi.texelHeight) || !readU32(f, mi.tilesX) ||
        !readU32(f, mi.tilesY) || !readU32(f, mi.firstTileIndex)) {
      fclose(f);
      return fail(error, "truncated mip directory"), false;
    }
    const uint32_t w = svtMipDim(width, m), h = svtMipDim(height, m);
    const bool tiled = m < tailFirstMip;
    if (mi.texelWidth != w || mi.texelHeight != h) {
      fclose(f);
      return fail(error, "mip dims mismatch"), false;
    }
    if (tiled) {
      const uint32_t tx = svtTilesPerAxis(w, tileSize), ty = svtTilesPerAxis(h, tileSize);
      if (mi.tilesX != tx || mi.tilesY != ty || mi.firstTileIndex != runningFirstTile) {
        fclose(f);
        return fail(error, "mip tile grid mismatch"), false;
      }
      runningFirstTile += tx * ty;
    } else if (mi.tilesX != 0 || mi.tilesY != 0 || mi.firstTileIndex != 0) {
      fclose(f);
      return fail(error, "tail mip must have zero tile grid"), false;
    }
  }
  if (runningFirstTile != tileDirCount) {
    fclose(f);
    return fail(error, "tile grid does not cover directory"), false;
  }

  // ---- tile directory（同样按 offset 定位）
  if (seek64(f, static_cast<long long>(tileDirOffset), SEEK_SET) != 0) {
    fclose(f);
    return fail(error, "seek to tile directory failed"), false;
  }
  const uint64_t tilePayload = svtTilePayloadBytes(fmt, tileSize, border);
  if (tilePayload > 0xFFFFFFFFull) {
    fclose(f);
    return fail(error, "tile payload too large"), false;
  }
  tiles_.resize(tileDirCount);
  for (uint32_t i = 0; i < tileDirCount; ++i) {
    Entry &e = tiles_[i];
    if (!readU64(f, e.fileOffset) || !readU32(f, e.storedSize) || !readU32(f, e.decodedSize) ||
        !readU32(f, e.codec) || !readU32(f, e.crc32)) {
      fclose(f);
      return fail(error, "truncated tile directory"), false;
    }
    if (e.codec != kSvtCodecRaw && e.codec != kSvtCodecLzfse) {
      fclose(f);
      return fail(error, "unknown tile codec"), false;
    }
    if (e.decodedSize != tilePayload) {
      fclose(f);
      return fail(error, "tile decodedSize mismatch"), false;
    }
    if (e.codec == kSvtCodecRaw && e.storedSize != e.decodedSize) {
      fclose(f);
      return fail(error, "raw tile storedSize mismatch"), false;
    }
    if (!rangeInFile(e.fileOffset, e.storedSize, fileLength_)) {
      fclose(f);
      return fail(error, "tile payload out of range"), false;
    }
  }

  // ---- tail directory
  if (seek64(f, static_cast<long long>(tailDirOffset), SEEK_SET) != 0) {
    fclose(f);
    return fail(error, "seek to tail directory failed"), false;
  }
  tail_.resize(tailDirCount);
  for (uint32_t i = 0; i < tailDirCount; ++i) {
    Entry &e = tail_[i];
    if (!readU64(f, e.fileOffset) || !readU32(f, e.storedSize) || !readU32(f, e.decodedSize) ||
        !readU32(f, e.codec) || !readU32(f, e.crc32)) {
      fclose(f);
      return fail(error, "truncated tail directory"), false;
    }
    const uint32_t mip = tailFirstMip + i;
    const uint64_t expected = svtMipByteSize(fmt, svtMipDim(width, mip), svtMipDim(height, mip));
    if (e.codec != kSvtCodecRaw && e.codec != kSvtCodecLzfse) {
      fclose(f);
      return fail(error, "unknown tail codec"), false;
    }
    if (e.decodedSize != expected) {
      fclose(f);
      return fail(error, "tail decodedSize mismatch"), false;
    }
    if (e.codec == kSvtCodecRaw && e.storedSize != e.decodedSize) {
      fclose(f);
      return fail(error, "raw tail storedSize mismatch"), false;
    }
    if (!rangeInFile(e.fileOffset, e.storedSize, fileLength_)) {
      fclose(f);
      return fail(error, "tail payload out of range"), false;
    }
  }

  file_ = f;
  width_ = width;
  height_ = height;
  mipCount_ = mipCount;
  tileSize_ = tileSize;
  border_ = border;
  format_ = fmt;
  tailFirstMip_ = tailFirstMip;
  sourceContentHash_ = contentHash;
  return true;
}

uint32_t SvtAsset::mipWidth(uint32_t mip) const { return svtMipDim(width_, mip); }
uint32_t SvtAsset::mipHeight(uint32_t mip) const { return svtMipDim(height_, mip); }
uint32_t SvtAsset::tilesX(uint32_t mip) const { return mips_.at(mip).tilesX; }
uint32_t SvtAsset::tilesY(uint32_t mip) const { return mips_.at(mip).tilesY; }
bool SvtAsset::mipIsTiled(uint32_t mip) const { return mip < tailFirstMip_; }
uint32_t SvtAsset::pageTableEntryCount() const {
  return static_cast<uint32_t>(tiles_.size());
}
uint32_t SvtAsset::tileIndex(uint32_t mip, uint32_t x, uint32_t y) const {
  const MipInfo &mi = mips_.at(mip);
  return mi.firstTileIndex + y * mi.tilesX + x;
}
uint32_t SvtAsset::tilePayloadBytes() const {
  return static_cast<uint32_t>(svtTilePayloadBytes(format_, tileSize_, border_));
}
uint32_t SvtAsset::tailMipPayloadBytes(uint32_t mip) const {
  return static_cast<uint32_t>(svtMipByteSize(format_, mipWidth(mip), mipHeight(mip)));
}

bool SvtAsset::readEntryPayload(const Entry &e, uint32_t expectedDecodedSize,
                                std::vector<uint8_t> &outPayload, std::string *error) const {
  outPayload.clear();
  if (e.decodedSize != expectedDecodedSize)
    return fail(error, "entry decodedSize unexpected"), false;  std::vector<uint8_t> stored(e.storedSize);
  if (seek64(file_, static_cast<long long>(e.fileOffset), SEEK_SET) != 0 ||
      !readExact(file_, stored.data(), stored.size())) {
    return fail(error, "payload read failed"), false;
  }

  outPayload.resize(e.decodedSize);
  if (e.codec == kSvtCodecRaw) {
    memcpy(outPayload.data(), stored.data(), stored.size());
  } else {
    const size_t decoded =
        lzfse_decode_buffer(outPayload.data(), outPayload.size(), stored.data(), stored.size(), nullptr);
    if (decoded != outPayload.size()) {
      outPayload.clear();
      return fail(error, "lzfse decode size mismatch"), false;
    }
  }
  if (svtCrc32(outPayload.data(), outPayload.size()) != e.crc32) {
    outPayload.clear();
    return fail(error, "payload CRC mismatch"), false;
  }
  return true;
}

bool SvtAsset::readTile(uint32_t mip, uint32_t x, uint32_t y,
                        std::vector<uint8_t> &outPayload, std::string *error) const {
  if (!file_)
    return fail(error, "asset not open"), false;
  if (mip >= tailFirstMip_ || x >= tilesX(mip) || y >= tilesY(mip))
    return fail(error, "tile coordinate out of range"), false;
  return readEntryPayload(tiles_[tileIndex(mip, x, y)], tilePayloadBytes(), outPayload, error);
}

bool SvtAsset::readTailMip(uint32_t mip, std::vector<uint8_t> &outPayload,
                           std::string *error) const {
  if (!file_)
    return fail(error, "asset not open"), false;
  if (mip < tailFirstMip_ || mip >= mipCount_)
    return fail(error, "tail mip out of range"), false;
  return readEntryPayload(tail_[mip - tailFirstMip_], tailMipPayloadBytes(mip), outPayload, error);
}

} // namespace svt
