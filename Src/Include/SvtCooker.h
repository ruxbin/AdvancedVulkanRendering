#pragma once
// SVT 离线切片（cook）核心库：从源纹理生成 .svt tile/tail 文件与 manifest。
// 设计依据：docs/superpowers/specs/2026-10-16-streaming-virtual-texture-design.md §6.1、§6.2。
//
// 核心逻辑放在 Src/SvtCooker.cpp（而非只放在 Tools/），因为设计 §13.1 要求
// cooker 的边框/切片正确性必须有 CPU 测试覆盖；Tools/SvtCooker.cpp 只是 CLI 壳。

#include "SvtAsset.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace svt {

// ---------------------------------------------------------------------------
// 源纹理：已解码的完整 mip 链（cook 时一次性使用；运行时用 range reader）
// ---------------------------------------------------------------------------

struct SvtSourceTexture {
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t mipCount = 0;
  SvtFormat format = SvtFormat::RGBA8;
  // mips[m] 为第 m 层完整解码数据（BC 为 block 网格，RGBA8 为行主序 texel）。
  std::vector<std::vector<uint8_t>> mips;
};

struct SvtCookParams {
  uint32_t tileSize = 128; // T：内部 texel
  uint32_t border = 4;     // B：每侧边框 texel
  bool compress = true;    // true = tile/tail 使用 LZFSE（codec 1）；压不小则回退 raw
};

struct SvtCookStats {
  size_t tileCount = 0;
  size_t tailMipCount = 0;
  uint64_t fileBytes = 0;
  uint64_t rawPayloadBytes = 0; // 未压缩 payload 总量
};

// 校验源纹理是否满足首版约束（POT、完整链、格式、尺寸一致）。
bool svtValidateSource(const SvtSourceTexture &src, const SvtCookParams &params,
                       std::string *error);

// 源内容 hash：对元数据与各 mip 解码字节做 FNV-1a 64。
uint64_t svtComputeContentHash(const SvtSourceTexture &src);

// 写出 .svt 文件。失败返回 false 并写 error。
bool svtCookTexture(const SvtSourceTexture &src, const SvtCookParams &params,
                    const std::filesystem::path &outPath, SvtCookStats *stats = nullptr,
                    std::string *error = nullptr);

// ---------------------------------------------------------------------------
// 诊断纹理生成（设计 §13.2：确定性、可验证）
// ---------------------------------------------------------------------------

// RGBA8 棋盘 + tile 网格线 + 渐变 + 每 mip 色调，用于视觉回归。
SvtSourceTexture svtMakeDiagnosticTexture(uint32_t width, uint32_t height,
                                          uint32_t tileSize = 128);
// RGBA8 确定性伪随机纹理，用于字节级边框/切片测试。
SvtSourceTexture svtMakeNoiseTexture(uint32_t width, uint32_t height, uint64_t seed);
// 伪随机 BC block 数据（每 block 字节独立同分布），用于压缩格式切片测试。
// compressible=true 生成高度可压缩的模式数据，使 cook 后 codec==1 可被验证。
SvtSourceTexture svtMakeNoiseBcTexture(SvtFormat format, uint32_t width, uint32_t height,
                                       uint64_t seed, bool compressible);

// ---------------------------------------------------------------------------
// bistro.dxt.bin 集成（设计 §2/§6.3；这里只做 cook 期解析）
// ---------------------------------------------------------------------------

// .dxt.bin 中纹理描述的独立解析结果（与 GpuScene::AAPLTextureData 字段对应）。
struct SvtBistroTextureInfo {
  std::string path;
  uint32_t pathHash = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t mipCount = 0;
  uint32_t pixelFormat = 0; // MTLPixelFormat 代码
  uint64_t pixelDataOffset = 0; // 相对纹理 blob 起点
  uint64_t pixelDataLength = 0;
  std::vector<uint64_t> mipOffsets; // 相对 pixelDataOffset
  std::vector<uint64_t> mipLengths;
};

struct SvtBistroInfo {
  uint64_t textureBlobOffset = 0; // blob 在 .dxt.bin 中的绝对偏移
  uint64_t textureBlobLength = 0;
  std::vector<SvtBistroTextureInfo> textures;
};

// 只解析目录（不解码任何 mip）。
bool svtReadBistroInfo(const std::filesystem::path &binPath, SvtBistroInfo &out,
                       std::string *error = nullptr);

// 解码指定纹理的完整 mip 链（AAPL LZFSE 分块）。
bool svtLoadBistroTexture(const std::filesystem::path &binPath, const SvtBistroInfo &info,
                          size_t textureIndex, SvtSourceTexture &out,
                          std::string *error = nullptr);

// MTLPixelFormat -> SvtFormat；不支持的格式返回 false（资产保留旧路径，设计 §6.1）。
bool svtFormatFromMtlPixelFormat(uint32_t mtlPixelFormat, SvtFormat &out);

// ---------------------------------------------------------------------------
// manifest（设计 §6.1：规范化路径 + 原 _pathHash + 内容 hash）
// ---------------------------------------------------------------------------

struct SvtManifestEntry {
  std::string sourcePath;  // 源纹理规范化路径（.dxt.bin 内记录的原始字符串）
  uint32_t pathHash = 0;   // 原 _pathHash（不单独信任，仅辅助匹配）
  uint64_t contentHash = 0;
  std::string svtFile;     // 相对 manifest 所在目录的 .svt 文件名
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t mipCount = 0;
  SvtFormat format = SvtFormat::RGBA8;
  uint32_t tailFirstMip = 0;
  uint64_t tileCount = 0;
};

bool svtWriteManifest(const std::filesystem::path &manifestPath,
                      const std::vector<SvtManifestEntry> &entries, std::string *error = nullptr);
bool svtReadManifest(const std::filesystem::path &manifestPath,
                     std::vector<SvtManifestEntry> &out, std::string *error = nullptr);

// ---------------------------------------------------------------------------
// 整包 cook：.dxt.bin -> outDir/*.svt + manifest.json
// ---------------------------------------------------------------------------

struct SvtBistroCookReport {
  std::vector<SvtManifestEntry> cooked;
  std::vector<std::pair<std::string, std::string>> skipped; // (源路径, 原因)
  uint64_t totalFileBytes = 0;
};

bool svtCookBistroBin(const std::filesystem::path &binPath,
                      const std::filesystem::path &outDir, const SvtCookParams &params,
                      SvtBistroCookReport &report, std::string *error = nullptr);

} // namespace svt
