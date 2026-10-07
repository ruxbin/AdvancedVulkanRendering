#include "PbrtExporter.h"
#include "GpuScene.h"
#include "ThirdParty/lzfse.h"
#include "spdlog/spdlog.h"
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iomanip>
#include <limits>
#include <unordered_set>

namespace {

constexpr int kPrecision = 6;

// Buffered output may only fail at flush/close. Check both before publishing
// a filename or reporting a successful scene export.
bool FinishOutputFile(std::ofstream& out) {
    out.flush();
    out.close();
    return out.good();
}

bool IsSupportedTexture(const AAPLTextureData& tex) {
    const bool supported = tex._pixelFormat == 131 || tex._pixelFormat == 135 ||
                           tex._pixelFormat == 142;
    // The decoders use uint32_t pixel/byte offsets and padded 4x4 blocks.
    const uint64_t maxDimension = (std::numeric_limits<int32_t>::max)() - 3ULL;
    if (!supported || tex._width == 0 || tex._height == 0 ||
        tex._width > maxDimension || tex._height > maxDimension)
        return false;
    const uint64_t blocks = ((tex._width + 3) / 4) * ((tex._height + 3) / 4);
    return blocks <= ((std::numeric_limits<uint32_t>::max)() - 54ULL) / 64;
}

// ---- ostream formatters ----

std::ostream& operator<<(std::ostream& os, const vec3& v) {
    os << std::fixed << std::setprecision(kPrecision);
    os << (std::isnan(v.x) ? 0.0f : (double)v.x) << ' '
       << (std::isnan(v.y) ? 0.0f : (double)v.y) << ' '
       << (std::isnan(v.z) ? 0.0f : (double)v.z);
    return os;
}

std::ostream& operator<<(std::ostream& os, const vec2& v) {
    os << std::fixed << std::setprecision(kPrecision);
    os << (std::isnan(v.x) ? 0.0f : (double)v.x) << ' '
       << (std::isnan(v.y) ? 0.0f : (double)v.y);
    return os;
}

template<typename T>
void WriteVec3Array(std::ofstream& out, const T* data, size_t count) {
    out << "[ ";
    for (size_t i = 0; i < count; ++i)
        out << data[i] << ' ';
    out << ']';
}

void WriteVec2Array(std::ofstream& out, const vec2* data, size_t count) {
    out << "[ ";
    for (size_t i = 0; i < count; ++i)
        out << data[i] << ' ';
    out << ']';
}

struct Indent {
    int level;
    explicit Indent(int n) : level(n) {}
    friend std::ostream& operator<<(std::ostream& os, const Indent& ind) {
        for (int i = 0; i < ind.level * 4; ++i) os.put(' ');
        return os;
    }
};

// ---- BCn decompression (BC1 / BC3 / BC5) ----

// MTLPixelFormat enum subset (matching GpuScene.cpp MTLPixelFormat enum)
enum : uint32_t {
    kBC1_RGBA_sRGB   = 131,
    kBC3_RGBA_sRGB   = 135,
    kBC5_RGUnorm     = 142,
};

inline uint16_t read16le(const uint8_t* p) { return (uint16_t)p[0] | ((uint16_t)p[1] << 8); }

// Decompress a single BC1 (DXT1) 4x4 block into 16 RGBA pixels.
// Output buffer must be at least 16 * 4 bytes.
// forceFourColor: always use the four-color interpolation mode, even when
// c0 <= c1 (required for BC3's color block, which has no transparent mode).
void DecodeBC1Block(const uint8_t* block, uint8_t* rgbaOut, bool forceFourColor = false) {
    uint16_t c0 = read16le(block);
    uint16_t c1 = read16le(block + 2);
    uint32_t bits = (uint32_t)block[4] | ((uint32_t)block[5] << 8) |
                    ((uint32_t)block[6] << 16) | ((uint32_t)block[7] << 24);

    // Extract 5-6-5 RGB
    uint8_t r0 = (uint8_t)(((c0 >> 11) & 0x1F) * 255 / 31);
    uint8_t g0 = (uint8_t)(((c0 >> 5)  & 0x3F) * 255 / 63);
    uint8_t b0 = (uint8_t)((c0         & 0x1F) * 255 / 31);
    uint8_t r1 = (uint8_t)(((c1 >> 11) & 0x1F) * 255 / 31);
    uint8_t g1 = (uint8_t)(((c1 >> 5)  & 0x3F) * 255 / 63);
    uint8_t b1 = (uint8_t)((c1         & 0x1F) * 255 / 31);

    for (int i = 0; i < 16; ++i) {
        uint8_t code = (uint8_t)((bits >> (i * 2)) & 3);
        uint8_t* dst = rgbaOut + i * 4;
        if (c0 > c1 || forceFourColor) {
            switch (code) {
            case 0: dst[0]=r0; dst[1]=g0; dst[2]=b0; dst[3]=255; break;
            case 1: dst[0]=r1; dst[1]=g1; dst[2]=b1; dst[3]=255; break;
            case 2: dst[0]=(uint8_t)((2*r0+r1)/3); dst[1]=(uint8_t)((2*g0+g1)/3); dst[2]=(uint8_t)((2*b0+b1)/3); dst[3]=255; break;
            case 3: dst[0]=(uint8_t)((r0+2*r1)/3); dst[1]=(uint8_t)((g0+2*g1)/3); dst[2]=(uint8_t)((b0+2*b1)/3); dst[3]=255; break;
            }
        } else {
            switch (code) {
            case 0: dst[0]=r0; dst[1]=g0; dst[2]=b0; dst[3]=255; break;
            case 1: dst[0]=r1; dst[1]=g1; dst[2]=b1; dst[3]=255; break;
            case 2: dst[0]=(uint8_t)((r0+r1)/2); dst[1]=(uint8_t)((g0+g1)/2); dst[2]=(uint8_t)((b0+b1)/2); dst[3]=255; break;
            case 3: dst[0]=0; dst[1]=0; dst[2]=0; dst[3]=0; break;
            }
        }
    }
}

// Decompress a single BC3 (DXT5) 4x4 block into 16 RGBA pixels.
void DecodeBC3Block(const uint8_t* block, uint8_t* rgbaOut) {
    // Alpha block (8 bytes): block[0..7]
    uint8_t a0 = block[0], a1 = block[1];
    // 48-bit alpha indices packed in block[2..7], 3 bits per pixel
    uint64_t aBits = 0;
    for (int i = 2; i < 8; ++i)
        aBits |= ((uint64_t)block[i]) << ((i - 2) * 8);

    uint8_t alphas[8];
    alphas[0] = a0;
    alphas[1] = a1;
    if (a0 > a1) {
        for (int i = 2; i < 8; ++i)
            alphas[i] = (uint8_t)(((8 - i) * a0 + (i - 1) * a1) / 7);
    } else {
        for (int i = 2; i < 6; ++i)
            alphas[i] = (uint8_t)(((6 - i) * a0 + (i - 1) * a1) / 5);
        alphas[6] = 0;
        alphas[7] = 255;
    }

    // Color block (8 bytes): block[8..15] — BC1 layout, but BC3 has no
    // transparent mode: always four-color interpolation (issue 5).
    uint8_t colorBlock[64]; // 16 pixels × 4 bytes RGBA
    DecodeBC1Block(block + 8, colorBlock, /*forceFourColor=*/true);

    for (int i = 0; i < 16; ++i) {
        uint8_t alphaIdx = (uint8_t)((aBits >> (i * 3)) & 7);
        uint8_t* dst = rgbaOut + i * 4;
        dst[0] = colorBlock[i*4];
        dst[1] = colorBlock[i*4+1];
        dst[2] = colorBlock[i*4+2];
        dst[3] = alphas[alphaIdx];
    }
}

// Decompress a single BC5 (3Dc) 4x4 block into 16 RG pixels (output as RG in RGBA).
// BC5 is two independent BC4 alpha blocks: red channel then green channel.
void DecodeBC4Block(const uint8_t* block, uint8_t* values) {
    uint8_t v0 = block[0], v1 = block[1];
    uint64_t bits = (uint64_t)read16le(block + 2) | ((uint64_t)read16le(block + 4) << 16) | ((uint64_t)read16le(block + 6) << 32);

    uint8_t palette[8];
    palette[0] = v0;
    palette[1] = v1;
    if (v0 > v1) {
        for (int i = 2; i < 8; ++i)
            palette[i] = (uint8_t)(((8 - i) * v0 + (i - 1) * v1) / 7);
    } else {
        for (int i = 2; i < 6; ++i)
            palette[i] = (uint8_t)(((6 - i) * v0 + (i - 1) * v1) / 5);
        palette[6] = 0;
        palette[7] = 255;
    }

    for (int i = 0; i < 16; ++i) {
        uint8_t idx = (uint8_t)((bits >> (i * 3)) & 7);
        values[i] = palette[idx];
    }
}

void DecodeBC5Block(const uint8_t* block, uint8_t* rgbaOut) {
    uint8_t r[16], g[16];
    DecodeBC4Block(block, r);
    DecodeBC4Block(block + 8, g);
    for (int i = 0; i < 16; ++i) {
        uint8_t* dst = rgbaOut + i * 4;
        dst[0] = r[i];
        dst[1] = g[i];
        // Reconstruct Z from XY: decode [0,255]→[-1,1], then Z=sqrt(1-X²-Y²)→[0,1]→[0,255]
        float x = r[i] / 127.5f - 1.0f;
        float y = g[i] / 127.5f - 1.0f;
        float z = std::sqrt((std::max)(0.0f, 1.0f - x*x - y*y));
        dst[2] = (uint8_t)((z * 0.5f + 0.5f) * 255.0f);
        dst[3] = 255;
    }
}

// ---- Float BCn decompression (linear PFM output path) ----
// The byte decoders above feed the BMP (color) path and stay byte-identical.
// Linear-data maps (normals, roughness, masks) need endpoint precision the
// byte rounding destroys (e.g. G=32/63 must stay 0.507937, not 129/255).

// sRGB EOTF; applied to RGB channels of sRGB-named source formats only
// (BC alpha channels are always linear).
float SRGBToLinear(float c) {
    return c <= 0.04045f ? c / 12.92f
                         : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

void DecodeBC1BlockFloat(const uint8_t* block, float* rgbaOut, bool forceFourColor) {
    uint16_t c0 = read16le(block);
    uint16_t c1 = read16le(block + 2);
    uint32_t bits = (uint32_t)block[4] | ((uint32_t)block[5] << 8) |
                    ((uint32_t)block[6] << 16) | ((uint32_t)block[7] << 24);

    float r0 = ((c0 >> 11) & 0x1F) / 31.0f;
    float g0 = ((c0 >> 5)  & 0x3F) / 63.0f;
    float b0 = (c0         & 0x1F) / 31.0f;
    float r1 = ((c1 >> 11) & 0x1F) / 31.0f;
    float g1 = ((c1 >> 5)  & 0x3F) / 63.0f;
    float b1 = (c1         & 0x1F) / 31.0f;

    for (int i = 0; i < 16; ++i) {
        uint8_t code = (uint8_t)((bits >> (i * 2)) & 3);
        float* dst = rgbaOut + i * 4;
        if (c0 > c1 || forceFourColor) {
            switch (code) {
            case 0: dst[0]=r0; dst[1]=g0; dst[2]=b0; dst[3]=1.0f; break;
            case 1: dst[0]=r1; dst[1]=g1; dst[2]=b1; dst[3]=1.0f; break;
            case 2: dst[0]=(2*r0+r1)/3; dst[1]=(2*g0+g1)/3; dst[2]=(2*b0+b1)/3; dst[3]=1.0f; break;
            case 3: dst[0]=(r0+2*r1)/3; dst[1]=(g0+2*g1)/3; dst[2]=(b0+2*b1)/3; dst[3]=1.0f; break;
            }
        } else {
            switch (code) {
            case 0: dst[0]=r0; dst[1]=g0; dst[2]=b0; dst[3]=1.0f; break;
            case 1: dst[0]=r1; dst[1]=g1; dst[2]=b1; dst[3]=1.0f; break;
            case 2: dst[0]=(r0+r1)/2; dst[1]=(g0+g1)/2; dst[2]=(b0+b1)/2; dst[3]=1.0f; break;
            case 3: dst[0]=0; dst[1]=0; dst[2]=0; dst[3]=0; break;
            }
        }
    }
}

void DecodeBC3BlockFloat(const uint8_t* block, float* rgbaOut) {
    uint8_t a0 = block[0], a1 = block[1];
    uint64_t aBits = 0;
    for (int i = 2; i < 8; ++i)
        aBits |= ((uint64_t)block[i]) << ((i - 2) * 8);

    float alphas[8];
    alphas[0] = a0 / 255.0f;
    alphas[1] = a1 / 255.0f;
    if (a0 > a1) {
        for (int i = 2; i < 8; ++i)
            alphas[i] = ((8 - i) * a0 + (i - 1) * a1) / (7.0f * 255.0f);
    } else {
        for (int i = 2; i < 6; ++i)
            alphas[i] = ((6 - i) * a0 + (i - 1) * a1) / (5.0f * 255.0f);
        alphas[6] = 0.0f;
        alphas[7] = 1.0f;
    }

    // BC3 color block: always four-color (issue 5), float precision.
    float colorBlock[64];
    DecodeBC1BlockFloat(block + 8, colorBlock, /*forceFourColor=*/true);

    for (int i = 0; i < 16; ++i) {
        uint8_t alphaIdx = (uint8_t)((aBits >> (i * 3)) & 7);
        float* dst = rgbaOut + i * 4;
        dst[0] = colorBlock[i*4];
        dst[1] = colorBlock[i*4+1];
        dst[2] = colorBlock[i*4+2];
        dst[3] = alphas[alphaIdx];
    }
}

void DecodeBC4BlockFloat(const uint8_t* block, float* values) {
    uint8_t v0 = block[0], v1 = block[1];
    uint64_t bits = (uint64_t)read16le(block + 2) | ((uint64_t)read16le(block + 4) << 16) | ((uint64_t)read16le(block + 6) << 32);

    float palette[8];
    palette[0] = v0 / 255.0f;
    palette[1] = v1 / 255.0f;
    if (v0 > v1) {
        for (int i = 2; i < 8; ++i)
            palette[i] = ((8 - i) * v0 + (i - 1) * v1) / (7.0f * 255.0f);
    } else {
        for (int i = 2; i < 6; ++i)
            palette[i] = ((6 - i) * v0 + (i - 1) * v1) / (5.0f * 255.0f);
        palette[6] = 0.0f;
        palette[7] = 1.0f;
    }

    for (int i = 0; i < 16; ++i)
        values[i] = palette[(uint8_t)((bits >> (i * 3)) & 7)];
}

void DecodeBC5BlockFloat(const uint8_t* block, float* rgbaOut) {
    float r[16], g[16];
    DecodeBC4BlockFloat(block, r);
    DecodeBC4BlockFloat(block + 8, g);
    for (int i = 0; i < 16; ++i) {
        float* dst = rgbaOut + i * 4;
        float x = r[i] * 2.0f - 1.0f;
        float y = g[i] * 2.0f - 1.0f;
        float z = std::sqrt((std::max)(0.0f, 1.0f - x*x - y*y));
        // Output stays in the [0,1] image encoding pbrt expects for normalmaps.
        dst[0] = r[i];
        dst[1] = g[i];
        dst[2] = z * 0.5f + 0.5f;
        dst[3] = 1.0f;
    }
}

// Float counterpart of DecompressBC; EOTF is applied to RGB channels when the
// source format is sRGB-named (131/135). BC5 (normals) is UNORM — no EOTF.
void DecompressBCFloat(uint32_t pixelFormat, const uint8_t* src,
                       uint32_t blockW, uint32_t blockH, float* rgbaOut) {
    uint32_t outStride = blockW * 16; // floats per output row (blockW*4 px * 4 ch)
    bool srgb = (pixelFormat == kBC1_RGBA_sRGB || pixelFormat == kBC3_RGBA_sRGB);

    for (uint32_t by = 0; by < blockH; ++by) {
        for (uint32_t bx = 0; bx < blockW; ++bx) {
            uint32_t blockIdx = by * blockW + bx;

            float blockPixels[64];
            if (pixelFormat == kBC1_RGBA_sRGB) {
                DecodeBC1BlockFloat(src + blockIdx * 8, blockPixels, false);
            } else if (pixelFormat == kBC3_RGBA_sRGB) {
                DecodeBC3BlockFloat(src + blockIdx * 16, blockPixels);
            } else if (pixelFormat == kBC5_RGUnorm) {
                DecodeBC5BlockFloat(src + blockIdx * 16, blockPixels);
            } else {
                continue;
            }

            if (srgb) {
                for (int i = 0; i < 16; ++i) {
                    blockPixels[i*4+0] = SRGBToLinear(blockPixels[i*4+0]);
                    blockPixels[i*4+1] = SRGBToLinear(blockPixels[i*4+1]);
                    blockPixels[i*4+2] = SRGBToLinear(blockPixels[i*4+2]);
                }
            }

            for (int row = 0; row < 4; ++row) {
                float* dstRow = rgbaOut + (by * 4 + row) * outStride + bx * 16;
                std::memcpy(dstRow, blockPixels + row * 16, 16 * sizeof(float));
            }
        }
    }
}

// Decompress a full BC texture into RGBA8.
// blockW/blockH: number of 4x4 blocks in each dimension.
// src: compressed data, rgbaOut: output buffer (blockW*4 * blockH*4 * 4 bytes).
void DecompressBC(uint32_t pixelFormat, const uint8_t* src,
                  uint32_t blockW, uint32_t blockH, uint8_t* rgbaOut) {
    uint32_t outStride = blockW * 16; // bytes per output row (blockW*4 pixels * 4 bytes)

    for (uint32_t by = 0; by < blockH; ++by) {
        for (uint32_t bx = 0; bx < blockW; ++bx) {
            uint32_t blockIdx = by * blockW + bx;

            // Decode to a temp buffer (16 pixels = 64 bytes RGBA)
            uint8_t blockPixels[64];
            if (pixelFormat == kBC1_RGBA_sRGB) {
                DecodeBC1Block(src + blockIdx * 8, blockPixels);
            } else if (pixelFormat == kBC3_RGBA_sRGB) {
                DecodeBC3Block(src + blockIdx * 16, blockPixels);
            } else if (pixelFormat == kBC5_RGUnorm) {
                DecodeBC5Block(src + blockIdx * 16, blockPixels);
            } else {
                continue;
            }

            // Copy 4 rows of 4 pixels each into the output image
            for (int row = 0; row < 4; ++row) {
                uint8_t* dstRow = rgbaOut + (by * 4 + row) * outStride + bx * 16;
                std::memcpy(dstRow, blockPixels + row * 16, 16);
            }
        }
    }
}

// ---- BMP file writer (no external dependencies) ----

#pragma pack(push, 1)
struct BmpHeader {
    uint16_t bfType = 0x4D42;   // 'BM'
    uint32_t bfSize;
    uint16_t bfReserved1 = 0;
    uint16_t bfReserved2 = 0;
    uint32_t bfOffBits = 54;
};
struct BmpDibHeader {
    uint32_t biSize = 40;
    int32_t  biWidth;
    int32_t  biHeight;          // positive = bottom-up
    uint16_t biPlanes = 1;
    uint16_t biBitCount = 32;   // 32 bpp BGRA
    uint32_t biCompression = 0; // BI_RGB
    uint32_t biSizeImage = 0;
    int32_t  biXPelsPerMeter = 2835; // 72 DPI
    int32_t  biYPelsPerMeter = 2835;
    uint32_t biClrUsed = 0;
    uint32_t biClrImportant = 0;
};
#pragma pack(pop)

// Write RGBA pixel data as a 32-bit BMP file (BGRA byte order).
// 'pixels' is row-major top-to-bottom RGBA. We flip to bottom-up BMP.
bool WriteBMP(const std::filesystem::path& path,
              uint32_t width, uint32_t height, const uint8_t* pixels) {
    std::ofstream f(path, std::ios::binary);
    if (!f.is_open()) return false;

    uint32_t rowBytes = width * 4;
    uint32_t padBytes = (4 - (rowBytes & 3)) & 3; // BMP rows are 4-byte aligned
    uint32_t paddedRow = rowBytes + padBytes;
    uint32_t pixelDataSize = paddedRow * height;

    BmpHeader hdr;
    hdr.bfSize = sizeof(BmpHeader) + sizeof(BmpDibHeader) + pixelDataSize;
    BmpDibHeader dib;
    dib.biWidth  = width;
    dib.biHeight = (int32_t)height;
    dib.biSizeImage = pixelDataSize;

    f.write(reinterpret_cast<const char*>(&hdr), sizeof(hdr));
    f.write(reinterpret_cast<const char*>(&dib), sizeof(dib));

    // Write bottom-up: last row first. Convert RGBA -> BGRA.
    std::vector<uint8_t> row(paddedRow, 0);
    for (int y = (int)height - 1; y >= 0; --y) {
        const uint8_t* srcRow = pixels + y * width * 4;
        uint8_t* dst = row.data();
        for (uint32_t x = 0; x < width; ++x) {
            dst[0] = srcRow[2]; // B
            dst[1] = srcRow[1]; // G
            dst[2] = srcRow[0]; // R
            dst[3] = srcRow[3]; // A
            dst += 4; srcRow += 4;
        }
        f.write(reinterpret_cast<const char*>(row.data()), paddedRow);
    }

    return FinishOutputFile(f);
}

// ---- PFM file writer (linear float data, no external dependencies) ----

// Write float pixel data as a PFM file (little-endian, bottom-up rows).
// channels: 1 = greyscale (Pf), 3 = RGB (PF).
// data: row-major top-to-bottom floats, srcStride floats per pixel (>= channels).
bool WritePFM(const std::filesystem::path& path,
              uint32_t width, uint32_t height, uint32_t channels,
              const float* data, uint32_t srcStride) {
    std::ofstream f(path, std::ios::binary);
    if (!f.is_open()) return false;

    f << (channels == 3 ? "PF\n" : "Pf\n") << width << ' ' << height << "\n-1.0\n";
    for (int y = (int)height - 1; y >= 0; --y) {
        for (uint32_t x = 0; x < width; ++x) {
            f.write(reinterpret_cast<const char*>(
                        data + ((size_t)y * width + x) * srcStride),
                    channels * sizeof(float));
        }
    }
    return FinishOutputFile(f);
}

// AAPL on-disk compression header: mode at 0, two uint64_t sizes at 8/16.
// Do not use the scene loader's uncompressData: it exits on corrupt headers
// and does not check how many bytes the codec actually produced.
void* DecodeBCMip(const unsigned char* data, size_t length, uint64_t expected) {
    constexpr size_t headerSize = 24;
    if (length < headerSize || expected == 0 ||
        expected >= (std::numeric_limits<size_t>::max)())
        return nullptr;
    uint32_t mode;
    uint64_t decodedSize, encodedSize;
    std::memcpy(&mode, data, sizeof(mode));
    std::memcpy(&decodedSize, data + 8, sizeof(decodedSize));
    std::memcpy(&encodedSize, data + 16, sizeof(encodedSize));
    if (mode != 2049 || decodedSize != expected || encodedSize != length - headerSize)
        return nullptr;
    // The codec reports the destination size on overflow. One extra byte
    // distinguishes exact output from an oversized stream that filled it.
    auto* decoded = static_cast<unsigned char*>(std::malloc(static_cast<size_t>(expected) + 1));
    if (!decoded) return nullptr;
    const size_t actual = lzfse_decode_buffer(decoded, static_cast<size_t>(expected) + 1,
                                             data + headerSize, length - headerSize, nullptr);
    if (actual != expected) {
        std::free(decoded);
        return nullptr;
    }
    return decoded;
}

// Walk down mip levels looking for one whose compressed data fits, and
// lzfse-decompress it. Returns malloc'd BC data (nullptr on total failure).
// Outputs the block/pixel dimensions and the mip level that succeeded.
void* AcquireBCMipData(const AAPLTextureData& tex, const AAPLMeshData* mesh,
                       uint32_t bcBlockBytes, const std::string& baseName,
                       uint32_t& blockWOut, uint32_t& blockHOut,
                       uint32_t& mipWOut, uint32_t& mipHOut, int& mipLevelOut) {
    if (!mesh->_textureData || tex._pixelDataOffset > mesh->_textureDataLength ||
        tex._pixelDataLength > mesh->_textureDataLength - tex._pixelDataOffset)
        return nullptr;
    uint32_t mipW = (uint32_t)tex._width;
    uint32_t mipH = (uint32_t)tex._height;

    for (int mipLevel = 0; mipLevel < (int)tex._mipmapLevelCount; ++mipLevel) {
        if (mipLevel >= (int)tex._mipOffsets.size() ||
            mipLevel >= (int)tex._mipLengths.size())
            break;

        unsigned long long compOff = tex._mipOffsets[mipLevel];
        unsigned long long compLen = tex._mipLengths[mipLevel];
        uint32_t blockW = ((mipW + 3) / 4 > 0) ? (mipW + 3) / 4 : 1;
        uint32_t blockH = ((mipH + 3) / 4 > 0) ? (mipH + 3) / 4 : 1;
        unsigned long long bcBytes = (unsigned long long)blockW * blockH * bcBlockBytes;

        void* bcData = nullptr;
        if (compOff <= tex._pixelDataLength &&
            compLen <= tex._pixelDataLength - compOff) {
            const auto* raw = static_cast<const unsigned char*>(mesh->_textureData)
                              + tex._pixelDataOffset + compOff;
            bcData = DecodeBCMip(raw, static_cast<size_t>(compLen), bcBytes);
        }
        if (bcData) {
            blockWOut = blockW;
            blockHOut = blockH;
            mipWOut = mipW;
            mipHOut = mipH;
            mipLevelOut = mipLevel;
            return bcData;
        }

        spdlog::debug("PbrtExporter: {} mip {} decompress failed ({}->{}), "
                      "trying next", baseName, mipLevel, compLen, bcBytes);
        mipW = (mipW > 1 ? mipW >> 1 : 1);
        mipH = (mipH > 1 ? mipH >> 1 : 1);
    }
    return nullptr;
}

// Given a texture hash, find the corresponding AAPLTextureData and export the
// decompressed pixel data as a BMP file. Returns the output filename (without
// directory) on success, empty string on failure.
// channel: -1 = all channels (RGBA), 0/1/2 = extract R/G/B as greyscale.
// suffix: appended before the ".bmp" extension (e.g. "_rough").
std::string ExportTextureData(
    uint32_t hash,
    const std::unordered_map<uint32_t, size_t>& streamingEntryMap,
    const std::vector<TextureStreamingEntry>& streamingEntries,
    const AAPLMeshData* mesh,
    const std::filesystem::path& outputDir,
    int channel = -1,
    const char* suffix = "")
{
    auto it = streamingEntryMap.find(hash);
    if (it == streamingEntryMap.end() || it->second >= streamingEntries.size()) return {};

    const auto& entry = streamingEntries[it->second];
    if (!entry.desc) return {};

    const AAPLTextureData& tex = *entry.desc;
    if (!IsSupportedTexture(tex)) {
        spdlog::warn("PbrtExporter: unsupported format or dimensions for texture {}", hash);
        return {};
    }

    // Build output filename from the embedded path (strip directories)
    std::string srcPath = tex._path;
    auto slashPos = srcPath.find_last_of("/\\");
    std::string baseName = (slashPos != std::string::npos)
        ? srcPath.substr(slashPos + 1) : srcPath;
    auto dotPos = baseName.find_last_of('.');
    if (dotPos != std::string::npos)
        baseName = baseName.substr(0, dotPos);
    // Hash identity prevents two source directories with the same basename
    // from silently overwriting each other. Generated names are PBRT-safe.
    std::string outName = "texture_" + std::to_string(hash) + suffix + ".bmp";
    auto outPath = outputDir / outName;

    if (!mesh->_textureData) return {};

    // Determine BC block size from pixel format.
    uint32_t bcBlockBytes = 8;   // BC1 default
    if (tex._pixelFormat == kBC3_RGBA_sRGB || tex._pixelFormat == kBC5_RGUnorm)
        bcBlockBytes = 16;

    // The .bin stores every mip as an independent lzfse-compressed chunk
    // at _pixelDataOffset + _mipOffsets[mip] with _mipLengths[mip] bytes.
    uint32_t blockW = 0, blockH = 0, mipW = 0, mipH = 0;
    int mipLevel = 0;
    void* bcData = AcquireBCMipData(tex, mesh, bcBlockBytes, baseName,
                                    blockW, blockH, mipW, mipH, mipLevel);

    if (!bcData) {
        spdlog::warn("PbrtExporter: texture {} all mips failed", baseName);
        return {};
    }

    // BC decompression → RGBA pixels
    uint32_t pixelCount = blockW * 4 * blockH * 4;
    std::vector<uint8_t> rgba(pixelCount * 4);
    DecompressBC(tex._pixelFormat, static_cast<const uint8_t*>(bcData),
                 blockW, blockH, rgba.data());
    free(bcData);

    uint32_t w = blockW * 4;
    uint32_t h = blockH * 4;
    if (w > mipW) w = mipW;
    if (h > mipH) h = mipH;

    // Crop if dimensions are not exact multiples of 4
    if (w != blockW * 4 || h != blockH * 4) {
        uint32_t outStride = blockW * 16;
        std::vector<uint8_t> cropped(w * h * 4);
        for (uint32_t y = 0; y < h; ++y)
            std::memcpy(cropped.data() + y * w * 4,
                        rgba.data() + y * outStride, w * 4);
        rgba = std::move(cropped);
    }

    // If a single channel was requested, convert to greyscale (R=G=B=channel).
    if (channel >= 0 && channel <= 2) {
        for (uint32_t i = 0; i < (uint32_t)rgba.size() / 4; ++i) {
            uint8_t v = rgba[i * 4 + channel];
            rgba[i * 4 + 0] = v;
            rgba[i * 4 + 1] = v;
            rgba[i * 4 + 2] = v;
        }
    }

    if (!WriteBMP(outPath, w, h, rgba.data())) {
        spdlog::warn("PbrtExporter: failed to write texture {}", outPath.string());
        return {};
    }

    if (mipLevel > 0)
        spdlog::info("PbrtExporter: exported texture {} ({}x{}, mip {})",
                     outName, w, h, mipLevel);
    else
        spdlog::info("PbrtExporter: exported texture {} ({}x{})", outName, w, h);
    return outName;
}

// Linear-data counterpart of ExportTextureData: same lookup/decompress path,
// but decodes to float (endpoint precision preserved), applies an optional
// per-pixel transform (e.g. roughness clamp+square), and writes a PFM file.
// channel: -1 = RGB (PF), 0/1/2 = extract that channel as greyscale (Pf),
//          extracted AFTER the transform runs.
// transform(px, pixelFormat) is called per pixel before channel extraction.
std::string ExportTextureDataLinear(
    uint32_t hash,
    const std::unordered_map<uint32_t, size_t>& streamingEntryMap,
    const std::vector<TextureStreamingEntry>& streamingEntries,
    const AAPLMeshData* mesh,
    const std::filesystem::path& outputDir,
    int channel = -1,
    const char* suffix = "",
    const std::function<void(float*, uint32_t)>& transform = nullptr)
{
    auto it = streamingEntryMap.find(hash);
    if (it == streamingEntryMap.end() || it->second >= streamingEntries.size()) return {};

    const auto& entry = streamingEntries[it->second];
    if (!entry.desc) return {};

    const AAPLTextureData& tex = *entry.desc;
    if (!IsSupportedTexture(tex)) {
        spdlog::warn("PbrtExporter: unsupported format or dimensions for texture {}", hash);
        return {};
    }

    // Build output filename from the embedded path (strip directories)
    std::string srcPath = tex._path;
    auto slashPos = srcPath.find_last_of("/\\");
    std::string baseName = (slashPos != std::string::npos)
        ? srcPath.substr(slashPos + 1) : srcPath;
    auto dotPos = baseName.find_last_of('.');
    if (dotPos != std::string::npos)
        baseName = baseName.substr(0, dotPos);
    std::string outName = "texture_" + std::to_string(hash) + suffix + ".pfm";
    auto outPath = outputDir / outName;

    if (!mesh->_textureData) return {};

    uint32_t bcBlockBytes = 8;   // BC1 default
    if (tex._pixelFormat == kBC3_RGBA_sRGB || tex._pixelFormat == kBC5_RGUnorm)
        bcBlockBytes = 16;

    uint32_t blockW = 0, blockH = 0, mipW = 0, mipH = 0;
    int mipLevel = 0;
    void* bcData = AcquireBCMipData(tex, mesh, bcBlockBytes, baseName,
                                    blockW, blockH, mipW, mipH, mipLevel);

    if (!bcData) {
        spdlog::warn("PbrtExporter: texture {} all mips failed", baseName);
        return {};
    }

    // BC decompression → float RGBA pixels (linear)
    uint32_t pixelCount = blockW * 4 * blockH * 4;
    std::vector<float> rgba(pixelCount * 4);
    DecompressBCFloat(tex._pixelFormat, static_cast<const uint8_t*>(bcData),
                      blockW, blockH, rgba.data());
    free(bcData);

    uint32_t w = blockW * 4;
    uint32_t h = blockH * 4;
    if (w > mipW) w = mipW;
    if (h > mipH) h = mipH;

    // Crop if dimensions are not exact multiples of 4
    if (w != blockW * 4 || h != blockH * 4) {
        uint32_t outStride = blockW * 16;
        std::vector<float> cropped((size_t)w * h * 4);
        for (uint32_t y = 0; y < h; ++y)
            std::memcpy(cropped.data() + (size_t)y * w * 4,
                        rgba.data() + (size_t)y * outStride, w * 4 * sizeof(float));
        rgba = std::move(cropped);
    }

    if (transform) {
        for (uint32_t i = 0; i < w * h; ++i)
            transform(rgba.data() + i * 4, tex._pixelFormat);
    }

    bool ok = false;
    if (channel >= 0 && channel <= 2) {
        std::vector<float> grey((size_t)w * h);
        for (uint32_t i = 0; i < (size_t)w * h; ++i)
            grey[i] = rgba[i * 4 + channel];
        ok = WritePFM(outPath, w, h, 1, grey.data(), 1);
    } else {
        ok = WritePFM(outPath, w, h, 3, rgba.data(), 4);
    }

    if (!ok) {
        spdlog::warn("PbrtExporter: failed to write texture {}", outPath.string());
        return {};
    }

    if (mipLevel > 0)
        spdlog::info("PbrtExporter: exported texture {} ({}x{}, mip {})",
                     outName, w, h, mipLevel);
    else
        spdlog::info("PbrtExporter: exported texture {} ({}x{})", outName, w, h);
    return outName;
}

// Write a binary little-endian PLY file for one submesh.
// Returns the output filename (without directory) on success, empty on failure.
std::string WritePLYMesh(
    const std::filesystem::path& outputDir, int meshIndex,
    const vec3* verts, const vec3* norms, const vec2* uvs,
    uint32_t rangeCount, uint32_t idxMin,
    const void* indices, bool is16bit,
    uint32_t indexBegin, uint32_t indexCount)
{
    std::string outName = "mesh_" + std::to_string(meshIndex) + ".ply";
    std::ofstream f(outputDir / outName, std::ios::binary);
    if (!f.is_open()) return {};

    uint32_t nTri = indexCount / 3;

    f << "ply\nformat binary_little_endian 1.0\n";
    f << "element vertex " << rangeCount << '\n';
    f << "property float x\nproperty float y\nproperty float z\n";
    if (norms) f << "property float nx\nproperty float ny\nproperty float nz\n";
    if (uvs)   f << "property float u\nproperty float v\n";
    f << "element face " << nTri << '\n';
    f << "property list uchar int vertex_indices\n";
    f << "end_header\n";

    auto wf = [&](float v) {
        if (std::isnan(v) || std::isinf(v)) v = 0.0f;
        f.write(reinterpret_cast<const char*>(&v), 4);
    };

    for (uint32_t i = 0; i < rangeCount; ++i) {
        const vec3& v = verts[idxMin + i];
        wf(v.x); wf(v.y); wf(v.z);
        if (norms) { const vec3& n = norms[idxMin + i]; wf(n.x); wf(n.y); wf(n.z); }
        if (uvs)   { const vec2& uv = uvs[idxMin + i]; wf(uv.x); wf(1.0f - uv.y); }
    }

    const uint8_t three = 3;
    for (uint32_t k = 0; k + 2 < indexCount; k += 3) {
        int32_t i0, i1, i2;
        if (is16bit) {
            const uint16_t* idx = static_cast<const uint16_t*>(indices) + indexBegin;
            i0 = idx[k] - idxMin; i1 = idx[k+1] - idxMin; i2 = idx[k+2] - idxMin;
        } else {
            const uint32_t* idx = static_cast<const uint32_t*>(indices) + indexBegin;
            i0 = idx[k] - idxMin; i1 = idx[k+1] - idxMin; i2 = idx[k+2] - idxMin;
        }
        f.write(reinterpret_cast<const char*>(&three), 1);
        f.write(reinterpret_cast<const char*>(&i0), 4);
        f.write(reinterpret_cast<const char*>(&i1), 4);
        f.write(reinterpret_cast<const char*>(&i2), 4);
    }

    return FinishOutputFile(f) ? outName : "";
}

} // anonymous namespace

// ---- PbrtExporter private helpers ----

void PbrtExporter::WriteCamera(std::ofstream& out, const GpuScene& scene) {
    const Camera* cam = scene.maincamera;
    if (!cam) {
        spdlog::warn("PbrtExporter: no camera; using defaults");
        out << "LookAt 0 0 0  0 0 1  0 1 0\n";
        out << R"(Camera "perspective" "float fov" [65])" << '\n';
        return;
    }
    const vec3& eye = cam->GetOrigin();
    vec3 dir = cam->GetCameraDir();
    vec3 lookAt(eye.x + dir.x, eye.y + dir.y, eye.z + dir.z);
    vec3 up(0.0f, 1.0f, 0.0f);
    out << "LookAt " << eye << ' ' << lookAt << ' ' << up << '\n';
    float fovDeg = cam->Fov() * (180.0f / 3.14159265358979323846f);
    out << R"(Camera "perspective" "float fov" [)" << fovDeg << "]\n";
}

void PbrtExporter::WriteFilm(std::ofstream& out, uint32_t width, uint32_t height) {
    out << R"(Film "gbuffer" "integer xresolution" [)" << width/2
        << R"(] "integer yresolution" [)" << height/2 << "]\n";
    // EXR preserves full HDR range; avoids "out of gamut" clamp warnings on PNG.
    out << R"(    "string filename" "output.exr")" << '\n';
}

void PbrtExporter::WriteMaterials(std::ofstream& out, const GpuScene& scene,
    const std::unordered_map<uint32_t, std::string>& exportedColorTexNames,
    const std::unordered_map<uint32_t, std::string>& exportedRoughTexNames,
    const std::unordered_map<uint32_t, std::string>& exportedNormalTexNames) {
    if (!scene.cpuMaterials || scene.applMesh->_materialCount == 0) {
        spdlog::warn("PbrtExporter: no materials to export");
        return;
    }
    const int materialCount = static_cast<int>(scene.applMesh->_materialCount);
    for (int i = 0; i < materialCount; ++i) {
        const AAPLMaterial& mat = scene.cpuMaterials[i];
        float metallic = mat.metallicRoughness.x;

        // pbrt-v4: use "conductor" for metals, "coateddiffuse" for dielectrics.
        // "uber" was removed in v4. Each parameter must appear EXACTLY ONCE
        // (inline value OR texture reference, never both).
        out << Indent(1) << "MakeNamedMaterial \"material_" << i << "\"\n";
        out << Indent(1) << "\"string type\" \""
            << (metallic > 0.5f ? "conductor" : "coateddiffuse") << "\"\n";
        // BaseColor: only reference the texture if it actually exported;
        // otherwise fall back to the constant color instead of leaving a
        // dangling reference that fails scene load (issue 3).
        if (mat.hasBaseColorTexture &&
            exportedColorTexNames.count(mat.baseColorTextureHash)) {
            out << Indent(1) << "\"texture reflectance\" \"color_" << i << "\"\n";
        } else {
            if (mat.hasBaseColorTexture)
                spdlog::warn("PbrtExporter: material {} base color texture "
                             "(hash {}) not exported; using constant",
                             i, mat.baseColorTextureHash);
            out << Indent(1) << "\"rgb reflectance\" [ "
                << mat.baseColor.x << ' ' << mat.baseColor.y << ' ' << mat.baseColor.z << " ]\n";
        }
        // GGX alpha matching the realtime shader: clamp to the 0.08 floor,
        // then square. pbrt's remaproughness must be off so uroughness/
        // vroughness are used as the alpha directly (issue 4).
        auto writeRoughnessConstant = [&](float roughness) {
            float r = (std::max)(roughness, 0.08f);
            float alpha = r * r;
            out << Indent(1) << "\"float uroughness\" [ " << alpha << " ]\n";
            out << Indent(1) << "\"float vroughness\" [ " << alpha << " ]\n";
        };
        if (mat.hasMetallicRoughnessTexture) {
            auto it = exportedRoughTexNames.find(mat.metallicRoughnessHash);
            if (it != exportedRoughTexNames.end()) {
                out << Indent(1) << "\"texture uroughness\" \"roughness_" << i << "\"\n";
                out << Indent(1) << "\"texture vroughness\" \"roughness_" << i << "\"\n";
            } else {
                writeRoughnessConstant(mat.metallicRoughness.y);
            }
        } else {
            writeRoughnessConstant(mat.metallicRoughness.y);
        }
        out << Indent(1) << "\"bool remaproughness\" [false]\n";
        if (mat.hasNormalMap) {
            auto it = exportedNormalTexNames.find(mat.normalMapHash);
            if (it != exportedNormalTexNames.end())
                out << Indent(1) << "\"string normalmap\" \"" << it->second << "\"\n";
        }
    }
}

// Returns true when every non-empty submesh was written; false on missing
// required geometry, an out-of-range index, or any PLY write failure —
// geometry errors are not degradable and must fail the export (issue 7).
bool PbrtExporter::WriteGeometry(std::ofstream& out, const GpuScene& scene,
                                  const std::filesystem::path& outputDir,
                                  const std::unordered_map<int, std::string>& materialAlphaTexNames,
                                  const std::unordered_map<uint32_t, std::string>& exportedEmissiveTexNames) {
    const AAPLMeshData* mesh = scene.applMesh;
    if (!mesh || !scene.m_SubMeshes) {
        spdlog::error("PbrtExporter: no mesh data to export");
        return false;
    }
    const vec3* verts = static_cast<const vec3*>(mesh->_vertexData);
    const vec3* norms = static_cast<const vec3*>(mesh->_normalData);
    const vec2* uvs = static_cast<const vec2*>(mesh->_uvData);
    const void* indices = mesh->_indexData;
    if (!verts || !indices) {
        spdlog::error("PbrtExporter: vertex or index data is null");
        return false;
    }
    const int meshCount = static_cast<int>(mesh->_meshCount);
    // The loader declares MTLIndexType (0/1), while existing Bistro .bin
    // assets store byte widths (2/4). Accept both representations.
    if (mesh->_indexType != 0 && mesh->_indexType != 1 &&
        mesh->_indexType != 2 && mesh->_indexType != 4) {
        spdlog::error("PbrtExporter: unsupported index type {}", mesh->_indexType);
        return false;
    }
    bool is16bit = (mesh->_indexType == 0 || mesh->_indexType == 2);
    bool anyWritten = false;
    for (int m = 0; m < meshCount; ++m) {
        const AAPLSubMesh& submesh = scene.m_SubMeshes[m];
        if (submesh.indexCount == 0) continue;
        if ((submesh.indexCount % 3) != 0) {
            spdlog::error("PbrtExporter: submesh {} has index count {}, not a multiple of 3",
                         m, submesh.indexCount);
            return false;
        }
        // Validate the slice BEFORE constructing or scanning an index pointer.
        if (submesh.indexBegin > mesh->_indexCount ||
            submesh.indexCount > mesh->_indexCount - submesh.indexBegin) {
            spdlog::error("PbrtExporter: submesh {} index range exceeds the index buffer", m);
            return false;
        }

        uint32_t idxMin = 0xFFFFFFFF, idxMax = 0;
        if (is16bit) {
            const uint16_t* scan = static_cast<const uint16_t*>(indices) + submesh.indexBegin;
            for (uint32_t k = 0; k < submesh.indexCount; ++k) {
                if (scan[k] < idxMin) idxMin = scan[k];
                if (scan[k] > idxMax) idxMax = scan[k];
            }
        } else {
            const uint32_t* scan = static_cast<const uint32_t*>(indices) + submesh.indexBegin;
            for (uint32_t k = 0; k < submesh.indexCount; ++k) {
                if (scan[k] < idxMin) idxMin = scan[k];
                if (scan[k] > idxMax) idxMax = scan[k];
            }
        }
        if (idxMin > idxMax) continue;
        if (idxMax >= mesh->_vertexCount) {
            spdlog::error("PbrtExporter: submesh {} index {} out of range "
                          "(vertex count {})", m, idxMax, mesh->_vertexCount);
            return false;
        }

        uint32_t rangeCount = idxMax - idxMin + 1;

        std::string plyName = WritePLYMesh(outputDir, m, verts, norms, uvs,
                                           rangeCount, idxMin, indices, is16bit,
                                           submesh.indexBegin, submesh.indexCount);
        if (plyName.empty()) {
            spdlog::error("PbrtExporter: failed to write PLY for submesh {}", m);
            return false;
        }
        anyWritten = true;

        out << '\n' << Indent(1) << "AttributeBegin\n";
        int matIndex = static_cast<int>(submesh.materialIndex);
        const AAPLMaterial* subMat = nullptr;
        if (matIndex >= 0 && matIndex < static_cast<int>(mesh->_materialCount)) {
            out << Indent(2) << "NamedMaterial \"material_" << matIndex << "\"\n";
            if (scene.cpuMaterials)
                subMat = &scene.cpuMaterials[matIndex];
        }
        // Area light for emissive materials (issue 6): texture wins when it
        // exported; otherwise the constant emissive color is the fallback.
        // Two-sided emission is NOT enabled — pbrt's default front-face
        // emission, matching the backface-culled realtime rendering.
        if (subMat) {
            bool wroteLight = false;
            if (subMat->hasEmissiveTexture) {
                auto it = exportedEmissiveTexNames.find(subMat->emissiveTextureHash);
                if (it != exportedEmissiveTexNames.end()) {
                    out << Indent(2) << "AreaLightSource \"diffuse\"\n";
                    out << Indent(3) << "\"string filename\" \"" << it->second << "\"\n";
                    out << Indent(3) << "\"float scale\" [ "
                        << scene.frameConstants.emissiveScale << " ]\n";
                    wroteLight = true;
                }
            }
            if (!wroteLight &&
                (subMat->emissiveColor.x > 0 || subMat->emissiveColor.y > 0 ||
                 subMat->emissiveColor.z > 0)) {
                out << Indent(2) << "AreaLightSource \"diffuse\"\n";
                out << Indent(3) << "\"rgb L\" [ "
                    << subMat->emissiveColor.x << ' ' << subMat->emissiveColor.y << ' '
                    << subMat->emissiveColor.z << " ]\n";
                out << Indent(3) << "\"float scale\" [ "
                    << scene.frameConstants.emissiveScale << " ]\n";
            }
        }
        out << Indent(2) << R"(Shape "plymesh" "string filename" ")" << plyName << "\"\n";
        // Alpha (issue 2): an exported mask binds to the Shape by texture name;
        // without one, cutout collapses to the thresholded constant and
        // fractional opacity to a constant coverage approximation. alpha = 1
        // is pbrt's default and is not written.
        if (subMat && (subMat->hasDiffuseMask || subMat->opacity < 1.0f)) {
            auto it = materialAlphaTexNames.find(matIndex);
            if (it != materialAlphaTexNames.end()) {
                out << Indent(3) << "\"texture alpha\" \"" << it->second << "\"\n";
            } else {
                float a = subMat->hasDiffuseMask
                    ? ((subMat->opacity >= 0.1f) ? 1.0f : 0.0f)
                    : subMat->opacity;
                if (a < 1.0f)
                    out << Indent(3) << "\"float alpha\" [ " << a << " ]\n";
            }
        }
        out << Indent(1) << "AttributeEnd\n";
    }
    if (!anyWritten) {
        spdlog::error("PbrtExporter: no geometry written");
        return false;
    }
    return true;
}

