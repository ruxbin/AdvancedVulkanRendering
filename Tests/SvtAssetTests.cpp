// SVT M1 CPU 测试：地址数学、文件格式校验、cooker 边框一致性、bistro 集成、manifest。
// 设计依据：docs/superpowers/specs/2026-10-16-streaming-virtual-texture-design.md §13.1。
// 覆盖：正方形/长方形、tail 边界、Repeat UV、最大合法值、非法坐标、
// 截断 header、错误版本、offset 溢出、错误 decodedSize/CRC、tile count 不符、
// block 对齐失败、136² 边框与整张源 mip 的 Repeat 采样一致、页角四邻居、
// 短边小于 128、BC block-copy 保留字节、tail mip 索引正确。

#include "SvtAsset.h"
#include "SvtCooker.h"

#include "ThirdParty/lzfse.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace {

int failures = 0;

void expectTrue(bool cond, const char *message) {
  if (!cond) {
    std::fprintf(stderr, "FAIL %s\n", message);
    ++failures;
  }
}

void expectEqU64(uint64_t actual, uint64_t expected, const char *message) {
  if (actual != expected) {
    std::fprintf(stderr, "FAIL %s (actual=%llu expected=%llu)\n", message,
                 static_cast<unsigned long long>(actual),
                 static_cast<unsigned long long>(expected));
    ++failures;
  }
}

// open 失败时把具体原因打到 stderr，便于定位。
bool expectOpen(svt::SvtAsset &asset, const std::filesystem::path &path, const char *what) {
  std::string error;
  const bool ok = asset.open(path, &error);
  if (!ok) {
    std::fprintf(stderr, "FAIL %s: %s\n", what, error.c_str());
    ++failures;
  }
  return ok;
}

std::vector<uint8_t> readFileBytes(const std::filesystem::path &p) {
  FILE *f =
#ifdef _WIN32
      nullptr;
  if (_wfopen_s(&f, p.wstring().c_str(), L"rb") != 0 || !f)
    return {};
#else
      fopen(p.string().c_str(), "rb");
  if (!f)
    return {};
#endif
  std::vector<uint8_t> bytes;
  uint8_t buf[65536];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
    bytes.insert(bytes.end(), buf, buf + n);
  fclose(f);
  return bytes;
}

void writeFileBytes(const std::filesystem::path &p, const std::vector<uint8_t> &bytes) {
#ifdef _WIN32
  FILE *f = nullptr;
  if (_wfopen_s(&f, p.wstring().c_str(), L"wb") != 0 || !f)
    return;
#else
  FILE *f = fopen(p.string().c_str(), "wb");
  if (!f)
    return;
#endif
  fwrite(bytes.data(), 1, bytes.size(), f);
  fclose(f);
}

uint32_t leU32(const std::vector<uint8_t> &b, size_t off) {
  return b[off] | (b[off + 1] << 8) | (uint32_t(b[off + 2]) << 16) | (uint32_t(b[off + 3]) << 24);
}
uint64_t leU64(const std::vector<uint8_t> &b, size_t off) {
  uint64_t v = 0;
  for (int i = 7; i >= 0; --i)
    v = (v << 8) | b[off + i];
  return v;
}
void patchU32(std::vector<uint8_t> &b, size_t off, uint32_t v) {
  for (int i = 0; i < 4; ++i)
    b[off + i] = static_cast<uint8_t>(v >> (8 * i));
}
void patchU64(std::vector<uint8_t> &b, size_t off, uint64_t v) {
  for (int i = 0; i < 8; ++i)
    b[off + i] = static_cast<uint8_t>(v >> (8 * i));
}

// 头部字段偏移（与 SvtAsset.cpp 的读写顺序一一对应）。
constexpr size_t kOffMagic = 0, kOffVersion = 4, kOffHeaderSize = 8, kOffFlags = 12;
constexpr size_t kOffWidth = 16, kOffHeight = 20, kOffMipCount = 24, kOffTileSize = 28;
constexpr size_t kOffBorder = 32, kOffFormat = 36, kOffColorSpace = 40;
constexpr size_t kOffAddressMode = 44, kOffTailFirstMip = 48, kOffMipDirCount = 52;
constexpr size_t kOffTileDirCount = 56, kOffTailDirCount = 60, kOffMipDirOffset = 64;
constexpr size_t kOffTileDirOffset = 72, kOffTailDirOffset = 80, kOffPayloadEnd = 88;

// ---------------------------------------------------------------------------
// 地址与编码（§13.1 第 1 条）
// ---------------------------------------------------------------------------

