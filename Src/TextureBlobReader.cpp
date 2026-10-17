#include "TextureBlobReader.h"

#include <cstdio>
#include <limits>
#include <memory>
#include <new>
#include <stdexcept>

namespace svt {
namespace {
#ifdef _WIN32
using FileOffset = __int64;
int seekFile(FILE *file, FileOffset offset, int origin) {
  return _fseeki64(file, offset, origin);
}
FileOffset tellFile(FILE *file) { return _ftelli64(file); }
#else
using FileOffset = off_t;
static_assert(sizeof(FileOffset) >= sizeof(int64_t),
              "Texture blob reads require 64-bit file offsets");
int seekFile(FILE *file, FileOffset offset, int origin) {
  return fseeko(file, offset, origin);
}
FileOffset tellFile(FILE *file) { return ftello(file); }
#endif
} // namespace

bool readTextureBlobRange(const std::filesystem::path &path, uint64_t blobBase,
                          uint64_t blobLength, uint64_t relativeOffset,
                          uint64_t length, std::vector<uint8_t> &output,
                          std::string *error) {
  output.clear();
  if (error)
    error->clear();
  const auto fail = [&](const char *message) {
    output.clear();
    if (error) {
      // A diagnostic must not turn allocation failure into an exception.
      try {
        *error = message;
      } catch (const std::bad_alloc &) {
        error->clear();
      }
    }
    return false;
  };

  if (relativeOffset > blobLength || length > blobLength - relativeOffset)
    return fail("texture range exceeds declared blob");
  if (blobLength > std::numeric_limits<uint64_t>::max() - blobBase)
    return fail("texture blob end overflows file offset");
  const uint64_t blobEnd = blobBase + blobLength;
  if (blobEnd > static_cast<uint64_t>(std::numeric_limits<FileOffset>::max()))
    return fail("texture blob exceeds supported file offsets");
  if (length > std::numeric_limits<size_t>::max() || length > output.max_size())
    return fail("texture range exceeds addressable memory");

  FILE *opened = nullptr;
#ifdef _WIN32
  if (_wfopen_s(&opened, path.c_str(), L"rb") != 0)
    return fail("cannot open texture blob file");
#else
  opened = std::fopen(path.c_str(), "rb");
#endif
  if (!opened)
    return fail("cannot open texture blob file");
  std::unique_ptr<FILE, decltype(&std::fclose)> file(opened, &std::fclose);

  if (seekFile(file.get(), 0, SEEK_END) != 0)
    return fail("cannot seek to texture file end");
  const FileOffset fileLength = tellFile(file.get());
  if (fileLength < 0)
    return fail("cannot determine texture file length");
  if (blobEnd > static_cast<uint64_t>(fileLength))
    return fail("texture blob is truncated");
  // The checked blob end bounds this addition and its conversion to FileOffset.
  if (seekFile(file.get(), static_cast<FileOffset>(blobBase + relativeOffset),
               SEEK_SET) != 0)
    return fail("cannot seek to texture range");
  try {
    output.resize(static_cast<size_t>(length));
  } catch (const std::bad_alloc &) {
    return fail("cannot allocate texture range");
  } catch (const std::length_error &) {
    return fail("texture range exceeds vector capacity");
  }
  if (length != 0 &&
      std::fread(output.data(), 1, output.size(), file.get()) != output.size())
    return fail("texture range read is incomplete");
  return true;
}
} // namespace svt
