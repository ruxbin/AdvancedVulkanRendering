# PBRT Scene Exporter — Design

2026-06-08

## Overview

Export the current Vulkan rendering scene to pbrt-v4 `.pbrt` format for offline CPU path tracing. A new `PbrtExporter` class reads scene data from the existing `GpuScene` and writes a self-contained pbrt scene description text file.

## Architecture

New files:
- `Src/PbrtExporter.h` — class declaration
- `Src/PbrtExporter.cpp` — implementation

```cpp
class PbrtExporter {
public:
  // Write scene.pbrt to outputPath (directory of scene.scene)
  static void Export(const GpuScene& scene,
                     const std::filesystem::path& outputPath);
};
```

`GpuScene` declares `PbrtExporter` as a friend so it can read private members (mesh data, streaming entries, material arrays, lights, camera, frame constants). No public accessor proliferation needed.

Existing file changes (minimal):
- `Src/GpuScene.h` — add `friend class PbrtExporter;`
- `Src/GpuScene.cpp` — add one ImGui button wiring the export call
- `Src/CMakeLists.txt` — add new source file

The exporter is read-only: it reads GpuScene data, writes a text file. No render pipeline or Vulkan state involved.

## Output

A `scene.pbrt` text file written next to `scene.scene`. Textures are referenced by relative paths (no copying). Structure:

```
Integrator "volpath" "integer maxdepth" [8]
Sampler "sobol" "integer pixelsamples" [4]
Film "image" ...
LookAt ... 
Camera "perspective" "float fov" [65]

WorldBegin
  # per-submesh triangle mesh blocks, grouped by material
  AttributeBegin
    NamedMaterial "material_0"
    Shape "trianglemesh" ...
  AttributeEnd
    
  # lights
  LightSource "distant" ...
  LightSource "point" ...
  LightSource "spot" ...
WorldEnd
```

## Geometry

Each `AAPLSubMesh` becomes one `Shape "trianglemesh"`:

- **Vertices**: decompressed from `.bin` file's lzfse-compressed vertex buffer
- **Normals**: decompressed from normal buffer (or computed via face normals if missing)
- **UVs**: decompressed from UV buffer; wrap to `[0,1]`
- **Indices**: from submesh's `indexBegin`/`indexCount` range. Convert 32-bit -> 16-bit if needed (pbrt expects a flat `integer` array).

Vertex data is extracted for the submesh's index range to avoid duplicating the full buffer per-submesh.

## Materials

`AAPLMaterial` mapped to pbrt `"uber"` BSDF:

| Source | pbrt parameter |
|--------|---------------|
| `baseColor.rgb` | `"rgb Kd"` |
| `metallicRoughness.x` | `"float metallic"` (0=dielectric, 1=metal) |
| `metallicRoughness.y` | `"float roughness"` |
| `emissiveColor.rgb` | `"rgb Le"` |
| `opacity` | `"float opacity"` |

Textures:
- Material stores texture hashes, not paths. Resolve via GpuScene's `streamingEntryMap` (hash → `TextureStreamingEntry` → `AAPLTextureData::_path`). Access through public getter or friend declaration.
- With texture path found: emit `Texture "tex_name" "color" "imagemap" "string filename" "textures/name.png"` and reference via `"texture Kd"`, `"texture bumpmap"` etc.
- Without texture path: use parameterized material values only (base color, roughness, metallic).

Each unique material becomes a `MakeNamedMaterial "material_N"` block before `WorldBegin`.

## Lights

### Directional sun

```
LightSource "distant"
  "point from" [ sx sy sz ]
  "rgb L" [ r g b ]
```

`(sx, sy, sz)` = `-sunDirection * 1000` (far enough for scene bounds).  
`(r, g, b)` = `sunColor` from `FrameConstants`.

### Point lights

Each `point_light` entry in `scene.scene`:

```
LightSource "point"
  "point from" [ px py pz ]
  "rgb I" [ r g b ]
```

Intensity scaled by `localLightIntensity` frame constant.  
`sqrt_radius` is culling metadata — not exported.

### Spot lights

Each `spot_light` entry:

```
LightSource "spot"
  "point from" [ px py pz ]
  "point to" [ px+dx*10 py+dy*10 pz+dz*10 ]
  "float coneangle" [ coneRad_degrees ]
  "rgb I" [ r g b ]
```

Cone angle converted radians → degrees. `height` and `for_transparent` are renderer internals — not exported.

## Camera

```
LookAt  eye_x eye_y eye_z  look_x look_y look_z  up_x up_y up_z
Camera "perspective" "float fov" [65]
```

- Eye: `camera_position`
- Look-at: `camera_position + camera_direction`
- Up: `camera_up`
- FOV: 65° (hardcoded constant in GpuScene constructor)

Film resolution defaults to swapchain extent (1224×691). Film block:

```
Film "rgb" "integer xresolution" [1224] "integer yresolution" [691]
  "string filename" "output.png"
```

## Trigger

ImGui button in the existing overlay (alongside ray tracing toggle, TAA toggle, etc.):

```
if (ImGui::Button("Export PBRT"))
  PbrtExporter::Export(*this, _rootPath / "scene.pbrt");
```

Show a brief status message on completion or error.

## Error handling

- Missing data (no mesh loaded): log error, don't crash
- Texture path resolution failure: warn, continue with parameterized material
- File write failure: log error with path
- All errors are non-fatal — renderer continues running

## Not in scope

- Medium/volumetric export (scatter volume)
- Decal export
- Animation or animated transforms
- Image output format configuration (uses PNG default)
- Incremental or partial export
