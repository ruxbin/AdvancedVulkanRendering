#include "VlmAsset.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>

#ifdef _WIN32
#include <windows.h>
#else
#include <filesystem>
#endif

namespace {

void putU32(std::vector<uint8_t>& o, uint32_t v) {
  o.push_back((uint8_t)(v & 0xFF));
  o.push_back((uint8_t)((v >> 8) & 0xFF));
  o.push_back((uint8_t)((v >> 16) & 0xFF));
  o.push_back((uint8_t)((v >> 24) & 0xFF));
}
void putU64(std::vector<uint8_t>& o, uint64_t v) {
  for (int i = 0; i < 8; ++i) o.push_back((uint8_t)((v >> (i * 8)) & 0xFF));
}
void putF32(std::vector<uint8_t>& o, float f) {
  uint32_t v;
  static_assert(sizeof(v) == sizeof(f));
  std::memcpy(&v, &f, 4);
  putU32(o, v);
}

struct Reader {
  const uint8_t* p;
  size_t size;
  size_t pos = 0;
  bool ok = true;
  uint32_t u32() {
    if (pos + 4 > size) { ok = false; return 0; }
    uint32_t v = (uint32_t)p[pos] | ((uint32_t)p[pos + 1] << 8) |
                 ((uint32_t)p[pos + 2] << 16) | ((uint32_t)p[pos + 3] << 24);
    pos += 4;
    return v;
  }
  uint64_t u64() {
    uint64_t lo = u32();
    uint64_t hi = u32();
    return lo | (hi << 32);
  }
  float f32() {
    uint32_t v = u32();
    float f;
    std::memcpy(&f, &v, 4);
    return f;
  }
};

bool writeFileAtomic(const std::string& path, const std::vector<uint8_t>& bytes) {
  const std::string tmp = path + ".tmp";
  {
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    f.write((const char*)bytes.data(), (std::streamsize)bytes.size());
    if (!f.good()) { std::remove(tmp.c_str()); return false; }
  }
#ifdef _WIN32
  // Windows:rename 不覆盖已存在目标,用 MoveFileExA 保证原子替换。
  if (MoveFileExA(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0) {
    std::remove(tmp.c_str());
    return false;
  }
  return true;
#else
  std::error_code ec;
  std::filesystem::rename(tmp, path, ec);
  if (ec) { std::remove(tmp.c_str()); return false; }
  return true;
#endif
}

} // namespace

uint32_t VlmCrc32(const void* data, size_t size) {
  static uint32_t table[256];
  static bool init = false;
  if (!init) {
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
      table[i] = c;
    }
    init = true;
  }
  uint32_t crc = 0xFFFFFFFFu;
  const uint8_t* b = (const uint8_t*)data;
  for (size_t i = 0; i < size; ++i) crc = table[(crc ^ b[i]) & 0xFF] ^ (crc >> 8);
  return crc ^ 0xFFFFFFFFu;
}

uint64_t VlmFnv1a64(const void* data, size_t size, uint64_t seed) {
  uint64_t h = seed;
  const uint8_t* b = (const uint8_t*)data;
  for (size_t i = 0; i < size; ++i) { h ^= b[i]; h *= 1099511628211ull; }
  return h;
}

bool VlmSaveAsset(const std::string& path, const VlmAssetData& a) {
  const uint64_t probes = a.ProbeCount();
  if (probes == 0 || a.validity.size() != probes || a.shFp16.size() != probes * 28) return false;

  std::vector<uint8_t> o;
  o.reserve(132 + (size_t)probes * 57);
  // 头部 [0,64):先占位,CRC 后补。
  putU32(o, kVlmMagic);
  putU32(o, kVlmVersion);
  putU32(o, kVlmEndianMarker);
  putU32(o, kVlmEncodingIrradiancePolynomialSH9V1);
  putU32(o, kVlmLayoutUniform);
  putU32(o, 0);                    // headerFlags
  putU64(o, 0);                    // totalLength 占位
  putU32(o, 0);                    // crc32 占位
  putU32(o, (uint32_t)probes);
  putU64(o, a.sceneHash);
  putU64(o, a.envHash);
  putU64(o, a.lightHash);
  // [64, ...)
  putU64(o, a.settingsHash);
  putU32(o, a.integratorVersion);
  putU32(o, a.samplesPerProbe);
  for (int i = 0; i < 3; ++i) putF32(o, a.bmin[i]);
  for (int i = 0; i < 3; ++i) putF32(o, a.step[i]);
  for (int i = 0; i < 3; ++i) putU32(o, a.cells[i]);
  putF32(o, a.bandWidth);
  putF32(o, a.unitScale);
  putU32(o, 0); // diagFlags:v1 恒 0
  putU32(o, 0); // reserved
  for (uint8_t v : a.validity) o.push_back(v);
  for (uint16_t v : a.shFp16) {
    o.push_back((uint8_t)(v & 0xFF));
    o.push_back((uint8_t)((v >> 8) & 0xFF));
  }

  // 回填 totalLength 与 crc32(覆盖 [64, end))。
  const uint64_t total = o.size();
  for (int i = 0; i < 8; ++i) o[24 + i] = (uint8_t)((total >> (i * 8)) & 0xFF);
  const uint32_t crc = VlmCrc32(o.data() + 64, o.size() - 64);
  for (int i = 0; i < 4; ++i) o[32 + i] = (uint8_t)((crc >> (i * 8)) & 0xFF);

  return writeFileAtomic(path, o);
}

