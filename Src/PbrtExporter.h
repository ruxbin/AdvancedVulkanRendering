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
    // Realtime point-light policy: omit emissive components enclosing
    // a point light, and resources used only by those components.
    // Returns true on success, false on error (logged to spdlog).
    static bool Export(const GpuScene& scene,
                       const std::filesystem::path& outputPath);

private:
    static void WriteCamera(std::ofstream& out, const GpuScene& scene);
    static void WriteFilm(std::ofstream& out, uint32_t width, uint32_t height);
    static void WriteMaterials(std::ofstream& out, const GpuScene& scene,
                                const std::vector<bool>& usedMaterials,
                                const std::unordered_map<uint32_t, std::string>& exportedColorTexNames,
                                const std::unordered_map<uint32_t, std::string>& exportedRoughTexNames,
                                const std::unordered_map<uint32_t, std::string>& exportedNormalTexNames);
    static bool WriteGeometry(std::ofstream& out, const GpuScene& scene,
                               const std::filesystem::path& outputDir,
                               const std::unordered_map<int, std::vector<uint32_t>>& filteredIndices,
                               const std::unordered_map<int, std::string>& materialAlphaTexNames,
                               const std::unordered_map<uint32_t, std::string>& exportedEmissiveTexNames);
    static void WriteLights(std::ofstream& out, const GpuScene& scene);
};
