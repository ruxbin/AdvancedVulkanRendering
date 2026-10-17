#include "TextureBlobReader.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <thread>

namespace {
int failures = 0;
void expect(bool condition, const char *message) {
  if (!condition) {
    std::fprintf(stderr, "FAIL %s\n", message);
    ++failures;
  }
}

struct Fixture {
  std::filesystem::path directory;
  std::filesystem::path path;
  Fixture() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    for (unsigned attempt = 0; attempt < 100; ++attempt) {
      directory = std::filesystem::temp_directory_path() /
                  ("texture-blob-reader-" + std::to_string(stamp) + "-" +
                   std::to_string(attempt));
      if (std::filesystem::create_directory(directory)) {
        path = directory / "blob.bin";
        std::ofstream file(path, std::ios::binary);
        const char bytes[] = {90, 91, 10, 11, 12, 13, 14, 15, 92, 93};
        file.write(bytes, sizeof(bytes));
        file.close();
        if (!file)
          throw std::runtime_error("failed to write test fixture");
        return;
      }
    }
    throw std::runtime_error("failed to create unique fixture directory");
  }
  ~Fixture() {
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
  }
};

void expectFailure(const std::filesystem::path &path, uint64_t base,
                   uint64_t blobLength, uint64_t offset, uint64_t length,
                   const char *message) {
  std::vector<uint8_t> bytes{99};
  std::string error;
  expect(!svt::readTextureBlobRange(path, base, blobLength, offset, length,
                                  bytes, &error), message);
  expect(bytes.empty(), "failed read clears prior output");
  expect(!error.empty(), "failed read reports a reason");
}

void testRanges(const Fixture &fixture) {
  std::vector<uint8_t> bytes{99};
  std::string error = "stale error";
  expect(svt::readTextureBlobRange(fixture.path, 2, 6, 0, 6, bytes, &error),
         "read complete declared blob");
  expect(bytes == std::vector<uint8_t>({10, 11, 12, 13, 14, 15}),
         "complete blob excludes prefix and suffix");
  expect(error.empty(), "success clears prior error");
  expect(svt::readTextureBlobRange(fixture.path, 2, 6, 1, 3, bytes),
         "read interior subrange without error output");
  expect(bytes == std::vector<uint8_t>({11, 12, 13}), "subrange bytes match");
  expect(svt::readTextureBlobRange(fixture.path, 2, 6, 5, 1, bytes),
         "read ending exactly at blob boundary");
  expect(bytes == std::vector<uint8_t>({15}), "last blob byte matches");
  expect(svt::readTextureBlobRange(fixture.path, 2, 6, 6, 0, bytes),
         "empty range at blob end is valid");
  expect(bytes.empty(), "empty read replaces prior output");
  expect(svt::readTextureBlobRange(fixture.path, 10, 0, 0, 0, bytes),
         "empty blob at file end is valid");
  expect(svt::readTextureBlobRange(fixture.path, 0, 10, 9, 1, bytes),
         "read ending exactly at file boundary");
  expect(bytes == std::vector<uint8_t>({93}), "last file byte matches");
}

void testBounds(const Fixture &fixture) {
  constexpr auto maximum = std::numeric_limits<uint64_t>::max();
  expectFailure(fixture.path, 2, 6, 7, 0, "empty range beyond blob rejected");
  expectFailure(fixture.path, 2, 6, 6, 1, "range crosses blob end rejected");
  expectFailure(fixture.path, 2, 6, 5, maximum, "range length overflow rejected");
  expectFailure(fixture.path, 2, 6, maximum, 2, "offset overflow rejected");
  expectFailure(fixture.path, maximum, 2, 0, 1, "blob end overflow rejected");
  expectFailure(fixture.path, maximum, 0, 0, 0, "unseekable blob base rejected");
  expectFailure(fixture.path, 3, maximum, maximum, 1,
                "absolute read offset overflow rejected");
  expectFailure(fixture.path, 11, 0, 0, 0, "blob starts beyond file rejected");
  expectFailure(fixture.path, 2, 9, 0, 1,
                "truncated blob rejected even when requested bytes exist");
  expectFailure(fixture.path, 2, 9, 8, 1, "read beyond physical file rejected");
  expectFailure(fixture.directory / "missing.bin", 0, 0, 0, 0,
                "missing file rejected even for empty range");
  std::vector<uint8_t> bytes{99};
  expect(!svt::readTextureBlobRange(fixture.path, 2, 6, 7, 0, bytes),
         "failure tolerates omitted error output");
  expect(bytes.empty(), "failure without error output clears bytes");
}

void testConcurrentReads(const Fixture &fixture) {
  std::atomic<unsigned> ready{0};
  std::atomic<bool> start{false};
  std::atomic<bool> valid{true};
  auto read = [&](uint64_t offset, std::vector<uint8_t> expected) {
    ready.fetch_add(1);
    while (!start.load())
      std::this_thread::yield();
    for (int iteration = 0; iteration < 200; ++iteration) {
      std::vector<uint8_t> bytes;
      if (!svt::readTextureBlobRange(fixture.path, 2, 6, offset,
                                     expected.size(), bytes) || bytes != expected)
        valid.store(false);
    }
  };
  std::thread first(read, 0, std::vector<uint8_t>{10, 11, 12});
  std::thread second(read, 3, std::vector<uint8_t>{13, 14, 15});
  while (ready.load() != 2)
    std::this_thread::yield();
  start.store(true);
  first.join();
  second.join();
  expect(valid.load(), "concurrent readers have independent file positions");
}
} // namespace

int main() {
  try {
    const Fixture fixture;
    testRanges(fixture);
    testBounds(fixture);
    testConcurrentReads(fixture);
  } catch (const std::exception &error) {
    std::fprintf(stderr, "FAIL fixture: %s\n", error.what());
    return 1;
  }
  if (failures)
    return 1;
  std::puts("TextureBlobReaderTests passed");
  return 0;
}
