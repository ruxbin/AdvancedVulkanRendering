# PBRT Scene Exporter — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Export the current Vulkan rendering scene to pbrt-v4 `.pbrt` format for offline CPU path tracing.

**Architecture:** A new `PbrtExporter` class (one `.h` + one `.cpp`) that reads GpuScene private members via friend declaration and writes a text-format pbrt scene file. An ImGui button in the existing overlay triggers the export.

**Tech Stack:** C++17, nlohmann/json (already in project), lzfse (already in project), pbrt-v4 text scene format

---

## File Map

| File | Action | Purpose |
|------|--------|---------|
| `Src/PbrtExporter.h` | Create | Class declaration |
| `Src/PbrtExporter.cpp` | Create | Implementation |
| `Src/Include/GpuScene.h` | Modify | Add `friend class PbrtExporter;` |
| `Src/GpuScene.cpp` | Modify | Add ImGui export button |
| `Src/CMakeLists.txt` | Modify | Add `PbrtExporter.cpp` to SOURCES |

---

### Task 1: Create PbrtExporter header

**Files:**
- Create: `Src/PbrtExporter.h`

- [ ] **Step 1: Write the header**

```cpp
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
```

- [ ] **Step 2: Commit**

```bash
git add Src/PbrtExporter.h
git commit -m "feat: add PbrtExporter header"
```

---

### Task 2: Add friend declaration to GpuScene

**Files:**
- Modify: `Src/Include/GpuScene.h`

- [ ] **Step 1: Add friend declaration**

In `GpuScene.h`, find the existing friend declarations (around line 958):

```cpp
  friend class Shadow;
  friend class PointLight;
  friend class SpotLight;
  friend class LightCuller;
  friend class RayTracing;
```

Add after `friend class RayTracing;`:

```cpp
  friend class PbrtExporter;
```

- [ ] **Step 2: Commit**

```bash
git add Src/Include/GpuScene.h
git commit -m "feat: add PbrtExporter as friend of GpuScene"
```

---

### Task 3: Create PbrtExporter implementation — helpers

**Files:**
- Create: `Src/PbrtExporter.cpp`

- [ ] **Step 1: Write includes and file-writing helpers**

```cpp
#include "PbrtExporter.h"
#include "GpuScene.h"
#include "spdlog/spdlog.h"

#include <fstream>
#include <iomanip>
#include <sstream>

namespace {

// ---- pbrt value formatters ----

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

// Write a vector of vec3 values as a pbrt array literal: [ x0 y0 z0  x1 y1 z1 ... ]
template<typename T>
void WriteVec3Array(std::ofstream& out, const T* data, size_t count) {
    out << "[ ";
    for (size_t i = 0; i < count; ++i)
        out << data[i] << ' ';
    out << ']';
}

// Write a vector of vec2 values as a pbrt array literal: [ u0 v0  u1 v1 ... ]
void WriteVec2Array(std::ofstream& out, const vec2* data, size_t count) {
    out << "[ ";
    for (size_t i = 0; i < count; ++i)
        out << data[i] << ' ';
    out << ']';
}

// Write a vector of uint32_t indices as a pbrt array literal: [ i0 i1 i2 ... ]
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

// Indent helper
struct Indent {
    int level;
    explicit Indent(int n) : level(n) {}
    friend std::ostream& operator<<(std::ostream& os, const Indent& ind) {
        for (int i = 0; i < ind.level * 4; ++i) os.put(' ');
        return os;
    }
};

// Resolve a texture path from a hash via streaming entries.
// Returns empty string if not found.
std::string ResolveTexturePath(
    uint32_t hash,
    const std::unordered_map<uint32_t, size_t>& streamingEntryMap,
    const std::vector<TextureStreamingEntry>& streamingEntries)
{
    auto it = streamingEntryMap.find(hash);
    if (it == streamingEntryMap.end()) return {};
    const auto& entry = streamingEntries[it->second];
    if (!entry.desc || entry.desc->_path.empty()) return {};
    // _path is set by the .bin loader; make it relative to the scene root
    return entry.desc->_path;
}

} // anonymous namespace
```

