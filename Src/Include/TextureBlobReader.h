#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace svt {

// Reads exactly [relativeOffset, relativeOffset + length) from a declared blob.
// The complete blob must fit the file; empty ranges at the blob end are valid.
// Each call owns its file handle, so concurrent calls may use the same path.
// Success replaces output and clears error; failure clears output and sets error.
bool readTextureBlobRange(const std::filesystem::path &path, uint64_t blobBase,
                          uint64_t blobLength, uint64_t relativeOffset,
                          uint64_t length, std::vector<uint8_t> &output,
                          std::string *error = nullptr);

} // namespace svt