void TestAddressMath() {
  expectEqU64(svt::svtMipDim(4096, 5), 128, "mipDim 4096>>5");
  expectEqU64(svt::svtMipDim(2048, 12), 1, "mipDim clamp to 1");
  expectEqU64(svt::svtMipDim(16384, 0), 16384, "mipDim mip0");
  expectEqU64(svt::svtTilesPerAxis(128, 128), 1, "tilesPerAxis exact");
  expectEqU64(svt::svtTilesPerAxis(129, 128), 2, "tilesPerAxis ceil");
  expectEqU64(svt::svtFullMipCount(4096, 2048), 13, "fullMipCount 4096x2048");
  expectEqU64(svt::svtFullMipCount(1, 1), 1, "fullMipCount 1x1");
  expectEqU64(svt::svtComputeTailFirstMip(4096, 2048, 13, 128), 5, "tailFirstMip 4096x2048");
  expectEqU64(svt::svtComputeTailFirstMip(16384, 16384, 15, 128), 7, "tailFirstMip 16384");
  expectEqU64(svt::svtComputeTailFirstMip(64, 64, 7, 128), 0, "small texture all tail");
  // 设计 §7.1：16384²、T=128 的页表项 = 128²+64²+…+2² = 21844。
  expectEqU64(svt::svtPageTableEntryCount(16384, 16384, 7, 128), 21844, "pageTable 16384");
  // 长方形极端：16384x2，height 全程 < tile。
  expectEqU64(svt::svtComputeTailFirstMip(16384, 2, 15, 128), 7, "tailFirstMip 16384x2");
  expectEqU64(svt::svtPageTableEntryCount(16384, 2, 7, 128), 254, "pageTable 16384x2");
  // Repeat 包装（含负坐标）。
  expectEqU64(svt::svtWrapRepeat(-1, 64), 63, "wrap -1");
  expectEqU64(svt::svtWrapRepeat(-65, 64), 63, "wrap -65");
  expectEqU64(svt::svtWrapRepeat(64, 64), 0, "wrap 64");
  expectEqU64(svt::svtWrapRepeat(129, 64), 1, "wrap 129");
  expectEqU64(svt::svtWrapRepeat(0, 64), 0, "wrap 0");
  // CRC32 标准校验值。
  expectEqU64(svt::svtCrc32(reinterpret_cast<const uint8_t *>("123456789"), 9), 0xCBF43926u,
              "crc32 check value");
  expectEqU64(svt::svtCrc32(nullptr, 0), 0, "crc32 empty");
}

// ---------------------------------------------------------------------------
// cooker 边框一致性：RGBA8 路径（136² 边框 vs 整张源 mip Repeat 采样）
// ---------------------------------------------------------------------------

bool verifyRgba8Tiles(const svt::SvtAsset &asset, const svt::SvtSourceTexture &src,
                      std::string *firstDiff) {
  const uint32_t P = asset.paddedTileSize();
  const uint32_t B = asset.border();
  for (uint32_t m = 0; m < asset.tailFirstMip(); ++m) {
    const uint32_t mw = asset.mipWidth(m), mh = asset.mipHeight(m);
    const uint8_t *mipData = src.mips[m].data();
    for (uint32_t ty = 0; ty < asset.tilesY(m); ++ty)
      for (uint32_t tx = 0; tx < asset.tilesX(m); ++tx) {
        std::vector<uint8_t> payload;
        if (!asset.readTile(m, tx, ty, payload, firstDiff))
          return false;
        for (uint32_t py = 0; py < P; ++py)
          for (uint32_t px = 0; px < P; ++px) {
            const uint32_t sx = svt::svtWrapRepeat(int64_t(tx) * asset.tileSize() + px - B, mw);
            const uint32_t sy = svt::svtWrapRepeat(int64_t(ty) * asset.tileSize() + py - B, mh);
            for (uint32_t c = 0; c < 4; ++c) {
              const uint8_t got = payload[(size_t(py) * P + px) * 4 + c];
              const uint8_t want = mipData[(size_t(sy) * mw + sx) * 4 + c];
              if (got != want) {
                if (firstDiff)
                  *firstDiff = "rgba8 texel mismatch mip=" + std::to_string(m) + " tile=" +
                               std::to_string(tx) + "," + std::to_string(ty) + " px=" +
                               std::to_string(px) + "," + std::to_string(py);
                return false;
              }
            }
          }
      }
  }
  return true;
}