- [ ] **Step 2: Commit**

```bash
git add Src/PbrtExporter.cpp
git commit -m "feat: add PbrtExporter helpers and formatters"
```

---

### Task 4: PbrtExporter — camera and film output

**Files:**
- Modify: `Src/PbrtExporter.cpp` (append to file)

- [ ] **Step 1: Write camera export function**

Add before the closing anonymous namespace:

```cpp
void WriteCamera(std::ofstream& out, const GpuScene& scene) {
    const Camera* cam = scene.maincamera;
    if (!cam) {
        spdlog::warn("PbrtExporter: no camera; using defaults");
        out << "LookAt 0 0 0  0 0 1  0 1 0\n";
        out << R"(Camera "perspective" "float fov" [65])" << '\n';
        return;
    }

    const vec3& eye = cam->GetOrigin();
    vec3 dir = cam->GetCameraDir();
    vec3 lookAt = eye + dir;
    // camera_up * -1: the scene loader negates up; undo for pbrt
    vec3 up = vec3(-0.008532071f, 0.999956667f, 0.003697905f);

    out << "LookAt " << eye << ' ' << lookAt << ' ' << up << '\n';
    // FOV is hardcoded 65 degrees in GpuScene constructor
    out << R"(Camera "perspective" "float fov" [65])" << '\n';
}

void WriteFilm(std::ofstream& out) {
    // swapchain resolution is 1224x691 per Common.h
    out << R"(Film "rgb" "integer xresolution" [1224] "integer yresolution" [691])" << '\n';
    out << R"(    "string filename" "output.png")" << '\n';
}
```

- [ ] **Step 2: Commit**

```bash
git add Src/PbrtExporter.cpp
git commit -m "feat: PbrtExporter camera and film output"
```

---

### Task 5: PbrtExporter — material output

**Files:**
- Modify: `Src/PbrtExporter.cpp` (append)

- [ ] **Step 1: Write material export function**

```cpp
void WriteMaterials(std::ofstream& out, const GpuScene& scene) {
    if (!scene.cpuMaterials || scene.applMesh->_materialCount == 0) {
        spdlog::warn("PbrtExporter: no materials to export");
        return;
    }

    const int materialCount = static_cast<int>(scene.applMesh->_materialCount);
    for (int i = 0; i < materialCount; ++i) {
        const AAPLMaterial& mat = scene.cpuMaterials[i];

        out << Indent(1) << "MakeNamedMaterial \"material_" << i << "\"\n";
        out << Indent(1) << "\"string type\" \"uber\"\n";

        // Base color
        out << Indent(1) << "\"rgb Kd\" [ "
            << mat.baseColor.x << ' '
            << mat.baseColor.y << ' '
            << mat.baseColor.z << " ]\n";

        // Metallic-roughness: metallicRoughness.x = metallic, .y = roughness
        float metallic = mat.metallicRoughness.x;
        float roughness = mat.metallicRoughness.y;
        out << Indent(1) << "\"float metallic\" [ " << metallic << " ]\n";
        out << Indent(1) << "\"float roughness\" [ " << roughness << " ]\n";

        // Opacity
        if (mat.opacity < 1.0f)
            out << Indent(1) << "\"float opacity\" [ " << mat.opacity << " ]\n";

        // Emissive
        if (mat.emissiveColor.x > 0 || mat.emissiveColor.y > 0 || mat.emissiveColor.z > 0)
            out << Indent(1) << "\"rgb Le\" [ "
                << mat.emissiveColor.x << ' '
                << mat.emissiveColor.y << ' '
                << mat.emissiveColor.z << " ]\n";

        // Textures
        if (mat.hasBaseColorTexture) {
            std::string path = ResolveTexturePath(mat.baseColorTextureHash,
                scene.streamingEntryMap, scene.streamingEntries);
            if (!path.empty()) {
                out << Indent(1) << "\"texture Kd\" \"color_" << i << "\"\n";
            }
        }
        if (mat.hasMetallicRoughnessTexture) {
            std::string path = ResolveTexturePath(mat.metallicRoughnessHash,
                scene.streamingEntryMap, scene.streamingEntries);
            if (!path.empty()) {
                out << Indent(1) << "\"texture roughness\" \"roughness_" << i << "\"\n";
            }
        }
        if (mat.hasNormalMap) {
            std::string path = ResolveTexturePath(mat.normalMapHash,
                scene.streamingEntryMap, scene.streamingEntries);
            if (!path.empty()) {
                out << Indent(1) << "\"texture bumpmap\" \"normal_" << i << "\"\n";
            }
        }
    }
}
```

