#include "PbrtExporter.h"
#include "GpuScene.h"
#include "spdlog/spdlog.h"

#include <iomanip>

namespace {

constexpr int kPrecision = 6;

std::ostream& operator<<(std::ostream& os, const vec3& v) {
    os << std::fixed << std::setprecision(kPrecision)
       << v.x << ' ' << v.y << ' ' << v.z;
    return os;
}

std::ostream& operator<<(std::ostream& os, const vec2& v) {
    os << std::fixed << std::setprecision(kPrecision)
       << v.x << ' ' << v.y;
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

void WriteIndexArray(std::ofstream& out, const uint32_t* data, size_t count) {
    out << "[ ";
    for (size_t i = 0; i < count; ++i) {
        if (i > 0 && i % 30 == 0) out << "\n  ";
        out << data[i] << ' ';
    }
    out << ']';
}

void WriteIndexArray16(std::ofstream& out, const uint16_t* data, size_t count) {
    out << "[ ";
    for (size_t i = 0; i < count; ++i) {
        if (i > 0 && i % 30 == 0) out << "\n  ";
        out << static_cast<uint32_t>(data[i]) << ' ';
    }
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

} // anonymous namespace

// ---- PbrtExporter private helpers ----

std::string PbrtExporter::ResolveTexturePath(
    uint32_t hash,
    const std::unordered_map<uint32_t, size_t>& streamingEntryMap,
    const std::vector<TextureStreamingEntry>& streamingEntries)
{
    auto it = streamingEntryMap.find(hash);
    if (it == streamingEntryMap.end()) return {};
    const auto& entry = streamingEntries[it->second];
    if (!entry.desc || entry.desc->_path.empty()) return {};
    return entry.desc->_path;
}

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
    out << R"(Camera "perspective" "float fov" [65])" << '\n';
}

void PbrtExporter::WriteFilm(std::ofstream& out) {
    out << R"(Film "rgb" "integer xresolution" [1224] "integer yresolution" [691])" << '\n';
    out << R"(    "string filename" "output.png")" << '\n';
}

void PbrtExporter::WriteMaterials(std::ofstream& out, const GpuScene& scene) {
    if (!scene.cpuMaterials || scene.applMesh->_materialCount == 0) {
        spdlog::warn("PbrtExporter: no materials to export");
        return;
    }
    const int materialCount = static_cast<int>(scene.applMesh->_materialCount);
    for (int i = 0; i < materialCount; ++i) {
        const AAPLMaterial& mat = scene.cpuMaterials[i];
        out << Indent(1) << "MakeNamedMaterial \"material_" << i << "\"\n";
        out << Indent(1) << "\"string type\" \"uber\"\n";
        out << Indent(1) << "\"rgb Kd\" [ "
            << mat.baseColor.x << ' ' << mat.baseColor.y << ' ' << mat.baseColor.z << " ]\n";
        float metallic = mat.metallicRoughness.x;
        float roughness = mat.metallicRoughness.y;
        out << Indent(1) << "\"float metallic\" [ " << metallic << " ]\n";
        out << Indent(1) << "\"float roughness\" [ " << roughness << " ]\n";
        if (mat.opacity < 1.0f)
            out << Indent(1) << "\"float opacity\" [ " << mat.opacity << " ]\n";
        if (mat.emissiveColor.x > 0 || mat.emissiveColor.y > 0 || mat.emissiveColor.z > 0)
            out << Indent(1) << "\"rgb Le\" [ "
                << mat.emissiveColor.x << ' ' << mat.emissiveColor.y << ' ' << mat.emissiveColor.z << " ]\n";
        if (mat.hasBaseColorTexture) {
            std::string path = ResolveTexturePath(mat.baseColorTextureHash,
                scene.streamingEntryMap, scene.streamingEntries);
            if (!path.empty())
                out << Indent(1) << "\"texture Kd\" \"color_" << i << "\"\n";
        }
        if (mat.hasMetallicRoughnessTexture) {
            std::string path = ResolveTexturePath(mat.metallicRoughnessHash,
                scene.streamingEntryMap, scene.streamingEntries);
            if (!path.empty())
                out << Indent(1) << "\"texture roughness\" \"roughness_" << i << "\"\n";
        }
        if (mat.hasNormalMap) {
            std::string path = ResolveTexturePath(mat.normalMapHash,
                scene.streamingEntryMap, scene.streamingEntries);
            if (!path.empty())
                out << Indent(1) << "\"texture bumpmap\" \"normal_" << i << "\"\n";
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
    size_t vertexCount = mesh->_vertexCount;
    bool is16bit = (mesh->_indexType == 2);
    for (int m = 0; m < meshCount; ++m) {
        const AAPLSubMesh& submesh = scene.m_SubMeshes[m];
        if((submesh.indexCount)%3!=0)
            spdlog::warn("PbrtExporter: submesh {} has index count {}, which is not a multiple of 3",
                         m, submesh.indexCount);
        size_t submeshVertexCount = submesh.indexCount / 3;
        if (submesh.indexCount == 0) continue;
        out << '\n' << Indent(1) << "AttributeBegin\n";
        int matIndex = static_cast<int>(submesh.materialIndex);
        if (matIndex >= 0 && matIndex < static_cast<int>(mesh->_materialCount))
            out << Indent(2) << "NamedMaterial \"material_" << matIndex << "\"\n";
        out << Indent(2) << R"(Shape "trianglemesh")" << '\n';
        out << Indent(2) << "\"point3 P\" ";
        WriteVec3Array(out, verts, submeshVertexCount);
        out << '\n';
        if (norms) {
            out << Indent(2) << "\"normal N\" ";
            WriteVec3Array(out, norms, submeshVertexCount);
            out << '\n';
        }
        if (uvs) {
            out << Indent(2) << "\"float uv\" ";
            WriteVec2Array(out, uvs, submeshVertexCount);
            out << '\n';
        }
        out << Indent(2) << "\"integer indices\" ";
        uint32_t indexBegin = submesh.indexBegin;
        uint32_t indexCountVal = submesh.indexCount;
        if (is16bit) {
            const uint16_t* idx = static_cast<const uint16_t*>(indices) + indexBegin;
            WriteIndexArray16(out, idx, indexCountVal);
        } else {
            const uint32_t* idx = static_cast<const uint32_t*>(indices) + indexBegin;
            WriteIndexArray(out, idx, indexCountVal);
        }
        out << '\n';
        out << Indent(1) << "AttributeEnd\n";
    }
}

void PbrtExporter::WriteLights(std::ofstream& out, const GpuScene& scene) {
    // Directional sun
    const vec3& sunDir = scene.frameConstants.sunDirection;
    const vec3& sunColor = scene.frameConstants.sunColor;
    if (sunColor.x > 0 || sunColor.y > 0 || sunColor.z > 0) {
        vec3 from(-sunDir.x * 10000.0f, -sunDir.y * 10000.0f, -sunDir.z * 10000.0f);
        out << '\n' << Indent(1) << R"(LightSource "distant")" << '\n';
        out << Indent(2) << "\"point from\" [ " << from << " ]\n";
        out << Indent(2) << "\"rgb L\" [ "
            << sunColor.x << ' ' << sunColor.y << ' ' << sunColor.z << " ]\n";
    }
    // Point lights
    for (const auto& pl : scene._pointLights) {
        const PointLightData* d = pl.getPointLightData();
        if (!d) continue;
        vec3 pos(d->posSqrRadius.x, d->posSqrRadius.y, d->posSqrRadius.z);
        float intensity = scene.frameConstants.localLightIntensity;
        out << '\n' << Indent(1) << R"(LightSource "point")" << '\n';
        out << Indent(2) << "\"point from\" [ " << pos << " ]\n";
        out << Indent(2) << "\"rgb I\" [ "
            << d->color.x * intensity << ' '
            << d->color.y * intensity << ' '
            << d->color.z * intensity << " ]\n";
    }
    // Spot lights
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
        out << Indent(2) << "\"point from\" [ " << pos << " ]\n";
        out << Indent(2) << "\"point to\" [ " << lookAt << " ]\n";
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
        std::ofstream out(outputPath);
        if (!out.is_open()) {
            spdlog::error("PbrtExporter: failed to open {} for writing",
                          outputPath.string());
            return false;
        }
        out << std::fixed << std::setprecision(kPrecision);

        out << R"(Integrator "volpath" "integer maxdepth" [8])" << '\n';
        out << R"(Sampler "sobol" "integer pixelsamples" [4])" << '\n';
        WriteFilm(out);
        WriteCamera(out, scene);
        out << R"(PixelFilter "gaussian" "float xradius" [1.5] "float yradius" [1.5])" << '\n';

        // Texture declarations before WorldBegin
        if (scene.cpuMaterials) {
            const int matCount = static_cast<int>(scene.applMesh->_materialCount);
            for (int i = 0; i < matCount; ++i) {
                const AAPLMaterial& mat = scene.cpuMaterials[i];
                if (mat.hasBaseColorTexture) {
                    std::string path = ResolveTexturePath(mat.baseColorTextureHash,
                        scene.streamingEntryMap, scene.streamingEntries);
                    if (!path.empty()) {
                        out << "Texture \"color_" << i << R"(" "color" "imagemap")" << '\n';
                        out << Indent(1) << "\"string filename\" \"" << path << "\"\n";
                    }
                }
                if (mat.hasMetallicRoughnessTexture) {
                    std::string path = ResolveTexturePath(mat.metallicRoughnessHash,
                        scene.streamingEntryMap, scene.streamingEntries);
                    if (!path.empty()) {
                        out << "Texture \"roughness_" << i << R"(" "color" "imagemap")" << '\n';
                        out << Indent(1) << "\"string filename\" \"" << path << "\"\n";
                    }
                }
                if (mat.hasNormalMap) {
                    std::string path = ResolveTexturePath(mat.normalMapHash,
                        scene.streamingEntryMap, scene.streamingEntries);
                    if (!path.empty()) {
                        out << "Texture \"normal_" << i << R"(" "color" "imagemap")" << '\n';
                        out << Indent(1) << "\"string filename\" \"" << path << "\"\n";
                    }
                }
            }
        }

        out << '\n' << "WorldBegin\n";

        WriteMaterials(out, scene);
        WriteGeometry(out, scene);
        WriteLights(out, scene);

        out << "WorldEnd\n";

        spdlog::info("PbrtExporter: wrote scene to {}", outputPath.string());
        return true;
    } catch (const std::exception& e) {
        spdlog::error("PbrtExporter: exception: {}", e.what());
        return false;
    }
}