bool verifyBcTiles(const svt::SvtAsset &asset, const svt::SvtSourceTexture &src,
                   std::string *firstDiff) {
  const uint32_t P = asset.paddedTileSize();
  const uint32_t B = asset.border();
  const uint32_t bpb = svt::svtFormatBytesPerUnit(asset.format());
  for (uint32_t m = 0; m < asset.tailFirstMip(); ++m) {
    const uint32_t mw = asset.mipWidth(m), mh = asset.mipHeight(m);
    const uint32_t bw = std::max(1u, (mw + 3) / 4), bh = std::max(1u, (mh + 3) / 4);
    const uint8_t *mipData = src.mips[m].data();
    for (uint32_t ty = 0; ty < asset.tilesY(m); ++ty)
      for (uint32_t tx = 0; tx < asset.tilesX(m); ++tx) {
        std::vector<uint8_t> payload;
        if (!asset.readTile(m, tx, ty, payload, firstDiff))
          return false;
        const uint32_t blocksPerSide = P / 4;
        for (uint32_t by = 0; by < blocksPerSide; ++by)
          for (uint32_t bx = 0; bx < blocksPerSide; ++bx) {
            const int64_t x0 = int64_t(tx) * asset.tileSize() + int64_t(bx) * 4 - B;
            const int64_t y0 = int64_t(ty) * asset.tileSize() + int64_t(by) * 4 - B;
            const uint32_t sbx = svt::svtWrapRepeat(x0 / 4, bw);
            const uint32_t sby = svt::svtWrapRepeat(y0 / 4, bh);
            const uint8_t *got = &payload[(size_t(by) * blocksPerSide + bx) * bpb];
            const uint8_t *want = &mipData[(size_t(sby) * bw + sbx) * bpb];
            if (memcmp(got, want, bpb) != 0) {
              if (firstDiff)
                *firstDiff = "bc block mismatch mip=" + std::to_string(m) + " tile=" +
                             std::to_string(tx) + "," + std::to_string(ty) + " block=" +
                             std::to_string(bx) + "," + std::to_string(by);
              return false;
            }
          }
      }
  }
  return true;
}

bool verifyTails(const svt::SvtAsset &asset, const svt::SvtSourceTexture &src,
                 std::string *firstDiff) {
  for (uint32_t m = asset.tailFirstMip(); m < asset.mipCount(); ++m) {
    std::vector<uint8_t> payload;
    if (!asset.readTailMip(m, payload, firstDiff))
      return false;
    if (payload != src.mips[m]) {
      if (firstDiff)
        *firstDiff = "tail mip mismatch at " + std::to_string(m);
      return false;
    }
  }
  return true;
}

void TestCookRoundTripRgba8() {
  // 512x256：长方形 + tilesY=1 的短边行 + 跨 tile 边界。
  svt::SvtSourceTexture src = svt::svtMakeNoiseTexture(512, 256, 1234);
  svt::SvtCookParams params; // T=128 B=4
  const std::filesystem::path path = "svt_test_rgba8.svt";
  std::string error;
  svt::SvtCookStats stats;
  expectTrue(svt::svtCookTexture(src, params, path, &stats, &error), "cook rgba8 noise");
  if (failures)
    return;

  svt::SvtAsset asset;
  expectOpen(asset, path, "open cooked rgba8");
  expectEqU64(asset.width(), 512, "rgba8 width");
  expectEqU64(asset.height(), 256, "rgba8 height");
  expectEqU64(asset.mipCount(), 10, "rgba8 mipCount");
  expectEqU64(asset.tailFirstMip(), 2, "rgba8 tailFirstMip (512->128@2, 256->64@2)");
  expectEqU64(asset.pageTableEntryCount(), 8 + 2, "rgba8 page table entries"); // mip0 4x2 + mip1 2x1
  expectEqU64(asset.tilePayloadBytes(), 136 * 136 * 4, "rgba8 tile payload 136^2*4");
  expectEqU64(stats.tileCount, 10, "rgba8 tile count");

  // mip 目录必须按 mip 顺序给出 firstTileIndex 前缀和。
  expectEqU64(asset.mipDirectory()[0].firstTileIndex, 0, "mip0 firstTileIndex");
  expectEqU64(asset.mipDirectory()[1].firstTileIndex, 8, "mip1 firstTileIndex");

  std::string diff;
  expectTrue(verifyRgba8Tiles(asset, src, &diff), diff.empty() ? "rgba8 tiles exact" : diff.c_str());
  expectTrue(verifyTails(asset, src, &diff), diff.empty() ? "rgba8 tails exact" : diff.c_str());
}

void TestCookRoundTripBc() {
  // 可压缩 BC1：验证 codec==1 的 lzfse 路径与 block-copy 字节保留。
  svt::SvtSourceTexture src = svt::svtMakeNoiseBcTexture(svt::SvtFormat::BC1, 512, 512, 77, true);
  svt::SvtCookParams params;
  const std::filesystem::path path = "svt_test_bc1.svt";
  std::string error;
  expectTrue(svt::svtCookTexture(src, params, path, nullptr, &error), "cook bc1 compressible");
  svt::SvtAsset asset;
  expectOpen(asset, path, "open cooked bc1");
  expectEqU64(asset.format() == svt::SvtFormat::BC1, true, "bc1 format");
  expectEqU64(asset.tilePayloadBytes(), 34 * 34 * 8, "bc1 tile payload 34^2*8");
  bool anyLzfse = false;
  for (const auto &e : asset.tileDirectory())
    if (e.codec == svt::kSvtCodecLzfse)
      anyLzfse = true;
  expectTrue(anyLzfse, "bc1 compressible tiles use lzfse codec");

  std::string diff;
  expectTrue(verifyBcTiles(asset, src, &diff), diff.empty() ? "bc1 tiles exact" : diff.c_str());
  expectTrue(verifyTails(asset, src, &diff), diff.empty() ? "bc1 tails exact" : diff.c_str());

  // BC7 + --raw：验证 codec 0 直接读取路径（16B/block）。
  // （随机数据也可能被 lzfse 压小几字节，因此 raw 路径用显式 compress=false 覆盖。）
  svt::SvtSourceTexture src7 = svt::svtMakeNoiseBcTexture(svt::SvtFormat::BC7, 256, 128, 99, false);
  const std::filesystem::path path7 = "svt_test_bc7.svt";
  svt::SvtCookParams rawParams;
  rawParams.compress = false;
  expectTrue(svt::svtCookTexture(src7, rawParams, path7, nullptr, &error), "cook bc7 noise");
  svt::SvtAsset asset7;
  expectOpen(asset7, path7, "open cooked bc7");
  expectEqU64(asset7.tilePayloadBytes(), 34 * 34 * 16, "bc7 tile payload 34^2*16");
  bool anyRaw = false;
  for (const auto &e : asset7.tileDirectory())
    if (e.codec == svt::kSvtCodecRaw)
      anyRaw = true;
  expectTrue(anyRaw, "bc7 tiles stored raw with compress=false");
  expectTrue(verifyBcTiles(asset7, src7, &diff), diff.empty() ? "bc7 tiles exact" : diff.c_str());
  expectTrue(verifyTails(asset7, src7, &diff), diff.empty() ? "bc7 tails exact" : diff.c_str());
}

