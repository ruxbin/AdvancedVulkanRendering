#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// .vlm 资产(规格 §9):小端逐字段写,不直接序列化 C++ struct。
// 编码 IrradiancePolynomialSH9_v1:物理辐照度多项式系数(A_l 与基常数已折入),
// 与 ComputeSH9FromEquirect 输出同语义;禁止按 Apple compatibility SH 解读。
inline constexpr uint32_t kVlmMagic = 0x314D4C56u; // 'VLM1' little-endian
inline constexpr uint32_t kVlmVersion = 1;
inline constexpr uint32_t kVlmEndianMarker = 0x01020304u;
inline constexpr uint32_t kVlmEncodingIrradiancePolynomialSH9V1 = 1;
inline constexpr uint32_t kVlmLayoutUniform = 1;
inline constexpr uint32_t kVlmIntegratorVersion = 3; // BSDF mixture + terminal segment + primary environment mixture

struct VlmAssetData {
  uint64_t sceneHash = 0, envHash = 0, lightHash = 0, settingsHash = 0;
  float bmin[3] = {};
  float step[3] = {};
  uint32_t cells[3] = {};
  float bandWidth = 0.0f;
  float unitScale = 1.0f;
  uint32_t samplesPerProbe = 0;
  uint32_t integratorVersion = kVlmIntegratorVersion;
  uint32_t flags = 0;
  std::vector<uint8_t> validity; // probeCount 字节(阶段 1 全 1)
  std::vector<uint16_t> shFp16;  // probeCount × 28

  uint64_t ProbeCount() const {
    for (auto c : cells) if (!c || c > 4096) return 0;
    return (uint64_t(cells[0]) + 1) * (uint64_t(cells[1]) + 1) * (uint64_t(cells[2]) + 1);
  }
};

bool VlmSaveAsset(const std::string& path, const VlmAssetData& asset);
bool VlmValidateAsset(const VlmAssetData& asset);

enum class VlmLoadResult { Ok, Stale, Rejected };
VlmLoadResult VlmLoadAsset(const std::string& path, VlmAssetData& out,
                           uint64_t expectedSceneHash, std::string& err);

uint32_t VlmCrc32(const void* data, size_t size);
inline constexpr uint64_t kVlmFnv1aBasis = 1469598103934665603ull;
uint64_t VlmFnv1a64(const void* data, size_t size, uint64_t seed);
// Streaming content identity; missing/unreadable input must never look like a valid snapshot.
bool VlmHashFile(const std::string& path, uint64_t seed, uint64_t& hash);
