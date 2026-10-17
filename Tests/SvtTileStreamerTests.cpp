// SVT tile 流送器测试（设计 §9、§13.1）。
// 覆盖：基本读取（payload 与 SvtAsset 直读一致）、有界队列背压、
// 资产取消（排队任务丢弃、在途任务 generation 丢弃）、索引重用后旧结果
// 全部丢弃、失败任务（损坏文件/CRC）报告、多 worker 并发正确性。

#include "SvtAsset.h"
#include "SvtCooker.h"
#include "SvtSystem.h"
#include "SvtTileStreamer.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>

using namespace svt;

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

// 等待 streamer 产出恰好 n 个结果（带超时）。
bool waitForResults(SvtTileStreamer &streamer,
                    std::vector<SvtTileReadResult> &out, size_t n,
                    int timeoutMs = 10000) {
  out.clear();
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(timeoutMs);
  while (out.size() < n) {
    std::vector<SvtTileReadResult> batch;
    streamer.drainCompleted(batch);
    out.insert(out.end(), std::make_move_iterator(batch.begin()),
               std::make_move_iterator(batch.end()));
    if (std::chrono::steady_clock::now() > deadline)
      return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  return true;
}

// cook 一个 2048x2048 BC1 测试资产（340 tiles）并返回路径。
std::filesystem::path cookTestAsset(const char *name, uint64_t seed) {
  SvtSourceTexture src = svtMakeNoiseBcTexture(SvtFormat::BC1, 2048, 2048, seed, true);
  SvtCookParams params;
  const std::filesystem::path path = name;
  std::string error;
  if (!svtCookTexture(src, params, path, nullptr, &error)) {
    std::fprintf(stderr, "FAIL cook %s: %s\n", name, error.c_str());
    ++failures;
    return {};
  }
  return path;
}

void TestBasicRead() {
  const std::filesystem::path path = cookTestAsset("streamer_basic.svt", 42);
  if (failures)
    return;

  SvtTileStreamer streamer(2);
  streamer.registerAsset(0, path);

  // 提交 4 个 tile（mip0 各角 + mip1）。
  SvtTileKey k0{0, 0, 0, 0, 0};
  SvtTileKey k1{0, 0, 0, 15, 7};
  SvtTileKey k2{0, 0, 0, 8, 4};
  SvtTileKey k3{0, 0, 1, 0, 0};
  expectTrue(streamer.submitRead(k0), "submit k0");
  expectTrue(streamer.submitRead(k1), "submit k1");
  expectTrue(streamer.submitRead(k2), "submit k2");
  expectTrue(streamer.submitRead(k3), "submit k3");

  std::vector<SvtTileReadResult> results;
  expectTrue(waitForResults(streamer, results, 4), "4 results within timeout");
  expectEqU64(results.size(), 4, "exactly 4 results");
  size_t success = 0;
  for (const auto &r : results)
    if (r.success)
      ++success;
  expectEqU64(success, 4, "all 4 succeeded");

  // payload 与 SvtAsset 直读一致（不可变数据正确性）。
  SvtAsset ref;
  expectTrue(ref.open(path), "open reference");
  std::vector<uint8_t> expected;
  expectTrue(ref.readTile(0, 15, 7, expected), "reference read");
  for (const auto &r : results) {
    if (r.key == k1) {
      expectTrue(r.payload == expected, "payload matches direct read");
      expectEqU64(r.payload.size(), 34u * 34 * 8, "BC1 payload size");
    }
  }
  expectEqU64(streamer.totalReadCount(), 4, "read count 4");
  expectEqU64(streamer.totalFailCount(), 0, "no failures");
  streamer.shutdown();
}

void TestQueueBackpressure() {
  const std::filesystem::path path = cookTestAsset("streamer_bp.svt", 7);
  if (failures)
    return;

  SvtTileStreamer streamer(1, 4 /*队列容量 4*/);
  streamer.registerAsset(0, path);

  // 提交 10 个：队列容量 4，超出的应被拒（背压）。
  size_t accepted = 0;
  for (uint32_t i = 0; i < 10; ++i) {
    SvtTileKey k{0, 0, 0, i % 16, i / 16};
    if (streamer.submitRead(k))
      ++accepted;
  }
  expectEqU64(accepted, 4, "queued, reading and completed share four credits");
  expectTrue(streamer.queuedCount() <= 4, "queue within capacity");

  // 等待全部完成（容量内的都应完成）。
  std::vector<SvtTileReadResult> results;
  expectTrue(waitForResults(streamer, results, accepted),
             "accepted tasks complete");
  streamer.shutdown();
}

void TestCancelAndGeneration() {
  const std::filesystem::path path = cookTestAsset("streamer_cancel.svt", 9);
  if (failures)
    return;

  SvtTileStreamer streamer(1);
  streamer.registerAsset(0, path);

  // 提交后立即取消：排队任务应被丢弃。
  for (uint32_t i = 0; i < 8; ++i) {
    SvtTileKey k{0, 0, 0, i % 16, i / 16};
    streamer.submitRead(k);
  }
  streamer.cancelAsset(0, 1 /*generation 已递增到 1*/);
  expectEqU64(streamer.queuedCount(), 0, "queue drained by cancel");

  // Cancellation removes old results rather than delivering stale failures.
  std::vector<SvtTileReadResult> results;
  streamer.drainCompleted(results);
  expectTrue(results.empty(), "cancelled results discarded");

  streamer.registerAsset(0, path, 1);
  SvtTileKey staleKey{0, 0, 0, 0, 0};
  expectTrue(!streamer.submitRead(staleKey), "stale generation rejected");
  SvtTileKey freshKey{0, 1, 0, 0, 0};
  expectTrue(streamer.submitRead(freshKey), "fresh submit accepted");
  std::vector<SvtTileReadResult> results2;
  expectTrue(waitForResults(streamer, results2, 1), "fresh result arrives");
  size_t freshOk = 0;
  for (const auto &r : results2) {
    expectTrue(r.key.generation == 1, "only current generation delivered");
    if (r.key == freshKey && r.success)
      ++freshOk;
  }
  expectEqU64(freshOk, 1, "fresh key reads fine after re-register");
  streamer.cancelAsset(0, 2);
  streamer.registerAsset(0, path);
  expectTrue(!streamer.submitRead(freshKey), "default registration preserves generation");
  expectTrue(streamer.submitRead({0, 2, 0, 0, 0}), "preserved generation accepted");
  expectTrue(waitForResults(streamer, results2, 1), "preserved generation completes");
  streamer.shutdown();
}

void TestCorruptFileFails() {
  const std::filesystem::path path = cookTestAsset("streamer_corrupt.svt", 11);
  if (failures)
    return;

  // 破坏一个 tile 的 CRC（tile 目录 offset + 20 处）。
  {
    FILE *f =
#ifdef _WIN32
        nullptr;
    _wfopen_s(&f, path.wstring().c_str(), L"r+b");
#else
        fopen(path.string().c_str(), "r+b");
#endif
    expectTrue(f != nullptr, "open for corruption");
    if (f) {
      // 复制 SvtAsset 的解析：header offset 72 = tileDirOffset。
      uint8_t header[120];
      fseek(f, 0, SEEK_SET);
      fread(header, 1, 120, f);
      uint64_t tileDirOffset = 0;
      for (int i = 7; i >= 0; --i)
        tileDirOffset = (tileDirOffset << 8) | header[72 + i];
      // 第一个 tile 目录项 +20 是 crc32。
      fseek(f, static_cast<long long>(tileDirOffset) + 20, SEEK_SET);
      uint8_t zero[4] = {0, 0, 0, 0};
      fwrite(zero, 1, 4, f);
      fclose(f);
    }
  }

  SvtTileStreamer streamer(1);
  streamer.registerAsset(0, path);
  SvtTileKey k{0, 0, 0, 0, 0};
  streamer.submitRead(k);

  std::vector<SvtTileReadResult> results;
  waitForResults(streamer, results, 1, 5000);
  expectEqU64(results.size(), 1, "one result");
  if (!results.empty()) {
    expectTrue(!results[0].success, "CRC corruption reported as failure");
    expectTrue(!results[0].error.empty(), "failure carries error message");
  }
  expectEqU64(streamer.totalFailCount(), 1, "fail count 1");
  streamer.shutdown();
}

void TestConcurrentWorkers() {
  const std::filesystem::path path = cookTestAsset("streamer_multi.svt", 13);
  if (failures)
    return;

  SvtTileStreamer streamer(4);
  streamer.registerAsset(0, path);

  // 提交 64 个 tile，4 worker 并发；全部应成功且 payload 一致。
  const uint32_t N = 64;
  for (uint32_t i = 0; i < N; ++i) {
    SvtTileKey k{0, 0, 0, i % 16, i / 16};
    if (!streamer.submitRead(k))
      break; // 队列满（容量 1024，不会）
  }
  std::vector<SvtTileReadResult> results;
  expectTrue(waitForResults(streamer, results, N), "all results in time");
  expectEqU64(results.size(), N, "exactly N results");

  // 对照直读。
  SvtAsset ref;
  ref.open(path);
  size_t verified = 0;
  for (const auto &r : results) {
    if (!r.success)
      continue;
    std::vector<uint8_t> expected;
    if (ref.readTile(r.key.mip, r.key.x, r.key.y, expected) &&
        r.payload == expected)
      ++verified;
  }
  expectEqU64(verified, N, "all payloads verified against direct read");
  expectEqU64(streamer.totalReadCount(), N, "read count N");
  streamer.shutdown();
}

void TestDuplicateSubmits() {
  // A key retains its credit until its result is drained.
  const std::filesystem::path path = cookTestAsset("streamer_dup.svt", 15);
  if (failures)
    return;
  SvtTileStreamer streamer(2);
  streamer.registerAsset(0, path);
  SvtTileKey k{0, 0, 0, 3, 3};
  expectTrue(streamer.submitRead(k), "first submit accepted");
  expectTrue(!streamer.submitRead(k), "duplicate outstanding key rejected");
  std::vector<SvtTileReadResult> results;
  expectTrue(waitForResults(streamer, results, 1), "one read completes");
  expectEqU64(results.size(), 1, "duplicate does not create work");
  expectTrue(streamer.submitRead(k), "draining releases duplicate key");
  expectTrue(waitForResults(streamer, results, 1), "resubmission completes");
  streamer.shutdown();
  expectTrue(!streamer.submitRead(k), "shutdown rejects work");
}

void TestUndrainedResultsBounded() {
  const auto path = cookTestAsset("streamer_bounded.svt", 19);
  SvtTileStreamer streamer(1, 2);
  streamer.registerAsset(0, path);
  expectTrue(streamer.submitRead({0,0,0,0,0}), "first credit");
  expectTrue(streamer.submitRead({0,0,0,1,0}), "second credit");
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  expectTrue(!streamer.submitRead({0,0,0,2,0}), "undrained results retain credits");
  streamer.cancelAsset(0,1);
  std::vector<SvtTileReadResult> out;
  streamer.drainCompleted(out);
  expectTrue(out.empty(), "cancelled generation not delivered");
}

} // namespace

int main() {
  const struct {
    const char *name;
    void (*fn)();
  } tests[] = {
      {"BasicRead", TestBasicRead},
      {"QueueBackpressure", TestQueueBackpressure},
      {"CancelAndGeneration", TestCancelAndGeneration},
      {"CorruptFileFails", TestCorruptFileFails},
      {"ConcurrentWorkers", TestConcurrentWorkers},
      {"DuplicateSubmits", TestDuplicateSubmits},
      {"UndrainedResultsBounded", TestUndrainedResultsBounded},
  };
  for (const auto &t : tests) {
    std::fprintf(stderr, "[run] %s\n", t.name);
    std::fflush(stderr);
    t.fn();
  }
  if (failures == 0)
    std::printf("SvtTileStreamerTests: all passed\n");
  else
    std::printf("SvtTileStreamerTests: %d failures\n", failures);
  return failures == 0 ? 0 : 1;
}