void TestTileCornerNeighbors() {
  // 页角四邻居（§13.1）：tile 边框 == 邻居 tile 的内侧 texel，跨 tile 连续。
  svt::SvtSourceTexture src = svt::svtMakeNoiseTexture(512, 512, 42);
  svt::SvtCookParams params;
  const std::filesystem::path path = "svt_test_neighbors.svt";
  std::string error;
  expectTrue(svt::svtCookTexture(src, params, path, nullptr, &error), "cook neighbors");
  svt::SvtAsset asset;
  expectOpen(asset, path, "open neighbors");

  const uint32_t m = 0;
  const uint32_t P = asset.paddedTileSize();
  const uint32_t B = asset.border();
  const uint32_t T = asset.tileSize();
  std::vector<std::vector<uint8_t>> tiles(asset.tilesX(m) * asset.tilesY(m));
  for (uint32_t ty = 0; ty < asset.tilesY(m); ++ty)
    for (uint32_t tx = 0; tx < asset.tilesX(m); ++tx)
      expectTrue(asset.readTile(m, tx, ty, tiles[ty * asset.tilesX(m) + tx], &error),
                 "read tile for neighbor test");

  const auto texel = [&](uint32_t i, uint32_t px, uint32_t py) -> const uint8_t * {
    return &tiles[i][(size_t(py) * P + px) * 4];
  };
  for (uint32_t ty = 0; ty < asset.tilesY(m); ++ty)
    for (uint32_t tx = 0; tx < asset.tilesX(m); ++tx) {
      const uint32_t i = ty * asset.tilesX(m) + tx;
      const uint32_t left = ty * asset.tilesX(m) + ((tx + asset.tilesX(m) - 1) % asset.tilesX(m));
      const uint32_t right = ty * asset.tilesX(m) + ((tx + 1) % asset.tilesX(m));
      const uint32_t up = ((ty + asset.tilesY(m) - 1) % asset.tilesY(m)) * asset.tilesX(m) + tx;
      const uint32_t down = ((ty + 1) % asset.tilesY(m)) * asset.tilesX(m) + tx;
      for (uint32_t b = 0; b < B; ++b) {
        expectTrue(memcmp(texel(i, b, B), texel(left, T + b, B), 4) == 0, "left border == left neighbor interior");
        expectTrue(memcmp(texel(i, T + B + b, B), texel(right, B + b, B), 4) == 0, "right border == right neighbor interior");
        expectTrue(memcmp(texel(i, B, b), texel(up, B, T + b), 4) == 0, "top border == upper neighbor interior");
        expectTrue(memcmp(texel(i, B, T + B + b), texel(down, B, B + b), 4) == 0, "bottom border == lower neighbor interior");
      }
    }
}

void TestSmallTextureAllTail() {
  // 64x64：整个作为 tail，无可流送页（设计 §6.1）。
  svt::SvtSourceTexture src = svt::svtMakeNoiseTexture(64, 64, 7);
  svt::SvtCookParams params;
  const std::filesystem::path path = "svt_test_small.svt";
  std::string error;
  expectTrue(svt::svtCookTexture(src, params, path, nullptr, &error), "cook small");
  svt::SvtAsset asset;
  expectOpen(asset, path, "open small");
  expectEqU64(asset.tailFirstMip(), 0, "small tailFirstMip 0");
  expectEqU64(asset.pageTableEntryCount(), 0, "small no tiles");
  expectEqU64(asset.tailMipCount(), 7, "small all mips in tail");
  std::string diff;
  expectTrue(verifyTails(asset, src, &diff), diff.empty() ? "small tails exact" : diff.c_str());
  std::vector<uint8_t> payload;
  expectTrue(!asset.readTile(0, 0, 0, payload, &error), "small readTile rejected");
}

