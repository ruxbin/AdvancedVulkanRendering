#pragma once

#include <filesystem>

class GpuScene;

class PbrtExporter {
public:
    // Write a pbrt-v4 scene description file at outputPath.
    // Textures are referenced by relative path.
    // Returns true on success, false on error (logged to spdlog).
    static bool Export(const GpuScene& scene,
                       const std::filesystem::path& outputPath);
};
