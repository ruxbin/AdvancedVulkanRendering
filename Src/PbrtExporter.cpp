#include "PbrtExporter.h"
#include "GpuScene.h"
#include "spdlog/spdlog.h"
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iomanip>

// From GpuScene.cpp: lzfse-decompresses mip data stored in the .bin file.
void *uncompressData(unsigned char *data, size_t dataLength,
                     uint64_t expectedsize);

namespace {

constexpr int kPrecision = 6;

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
void DecodeBC1Block(const uint8_t* block, uint8_t* rgbaOut) {
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
        if (c0 > c1) {
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

    // Color block (8 bytes): block[8..15] — same as BC1
    uint8_t colorBlock[64]; // 16 pixels × 4 bytes RGBA
    DecodeBC1Block(block + 8, colorBlock);

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
        float z = std::sqrt(std::max(0.0f, 1.0f - x*x - y*y));
        dst[2] = (uint8_t)((z * 0.5f + 0.5f) * 255.0f);
        dst[3] = 255;
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

    return f.good();
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
    if (it == streamingEntryMap.end()) return {};

    const auto& entry = streamingEntries[it->second];
    if (!entry.desc) return {};

    const AAPLTextureData& tex = *entry.desc;

    // Build output filename from the embedded path (strip directories)
    std::string srcPath = tex._path;
    auto slashPos = srcPath.find_last_of("/\\");
    std::string baseName = (slashPos != std::string::npos)
        ? srcPath.substr(slashPos + 1) : srcPath;
    auto dotPos = baseName.find_last_of('.');
    if (dotPos != std::string::npos)
        baseName = baseName.substr(0, dotPos);
    std::string outName = baseName + suffix + ".bmp";
    auto outPath = outputDir / outName;

    if (!mesh->_textureData) return {};

    // Determine BC block size from pixel format.
    uint32_t bcBlockBytes = 8;   // BC1 default
    if (tex._pixelFormat == kBC3_RGBA_sRGB || tex._pixelFormat == kBC5_RGUnorm)
        bcBlockBytes = 16;

    // Walk down mip levels looking for one whose compressed data fits.
    // The .bin stores every mip as an independent lzfse-compressed chunk
    // at _pixelDataOffset + _mipOffsets[mip] with _mipLengths[mip] bytes.
    uint32_t mipW = (uint32_t)tex._width;
    uint32_t mipH = (uint32_t)tex._height;
    int mipLevel = 0;
    void* bcData = nullptr;
    uint32_t blockW = 0, blockH = 0;
    unsigned long long bcBytes = 0;

    for (; mipLevel < (int)tex._mipmapLevelCount; ++mipLevel) {
        if (mipLevel >= (int)tex._mipOffsets.size() ||
            mipLevel >= (int)tex._mipLengths.size())
            break;

        unsigned long long compOff = tex._mipOffsets[mipLevel];
        unsigned long long compLen = tex._mipLengths[mipLevel];
        blockW = ((mipW + 3) / 4 > 0) ? (mipW + 3) / 4 : 1;
        blockH = ((mipH + 3) / 4 > 0) ? (mipH + 3) / 4 : 1;
        bcBytes = (unsigned long long)blockW * blockH * bcBlockBytes;

        unsigned char* raw = (unsigned char*)mesh->_textureData
                             + tex._pixelDataOffset + compOff;
        bcData = uncompressData(raw, (size_t)compLen, bcBytes);
        if (bcData) break;

        spdlog::debug("PbrtExporter: {} mip {} decompress failed ({}->{}), "
                      "trying next", baseName, mipLevel, compLen, bcBytes);
        mipW = (mipW > 1 ? mipW >> 1 : 1);
        mipH = (mipH > 1 ? mipH >> 1 : 1);
    }

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
    out << R"(Film "rgb" "integer xresolution" [)" << width
        << R"(] "integer yresolution" [)" << height << "]\n";
    out << R"(    "string filename" "output.png")" << '\n';
}

void PbrtExporter::WriteMaterials(std::ofstream& out, const GpuScene& scene,
    const std::unordered_map<uint32_t, std::string>& exportedColorTexNames,
    const std::unordered_map<uint32_t, std::string>& exportedRoughTexNames) {
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
        if (mat.hasBaseColorTexture)
            out << Indent(1) << "\"texture reflectance\" \"color_" << i << "\"\n";
        else
            out << Indent(1) << "\"rgb reflectance\" [ "
                << mat.baseColor.x << ' ' << mat.baseColor.y << ' ' << mat.baseColor.z << " ]\n";
        if (mat.hasMetallicRoughnessTexture) {
            auto it = exportedRoughTexNames.find(mat.metallicRoughnessHash);
            if (it != exportedRoughTexNames.end()) {
                out << Indent(1) << "\"texture uroughness\" \"roughness_" << i << "\"\n";
                out << Indent(1) << "\"texture vroughness\" \"roughness_" << i << "\"\n";
            } else {
                float roughness = mat.metallicRoughness.y;
                out << Indent(1) << "\"float uroughness\" [ " << roughness << " ]\n";
                out << Indent(1) << "\"float vroughness\" [ " << roughness << " ]\n";
            }
        } else {
            float roughness = mat.metallicRoughness.y;
            out << Indent(1) << "\"float uroughness\" [ " << roughness << " ]\n";
            out << Indent(1) << "\"float vroughness\" [ " << roughness << " ]\n";
        }
        if (mat.hasNormalMap) {
            auto it = exportedColorTexNames.find(mat.normalMapHash);
            if (it != exportedColorTexNames.end())
                out << Indent(1) << "\"string normalmap\" \"" << it->second << "\"\n";
        }
    }
}

void PbrtExporter::WriteGeometry(std::ofstream& out, const GpuScene& scene) {
    const AAPLMeshData* mesh = scene.applMesh;
    if (!mesh || !scene.m_SubMeshes) {
        spdlog::warn("PbrtExporter: no mesh data to export");
        return;
    }
    const vec3* verts = static_cast<const vec3*>(mesh->_vertexData);
    const vec3* norms = static_cast<const vec3*>(mesh->_normalData);
    const vec2* uvs = static_cast<const vec2*>(mesh->_uvData);
    const void* indices = mesh->_indexData;
    if (!verts || !indices) {
        spdlog::error("PbrtExporter: vertex or index data is null");
        return;
    }
    const int meshCount = static_cast<int>(mesh->_meshCount);
    bool is16bit = (mesh->_indexType == 2);
    for (int m = 0; m < meshCount; ++m) {
        const AAPLSubMesh& submesh = scene.m_SubMeshes[m];
        if (submesh.indexCount == 0) continue;
        if ((submesh.indexCount % 3) != 0)
            spdlog::warn("PbrtExporter: submesh {} has index count {}, not a multiple of 3",
                         m, submesh.indexCount);

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

        uint32_t rangeCount = idxMax - idxMin + 1;

        out << '\n' << Indent(1) << "AttributeBegin\n";
        int matIndex = static_cast<int>(submesh.materialIndex);
        if (matIndex >= 0 && matIndex < static_cast<int>(mesh->_materialCount))
            out << Indent(2) << "NamedMaterial \"material_" << matIndex << "\"\n";
        out << Indent(2) << R"(Shape "trianglemesh")" << '\n';
        out << Indent(2) << "\"point3 P\" ";
        WriteVec3Array(out, verts + idxMin, rangeCount);
        out << '\n';
        if (norms) {
            out << Indent(2) << "\"normal N\" ";
            WriteVec3Array(out, norms + idxMin, rangeCount);
            out << '\n';
        }
        if (uvs) {
            out << Indent(2) << "\"point2 uv\" ";
            WriteVec2Array(out, uvs + idxMin, rangeCount);
            out << '\n';
        }
        out << Indent(2) << "\"integer indices\" ";
        out << "[ ";
        uint32_t indexCountVal = submesh.indexCount;
        if (is16bit) {
            const uint16_t* idx = static_cast<const uint16_t*>(indices) + submesh.indexBegin;
            for (uint32_t k = 0; k < indexCountVal; ++k) {
                if (k > 0 && k % 30 == 0) out << "\n  ";
                out << static_cast<uint32_t>(idx[k] - idxMin) << ' ';
            }
        } else {
            const uint32_t* idx = static_cast<const uint32_t*>(indices) + submesh.indexBegin;
            for (uint32_t k = 0; k < indexCountVal; ++k) {
                if (k > 0 && k % 30 == 0) out << "\n  ";
                out << (idx[k] - idxMin) << ' ';
            }
        }
        out << "]\n";
        out << Indent(1) << "AttributeEnd\n";
    }
}

void PbrtExporter::WriteLights(std::ofstream& out, const GpuScene& scene) {
    const vec3& sunDir = scene.frameConstants.sunDirection;
    const vec3& sunColor = scene.frameConstants.sunColor;
    if (sunColor.x > 0 || sunColor.y > 0 || sunColor.z > 0) {
        // sunDirection is surface→sun; place the distant light source in that direction.
        vec3 from(sunDir.x * 10000.0f, sunDir.y * 10000.0f, sunDir.z * 10000.0f);
        out << '\n' << Indent(1) << R"(LightSource "distant")" << '\n';
        out << Indent(2) << "\"point3 from\" [ " << from << " ]\n";
        out << Indent(2) << "\"rgb L\" [ "
            << sunColor.x << ' ' << sunColor.y << ' ' << sunColor.z << " ]\n";
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
    }
}

// ---- Public API ----

bool PbrtExporter::Export(const GpuScene& scene,
                          const std::filesystem::path& outputPath) {
    try {
        // Step 1: Export texture data to BMP files alongside the .pbrt file
        std::filesystem::path outputDir = outputPath.parent_path();
        if (outputDir.empty()) outputDir = ".";
        std::unordered_map<uint32_t, std::string> exportedColorTexNames; // hash -> color/normalmap BMP filename
        std::unordered_map<uint32_t, std::string> exportedRoughTexNames; // hash -> roughness (G-channel) BMP filename

        if (scene.cpuMaterials && scene.applMesh->_textureData) {
            const int matCount = static_cast<int>(scene.applMesh->_materialCount);
            for (int i = 0; i < matCount; ++i) {
                const AAPLMaterial& mat = scene.cpuMaterials[i];
                // Export color and normal map textures with all channels.
                auto exportTex = [&](uint32_t hash) {
                    if (hash == 0 || exportedColorTexNames.count(hash)) return;
                    std::string name = ExportTextureData(hash,
                        scene.streamingEntryMap, scene.streamingEntries,
                        scene.applMesh, outputDir);
                    if (!name.empty())
                        exportedColorTexNames[hash] = name;
                };
                // Export roughness as G-channel-only greyscale BMP.
                auto exportRoughTex = [&](uint32_t hash) {
                    if (hash == 0 || exportedRoughTexNames.count(hash)) return;
                    std::string name = ExportTextureData(hash,
                        scene.streamingEntryMap, scene.streamingEntries,
                        scene.applMesh, outputDir, /*channel=*/1, /*suffix=*/"_rough");
                    if (!name.empty())
                        exportedRoughTexNames[hash] = name;
                };
                if (mat.hasBaseColorTexture)        exportTex(mat.baseColorTextureHash);
                if (mat.hasMetallicRoughnessTexture) exportRoughTex(mat.metallicRoughnessHash);
                if (mat.hasNormalMap)               exportTex(mat.normalMapHash);
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

        out << R"(Integrator "volpath" "integer maxdepth" [8])" << '\n';
        out << R"(Sampler "sobol" "integer pixelsamples" [4])" << '\n';
        const VkExtent2D& ext = scene.device.getSwapChainExtent();
        WriteFilm(out, ext.width, ext.height);
        WriteCamera(out, scene);
        out << R"(PixelFilter "gaussian" "float xradius" [1.5] "float yradius" [1.5])" << '\n';

        out << '\n' << "WorldBegin\n";

        // Texture declarations with exported BMP filenames (must be inside WorldBegin)
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
                // Normal maps use "string normalmap" directly (not a texture reference)
            }
        }

        WriteMaterials(out, scene, exportedColorTexNames, exportedRoughTexNames);
        WriteGeometry(out, scene);
        WriteLights(out, scene);

        spdlog::info("PbrtExporter: wrote scene to {}", outputPath.string());
        return true;
    } catch (const std::exception& e) {
        spdlog::error("PbrtExporter: exception: {}", e.what());
        return false;
    }
}