void TestInvalidCoordinates() {
  svt::SvtSourceTexture src = svt::svtMakeNoiseTexture(512, 256, 5);
  svt::SvtCookParams params;
  const std::filesystem::path path = "svt_test_coords.svt";
  std::string error;
  expectTrue(svt::svtCookTexture(src, params, path, nullptr, &error), "cook coords");
  svt::SvtAsset asset;
  expectOpen(asset, path, "open coords");
  std::vector<uint8_t> payload;
  expectTrue(!asset.readTile(0, 999, 0, payload, &error), "x out of range rejected");
  expectTrue(!asset.readTile(0, 0, 999, payload, &error), "y out of range rejected");
  expectTrue(!asset.readTile(99, 0, 0, payload, &error), "mip out of range rejected");
  expectTrue(!asset.readTailMip(0, payload, &error), "tail mip below range rejected");
  expectTrue(!asset.readTailMip(asset.mipCount(), payload, &error), "tail mip above range rejected");
  expectTrue(asset.readTile(0, asset.tilesX(0) - 1, asset.tilesY(0) - 1, payload, &error),
             "max legal tile coordinate accepted");
  svt::SvtAsset closed;
  expectTrue(!closed.readTile(0, 0, 0, payload, &error), "read on unopened asset rejected");
}

// ---------------------------------------------------------------------------
// 恶意/损坏文件（§13.1 第 2 条）
// ---------------------------------------------------------------------------

std::vector<uint8_t> cookNoiseBytes(const std::filesystem::path &path) {
  svt::SvtSourceTexture src = svt::svtMakeNoiseTexture(256, 256, 31337);
  svt::SvtCookParams params;
  std::string error;
  if (!svt::svtCookTexture(src, params, path, nullptr, &error)) {
    std::fprintf(stderr, "FAIL cook for malformed tests: %s\n", error.c_str());
    ++failures;
    return {};
  }
  return readFileBytes(path);
}

void expectOpenFails(const std::vector<uint8_t> &bytes, const char *name, const char *what) {
  const std::filesystem::path path = std::string("svt_malformed_") + name + ".svt";
  writeFileBytes(path, bytes);
  svt::SvtAsset asset;
  std::string error;
  expectTrue(!asset.open(path, &error), what);
}