VlmLoadResult VlmLoadAsset(const std::string& path, VlmAssetData& out,
                           uint64_t expectedSceneHash, std::string& err) {
  auto reject = [&](const char* what) {
    err = std::string("vlm load rejected: ") + what;
    return VlmLoadResult::Rejected;
  };

  std::ifstream f(path, std::ios::binary);
  if (!f.good()) return reject("file not readable");
  std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), {});
  if (bytes.size() < 132) return reject("file too small");

  Reader r{bytes.data(), bytes.size()};
  if (r.u32() != kVlmMagic) return reject("bad magic");
  if (r.u32() != kVlmVersion) return reject("unsupported version");
  if (r.u32() != kVlmEndianMarker) return reject("endian marker mismatch");
  if (r.u32() != kVlmEncodingIrradiancePolynomialSH9V1) return reject("unknown encoding");
  if (r.u32() != kVlmLayoutUniform) return reject("unknown layout type");
  if (r.u32() != 0) return reject("unknown header flags");
  const uint64_t totalLength = r.u64();
  const uint32_t crc = r.u32();
  const uint32_t probeCount = r.u32();
  out.sceneHash = r.u64();
  out.envHash = r.u64();
  out.lightHash = r.u64();
  out.settingsHash = r.u64();
  out.integratorVersion = r.u32();
  out.samplesPerProbe = r.u32();
  for (int i = 0; i < 3; ++i) out.bmin[i] = r.f32();
  for (int i = 0; i < 3; ++i) out.step[i] = r.f32();
  for (int i = 0; i < 3; ++i) out.cells[i] = r.u32();
  out.bandWidth = r.f32();
  out.unitScale = r.f32();
  const uint32_t diagFlags = r.u32();
  r.u32(); // reserved
  if (!r.ok) return reject("header truncated");

  if (totalLength != bytes.size()) return reject("totalLength != file size");
  if (VlmCrc32(bytes.data() + 64, bytes.size() - 64) != crc) return reject("crc32 mismatch");
  if (probeCount == 0 || probeCount != out.ProbeCount()) return reject("probeCount mismatch");
  for (int i = 0; i < 3; ++i)
    if (out.cells[i] < 1 || out.cells[i] > 4096) return reject("cells out of range");
  for (int i = 0; i < 3; ++i)
    if (!std::isfinite(out.bmin[i]) || !std::isfinite(out.step[i]) || out.step[i] <= 0.0f)
      return reject("non-finite or non-positive volume params");
  if (!std::isfinite(out.bandWidth) || !std::isfinite(out.unitScale) || out.unitScale <= 0.0f)
    return reject("non-finite band/unit params");
  if (diagFlags != 0) return reject("diagFlags unsupported in v1");
  if (totalLength != 132ull + (uint64_t)probeCount * 57ull) return reject("chunk size mismatch");

  out.validity.assign(bytes.begin() + 132, bytes.begin() + 132 + probeCount);
  out.shFp16.resize((size_t)probeCount * 28);
  const uint8_t* sp = bytes.data() + 132 + probeCount;
  for (size_t i = 0; i < out.shFp16.size(); ++i)
    out.shFp16[i] = (uint16_t)sp[i * 2] | ((uint16_t)sp[i * 2 + 1] << 8);
  out.flags = 0;

  if (out.sceneHash != expectedSceneHash) {
    err = "vlm load: scene hash mismatch (stale asset)";
    return VlmLoadResult::Stale;
  }
  return VlmLoadResult::Ok;
}