void PbrtExporter::WriteLights(std::ofstream& out, const GpuScene& scene) {
    // Game engines store colors in normalized [0,1] linear sRGB; pbrt treats
    // "rgb L/I" as physical W/(m²·sr) / W·sr⁻¹. Scale by 1/π so a diffuse-white
    // surface lit head-on by a normalized sun produces exactly 1.0 output radiance.
    constexpr float kLightScale = 1.0f / 3.14159265358979323846f;

    const vec3& sunDir = scene.frameConstants.sunDirection;
    const vec3& sunColor = scene.frameConstants.sunColor;
    if (sunColor.x > 0 || sunColor.y > 0 || sunColor.z > 0) {
        // sunDirection is surface→sun; place the distant light source in that direction.
        vec3 from(sunDir.x * 10000.0f, sunDir.y * 10000.0f, sunDir.z * 10000.0f);
        out << '\n' << Indent(1) << R"(LightSource "distant")" << '\n';
        out << Indent(2) << "\"point3 from\" [ " << from << " ]\n";
        out << Indent(2) << "\"rgb L\" [ "
            << sunColor.x << ' ' << sunColor.y << ' ' << sunColor.z << " ]\n";
        out << Indent(2) << "\"float scale\" [ " << kLightScale << " ]\n";
    }
    for (const auto& pl : scene._pointLights) {
        const PointLightData* d = pl.getPointLightData();
        if (!d) continue;
        vec3 pos(d->posSqrRadius.x, d->posSqrRadius.y, d->posSqrRadius.z);
        float intensity = scene.frameConstants.localLightIntensity;
        out << '\n' << Indent(1) << R"(LightSource "point")" << '\n';
        out << Indent(2) << "\"point3 from\" [ " << pos << " ]\n";
        out << Indent(2) << "\"rgb I\" [ "
            << d->color.x * intensity << ' '
            << d->color.y * intensity << ' '
            << d->color.z * intensity << " ]\n";
        out << Indent(2) << "\"float scale\" [ " << kLightScale << " ]\n";
    }
    for (const auto& sl : scene._spotLights) {
        const SpotLightData* d = sl._spotLightData;
        if (!d) continue;
        vec3 pos(d->posAndHeight.x, d->posAndHeight.y, d->posAndHeight.z);
        vec3 dir(d->dirAndOuterAngle.x, d->dirAndOuterAngle.y, d->dirAndOuterAngle.z);
        vec3 lookAt(pos.x + dir.x * 10.0f, pos.y + dir.y * 10.0f, pos.z + dir.z * 10.0f);
        float coneDeg = d->dirAndOuterAngle.w * 180.0f / 3.1415926535897932f;
        vec3 color(d->colorAndInnerAngle.x, d->colorAndInnerAngle.y, d->colorAndInnerAngle.z);
        float intensity = scene.frameConstants.localLightIntensity;
        out << '\n' << Indent(1) << R"(LightSource "spot")" << '\n';
        out << Indent(2) << "\"point3 from\" [ " << pos << " ]\n";
        out << Indent(2) << "\"point3 to\" [ " << lookAt << " ]\n";
        out << Indent(2) << "\"float coneangle\" [ " << coneDeg << " ]\n";
        out << Indent(2) << "\"rgb I\" [ "
            << color.x * intensity << ' '
            << color.y * intensity << ' '
            << color.z * intensity << " ]\n";
        out << Indent(2) << "\"float scale\" [ " << kLightScale << " ]\n";
    }
}