- [ ] **Step 2: Commit**

```bash
git add Src/PbrtExporter.cpp
git commit -m "feat: PbrtExporter material output"
```

---

### Task 6: PbrtExporter — geometry output

**Files:**
- Modify: `Src/PbrtExporter.cpp` (append)

- [ ] **Step 1: Write geometry export function**

```cpp
void WriteGeometry(std::ofstream& out, const GpuScene& scene) {
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
    for (int m = 0; m < meshCount; ++m) {
        const AAPLSubMesh& submesh = scene.m_SubMeshes[m];
        if (submesh.indexCount == 0) continue;

        out << '\n' << Indent(1) << "AttributeBegin\n";

        int matIndex = static_cast<int>(submesh.materialIndex);
        if (matIndex >= 0 && matIndex < static_cast<int>(mesh->_materialCount))
            out << Indent(2) << "NamedMaterial \"material_" << matIndex << "\"\n";

        // Collect unique vertices referenced by this submesh.
        // The submesh's index range is [indexBegin, indexBegin + indexCount).
        // We emit all vertices in that index range and remap indices.
        uint32_t indexBegin = submesh.indexBegin;
        uint32_t indexCountVal = submesh.indexCount;

        // Check index type: 32-bit or 16-bit
        bool is16bit = (mesh->_indexType == 2);  // 2 bytes per index

        // For simplicity, emit all vertices (0..vertexCount) referenced in the
        // submesh. Then emit indices as-is (they're relative to the full vertex buffer).
        // This avoids remapping but increases file size.
        //
        // Vertices
        out << Indent(2) << R"(Shape "trianglemesh")" << '\n';

        // Since pbrt expects indices referencing the vertex array we provide,
        // and we provide ALL vertices, we can use indices as-is.
        size_t vertexCount = mesh->_vertexCount;

        out << Indent(2) << "\"point3 P\" ";
        WriteVec3Array(out, verts, vertexCount);
        out << '\n';

        if (norms) {
            out << Indent(2) << "\"normal N\" ";
            WriteVec3Array(out, norms, vertexCount);
            out << '\n';
        }

        if (uvs) {
            out << Indent(2) << "\"float uv\" ";
            WriteVec2Array(out, uvs, vertexCount);
            out << '\n';
        }

        // Indices for this submesh
        out << Indent(2) << "\"integer indices\" ";
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
```

- [ ] **Step 2: Commit**

```bash
git add Src/PbrtExporter.cpp
git commit -m "feat: PbrtExporter geometry output"
```

---

### Task 7: PbrtExporter — lights output

**Files:**
- Modify: `Src/PbrtExporter.cpp` (append)

- [ ] **Step 1: Write lights export function**

