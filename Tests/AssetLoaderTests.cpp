#include "AssetLoader.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

int main() {
  const auto path = std::filesystem::temp_directory_path() / "avr-asset-reader-test.bin";
  { std::ofstream file(path, std::ios::binary); file << "abcd"; }
  int failures = 0;
  {
    AssetLoader::BinaryFileReader reader(path.string().c_str());
    char bytes[5]{};
    if (reader.remaining() != 4) ++failures;
    reader.readExact(bytes, 3);
    if (std::string(bytes) != "abc" || reader.remaining() != 1) ++failures;
    try { reader.readExact(bytes, 2); ++failures; }
    catch (const std::runtime_error&) {}
    // Bounds failures must leave the stream at its original position.
    if (reader.remaining() != 1) ++failures;
    reader.readExact(bytes, 1);
    if (bytes[0] != 'd' || reader.remaining() != 0) ++failures;
  }
  std::filesystem::remove(path);
  AssetLoader::BinaryFileReader missing(path.string().c_str());
  try { char b; missing.readExact(&b, 1); ++failures; }
  catch (const std::runtime_error&) {}
  if (failures) std::cerr << failures << " asset reader failures\n";
  return failures ? 1 : 0;
}
