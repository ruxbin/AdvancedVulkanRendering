#include "VlmAsset.h"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <vector>

namespace {

int failures = 0;

void expectTrue(bool cond, const char* message) {
  if (!cond) { std::fprintf(stderr, "FAIL %s\n", message); ++failures; }
}

VlmAssetData makeAsset() {
  VlmAssetData a;
  a.sceneHash = 111; a.envHash = 222; a.lightHash = 333; a.settingsHash = 444;
  a.bmin[0] = -1; a.bmin[1] = -2; a.bmin[2] = -3;
  a.step[0] = 1.0f; a.step[1] = 1.0f; a.step[2] = 1.0f;
  a.cells[0] = 1; a.cells[1] = 2; a.cells[2] = 3; // probes = 2*3*4 = 24
  a.bandWidth = 1.0f; a.unitScale = 1.0f;
  a.samplesPerProbe = 2048; a.integratorVersion = 1; a.flags = 0;
  const size_t probes = (size_t)a.ProbeCount();
  a.validity.assign(probes, 1);
  a.shFp16.resize(probes * 28);
  for (size_t i = 0; i < a.shFp16.size(); ++i) a.shFp16[i] = (uint16_t)(i * 37 % 65536);
  return a;
}

std::vector<uint8_t> readFile(const char* path) {
  std::ifstream f(path, std::ios::binary);
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(f), {});
}

bool assetEqual(const VlmAssetData& a, const VlmAssetData& b) {
  if (a.sceneHash != b.sceneHash || a.envHash != b.envHash ||
      a.lightHash != b.lightHash || a.settingsHash != b.settingsHash) return false;
  for (int i = 0; i < 3; ++i) {
    if (a.bmin[i] != b.bmin[i] || a.step[i] != b.step[i] || a.cells[i] != b.cells[i]) return false;
  }
  return a.bandWidth == b.bandWidth && a.unitScale == b.unitScale &&
         a.samplesPerProbe == b.samplesPerProbe && a.flags == b.flags &&
         a.validity == b.validity && a.shFp16 == b.shFp16;
}

void rewriteFile(const char* path, std::vector<uint8_t> bytes) {
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  f.write((const char*)bytes.data(), (std::streamsize)bytes.size());
}

void TestRoundTrip() {
  const char* path = "vlm_test_roundtrip.vlm";
  VlmAssetData a = makeAsset();
  expectTrue(VlmSaveAsset(path, a), "save ok");
  expectTrue(std::ifstream(path).good(), "file exists");
  expectTrue(!std::ifstream(std::string(path) + ".tmp").good(), "tmp file cleaned");

  VlmAssetData b;
  std::string err;
  expectTrue(VlmLoadAsset(path, b, 111, err) == VlmLoadResult::Ok, "load ok");
  expectTrue(assetEqual(a, b), "round trip equal");
  std::remove(path);
}

void TestStaleOnHashMismatch() {
  const char* path = "vlm_test_stale.vlm";
  expectTrue(VlmSaveAsset(path, makeAsset()), "save ok");
  VlmAssetData b;
  std::string err;
  expectTrue(VlmLoadAsset(path, b, 999, err) == VlmLoadResult::Stale, "hash mismatch -> Stale");
  expectTrue(b.cells[1] == 2, "Stale still fills data");
  std::remove(path);
}

void TestRejectCorruptions() {
  const char* path = "vlm_test_corrupt.vlm";
  expectTrue(VlmSaveAsset(path, makeAsset()), "save ok");
  const std::vector<uint8_t> good = readFile(path);
  VlmAssetData b;
  std::string err;

  auto expectReject = [&](std::vector<uint8_t> bytes, const char* what) {
    rewriteFile(path, bytes);
    err.clear();
    if (VlmLoadAsset(path, b, 111, err) != VlmLoadResult::Rejected) {
      std::fprintf(stderr, "FAIL corruption not rejected: %s\n", what);
      ++failures;
    }
  };

  { auto v = good; v[0] ^= 0xFF; expectReject(v, "bad magic"); }
  { auto v = good; v[4] = 99; expectReject(v, "bad version"); }
  { auto v = good; v[12] = 7; expectReject(v, "unknown encoding"); }
  { auto v = good; v[16] = 9; expectReject(v, "unknown layout"); }
  { auto v = good; v.resize(good.size() - 10); expectReject(v, "truncated"); }
  { auto v = good; v.back() ^= 0x01; expectReject(v, "crc mismatch"); }
  { auto v = good; v[104] = 0; expectReject(v, "cells[0]=0 rejected"); }
  { // bmin 改 NaN 后必须重算 crc 才能走到 NaN 检查:直接改前 64 字节外的 bmin 区,
    // 但 crc 会失配——所以本用例验证的是"crc 先挡下";NaN 专项在 save 侧不负责,
    // 加载侧对通过 crc 的数据仍逐字段查有限性,此处用合法 crc 的脏数据:
    auto v = good;
    float nan = NAN;
    for (int i = 0; i < 4; ++i) v[80 + i] = ((const uint8_t*)&nan)[i];
    // 重算 crc 使数据"形式合法":
    uint32_t crc = VlmCrc32(v.data() + 64, v.size() - 64);
    for (int i = 0; i < 4; ++i) v[32 + i] = ((const uint8_t*)&crc)[i];
    expectReject(v, "NaN bmin rejected");
  }
  std::remove(path);
}

void TestMissingFile() {
  VlmAssetData b;
  std::string err;
  expectTrue(VlmLoadAsset("vlm_test_does_not_exist.vlm", b, 0, err) == VlmLoadResult::Rejected,
             "missing file rejected");
  expectTrue(!err.empty(), "error message filled");
}

} // namespace

int main() {
  TestRoundTrip();
  TestStaleOnHashMismatch();
  TestRejectCorruptions();
  TestMissingFile();
  if (failures == 0) std::printf("all VlmAsset tests passed\n");
  return failures == 0 ? 0 : 1;
}