```cpp
void WriteLights(std::ofstream& out, const GpuScene& scene) {
    // Directional sun light
    const vec3& sunDir = scene.frameConstants.sunDirection;
    const vec3& sunColor = scene.frameConstants.sunColor;
    if (sunColor.x > 0 || sunColor.y > 0 || sunColor.z > 0) {
        // pbrt "distant" light: point FROM the light direction * -large
        vec3 from = vec3(-sunDir.x * 10000, -sunDir.y * 10000, -sunDir.z * 10000);
        out << '\n' << Indent(1) << R"(LightSource "distant")" << '\n';
        out << Indent(2) << "\"point from\" [ " << from << " ]\n";
        out << Indent(2) << "\"rgb L\" [ "
            << sunColor.x << ' '
            << sunColor.y << ' '
            << sunColor.z << " ]\n";
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
        vec3 lookAt(pos.x + dir.x * 10, pos.y + dir.y * 10, pos.z + dir.z * 10);

        // cone angle: outer angle is stored in dirAndOuterAngle.w (radians)
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
```

- [ ] **Step 2: Commit**

```bash
git add Src/PbrtExporter.cpp
git commit -m "feat: PbrtExporter lights output"
```

---

### Task 8: PbrtExporter — main Export function

**Files:**
- Modify: `Src/PbrtExporter.cpp` (append after helpers, before the closing `}` of the anonymous namespace)

- [ ] **Step 1: Write main Export function**

```cpp
} // anonymous namespace

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

        // Header
        out << R"(Integrator "volpath" "integer maxdepth" [8])" << '\n';
        out << R"(Sampler "sobol" "integer pixelsamples" [4])" << '\n';
        WriteFilm(out);
        WriteCamera(out, scene);
        out << R"(PixelFilter "gaussian" "float xradius" [1.5] "float yradius" [1.5])" << '\n';

        // Texture declarations (for materials that reference textures)
        if (scene.cpuMaterials) {
            const int matCount = static_cast<int>(scene.applMesh->_materialCount);
            for (int i = 0; i < matCount; ++i) {
                const AAPLMaterial& mat = scene.cpuMaterials[i];

                auto emitTex = [&](uint32_t hash, const char* texName) {
                    std::string path = ResolveTexturePath(hash,
                        scene.streamingEntryMap, scene.streamingEntries);
                    if (!path.empty()) {
                        out << R"(Texture ")" << texName << i << R"(" "color" "imagemap")" << '\n';
                        out << Indent(1) << R"("string filename" ")" << path << R"(")" << '\n';
                    }
                };

                if (mat.hasBaseColorTexture)
                    emitTex(mat.baseColorTextureHash, "color_");
                if (mat.hasMetallicRoughnessTexture)
                    emitTex(mat.metallicRoughnessHash, "roughness_");
                if (mat.hasNormalMap)
                    emitTex(mat.normalMapHash, "normal_");
            }
        }

        out << '\n' << "WorldBegin\n";

        WriteMaterials(out, scene);
        WriteGeometry(out, scene);
        WriteLights(out, scene);

        out << "WorldEnd\n";

        spdlog::info("PbrtExporter: wrote scene to {}",
                     outputPath.string());
        return true;
    } catch (const std::exception& e) {
        spdlog::error("PbrtExporter: exception: {}", e.what());
        return false;
    }
}
```

Note: The temporary `emitTex` lambda captures `texture` path resolution and outputs the texture declaration. The `Texture` block needs to be placed before `WorldBegin`.

- [ ] **Step 2: Commit**

```bash
git add Src/PbrtExporter.cpp
git commit -m "feat: PbrtExporter main Export function"
```

---

### Task 9: Add ImGui export button

**Files:**
- Modify: `Src/GpuScene.cpp`

- [ ] **Step 1: Add ImGui button and include**

Add `#include "PbrtExporter.h"` at the top of `GpuScene.cpp` (near other includes, around line 1-25).

Find the ImGui overlay section (around line 8489 in `renderImGuiOverlay`). After the existing `ImGui::Checkbox("Spot Light Cones", ...)` line, add:

```cpp
  ImGui::Separator();
  if (ImGui::Button("Export PBRT")) {
      bool ok = PbrtExporter::Export(*this, _rootPath / "scene.pbrt");
      if (ok) {
          spdlog::info("Exported scene to scene.pbrt");
      } else {
          spdlog::error("PBRT export failed");
      }
  }
```