void TestMalformedFiles() {
  const std::vector<uint8_t> good = cookNoiseBytes("svt_test_good.svt");
  if (good.empty())
    return;
  // 256x256, T=128: mip1 (128x128) 已 <= T，因此只有 mip0 有 2x2 tiles，tailFirstMip=1。
  expectEqU64(leU32(good, kOffTailFirstMip), 1, "baseline tailFirstMip");

  { auto b = good; b.resize(10); expectOpenFails(b, "trunc_hdr", "truncated header rejected"); }
  { auto b = good; patchU32(b, kOffMagic, 0x31545654u); expectOpenFails(b, "magic", "bad magic rejected"); }
  { auto b = good; patchU32(b, kOffVersion, 2); expectOpenFails(b, "version", "bad version rejected"); }
  { auto b = good; patchU32(b, kOffHeaderSize, 8); expectOpenFails(b, "hdrsize", "bad headerSize rejected"); }
  { auto b = good; patchU32(b, kOffFlags, 0); expectOpenFails(b, "endian", "non-little-endian rejected"); }
  { auto b = good; patchU32(b, kOffWidth, 255); expectOpenFails(b, "pot", "non-POT width rejected"); }
  { auto b = good; patchU32(b, kOffWidth, 1u << 20); expectOpenFails(b, "bigdim", "oversized width rejected"); }
  { auto b = good; patchU32(b, kOffFormat, 200); expectOpenFails(b, "format", "unknown format rejected"); }
  { auto b = good; patchU32(b, kOffColorSpace, 1); expectOpenFails(b, "cs", "colorSpace mismatch rejected"); }
  { auto b = good; patchU32(b, kOffAddressMode, 1); expectOpenFails(b, "addr", "non-Repeat address rejected"); }
  { auto b = good; patchU32(b, kOffTailFirstMip, 3); expectOpenFails(b, "tail", "inconsistent tailFirstMip rejected"); }
  { auto b = good; patchU32(b, kOffTileDirCount, 6); expectOpenFails(b, "tilecount", "tile count mismatch rejected"); }
  { auto b = good; patchU32(b, kOffTailDirCount, 5); expectOpenFails(b, "tailcount", "tail count mismatch rejected"); }
  { auto b = good; patchU32(b, kOffTileDirCount, 0x0FFFFFFFu); expectOpenFails(b, "hugecount", "huge tile count rejected before alloc"); }
  { auto b = good; patchU64(b, kOffMipDirOffset, leU64(good, kOffPayloadEnd)); expectOpenFails(b, "mipoff", "mip dir offset out of range rejected"); }
  { auto b = good; patchU64(b, kOffTileDirOffset, leU64(good, kOffPayloadEnd) - 10); expectOpenFails(b, "tileoff", "tile dir offset out of range rejected"); }
  { auto b = good; patchU64(b, kOffPayloadEnd, leU64(good, kOffPayloadEnd) - 1); expectOpenFails(b, "payloadend", "payloadEnd != file length rejected"); }

  // tile 目录项破坏：decodedSize、CRC、offset 溢出、storedSize。
  const uint64_t tileDirOffset = leU64(good, kOffTileDirOffset);
  { auto b = good; patchU32(b, size_t(tileDirOffset) + 12, 12345); expectOpenFails(b, "decoded", "tile decodedSize mismatch rejected"); }
  { auto b = good; patchU32(b, size_t(tileDirOffset) + 20, 0); // CRC 不在 open 时校验，readTile 时拒绝
    const std::filesystem::path p = "svt_malformed_crc.svt";
    writeFileBytes(p, b);
    svt::SvtAsset a;
    std::string err;
    expectTrue(a.open(p, &err), "CRC-corrupted file still opens (validated at read)");
    std::vector<uint8_t> payload;
    expectTrue(!a.readTile(0, 0, 0, payload, &err), "tile CRC mismatch rejected at read"); }
  { auto b = good; patchU64(b, size_t(tileDirOffset), leU64(good, kOffPayloadEnd)); expectOpenFails(b, "tileoff2", "tile payload offset overflow rejected"); }
  { auto b = good; patchU32(b, size_t(tileDirOffset) + 8, 0xFFFFFFF0u); expectOpenFails(b, "stored", "tile storedSize overflow rejected"); }
  { auto b = good; patchU32(b, size_t(tileDirOffset) + 16, 5); expectOpenFails(b, "codec", "unknown tile codec rejected"); }

  // BC block 对齐：对 BC 文件给非 4 对齐的 tileSize/border。
  {
    svt::SvtSourceTexture src = svt::svtMakeNoiseBcTexture(svt::SvtFormat::BC1, 256, 256, 8, true);
    svt::SvtCookParams params;
    const std::filesystem::path bcPath = "svt_test_bc_align.svt";
    std::string error;
    expectTrue(svt::svtCookTexture(src, params, bcPath, nullptr, &error), "cook bc align baseline");
    auto b = readFileBytes(bcPath);
    expectTrue(!b.empty(), "read bc align baseline");
    patchU32(b, kOffTileSize, 126);
    expectOpenFails(b, "align", "BC tile not block aligned rejected");
  }

  // 源校验：非完整 mip 链、非 POT、mip 字节数不符。
  {
    svt::SvtSourceTexture src = svt::svtMakeNoiseTexture(256, 256, 9);
    src.mips.pop_back();
    std::string error;
    expectTrue(!svt::svtCookTexture(src, svt::SvtCookParams{}, "svt_bad_chain.svt", nullptr, &error),
               "incomplete mip chain rejected");
    svt::SvtSourceTexture badDim = svt::svtMakeNoiseTexture(256, 256, 10);
    badDim.width = 100; // 非 POT
    expectTrue(!svt::svtCookTexture(badDim, svt::SvtCookParams{}, "svt_bad_dim.svt", nullptr, &error),
               "non-POT source rejected");
    svt::SvtSourceTexture badSize = svt::svtMakeNoiseTexture(256, 256, 11);
    badSize.mips[0].pop_back();
    expectTrue(!svt::svtCookTexture(badSize, svt::SvtCookParams{}, "svt_bad_size.svt", nullptr, &error),
               "mip byte size mismatch rejected");
  }
}

// ---------------------------------------------------------------------------
// bistro .dxt.bin 集成：合成 mini bin -> 解析 -> 解码 -> cook
// ---------------------------------------------------------------------------

void wU32ToVec(std::vector<uint8_t> &v, uint32_t x);
void wU64ToVec(std::vector<uint8_t> &v, uint64_t x);