// ---- Public API ----

bool PbrtExporter::Export(const GpuScene& scene,
                          const std::filesystem::path& outputPath) {
    try {
        if (!scene.applMesh ||
            (scene.applMesh->_materialCount != 0 && !scene.cpuMaterials)) {
            spdlog::error("PbrtExporter: missing scene or material data");
            return false;
        }
        // Step 1: Export BMP color textures and PFM data maps alongside the scene.
        std::filesystem::path outputDir = outputPath.parent_path();
        if (outputDir.empty()) outputDir = ".";
        std::unordered_map<uint32_t, std::string> exportedColorTexNames; // hash -> color BMP filename
        std::unordered_map<uint32_t, std::string> exportedRoughTexNames; // hash -> roughness PFM filename (linear)
        std::unordered_map<uint32_t, std::string> exportedNormalTexNames; // hash -> normal map PFM filename (linear)
        std::unordered_map<uint32_t, std::string> exportedEmissiveTexNames; // hash -> emissive BMP filename
        // Alpha masks are per (hash, opacity, cutout) — a shared source image
        // with different opacities must NOT share a processed mask.
        struct AlphaExport { std::string texName; std::string fileName; };
        std::unordered_map<int, AlphaExport> alphaExports;           // material index -> mask
        std::unordered_map<std::string, AlphaExport> alphaCache;     // combo key -> dedupe
        std::unordered_map<int, std::string> materialAlphaTexNames;  // material index -> pbrt texture name

        if (scene.cpuMaterials && scene.applMesh->_textureData) {
            const int matCount = static_cast<int>(scene.applMesh->_materialCount);
            for (int i = 0; i < matCount; ++i) {
                const AAPLMaterial& mat = scene.cpuMaterials[i];
                // Export base color textures with all channels.
                auto exportTex = [&](uint32_t hash) {
                    if (hash == 0 || exportedColorTexNames.count(hash)) return;
                    std::string name = ExportTextureData(hash,
                        scene.streamingEntryMap, scene.streamingEntries,
                        scene.applMesh, outputDir);
                    if (!name.empty())
                        exportedColorTexNames[hash] = name;
                };
                // Export roughness as linear PFM: the source G channel is
                // sRGB-decoded by the float path, then clamped to the realtime
                // shader's 0.08 floor and squared (alpha = roughness²).
                auto exportRoughTex = [&](uint32_t hash) {
                    if (hash == 0 || exportedRoughTexNames.count(hash)) return;
                    std::string name = ExportTextureDataLinear(hash,
                        scene.streamingEntryMap, scene.streamingEntries,
                        scene.applMesh, outputDir, /*channel=*/1, /*suffix=*/"_rough",
                        [](float* px, uint32_t) {
                            float r = (std::max)(px[1], 0.08f);
                            float alpha = r * r;
                            px[0] = px[1] = px[2] = alpha;
                            px[3] = 1.0f;
                        });
                    if (!name.empty())
                        exportedRoughTexNames[hash] = name;
                };
                // Export normal maps as linear PFM (pbrt reads PFM as linear
                // float; BMP would be sRGB-decoded — issue 1). Tangent-frame
                // matching is a separate validation; PLY stores no tangents.
                auto exportNormalTex = [&](uint32_t hash) {
                    if (hash == 0 || exportedNormalTexNames.count(hash)) return;
                    std::string name = ExportTextureDataLinear(hash,
                        scene.streamingEntryMap, scene.streamingEntries,
                        scene.applMesh, outputDir, /*channel=*/-1, /*suffix=*/"_nm");
                    if (!name.empty())
                        exportedNormalTexNames[hash] = name;
                };
                // Export emissive textures as BMP (color semantics: pbrt's
                // sRGB image decode is correct for them — issue 6).
                auto exportEmissiveTex = [&](uint32_t hash) {
                    if (hash == 0 || exportedEmissiveTexNames.count(hash)) return;
                    std::string name = ExportTextureData(hash,
                        scene.streamingEntryMap, scene.streamingEntries,
                        scene.applMesh, outputDir, /*channel=*/-1, /*suffix=*/"_emissive");
                    if (!name.empty())
                        exportedEmissiveTexNames[hash] = name;
                };
                if (mat.hasBaseColorTexture)        exportTex(mat.baseColorTextureHash);
                if (mat.hasMetallicRoughnessTexture) exportRoughTex(mat.metallicRoughnessHash);
                if (mat.hasNormalMap)               exportNormalTex(mat.normalMapHash);
                if (mat.hasEmissiveTexture)         exportEmissiveTex(mat.emissiveTextureHash);

                // Alpha mask (issue 2): cutout thresholds alpha*opacity at
                // the realtime shader's 0.1; fractional opacity keeps
                // alpha*opacity as a coverage approximation. Mask is linear
                // PFM, extracted per material combo (never shared across
                // differing opacities).
                bool wantsMask = (mat.hasDiffuseMask || mat.opacity < 1.0f) &&
                                 mat.hasBaseColorTexture;
                if (wantsMask) {
                    bool cutout = mat.hasDiffuseMask;
                    float opacity = mat.opacity;
                    // Decimal rounding can merge values on opposite sides of
                    // the cutout threshold. Cache the exact float identity.
                    uint32_t opacityBits;
                    static_assert(sizeof(opacityBits) == sizeof(opacity));
                    std::memcpy(&opacityBits, &opacity, sizeof(opacity));
                    std::string key = std::to_string(mat.baseColorTextureHash) +
                        (cutout ? "|c|" : "|v|") + std::to_string(opacityBits);
                    auto cached = alphaCache.find(key);
                    if (cached != alphaCache.end()) {
                        alphaExports[i] = cached->second;
                        materialAlphaTexNames[i] = cached->second.texName;
                    } else {
                        std::string texName = "alpha_" + std::to_string(i);
                        std::string fileSuffix = "_alpha_" + std::to_string(i);
                        std::string fname = ExportTextureDataLinear(
                            mat.baseColorTextureHash,
                            scene.streamingEntryMap, scene.streamingEntries,
                            scene.applMesh, outputDir, /*channel=*/0,
                            fileSuffix.c_str(),
                            [cutout, opacity](float* px, uint32_t) {
                                float a = px[3] * opacity;
                                float m = cutout ? ((a >= 0.1f) ? 1.0f : 0.0f) : a;
                                px[0] = px[1] = px[2] = m;
                                px[3] = 1.0f;
                            });
                        if (!fname.empty()) {
                            AlphaExport rec{texName, fname};
                            alphaCache[key] = rec;
                            alphaExports[i] = rec;
                            materialAlphaTexNames[i] = texName;
                        }
                    }
                }
            }
        }

        // Step 2: Write the .pbrt scene file
        std::ofstream out(outputPath);
        if (!out.is_open()) {
            spdlog::error("PbrtExporter: failed to open {} for writing",
                          outputPath.string());
            return false;
        }
        out << std::fixed << std::setprecision(kPrecision);

        out << R"(Integrator "path" "integer maxdepth" [8])" << '\n';
        out << R"(Sampler "zsobol" "integer pixelsamples" [256])" << '\n';
        const VkExtent2D& ext = scene.device.getSwapChainExtent();
        WriteFilm(out, ext.width, ext.height);
        WriteCamera(out, scene);
        out << R"(PixelFilter "gaussian" "float xradius" [1.5] "float yradius" [1.5])" << '\n';

        out << '\n' << "WorldBegin\n";

        // Texture declarations with exported filenames (must be inside WorldBegin)
        std::unordered_set<std::string> declaredAlphaTexNames;
        if (scene.cpuMaterials) {
            const int matCount = static_cast<int>(scene.applMesh->_materialCount);
            for (int i = 0; i < matCount; ++i) {
                const AAPLMaterial& mat = scene.cpuMaterials[i];
                auto declColorTex = [&](uint32_t hash, const char* prefix, const char* texType) {
                    auto it = exportedColorTexNames.find(hash);
                    if (it == exportedColorTexNames.end()) return;
                    out << Indent(1) << "Texture \"" << prefix << i << "\" \""
                        << texType << "\" \"imagemap\"" << '\n';
                    out << Indent(2) << "\"string filename\" \""
                        << it->second << "\"\n";
                };
                auto declRoughTex = [&](uint32_t hash, const char* prefix) {
                    auto it = exportedRoughTexNames.find(hash);
                    if (it == exportedRoughTexNames.end()) return;
                    out << Indent(1) << "Texture \"" << prefix << i << "\" "
                        << "\"float\" \"imagemap\"" << '\n';
                    out << Indent(2) << "\"string filename\" \""
                        << it->second << "\"\n";
                };
                if (mat.hasBaseColorTexture)
                    declColorTex(mat.baseColorTextureHash, "color_", "spectrum");
                if (mat.hasMetallicRoughnessTexture)
                    declRoughTex(mat.metallicRoughnessHash, "roughness_");
                // Alpha mask declarations; shared combos declare once.
                auto ait = alphaExports.find(i);
                if (ait != alphaExports.end() &&
                    declaredAlphaTexNames.insert(ait->second.texName).second) {
                    out << Indent(1) << "Texture \"" << ait->second.texName
                        << "\" \"float\" \"imagemap\"" << '\n';
                    out << Indent(2) << "\"string filename\" \""
                        << ait->second.fileName << "\"\n";
                }
                // Normal maps use "string normalmap" directly (not a texture reference)
            }
        }

        WriteMaterials(out, scene, exportedColorTexNames, exportedRoughTexNames, exportedNormalTexNames);
        if (!WriteGeometry(out, scene, outputDir, materialAlphaTexNames, exportedEmissiveTexNames))
            return false;
        WriteLights(out, scene);

        // Explicitly complete the write before reporting success (issue 7):
        // a truncated main scene file must not count as a successful export.
        if (!FinishOutputFile(out)) {
            spdlog::error("PbrtExporter: failed while writing {}",
                          outputPath.string());
            return false;
        }

        spdlog::info("PbrtExporter: wrote scene to {}", outputPath.string());
        return true;
    } catch (const std::exception& e) {
        spdlog::error("PbrtExporter: exception: {}", e.what());
        return false;
    }
}
