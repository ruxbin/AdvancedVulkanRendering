// SvtCooker 命令行工具：离线生成 .svt tile/tail 文件与 manifest。
// 用法：
//   SvtCooker bistro <bistro.dxt.bin> <outDir> [--tile N] [--border N] [--raw]
//   SvtCooker diag <out.svt> <width> <height> [--tile N] [--border N] [--raw]
//   SvtCooker info <file.svt>
// 设计依据：docs/superpowers/specs/2026-10-16-streaming-virtual-texture-design.md §6、§14 M1。

#include "SvtAsset.h"
#include "SvtCooker.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

namespace {

int usage() {
  std::fprintf(stderr,
               "Usage:\n"
               "  SvtCooker bistro <bistro.dxt.bin> <outDir> [--tile N] [--border N] [--raw]\n"
               "  SvtCooker diag <out.svt> <width> <height> [--tile N] [--border N] [--raw]\n"
               "  SvtCooker info <file.svt>\n");
  return 2;
}

bool parseParams(int argc, char **argv, int firstOpt, svt::SvtCookParams &params) {
  for (int i = firstOpt; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--raw") {
      params.compress = false;
    } else if (arg == "--tile" && i + 1 < argc) {
      params.tileSize = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 10));
    } else if (arg == "--border" && i + 1 < argc) {
      params.border = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 10));
    } else {
      std::fprintf(stderr, "unknown option: %s\n", arg.c_str());
      return false;
    }
  }
  return true;
}

int cmdBistro(int argc, char **argv) {
  if (argc < 4)
    return usage();
  svt::SvtCookParams params;
  if (!parseParams(argc, argv, 4, params))
    return 2;

  svt::SvtBistroCookReport report;
  std::string error;
  if (!svt::svtCookBistroBin(argv[2], argv[3], params, report, &error)) {
    std::fprintf(stderr, "cook failed: %s\n", error.c_str());
    return 1;
  }
  const std::filesystem::path manifestPath =
      std::filesystem::path(argv[3]) / "manifest.json";
  if (!svt::svtWriteManifest(manifestPath, report.cooked, &error)) {
    std::fprintf(stderr, "manifest write failed: %s\n", error.c_str());
    return 1;
  }
  std::printf("cooked %zu textures (%llu bytes), skipped %zu\n", report.cooked.size(),
              static_cast<unsigned long long>(report.totalFileBytes), report.skipped.size());
  for (const auto &s : report.skipped)
    std::printf("  skip %s : %s\n", s.first.c_str(), s.second.c_str());
  std::printf("manifest: %s\n", manifestPath.string().c_str());
  return 0;
}

int cmdDiag(int argc, char **argv) {
  if (argc < 5)
    return usage();
  svt::SvtCookParams params;
  if (!parseParams(argc, argv, 5, params))
    return 2;
  const uint32_t width = static_cast<uint32_t>(std::strtoul(argv[3], nullptr, 10));
  const uint32_t height = static_cast<uint32_t>(std::strtoul(argv[4], nullptr, 10));

  svt::SvtSourceTexture src = svt::svtMakeDiagnosticTexture(width, height, params.tileSize);
  svt::SvtCookStats stats;
  std::string error;
  if (!svt::svtCookTexture(src, params, argv[2], &stats, &error)) {
    std::fprintf(stderr, "cook failed: %s\n", error.c_str());
    return 1;
  }
  std::printf("wrote %s: %zu tiles, %zu tail mips, %llu bytes\n", argv[2],
              stats.tileCount, stats.tailMipCount,
              static_cast<unsigned long long>(stats.fileBytes));
  return 0;
}

int cmdInfo(int argc, char **argv) {
  if (argc < 3)
    return usage();
  svt::SvtAsset asset;
  std::string error;
  if (!asset.open(argv[2], &error)) {
    std::fprintf(stderr, "open failed: %s\n", error.c_str());
    return 1;
  }
  std::printf("size: %ux%u, mips: %u, format: %u\n", asset.width(), asset.height(),
              asset.mipCount(), static_cast<uint32_t>(asset.format()));
  std::printf("tile: T=%u B=%u P=%u, tailFirstMip: %u (%u tail mips)\n",
              asset.tileSize(), asset.border(), asset.paddedTileSize(),
              asset.tailFirstMip(), asset.tailMipCount());
  std::printf("page table entries: %u, tile payload: %u bytes\n",
              asset.pageTableEntryCount(), asset.tilePayloadBytes());
  std::printf("content hash: 0x%016llx, file: %llu bytes\n",
              static_cast<unsigned long long>(asset.sourceContentHash()),
              static_cast<unsigned long long>(asset.fileLength()));
  for (uint32_t m = 0; m < asset.mipCount(); ++m) {
    if (asset.mipIsTiled(m))
      std::printf("  mip %2u: %ux%u, tiles %ux%u\n", m, asset.mipWidth(m), asset.mipHeight(m),
                  asset.tilesX(m), asset.tilesY(m));
    else
      std::printf("  mip %2u: %ux%u, tail (%u bytes)\n", m, asset.mipWidth(m),
                  asset.mipHeight(m), asset.tailMipPayloadBytes(m));
  }
  return 0;
}

} // namespace

int main(int argc, char **argv) {
  if (argc < 2)
    return usage();
  const std::string cmd = argv[1];
  if (cmd == "bistro")
    return cmdBistro(argc, argv);
  if (cmd == "diag")
    return cmdDiag(argc, argv);
  if (cmd == "info")
    return cmdInfo(argc, argv);
  return usage();
}