void writeMiniBistroBin(const std::filesystem::path &path, const svt::SvtSourceTexture &src,
                        uint32_t mtlPixelFormat, uint32_t pathHash) {
#ifdef _WIN32
  FILE *f = nullptr;
  if (_wfopen_s(&f, path.wstring().c_str(), L"wb") != 0 || !f)
#else
  FILE *f = fopen(path.string().c_str(), "wb");
  if (!f)
#endif
    return;
  const auto wU32 = [&](uint32_t v) { fwrite(&v, 4, 1, f); };
  const auto wU64 = [&](uint64_t v) { fwrite(&v, 8, 1, f); };
  const auto wBytes = [&](const void *p, size_t n) { fwrite(p, 1, n, f); };

  // AAPLMeshData 头部：12 个 u64 计数字段（需满足分类计数总和 == 总数的自校验）。
  const uint64_t counts[12] = {/*vertex*/ 8, /*index*/ 36, /*indexType*/ 1,
                               /*chunk*/ 3, /*mesh*/ 3,
                               /*opaqueChunk*/ 1, /*opaqueMesh*/ 1,
                               /*alphaMaskedChunk*/ 1, /*alphaMaskedMesh*/ 1,
                               /*transparentChunk*/ 1, /*transparentMesh*/ 1,
                               /*material*/ 1};
  for (int i = 0; i < 12; ++i)
    wU64(counts[i]);
  for (int i = 0; i < 8; ++i) { // 8 个压缩数据段
    wU64(4);
    const uint8_t pad[4] = {1, 2, 3, 4};
    wBytes(pad, 4);
  }
  wU64(1); // texture_count

  // 纹理描述符
  const std::string texPath = "textures/Bistro/Test_BaseColor";
  wU32(static_cast<uint32_t>(texPath.size()));
  wBytes(texPath.data(), texPath.size());
  wU32(pathHash);
  wU64(src.width);
  wU64(src.height);
  wU64(src.mipCount);
  wU32(mtlPixelFormat);
  const uint64_t pixelDataOffset = 0;

  // 先构造 blob（Apple LZFSE 分块），才能写 pixelDataLength 与 mip 目录。
  std::vector<uint64_t> mipOffsets(src.mipCount), mipLengths(src.mipCount);
  std::vector<uint8_t> blob;
  for (uint32_t m = 0; m < src.mipCount; ++m) {
    mipOffsets[m] = blob.size();
    std::vector<uint8_t> compressed(src.mips[m].size() + 1024);
    const size_t r = lzfse_encode_buffer(compressed.data(), compressed.size(),
                                         src.mips[m].data(), src.mips[m].size(), nullptr);
    compressed.resize(r);
    // AAPLCompressionHeader：u32 mode | pad | u64 uncomp | u64 comp（24 字节）
    wU32ToVec(blob, 2049);
    wU32ToVec(blob, 0);
    wU64ToVec(blob, src.mips[m].size());
    wU64ToVec(blob, compressed.size());
    blob.insert(blob.end(), compressed.begin(), compressed.end());
    mipLengths[m] = 24 + compressed.size();
  }
  const uint64_t pixelDataLength = blob.size();

  wU64(pixelDataOffset);
  wU64(pixelDataLength);
  for (uint64_t o : mipOffsets) wU64(o);
  for (uint64_t l : mipLengths) wU64(l);
  wU64(blob.size());
  wBytes(blob.data(), blob.size());
  fclose(f);
}

void wU32ToVec(std::vector<uint8_t> &v, uint32_t x) {
  for (int i = 0; i < 4; ++i)
    v.push_back(static_cast<uint8_t>(x >> (8 * i)));
}
void wU64ToVec(std::vector<uint8_t> &v, uint64_t x) {
  for (int i = 0; i < 8; ++i)
    v.push_back(static_cast<uint8_t>(x >> (8 * i)));
}

void TestBistroIntegration() {
  const svt::SvtSourceTexture src = svt::svtMakeNoiseBcTexture(svt::SvtFormat::BC1, 256, 256, 555, true);
  const std::filesystem::path binPath = "svt_test_mini.dxt.bin";
  writeMiniBistroBin(binPath, src, 130 /*MTLPixelFormatBC1_RGBA*/, 0xDEADBEEFu);

  svt::SvtBistroInfo info;
  std::string error;
  expectTrue(svt::svtReadBistroInfo(binPath, info, &error), "read bistro info");
  expectEqU64(info.textures.size(), 1, "bistro texture count");
  if (info.textures.size() == 1) {
    const auto &t = info.textures[0];
    expectTrue(t.path == "textures/Bistro/Test_BaseColor", "bistro texture path");
    expectEqU64(t.pathHash, 0xDEADBEEFu, "bistro path hash");
    expectEqU64(t.width, 256, "bistro width");
    expectEqU64(t.height, 256, "bistro height");
    expectEqU64(t.mipCount, 9, "bistro mip count");
    expectEqU64(t.pixelFormat, 130, "bistro pixel format");
    expectTrue(t.pixelDataLength == info.textureBlobLength, "bistro blob length");

    svt::SvtSourceTexture loaded;
    expectTrue(svt::svtLoadBistroTexture(binPath, info, 0, loaded, &error), "load bistro texture");
    expectTrue(loaded.mips == src.mips, "bistro decoded mips match source");

    // cook 后再验证 tile/tail 与 bin 解码数据一致。
    svt::SvtCookParams params;
    const std::filesystem::path outDir = "svt_test_bistro_out";
    std::error_code ec;
    std::filesystem::create_directories(outDir, ec);
    const std::filesystem::path svtPath = outDir / "000000_deadbeef.svt";
    expectTrue(svt::svtCookTexture(loaded, params, svtPath, nullptr, &error), "cook bistro texture");
    svt::SvtAsset asset;
    expectOpen(asset, svtPath, "open cooked bistro");
    std::string diff;
    expectTrue(verifyBcTiles(asset, loaded, &diff), diff.empty() ? "bistro tiles exact" : diff.c_str());
    expectTrue(verifyTails(asset, loaded, &diff), diff.empty() ? "bistro tails exact" : diff.c_str());
  }

  // 不支持的格式（BC2=132）应被拒绝。
  {
    const std::filesystem::path bin2 = "svt_test_mini_bc2.dxt.bin";
    writeMiniBistroBin(bin2, src, 132, 1);
    svt::SvtBistroInfo info2;
    expectTrue(svt::svtReadBistroInfo(bin2, info2, &error), "read bistro info bc2");
    svt::SvtSourceTexture loaded;
    expectTrue(!svt::svtLoadBistroTexture(bin2, info2, 0, loaded, &error),
               "unsupported pixel format rejected");
  }
}

