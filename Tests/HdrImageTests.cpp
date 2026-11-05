#define STB_IMAGE_IMPLEMENTATION
#include "HdrImage.h"
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>

int main() {
  const auto path = std::filesystem::temp_directory_path() / "avr-hdr-image-test.hdr";
  {
    std::ofstream file(path, std::ios::binary);
    file << "#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y 1 +X 1\n";
    const unsigned char rgbe[] = {128, 64, 32, 130}; // Linear HDR RGB = (2, 1, 0.5).
    file.write(reinterpret_cast<const char*>(rgbe), sizeof(rgbe));
  }
  int w = 0, h = 0, ch = 0;
  float* pixels = LoadHdrImage(path.string(), &w, &h, &ch);
  const bool valid = pixels && w == 1 && h == 1 &&
      std::abs(pixels[0] - 2.f) < 0.001f &&
      std::abs(pixels[1] - 1.f) < 0.001f &&
      std::abs(pixels[2] - 0.5f) < 0.001f && pixels[3] == 1.f;
  stbi_image_free(pixels);
  { std::ofstream file(path, std::ios::binary | std::ios::trunc); file << "invalid HDR"; }
  pixels = LoadHdrImage(path.string(), &w, &h, &ch);
  const bool invalidRejected = !pixels;
  stbi_image_free(pixels);
  std::filesystem::remove(path);
  pixels = LoadHdrImage(path.string(), &w, &h, &ch);
  const bool missingRejected = !pixels;
  stbi_image_free(pixels);
  if (!valid || !invalidRejected || !missingRejected) {
    std::cerr << "HDR asset loading/linear range/fallback regression\n";
    return 1;
  }
}