- [ ] **Step 2: Commit**

```bash
git add Src/GpuScene.cpp
git commit -m "feat: add ImGui button for PBRT export"
```

---

### Task 10: Add PbrtExporter.cpp to CMakeLists.txt

**Files:**
- Modify: `Src/CMakeLists.txt`

- [ ] **Step 1: Add source file**

In the `set(SOURCES ...)` block, add after the Raytracing.cpp line:

```cmake
    ${CMAKE_CURRENT_LIST_DIR}/PbrtExporter.cpp
```

The relevant section should look like:

```cmake
set(SOURCES
    ${CMAKE_CURRENT_LIST_DIR}/Camera.cpp
    ${CMAKE_CURRENT_LIST_DIR}/Window.cpp
    ${CMAKE_CURRENT_LIST_DIR}/Mesh.cpp
    ${CMAKE_CURRENT_LIST_DIR}/VulkanSetup.cpp
    ${CMAKE_CURRENT_LIST_DIR}/GpuScene.cpp
    ${CMAKE_CURRENT_LIST_DIR}/ScatteringVolume.cpp
    ${CMAKE_CURRENT_LIST_DIR}/ObjLoader.cpp
    ${CMAKE_CURRENT_LIST_DIR}/Shadow.cpp
    ${CMAKE_CURRENT_LIST_DIR}/Light.cpp
    ${CMAKE_CURRENT_LIST_DIR}/AssetLoader.cpp
    ${CMAKE_CURRENT_LIST_DIR}/Raytracing.cpp
    ${CMAKE_CURRENT_LIST_DIR}/PbrtExporter.cpp
    ...
)
```

- [ ] **Step 2: Commit**

```bash
git add Src/CMakeLists.txt
git commit -m "build: add PbrtExporter.cpp to CMakeLists"
```

---

### Task 11: Build and verify compilation

**Files:**
- None (verification only)

- [ ] **Step 1: Run CMake configuration and build**

```bash
cmake --build build --target AdvancedVulkanRendering --config Debug
```

Expected: no compilation errors. The build should complete successfully.

If there are compilation errors (e.g., `operator<<` ambiguity for vec2/vec3), add inline `friend` stream operators to the vec2/vec3 classes in `Matrix.h` or use explicit formatting functions instead.

- [ ] **Step 2: Verify the export button appears**

Launch the application. The ImGui overlay ("Culling Stats" window) should now show an "Export PBRT" button below the Spot Light Cones checkbox and a separator.

- [ ] **Step 3: Click Export and verify output file**

Click the Export PBRT button. Verify:
1. `scene.pbrt` appears next to `scene.scene`
2. File is non-empty
3. File begins with `Integrator "volpath"`

- [ ] **Step 4: Commit any fixes**

If any compilation or runtime issues were fixed, commit them.

---

### Task 12: End-to-end validation with pbrt

**Files:**
- None (verification only)

- [ ] **Step 1: Attempt to render with pbrt**

```bash
cd <scene-directory>
D:\SourceCode\pbrt-v4\build\pbrt.exe scene.pbrt
```

Expected: pbrt loads the scene and begins rendering. Check for:
- No parse errors about malformed arrays
- Triangle count matches expected submesh count
- Material names reference correctly
- Light sources are recognized

- [ ] **Step 2: Fix any pbrt parse errors**

Common issues to watch for:
- Missing closing brackets in arrays
- Incorrect vec2 format (pbrt expects `"float uv"` not `"point2 uv"`)
- Index out of bounds (ensure indices don't exceed vertex count)
- Incorrect `metallicRoughness` component mapping (verify `.x` = metallic, `.y` = roughness matches the AAPLMaterial convention)

If fixes are needed, edit `PbrtExporter.cpp` and commit:

```bash
git add Src/PbrtExporter.cpp
git commit -m "fix: pbrt parse errors in exported scene"
```
