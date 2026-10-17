#pragma once
// Streaming Virtual Texture (SVT) 资产文件格式、读取器与地址数学。
// 设计依据：docs/superpowers/specs/2026-10-16-streaming-virtual-texture-design.md §6.1、§7.1。
//
// 本模块只负责 CPU 侧文件解析与校验，不创建任何 GPU 资源（见设计 §5 模块所有权表）。
// 所有磁盘字段按版本逐项小端读写；offset/长度使用 uint64，读取前必须做有界整数校验。

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace svt {

// ---------------------------------------------------------------------------
// 格式常量（设计 §6.1）
// ---------------------------------------------------------------------------

constexpr uint32_t kSvtMagic = 0x31545653u; // "SVT1" 小端
constexpr uint32_t kSvtVersion = 1;

// header.flags：bit0 = 写入方为小端。读取方只接受小端文件。
constexpr uint32_t kSvtFlagLittleEndian = 0x1u;

// addressMode
constexpr uint32_t kSvtAddressRepeat = 0; // 首版唯一支持的寻址模式

// tile codec
constexpr uint32_t kSvtCodecRaw = 0;   // 直接读取 GPU-native blocks
constexpr uint32_t kSvtCodecLzfse = 1; // 单 tile LZFSE

// 上限（注册时检查；设计 §8.3 首版单边最大 16384）
constexpr uint32_t kSvtMaxTextureDim = 16384;
constexpr uint32_t kSvtMaxMipCount = 32;
constexpr uint32_t kSvtMaxTileSize = 1024;
constexpr uint32_t kSvtMaxBorder = 64;
constexpr uint32_t kSvtMaxTileEntries = 4u * 1024u * 1024u; // 单资产 tile 数 sanity cap
constexpr uint64_t kSvtMaxFileSize = 16ull << 30;           // 16 GiB

// 磁盘 header 的固定大小（字节）。headerSize 字段允许未来扩展。
constexpr uint32_t kSvtHeaderSize = 120;

enum class SvtFormat : uint32_t {
  RGBA8 = 0,
  RGBA8_SRGB = 1,
  BC1 = 2,
  BC1_SRGB = 3,
  BC3 = 4,
  BC3_SRGB = 5,
  BC4 = 6,
  BC5 = 7,
  BC5_SNORM = 8,
  BC7 = 9,
  BC7_SRGB = 10,
};

// 返回 false 表示未知格式。
bool svtFormatIsValid(uint32_t format);
// 压缩格式（4x4 block）；RGBA8 返回 false。
bool svtFormatIsBlockCompressed(SvtFormat format);
// 压缩格式的 block 边长（4）；RGBA8 返回 1（按 texel）。
uint32_t svtFormatBlockSize(SvtFormat format);
// 压缩格式每 block 字节数（8/16）；RGBA8 返回 4（每 texel 字节）。
uint32_t svtFormatBytesPerUnit(SvtFormat format);
bool svtFormatIsSrgb(SvtFormat format);

// ---------------------------------------------------------------------------
// 纯地址数学（shader/CPU 共用语义，设计 §7.1）
// ---------------------------------------------------------------------------

// mip m 的某维 texel 尺寸：max(1, dim >> m)。要求 mip < 32。
uint32_t svtMipDim(uint32_t dim, uint32_t mip);
// 某轴的 tile 数：ceil(dim / tile)。要求 dim、tile 非零。
uint32_t svtTilesPerAxis(uint32_t dim, uint32_t tileSize);
// 完整 mip 链层数（到 1x1）。
uint32_t svtFullMipCount(uint32_t width, uint32_t height);
// 第一个宽与高均 <= tileSize 的 mip。小纹理返回 0（整张作为 tail）。
uint32_t svtComputeTailFirstMip(uint32_t width, uint32_t height, uint32_t mipCount,
                                uint32_t tileSize);
// 可流送页的页表项总数（mips [0, tailFirstMip) 的 tilesX*tilesY 之和）。
uint64_t svtPageTableEntryCount(uint32_t width, uint32_t height, uint32_t tailFirstMip,
                                uint32_t tileSize);
// CPU 侧 Repeat 包装（shader 内做 frac；这里给 cooker/测试用）。
uint32_t svtWrapRepeat(int64_t coord, uint32_t dim);
// CRC32（IEEE，poly 0xEDB88320，init 0xFFFFFFFF，结果取反）。
uint32_t svtCrc32(const uint8_t *data, size_t size);
// FNV-1a 64；用于源内容 hash。
uint64_t svtFnv1a64(const uint8_t *data, size_t size, uint64_t h = 14695981039346656037ull);

// 某 mip 的解码后字节数（整张 mip，BC 为 block 网格）。
uint64_t svtMipByteSize(SvtFormat format, uint32_t mipWidth, uint32_t mipHeight);
// 一个含边框 tile 的解码后字节数（padded = tileSize + 2*border）。
uint64_t svtTilePayloadBytes(SvtFormat format, uint32_t tileSize, uint32_t border);

// ---------------------------------------------------------------------------
// SvtAsset：打开/校验 .svt 文件并按 range 读取 tile/tail
// ---------------------------------------------------------------------------

class SvtAsset {
public:
  struct MipInfo {
    uint32_t texelWidth = 0;
    uint32_t texelHeight = 0;
    uint32_t tilesX = 0;
    uint32_t tilesY = 0;
    uint32_t firstTileIndex = 0;
  };
  struct Entry {
    uint64_t fileOffset = 0;
    uint32_t storedSize = 0;
    uint32_t decodedSize = 0;
    uint32_t codec = 0;
    uint32_t crc32 = 0;
  };

  SvtAsset() = default;
  ~SvtAsset();
  SvtAsset(const SvtAsset &) = delete;
  SvtAsset &operator=(const SvtAsset &) = delete;
  // 拥有 FILE*，且析构函数声明抑制了隐式移动，需要显式提供。
  SvtAsset(SvtAsset &&rhs) noexcept { moveFrom(rhs); }
  SvtAsset &operator=(SvtAsset &&rhs) noexcept {
    if (this != &rhs) {
      close();
      moveFrom(rhs);
    }
    return *this;
  }

  // 解析并完整校验文件。失败时返回 false 并写 error（可为 nullptr）。
  bool open(const std::filesystem::path &path, std::string *error = nullptr);
  void close();
  // 只释放文件句柄，保留已解析的目录元数据（mip/tile/tail 目录与几何）。
  // 供"先统计后读取"的两阶段使用（如全驻留上传）：统计完先释放句柄，
  // 读取阶段用 reopen() 短暂重开。避免数百资产长期占用 stdio 句柄
  // （MSVC CRT 默认 ~512 个 FILE* 上限）。
  void closeFile();
  // 重新打开底层文件（目录元数据必须已由 open() 填充；保留原路径）。
  bool reopen(std::string *error = nullptr);
  bool isOpen() const { return file_ != nullptr; }

  // ---- 元数据
  uint32_t width() const { return width_; }
  uint32_t height() const { return height_; }
  uint32_t mipCount() const { return mipCount_; }
  uint32_t tileSize() const { return tileSize_; }           // T：内部 texel
  uint32_t border() const { return border_; }               // B：每侧 texel
  uint32_t paddedTileSize() const { return tileSize_ + 2 * border_; } // P = T + 2B
  SvtFormat format() const { return format_; }
  uint32_t tailFirstMip() const { return tailFirstMip_; }
  uint32_t tailMipCount() const { return mipCount_ - tailFirstMip_; }
  uint64_t sourceContentHash() const { return sourceContentHash_; }
  uint64_t fileLength() const { return fileLength_; }
  const std::vector<MipInfo> &mipDirectory() const { return mips_; }
  const std::vector<Entry> &tileDirectory() const { return tiles_; }
  const std::vector<Entry> &tailDirectory() const { return tail_; }

  // ---- 地址数学（基于已打开资产的元数据）
  uint32_t mipWidth(uint32_t mip) const;
  uint32_t mipHeight(uint32_t mip) const;
  uint32_t tilesX(uint32_t mip) const;
  uint32_t tilesY(uint32_t mip) const;
  bool mipIsTiled(uint32_t mip) const; // mip < tailFirstMip
  uint32_t pageTableEntryCount() const;
  // 页表项索引（不含系统层 pageBase；设计 §7.1）。
  uint32_t tileIndex(uint32_t mip, uint32_t x, uint32_t y) const;
  // 一个 tile / 一个 tail mip 的解码后字节数。
  uint32_t tilePayloadBytes() const;
  uint32_t tailMipPayloadBytes(uint32_t mip) const;

  // ---- range 读取（解码 + CRC 校验）
  // mip/x/y 必须位于 [0, tailFirstMip) 的 tile 网格内。
  bool readTile(uint32_t mip, uint32_t x, uint32_t y,
                std::vector<uint8_t> &outPayload, std::string *error = nullptr) const;
  // mip 为绝对 mip 索引，位于 [tailFirstMip, mipCount)。
  bool readTailMip(uint32_t mip, std::vector<uint8_t> &outPayload,
                   std::string *error = nullptr) const;

private:
  bool readEntryPayload(const Entry &e, uint32_t expectedDecodedSize,
                        std::vector<uint8_t> &outPayload, std::string *error) const;
  void moveFrom(SvtAsset &rhs) noexcept;
  // open() 的公共实现（reopen 复用；不重置已有元数据）。
  bool openFileHandle(std::string *error);

  FILE *file_ = nullptr;
  std::filesystem::path openPath_; // reopen() 用
  uint64_t fileLength_ = 0;
  uint32_t width_ = 0;
  uint32_t height_ = 0;
  uint32_t mipCount_ = 0;
  uint32_t tileSize_ = 0;
  uint32_t border_ = 0;
  SvtFormat format_ = SvtFormat::RGBA8;
  uint32_t tailFirstMip_ = 0;
  uint64_t sourceContentHash_ = 0;
  std::vector<MipInfo> mips_;
  std::vector<Entry> tiles_;
  std::vector<Entry> tail_;
};

} // namespace svt
