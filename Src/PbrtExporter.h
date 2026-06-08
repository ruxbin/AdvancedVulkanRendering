#pragma once

#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

class GpuScene;
struct TextureStreamingEntry;

class PbrtExporter {
public:
    // Write a pbrt-v4 scene description file at outputPath.
    // Textures are referenced by relative path.
    // Returns true on success, false on error (logged to spdlog).
    static bool Export(const GpuScene& scene,
                       const std::filesystem::path& outputPath);

private:
    static std::string ResolveTexturePath(
        uint32_t hash,
        const std::unordered_map<uint32_t, size_t>& streamingEntryMap,
        const std::vector<TextureStreamingEntry>& streamingEntries);

    static void WriteCamera(std::ofstream& out, const GpuScene& scene);
    static void WriteFilm(std::ofstream& out);
    static void WriteMaterials(std::ofstream& out, const GpuScene& scene);
    static void WriteGeometry(std::ofstream& out, const GpuScene& scene);
    static void WriteLights(std::ofstream& out, const GpuScene& scene);
};