void TestManifestRoundTrip() {
  svt::SvtSourceTexture src = svt::svtMakeNoiseTexture(512, 256, 2024);
  svt::SvtCookParams params;
  const std::filesystem::path svtPath = "svt_test_manifest.svt";
  std::string error;
  svt::SvtCookStats stats;
  expectTrue(svt::svtCookTexture(src, params, svtPath, &stats, &error), "cook manifest asset");

  std::vector<svt::SvtManifestEntry> entries;
  svt::SvtManifestEntry e;
  e.sourcePath = "textures/Bistro/Manifest_Test";
  e.pathHash = 0x12345678;
  e.contentHash = svt::svtComputeContentHash(src);
  e.svtFile = svtPath.filename().string();
  e.width = src.width;
  e.height = src.height;
  e.mipCount = src.mipCount;
  e.format = src.format;
  e.tailFirstMip = svt::svtComputeTailFirstMip(src.width, src.height, src.mipCount, params.tileSize);
  e.tileCount = stats.tileCount;
  entries.push_back(e);

  const std::filesystem::path manifestPath = "svt_test_manifest.json";
  expectTrue(svt::svtWriteManifest(manifestPath, entries, &error), "write manifest");
  std::vector<svt::SvtManifestEntry> loaded;
  expectTrue(svt::svtReadManifest(manifestPath, loaded, &error), "read manifest");
  expectEqU64(loaded.size(), 1, "manifest entry count");
  if (loaded.size() == 1) {
    expectTrue(loaded[0].sourcePath == e.sourcePath, "manifest sourcePath");
    expectEqU64(loaded[0].pathHash, e.pathHash, "manifest pathHash");
    expectEqU64(loaded[0].contentHash, e.contentHash, "manifest contentHash exact uint64");
    expectTrue(loaded[0].svtFile == e.svtFile, "manifest svtFile");
    expectEqU64(loaded[0].width, e.width, "manifest width");
    expectEqU64(loaded[0].tailFirstMip, e.tailFirstMip, "manifest tailFirstMip");
    expectEqU64(loaded[0].tileCount, e.tileCount, "manifest tileCount");
  }

  // manifest 中的 .svt 能被打开且内容 hash 匹配。
  svt::SvtAsset asset;
  expectOpen(asset, manifestPath.parent_path() / loaded[0].svtFile,
             "open manifest-referenced svt");
  expectEqU64(asset.sourceContentHash(), e.contentHash, "svt content hash matches manifest");
}

void TestDiagnosticTextureShape() {
  svt::SvtSourceTexture t = svt::svtMakeDiagnosticTexture(256, 128, 128);
  expectEqU64(t.width, 256, "diag width");
  expectEqU64(t.height, 128, "diag height");
  expectEqU64(t.mipCount, 9, "diag mipCount");
  expectEqU64(t.mips[0].size(), 256 * 128 * 4, "diag mip0 bytes");
  expectEqU64(t.mips[8].size(), 1 * 1 * 4, "diag last mip bytes");
  // 生成后可直接 cook（诊断资产用于 M2 GPU 场景）。
  std::string error;
  expectTrue(svt::svtCookTexture(t, svt::SvtCookParams{}, "svt_test_diag.svt", nullptr, &error),
             "cook diagnostic texture");
}

} // namespace

int main() {
  const struct {
    const char *name;
    void (*fn)();
  } tests[] = {
      {"AddressMath", TestAddressMath},
      {"CookRoundTripRgba8", TestCookRoundTripRgba8},
      {"CookRoundTripBc", TestCookRoundTripBc},
      {"TileCornerNeighbors", TestTileCornerNeighbors},
      {"SmallTextureAllTail", TestSmallTextureAllTail},
      {"InvalidCoordinates", TestInvalidCoordinates},
      {"MalformedFiles", TestMalformedFiles},
      {"BistroIntegration", TestBistroIntegration},
      {"ManifestRoundTrip", TestManifestRoundTrip},
      {"DiagnosticTextureShape", TestDiagnosticTextureShape},
  };
  for (const auto &t : tests) {
    std::fprintf(stderr, "[run] %s\n", t.name);
    std::fflush(stderr);
    t.fn();
  }
  if (failures == 0)
    std::printf("SvtAssetTests: all passed\n");
  else
    std::printf("SvtAssetTests: %d failures\n", failures);
  return failures == 0 ? 0 : 1;
}
