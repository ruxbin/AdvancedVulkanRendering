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
    static void WriteCamera(std::ofstream& out, const GpuScene& scene);
    static void WriteFilm(std::ofstream& out, uint32_t width, uint32_t height);
    static void WriteMaterials(std::ofstream& out, const GpuScene& scene,
                                const std::unordered_map<uint32_t, std::string>& exportedColorTexNames,
                                const std::unordered_map<uint32_t, std::string>& exportedRoughTexNames);
    static void WriteGeometry(std::ofstream& out, const GpuScene& scene);
    static void WriteLights(std::ofstream& out, const GpuScene& scene);
};
