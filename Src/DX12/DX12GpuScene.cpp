#ifdef ENABLE_DX12

#include "DX12GpuScene.h"
#include "DX12ScenePolicy.h"
#include "DX12TextureStreamingPolicy.h"
#include "GpuScene.h"      // for AAPLMeshData, AAPLTextureData, readFile
#include "AssetLoader.h"
#include "ThirdParty/lzfse.h"

#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_dx12.h"

#include <algorithm>
#include <cmath>
#include <fstream>

// Reuse readFile from GpuScene linkage
extern std::vector<char> readFile(const std::string& filename);

// Indirect argument record for the GPU-culled draws.
//
// D3D12's SV_InstanceID is 0-based per draw and ignores StartInstanceLocation,
// so the Vulkan scheme of smuggling the chunkIndices slot through firstInstance
// does not reach the shader here. Instead the command signature is
// [CONSTANT(b1), DRAW_INDEXED]: `drawSlot` is patched into the pushConstants
// root constant before each draw and the VS reads chunkIndex[drawSlot].
//
// Must stay in sync with DrawIndexedIndirectCommand in commonstruct.hlsl
// (DX12_BACKEND branch).
struct DX12DrawIndexedArgs {
  uint32_t drawSlot;
  D3D12_DRAW_INDEXED_ARGUMENTS draw;
};
static_assert(sizeof(DX12DrawIndexedArgs) == 24,
              "indirect arg stride must match the [CONSTANT, DRAW_INDEXED] signature");

// Root-constant slot that the command signature patches (3rd uint of
// PushConstants {materialIndex, shadowIndex, drawSlot}).
static constexpr uint32_t DRAW_SLOT_CONSTANT_OFFSET = 2;
static constexpr uint32_t PUSH_CONSTANT_COUNT = 3;

// Shadow cull params - POD structs mirroring shadowcull.hlsl cbuffer layout.
// Kept here (not in header) so buildCascadeFrustum can access them as file-scope types.
struct ShadowPlane {
  vec3 normal;
  float w;
};
struct ShadowFrustum {
  ShadowPlane borders[6];
};
static constexpr uint32_t SHADOW_CULL_CASCADE_COUNT = 3; // matches DX12GpuScene::SHADOW_CASCADE_COUNT
struct ShadowCullParams {
  uint32_t opaqueChunkCount;
  uint32_t alphaMaskedChunkCount;
  uint32_t cascadeMaxChunks;
  uint32_t cascadeCount;
  vec4 cascadeCullThreshold[SHADOW_CULL_CASCADE_COUNT];
  ShadowFrustum cascadeFrustum[SHADOW_CULL_CASCADE_COUNT];
};

// Decompress LZFSE data to malloc'd buffer. Returns {ptr, size}.
// Must match GpuScene.cpp: compressionMode(u32) + 4-byte padding + uncompressedSize(u64) + compressedSize(u64)
struct AAPLCompressionHeader {
  uint32_t compressionMode;
  uint64_t uncompressedSize;
  uint64_t compressedSize;
};

static std::pair<void*, size_t> decompressToHeap(void* compressedData, size_t compressedLength) {
  auto* header = reinterpret_cast<AAPLCompressionHeader*>(compressedData);
  size_t uncompSize = header->uncompressedSize;
  void* dst = malloc(uncompSize);
  lzfse_decode_buffer((uint8_t*)dst, uncompSize,
                      (const uint8_t*)(header + 1), header->compressedSize, nullptr);
  return {dst, uncompSize};
}

// ---- Constructor ----
DX12GpuScene::DX12GpuScene(std::filesystem::path& root, DX12Device& device)
    : _rootPath(root), _device(device) {

  // Load the scene file FIRST 鈥?the camera/sun reads below depend on it.
  // (LoadMeshData used to load it too late; the camera then fell back to
  // the (0,2,5) default, matching the corrupted dx12.rdc capture.)
  LoadSceneFile();

  // Read camera and sun from scene file (matching Vulkan path)
  vec3 cameraPos(0, 2, 5), cameraLookat(0, 0, -1), cameraUp(0, 1, 0);
  if (_sceneFile.contains("camera_position")) {
    cameraPos = vec3(_sceneFile["camera_position"][0].template get<float>(),
                     _sceneFile["camera_position"][1].template get<float>(),
                     _sceneFile["camera_position"][2].template get<float>());
    cameraUp = DX12ScenePolicy::CameraUp(vec3(
        _sceneFile["camera_up"][0].template get<float>(),
        _sceneFile["camera_up"][1].template get<float>(),
        _sceneFile["camera_up"][2].template get<float>()));
    cameraLookat = vec3(_sceneFile["camera_direction"][0].template get<float>(),
                        _sceneFile["camera_direction"][1].template get<float>(),
                        _sceneFile["camera_direction"][2].template get<float>());
  }

  _mainCamera = new Camera(
      65.0f * (3.14159265f / 180.0f), 0.1f, 100.0f,
      cameraPos,
      (float)_device.GetWidth() / _device.GetHeight(),
      cameraLookat,
      cameraUp);

  // Default frame constants
  _frameConstants.sunDirection = vec3(0.3f, 1.0f, 0.5f);
  if (_sceneFile.contains("sun_direction")) {
    _frameConstants.sunDirection = vec3(
        _sceneFile["sun_direction"][0].template get<float>(),
        _sceneFile["sun_direction"][1].template get<float>(),
        _sceneFile["sun_direction"][2].template get<float>());
  }
  _frameConstants.sunColor     = vec3(1.0f, 0.95f, 0.8f);
  _frameConstants.skyColor     = vec3(0.2f, 0.3f, 0.5f);
  _frameConstants.exposure     = 1.0f;
  _frameConstants.emissiveScale     = 1.0f;
  _frameConstants.localLightIntensity = 1.0f;
  // Vulkan parity (GpuScene.h frameConstants init): scatterScale 2.0 (Metal
  // parity, unitless multiplier on scatteringCoeff). Was 0 — the whole scatter
  // pipeline never ran.
  _frameConstants.scatterScale = 2.0f;
  // IBL defaults (written once at init; DX12 shaders don't consume these yet 鈥?
  // values exist only so the field is never read uninitialized).
  _frameConstants.iblScale          = 1.0f;
  _frameConstants.iblSpecularScale  = 4.0f;

  // Init descriptor heaps
  _cbvSrvUavHeap.Init(_device.GetDevice(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
                       2100, 4096, true);
  _samplerHeap.Init(_device.GetDevice(), D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER,
                     16, 0, true);

  // Load mesh data first so per-frame resources can use chunk counts
  LoadMeshData();

  // Create per-frame resources
  _frameResources.resize(_device.GetFrameCount());
  uint32_t maxShadowDraws = (uint32_t)(_applMesh->_opaqueChunkCount + _applMesh->_alphaMaskedChunkCount);
  for (uint32_t i = 0; i < _device.GetFrameCount(); ++i) {
    // Uniform buffer (persistently mapped)
    _frameResources[i].uniformBuffer = DX12Util::CreateUploadBuffer(
        _device.GetDevice(), sizeof(FrameData), &_frameResources[i].uniformMapped);

    // Write index buffer 鈥?upload (for zeroing), GPU default (UAV), readback (CPU read)
    _frameResources[i].writeIndexUpload = DX12Util::CreateUploadBuffer(
        _device.GetDevice(), 16 * sizeof(uint32_t), &_frameResources[i].writeIndexUploadMapped);
    // Zero it initially
    memset(_frameResources[i].writeIndexUploadMapped, 0, 16 * sizeof(uint32_t));

    _frameResources[i].writeIndexBuffer = DX12Util::CreateGPUBuffer(
        _device.GetDevice(), 16 * sizeof(uint32_t),
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);

    // Readback buffer
    {
      D3D12_HEAP_PROPERTIES heapProps = {};
      heapProps.Type = D3D12_HEAP_TYPE_READBACK;
      D3D12_RESOURCE_DESC bufDesc = {};
      bufDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
      bufDesc.Width = 16 * sizeof(uint32_t);
      bufDesc.Height = 1; bufDesc.DepthOrArraySize = 1; bufDesc.MipLevels = 1;
      bufDesc.SampleDesc.Count = 1;
      bufDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
      _device.GetDevice()->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE,
          &bufDesc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
          IID_PPV_ARGS(&_frameResources[i].writeIndexReadback));
    }

    // Cull params (upload buffer)
    _frameResources[i].cullParamsBuffer = DX12Util::CreateUploadBuffer(
        _device.GetDevice(), 512, &_frameResources[i].cullParamsMapped);

    // Shadow cull resources 鈥?shader uses cascadeMaxChunks * 2 per cascade
    // (opaque bucket + alpha bucket each get cascadeMaxChunks slots)
    _frameResources[i].shadowDrawParams = DX12Util::CreateGPUBuffer(
        _device.GetDevice(),
        (UINT64)maxShadowDraws * SHADOW_CASCADE_COUNT * 2 * sizeof(DX12DrawIndexedArgs),
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);

    _frameResources[i].shadowWriteIndexUpload = DX12Util::CreateUploadBuffer(
        _device.GetDevice(), SHADOW_CASCADE_COUNT * 2 * sizeof(uint32_t),
        &_frameResources[i].shadowWriteIndexUploadMapped);
    memset(_frameResources[i].shadowWriteIndexUploadMapped, 0,
           SHADOW_CASCADE_COUNT * 2 * sizeof(uint32_t));

    _frameResources[i].shadowWriteIndex = DX12Util::CreateGPUBuffer(
        _device.GetDevice(), SHADOW_CASCADE_COUNT * 2 * sizeof(uint32_t),
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);

    _frameResources[i].shadowCullParams = DX12Util::CreateUploadBuffer(
        _device.GetDevice(), 512, &_frameResources[i].shadowCullParamsMapped);

    _frameResources[i].shadowChunkIndicesBuffer = DX12Util::CreateGPUBuffer(
        _device.GetDevice(),
        (UINT64)maxShadowDraws * SHADOW_CASCADE_COUNT * 2 * sizeof(uint32_t),
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
  }

  CreateBuffers();
  CreateTextures();
  CreateRootSignatures();
  CreatePipelineStates();
  CreateGBuffers();
  CreateHDRResources();
  CreateStaticDescriptors();
  CreateHiZResources();
  CreateShadowResources();
  CreateSpotShadowResources();
  CreateCommandSignature();
  CreateLights();
  CreateLightCullPipelines();
  CreateScatterResources();
  initTextureStreaming();

  spdlog::info("DX12GpuScene initialized");
}

DX12GpuScene::~DX12GpuScene() {
  _device.WaitForGpu();
  shutdownTextureStreaming(); // join 鍚庡彴绾跨▼,椤诲湪 delete _applMesh 涔嬪墠
  if (_imguiInitialized) {
    // The renderer backend must release its dummy main-viewport data before
    // destroying secondary platform windows (the main viewport has no queue).
    ImGui_ImplDX12_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
  }
  delete _mainCamera;
  free(_subMeshes);
  free(_cpuMaterials);
  delete _applMesh;
}

// ---- Load Scene File (camera, sun, lights, occluders) ----
void DX12GpuScene::LoadSceneFile() {
  // Must match the Vulkan path (GpuScene.cpp reads "scene.scene").
  // NOTE: "bistro.dxt.bin.json" does not exist 鈥?it silently fell back to
  // default camera/sun/lights, producing the all-black dx12.rdc capture.
  std::string scenePath = (_rootPath / "scene.scene").generic_string();
  std::ifstream sceneFileStream(scenePath);
  if (sceneFileStream.is_open()) {
    sceneFileStream >> _sceneFile;
    spdlog::info("DX12: Loaded scene file {}", scenePath);
  } else {
    spdlog::warn("DX12: Scene file not found: {} 鈥?camera/sun/lights will use defaults", scenePath);
  }
}

// ---- Load Mesh Data ----
void DX12GpuScene::LoadMeshData() {
  std::string meshPath = (_rootPath / "bistro.dxt.bin").generic_string();
  _applMesh = new AAPLMeshData(meshPath.c_str());

  spdlog::info("DX12: Loaded mesh - {} vertices, {} indices, {} chunks",
               _applMesh->_vertexCount, _applMesh->_indexCount, _applMesh->_chunkCount);

  // 淇濈暀 submesh 鍖呭洿鐞冧緵 streaming 瑕嗙洊搴﹁绠?瀵归綈 GpuScene.cpp:2773)
  _subMeshes = (AAPLSubMesh*)decompressToHeap(_applMesh->_meshData,
      _applMesh->compressedMeshDataLength).first;
}

// ---- Create Buffers (VB/IB/Chunks/Materials upload to GPU) ----
void DX12GpuScene::CreateBuffers() {
  auto* dev = _device.GetDevice();
  auto* cmdList = _device.GetCommandList();

  // Reset command list for upload work
  _device.GetCommandAllocator(0)->Reset();
  cmdList->Reset(_device.GetCommandAllocator(0), nullptr);

  // Track upload buffers that must stay alive until command list executes
  std::vector<ComPtr<ID3D12Resource>> uploadBuffers;

  auto uploadBuffer = [&](void* data, size_t size) -> ComPtr<ID3D12Resource> {
    ComPtr<ID3D12Resource> upload;
    auto buf = DX12Util::CreateDefaultBuffer(dev, cmdList, data, (UINT64)size, upload);
    uploadBuffers.push_back(upload);
    return buf;
  };

  // --- Decompress and upload vertex data ---
  {
    auto [data, size] = decompressToHeap(_applMesh->_vertexData, _applMesh->compressedVertexDataLength);
    _vertexBuffer = uploadBuffer(data, size);
    free(data);
  }
  {
    auto [data, size] = decompressToHeap(_applMesh->_normalData, _applMesh->compressedNormalDataLength);
    _normalBuffer = uploadBuffer(data, size);
    free(data);
  }
  {
    auto [data, size] = decompressToHeap(_applMesh->_tangentData, _applMesh->compressedTangentDataLength);
    _tangentBuffer = uploadBuffer(data, size);
    free(data);
  }
  {
    auto [data, size] = decompressToHeap(_applMesh->_uvData, _applMesh->compressedUvDataLength);
    _uvBuffer = uploadBuffer(data, size);
    free(data);
  }
  {
    auto [data, size] = decompressToHeap(_applMesh->_indexData, _applMesh->compressedIndexDataLength);
    _indexBuffer = uploadBuffer(data, size);
    free(data);
  }

  // --- Mesh chunks (structured buffer) ---
  {
    auto [data, size] = decompressToHeap(_applMesh->_chunkData, _applMesh->compressedChunkDataLength);
    _meshChunksBuffer = uploadBuffer(data, size);
    // Keep a CPU copy for the spot-shadow direct-draw argument records
    // (indexCount/indexBegin per chunk, mirroring Vulkan's m_Chunks).
    const size_t chunkCount = size / sizeof(AAPLMeshChunk);
    _cpuChunks.resize(chunkCount);
    memcpy(_cpuChunks.data(), data, chunkCount * sizeof(AAPLMeshChunk));
    free(data);
  }

  // --- Materials ---
  {
    // Process materials same as Vulkan path
    auto [matData, matSize] = decompressToHeap(_applMesh->_materialData, _applMesh->compressedMaterialDataLength);
    AAPLMaterial* rawMats = (AAPLMaterial*)matData;

    std::vector<AAPLShaderMaterial> shaderMats(_applMesh->_materialCount);
    // TODO: ConfigureMaterial equivalent - for now just copy indices
    for (size_t i = 0; i < _applMesh->_materialCount; ++i) {
      shaderMats[i].albedo_texture_index = 0;
      shaderMats[i].roughness_texture_index = 0;
      shaderMats[i].normal_texture_index = 0;
      shaderMats[i].emissive_texture_index = 0;
      shaderMats[i].alpha = rawMats[i].opacity;
      shaderMats[i].hasMetallicRoughness = rawMats[i].hasMetallicRoughnessTexture ? 1 : 0;
      shaderMats[i].hasEmissive = rawMats[i].hasEmissiveTexture ? 1 : 0;
    }
    _cpuMaterials = (AAPLMaterial*)matData; // 淇濈暀渚?streaming hash 鏌ヨ,鏋愭瀯閲婃斁

    _materialBuffer = uploadBuffer(shaderMats.data(), shaderMats.size() * sizeof(AAPLShaderMaterial));
  }

  // --- Occluder data from scene file ---
  // occluder_verts is a nested array [[x,y,z],...]; mirror the Vulkan path
  // (GpuScene.cpp:2279-2290): swap y/z and subtract the scene center_offset.
  // NOTE: this code was dead while the scene file never loaded (wrong path);
  // the old `v.get<float>()` also crashed on the nested arrays (nlohmann
  // type_error) once the file started loading.
  if (_sceneFile.contains("occluder_verts") && _sceneFile.contains("occluder_indices")) {
    auto& verts = _sceneFile["occluder_verts"];
    auto& indices = _sceneFile["occluder_indices"];

    vec3 centerOffset(0, 0, 0);
    if (_sceneFile.contains("center_offset")) {
      centerOffset = vec3(_sceneFile["center_offset"][0].template get<float>(),
                          _sceneFile["center_offset"][1].template get<float>(),
                          _sceneFile["center_offset"][2].template get<float>());
    }

    std::vector<float> vertData;
    vertData.reserve(verts.size() * 3);
    for (auto& v : verts) {
      float x = v[0].template get<float>() - centerOffset.x;
      float z = v[1].template get<float>() - centerOffset.z;
      float y = v[2].template get<float>() - centerOffset.y;
      vertData.push_back(x);
      vertData.push_back(y);
      vertData.push_back(z);
    }
    std::vector<uint32_t> idxData;
    idxData.reserve(indices.size());
    for (auto& i : indices) idxData.push_back(i.template get<uint32_t>());

    _occluderVertexBuffer = uploadBuffer(vertData.data(), vertData.size() * sizeof(float));
    _occluderIndexBuffer = uploadBuffer(idxData.data(), idxData.size() * sizeof(uint32_t));
    _occluderIndexCount = (uint32_t)idxData.size();

    spdlog::info("DX12: Loaded {} occluder vertices, {} indices",
                 vertData.size() / 3, idxData.size());
  }

  // --- Per-frame GPU buffers (draw params, chunk indices) ---
  uint32_t totalChunks = (uint32_t)(_applMesh->_opaqueChunkCount +
                                     _applMesh->_alphaMaskedChunkCount +
                                     _applMesh->_transparentChunkCount);
  for (auto& fr : _frameResources) {
    // Draw params buffer (indirect args, needs UAV for compute write)
    fr.drawParamsBuffer = DX12Util::CreateGPUBuffer(
        dev, totalChunks * sizeof(DX12DrawIndexedArgs),
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATE_COMMON);

    // Chunk indices buffer (UAV)
    fr.chunkIndicesBuffer = DX12Util::CreateGPUBuffer(
        dev, totalChunks * sizeof(uint32_t),
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATE_COMMON);
  }

  // Execute upload commands
  cmdList->Close();
  ID3D12CommandList* lists[] = { cmdList };
  _device.GetCommandQueue()->ExecuteCommandLists(1, lists);
  FlushCommandQueue();

  spdlog::info("DX12: All buffers uploaded ({} chunks, {} materials)",
               totalChunks, _applMesh->_materialCount);
}

// ---- Map Metal pixel format to DXGI format ----
static DXGI_FORMAT MapMTLToDXGI(uint32_t mtlFormat) {
  // MTLPixelFormat values from Apple's Metal/MTLPixelFormat.h
  switch (mtlFormat) {
  case 130: return DXGI_FORMAT_BC1_UNORM;       // BC1_RGBA
  case 131: return DXGI_FORMAT_BC1_UNORM_SRGB;  // BC1_RGBA_sRGB
  case 132: return DXGI_FORMAT_BC2_UNORM;       // BC2_RGBA
  case 133: return DXGI_FORMAT_BC2_UNORM_SRGB;  // BC2_RGBA_sRGB
  case 134: return DXGI_FORMAT_BC3_UNORM;       // BC3_RGBA
  case 135: return DXGI_FORMAT_BC3_UNORM_SRGB;  // BC3_RGBA_sRGB
  case 140: return DXGI_FORMAT_BC4_UNORM;       // BC4_RUnorm
  case 141: return DXGI_FORMAT_BC4_SNORM;       // BC4_RSnorm
  case 142: return DXGI_FORMAT_BC5_UNORM;       // BC5_RGUnorm
  case 143: return DXGI_FORMAT_BC5_SNORM;       // BC5_RGSnorm
  case 150: return DXGI_FORMAT_BC6H_SF16;       // BC6H_RGBFloat
  case 151: return DXGI_FORMAT_BC6H_UF16;       // BC6H_RGBUfloat
  case 152: return DXGI_FORMAT_BC7_UNORM;       // BC7_RGBAUnorm
  case 153: return DXGI_FORMAT_BC7_UNORM_SRGB;  // BC7_RGBAUnorm_sRGB
  default:
    spdlog::warn("DX12: Unknown MTL pixel format {}, using BC3_SRGB", mtlFormat);
    return DXGI_FORMAT_BC3_UNORM_SRGB;
  }
}

// ---- Create Textures (bindless) ----
void DX12GpuScene::CreateTextures() {
  auto* dev = _device.GetDevice();
  auto* cmdList = _device.GetCommandList();

  // Reset command list for texture uploads
  _device.GetCommandAllocator(0)->Reset();
  cmdList->Reset(_device.GetCommandAllocator(0), nullptr);

  std::vector<ComPtr<ID3D12Resource>> uploadBuffers;

  for (size_t texIdx = 0; texIdx < _applMesh->_textures.size(); ++texIdx) {
    auto& texData = _applMesh->_textures[texIdx];
    DXGI_FORMAT format = MapMTLToDXGI(texData._pixelFormat);
    uint32_t w = (uint32_t)texData._width;
    uint32_t h = (uint32_t)texData._height;
    uint32_t mipCount = (uint32_t)texData._mipmapLevelCount;

    uint32_t permanentMip = DX12TextureStreamingPolicy::CalculateMinMip(
        w, h, mipCount, DX12TextureStreamingPolicy::PERMANENT_TEXTURE_SIZE);
    uint32_t loadedMipCount = mipCount - permanentMip;
    uint32_t baseW = (w >> permanentMip) > 0 ? (w >> permanentMip) : 1;
    uint32_t baseH = (h >> permanentMip) > 0 ? (h >> permanentMip) : 1;

    // Create texture resource with format from MapMTLToDXGI
    auto tex = DX12Util::CreateTexture2D(dev, baseW, baseH, format,
        D3D12_RESOURCE_FLAG_NONE, loadedMipCount, 1, D3D12_RESOURCE_STATE_COPY_DEST);

    // Upload each mip level
    for (uint32_t srcMip = permanentMip; srcMip < mipCount; ++srcMip) {
      uint32_t mipW = (w >> srcMip) > 1 ? (w >> srcMip) : 1;
      uint32_t mipH = (h >> srcMip) > 1 ? (h >> srcMip) : 1;

      // Get mip data offset and length
      size_t dataOffset = texData._mipOffsets.size() > srcMip ? texData._mipOffsets[srcMip] : 0;
      size_t dataLen = texData._mipLengths.size() > srcMip ? texData._mipLengths[srcMip] : 0;
      if (dataLen == 0) continue;

      // Decompress mip data
      uint8_t* compressedSrc = (uint8_t*)_applMesh->_textureData + texData._pixelDataOffset + dataOffset;
      auto [mipData, mipSize] = decompressToHeap(compressedSrc, dataLen);

      // Calculate row pitch for BC formats (4x4 block compressed)
      uint32_t blockW = (mipW + 3) / 4;
      uint32_t blockH = (mipH + 3) / 4;
      uint32_t bytesPerBlock = (format == DXGI_FORMAT_BC1_UNORM || format == DXGI_FORMAT_BC1_UNORM_SRGB
                                || format == DXGI_FORMAT_BC4_UNORM || format == DXGI_FORMAT_BC4_SNORM) ? 8 : 16;
      uint32_t rowPitch = blockW * bytesPerBlock;
      uint32_t alignedRowPitch = (rowPitch + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) & ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
      uint32_t uploadSize = alignedRowPitch * blockH;

      // Create upload buffer for this mip
      ComPtr<ID3D12Resource> uploadBuf;
      D3D12_HEAP_PROPERTIES uploadHeap = {};
      uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
      D3D12_RESOURCE_DESC bufDesc = {};
      bufDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
      bufDesc.Width = uploadSize;
      bufDesc.Height = 1; bufDesc.DepthOrArraySize = 1; bufDesc.MipLevels = 1;
      bufDesc.SampleDesc.Count = 1;
      bufDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
      dev->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &bufDesc,
          D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&uploadBuf));

      // Copy with row pitch alignment
      void* mapped = nullptr;
      uploadBuf->Map(0, nullptr, &mapped);
      for (uint32_t row = 0; row < blockH; ++row) {
        memcpy((uint8_t*)mapped + row * alignedRowPitch,
               (uint8_t*)mipData + row * rowPitch,
               rowPitch);
      }
      uploadBuf->Unmap(0, nullptr);
      free(mipData);

      // Copy to texture subresource
      D3D12_TEXTURE_COPY_LOCATION dst = {};
      dst.pResource = tex.Get();
      dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
      dst.SubresourceIndex = srcMip - permanentMip;

      D3D12_TEXTURE_COPY_LOCATION src = {};
      src.pResource = uploadBuf.Get();
      src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
      src.PlacedFootprint.Footprint.Format = format;
      src.PlacedFootprint.Footprint.Width = mipW;
      src.PlacedFootprint.Footprint.Height = mipH;
      src.PlacedFootprint.Footprint.Depth = 1;
      src.PlacedFootprint.Footprint.RowPitch = alignedRowPitch;

      cmdList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
      uploadBuffers.push_back(uploadBuf);
    }

    // Transition to SRV
    DX12Util::TransitionBarrier(cmdList, tex.Get(),
        D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    // Create SRV in bindless region
    uint32_t srvIndex = SRV_BINDLESS_START + (uint32_t)texIdx;
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = format;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MipLevels = loadedMipCount;
    dev->CreateShaderResourceView(tex.Get(), &srvDesc,
        _cbvSrvUavHeap.GetStaticCPU(srvIndex));

    _textureHashMap[texData._pathHash] = texIdx;
    _textures.push_back(tex);
  }

  // Execute uploads
  cmdList->Close();
  ID3D12CommandList* lists[] = { cmdList };
  _device.GetCommandQueue()->ExecuteCommandLists(1, lists);
  FlushCommandQueue();

  // Now configure material texture indices
  if (_materialBuffer && !_applMesh->_textures.empty()) {
    // Re-decompress materials to update texture indices
    auto [matData, matSize] = decompressToHeap(_applMesh->_materialData, _applMesh->compressedMaterialDataLength);
    AAPLMaterial* rawMats = (AAPLMaterial*)matData;

    std::vector<AAPLShaderMaterial> shaderMats(_applMesh->_materialCount);
    for (size_t i = 0; i < _applMesh->_materialCount; ++i) {
      auto findTex = [&](uint32_t hash) -> uint32_t {
        auto it = _textureHashMap.find(hash);
        return it != _textureHashMap.end() ? (uint32_t)it->second : 0;
      };
      shaderMats[i].albedo_texture_index = rawMats[i].hasBaseColorTexture ? findTex(rawMats[i].baseColorTextureHash) : 0;
      shaderMats[i].roughness_texture_index = rawMats[i].hasMetallicRoughnessTexture ? findTex(rawMats[i].metallicRoughnessHash) : 0;
      shaderMats[i].normal_texture_index = rawMats[i].hasNormalMap ? findTex(rawMats[i].normalMapHash) : 0;
      shaderMats[i].emissive_texture_index = rawMats[i].hasEmissiveTexture ? findTex(rawMats[i].emissiveTextureHash) : 0;
      shaderMats[i].alpha = rawMats[i].opacity;
      shaderMats[i].hasMetallicRoughness = rawMats[i].hasMetallicRoughnessTexture ? 1 : 0;
      shaderMats[i].hasEmissive = rawMats[i].hasEmissiveTexture ? 1 : 0;
    }
    free(matData);

    // Re-upload materials with correct indices
    _device.GetCommandAllocator(0)->Reset();
    cmdList->Reset(_device.GetCommandAllocator(0), nullptr);
    ComPtr<ID3D12Resource> matUpload;
    _materialBuffer = DX12Util::CreateDefaultBuffer(dev, cmdList,
        shaderMats.data(), shaderMats.size() * sizeof(AAPLShaderMaterial), matUpload);
    cmdList->Close();
    ID3D12CommandList* matLists[] = { cmdList };
    _device.GetCommandQueue()->ExecuteCommandLists(1, matLists);
    FlushCommandQueue();

    // Re-create materials SRV
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = DXGI_FORMAT_UNKNOWN;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Buffer.NumElements = (UINT)_applMesh->_materialCount;
    srvDesc.Buffer.StructureByteStride = sizeof(AAPLShaderMaterial);
    dev->CreateShaderResourceView(_materialBuffer.Get(), &srvDesc,
        _cbvSrvUavHeap.GetStaticCPU(SRV_MATERIALS));
  }

  // Initialize texture streaming entries (textures start at permanent mip;
  // finer mips stream in via UpdateTextureStreaming, mirroring the Vulkan path)
  _streamEntries.resize(_textures.size());
  _streamEntryMap.clear();
  for (size_t k = 0; k < _textures.size(); ++k) {
    auto& e = _streamEntries[k];
    e.desc = &_applMesh->_textures[k];
    e.totalMips = (uint32_t)e.desc->_mipmapLevelCount;
    e.currentMip = DX12TextureStreamingPolicy::CalculateMinMip(
        e.desc->_width, e.desc->_height, e.totalMips,
        DX12TextureStreamingPolicy::PERMANENT_TEXTURE_SIZE);
    e.requiredMip = e.currentMip;
    e.inFlight = false;
    _streamEntryMap[e.desc->_pathHash] = k;
  }

  spdlog::info("DX12: {} textures loaded into bindless heap", _textures.size());
}

// ---- Helper: create root signature from serialized blob ----
static ComPtr<ID3D12RootSignature> CreateRootSigFromDesc(
    ID3D12Device* device,
    const D3D12_ROOT_SIGNATURE_DESC& desc) {
  ComPtr<ID3DBlob> serialized, error;
  HRESULT hr = D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1,
                                            &serialized, &error);
  if (FAILED(hr)) {
    if (error) spdlog::error("Root sig error: {}", (const char*)error->GetBufferPointer());
    throw std::runtime_error("Failed to serialize root signature");
  }
  ComPtr<ID3D12RootSignature> rootSig;
  DX12Util::ThrowIfFailed(
      device->CreateRootSignature(0, serialized->GetBufferPointer(),
                                   serialized->GetBufferSize(), IID_PPV_ARGS(&rootSig)),
      "CreateRootSignature");
  return rootSig;
}

// ---- Create Root Signatures ----
void DX12GpuScene::CreateRootSignatures() {
  auto* dev = _device.GetDevice();

  // 1. Occluder root signature: just a CBV for camera params
  {
    D3D12_ROOT_PARAMETER param = {};
    param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    param.Descriptor.ShaderRegister = 0;
    param.Descriptor.RegisterSpace = 0;
    param.ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;

    D3D12_ROOT_SIGNATURE_DESC desc = {};
    desc.NumParameters = 1;
    desc.pParameters = &param;
    desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    _occluderRootSig = CreateRootSigFromDesc(dev, desc);
  }

  // 2. Shadow root signature: CBV (b0) + root constant (b1=cascadeIndex) + SRV chunkIndex (t4,space1)
  //    + alpha-mask PS tables (t0/t3 space1, bindless t0+ space2) — D1 parity with
  //    Vulkan's _shadowPassPipelineAlphaMask (RenderSceneShadowDepthIndirect PS).
  {
    D3D12_ROOT_PARAMETER params[5] = {};
    // [0] CBV for FrameData (shadow VP matrices live here)
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[0].Descriptor.ShaderRegister = 0;
    params[0].Descriptor.RegisterSpace = 0;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    // [1] Root constant: PushConstants {materialIndex, shadowIndex, drawSlot}
    // (b1, space0). drawSlot is patched per-draw by the command signature.
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[1].Constants.ShaderRegister = 1;
    params[1].Constants.RegisterSpace = 0;
    params[1].Constants.Num32BitValues = PUSH_CONSTANT_COUNT;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    // [2] Descriptor table: chunkIndex SRV at t4, space1
    D3D12_DESCRIPTOR_RANGE chunkRange = {};
    chunkRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    chunkRange.NumDescriptors = 1;
    chunkRange.BaseShaderRegister = 4;
    chunkRange.RegisterSpace = 1;
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[2].DescriptorTable.NumDescriptorRanges = 1;
    params[2].DescriptorTable.pDescriptorRanges = &chunkRange;
    params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    // [3] Descriptor table: materials (t0,space1) + meshChunks (t3,space1) — alpha-mask PS
    D3D12_DESCRIPTOR_RANGE psRanges[2] = {};
    psRanges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    psRanges[0].NumDescriptors = 1;
    psRanges[0].BaseShaderRegister = 0;  // t0,space1 materials
    psRanges[0].RegisterSpace = 1;
    psRanges[0].OffsetInDescriptorsFromTableStart = 0;
    psRanges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    psRanges[1].NumDescriptors = 1;
    psRanges[1].BaseShaderRegister = 3;  // t3,space1 meshChunks
    psRanges[1].RegisterSpace = 1;
    psRanges[1].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[3].DescriptorTable.NumDescriptorRanges = 2;
    params[3].DescriptorTable.pDescriptorRanges = psRanges;
    params[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    // [4] Descriptor table: bindless textures (t0+, space2, unbounded) — alpha-mask PS
    D3D12_DESCRIPTOR_RANGE bindlessRange = {};
    bindlessRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    bindlessRange.NumDescriptors = UINT_MAX;
    bindlessRange.BaseShaderRegister = 0;
    bindlessRange.RegisterSpace = 2;
    bindlessRange.OffsetInDescriptorsFromTableStart = 0;
    params[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[4].DescriptorTable.NumDescriptorRanges = 1;
    params[4].DescriptorTable.pDescriptorRanges = &bindlessRange;
    params[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    // s1,space1: _LinearRepeatSampler (drawcluster.hlsl alpha-mask PS)
    D3D12_STATIC_SAMPLER_DESC psSampler = {};
    psSampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    psSampler.AddressU = psSampler.AddressV = psSampler.AddressW =
        D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    psSampler.ShaderRegister = 1;
    psSampler.RegisterSpace = 1;
    psSampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC desc = {};
    desc.NumParameters = 5;
    desc.pParameters = params;
    desc.NumStaticSamplers = 1;
    desc.pStaticSamplers = &psSampler;
    desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    _shadowRootSig = CreateRootSigFromDesc(dev, desc);
  }

  // 3. DrawCluster root signature:
  //   [0] CBV b0 space0 (FrameData) - all stages
  //   [1] Descriptor table: t0/t3/t4 space1 (materials, meshChunks, chunkIndex) - all stages
  //   [2] Descriptor table: t0+ space2 (bindless textures, unbounded) - pixel only
  //   [3] Root constants b1 space0 (push constants: materialIndex/shadowIndex) - all stages
  {
    D3D12_ROOT_PARAMETER params[4] = {};

    // [0] CBV
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[0].Descriptor.ShaderRegister = 0;
    params[0].Descriptor.RegisterSpace = 0;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    // [1] Descriptor table: discrete ranges for non-bindless SRVs in space1
    D3D12_DESCRIPTOR_RANGE ranges[3] = {};
    // t0,space1: materials
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = 1;
    ranges[0].BaseShaderRegister = 0;
    ranges[0].RegisterSpace = 1;
    // t3,space1: meshChunks (gap t1-t2 skipped via explicit offset)
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[1].NumDescriptors = 1;
    ranges[1].BaseShaderRegister = 3;
    ranges[1].RegisterSpace = 1;
    ranges[1].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    // t4,space1: chunkIndex
    ranges[2].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[2].NumDescriptors = 1;
    ranges[2].BaseShaderRegister = 4;
    ranges[2].RegisterSpace = 1;
    ranges[2].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 3;
    params[1].DescriptorTable.pDescriptorRanges = ranges;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    // [2] Descriptor table: unbounded bindless textures in space2 (own param so it can
    // point to the fixed static region rather than being contiguous with the dynamic table)
    D3D12_DESCRIPTOR_RANGE bindlessRange = {};
    bindlessRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    bindlessRange.NumDescriptors = UINT_MAX;
    bindlessRange.BaseShaderRegister = 0;
    bindlessRange.RegisterSpace = 2;
    bindlessRange.OffsetInDescriptorsFromTableStart = 0;

    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[2].DescriptorTable.NumDescriptorRanges = 1;
    params[2].DescriptorTable.pDescriptorRanges = &bindlessRange;
    params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    // [3] Root constants (push constants: materialIndex/shadowIndex/drawSlot)
    params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[3].Constants.ShaderRegister = 1;
    params[3].Constants.RegisterSpace = 0;
    params[3].Constants.Num32BitValues = PUSH_CONSTANT_COUNT;
    params[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    // Static sampler for linear repeat
    D3D12_STATIC_SAMPLER_DESC sampler = {};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.ShaderRegister = 1;
    sampler.RegisterSpace = 1;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC desc = {};
    desc.NumParameters = 4;
    desc.pParameters = params;
    desc.NumStaticSamplers = 1;
    desc.pStaticSamplers = &sampler;
    desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    _drawClusterRootSig = CreateRootSigFromDesc(dev, desc);
  }

  // 3. GPU Cull root signature
  {
    D3D12_ROOT_PARAMETER params[2] = {};

    // [0] CBV for cullParams
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[0].Descriptor.ShaderRegister = 1;
    params[0].Descriptor.RegisterSpace = 0;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    // [1] Descriptor table: UAVs + SRVs
    D3D12_DESCRIPTOR_RANGE ranges[4] = {};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[0].NumDescriptors = 1; // u0 drawParams
    ranges[0].BaseShaderRegister = 0;
    ranges[0].RegisterSpace = 0;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[1].NumDescriptors = 2; // u3 writeIndex, u4 chunkIndices
    ranges[1].BaseShaderRegister = 3;
    ranges[1].RegisterSpace = 0;
    ranges[1].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    ranges[2].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[2].NumDescriptors = 1; // t2 meshChunks
    ranges[2].BaseShaderRegister = 2;
    ranges[2].RegisterSpace = 0;
    ranges[2].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    ranges[3].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[3].NumDescriptors = 1; // t6 hizTexture
    ranges[3].BaseShaderRegister = 6;
    ranges[3].RegisterSpace = 0;
    ranges[3].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 4;
    params[1].DescriptorTable.pDescriptorRanges = ranges;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_STATIC_SAMPLER_DESC sampler = {};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.ShaderRegister = 7;
    sampler.RegisterSpace = 0;

    D3D12_ROOT_SIGNATURE_DESC desc = {};
    desc.NumParameters = 2;
    desc.pParameters = params;
    desc.NumStaticSamplers = 1;
    desc.pStaticSamplers = &sampler;
    _gpuCullRootSig = CreateRootSigFromDesc(dev, desc);
  }

  // 4. Deferred lighting root signature
  {
    D3D12_ROOT_PARAMETER params[2] = {};

    // [0] CBV
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[0].Descriptor.ShaderRegister = 0;
    params[0].Descriptor.RegisterSpace = 0;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    // [1] Descriptor table: SRVs (G-buffers, depth, shadow, AO, lights)
    // t0-t17 in space1: covers all deferredlighting.hlsl bindings
    D3D12_DESCRIPTOR_RANGE range = {};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 18; // t0-t17 in space1
    range.BaseShaderRegister = 0;
    range.RegisterSpace = 1;

    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 1;
    params[1].DescriptorTable.pDescriptorRanges = &range;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC samplers[4] = {};
    // Nearest clamp (s5, space1)
    samplers[0].Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    samplers[0].AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplers[0].AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplers[0].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplers[0].ShaderRegister = 5;
    samplers[0].RegisterSpace = 1;
    samplers[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    // Shadow comparison (s7, space1)
    samplers[1].Filter = D3D12_FILTER_COMPARISON_MIN_MAG_MIP_LINEAR;
    samplers[1].AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplers[1].AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplers[1].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplers[1].ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    samplers[1].ShaderRegister = 7;
    samplers[1].RegisterSpace = 1;
    samplers[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    // Spot shadow comparison (s14, space1)
    samplers[2].Filter = D3D12_FILTER_COMPARISON_MIN_MAG_MIP_LINEAR;
    samplers[2].AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplers[2].AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplers[2].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplers[2].ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    samplers[2].ShaderRegister = 14;
    samplers[2].RegisterSpace = 1;
    samplers[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    // Linear clamp for scatter volume (s17, space1)
    samplers[3].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    samplers[3].AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplers[3].AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplers[3].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplers[3].ShaderRegister = 17;
    samplers[3].RegisterSpace = 1;
    samplers[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC desc = {};
    desc.NumParameters = 2;
    desc.pParameters = params;
    desc.NumStaticSamplers = 4;
    desc.pStaticSamplers = samplers;
    _deferredLightingRootSig = CreateRootSigFromDesc(dev, desc);
  }

  // 5. HiZ root signature: SRV input + UAV output + root constants
  {
    D3D12_ROOT_PARAMETER params[2] = {};

    D3D12_DESCRIPTOR_RANGE ranges[2] = {};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = 1;
    ranges[0].BaseShaderRegister = 0;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[1].NumDescriptors = 1;
    ranges[1].BaseShaderRegister = 1;
    ranges[1].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].DescriptorTable.NumDescriptorRanges = 2;
    params[0].DescriptorTable.pDescriptorRanges = ranges;

    // Root constants: srcSize + dstSize (4 uint32)
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[1].Constants.ShaderRegister = 0;
    params[1].Constants.Num32BitValues = 4;

    D3D12_ROOT_SIGNATURE_DESC desc = {};
    desc.NumParameters = 2;
    desc.pParameters = params;
    _hizRootSig = CreateRootSigFromDesc(dev, desc);
  }

  // 6. SAO root signature: 2 SRV + CBV + UAV + root constants
  {
    D3D12_ROOT_PARAMETER params[3] = {};

    D3D12_DESCRIPTOR_RANGE ranges[3] = {};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = 2; // t0 depth, t1 depthMip
    ranges[0].BaseShaderRegister = 0;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[1].NumDescriptors = 1; // u3 aoOutput
    ranges[1].BaseShaderRegister = 3;
    ranges[1].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].DescriptorTable.NumDescriptorRanges = 2;
    params[0].DescriptorTable.pDescriptorRanges = ranges;

    // CBV for camera params
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[1].Descriptor.ShaderRegister = 2;
    params[1].Descriptor.RegisterSpace = 0;

    // Root constants: screenSize (2 uint32)
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[2].Constants.ShaderRegister = 3;
    params[2].Constants.Num32BitValues = 2;

    D3D12_ROOT_SIGNATURE_DESC desc = {};
    desc.NumParameters = 3;
    desc.pParameters = params;
    _saoRootSig = CreateRootSigFromDesc(dev, desc);
  }

  // 7. Shadow Cull compute root signature:
  // [0] CBV b1 (ShadowCullParams cbuffer)
  // [1] Descriptor table: u0..u4 (UAVs), t2..t4 (SRVs)
  {
    D3D12_ROOT_PARAMETER params[2] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[0].Descriptor.ShaderRegister = 1;
    params[0].Descriptor.RegisterSpace = 0;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_DESCRIPTOR_RANGE ranges[2] = {};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[0].NumDescriptors = 5; // u0..u4
    ranges[0].BaseShaderRegister = 0;
    ranges[0].RegisterSpace = 0;
    ranges[0].OffsetInDescriptorsFromTableStart = 0;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[1].NumDescriptors = 3; // t2..t4
    ranges[1].BaseShaderRegister = 2;
    ranges[1].RegisterSpace = 0;
    ranges[1].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 2;
    params[1].DescriptorTable.pDescriptorRanges = ranges;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC desc = {};
    desc.NumParameters = 2;
    desc.pParameters = params;
    _shadowCullRootSig = CreateRootSigFromDesc(dev, desc);
  }

  // 8. Light Cull root signature:
  // [0] CBV b0 space0 (frameData 鈥?camera params)
  // [1] CBV b1 space0 (cullParams 鈥?light counts, screen size, VP matrix, frustum)
  // [2] Descriptor table:
  //     SRVs: t1,t2 (pointLight, depth) and t8 (spotLight) in space1
  //     UAVs: u3..u11 in space1
  {
    D3D12_ROOT_PARAMETER params[3] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[0].Descriptor.ShaderRegister = 0; params[0].Descriptor.RegisterSpace = 0;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[1].Descriptor.ShaderRegister = 1; params[1].Descriptor.RegisterSpace = 0;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    // Descriptor table: SRV range (t1..t8, space1) + UAV range (u3..u11, space1)
    D3D12_DESCRIPTOR_RANGE lcRanges[2] = {};
    // SRVs: t1 and t2 (pointLight, depth) and t8 (spotLight) 鈥?cover t1..t8 contiguously
    lcRanges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    lcRanges[0].NumDescriptors = 8;  // t1..t8 space1
    lcRanges[0].BaseShaderRegister = 1;
    lcRanges[0].RegisterSpace = 1;
    lcRanges[0].OffsetInDescriptorsFromTableStart = 0;
    // UAVs: u3..u11 (9 slots)
    lcRanges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    lcRanges[1].NumDescriptors = 9;  // u3..u11 space1
    lcRanges[1].BaseShaderRegister = 3;
    lcRanges[1].RegisterSpace = 1;
    lcRanges[1].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[2].DescriptorTable.NumDescriptorRanges = 2;
    params[2].DescriptorTable.pDescriptorRanges = lcRanges;
    params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC desc = {};
    desc.NumParameters = 3; desc.pParameters = params;
    _lightCullRootSig = CreateRootSigFromDesc(dev, desc);
  }

  // 9. Resolve root signature:
  // [0] CBV b0 space0 (FrameData: camera params + frame constants)
  // [1] Descriptor table: SRV t0..t2 space1 (hdrBuffer, historyTex, depthTex)
  // Static samplers: s3 space1 (nearest clamp), s4 space1 (linear clamp)
  {
    D3D12_ROOT_PARAMETER params[2] = {};
    // [0] CBV
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[0].Descriptor.ShaderRegister = 0;
    params[0].Descriptor.RegisterSpace = 0;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    // [1] Descriptor table: 3 SRVs (t0..t2, space1)
    D3D12_DESCRIPTOR_RANGE srvRange = {};
    srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange.NumDescriptors = 3;
    srvRange.BaseShaderRegister = 0;
    srvRange.RegisterSpace = 1;
    srvRange.OffsetInDescriptorsFromTableStart = 0;

    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 1;
    params[1].DescriptorTable.pDescriptorRanges = &srvRange;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    // Static samplers matching resolve.hlsl: s3 space1 = nearest clamp, s4 space1 = linear clamp
    D3D12_STATIC_SAMPLER_DESC staticSamplers[2] = {};
    // s3 space1: _NearestClampSampler
    staticSamplers[0].Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    staticSamplers[0].AddressU = staticSamplers[0].AddressV = staticSamplers[0].AddressW =
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    staticSamplers[0].ShaderRegister = 3;
    staticSamplers[0].RegisterSpace = 1;
    staticSamplers[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    // s4 space1: _LinearClampSampler
    staticSamplers[1].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    staticSamplers[1].AddressU = staticSamplers[1].AddressV = staticSamplers[1].AddressW =
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    staticSamplers[1].ShaderRegister = 4;
    staticSamplers[1].RegisterSpace = 1;
    staticSamplers[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC desc = {};
    desc.NumParameters = 2; desc.pParameters = params;
    desc.NumStaticSamplers = 2; desc.pStaticSamplers = staticSamplers;
    _resolveRootSig = CreateRootSigFromDesc(dev, desc);
  }

  // 11. Spot shadow root signature (drawclusterShadowSpot.hlsl + shadow alpha-mask PS):
  // [0] Root constants b2 space0: { float4x4 viewProj (16), uint chunkId (1) }
  //     鈥?chunkId is patched per-draw by the [CONSTANT(b2), DRAW_INDEXED] command
  //     signature (same trick as the CSM drawSlot).
  // [1] Descriptor table: t0,space1 (materials) + t3,space1 (meshChunks) 鈥?alpha-mask PS
  // [2] Descriptor table: bindless textures t0+, space2 (unbounded) 鈥?alpha-mask PS
  // Static sampler s1,space1 (linear repeat, wrap) 鈥?matches drawcluster.hlsl
  {
    D3D12_ROOT_PARAMETER params[3] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.ShaderRegister = 2;
    params[0].Constants.RegisterSpace = 0;
    params[0].Constants.Num32BitValues = 17; // 16 floats (viewProj) + chunkId
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;

    D3D12_DESCRIPTOR_RANGE ranges[2] = {};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = 1;
    ranges[0].BaseShaderRegister = 0;  // t0,space1 materials
    ranges[0].RegisterSpace = 1;
    ranges[0].OffsetInDescriptorsFromTableStart = 0;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[1].NumDescriptors = 1;
    ranges[1].BaseShaderRegister = 3;  // t3,space1 meshChunks
    ranges[1].RegisterSpace = 1;
    ranges[1].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 2;
    params[1].DescriptorTable.pDescriptorRanges = ranges;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_DESCRIPTOR_RANGE bindlessRange = {};
    bindlessRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    bindlessRange.NumDescriptors = UINT_MAX;
    bindlessRange.BaseShaderRegister = 0; // t0+, space2
    bindlessRange.RegisterSpace = 2;
    bindlessRange.OffsetInDescriptorsFromTableStart = 0;
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[2].DescriptorTable.NumDescriptorRanges = 1;
    params[2].DescriptorTable.pDescriptorRanges = &bindlessRange;
    params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC sampler = {};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.ShaderRegister = 1;
    sampler.RegisterSpace = 1;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC desc = {};
    desc.NumParameters = 3; desc.pParameters = params;
    desc.NumStaticSamplers = 1; desc.pStaticSamplers = &sampler;
    desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    _spotShadowRootSig = CreateRootSigFromDesc(dev, desc);
  }

  // 10. Scatter volume root signature (ScatterVolume kernel)
  // [0] Root constants (b0): PushConstants { volumeWidth, volumeHeight, screenWidth, screenHeight, resetHistory }
  // [1] CBV (b1): ScatterUBO (CameraParamsBufferFull + AAPLFrameConstants)
  // [2] Descriptor table: UAV u0 + SRVs t2..t14 in space0
  // Static samplers: s3=shadowSampler (cmp), s11=spotShadowSampler (cmp), s14=linearSampler
  {
    D3D12_ROOT_PARAMETER params[3] = {};
    // [0] Root constants for PushConstants at b0 space0
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.ShaderRegister = 0;
    params[0].Constants.RegisterSpace = 0;
    params[0].Constants.Num32BitValues = 5; // volumeWidth, volumeHeight, screenWidth, screenHeight, resetHistory
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    // [1] CBV at b1 space0
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[1].Descriptor.ShaderRegister = 1;
    params[1].Descriptor.RegisterSpace = 0;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    // [2] Descriptor table: u0 (scatterOut UAV) + t2..t13 SRVs (12 slots)
    // Layout: slot 0 = u0, slot 1 = t2, slot 2 = t3, ..., slot 12 = t13
    D3D12_DESCRIPTOR_RANGE scatterRanges[2] = {};
    scatterRanges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    scatterRanges[0].NumDescriptors = 1; // u0 scatterOut
    scatterRanges[0].BaseShaderRegister = 0;
    scatterRanges[0].RegisterSpace = 0;
    scatterRanges[0].OffsetInDescriptorsFromTableStart = 0;
    scatterRanges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    scatterRanges[1].NumDescriptors = 12; // t2..t13 (12 slots: shadowMaps, null, scatterPrev, blueNoise, pointLightData, pointLightIndices, spotLightData, spotLightIndices, spotShadowMaps, null, spotViewProjMatrices, perlinNoise)
    scatterRanges[1].BaseShaderRegister = 2;
    scatterRanges[1].RegisterSpace = 0;
    scatterRanges[1].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[2].DescriptorTable.NumDescriptorRanges = 2;
    params[2].DescriptorTable.pDescriptorRanges = scatterRanges;
    params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    // Static samplers
    D3D12_STATIC_SAMPLER_DESC scatterSamplers[3] = {};
    // s3 space0: shadowSampler (comparison, for cascade shadows)
    scatterSamplers[0].Filter = D3D12_FILTER_COMPARISON_MIN_MAG_MIP_LINEAR;
    scatterSamplers[0].AddressU = scatterSamplers[0].AddressV = scatterSamplers[0].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    scatterSamplers[0].ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    scatterSamplers[0].ShaderRegister = 3;
    scatterSamplers[0].RegisterSpace = 0;
    scatterSamplers[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    // s11 space0: spotShadowSampler (comparison)
    scatterSamplers[1].Filter = D3D12_FILTER_COMPARISON_MIN_MAG_MIP_LINEAR;
    scatterSamplers[1].AddressU = scatterSamplers[1].AddressV = scatterSamplers[1].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    scatterSamplers[1].ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    scatterSamplers[1].ShaderRegister = 11;
    scatterSamplers[1].RegisterSpace = 0;
    scatterSamplers[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    // s14 space0: linearSampler (trilinear WRAP — Vulkan parity: ScatteringVolume.cpp
    // createSampler uses VK_SAMPLER_ADDRESS_MODE_REPEAT; the Perlin tile is sampled
    // in world space and must tile seamlessly)
    scatterSamplers[2].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    scatterSamplers[2].AddressU = scatterSamplers[2].AddressV = scatterSamplers[2].AddressW =
        D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    scatterSamplers[2].ShaderRegister = 14;
    scatterSamplers[2].RegisterSpace = 0;
    scatterSamplers[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC scatterSigDesc = {};
    scatterSigDesc.NumParameters = 3;
    scatterSigDesc.pParameters = params;
    scatterSigDesc.NumStaticSamplers = 3;
    scatterSigDesc.pStaticSamplers = scatterSamplers;
    _scatterRootSig = CreateRootSigFromDesc(dev, scatterSigDesc);
  }

  // 11. Accumulate scatter root signature (AccumulateScattering kernel)
  // [0] Root constants (b0): PushConstants { volumeWidth, volumeHeight, ... }
  // [1] Descriptor table: SRV t0 (scatterIn) + UAV u1 (accumOut) in space0
  {
    D3D12_ROOT_PARAMETER params[2] = {};
    // [0] Root constants for PushConstants at b0 space0
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.ShaderRegister = 0;
    params[0].Constants.RegisterSpace = 0;
    params[0].Constants.Num32BitValues = 5;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    // [1] Descriptor table: t0 (SRV) + u1 (UAV)
    D3D12_DESCRIPTOR_RANGE accumRanges[2] = {};
    accumRanges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    accumRanges[0].NumDescriptors = 1; // t0 scatterIn
    accumRanges[0].BaseShaderRegister = 0;
    accumRanges[0].RegisterSpace = 0;
    accumRanges[0].OffsetInDescriptorsFromTableStart = 0;
    accumRanges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    accumRanges[1].NumDescriptors = 1; // u1 accumOut
    accumRanges[1].BaseShaderRegister = 1;
    accumRanges[1].RegisterSpace = 0;
    accumRanges[1].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 2;
    params[1].DescriptorTable.pDescriptorRanges = accumRanges;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC accumSigDesc = {};
    accumSigDesc.NumParameters = 2;
    accumSigDesc.pParameters = params;
    _accumRootSig = CreateRootSigFromDesc(dev, accumSigDesc);
  }

  spdlog::info("DX12: All root signatures created");
}

// ---- Create Pipeline States ----
void DX12GpuScene::CreatePipelineStates() {
  auto* dev = _device.GetDevice();
  std::string shaderDir = (_rootPath / "shaders/").generic_string();

  // Vertex input layout (shared by drawcluster and occluder)
  D3D12_INPUT_ELEMENT_DESC clusterLayout[] = {
    {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    {"NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 1, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    {"Tangent",  0, DXGI_FORMAT_R32G32B32_FLOAT, 2, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    3, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
  };

  D3D12_INPUT_ELEMENT_DESC occluderLayout[] = {
    {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
  };

  // Helper to load shader bytecode
  auto loadShader = [&](const std::string& name) -> D3D12_SHADER_BYTECODE {
    auto code = DX12Util::ReadShaderFile(shaderDir + name);
    if (code.empty()) return {nullptr, 0};
    // Copy to persistent storage
    void* data = malloc(code.size());
    memcpy(data, code.data(), code.size());
    return {data, code.size()};
  };

  // 1. Occluder depth-only PSO
  {
    auto vs = loadShader("occluders.vs.cso");

    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};
    desc.pRootSignature = _occluderRootSig.Get();
    desc.VS = vs;
    desc.InputLayout = {occluderLayout, 1};
    desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    desc.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
    desc.RasterizerState.FrontCounterClockwise = FALSE;
    desc.RasterizerState.DepthClipEnable = TRUE;
    desc.DepthStencilState.DepthEnable = TRUE;
    desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER; // reverse-Z
    desc.BlendState.RenderTarget[0].RenderTargetWriteMask = 0; // depth only
    desc.SampleMask = UINT_MAX;
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.NumRenderTargets = 0;
    desc.DSVFormat = _device.GetDepthFormat();
    desc.SampleDesc.Count = 1;

    DX12Util::ThrowIfFailed(
        dev->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&_occluderPSO)),
        "Failed to create occluder PSO");
    free((void*)vs.pShaderBytecode);
  }

  // 2. Base pass PSO (G-buffer output)
  {
    auto vs = loadShader("drawcluster.vs.cso");
    auto ps = loadShader("drawcluster.base.ps.cso");

    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};
    desc.pRootSignature = _drawClusterRootSig.Get();
    desc.VS = vs;
    desc.PS = ps;
    desc.InputLayout = {clusterLayout, 4};
    desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    desc.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
    desc.RasterizerState.FrontCounterClockwise = FALSE;
    desc.RasterizerState.DepthClipEnable = TRUE;
    desc.DepthStencilState.DepthEnable = TRUE;
    desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER; // reverse-Z
    for (int i = 0; i < 4; ++i) {
      desc.BlendState.RenderTarget[i].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    }
    desc.SampleMask = UINT_MAX;
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.NumRenderTargets = 4;
    desc.RTVFormats[0] = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB; // albedo
    desc.RTVFormats[1] = DXGI_FORMAT_R16G16B16A16_FLOAT;   // normals
    desc.RTVFormats[2] = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB; // emissive
    desc.RTVFormats[3] = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB; // F0Roughness
    desc.DSVFormat = _device.GetDepthFormat();
    desc.SampleDesc.Count = 1;

    DX12Util::ThrowIfFailed(
        dev->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&_basePassPSO)),
        "Failed to create base pass PSO");
    free((void*)vs.pShaderBytecode);
    free((void*)ps.pShaderBytecode);
  }

  // 2a. Base pass alpha-mask PSO
  {
    auto vs = loadShader("drawcluster.vs.cso");
    auto ps = loadShader("drawcluster.base.alphamask.ps.cso");

    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};
    desc.pRootSignature = _drawClusterRootSig.Get();
    desc.VS = vs;
    desc.PS = ps;
    desc.InputLayout = {clusterLayout, 4};
    desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE; // double-sided for alpha-mask
    desc.RasterizerState.FrontCounterClockwise = FALSE;
    desc.RasterizerState.DepthClipEnable = TRUE;
    desc.DepthStencilState.DepthEnable = TRUE;
    desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER;
    for (int i = 0; i < 4; ++i)
      desc.BlendState.RenderTarget[i].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    desc.SampleMask = UINT_MAX;
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.NumRenderTargets = 4;
    desc.RTVFormats[0] = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    desc.RTVFormats[1] = DXGI_FORMAT_R16G16B16A16_FLOAT;
    desc.RTVFormats[2] = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    desc.RTVFormats[3] = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    desc.DSVFormat = _device.GetDepthFormat();
    desc.SampleDesc.Count = 1;

    DX12Util::ThrowIfFailed(
        dev->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&_basePassAlphaMaskPSO)),
        "Failed to create base pass alpha-mask PSO");
    free((void*)vs.pShaderBytecode);
    free((void*)ps.pShaderBytecode);
  }

  // 2b. Forward pass PSO (alpha blending)
  {
    auto vs = loadShader("drawcluster.vs.cso");
    auto ps = loadShader("drawcluster.forward.indirect.ps.cso");

    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};
    desc.pRootSignature = _drawClusterRootSig.Get();
    desc.VS = vs;
    desc.PS = ps;
    desc.InputLayout = {clusterLayout, 4};
    desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    desc.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
    desc.RasterizerState.FrontCounterClockwise = FALSE;
    desc.RasterizerState.DepthClipEnable = TRUE;
    desc.DepthStencilState.DepthEnable = TRUE;
    desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO; // depth read-only
    desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER; // reverse-Z
    desc.BlendState.RenderTarget[0].BlendEnable = TRUE;
    desc.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
    desc.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    desc.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
    desc.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
    desc.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ZERO;
    desc.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
    desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    desc.SampleMask = UINT_MAX;
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.NumRenderTargets = 1;
    desc.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT; // HDR buffer (resolve pass tone-maps to swapchain)
    desc.DSVFormat = _device.GetDepthFormat();
    desc.SampleDesc.Count = 1;

    DX12Util::ThrowIfFailed(
        dev->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&_forwardPSO)),
        "Failed to create forward PSO");
    free((void*)vs.pShaderBytecode);
    free((void*)ps.pShaderBytecode);
  }

  // 3. Deferred lighting PSO (full-screen triangle)
  {
    auto vs = loadShader("deferredlighting.vs.cso");
    auto ps = loadShader("deferredlighting.ps.cso");

    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};
    desc.pRootSignature = _deferredLightingRootSig.Get();
    desc.VS = vs;
    desc.PS = ps;
    desc.InputLayout = {nullptr, 0}; // no vertex input
    desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    desc.RasterizerState.DepthClipEnable = TRUE;
    desc.DepthStencilState.DepthEnable = FALSE;
    desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    desc.SampleMask = UINT_MAX;
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.NumRenderTargets = 1;
    desc.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT; // HDR buffer (resolve pass tone-maps to swapchain)
    desc.DSVFormat = _device.GetDepthFormat();
    desc.SampleDesc.Count = 1;

    DX12Util::ThrowIfFailed(
        dev->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&_deferredLightingPSO)),
        "Failed to create deferred lighting PSO");
    free((void*)vs.pShaderBytecode);
    free((void*)ps.pShaderBytecode);
  }

  // 4. GPU Cull compute PSO
  {
    auto cs = loadShader("gpucull.cs.cso");
    D3D12_COMPUTE_PIPELINE_STATE_DESC desc = {};
    desc.pRootSignature = _gpuCullRootSig.Get();
    desc.CS = cs;
    DX12Util::ThrowIfFailed(
        dev->CreateComputePipelineState(&desc, IID_PPV_ARGS(&_gpuCullPSO)),
        "Failed to create GPU cull PSO");
    free((void*)cs.pShaderBytecode);
  }

  // 5. HiZ Copy + Downsample compute PSOs
  {
    auto csCopy = loadShader("hiz_copy.cs.cso");
    D3D12_COMPUTE_PIPELINE_STATE_DESC desc = {};
    desc.pRootSignature = _hizRootSig.Get();
    desc.CS = csCopy;
    DX12Util::ThrowIfFailed(
        dev->CreateComputePipelineState(&desc, IID_PPV_ARGS(&_hizCopyPSO)),
        "Failed to create HiZ copy PSO");
    free((void*)csCopy.pShaderBytecode);

    auto csDown = loadShader("hiz_downsample.cs.cso");
    desc.CS = csDown;
    DX12Util::ThrowIfFailed(
        dev->CreateComputePipelineState(&desc, IID_PPV_ARGS(&_hizDownsamplePSO)),
        "Failed to create HiZ downsample PSO");
    free((void*)csDown.pShaderBytecode);
  }

  // 6. SAO compute PSO
  {
    auto cs = loadShader("sao.cs.cso");
    D3D12_COMPUTE_PIPELINE_STATE_DESC desc = {};
    desc.pRootSignature = _saoRootSig.Get();
    desc.CS = cs;
    DX12Util::ThrowIfFailed(
        dev->CreateComputePipelineState(&desc, IID_PPV_ARGS(&_saoPSO)),
        "Failed to create SAO PSO");
    free((void*)cs.pShaderBytecode);
  }

  // 7. Resolve PSO (tone map + TAA): 2 render targets (swapchain + TAA history)
  {
    auto vs = loadShader("resolve.vs.cso");
    auto ps = loadShader("resolve.ps.cso");
    if (vs.pShaderBytecode && ps.pShaderBytecode) {
      D3D12_GRAPHICS_PIPELINE_STATE_DESC d = {};
      d.pRootSignature = _resolveRootSig.Get();
      d.VS = vs; d.PS = ps;
      d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
      d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
      for (int i = 0; i < 2; ++i)
        d.BlendState.RenderTarget[i].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
      d.SampleMask = UINT_MAX;
      d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
      d.NumRenderTargets = 2;
      d.RTVFormats[0] = _device.GetSwapChainFormat(); // swapchain output
      d.RTVFormats[1] = DXGI_FORMAT_R16G16B16A16_FLOAT; // TAA history write
      d.SampleDesc.Count = 1;
      DX12Util::ThrowIfFailed(dev->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&_resolvePSO)), "resolve PSO");
      free((void*)vs.pShaderBytecode);
      free((void*)ps.pShaderBytecode);
    }
  }

  // === Scatter volume PSOs ===
  {
    auto scatterCS = loadShader("scattervolume.cs.cso");
    if (scatterCS.pShaderBytecode) {
      D3D12_COMPUTE_PIPELINE_STATE_DESC d = {};
      d.pRootSignature = _scatterRootSig.Get();
      d.CS = scatterCS;
      DX12Util::ThrowIfFailed(dev->CreateComputePipelineState(&d, IID_PPV_ARGS(&_scatterVolumePSO)), "scatterVolume PSO");
      free((void*)scatterCS.pShaderBytecode);
    }
  }
  {
    auto accumCS = loadShader("accumulatescatter.cs.cso");
    if (accumCS.pShaderBytecode) {
      D3D12_COMPUTE_PIPELINE_STATE_DESC d = {};
      d.pRootSignature = _accumRootSig.Get();
      d.CS = accumCS;
      DX12Util::ThrowIfFailed(dev->CreateComputePipelineState(&d, IID_PPV_ARGS(&_accumulatePSO)), "accumulateScatter PSO");
      free((void*)accumCS.pShaderBytecode);
    }
  }

  spdlog::info("DX12: All pipeline states created");
}

// ---- Create G-Buffers ----
void DX12GpuScene::CreateGBuffers() {
  auto* dev = _device.GetDevice();
  uint32_t w = _device.GetWidth(), h = _device.GetHeight();

  DXGI_FORMAT formats[4] = {
    DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,  // albedo
    DXGI_FORMAT_R16G16B16A16_FLOAT,    // normals
    DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,  // emissive
    DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,  // F0Roughness
  };

  // Create RTV heap for G-buffers
  {
    D3D12_DESCRIPTOR_HEAP_DESC desc = {};
    desc.NumDescriptors = 4;
    desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    dev->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&_gbufferRtvHeap));
    _gbufferRtvSize = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
  }

  // Create DSV heap for occluder depth
  {
    D3D12_DESCRIPTOR_HEAP_DESC desc = {};
    desc.NumDescriptors = 1;
    desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    dev->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&_gbufferDsvHeap));
  }

  D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = _gbufferRtvHeap->GetCPUDescriptorHandleForHeapStart();
  for (int i = 0; i < 4; ++i) {
    D3D12_CLEAR_VALUE clearVal = {};
    clearVal.Format = formats[i];
    _gbuffers[i] = DX12Util::CreateTexture2D(dev, w, h, formats[i],
        D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, 1, 1,
        D3D12_RESOURCE_STATE_RENDER_TARGET, &clearVal);
    dev->CreateRenderTargetView(_gbuffers[i].Get(), nullptr, rtvHandle);
    rtvHandle.ptr += _gbufferRtvSize;
  }

  // Occluder depth texture
  D3D12_CLEAR_VALUE depthClear = {};
  depthClear.Format = _device.GetDepthFormat();
  depthClear.DepthStencil.Depth = 0.0f;
  _depthTexture = DX12Util::CreateTexture2D(dev, w, h, _device.GetDepthFormat(),
      D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL, 1, 1,
      D3D12_RESOURCE_STATE_DEPTH_WRITE, &depthClear);
  {
    D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
    dsvDesc.Format = _device.GetDepthFormat();
    dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    dev->CreateDepthStencilView(_depthTexture.Get(), &dsvDesc,
        _gbufferDsvHeap->GetCPUDescriptorHandleForHeapStart());
  }

  // AO texture
  _aoTexture = DX12Util::CreateTexture2D(dev, w, h, DXGI_FORMAT_R8_UNORM,
      D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, 1, 1,
      D3D12_RESOURCE_STATE_COMMON);

  spdlog::info("DX12: G-buffers created ({}x{}) with RTV/DSV heaps", w, h);
}

// ---- Create HDR intermediate buffer + TAA history resources ----
void DX12GpuScene::CreateHDRResources() {
  auto* dev = _device.GetDevice();
  uint32_t w = _device.GetWidth(), h = _device.GetHeight();

  // HDR buffer: render target for deferred lighting output
  {
    D3D12_CLEAR_VALUE clearVal = {};
    clearVal.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    _hdrBuffer = DX12Util::CreateTexture2D(dev, w, h, DXGI_FORMAT_R16G16B16A16_FLOAT,
        D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
        1, 1, D3D12_RESOURCE_STATE_RENDER_TARGET, &clearVal);
  }

  // TAA history ping-pong buffers (start as SRV so first resolve reads them as black)
  for (int k = 0; k < 2; ++k) {
    _taaHistory[k] = DX12Util::CreateTexture2D(dev, w, h, DXGI_FORMAT_R16G16B16A16_FLOAT,
        D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
        1, 1, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
  }

  // RTV heap: [0] = HDR buffer,  [1] = current TAA history write target
  {
    D3D12_DESCRIPTOR_HEAP_DESC rtvDesc = {};
    rtvDesc.NumDescriptors = 2;
    rtvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    DX12Util::ThrowIfFailed(dev->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(&_hdrRtvHeap)), "HDR RTV heap");
    _hdrRtvSize = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
  }

  // Create RTV for HDR buffer at slot [0]
  dev->CreateRenderTargetView(_hdrBuffer.Get(), nullptr,
      _hdrRtvHeap->GetCPUDescriptorHandleForHeapStart());

  // SRV slots for the HDR buffer and initial TAA history are compile-time constants
  // (SRV_HDR_BUFFER=2050, SRV_TAA_HISTORY=2051), well above the bindless range.
  D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
  srvDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
  srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  srvDesc.Texture2D.MipLevels = 1;

  dev->CreateShaderResourceView(_hdrBuffer.Get(), &srvDesc,
      _cbvSrvUavHeap.GetStaticCPU(SRV_HDR_BUFFER));

  // TAA history SRV: points to the currently readable history buffer (_taaHistoryIndex ^ 1 is read, _taaHistoryIndex is write)
  dev->CreateShaderResourceView(_taaHistory[_taaHistoryIndex ^ 1].Get(), &srvDesc,
      _cbvSrvUavHeap.GetStaticCPU(SRV_TAA_HISTORY));

  // Clear both history buffers once so the very first resolve reads black
  // instead of undefined memory (variance clamping would mostly hide it, but
  // a deterministic start is cheaper to reason about). Also re-runs on
  // OnResize, discarding stale history of the old size.
  {
    auto* cmdList = _device.GetCommandList();
    _device.GetCommandAllocator(0)->Reset();
    cmdList->Reset(_device.GetCommandAllocator(0), nullptr);
    float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    for (int k = 0; k < 2; ++k) {
      D3D12_RESOURCE_BARRIER b = {};
      b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
      b.Transition.pResource = _taaHistory[k].Get();
      b.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
      b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
      cmdList->ResourceBarrier(1, &b);

      D3D12_CPU_DESCRIPTOR_HANDLE rtv =
          _hdrRtvHeap->GetCPUDescriptorHandleForHeapStart();
      rtv.ptr += _hdrRtvSize; // slot [1]: scratch for the per-frame history RTV
      dev->CreateRenderTargetView(_taaHistory[k].Get(), nullptr, rtv);
      cmdList->ClearRenderTargetView(rtv, black, 0, nullptr);

      b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
      b.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
      cmdList->ResourceBarrier(1, &b);
    }
    cmdList->Close();
    ID3D12CommandList* lists[] = { cmdList };
    _device.GetCommandQueue()->ExecuteCommandLists(1, lists);
    FlushCommandQueue();
  }

  spdlog::info("DX12: HDR buffer + TAA history created ({}x{}), SRV_HDR={}, SRV_TAA={}", w, h, SRV_HDR_BUFFER, SRV_TAA_HISTORY);
}

// ---- Create Static Descriptors in shader-visible heap ----
void DX12GpuScene::CreateStaticDescriptors() {
  auto* dev = _device.GetDevice();

  DXGI_FORMAT gbufferFormats[4] = {
    DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,
    DXGI_FORMAT_R16G16B16A16_FLOAT,
    DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,
    DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,
  };

  // G-buffer SRVs [1..4]
  for (int i = 0; i < 4; ++i) {
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = gbufferFormats[i];
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MipLevels = 1;
    dev->CreateShaderResourceView(_gbuffers[i].Get(), &srvDesc,
        _cbvSrvUavHeap.GetStaticCPU(SRV_GBUFFER_START + i));
  }

  // Window depth SRV [5]
  {
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = DXGI_FORMAT_R32_FLOAT; // SRV view of D32_FLOAT
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MipLevels = 1;
    dev->CreateShaderResourceView(_device.GetDepthStencilBuffer(), &srvDesc,
        _cbvSrvUavHeap.GetStaticCPU(SRV_DEPTH));
  }

  // AO SRV [6]
  {
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = DXGI_FORMAT_R8_UNORM;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MipLevels = 1;
    dev->CreateShaderResourceView(_aoTexture.Get(), &srvDesc,
        _cbvSrvUavHeap.GetStaticCPU(SRV_AO));
  }

  // Materials SRV [10]
  if (_materialBuffer) {
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = DXGI_FORMAT_UNKNOWN;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Buffer.NumElements = (UINT)_applMesh->_materialCount;
    srvDesc.Buffer.StructureByteStride = sizeof(AAPLShaderMaterial);
    dev->CreateShaderResourceView(_materialBuffer.Get(), &srvDesc,
        _cbvSrvUavHeap.GetStaticCPU(SRV_MATERIALS));
  }

  // MeshChunks SRV [11]
  if (_meshChunksBuffer) {
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = DXGI_FORMAT_UNKNOWN;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Buffer.NumElements = (UINT)_applMesh->_chunkCount;
    srvDesc.Buffer.StructureByteStride = MESH_CHUNK_STRIDE;
    dev->CreateShaderResourceView(_meshChunksBuffer.Get(), &srvDesc,
        _cbvSrvUavHeap.GetStaticCPU(SRV_MESH_CHUNKS));
  }

  spdlog::info("DX12: Static descriptors created");
}

// ---- Create HiZ Resources ----
void DX12GpuScene::CreateHiZResources() {
  auto* dev = _device.GetDevice();
  uint32_t w = _device.GetWidth(), h = _device.GetHeight();
  _hizMipLevels = (uint32_t)floor(log2f((float)(w > h ? w : h))) + 1;

  // HiZ pyramid texture (R32_FLOAT, full mip chain, UAV for compute writes)
  _hizTexture = DX12Util::CreateTexture2D(dev, w, h, DXGI_FORMAT_R32_FLOAT,
      D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
      _hizMipLevels, 1, D3D12_RESOURCE_STATE_COMMON);

  // SAO depth pyramid (same format, full mip chain)
  _saoMipLevels = _hizMipLevels;
  _saoDepthPyramid = DX12Util::CreateTexture2D(dev, w, h, DXGI_FORMAT_R32_FLOAT,
      D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
      _saoMipLevels, 1, D3D12_RESOURCE_STATE_COMMON);

  spdlog::info("DX12: HiZ resources created ({}x{}, {} mips)", w, h, _hizMipLevels);
}

// ---- Create Shadow Resources ----
void DX12GpuScene::CreateShadowResources() {
  auto* dev = _device.GetDevice();

  // Shadow map Texture2DArray (3 cascades)
  D3D12_CLEAR_VALUE depthClear = {};
  depthClear.Format = DXGI_FORMAT_D32_FLOAT;
  // Standard-Z (not reverse-Z): the shadow PSO uses DepthFunc = LESS and the
  // comparison sampler uses LESS_EQUAL, matching Vulkan (Shadow.cpp clears to
  // 1.0 with VK_COMPARE_OP_LESS). Clearing to 0.0 here would make every
  // fragment fail the depth test and leave the map empty.
  depthClear.DepthStencil.Depth = 1.0f;
  _shadowMapArray = DX12Util::CreateTexture2D(dev, SHADOW_MAP_SIZE, SHADOW_MAP_SIZE,
      DXGI_FORMAT_R32_TYPELESS, // typeless for both DSV and SRV views
      D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL,
      1, SHADOW_CASCADE_COUNT,
      D3D12_RESOURCE_STATE_DEPTH_WRITE, &depthClear);

  // DSV heap (one per cascade)
  {
    D3D12_DESCRIPTOR_HEAP_DESC desc = {};
    desc.NumDescriptors = SHADOW_CASCADE_COUNT;
    desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    dev->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&_shadowDsvHeap));

    uint32_t dsvSize = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle = _shadowDsvHeap->GetCPUDescriptorHandleForHeapStart();
    for (uint32_t i = 0; i < SHADOW_CASCADE_COUNT; ++i) {
      D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
      dsvDesc.Format = DXGI_FORMAT_D32_FLOAT;
      dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
      dsvDesc.Texture2DArray.FirstArraySlice = i;
      dsvDesc.Texture2DArray.ArraySize = 1;
      dev->CreateDepthStencilView(_shadowMapArray.Get(), &dsvDesc, dsvHandle);
      dsvHandle.ptr += dsvSize;
    }
  }

  // SRV for shadow maps (full array, for deferred lighting)
  {
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = DXGI_FORMAT_R32_FLOAT;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2DArray.MipLevels = 1;
    srvDesc.Texture2DArray.ArraySize = SHADOW_CASCADE_COUNT;
    dev->CreateShaderResourceView(_shadowMapArray.Get(), &srvDesc,
        _cbvSrvUavHeap.GetStaticCPU(SRV_SHADOW_MAPS));
  }

  spdlog::info("DX12: Shadow resources created ({}x{}, {} cascades)",
               SHADOW_MAP_SIZE, SHADOW_MAP_SIZE, SHADOW_CASCADE_COUNT);

  // Shadow depth PSO (uses dedicated shadow root sig with cascade index constant)
  {
    auto vs = DX12Util::ReadShaderFile((_rootPath / "shaders/drawclusterShadow.vs.cso").generic_string());
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};
    desc.pRootSignature = _shadowRootSig.Get(); // CBV for VP + root const for cascade index
    desc.VS = {vs.data(), vs.size()};
    D3D12_INPUT_ELEMENT_DESC layout[] = {
      {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
      {"NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 1, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
      {"Tangent",  0, DXGI_FORMAT_R32G32B32_FLOAT, 2, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
      {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    3, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    };
    desc.InputLayout = {layout, 4};
    desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    desc.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
    // Shadow pass is the one geometry pass Vulkan renders CCW-front
    // (Shadow.cpp: VK_FRONT_FACE_COUNTER_CLOCKWISE) 鈥?the shadow orthographic
    // projection has no flipX, unlike the camera projection. With the
    // negative-height viewport making rasterization match Vulkan, the mapping
    // is direct: VK_FRONT_FACE_COUNTER_CLOCKWISE -> FrontCounterClockwise TRUE.
    desc.RasterizerState.FrontCounterClockwise = TRUE;
    desc.RasterizerState.DepthClipEnable = TRUE;
    desc.RasterizerState.DepthBias = 1000;
    desc.RasterizerState.DepthBiasClamp = 0.0f;
    desc.RasterizerState.SlopeScaledDepthBias = 1.0f;
    desc.DepthStencilState.DepthEnable = TRUE;
    desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS; // standard-Z orthographic, matches Vulkan VK_COMPARE_OP_LESS
    desc.BlendState.RenderTarget[0].RenderTargetWriteMask = 0;
    desc.SampleMask = UINT_MAX;
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.NumRenderTargets = 0;
    desc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    desc.SampleDesc.Count = 1;

    DX12Util::ThrowIfFailed(
        dev->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&_shadowPSO)),
        "Failed to create shadow PSO");

    // D1: alpha-mask variant — same VS + RenderSceneShadowDepthIndirect PS
    // (SPEC_ALPHA_MASK macro build) for cutout casters, matching Vulkan's
    // _shadowPassPipelineAlphaMask. PS reads meshChunks/materials (t0/t3
    // space1), bindless textures (t0+ space2) and s1 — bound via root sig
    // params [3]/[4] added for this PSO.
    auto psAlpha = DX12Util::ReadShaderFile(
        (_rootPath / "shaders/drawcluster.shadow.indirect.alphamask.ps.cso").generic_string());
    if (!psAlpha.empty()) {
      desc.PS = {psAlpha.data(), psAlpha.size()};
      DX12Util::ThrowIfFailed(
          dev->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&_shadowAlphaMaskPSO)),
          "Failed to create shadow alpha-mask PSO");
    } else {
      spdlog::warn("DX12: drawcluster.shadow.indirect.alphamask.ps.cso missing — "
                   "CSM alpha-masked casters will render unclipped");
    }
  }

  // Shadow cull compute PSO
  {
    auto cs = DX12Util::ReadShaderFile((_rootPath / "shaders/shadowcull.cs.cso").generic_string());
    D3D12_COMPUTE_PIPELINE_STATE_DESC desc = {};
    desc.pRootSignature = _shadowCullRootSig.Get();
    desc.CS = {cs.data(), cs.size()};
    DX12Util::ThrowIfFailed(
        dev->CreateComputePipelineState(&desc, IID_PPV_ARGS(&_shadowCullPSO)),
        "Failed to create shadow cull PSO");
  }

  // Shadow ExecuteIndirect command signature.
  // [CONSTANT(b1 -> root param 1, offset 2) , DRAW_INDEXED] 鈥?the leading uint
  // of each record lands in pushConstants.drawSlot so the VS can resolve
  // chunkIndex[drawSlot]. Signatures that patch root arguments must be created
  // against the root signature they will be used with.
  {
    D3D12_INDIRECT_ARGUMENT_DESC argDescs[2] = {};
    argDescs[0].Type = D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT;
    argDescs[0].Constant.RootParameterIndex = 1; // shadow root sig: 32BIT_CONSTANTS
    argDescs[0].Constant.DestOffsetIn32BitValues = DRAW_SLOT_CONSTANT_OFFSET;
    argDescs[0].Constant.Num32BitValuesToSet = 1;
    argDescs[1].Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED;

    D3D12_COMMAND_SIGNATURE_DESC sigDesc = {};
    sigDesc.ByteStride = sizeof(DX12DrawIndexedArgs);
    sigDesc.NumArgumentDescs = 2;
    sigDesc.pArgumentDescs = argDescs;
    DX12Util::ThrowIfFailed(
        dev->CreateCommandSignature(&sigDesc, _shadowRootSig.Get(),
                                    IID_PPV_ARGS(&_shadowDrawCmdSig)),
        "Failed to create shadow draw command signature");
  }
}

// ---- Create Spot Shadow Resources (array, PSOs, direct-draw args) ----
// Mirrors Shadow.cpp's Vulkan spot shadow path: a 256x256 D32 Texture2DArray
// with MAX_SPOT_SHADOWS slices, an opaque VS-only pipeline and an
// alpha-mask VS+PS pipeline, both drawing every opaque/alpha chunk directly
// (no GPU culling) per light via ExecuteIndirect.
void DX12GpuScene::CreateSpotShadowResources() {
  auto* dev = _device.GetDevice();

  // 1. Shadow map array (standard-Z: LESS + clear 1.0, like the CSM array)
  D3D12_CLEAR_VALUE depthClear = {};
  depthClear.Format = DXGI_FORMAT_D32_FLOAT;
  depthClear.DepthStencil.Depth = 1.0f;
  _spotShadowMapArray = DX12Util::CreateTexture2D(dev,
      SPOT_SHADOW_MAP_SIZE, SPOT_SHADOW_MAP_SIZE,
      DXGI_FORMAT_R32_TYPELESS,
      D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL,
      1, MAX_SPOT_SHADOWS,
      D3D12_RESOURCE_STATE_DEPTH_WRITE, &depthClear);

  // 2. DSV heap (one per slice)
  {
    D3D12_DESCRIPTOR_HEAP_DESC desc = {};
    desc.NumDescriptors = MAX_SPOT_SHADOWS;
    desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    DX12Util::ThrowIfFailed(
        dev->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&_spotShadowDsvHeap)),
        "spot shadow DSV heap");

    uint32_t dsvSize = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle = _spotShadowDsvHeap->GetCPUDescriptorHandleForHeapStart();
    for (uint32_t i = 0; i < MAX_SPOT_SHADOWS; ++i) {
      D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
      dsvDesc.Format = DXGI_FORMAT_D32_FLOAT;
      dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
      dsvDesc.Texture2DArray.FirstArraySlice = i;
      dsvDesc.Texture2DArray.ArraySize = 1;
      dev->CreateDepthStencilView(_spotShadowMapArray.Get(), &dsvDesc, dsvHandle);
      dsvHandle.ptr += dsvSize;
    }
  }

  // 3. Direct-draw argument records: one per opaque+alphaMasked chunk.
  //    Record layout matches the [CONSTANT, DRAW_INDEXED] signature:
  //    { chunkId (patched into push.chunkId), indexCount, 1, indexBegin, 0, 0 }.
  //    Vulkan feeds the same values through firstInstance -> gl_InstanceIndex.
  if (!_cpuChunks.empty()) {
    uint32_t opaqueCount = (uint32_t)_applMesh->_opaqueChunkCount;
    uint32_t alphaCount = (uint32_t)_applMesh->_alphaMaskedChunkCount;
    std::vector<DX12DrawIndexedArgs> records(opaqueCount + alphaCount);
    for (uint32_t c = 0; c < opaqueCount + alphaCount; ++c) {
      records[c].drawSlot = c;
      records[c].draw.IndexCountPerInstance = _cpuChunks[c].indexCount;
      records[c].draw.InstanceCount = 1;
      records[c].draw.StartIndexLocation = _cpuChunks[c].indexBegin;
      records[c].draw.BaseVertexLocation = 0;
      records[c].draw.StartInstanceLocation = 0;
    }
    ComPtr<ID3D12Resource> upload;
    _device.GetCommandAllocator(0)->Reset();
    auto* cmdList = _device.GetCommandList();
    cmdList->Reset(_device.GetCommandAllocator(0), nullptr);
    _spotDrawArgsBuffer = DX12Util::CreateDefaultBuffer(dev, cmdList,
        records.data(), (UINT64)records.size() * sizeof(DX12DrawIndexedArgs), upload);
    cmdList->Close();
    ID3D12CommandList* lists[] = { cmdList };
    _device.GetCommandQueue()->ExecuteCommandLists(1, lists);
    FlushCommandQueue();
  }

  // 4. Command signature: [CONSTANT(param 0, offset 16 = chunkId), DRAW_INDEXED]
  //    鈥?same 24-byte record stride as the CSM signature.
  {
    D3D12_INDIRECT_ARGUMENT_DESC argDescs[2] = {};
    argDescs[0].Type = D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT;
    argDescs[0].Constant.RootParameterIndex = 0; // spot shadow root sig: 32BIT_CONSTANTS
    argDescs[0].Constant.DestOffsetIn32BitValues = 16; // push.chunkId (after the mat4)
    argDescs[0].Constant.Num32BitValuesToSet = 1;
    argDescs[1].Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED;

    D3D12_COMMAND_SIGNATURE_DESC sigDesc = {};
    sigDesc.ByteStride = sizeof(DX12DrawIndexedArgs);
    sigDesc.NumArgumentDescs = 2;
    sigDesc.pArgumentDescs = argDescs;
    DX12Util::ThrowIfFailed(
        dev->CreateCommandSignature(&sigDesc, _spotShadowRootSig.Get(),
                                    IID_PPV_ARGS(&_spotShadowDrawCmdSig)),
        "Failed to create spot shadow draw command signature");
  }

  // 5. PSOs. Rasterizer matches the CSM shadow PSO conventions: the spot
  //    perspective has no Y flip, so the negative-height viewport keeps the
  //    Vulkan-authored VS valid and FrontCounterClockwise=TRUE maps directly
  //    from Vulkan's VK_FRONT_FACE_COUNTER_CLOCKWISE. Depth bias mirrors
  //    Vulkan's constant 1.25 / slope 1.75 (D3D's DepthBias is integral, so
  //    1 is the nearest representable value).
  D3D12_INPUT_ELEMENT_DESC layout[] = {
    {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    {"NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 1, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    {"Tangent",  0, DXGI_FORMAT_R32G32B32_FLOAT, 2, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    3, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
  };
  auto vs = DX12Util::ReadShaderFile((_rootPath / "shaders/drawclusterShadowSpot.vs.cso").generic_string());
  if (!vs.empty()) {
    // Opaque: VS only
    {
      D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};
      desc.pRootSignature = _spotShadowRootSig.Get();
      desc.VS = {vs.data(), vs.size()};
      desc.InputLayout = {layout, 4};
      desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
      desc.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
      desc.RasterizerState.FrontCounterClockwise = TRUE;
      desc.RasterizerState.DepthClipEnable = TRUE;
      desc.RasterizerState.DepthBias = 1;
      desc.RasterizerState.DepthBiasClamp = 0.0f;
      desc.RasterizerState.SlopeScaledDepthBias = 1.75f;
      desc.DepthStencilState.DepthEnable = TRUE;
      desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
      desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS; // standard-Z
      desc.BlendState.RenderTarget[0].RenderTargetWriteMask = 0;
      desc.SampleMask = UINT_MAX;
      desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
      desc.NumRenderTargets = 0;
      desc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
      desc.SampleDesc.Count = 1;
      DX12Util::ThrowIfFailed(
          dev->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&_spotShadowOpaquePSO)),
          "Failed to create spot shadow opaque PSO");
    }
    // Alpha-masked: VS + alpha-clip PS (RenderSceneShadowDepthIndirect with
    // SPEC_ALPHA_MASK, compiled as drawcluster.shadow.indirect.alphamask.ps.cso)
    {
      auto ps = DX12Util::ReadShaderFile(
          (_rootPath / "shaders/drawcluster.shadow.indirect.alphamask.ps.cso").generic_string());
      D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};
      desc.pRootSignature = _spotShadowRootSig.Get();
      desc.VS = {vs.data(), vs.size()};
      if (!ps.empty()) desc.PS = {ps.data(), ps.size()};
      desc.InputLayout = {layout, 4};
      desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
      desc.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
      desc.RasterizerState.FrontCounterClockwise = TRUE;
      desc.RasterizerState.DepthClipEnable = TRUE;
      desc.RasterizerState.DepthBias = 1;
      desc.RasterizerState.DepthBiasClamp = 0.0f;
      desc.RasterizerState.SlopeScaledDepthBias = 1.75f;
      desc.DepthStencilState.DepthEnable = TRUE;
      desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
      desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
      desc.BlendState.RenderTarget[0].RenderTargetWriteMask = 0;
      desc.SampleMask = UINT_MAX;
      desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
      desc.NumRenderTargets = 0;
      desc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
      desc.SampleDesc.Count = 1;
      DX12Util::ThrowIfFailed(
          dev->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&_spotShadowAlphaMaskPSO)),
          "Failed to create spot shadow alpha-mask PSO");
    }
  } else {
    spdlog::warn("DX12: drawclusterShadowSpot.vs.cso missing 鈥?spot shadows disabled");
  }

  spdlog::info("DX12: Spot shadow resources created ({}x{} x {} slices)",
               SPOT_SHADOW_MAP_SIZE, SPOT_SHADOW_MAP_SIZE, MAX_SPOT_SHADOWS);
}

// ---- Render Spot Shadow Maps (once; the scene is static) ----
// Per light: clear the slice, set the light's view-proj as root constants,
// ExecuteIndirect over the opaque records then the alpha-masked records.
// Matches Vulkan Shadow::RenderSpotShadowMaps.
void DX12GpuScene::RenderSpotShadowMaps(ID3D12GraphicsCommandList* cmdList) {
  if (_spotShadowsRendered) return;
  if (!_spotShadowOpaquePSO || !_spotDrawArgsBuffer || _spotViewProjMatrices.empty()) {
    _spotShadowsRendered = true;
    return;
  }

  uint32_t spotCount = (uint32_t)std::min(_spotViewProjMatrices.size(),
                                          (size_t)MAX_SPOT_SHADOWS);
  uint32_t opaqueCount = (uint32_t)_applMesh->_opaqueChunkCount;
  uint32_t alphaCount = (uint32_t)_applMesh->_alphaMaskedChunkCount;

  // Viewport: negative height, same Vulkan-parity reason as every other pass.
  D3D12_VIEWPORT vp = {0, (float)SPOT_SHADOW_MAP_SIZE,
                       (float)SPOT_SHADOW_MAP_SIZE, -(float)SPOT_SHADOW_MAP_SIZE,
                       0.0f, 1.0f};
  D3D12_RECT scissor = {0, 0, (LONG)SPOT_SHADOW_MAP_SIZE, (LONG)SPOT_SHADOW_MAP_SIZE};

  D3D12_VERTEX_BUFFER_VIEW vbvs[4] = {};
  ID3D12Resource* vbs[] = {_vertexBuffer.Get(), _normalBuffer.Get(),
                           _tangentBuffer.Get(), _uvBuffer.Get()};
  uint32_t strides[] = {sizeof(float)*3, sizeof(float)*3, sizeof(float)*3, sizeof(float)*2};
  for (int k = 0; k < 4; ++k) {
    vbvs[k].BufferLocation = vbs[k]->GetGPUVirtualAddress();
    vbvs[k].SizeInBytes = (UINT)vbs[k]->GetDesc().Width;
    vbvs[k].StrideInBytes = strides[k];
  }
  D3D12_INDEX_BUFFER_VIEW ibv = {};
  ibv.BufferLocation = _indexBuffer->GetGPUVirtualAddress();
  ibv.SizeInBytes = (UINT)_indexBuffer->GetDesc().Width;
  ibv.Format = DXGI_FORMAT_R32_UINT;

  // Descriptor table [1]: t0 materials + t3 meshChunks (alpha-mask PS only,
  // but bound for both pipelines 鈥?the opaque VS never touches it).
  auto tbl = _cbvSrvUavHeap.AllocateDynamic(2);
  {
    auto* dev = _device.GetDevice();
    auto ds = _cbvSrvUavHeap.GetDescriptorSize();
    D3D12_SHADER_RESOURCE_VIEW_DESC dm = {};
    dm.Format = DXGI_FORMAT_UNKNOWN;
    dm.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    dm.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    dm.Buffer.NumElements = (UINT)_applMesh->_materialCount;
    dm.Buffer.StructureByteStride = sizeof(AAPLShaderMaterial);
    dev->CreateShaderResourceView(_materialBuffer.Get(), &dm, tbl.cpu);

    D3D12_SHADER_RESOURCE_VIEW_DESC dc = {};
    dc.Format = DXGI_FORMAT_UNKNOWN;
    dc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    dc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    dc.Buffer.NumElements = (UINT)_applMesh->_chunkCount;
    dc.Buffer.StructureByteStride = MESH_CHUNK_STRIDE;
    dev->CreateShaderResourceView(_meshChunksBuffer.Get(), &dc, {tbl.cpu.ptr + ds});
  }

  uint32_t dsvSize = _device.GetDevice()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);

  // Argument records must be in INDIRECT_ARGUMENT state for ExecuteIndirect.
  // Left in that state afterwards 鈥?nothing else ever touches this buffer and
  // the one-time render never runs again.
  DX12Util::TransitionBarrier(cmdList, _spotDrawArgsBuffer.Get(),
      D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);

  cmdList->SetGraphicsRootSignature(_spotShadowRootSig.Get());
  cmdList->SetGraphicsRootDescriptorTable(1, tbl.gpu);
  cmdList->SetGraphicsRootDescriptorTable(2, _cbvSrvUavHeap.GetStaticGPU(SRV_BINDLESS_START));
  cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  cmdList->IASetVertexBuffers(0, 4, vbvs);
  cmdList->IASetIndexBuffer(&ibv);
  cmdList->RSSetViewports(1, &vp);
  cmdList->RSSetScissorRects(1, &scissor);

  for (uint32_t lightIdx = 0; lightIdx < spotCount; ++lightIdx) {
    D3D12_CPU_DESCRIPTOR_HANDLE dsv = _spotShadowDsvHeap->GetCPUDescriptorHandleForHeapStart();
    dsv.ptr += (SIZE_T)lightIdx * dsvSize;
    cmdList->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
    cmdList->OMSetRenderTargets(0, nullptr, FALSE, &dsv);

    // Root constants: 16 matrix floats. The stored mat4 is already the
    // HLSL-ready column-major form (see CreateLights), so upload verbatim.
    // chunkId (offset 16) is patched per draw by the command signature.
    cmdList->SetGraphicsRoot32BitConstants(0, 16,
        _spotViewProjMatrices[lightIdx].value_ptr(), 0);

    if (opaqueCount > 0) {
      cmdList->SetPipelineState(_spotShadowOpaquePSO.Get());
      cmdList->ExecuteIndirect(_spotShadowDrawCmdSig.Get(), opaqueCount,
          _spotDrawArgsBuffer.Get(), 0, nullptr, 0);
    }
    if (alphaCount > 0 && _spotShadowAlphaMaskPSO) {
      cmdList->SetPipelineState(_spotShadowAlphaMaskPSO.Get());
      cmdList->ExecuteIndirect(_spotShadowDrawCmdSig.Get(), alphaCount,
          _spotDrawArgsBuffer.Get(),
          (UINT64)opaqueCount * sizeof(DX12DrawIndexedArgs), nullptr, 0);
    }
  }

  // Transition the whole array to SRV for deferred lighting (t13).
  DX12Util::TransitionBarrier(cmdList, _spotShadowMapArray.Get(),
      D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

  _spotShadowsRendered = true;
  spdlog::info("DX12: Spot shadow maps rendered for {} lights", spotCount);
}

// ---- Debug dump (AVR_DUMP=<dir>) ----
// Screen captures of the swapchain are unreliable (window z-order, focus,
// DWM ghost windows), so this dumps the render targets directly from the GPU.
// Triggered once after 60 frames (TAA converged, streaming settled) when the
// AVR_DUMP environment variable names an output directory.
void DX12GpuScene::DebugDumpFrame() {
  const char* dirEnv = getenv("AVR_DUMP");
  if (!dirEnv || !*dirEnv) return;
  std::filesystem::path dir(dirEnv);
  std::filesystem::create_directories(dir);

  auto* dev = _device.GetDevice();

  // One-shot allocator + command list (the frame's list is closed/presented).
  ComPtr<ID3D12CommandAllocator> alloc;
  ComPtr<ID3D12GraphicsCommandList> list;
  DX12Util::ThrowIfFailed(dev->CreateCommandAllocator(
      D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc)), "dump alloc");
  DX12Util::ThrowIfFailed(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
      alloc.Get(), nullptr, IID_PPV_ARGS(&list)), "dump list");
  list->Close();
  alloc->Reset();
  list->Reset(alloc.Get(), nullptr);

  // --- helper: readback staging buffer with copy footprints for a texture ---
  auto copyTexToBuf = [&](ID3D12Resource* tex, D3D12_RESOURCE_STATES before,
                          UINT firstSlice, UINT numSlices,
                          ComPtr<ID3D12Resource>& outBuf,
                          std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT>& footprints,
                          UINT64& totalSize) {
    D3D12_RESOURCE_DESC d = tex->GetDesc();
    UINT numSub = d.MipLevels * d.DepthOrArraySize;
    footprints.resize(numSlices);
    // footprint for the requested contiguous slice range
    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> all(numSub);
    // pNumRows/pRowSizeInBytes are PER-SUBRESOURCE arrays, not scalars.
    std::vector<UINT> numRowsVec(numSlices, 0);
    UINT64 size = 0;
    dev->GetCopyableFootprints(&d, firstSlice, numSlices, 0, all.data(),
                               numRowsVec.data(), nullptr, &size);
    UINT64 offset = 0;
    for (UINT i = 0; i < numSlices; ++i) {
      footprints[i] = all[i];
      footprints[i].Offset = offset;
      offset += (UINT64)all[i].Footprint.RowPitch * all[i].Footprint.Height;
      offset = (offset + 511) & ~511ull; // D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT
    }
    totalSize = offset;
    D3D12_HEAP_PROPERTIES hp = {}; hp.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC bd = {};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = totalSize; bd.Height = 1; bd.DepthOrArraySize = 1;
    bd.MipLevels = 1; bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    DX12Util::ThrowIfFailed(dev->CreateCommittedResource(&hp,
        D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
        IID_PPV_ARGS(&outBuf)), "dump staging");

    // transition in, copies, transition back
    DX12Util::TransitionBarrier(list.Get(), tex, before, D3D12_RESOURCE_STATE_COPY_SOURCE);
    for (UINT i = 0; i < numSlices; ++i) {
      D3D12_TEXTURE_COPY_LOCATION dst = {};
      dst.pResource = outBuf.Get();
      dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
      dst.PlacedFootprint = footprints[i];
      D3D12_TEXTURE_COPY_LOCATION src = {};
      src.pResource = tex;
      src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
      src.SubresourceIndex = firstSlice + i;
      list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    }
    DX12Util::TransitionBarrier(list.Get(), tex,
        D3D12_RESOURCE_STATE_COPY_SOURCE, before);
  };

  // --- helper: readback whole buffer ---
  auto copyBuf = [&](ID3D12Resource* buf, D3D12_RESOURCE_STATES before,
                     ComPtr<ID3D12Resource>& outBuf) {
    UINT64 sz = buf->GetDesc().Width;
    D3D12_HEAP_PROPERTIES hp = {}; hp.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC bd = {};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = sz; bd.Height = 1; bd.DepthOrArraySize = 1;
    bd.MipLevels = 1; bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    DX12Util::ThrowIfFailed(dev->CreateCommittedResource(&hp,
        D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
        IID_PPV_ARGS(&outBuf)), "dump staging buf");
    DX12Util::TransitionBarrier(list.Get(), buf, before, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->CopyBufferRegion(outBuf.Get(), 0, buf, 0, sz);
    DX12Util::TransitionBarrier(list.Get(), buf,
        D3D12_RESOURCE_STATE_COPY_SOURCE, before);
  };

  // --- PPM writer ---
  auto writePPM = [&](const std::filesystem::path& p, uint32_t w, uint32_t h,
                      const std::vector<uint8_t>& rgb) {
    FILE* f = nullptr;
    if (fopen_s(&f, p.string().c_str(), "wb") == 0 && f) {
      fprintf(f, "P6\n%u %u\n255\n", w, h);
      fwrite(rgb.data(), 1, rgb.size(), f);
      fclose(f);
    }
  };

  // half -> float
  auto hf = [](uint16_t v) -> float {
    uint32_t sign = (v >> 15) & 1, exp = (v >> 10) & 0x1f, man = v & 0x3ff;
    uint32_t bits;
    if (exp == 0) bits = (sign << 31) | (man << 13); // subnormal -> tiny
    else if (exp == 31) bits = (sign << 31) | 0x7f800000u | (man << 13);
    else bits = (sign << 31) | ((exp - 15 + 127) << 23) | (man << 13);
    float f; memcpy(&f, &bits, 4); return f;
  };
  auto sat = [](float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); };

  ComPtr<ID3D12Resource> hdrBuf, spotBuf, csmBuf, sliBuf, pliBuf;
  std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> hdrFp, spotFp, csmFp;
  UINT64 sz = 0;

  // HDR buffer (post-lighting, pre-resolve). State after Draw: RENDER_TARGET.
  copyTexToBuf(_hdrBuffer.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
               0, 1, hdrBuf, hdrFp, sz);
  // Spot shadow array (all slices). State: PIXEL_SHADER_RESOURCE.
  UINT spotSlices = std::min<UINT>((UINT)_spotLights.size(), MAX_SPOT_SHADOWS);
  if (_spotShadowMapArray && spotSlices > 0)
    copyTexToBuf(_spotShadowMapArray.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                 0, spotSlices, spotBuf, spotFp, sz);
  // CSM cascades. State: DEPTH_WRITE.
  if (_shadowMapArray)
    copyTexToBuf(_shadowMapArray.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE,
                 0, SHADOW_CASCADE_COUNT, csmBuf, csmFp, sz);
  // Tile index buffers. State: COMMON (restored at end of deferred).
  if (_spotLightIndicesBuffer)
    copyBuf(_spotLightIndicesBuffer.Get(), D3D12_RESOURCE_STATE_COMMON, sliBuf);
  if (_lightIndicesBuffer)
    copyBuf(_lightIndicesBuffer.Get(), D3D12_RESOURCE_STATE_COMMON, pliBuf);

  // Scatter accum volume (3D, single subresource — NOT the 2D-array layout the
  // helper above assumes). Rests in PSR after a scatter frame, UAV otherwise.
  ComPtr<ID3D12Resource> scatterBuf;
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT scatterFp = {};
  if (_scatterAccumVolume) {
    D3D12_RESOURCE_DESC d = _scatterAccumVolume->GetDesc();
    UINT numRows = 0; UINT64 scSize = 0;
    dev->GetCopyableFootprints(&d, 0, 1, 0, &scatterFp, &numRows, nullptr, &scSize);
    D3D12_HEAP_PROPERTIES hp = {}; hp.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC bd = {};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = scSize; bd.Height = 1; bd.DepthOrArraySize = 1;
    bd.MipLevels = 1; bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    DX12Util::ThrowIfFailed(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&scatterBuf)), "scatter staging");
    D3D12_RESOURCE_STATES before = _scatterAccumIsInPSR
        ? D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE
        : D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    DX12Util::TransitionBarrier(list.Get(), _scatterAccumVolume.Get(), before,
        D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION dst = {};
    dst.pResource = scatterBuf.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = scatterFp;
    D3D12_TEXTURE_COPY_LOCATION src = {};
    src.pResource = _scatterAccumVolume.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    DX12Util::TransitionBarrier(list.Get(), _scatterAccumVolume.Get(),
        D3D12_RESOURCE_STATE_COPY_SOURCE, before);
  }

  list->Close();
  ID3D12CommandList* lists[] = { list.Get() };
  _device.GetCommandQueue()->ExecuteCommandLists(1, lists);
  FlushCommandQueue();
  spdlog::info("DX12 dump: analyzing");

  // ================= analysis =================
  // 1. HDR: stats + ACES-tonemapped PPM
  if (hdrBuf) {
    const auto& fp = hdrFp[0].Footprint;
    uint32_t w = fp.Width, h = fp.Height;
    uint16_t* mapped = nullptr;
    if (SUCCEEDED(hdrBuf->Map(0, nullptr, (void**)&mapped))) {
      std::vector<uint8_t> rgb((size_t)w * h * 3);
      float mn[4] = {1e30f,1e30f,1e30f,1e30f}, mx[4] = {-1e30f,-1e30f,-1e30f,-1e30f};
      double sum[4] = {0,0,0,0};
      size_t px = 0; // byte offset into rgb (3 bytes per pixel)
      for (uint32_t y = 0; y < h; ++y) {
        const uint16_t* row = mapped + (size_t)y * (fp.RowPitch / 2);
        for (uint32_t x = 0; x < w; ++x, px += 3) {
          float v[4];
          for (int c = 0; c < 4; ++c) {
            v[c] = hf(row[(size_t)x * 4 + c]);
            mn[c] = (std::min)(mn[c], v[c]); mx[c] = (std::max)(mx[c], v[c]);
            sum[c] += v[c];
          }
          // ACES (same curve as resolve.hlsl), then sRGB gamma
          float ex = _frameConstants.exposure;
          for (int c = 0; c < 3; ++c) {
            float t = v[c] * ex;
            t = sat((t * (2.51f * t + 0.03f)) / (t * (2.43f * t + 0.59f) + 0.14f));
            t = std::pow(t, 1.0f / 2.2f);
            rgb[px + c] = (uint8_t)(t * 255.0f + 0.5f);
          }
        }
      }
      hdrBuf->Unmap(0, nullptr);
      writePPM(dir / "hdr_tonemapped.ppm", w, h, rgb);
      FILE* f = nullptr;
      if (fopen_s(&f, (dir / "hdr_stats.txt").string().c_str(), "w") == 0 && f) {
        fprintf(f, "HDR buffer %ux%u RGBA16F (pre-resolve, linear):\n", w, h);
        for (int c = 0; c < 4; ++c)
          fprintf(f, "  ch%d: min=%.4f max=%.4f mean=%.4f\n", c, mn[c], mx[c],
                  sum[c] / (double)(w * h));
        fprintf(f, "exposure=%.4f taaEnabled=%u\n", _frameConstants.exposure,
                _frameConstants.taaEnabled);
        fclose(f);
      }
    }
  }

  // 2. Spot shadows: per-slice stats + slice 0 PPM (depth: white=clear 1.0)
  if (spotBuf) {
    FILE* f = nullptr;
    if (fopen_s(&f, (dir / "spotshadow_stats.txt").string().c_str(), "w") == 0 && f) {
      float* mapped = nullptr;
      if (SUCCEEDED(spotBuf->Map(0, nullptr, (void**)&mapped))) {
      for (UINT s = 0; s < spotSlices; ++s) {
        const auto& fp = spotFp[s].Footprint;
        const float* base = (const float*)((uint8_t*)mapped + spotFp[s].Offset);
        float mn = 1e30f, mx = -1e30f; double sum = 0; uint32_t lit = 0;
        for (uint32_t y = 0; y < fp.Height; ++y)
          for (uint32_t x = 0; x < fp.Width; ++x) {
            float d = base[(size_t)y * (fp.RowPitch / 4) + x];
            mn = (std::min)(mn, d); mx = (std::max)(mx, d); sum += d;
            if (d < 0.999f) ++lit;
          }
        fprintf(f, "slice %2u (%ux%u): min=%.4f max=%.4f mean=%.4f geometry-px=%.2f%%\n",
                s, fp.Width, fp.Height, mn, mx, sum / (fp.Width * (double)fp.Height),
                100.0 * lit / (fp.Width * (double)fp.Height));
        if (s == 0) {
          std::vector<uint8_t> rgb((size_t)fp.Width * fp.Height * 3);
          for (uint32_t y = 0; y < fp.Height; ++y)
            for (uint32_t x = 0; x < fp.Width; ++x) {
              float d = base[(size_t)y * (fp.RowPitch / 4) + x];
              uint8_t g = (uint8_t)(sat(d) * 255.0f + 0.5f);
              size_t i = ((size_t)y * fp.Width + x) * 3;
              rgb[i] = rgb[i+1] = rgb[i+2] = g;
            }
          writePPM(dir / "spotshadow_0.ppm", fp.Width, fp.Height, rgb);
        }
      }
      spotBuf->Unmap(0, nullptr);
      }
      fclose(f);
    }
  }

  // 3. CSM cascades: stats
  if (csmBuf) {
    FILE* f = nullptr;
    if (fopen_s(&f, (dir / "csm_stats.txt").string().c_str(), "w") == 0 && f) {
      float* mapped = nullptr;
      if (SUCCEEDED(csmBuf->Map(0, nullptr, (void**)&mapped))) {
        for (UINT s = 0; s < SHADOW_CASCADE_COUNT; ++s) {
          const auto& fp = csmFp[s].Footprint;
          const float* base = (const float*)((uint8_t*)mapped + csmFp[s].Offset);
          float mn = 1e30f, mx = -1e30f; double sum = 0; uint32_t lit = 0;
          for (uint32_t y = 0; y < fp.Height; ++y)
            for (uint32_t x = 0; x < fp.Width; ++x) {
              float d = base[(size_t)y * (fp.RowPitch / 4) + x];
              mn = (std::min)(mn, d); mx = (std::max)(mx, d); sum += d;
              if (d < 0.999f) ++lit;
            }
          fprintf(f, "cascade %u (%ux%u): min=%.4f max=%.4f mean=%.4f geometry-px=%.2f%%\n",
                  s, fp.Width, fp.Height, mn, mx,
                  sum / (fp.Width * (double)fp.Height),
                  100.0 * lit / (fp.Width * (double)fp.Height));
        }
        csmBuf->Unmap(0, nullptr);
      }
      fclose(f);
    }
  }

  // 4. Tile light-index counts (first 400 tiles)
  if (sliBuf && pliBuf) {
    FILE* f = nullptr;
    if (fopen_s(&f, (dir / "tile_stats.txt").string().c_str(), "w") == 0 && f) {
      uint32_t* sm = nullptr; uint32_t* pm = nullptr;
      if (SUCCEEDED(sliBuf->Map(0, nullptr, (void**)&sm)) &&
          SUCCEEDED(pliBuf->Map(0, nullptr, (void**)&pm))) {
        uint32_t tileW = (_device.GetWidth() + LIGHT_TILE_SIZE - 1) / LIGHT_TILE_SIZE;
        uint32_t tileH = (_device.GetHeight() + LIGHT_TILE_SIZE - 1) / LIGHT_TILE_SIZE;
        uint32_t tiles = std::min<uint32_t>(tileW * tileH, 400);
        uint32_t spotTot = 0, ptTot = 0, spotTiles = 0, ptTiles = 0;
        for (uint32_t t = 0; t < tileW * tileH; ++t) {
          uint32_t sc = sm[(size_t)t * MAX_LIGHTS_PER_TILE_LC];
          uint32_t pc = pm[(size_t)t * MAX_LIGHTS_PER_TILE_LC];
          spotTot += (std::min)(sc, 63u); ptTot += (std::min)(pc, 63u);
          if (sc > 0) spotTiles++;
          if (pc > 0) ptTiles++;
          if (t < tiles)
            fprintf(f, "tile %3u (row %2u col %2u): points=%u spots=%u\n", t,
                    t / tileW, t % tileW, pc, sc);
        }
        fprintf(f, "\nSUMMARY: tiles=%u  tiles-with-points=%u (avg %.2f)  tiles-with-spots=%u (avg %.2f)\n",
                tileW * tileH, ptTiles, (double)ptTot / (tileW * (double)tileH),
                spotTiles, (double)spotTot / (tileW * (double)tileH));
        sliBuf->Unmap(0, nullptr);
        pliBuf->Unmap(0, nullptr);
      }
      fclose(f);
    }
  }

  // 5. Scatter accum volume: per-depth-slice stats. Physics check: alpha
  // (transmittance) must be monotonically non-increasing with depth, rgb
  // (accumulated inscatter) non-decreasing; near slices ~1.0 alpha, far < 1.
  if (scatterBuf) {
    FILE* f = nullptr;
    if (fopen_s(&f, (dir / "scatter_stats.txt").string().c_str(), "w") == 0 && f) {
      uint16_t* mapped = nullptr;
      if (SUCCEEDED(scatterBuf->Map(0, nullptr, (void**)&mapped))) {
        const auto& fp = scatterFp.Footprint;
        fprintf(f, "Scatter accum %ux%ux%u RGBA16F (rgb=inscatter, a=transmittance):\n",
                fp.Width, fp.Height, fp.Depth);
        float prevAlpha = 1.0f, prevLum = -1.0f;
        bool alphaMono = true, lumMono = true;
        double alphaFirst = 0, alphaLast = 0, lumLast = 0;
        for (UINT z = 0; z < fp.Depth; ++z) {
          float aMn = 1e30f, aMx = -1e30f, lumMn = 1e30f, lumMx = -1e30f;
          double aSum = 0, rSum = 0, gSum = 0, bSum = 0, lumSum = 0;
          for (uint32_t y = 0; y < fp.Height; ++y) {
            const uint16_t* row = mapped +
                ((size_t)z * fp.Height + y) * (fp.RowPitch / 2);
            for (uint32_t x = 0; x < fp.Width; ++x) {
              float r = hf(row[(size_t)x * 4 + 0]);
              float g = hf(row[(size_t)x * 4 + 1]);
              float b = hf(row[(size_t)x * 4 + 2]);
              float a = hf(row[(size_t)x * 4 + 3]);
              aMn = (std::min)(aMn, a); aMx = (std::max)(aMx, a);
              float lum = r * 0.2126f + g * 0.7152f + b * 0.0722f;
              lumMn = (std::min)(lumMn, lum); lumMx = (std::max)(lumMx, lum);
              aSum += a; rSum += r; gSum += g; bSum += b; lumSum += lum;
            }
          }
          double n = fp.Width * (double)fp.Height;
          float aMean = (float)(aSum / n), lumMean = (float)(lumSum / n);
          if (z == 0) { alphaFirst = aSum / n; }
          alphaLast = aSum / n; lumLast = lumSum / n;
          if (aMean > prevAlpha + 1e-4f) alphaMono = false;
          if (lumMean < prevLum - 1e-4f) lumMono = false;
          prevAlpha = aMean; prevLum = lumMean;
          if (z % 8 == 0 || z == fp.Depth - 1)
            fprintf(f, "  z=%2u: a[min=%.4f mean=%.4f max=%.4f]  rgb-mean=(%.5f,%.5f,%.5f)\n",
                    z, aMn, aMean, aMx, rSum / n, gSum / n, bSum / n);
        }
        fprintf(f, "alpha(first=%.4f last=%.4f) lum(first-slices~%.4f last=%.4f)\n",
                alphaFirst, alphaLast, prevLum, lumLast);
        fprintf(f, "monotonic: alpha-nonincreasing=%s rgb-nondecreasing=%s\n",
                alphaMono ? "YES" : "NO", lumMono ? "YES" : "NO");
        scatterBuf->Unmap(0, nullptr);
      }
      fclose(f);
    }
  }

  spdlog::info("DX12: Debug frame dumped to '{}'", dirEnv);
}

// ---- Create Command Signature for ExecuteIndirect ----
void DX12GpuScene::CreateCommandSignature() {
  // Same [CONSTANT, DRAW_INDEXED] layout as the shadow signature; the
  // drawcluster root signature puts its 32BIT_CONSTANTS at param 3.
  D3D12_INDIRECT_ARGUMENT_DESC argDescs[2] = {};
  argDescs[0].Type = D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT;
  argDescs[0].Constant.RootParameterIndex = 3;
  argDescs[0].Constant.DestOffsetIn32BitValues = DRAW_SLOT_CONSTANT_OFFSET;
  argDescs[0].Constant.Num32BitValuesToSet = 1;
  argDescs[1].Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED;

  D3D12_COMMAND_SIGNATURE_DESC sigDesc = {};
  sigDesc.ByteStride = sizeof(DX12DrawIndexedArgs); // 4 (constant) + 20 (draw)
  sigDesc.NumArgumentDescs = 2;
  sigDesc.pArgumentDescs = argDescs;

  DX12Util::ThrowIfFailed(
      _device.GetDevice()->CreateCommandSignature(
          &sigDesc, _drawClusterRootSig.Get(), IID_PPV_ARGS(&_drawIndexedCmdSig)),
      "Failed to create draw indexed command signature");
}

// ---- Flush Command Queue ----
void DX12GpuScene::FlushCommandQueue() {
  _device.WaitForGpu();
}

// ---- Texture streaming pipeline(闀滃儚 GpuScene.cpp 鐨?Vulkan 璺緞)----
void DX12GpuScene::initTextureStreaming() {
  _retiredResources.resize(DX12Device::FRAME_COUNT);
  auto* dev = _device.GetDevice();
  DX12Util::ThrowIfFailed(
      dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
          IID_PPV_ARGS(&_streamingCmdAllocator)), "streaming cmd allocator");
  DX12Util::ThrowIfFailed(
      dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
          _streamingCmdAllocator.Get(), nullptr,
          IID_PPV_ARGS(&_streamingCmdList)), "streaming cmd list");
  _streamingCmdList->Close();
  _streamingThreadRunning = true;
  _streamingThread = std::thread(&DX12GpuScene::blitThreadFunc, this);
}

void DX12GpuScene::shutdownTextureStreaming() {
  _streamingThreadRunning = false;
  _pendingCv.notify_all();
  if (_streamingThread.joinable()) _streamingThread.join();
  _pendingWorks.clear();
  _cpuCompletedWorks.clear(); // ComPtr 鑷姩閲婃斁
  for (auto& slot : _retiredResources) slot.clear();
}

void DX12GpuScene::dispatchStreamingRequest(size_t entryIndex) {
  auto& entry = _streamEntries[entryIndex];
  PendingStreamWork work;
  work.entryIndex = entryIndex;
  work.targetMip = entry.requiredMip;
  work.snapshotCurrentMip = entry.currentMip;
  work.snapshotSource = _textures[entryIndex];
  // 鍏堝叆闃熷墠缃綅,闃蹭笅涓€甯ч噸澶?dispatch(瀵归綈 GpuScene.cpp:5442)
  entry.inFlight = true;
  {
    std::lock_guard<std::mutex> lock(_pendingMutex);
    _pendingWorks.push_back(std::move(work));
  }
  _pendingCv.notify_one();
}

void DX12GpuScene::blitThreadFunc() {
  // CPU-only:寤烘浛浠ｇ汗鐞嗐€佽В鍘嬫柊 mip 杩?staging銆侴PU 鍛戒护鍦ㄤ富绾跨▼褰曞埗銆?
  // (D3D12 device 鏂规硶 free-threaded;鍛戒护褰曞埗/Execute/SRV 閲嶅啓鍦ㄤ富绾跨▼銆?
  namespace TSP = DX12TextureStreamingPolicy;
  while (_streamingThreadRunning) {
    std::unique_lock<std::mutex> lock(_pendingMutex);
    _pendingCv.wait(lock, [this] {
      return !_pendingWorks.empty() || !_streamingThreadRunning;
    });
    if (!_streamingThreadRunning) break;

    std::vector<PendingStreamWork> workItems;
    std::swap(workItems, _pendingWorks);
    lock.unlock();

    for (auto& work : workItems) {
      auto& entry = _streamEntries[work.entryIndex];
      const AAPLTextureData& desc = *entry.desc;

      CpuStreamingWork cpuWork;
      cpuWork.entryIndex = work.entryIndex;
      cpuWork.targetMip = work.targetMip;
      cpuWork.sourceTexture = work.snapshotSource;

      try {
        cpuWork.format = MapMTLToDXGI(desc._pixelFormat);
        cpuWork.plan = TSP::BuildStreamingCopyPlan(desc._width, desc._height,
            desc._mipmapLevelCount, desc._pixelFormat, work.targetMip,
            work.snapshotCurrentMip);

        uint32_t baseW = (uint32_t)((desc._width >> work.targetMip) > 0
            ? (desc._width >> work.targetMip) : 1);
        uint32_t baseH = (uint32_t)((desc._height >> work.targetMip) > 0
            ? (desc._height >> work.targetMip) : 1);

        D3D12_HEAP_PROPERTIES heapProps = {};
        heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC texDesc = {};
        texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        texDesc.Width = baseW;
        texDesc.Height = baseH;
        texDesc.DepthOrArraySize = 1;
        texDesc.MipLevels = (UINT16)cpuWork.plan.newMipCount;
        texDesc.Format = cpuWork.format;
        texDesc.SampleDesc.Count = 1;
        DX12Util::ThrowIfFailed(
            _device.GetDevice()->CreateCommittedResource(&heapProps,
                D3D12_HEAP_FLAG_NONE, &texDesc, D3D12_RESOURCE_STATE_COPY_DEST,
                nullptr, IID_PPV_ARGS(&cpuWork.newTexture)),
            "streaming: create texture");

        if (cpuWork.plan.stagingSize > 0) {
          cpuWork.staging = DX12Util::CreateUploadBuffer(_device.GetDevice(),
              cpuWork.plan.stagingSize, nullptr);
          void* mapped = nullptr;
          cpuWork.staging->Map(0, nullptr, &mapped);
          if (!mapped) throw std::runtime_error("streaming: staging map failed");
          for (const auto& copy : cpuWork.plan.bufferCopies) {
            uint32_t srcMip = work.targetMip + copy.dstSubresource;
            uint8_t* compressedSrc = (uint8_t*)_applMesh->_textureData +
                desc._pixelDataOffset + desc._mipOffsets[srcMip];
            auto [mipData, mipSize] = decompressToHeap(compressedSrc,
                desc._mipLengths[srcMip]);
            // 绱у瘑琛岃窛 -> 256 瀵归綈琛岃窛
            for (uint32_t row = 0; row < copy.blockRows; ++row) {
              memcpy((uint8_t*)mapped + copy.stagingOffset + row * copy.rowPitch,
                     (uint8_t*)mipData + row * copy.srcRowPitch, copy.srcRowPitch);
            }
            free(mipData);
          }
          cpuWork.staging->Unmap(0, nullptr);
        }
      } catch (const std::exception& e) {
        spdlog::warn("DX12 streaming: work failed: {}", e.what());
        cpuWork.newTexture.Reset(); // 涓荤嚎绋嬫寜澶辫触 work 澶勭悊:鍙浣?inFlight
        cpuWork.staging.Reset();
      }
      {
        std::lock_guard<std::mutex> clock(_cpuWorkMutex);
        _cpuCompletedWorks.push_back(std::move(cpuWork));
      }
    }
  }
}

void DX12GpuScene::processStreamingWork() {
  std::vector<CpuStreamingWork> workItems;
  {
    std::lock_guard<std::mutex> lock(_cpuWorkMutex);
    std::swap(workItems, _cpuCompletedWorks);
  }
  if (workItems.empty()) return;

  auto* dev = _device.GetDevice();

  // 鍔?static(shader-visible)SRV slot 鎴?transition 鏃х汗鐞嗗墠蹇呴』 GPU 绌洪棽:
  // 鎵€鏈?in-flight 甯у叡浜繖浠芥弿杩扮鍫嗐€傚绛変簬 Vulkan processStreamingWork
  // 鍐?endSingleTimeCommands 鐨?vkQueueWaitIdle鈥斺€斿彧鍦ㄧ湡鏈?work 瀹屾垚鏃跺彂鐢熴€?
  _device.WaitForGpu();

  _streamingCmdAllocator->Reset();
  _streamingCmdList->Reset(_streamingCmdAllocator.Get(), nullptr);

  for (auto& work : workItems) {
    if (!work.newTexture) continue; // 鍚庡彴澶辫触;inFlight 鍦ㄤ笅闈㈢粺涓€澶嶄綅

    if (!work.plan.imageCopies.empty() && work.sourceTexture) {
      DX12Util::TransitionBarrier(_streamingCmdList.Get(), work.sourceTexture.Get(),
          D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
    }
    // 鍏变韩 mip:old -> new
    for (const auto& copy : work.plan.imageCopies) {
      D3D12_TEXTURE_COPY_LOCATION dst = {};
      dst.pResource = work.newTexture.Get();
      dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
      dst.SubresourceIndex = copy.dstSubresource;
      D3D12_TEXTURE_COPY_LOCATION src = {};
      src.pResource = work.sourceTexture.Get();
      src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
      src.SubresourceIndex = copy.srcSubresource;
      // 鍏?subresource copy,浼?nullptr box:BC 鏍煎紡鐨?src box 蹇呴』 4x4 鍧楀榻?
      // (#873 COPYTEXTUREREGION_INVALIDSRCBOX),灏鹃儴 1x1/2x2 mip 涓庨潪 4 鍊嶆暟
      // 灏哄浼氳Е鍙?绌?box 绛変环 CopySubresourceRegion,缁曞紑瀵归綈鏍￠獙銆?
      _streamingCmdList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    }
    // 鏂?mip:staging -> new
    for (const auto& copy : work.plan.bufferCopies) {
      D3D12_TEXTURE_COPY_LOCATION dst = {};
      dst.pResource = work.newTexture.Get();
      dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
      dst.SubresourceIndex = copy.dstSubresource;
      D3D12_TEXTURE_COPY_LOCATION src = {};
      src.pResource = work.staging.Get();
      src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
      src.PlacedFootprint.Offset = copy.stagingOffset;
      src.PlacedFootprint.Footprint.Format = work.format;
      src.PlacedFootprint.Footprint.Width = copy.width;
      src.PlacedFootprint.Footprint.Height = copy.height;
      src.PlacedFootprint.Footprint.Depth = 1;
      src.PlacedFootprint.Footprint.RowPitch = copy.rowPitch;
      _streamingCmdList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    }
    // 鏂扮汗鐞嗗嚭鐢熷嵆 COPY_DEST,鏃犻渶杩涘叆 barrier;鏃х汗鐞嗕笉杞洖(鍗冲皢閫€褰?
    DX12Util::TransitionBarrier(_streamingCmdList.Get(), work.newTexture.Get(),
        D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
  }

  // Close 浼氳繑鍥炲綍鍒舵湡闂寸殑棣栦釜鏍￠獙閿欒(濡傛湁)銆傚嚭閿欐椂缁濅笉鑳?Execute/swap:
  // 鍚﹀垯涓瘨鍒楄〃婊炵暀璧勬簮寮曠敤,鑰?swap 鍙堟敼鍐?SRV/閫€褰规棫绾圭悊鈥斺€?
  // 鐘舵€佽厫璐ョ骇鑱斿埌 OBJECT_DELETED_WHILE_STILL_IN_USE 鐩磋嚦 device removed銆?
  // 杩欓噷鏁存壒鏀惧純:鍙浣?inFlight(涓嬪抚閲嶈瘯),list 鐣欏緟涓嬫 Reset銆?
  HRESULT closeHr = _streamingCmdList->Close();
  if (FAILED(closeHr)) {
    spdlog::error("DX12 streaming: cmd list close failed (HRESULT: 0x{:08X}), batch dropped",
                  (unsigned)closeHr);
    for (auto& work : workItems)
      _streamEntries[work.entryIndex].inFlight = false;
    return;
  }
  ID3D12CommandList* lists[] = { _streamingCmdList.Get() };
  _device.GetCommandQueue()->ExecuteCommandLists(1, lists);

  // CPU 渚?swap銆侴PU 璺?copy 鏃堕噸鍐?SRV slot 鍚堟硶:copy 涓嶈鎻忚堪绗?
  // 涔嬪悗褰曞埗鐨勫懡浠ゅ垪琛ㄨ鍒扮殑宸叉槸鏂版弿杩扮(spec 搂4)銆?
  for (auto& work : workItems) {
    auto& entry = _streamEntries[work.entryIndex];
    entry.inFlight = false;
    if (!work.newTexture) continue;

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = work.format;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MostDetailedMip = 0;
    srvDesc.Texture2D.MipLevels = work.plan.newMipCount;
    dev->CreateShaderResourceView(work.newTexture.Get(), &srvDesc,
        _cbvSrvUavHeap.GetStaticCPU(SRV_BINDLESS_START + (uint32_t)work.entryIndex));

    // 鏃х汗鐞?+ staging 閫€褰?Draw 寮€澶撮殢甯фЫ fence 娓呯┖,鏃犻渶浜屾 stall
    if (work.sourceTexture)
      _retiredResources[_currentFrame].push_back(work.sourceTexture);
    if (work.staging)
      _retiredResources[_currentFrame].push_back(work.staging);

    _textures[work.entryIndex] = work.newTexture;
    spdlog::debug("DX12 streaming: tex[{}] mip {} -> {}", work.entryIndex,
                  entry.currentMip, work.targetMip);
    entry.currentMip = work.targetMip;
  }
}

// 鐞?AABB 瑙嗛敟娴嬭瘯(鐩存妱 GpuScene.cpp:5351 鐨?file-static)
static bool SphereInFrustum(const Frustum& frustum, const AAPLSphere& sphere) {
  AAPLBoundingBox3 aabb;
  aabb.min.x = sphere.data.x - sphere.data.w;
  aabb.min.y = sphere.data.y - sphere.data.w;
  aabb.min.z = sphere.data.z - sphere.data.w;
  aabb.max.x = sphere.data.x + sphere.data.w;
  aabb.max.y = sphere.data.y + sphere.data.w;
  aabb.max.z = sphere.data.z + sphere.data.w;
  return !frustum.FrustumCull(aabb);
}

// ---- Update Texture Streaming (mip LOD selection) ----
void DX12GpuScene::UpdateTextureStreaming() {
  if (_streamEntries.empty()) return;

  namespace TSP = DX12TextureStreamingPolicy;

  // 1. reset requiredMip 鍒?permanent(鍏佽椹遍€愬洖鍘荤殑鏈€绮?mip)
  for (auto& e : _streamEntries) {
    e.requiredMip = TSP::CalculateMinMip(e.desc->_width, e.desc->_height,
                                         e.totalMips, TSP::PERMANENT_TEXTURE_SIZE);
  }

  // 2. 閫?submesh 瑕嗙洊搴?闀滃儚 GpuScene.cpp:5784-5832;鍙彇 proj[0][0] 涓?
  //    view z,涓?NDC Y / 璐熼珮搴?viewport / mat4 瀛樺偍绾﹀畾鏃犲叧)
  Camera* cam = _mainCamera;
  if (cam && _subMeshes && _applMesh && _cpuMaterials) {
    mat4 viewMatrix = cam->getObjectToCamera();
    float focalLength = cam->getProjectMatrix()[0][0];
    float focalLengthSquared = focalLength * focalLength;
    float viewW = (float)_device.GetWidth();
    float viewH = (float)_device.GetHeight();
    Frustum frustum = cam->getFrustum();

    for (unsigned int i = 0; i < _applMesh->_meshCount; ++i) {
      const AAPLSubMesh& mesh = _subMeshes[i];
      const AAPLSphere& sphere = mesh.boundingSphere;
      if (!SphereInFrustum(frustum, sphere)) continue;

      vec4 viewPos = viewMatrix * vec4(sphere.data.x, sphere.data.y,
                                       sphere.data.z, 1.0f);
      float area = TSP::ComputeScreenArea(viewPos.x, viewPos.y, viewPos.z,
          sphere.data.w, focalLengthSquared, viewW, viewH);

      uint32_t matIndex = mesh.materialIndex;
      if (matIndex >= _applMesh->_materialCount) continue;
      const AAPLMaterial& cpuMat = _cpuMaterials[matIndex];

      // MIN 绱Н:鏈€杩戠殑 submesh 璧?瀵归綈 GpuScene.cpp::setRequiredMip)
      auto require = [&](uint32_t hash) {
        auto it = _streamEntryMap.find(hash);
        if (it == _streamEntryMap.end()) return;
        auto& e = _streamEntries[it->second];
        uint32_t mip = TSP::CalculateRequiredMip(e.desc->_width, e.desc->_height,
                                                 e.totalMips, area);
        if (mip < e.requiredMip) e.requiredMip = mip;
      };
      if (cpuMat.hasBaseColorTexture) require(cpuMat.baseColorTextureHash);
      if (cpuMat.hasNormalMap) require(cpuMat.normalMapHash);
      if (cpuMat.hasMetallicRoughnessTexture) require(cpuMat.metallicRoughnessHash);
      if (cpuMat.hasEmissiveTexture) require(cpuMat.emissiveTextureHash);
    }
  }

  _streamWantUpgrade = 0;
  for (auto& e : _streamEntries)
    if (e.requiredMip != e.currentMip) ++_streamWantUpgrade;

  // 3. 娓呯┖鏈抚妲界殑閫€褰硅祫婧愨€斺€斿繀椤诲厛浜?dispatch/processStreamingWork:
  //    鍚庤€呬細寰€鍚屼竴妲藉帇鏂伴€€褰硅祫婧?copy 鍒氭彁浜や粛鍦ㄩ€?銆傚畨鍏ㄦ€?Draw 寮€澶?
  //    _currentFrame 妲界殑涓婅疆 fence 宸茶 MoveToNextFrame 绛夎繃(spec 搂3 姝?3)銆?
  _retiredResources[_currentFrame].clear();

  // 4. dispatch(璺宠繃鍦ㄩ€旀潯鐩?
  for (size_t i = 0; i < _streamEntries.size(); ++i) {
    auto& e = _streamEntries[i];
    if (e.currentMip != e.requiredMip && !e.inFlight)
      dispatchStreamingRequest(i);
  }

  // 5. 娑堣垂鍚庡彴瀹屾垚鐨?work(GPU copy + swap,鏈嚎绋?
  processStreamingWork();
}

// ---- Update Uniforms ----
void DX12GpuScene::UpdateUniforms() {
  auto& frame = _frameResources[_currentFrame];

  _frameConstants.nearPlane = _mainCamera->Near();
  _frameConstants.farPlane = _mainCamera->Far();
  static uint32_t sFrameCounter = 0;
  _frameConstants.frameCounter = sFrameCounter++;
  // Wind-animated noise offset for the scatter volume's Perlin fog detail
  // (Vulkan parity, GpuScene.cpp:3942-3944: ~0.5 m/s horizontal wind).
  {
    float t = (float)_frameConstants.frameCounter * 0.016f;
    _frameConstants.globalNoiseOffset = vec3(t * 0.5f, 0.0f, t * 0.3f);
  }
  _frameConstants.physicalSize = vec2((float)_device.GetWidth(), (float)_device.GetHeight());

  // Build FrameData
  FrameData frameData;
  // Shadow matrices (simple directional light shadow from sun direction)
  {
    vec3 sunDir = normalize(_frameConstants.sunDirection);
    float cascadeSplits[3] = {3.0f, 10.0f, 50.0f};
    for (int i = 0; i < SHADOW_CASCADE_COUNT; ++i) {
      float range = cascadeSplits[i];
      vec3 center = _mainCamera->GetOrigin() + _mainCamera->GetCameraDir() * (range * 0.5f);
      // D2 (Vulkan parity, Shadow.cpp:131): retreat a FIXED 100 units along
      // the sun direction and use a fixed 200-unit far plane. Retreating by
      // only `range` near-clipped tall casters standing between the shadow
      // eye and the ground (a 10-unit tree vs. a 3-unit retreat), so their
      // shadows went missing from the near cascade.
      vec3 eye = center + sunDir * 100.0f;
      // Must use invLookAt (same convention as Camera::getObjectToCamera): the
      // mat4 members hold the ROWS of the math matrix, and the upload below
      // transposes them so HLSL's column-major cbuffer sees the matrix itself.
      // Building the view "column-wise" here instead makes the upload hand
      // HLSL V-transposed, which blows up clip.w and throws every caster
      // off-screen.
      _shadowViewMatrices[i] = invLookAt(eye, vec3(0, 1, 0), sunDir * -1.0f);
      // near MUST stay 0: orthographic() writes its offsets/near term into the
      // 4th *column*, i.e. the opposite storage convention from every other
      // matrix builder here. With near = 0 and zero xy offsets the result is
      // diagonal, so the upload's transpose() is a no-op and HLSL gets the
      // right matrix. A non-zero near lands -n/(f-n) in what HLSL reads as
      // row 3 col 2, making clip.w != 1. Vulkan (Shadow.cpp) passes 0 too.
      _shadowProjectionMatrices[i] = orthographic(range * 2.0f, range * 2.0f,
          0.0f, 200.0f, 0, 0);

      // Record the same box invLookAt/orthographic just encoded, in explicit
      // world-space terms, for the GPU cull frustum (see BuildCascadeFrustum).
      // Basis must match invLookAt exactly: z = normalize(lookat),
      // x = normalize(up x z), y = z x x.
      ShadowCascadeBox& box = _shadowCascadeBoxes[i];
      box.eye   = eye;
      box.axisZ = normalize(sunDir * -1.0f);
      box.axisX = normalize(vec3(0, 1, 0).cross(box.axisZ));
      box.axisY = box.axisZ.cross(box.axisX);
      box.halfExtent = range;      // orthographic() width/height = range * 2
      box.farZ       = 200.0f;     // matches the fixed far plane above
    }
    frameData.camConstants.shadowProjectionMatrix0 = transpose(_shadowProjectionMatrices[0]);
    frameData.camConstants.shadowViewMatrix0 = transpose(_shadowViewMatrices[0]);
    frameData.camConstants.shadowProjectionMatrix1 = transpose(_shadowProjectionMatrices[1]);
    frameData.camConstants.shadowViewMatrix1 = transpose(_shadowViewMatrices[1]);
    frameData.camConstants.shadowProjectionMatrix2 = transpose(_shadowProjectionMatrices[2]);
    frameData.camConstants.shadowViewMatrix2 = transpose(_shadowViewMatrices[2]);
  }
  // Halton jitter for TAA (base 2 and base 3)
  {
    static auto halton = [](uint32_t i, uint32_t base) -> float {
      float f = 1.0f, r = 0.0f;
      while (i > 0) { f /= base; r += f * (i % base); i /= base; }
      return r;
    };
    uint32_t jitterIdx = (_frameConstants.frameCounter % 16) + 1;
    _frameConstants.taaJitter = vec2(
        (halton(jitterIdx, 2) - 0.5f) / (float)_device.GetWidth(),
        (halton(jitterIdx, 3) - 0.5f) / (float)_device.GetHeight());
    _frameConstants.taaEnabled = _taaEnabled ? 1u : 0u;
    _frameConstants.invPhysicalSize = vec2(1.0f / (float)_device.GetWidth(),
                                           1.0f / (float)_device.GetHeight());
  }

  // TAA sub-pixel jitter on the projection used for vertex shading (Vulkan
  // parity, GpuScene.cpp:3980): m[2].w scales z into w, so scaling the jitter
  // by it gives the exact NDC offset regardless of depth.
  mat4 cleanProj = _mainCamera->getProjectMatrix();
  mat4 jitteredProj = cleanProj;
  if (_taaEnabled) {
    float Zt = cleanProj[2].w;
    jitteredProj[2][0] += _frameConstants.taaJitter.x * Zt;
    jitteredProj[2][1] += _frameConstants.taaJitter.y * Zt;
  }
  _jitteredProj = jitteredProj; // reused by the GPU/light cull param uploads

  mat4 cleanView = _mainCamera->getObjectToCamera();
  frameData.camConstants.projectionMatrix = transpose(jitteredProj);
  frameData.camConstants.viewMatrix = transpose(cleanView);
  frameData.camConstants.invViewMatrix = transpose(_mainCamera->getInvViewMatrix());
  // Jittered inverse VP so world-position reconstruction stays consistent
  // with the jittered depth buffer (same as the Vulkan path).
  frameData.camConstants.invViewProjectionMatrix =
      transpose(inverse(cleanView * jitteredProj));
  frameData.camConstants.invProjectionMatrix = transpose(inverse(cleanProj));

  // prevViewProjectionMatrix for TAA reprojection:
  // With mat4 quirk (A*B = math B*A), view*proj gives the mathematical VP.
  // Must be transposed before uploading (HLSL row-major convention).
  {
    static mat4 sPrevVP = mat4(1.0f);
    frameData.camConstants.prevViewProjectionMatrix = transpose(sPrevVP);
    // Save the current JITTERED VP 鈥?the history lookup then compensates for
    // the previous frame's sub-pixel offset, giving sharper accumulation.
    sPrevVP = cleanView * jitteredProj;
  }

  frameData.frameConstants = _frameConstants;
  frameData.frameConstants.scatterScale = DX12ScenePolicy::EffectiveScatterScale(
      _frameConstants.scatterScale, IsScatterReady());

  memcpy(frame.uniformMapped, &frameData, sizeof(FrameData));
}

// ---- Create Lights (place default point lights + alloc GPU buffers) ----
void DX12GpuScene::CreateLights() {
  auto* dev = _device.GetDevice();
  auto* cmdList = _device.GetCommandList();

  // Load point lights from the scene file (matches Vulkan GpuScene.cpp:2822).
  // sqrt_radius is the SQUARED radius 鈥?the cull/lighting shaders use the
  // sphere radius directly (posRadius.w), so take the square root here.
  if (_sceneFile.contains("point_lights")) {
    for (auto& j : _sceneFile["point_lights"]) {
      AAPLPointLightCullingData pl;
      pl.posRadius = vec4(j["position_x"].template get<float>(),
                          j["position_y"].template get<float>(),
                          j["position_z"].template get<float>(),
                          std::sqrt(j["sqrt_radius"].template get<float>()));
      // color.w encodes the transparent flag: >= 0 opaque, < 0 transparent
      // (matches the Vulkan culling data layout).
      pl.color = vec4(j["color_r"].template get<float>(),
                      j["color_g"].template get<float>(),
                      j["color_b"].template get<float>(),
                      j["for_transparent"].template get<bool>() ? -1.0f : 1.0f);
      _pointLights.push_back(pl);
    }
    spdlog::info("DX12: Loaded {} point lights from scene file", _pointLights.size());
  }

  // Fallback: small debug grid when the scene file has no lights
  if (_pointLights.empty()) {
    // Place a small grid of point lights around the scene
    float positions[][3] = {{2,1,0},{-2,1,0},{0,1,3},{0,1,-3}};
    float colors[][3] = {{1,.8f,.6f},{.6f,.8f,1.f},{1,1,.8f},{.8f,1,.9f}};
    for (int k = 0; k < 4; ++k) {
      AAPLPointLightCullingData pl;
      pl.posRadius = vec4(positions[k][0], positions[k][1], positions[k][2], 5.0f);
      pl.color = vec4(colors[k][0]*3.f, colors[k][1]*3.f, colors[k][2]*3.f, 0.f);
      _pointLights.push_back(pl);
    }
  }

  // Upload point light data to GPU
  size_t plSize = _pointLights.size() * sizeof(AAPLPointLightCullingData);
  {
    ComPtr<ID3D12Resource> uploadBuf;
    _device.GetCommandAllocator(_currentFrame)->Reset();
    cmdList->Reset(_device.GetCommandAllocator(_currentFrame), nullptr);
    _pointLightBuffer = DX12Util::CreateDefaultBuffer(dev, cmdList,
        _pointLights.data(), (UINT64)plSize, uploadBuf);
    cmdList->Close();
    ID3D12CommandList* lists[] = { cmdList };
    _device.GetCommandQueue()->ExecuteCommandLists(1, lists);
    FlushCommandQueue();
    // uploadBuf stays alive until FlushCommandQueue returns
  }

  // --- Spot lights from the scene file (mirrors Vulkan GpuScene.cpp:3399) ---
  // Builds AAPLSpotLightCullingData directly: cos(inner/outer) precomputed,
  // direction normalized, color.w = 卤1 opaque/transparent sign 鈥?the exact
  // layout Light.cpp's GpuSpot upload produces on the Vulkan side.
  if (_sceneFile.contains("spot_lights")) {
    for (auto& j : _sceneFile["spot_lights"]) {
      vec3 pos(j["position_x"].template get<float>(),
               j["position_y"].template get<float>(),
               j["position_z"].template get<float>());
      vec3 color(j["color_r"].template get<float>(),
                 j["color_g"].template get<float>(),
                 j["color_b"].template get<float>());
      float coneRad = j["coneRad"].template get<float>();
      float height  = j["height"].template get<float>();
      vec3 spotDir(j["direction_x"].template get<float>(),
                   j["direction_y"].template get<float>(),
                   j["direction_z"].template get<float>());
      bool forTransparent = j["for_transparent"].template get<bool>();

      // Apple's addSpotLight assumes a unit direction; normalize defensively.
      float dirLen = spotDir.length();
      if (dirLen > 1e-6f)
        spotDir = vec3(spotDir.x / dirLen, spotDir.y / dirLen, spotDir.z / dirLen);

      // Bounding sphere of the cone (same case split as Vulkan).
      vec4 boundingSphere;
      if (coneRad > M_PI_F / 4.0f) {
        float R = height * tanf(coneRad);
        boundingSphere = vec4(pos + spotDir * height, R);
      } else {
        float R = height / (2 * cos(coneRad) * cos(coneRad));
        boundingSphere = vec4(pos + spotDir * R, R);
      }

      AAPLSpotLightCullingData sl;
      sl.posRadius        = boundingSphere;
      sl.posAndHeight     = vec4(pos, height);
      sl.dirAndOuterAngle = vec4(spotDir, std::cos(coneRad));
      sl.color            = vec4(color, forTransparent ? -1.0f : 1.0f);
      sl.cosInnerAngle    = std::cos(coneRad * SPOT_LIGHT_INNER_SCALE);
      sl._padSpot0 = sl._padSpot1 = sl._padSpot2 = 0.0f;
      _spotLights.push_back(sl);

      // Spot shadow view-projection, in the HLSL-ready transposed form:
      // `transpose(spotP) * transpose(spotV)` evaluates (mat4 quirk) to the
      // mathematical P*V stored as standard column-major bytes, so both the
      // VS push constant and the deferred StructuredBuffer<float4x4> can use
      // it verbatim with mul(matrix, worldPos). Same expression as Vulkan.
      vec3 up = (fabsf(spotDir.y) < 0.99f) ? vec3(0.f, 1.f, 0.f) : vec3(0.f, 0.f, 1.f);
      mat4 spotV = invLookAt(pos, up, spotDir);
      mat4 spotP = perspective(2.0f * coneRad, 1.0f, 0.1f, height);
      _spotViewProjMatrices.push_back(transpose(spotP) * transpose(spotV));
    }
    spdlog::info("DX12: Loaded {} spot lights from scene file", _spotLights.size());
  }

  // Upload spot light culling data + per-light view-proj matrices.
  // Buffers always hold >= 1 element so the descriptors stay valid when the
  // scene has zero spots (mirrors Vulkan's dummy-entry behavior).
  {
    size_t spotCount = std::max<size_t>(_spotLights.size(), 1);
    std::vector<uint8_t> spotUpload(spotCount * sizeof(AAPLSpotLightCullingData), 0);
    if (!_spotLights.empty())
      memcpy(spotUpload.data(), _spotLights.data(),
             _spotLights.size() * sizeof(AAPLSpotLightCullingData));

    // t15 layout: MAX_SPOT_SHADOWS mat4s, zero-padded past the light count.
    std::vector<uint8_t> vpUpload(MAX_SPOT_SHADOWS * sizeof(mat4), 0);
    for (size_t i = 0; i < _spotViewProjMatrices.size() && i < MAX_SPOT_SHADOWS; ++i)
      memcpy(vpUpload.data() + i * sizeof(mat4),
             _spotViewProjMatrices[i].value_ptr(), sizeof(mat4));

    ComPtr<ID3D12Resource> uploadA, uploadB;
    _device.GetCommandAllocator(_currentFrame)->Reset();
    cmdList->Reset(_device.GetCommandAllocator(_currentFrame), nullptr);
    _spotLightBuffer = DX12Util::CreateDefaultBuffer(dev, cmdList,
        spotUpload.data(), (UINT64)spotUpload.size(), uploadA);
    _spotViewProjBuffer = DX12Util::CreateDefaultBuffer(dev, cmdList,
        vpUpload.data(), (UINT64)vpUpload.size(), uploadB);
    cmdList->Close();
    ID3D12CommandList* lists[] = { cmdList };
    _device.GetCommandQueue()->ExecuteCommandLists(1, lists);
    FlushCommandQueue();
  }

  // Allocate per-tile light output buffers
  uint32_t w = _device.GetWidth(), h = _device.GetHeight();
  uint32_t tileW = (w + LIGHT_TILE_SIZE - 1) / LIGHT_TILE_SIZE;
  uint32_t tileH = (h + LIGHT_TILE_SIZE - 1) / LIGHT_TILE_SIZE;
  uint32_t totalTiles = tileW * tileH;
  uint32_t maxPL = (uint32_t)_pointLights.size();
  uint32_t maxSL = (uint32_t)_spotLights.size();
  if (maxSL == 0) maxSL = 1;
  _lightXZRangeBuffer = DX12Util::CreateGPUBuffer(dev,
      (UINT64)maxPL * 8, // sizeof(uint16_t4) = 8
      D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
  _spotXZRangeBuffer = DX12Util::CreateGPUBuffer(dev,
      (UINT64)maxSL * 8,
      D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);

  // Light index buffers: [count, idx0, idx1, ...] per tile 脳 MAX_LIGHTS_PER_TILE
  UINT64 idxBufSize = (UINT64)totalTiles * MAX_LIGHTS_PER_TILE_LC * sizeof(uint32_t);
  _lightIndicesBuffer = DX12Util::CreateGPUBuffer(dev, idxBufSize,
      D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
  _lightIndicesTransparentBuffer = DX12Util::CreateGPUBuffer(dev, idxBufSize,
      D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
  _spotLightIndicesBuffer = DX12Util::CreateGPUBuffer(dev, idxBufSize,
      D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
  _spotLightIndicesTransparentBuffer = DX12Util::CreateGPUBuffer(dev, idxBufSize,
      D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);

  // Debug texture: RWTexture2D<uint> at tile resolution
  _lightDebugTexture = DX12Util::CreateTexture2D(dev, tileW, tileH,
      DXGI_FORMAT_R32_UINT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, 1, 1,
      D3D12_RESOURCE_STATE_COMMON);

  // Persistently mapped upload buffer for LightCullParams cbuffer
  _lightCullParamsBuffer = DX12Util::CreateUploadBuffer(dev,
      512, &_lightCullParamsMapped);

  spdlog::info("DX12: CreateLights 鈥?{} point lights, tileGrid {}x{}", maxPL, tileW, tileH);
}

// ---- Create Light Cull Pipelines ----
void DX12GpuScene::CreateLightCullPipelines() {
  if (!_lightCullRootSig) return;
  auto* dev = _device.GetDevice();

  auto loadCS = [&](const char* name, ComPtr<ID3D12PipelineState>& pso) {
    auto cs = DX12Util::ReadShaderFile((_rootPath / "shaders" / name).generic_string());
    if (cs.empty()) {
      spdlog::warn("DX12: Missing light cull shader: {}", name);
      return;
    }
    D3D12_COMPUTE_PIPELINE_STATE_DESC d = {};
    d.pRootSignature = _lightCullRootSig.Get();
    d.CS = {cs.data(), cs.size()};
    DX12Util::ThrowIfFailed(dev->CreateComputePipelineState(&d, IID_PPV_ARGS(&pso)), name);
    spdlog::info("DX12: Created light cull PSO: {}", name);
  };

  loadCS("CoarseCull.cs.cso",    _coarseCullPSO);
  loadCS("TraditionalCull.cs.cso", _traditionalCullPSO);
  loadCS("ClearIndices.cs.cso",  _clearIndicesPSO);
  loadCS("CoarseCullSpot.cs.cso",       _coarseCullSpotPSO);
  loadCS("TraditionalCullSpot.cs.cso",  _traditionalCullSpotPSO);
  loadCS("ClearIndicesSpot.cs.cso",     _clearIndicesSpotPSO);
}

// ---- Create Scatter Volume Resources ----
void DX12GpuScene::CreateScatterNoiseTextures() {
  // ---- Blue noise 64x64 R8: Vulkan-identical (ScatteringVolume.cpp uploadBlueNoise) ----
  // Two interleaved 8x8 Bayer matrices; value = bayer8[y&7][x&7] * 4 → [0,252].
  auto* dev = _device.GetDevice();
  {
    constexpr uint32_t SZ = 64;
    static const uint8_t bayer8[8][8] = {
        { 0,32, 8,40, 2,34,10,42},
        {48,16,56,24,50,18,58,26},
        {12,44, 4,36,14,46, 6,38},
        {60,28,52,20,62,30,54,22},
        { 3,35,11,43, 1,33, 9,41},
        {51,19,59,27,49,17,57,25},
        {15,47, 7,39,13,45, 5,37},
        {63,31,55,23,61,29,53,21}};
    std::vector<uint8_t> pixels(SZ * SZ);
    for (uint32_t y = 0; y < SZ; ++y)
      for (uint32_t x = 0; x < SZ; ++x)
        pixels[y * SZ + x] = (uint8_t)(bayer8[y & 7][x & 7] * 4);

    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC d = {};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = SZ; d.Height = SZ;
    d.DepthOrArraySize = 1; d.MipLevels = 1;
    d.Format = DXGI_FORMAT_R8_UNORM;
    d.SampleDesc.Count = 1;
    DX12Util::ThrowIfFailed(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&_scatterBlueNoiseTex)),
        "blue noise tex");

    // Upload via staging buffer
    constexpr uint32_t rowPitch = SZ; // 64 bytes → already 256-aligned? No: 64 < 256.
    const uint32_t alignedPitch = (rowPitch + 255) & ~255u;
    ComPtr<ID3D12Resource> upload;
    D3D12_HEAP_PROPERTIES up = {};
    up.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC bd = {};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = alignedPitch * SZ;
    bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    DX12Util::ThrowIfFailed(dev->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &bd,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload)),
        "blue noise upload");
    uint8_t* mapped = nullptr;
    upload->Map(0, nullptr, (void**)&mapped);
    for (uint32_t y = 0; y < SZ; ++y)
      memcpy(mapped + (size_t)y * alignedPitch, &pixels[y * SZ], SZ);
    upload->Unmap(0, nullptr);

    auto* cmdList = _device.GetCommandList();
    _device.GetCommandAllocator(0)->Reset();
    cmdList->Reset(_device.GetCommandAllocator(0), nullptr);

    D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {};
    dst.pResource = _scatterBlueNoiseTex.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.SubresourceIndex = 0;
    src.pResource = upload.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8_UNORM;
    src.PlacedFootprint.Footprint.Width = SZ;
    src.PlacedFootprint.Footprint.Height = SZ;
    src.PlacedFootprint.Footprint.Depth = 1;
    src.PlacedFootprint.Footprint.RowPitch = alignedPitch;
    cmdList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    // Compute-only reads (scatter kernel) → rest in NON_PIXEL_SHADER_RESOURCE.
    DX12Util::TransitionBarrier(cmdList, _scatterBlueNoiseTex.Get(),
        D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    cmdList->Close();
    ID3D12CommandList* lists[] = { cmdList };
    _device.GetCommandQueue()->ExecuteCommandLists(1, lists);
    FlushCommandQueue();

    // Static SRV — read exclusively by the scatter compute kernel.
    D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
    sd.Format = DXGI_FORMAT_R8_UNORM;
    sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sd.Texture2D.MipLevels = 1;
    dev->CreateShaderResourceView(_scatterBlueNoiseTex.Get(), &sd,
        _cbvSrvUavHeap.GetStaticCPU(SRV_SCATTER_BLUE_NOISE));
    spdlog::info("DX12: scatter blue noise 64x64 uploaded");
  }

  // ---- Perlin 32^3 R8 fBm: Vulkan-identical (ScatteringVolume.cpp uploadPerlinNoise) ----
  {
    constexpr uint32_t SZ = 32;
    static const int ptable[256] = {
        151,160,137,91,90,15,131,13,201,95,96,53,194,233,7,225,140,36,103,30,69,142,
        8,99,37,240,21,10,23,190,6,148,247,120,234,75,0,26,197,62,94,252,219,203,
        117,35,11,32,57,177,33,88,237,149,56,87,174,20,125,136,171,168,68,175,74,165,71,
        134,139,48,27,166,77,146,158,231,83,111,229,122,60,211,133,230,220,105,92,41,55,
        46,245,40,244,102,143,54,65,25,63,161,1,216,80,73,209,76,132,187,208,89,18,169,
        200,196,135,130,116,188,159,86,164,100,109,198,173,186,3,64,52,217,226,250,124,
        123,5,202,38,147,118,126,255,82,85,212,207,206,59,227,47,16,58,17,182,189,28,42,
        223,183,170,213,119,248,152,2,44,154,163,70,221,153,101,155,167,43,172,9,129,22,
        39,253,19,98,108,110,79,113,224,232,178,185,112,104,218,246,97,228,251,34,242,
        193,238,210,144,12,191,179,162,241,81,51,145,235,249,14,239,107,49,192,214,31,
        181,199,106,157,184,84,204,176,115,121,50,45,127,4,150,254,138,236,205,93,222,
        114,67,29,24,72,243,141,128,195,78,66,215,61,156,180};
    int perm[512];
    for (int i = 0; i < 256; ++i) perm[i] = perm[256 + i] = ptable[i];
    auto fade = [](float t) { return t * t * t * (t * (t * 6 - 15) + 10); };
    auto lerpf = [](float a, float b, float t) { return a + t * (b - a); };
    auto grad = [](int hash, float x, float y, float z) {
      int h = hash & 15;
      float u = h < 8 ? x : y;
      float v = h < 4 ? y : (h == 12 || h == 14 ? x : z);
      return ((h & 1) ? -u : u) + ((h & 2) ? -v : v);
    };
    auto perlin3 = [&](float x, float y, float z) {
      int X = (int)floorf(x) & 255, Y = (int)floorf(y) & 255, Z = (int)floorf(z) & 255;
      x -= floorf(x); y -= floorf(y); z -= floorf(z);
      float u = fade(x), v = fade(y), w = fade(z);
      int A = perm[X] + Y, AA = perm[A] + Z, AB = perm[A + 1] + Z,
          B = perm[X + 1] + Y, BA = perm[B] + Z, BB = perm[B + 1] + Z;
      return lerpf(lerpf(lerpf(grad(perm[AA], x, y, z), grad(perm[BA], x - 1, y, z), u),
                         lerpf(grad(perm[AB], x, y - 1, z), grad(perm[BB], x - 1, y - 1, z), u), v),
                   lerpf(lerpf(grad(perm[AA + 1], x, y, z - 1), grad(perm[BA + 1], x - 1, y, z - 1), u),
                         lerpf(grad(perm[AB + 1], x, y - 1, z - 1), grad(perm[BB + 1], x - 1, y - 1, z - 1), u), v), w);
    };
    auto fbm = [&](float x, float y, float z) {
      return 0.5f * perlin3(x, y, z) + 0.25f * perlin3(x * 2, y * 2, z * 2)
           + 0.125f * perlin3(x * 4, y * 4, z * 4);
    };

    std::vector<uint8_t> voxels(SZ * SZ * SZ);
    for (uint32_t z = 0; z < SZ; ++z)
      for (uint32_t y = 0; y < SZ; ++y)
        for (uint32_t x = 0; x < SZ; ++x) {
          float n = fbm(x / 8.0f, y / 8.0f, z / 8.0f);
          n = n * 0.5f + 0.5f;
          n = n < 0.0f ? 0.0f : (n > 1.0f ? 1.0f : n);
          voxels[(z * SZ + y) * SZ + x] = (uint8_t)(n * 255.0f);
        }

    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC d = {};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
    d.Width = SZ; d.Height = SZ; d.DepthOrArraySize = (UINT16)SZ;
    d.MipLevels = 1;
    d.Format = DXGI_FORMAT_R8_UNORM;
    d.SampleDesc.Count = 1;
    DX12Util::ThrowIfFailed(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&_scatterPerlinTex)),
        "perlin tex");

    // 3D upload: row pitch 32 → align to 256; slice pitch = aligned row * 32
    const uint32_t alignedRow = (SZ + 255) & ~255u;
    const uint32_t sliceBytes = alignedRow * SZ;
    ComPtr<ID3D12Resource> upload;
    D3D12_HEAP_PROPERTIES up = {};
    up.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC bd = {};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = (UINT64)sliceBytes * SZ;
    bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    DX12Util::ThrowIfFailed(dev->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &bd,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload)),
        "perlin upload");
    uint8_t* mapped = nullptr;
    upload->Map(0, nullptr, (void**)&mapped);
    for (uint32_t z = 0; z < SZ; ++z)
      for (uint32_t y = 0; y < SZ; ++y)
        memcpy(mapped + (size_t)z * sliceBytes + (size_t)y * alignedRow,
               &voxels[(z * SZ + y) * SZ], SZ);
    upload->Unmap(0, nullptr);

    auto* cmdList = _device.GetCommandList();
    _device.GetCommandAllocator(0)->Reset();
    cmdList->Reset(_device.GetCommandAllocator(0), nullptr);

    D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {};
    dst.pResource = _scatterPerlinTex.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.SubresourceIndex = 0;
    src.pResource = upload.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8_UNORM;
    src.PlacedFootprint.Footprint.Width = SZ;
    src.PlacedFootprint.Footprint.Height = SZ;
    src.PlacedFootprint.Footprint.Depth = SZ;
    src.PlacedFootprint.Footprint.RowPitch = alignedRow;
    cmdList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    // Compute-only reads (scatter kernel) → rest in NON_PIXEL_SHADER_RESOURCE.
    DX12Util::TransitionBarrier(cmdList, _scatterPerlinTex.Get(),
        D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    cmdList->Close();
    ID3D12CommandList* lists[] = { cmdList };
    _device.GetCommandQueue()->ExecuteCommandLists(1, lists);
    FlushCommandQueue();

    D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
    sd.Format = DXGI_FORMAT_R8_UNORM;
    sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sd.Texture3D.MipLevels = 1;
    dev->CreateShaderResourceView(_scatterPerlinTex.Get(), &sd,
        _cbvSrvUavHeap.GetStaticCPU(SRV_SCATTER_PERLIN));
    spdlog::info("DX12: scatter perlin 32^3 uploaded");
  }
}

void DX12GpuScene::CreateScatterResources() {
  if (!_scatterRootSig || !_accumulatePSO) {
    spdlog::warn("DX12: Scatter PSOs/RootSig not ready 鈥?skipping CreateScatterResources");
    return;
  }
  auto* dev = _device.GetDevice();

  // Volume dims from the SCREEN (Vulkan parity: ScatteringVolume::create uses
  // ceil(screen / SCATTER_TILE_SIZE=8)), not a fixed 160x90 — the old constants
  // mismatched the dispatch and left the right/bottom froxels unwritten.
  _scatterFroxelW = (_device.GetWidth() + 7) / 8;
  _scatterFroxelH = (_device.GetHeight() + 7) / 8;

  auto makeVol = [&](ComPtr<ID3D12Resource>& res, const char* name,
                     D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES initial) {
    D3D12_RESOURCE_DESC d = {};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
    d.Width = _scatterFroxelW;
    d.Height = _scatterFroxelH;
    d.DepthOrArraySize = (UINT16)SCATTER_FROXEL_D;
    d.MipLevels = 1;
    d.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    d.Flags = flags;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    HRESULT hr = dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d,
        initial, nullptr, IID_PPV_ARGS(&res));
    if (FAILED(hr)) {
      spdlog::error("DX12: Failed to create scatter volume resource: {}", name);
    } else {
      spdlog::info("DX12: Created scatter volume resource: {} ({}x{}x{})", name,
                   _scatterFroxelW, _scatterFroxelH, SCATTER_FROXEL_D);
    }
  };
  makeVol(_scatterVolume,        "scatterVolume",        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
          D3D12_RESOURCE_STATE_UNORDERED_ACCESS);       // UAV (scatterOut)
  makeVol(_scatterAccumVolume,   "scatterAccumVolume",   D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
          D3D12_RESOURCE_STATE_UNORDERED_ACCESS);       // UAV (accumOut) + SRV for deferred
  // History is copy-dst + compute-read only: no UAV flag, rests in NPSR.
  makeVol(_scatterHistoryVolume, "scatterHistoryVolume", D3D12_RESOURCE_FLAG_NONE,
          D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

  // Create static SRVs
  {
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture3D.MipLevels = 1;
    dev->CreateShaderResourceView(_scatterAccumVolume.Get(), &srvDesc,
        _cbvSrvUavHeap.GetStaticCPU(SRV_SCATTER_ACCUM));
    dev->CreateShaderResourceView(_scatterHistoryVolume.Get(), &srvDesc,
        _cbvSrvUavHeap.GetStaticCPU(SRV_SCATTER_HISTORY));
  }

  // Noise textures (blue noise + perlin), once per init — resolution-independent.
  CreateScatterNoiseTextures();

  _scatterAccumIsInPSR = false;
  _scatterHistoryValid = false;
  spdlog::info("DX12: Scatter resources created (froxel: {}x{}x{})",
               _scatterFroxelW, _scatterFroxelH, SCATTER_FROXEL_D);
}

bool DX12GpuScene::IsScatterReady() const {
  return DX12ScenePolicy::IsScatterReady({
      _scatterVolumePSO.Get() != nullptr,
      _accumulatePSO.Get() != nullptr,
      _scatterRootSig.Get() != nullptr,
      _accumRootSig.Get() != nullptr,
      _scatterVolume.Get() != nullptr,
      _scatterAccumVolume.Get() != nullptr,
  });
}

// ---- Dispatch Light Culling (CoarseCull + ClearIndices + TraditionalCull) ----
void DX12GpuScene::DispatchLightCulling(ID3D12GraphicsCommandList* cmdList) {
  if (_pointLights.empty()) return;
  if (!_coarseCullPSO || !_lightCullRootSig) return;

  auto* dev = _device.GetDevice();
  uint32_t w = _device.GetWidth(), h = _device.GetHeight();
  uint32_t tileW = (w + LIGHT_TILE_SIZE - 1) / LIGHT_TILE_SIZE;
  uint32_t tileH = (h + LIGHT_TILE_SIZE - 1) / LIGHT_TILE_SIZE;
  uint32_t maxPL = (uint32_t)_pointLights.size();
  uint32_t maxSL = (uint32_t)_spotLights.size();
  if (maxSL == 0) maxSL = 1;
  auto ds = _cbvSrvUavHeap.GetDescriptorSize();

  // --- Upload LightCullParams cbuffer ---
  // Must match lightculling.hlsl cbuffer cullParams at b1 space0 layout:
  //   uint opaqueChunkCount, alphaMaskedChunkCount, transparentChunkCount;
  //   uint totalPointLights, totalSpotLights, hizMipLevels;
  //   float2 screenSize; float4x4 viewProjMatrix; Frustum frustum;
  // We pack it as raw bytes with correct alignment.
  struct LightCullCBData {
    uint32_t opaqueChunkCount;
    uint32_t alphaMaskedChunkCount;
    uint32_t transparentChunkCount;
    uint32_t totalPointLights;
    uint32_t totalSpotLights;
    uint32_t hizMipLevels;
    float    screenWidth;
    float    screenHeight;
    float    vpMatrix[16]; // float4x4 at 16-byte aligned offset
    // Frustum: 6 planes 脳 (vec3 normal + float w) = 6 脳 16 = 96 bytes
    struct {
      struct { float x, y, z, w; } normal_w;
    } frustumPlanes[6];
  };
  static_assert(sizeof(LightCullCBData) <= 512, "LightCullCBData too large");

  LightCullCBData lcp = {};
  if (_applMesh) {
    lcp.opaqueChunkCount      = (uint32_t)_applMesh->_opaqueChunkCount;
    lcp.alphaMaskedChunkCount = (uint32_t)_applMesh->_alphaMaskedChunkCount;
    lcp.transparentChunkCount = (uint32_t)_applMesh->_transparentChunkCount;
  }
  lcp.totalPointLights = maxPL;
  lcp.totalSpotLights  = (uint32_t)_spotLights.size();
  lcp.hizMipLevels     = _hizMipLevels;
  lcp.screenWidth      = (float)w;
  lcp.screenHeight     = (float)h;
  // mat4 quirk: `A * B` computes the mathematical B*A, so view * proj is the
  // mathematical P*V 鈥?which is what mul(viewProjMatrix, pos) expects.
  // Matches GpuScene.cpp's Vulkan path (which uploads the jittered VP).
  mat4 vp = transpose(_mainCamera->getObjectToCamera() * _jitteredProj);
  memcpy(lcp.vpMatrix, vp.value_ptr(), sizeof(float) * 16);
  // Build frustum planes from camera frustum (layout-compatible with HLSL Frustum)
  const Frustum& camFrustum = _mainCamera->getFrustum();
  memcpy(lcp.frustumPlanes, &camFrustum, sizeof(lcp.frustumPlanes));
  memcpy(_lightCullParamsMapped, &lcp, sizeof(lcp));

  // --- Transition output buffers to UAV ---
  // Also transition window depth to SRV for TraditionalCull (reads inDepth)
  DX12Util::TransitionBarrier(cmdList, _device.GetDepthStencilBuffer(),
      D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  DX12Util::TransitionBarrier(cmdList, _lightXZRangeBuffer.Get(),
      D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  DX12Util::TransitionBarrier(cmdList, _lightIndicesBuffer.Get(),
      D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  DX12Util::TransitionBarrier(cmdList, _lightIndicesTransparentBuffer.Get(),
      D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  DX12Util::TransitionBarrier(cmdList, _lightDebugTexture.Get(),
      D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  DX12Util::TransitionBarrier(cmdList, _spotXZRangeBuffer.Get(),
      D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  DX12Util::TransitionBarrier(cmdList, _spotLightIndicesBuffer.Get(),
      D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  DX12Util::TransitionBarrier(cmdList, _spotLightIndicesTransparentBuffer.Get(),
      D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

  // --- Build descriptor table: 8 SRVs (t1-t8) + 9 UAVs (u3-u11) = 17 slots ---
  // Slot layout (matches lcRanges[] in root sig):
  //   [0-7]  SRVs for t1..t8 space1:
  //          slot0=t1 pointLightCullingData
  //          slot1=t2 inDepth (window depth)
  //          slot2-6 = null (t3-t7)
  //          slot7=t8 spotLightCullingData
  //   [8-16] UAVs for u3..u11 space1:
  //          slot8=u3 lightXZRange
  //          slot9=u4 lightDebug
  //          slot10=u5 lightIndices
  //          slot11=u6 null (tradtionDebug Texture2D 鈥?skip)
  //          slot12=u7 lightIndicesTransparent
  //          slot13=u8 null
  //          slot14=u9 spotXZRange
  //          slot15=u10 spotLightIndices
  //          slot16=u11 spotLightIndicesTransparent
  auto lcDesc = _cbvSrvUavHeap.AllocateDynamic(17);

  // SRV t1: pointLightCullingData
  {
    D3D12_SHADER_RESOURCE_VIEW_DESC d = {};
    d.Format = DXGI_FORMAT_UNKNOWN;
    d.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    d.Buffer.NumElements = maxPL;
    d.Buffer.StructureByteStride = sizeof(AAPLPointLightCullingData);
    dev->CreateShaderResourceView(_pointLightBuffer.Get(), &d, {lcDesc.cpu.ptr + 0 * ds});
  }
  // SRV t2: inDepth (window depth buffer as SRV)
  {
    D3D12_SHADER_RESOURCE_VIEW_DESC d = {};
    d.Format = DXGI_FORMAT_R32_FLOAT;
    d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    d.Texture2D.MipLevels = 1;
    dev->CreateShaderResourceView(_device.GetDepthStencilBuffer(), &d, {lcDesc.cpu.ptr + 1 * ds});
  }
  // SRV t3-t7: null pads
  for (int k = 2; k <= 6; ++k) {
    D3D12_SHADER_RESOURCE_VIEW_DESC nd = {};
    nd.Format = DXGI_FORMAT_R32_FLOAT;
    nd.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    nd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    dev->CreateShaderResourceView(nullptr, &nd, {lcDesc.cpu.ptr + (SIZE_T)k * ds});
  }
  // SRV t8: spotLightCullingData (null if no spot lights)
  {
    D3D12_SHADER_RESOURCE_VIEW_DESC d = {};
    d.Format = DXGI_FORMAT_R32_FLOAT;
    d.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    if (_spotLightBuffer && !_spotLights.empty()) {
      d.Format = DXGI_FORMAT_UNKNOWN;
      d.Buffer.NumElements = (UINT)_spotLights.size();
      d.Buffer.StructureByteStride = sizeof(AAPLSpotLightCullingData);
      dev->CreateShaderResourceView(_spotLightBuffer.Get(), &d, {lcDesc.cpu.ptr + 7 * ds});
    } else {
      dev->CreateShaderResourceView(nullptr, &d, {lcDesc.cpu.ptr + 7 * ds});
    }
  }

  // UAV u3: lightXZRange (RWStructuredBuffer<uint16_t4>)
  {
    D3D12_UNORDERED_ACCESS_VIEW_DESC d = {};
    d.Format = DXGI_FORMAT_UNKNOWN;
    d.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    d.Buffer.NumElements = maxPL;
    d.Buffer.StructureByteStride = 8; // sizeof(uint16_t4)
    dev->CreateUnorderedAccessView(_lightXZRangeBuffer.Get(), nullptr, &d, {lcDesc.cpu.ptr + 8 * ds});
  }
  // UAV u4: lightDebug (RWTexture2D<uint>)
  {
    D3D12_UNORDERED_ACCESS_VIEW_DESC d = {};
    d.Format = DXGI_FORMAT_R32_UINT;
    d.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    dev->CreateUnorderedAccessView(_lightDebugTexture.Get(), nullptr, &d, {lcDesc.cpu.ptr + 9 * ds});
  }
  // UAV u5: lightIndices (RWStructuredBuffer<uint>)
  {
    D3D12_UNORDERED_ACCESS_VIEW_DESC d = {};
    d.Format = DXGI_FORMAT_UNKNOWN;
    d.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    d.Buffer.NumElements = (UINT)(tileW * tileH * MAX_LIGHTS_PER_TILE_LC);
    d.Buffer.StructureByteStride = sizeof(uint32_t);
    dev->CreateUnorderedAccessView(_lightIndicesBuffer.Get(), nullptr, &d, {lcDesc.cpu.ptr + 10 * ds});
  }
  // UAV u6: tradtionDebug 鈥?null Texture2D (RWTexture2D<float4> in shader, unused/commented)
  {
    D3D12_UNORDERED_ACCESS_VIEW_DESC nd = {};
    nd.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    nd.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    dev->CreateUnorderedAccessView(nullptr, nullptr, &nd, {lcDesc.cpu.ptr + 11 * ds});
  }
  // UAV u7: lightIndicesTransparent
  {
    D3D12_UNORDERED_ACCESS_VIEW_DESC d = {};
    d.Format = DXGI_FORMAT_UNKNOWN;
    d.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    d.Buffer.NumElements = (UINT)(tileW * tileH * MAX_LIGHTS_PER_TILE_LC);
    d.Buffer.StructureByteStride = sizeof(uint32_t);
    dev->CreateUnorderedAccessView(_lightIndicesTransparentBuffer.Get(), nullptr, &d, {lcDesc.cpu.ptr + 12 * ds});
  }
  // UAV u8: null (padding slot between u7 and u9)
  {
    D3D12_UNORDERED_ACCESS_VIEW_DESC nd = {};
    nd.Format = DXGI_FORMAT_R32_UINT;
    nd.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    dev->CreateUnorderedAccessView(nullptr, nullptr, &nd, {lcDesc.cpu.ptr + 13 * ds});
  }
  // UAV u9: spotXZRange
  {
    D3D12_UNORDERED_ACCESS_VIEW_DESC d = {};
    d.Format = DXGI_FORMAT_UNKNOWN;
    d.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    d.Buffer.NumElements = maxSL;
    d.Buffer.StructureByteStride = 8;
    dev->CreateUnorderedAccessView(_spotXZRangeBuffer.Get(), nullptr, &d, {lcDesc.cpu.ptr + 14 * ds});
  }
  // UAV u10: spotLightIndices
  {
    D3D12_UNORDERED_ACCESS_VIEW_DESC d = {};
    d.Format = DXGI_FORMAT_UNKNOWN;
    d.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    d.Buffer.NumElements = (UINT)(tileW * tileH * MAX_LIGHTS_PER_TILE_LC);
    d.Buffer.StructureByteStride = sizeof(uint32_t);
    dev->CreateUnorderedAccessView(_spotLightIndicesBuffer.Get(), nullptr, &d, {lcDesc.cpu.ptr + 15 * ds});
  }
  // UAV u11: spotLightIndicesTransparent
  {
    D3D12_UNORDERED_ACCESS_VIEW_DESC d = {};
    d.Format = DXGI_FORMAT_UNKNOWN;
    d.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    d.Buffer.NumElements = (UINT)(tileW * tileH * MAX_LIGHTS_PER_TILE_LC);
    d.Buffer.StructureByteStride = sizeof(uint32_t);
    dev->CreateUnorderedAccessView(_spotLightIndicesTransparentBuffer.Get(), nullptr, &d, {lcDesc.cpu.ptr + 16 * ds});
  }

  // --- Set shared state ---
  cmdList->SetComputeRootSignature(_lightCullRootSig.Get());
  cmdList->SetComputeRootConstantBufferView(0,
      _frameResources[_currentFrame].uniformBuffer->GetGPUVirtualAddress());
  cmdList->SetComputeRootConstantBufferView(1,
      _lightCullParamsBuffer->GetGPUVirtualAddress());
  cmdList->SetComputeRootDescriptorTable(2, lcDesc.gpu);

  // --- ClearIndices: zero lightIndices counts ---
  if (_clearIndicesPSO) {
    cmdList->SetPipelineState(_clearIndicesPSO.Get());
    cmdList->Dispatch(tileW, tileH, 1);
    DX12Util::UAVBarrier(cmdList, _lightIndicesBuffer.Get());
  }

  // --- CoarseCull: one thread per light ---
  {
    cmdList->SetPipelineState(_coarseCullPSO.Get());
    cmdList->Dispatch((maxPL + 127) / 128, 1, 1);
    DX12Util::UAVBarrier(cmdList, _lightXZRangeBuffer.Get());
    DX12Util::UAVBarrier(cmdList, _lightDebugTexture.Get());
  }

  // --- TraditionalCull: tileW 脳 tileH thread groups (16脳16 each) ---
  if (_traditionalCullPSO) {
    cmdList->SetPipelineState(_traditionalCullPSO.Get());
    cmdList->Dispatch(tileW, tileH, 1);
    DX12Util::UAVBarrier(cmdList, _lightIndicesBuffer.Get());
  }

  // --- Spot light culling chain (clear -> coarse -> traditional) ---
  // Same kernel sequence as the Vulkan LightCuller (Light.cpp:1731-1775);
  // the spot kernels share this root signature / descriptor table.
  if (!_spotLights.empty() && _clearIndicesSpotPSO && _coarseCullSpotPSO &&
      _traditionalCullSpotPSO) {
    uint32_t spotCount = (uint32_t)_spotLights.size();

    cmdList->SetPipelineState(_clearIndicesSpotPSO.Get());
    cmdList->Dispatch(tileW, tileH, 1);
    DX12Util::UAVBarrier(cmdList, _spotLightIndicesBuffer.Get());
    DX12Util::UAVBarrier(cmdList, _spotLightIndicesTransparentBuffer.Get());

    cmdList->SetPipelineState(_coarseCullSpotPSO.Get());
    cmdList->Dispatch((spotCount + 127) / 128, 1, 1);
    DX12Util::UAVBarrier(cmdList, _spotXZRangeBuffer.Get());

    cmdList->SetPipelineState(_traditionalCullSpotPSO.Get());
    cmdList->Dispatch(tileW, tileH, 1);
    DX12Util::UAVBarrier(cmdList, _spotLightIndicesBuffer.Get());
    DX12Util::UAVBarrier(cmdList, _spotLightIndicesTransparentBuffer.Get());
  }

  // --- Transition lightIndices to SRV for deferred lighting ---
  DX12Util::TransitionBarrier(cmdList, _lightIndicesBuffer.Get(),
      D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_GENERIC_READ);
  // Spot indices are also read by deferred lighting (t12) when spots exist.
  if (!_spotLights.empty()) {
    DX12Util::TransitionBarrier(cmdList, _spotLightIndicesBuffer.Get(),
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_GENERIC_READ);
  }
  DX12Util::TransitionBarrier(cmdList, _lightXZRangeBuffer.Get(),
      D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
  DX12Util::TransitionBarrier(cmdList, _lightIndicesTransparentBuffer.Get(),
      D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
  DX12Util::TransitionBarrier(cmdList, _lightDebugTexture.Get(),
      D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
  DX12Util::TransitionBarrier(cmdList, _spotXZRangeBuffer.Get(),
      D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
  // _spotLightIndicesBuffer: already GENERIC_READ when spots exist (deferred
  // lighting reads it at t12); restore COMMON only when the spot chain never ran.
  if (_spotLights.empty()) {
    DX12Util::TransitionBarrier(cmdList, _spotLightIndicesBuffer.Get(),
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
  }
  DX12Util::TransitionBarrier(cmdList, _spotLightIndicesTransparentBuffer.Get(),
      D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
  // Restore depth to DEPTH_WRITE for deferred lighting depth-read transition
  DX12Util::TransitionBarrier(cmdList, _device.GetDepthStencilBuffer(),
      D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE);
}

// ---- Readback Culling Stats ----
void DX12GpuScene::ReadbackCullingStats() {
  if (!_applMesh) return;

  _cullingStats.totalOpaque = (uint32_t)_applMesh->_opaqueChunkCount;
  _cullingStats.totalAlphaMask = (uint32_t)_applMesh->_alphaMaskedChunkCount;
  _cullingStats.totalTransparent = (uint32_t)_applMesh->_transparentChunkCount;

  // Read from previous frame's readback buffer
  uint32_t prevFrame = (_currentFrame + _device.GetFrameCount() - 1) % _device.GetFrameCount();
  auto& prevRes = _frameResources[prevFrame];
  if (prevRes.writeIndexReadback) {
    void* mapped = nullptr;
    D3D12_RANGE readRange = {0, 3 * sizeof(uint32_t)};
    if (SUCCEEDED(prevRes.writeIndexReadback->Map(0, &readRange, &mapped))) {
      uint32_t* counts = (uint32_t*)mapped;
      _cullingStats.visibleOpaque = counts[0];
      _cullingStats.visibleAlphaMask = counts[1];
      _cullingStats.visibleTransparent = counts[2];
      D3D12_RANGE writeRange = {0, 0};
      prevRes.writeIndexReadback->Unmap(0, &writeRange);
    }
  }
}

// Helper: build the 6 world-space cull planes of a cascade's sun-view box.
//
// Deliberately corner-based rather than Gribb-Hartmann plane extraction from
// the VP matrix: this codebase stores matrices in two different conventions
// (see docs/dx12-matrix-and-binding-audit.md), so "which vec4 is a row" is a
// trap. Corners are unambiguous. Mirrors Shadow.cpp's Vulkan implementation.
//
// Plane convention must match commonstruct.hlsl's IsInside(): a point is
// inside when dot(p, normal) - w > 0, i.e. w = p_on_plane . normal.
static ShadowFrustum buildCascadeFrustum(const vec3& eye,
                                         const vec3& axisX, const vec3& axisY,
                                         const vec3& axisZ,
                                         float halfExtent, float farZ) {
  // Box corners: bit0 = +/-halfExtent along X, bit1 = along Y, bit2 = z 0 or farZ
  vec3 c[8];
  for (int k = 0; k < 8; ++k) {
    float vx = (k & 1) ? halfExtent : -halfExtent;
    float vy = (k & 2) ? halfExtent : -halfExtent;
    float vz = (k & 4) ? farZ : 0.0f;
    c[k] = eye + axisX * vx + axisY * vy + axisZ * vz;
  }

  // Winding of each triple is chosen so the normal points INTO the box.
  auto makePlane = [](const vec3& p1, const vec3& p2, const vec3& p3) -> ShadowPlane {
    ShadowPlane pl;
    pl.normal = normalize((p2 - p1).cross(p3 - p1));
    pl.w = p1.dot(pl.normal);
    return pl;
  };

  ShadowFrustum f;
  f.borders[0] = makePlane(c[0], c[2], c[4]); // -X
  f.borders[1] = makePlane(c[1], c[5], c[3]); // +X
  f.borders[2] = makePlane(c[0], c[4], c[1]); // -Y
  f.borders[3] = makePlane(c[2], c[3], c[6]); // +Y
  f.borders[4] = makePlane(c[0], c[1], c[2]); // -Z (sun-near)
  f.borders[5] = makePlane(c[4], c[6], c[5]); // +Z (sun-far)
  return f;
}

// ---- Draw (main frame) ----
void DX12GpuScene::Draw() {
  BuildImGuiFrame();
  if (_sdlWindow && (SDL_GetWindowFlags(_sdlWindow) & SDL_WINDOW_MINIMIZED)) {
    if (_imguiInitialized) EditorUi::RenderPlatformWindows();
    SDL_Delay(10);
    return;
  }
  _currentFrame = _device.GetFrameIndex();

  UpdateUniforms();
  ReadbackCullingStats();
  UpdateTextureStreaming();  // streaming tick: coverage -> dispatch -> process swaps (before command recording)

  _device.BeginFrame();
  _cbvSrvUavHeap.ResetFrame();

  auto* cmdList = _device.GetCommandList();
  auto rtvHandle = _device.GetRTV(_device.GetFrameIndex());
  auto dsvHandle = _device.GetDSV();

  // Set descriptor heaps
  ID3D12DescriptorHeap* heaps[] = { _cbvSrvUavHeap.GetHeap(), _samplerHeap.GetHeap() };
  cmdList->SetDescriptorHeaps(2, heaps);

  // Set viewport and scissor.
  //
  // Negative height so D3D12's viewport transform reproduces Vulkan's:
  //   D3D12:  y_w = (1 - ndc.y) * Height/2 + TopLeftY
  //   with TopLeftY = H, Height = -H  =>  y_w = (ndc.y + 1) * H/2
  // which is exactly what Vulkan does with a positive-height viewport.
  //
  // Without this, D3D12's Y-up NDC renders everything vertically mirrored
  // relative to Vulkan, which (a) reverses screen-space winding and (b)
  // invalidates every `uv = ndc.xy * 0.5 + 0.5` in the shaders 鈥?all of which
  // were written against the Vulkan backend. Flipping here keeps the whole
  // shader set valid verbatim; see docs/dx12-matrix-and-binding-audit.md.
  D3D12_VIEWPORT viewport = {};
  viewport.TopLeftX = 0.0f;
  viewport.TopLeftY = (float)_device.GetHeight();
  viewport.Width = (float)_device.GetWidth();
  viewport.Height = -(float)_device.GetHeight();
  viewport.MinDepth = 0.0f;
  viewport.MaxDepth = 1.0f;
  cmdList->RSSetViewports(1, &viewport);

  D3D12_RECT scissor = {};
  scissor.right = _device.GetWidth();
  scissor.bottom = _device.GetHeight();
  cmdList->RSSetScissorRects(1, &scissor);

  // Clear render target and depth
  float clearColor[] = {0.0f, 0.0f, 0.0f, 1.0f};
  cmdList->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);
  cmdList->ClearDepthStencilView(dsvHandle, D3D12_CLEAR_FLAG_DEPTH, 0.0f, 0, 0, nullptr);

  // === Shadow Cull + Draw (per cascade, GPU-culled) ===
  if (_shadowCullPSO && _shadowPSO && _shadowMapArray) {
    auto& fr = _frameResources[_currentFrame];
    uint32_t totalShadowChunks = (uint32_t)(_applMesh->_opaqueChunkCount +
                                              _applMesh->_alphaMaskedChunkCount);
    auto* dev = _device.GetDevice();
    auto ds = _cbvSrvUavHeap.GetDescriptorSize();

    // --- Zero shadow write indices ---
    memset(fr.shadowWriteIndexUploadMapped, 0, SHADOW_CASCADE_COUNT * 2 * sizeof(uint32_t));
    DX12Util::TransitionBarrier(cmdList, fr.shadowWriteIndex.Get(),
        D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
    cmdList->CopyBufferRegion(fr.shadowWriteIndex.Get(), 0,
        fr.shadowWriteIndexUpload.Get(), 0, SHADOW_CASCADE_COUNT * 2 * sizeof(uint32_t));

    // --- Transition resources to UAV ---
    DX12Util::TransitionBarrier(cmdList, fr.shadowDrawParams.Get(),
        D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    DX12Util::TransitionBarrier(cmdList, fr.shadowWriteIndex.Get(),
        D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    DX12Util::TransitionBarrier(cmdList, fr.shadowChunkIndicesBuffer.Get(),
        D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    // --- Upload ShadowCullParams ---
    ShadowCullParams scp = {};
    scp.opaqueChunkCount = (uint32_t)_applMesh->_opaqueChunkCount;
    scp.alphaMaskedChunkCount = (uint32_t)_applMesh->_alphaMaskedChunkCount;
    scp.cascadeMaxChunks = totalShadowChunks;
    scp.cascadeCount = SHADOW_CASCADE_COUNT;
    for (uint32_t c = 0; c < SHADOW_CASCADE_COUNT; ++c) {
      scp.cascadeCullThreshold[c] = vec4(0.0f, 0.0f, 0.0f, 0.0f);
      const auto& box = _shadowCascadeBoxes[c];
      scp.cascadeFrustum[c] = buildCascadeFrustum(box.eye, box.axisX, box.axisY,
                                                  box.axisZ, box.halfExtent,
                                                  box.farZ);
    }
    memcpy(fr.shadowCullParamsMapped, &scp, sizeof(ShadowCullParams));

    // --- Allocate 8 descriptors: u0..u4 (5 UAVs) + t2..t4 (3 SRVs) ---
    auto cullDesc = _cbvSrvUavHeap.AllocateDynamic(8);
    // u0: shadowDrawParams
    {
      D3D12_UNORDERED_ACCESS_VIEW_DESC d = {};
      d.Format = DXGI_FORMAT_UNKNOWN;
      d.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
      d.Buffer.NumElements = totalShadowChunks * SHADOW_CASCADE_COUNT * 2;
      d.Buffer.StructureByteStride = sizeof(DX12DrawIndexedArgs);
      dev->CreateUnorderedAccessView(fr.shadowDrawParams.Get(), nullptr, &d,
          {cullDesc.cpu.ptr + 0 * ds});
    }
    // u1, u2: null UAV pads (format-only, no resource)
    for (int k = 1; k <= 2; ++k) {
      D3D12_UNORDERED_ACCESS_VIEW_DESC nd = {};
      nd.Format = DXGI_FORMAT_R32_UINT;
      nd.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
      dev->CreateUnorderedAccessView(nullptr, nullptr, &nd,
          {cullDesc.cpu.ptr + (SIZE_T)k * ds});
    }
    // u3: shadowWriteIndex
    {
      D3D12_UNORDERED_ACCESS_VIEW_DESC d = {};
      d.Format = DXGI_FORMAT_R32_UINT;
      d.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
      d.Buffer.NumElements = SHADOW_CASCADE_COUNT * 2;
      dev->CreateUnorderedAccessView(fr.shadowWriteIndex.Get(), nullptr, &d,
          {cullDesc.cpu.ptr + 3 * ds});
    }
    // u4: shadowChunkIndices
    {
      D3D12_UNORDERED_ACCESS_VIEW_DESC d = {};
      d.Format = DXGI_FORMAT_R32_UINT;
      d.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
      d.Buffer.NumElements = totalShadowChunks * SHADOW_CASCADE_COUNT * 2;
      dev->CreateUnorderedAccessView(fr.shadowChunkIndicesBuffer.Get(), nullptr, &d,
          {cullDesc.cpu.ptr + 4 * ds});
    }
    // t2: meshChunks SRV
    {
      D3D12_SHADER_RESOURCE_VIEW_DESC d = {};
      d.Format = DXGI_FORMAT_UNKNOWN;
      d.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
      d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
      d.Buffer.NumElements = (UINT)_applMesh->_chunkCount;
      d.Buffer.StructureByteStride = MESH_CHUNK_STRIDE; // sizeof(AAPLMeshChunk)
      dev->CreateShaderResourceView(_meshChunksBuffer.Get(), &d,
          {cullDesc.cpu.ptr + 5 * ds});
    }
    // t3, t4: null SRV pads
    for (int k = 6; k <= 7; ++k) {
      D3D12_SHADER_RESOURCE_VIEW_DESC nd = {};
      nd.Format = DXGI_FORMAT_R32_FLOAT;
      nd.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
      nd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
      dev->CreateShaderResourceView(nullptr, &nd, {cullDesc.cpu.ptr + (SIZE_T)k * ds});
    }

    // --- Dispatch shadow cull compute (all cascades in one pass) ---
    cmdList->SetPipelineState(_shadowCullPSO.Get());
    cmdList->SetComputeRootSignature(_shadowCullRootSig.Get());
    cmdList->SetComputeRootConstantBufferView(0, fr.shadowCullParams->GetGPUVirtualAddress());
    cmdList->SetComputeRootDescriptorTable(1, cullDesc.gpu);
    cmdList->Dispatch((totalShadowChunks + 127) / 128, 1, 1);

    // --- UAV barriers then transition to indirect arg ---
    DX12Util::UAVBarrier(cmdList, fr.shadowDrawParams.Get());
    DX12Util::UAVBarrier(cmdList, fr.shadowWriteIndex.Get());
    DX12Util::TransitionBarrier(cmdList, fr.shadowDrawParams.Get(),
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
    DX12Util::TransitionBarrier(cmdList, fr.shadowWriteIndex.Get(),
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
    // The VS reads chunkIndex as an SRV (t4, space1), indexed by the drawSlot
    // root constant that the command signature patches per draw.
    DX12Util::TransitionBarrier(cmdList, fr.shadowChunkIndicesBuffer.Get(),
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    // --- Per-cascade shadow draw via ExecuteIndirect ---
    uint32_t dsvSize = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    // Negative height, same Vulkan-parity reason as the main viewport above.
    D3D12_VIEWPORT shadowViewport = {0, (float)SHADOW_MAP_SIZE,
                                     (float)SHADOW_MAP_SIZE, -(float)SHADOW_MAP_SIZE,
                                     0.0f, 1.0f};
    D3D12_RECT shadowScissor = {0, 0, (LONG)SHADOW_MAP_SIZE, (LONG)SHADOW_MAP_SIZE};

    D3D12_VERTEX_BUFFER_VIEW vbvs[4] = {};
    ID3D12Resource* vbs[] = {_vertexBuffer.Get(), _normalBuffer.Get(), _tangentBuffer.Get(), _uvBuffer.Get()};
    uint32_t strides[] = {sizeof(float)*3, sizeof(float)*3, sizeof(float)*3, sizeof(float)*2};
    for (int k = 0; k < 4; ++k) {
      vbvs[k].BufferLocation = vbs[k]->GetGPUVirtualAddress();
      vbvs[k].SizeInBytes = (UINT)vbs[k]->GetDesc().Width;
      vbvs[k].StrideInBytes = strides[k];
    }
    D3D12_INDEX_BUFFER_VIEW ibv = {};
    ibv.BufferLocation = _indexBuffer->GetGPUVirtualAddress();
    ibv.SizeInBytes = (UINT)_indexBuffer->GetDesc().Width;
    ibv.Format = DXGI_FORMAT_R32_UINT;

    for (uint32_t cascade = 0; cascade < SHADOW_CASCADE_COUNT; ++cascade) {
      D3D12_CPU_DESCRIPTOR_HANDLE cascadeDsv = _shadowDsvHeap->GetCPUDescriptorHandleForHeapStart();
      cascadeDsv.ptr += (SIZE_T)cascade * dsvSize;
      cmdList->ClearDepthStencilView(cascadeDsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
      cmdList->OMSetRenderTargets(0, nullptr, FALSE, &cascadeDsv);
      cmdList->RSSetViewports(1, &shadowViewport);
      cmdList->RSSetScissorRects(1, &shadowScissor);

      cmdList->SetPipelineState(_shadowPSO.Get());
      cmdList->SetGraphicsRootSignature(_shadowRootSig.Get());
      cmdList->SetGraphicsRootConstantBufferView(0,
          fr.uniformBuffer->GetGPUVirtualAddress());
      // PushConstants {materialIndex, shadowIndex, drawSlot}. Only the first
      // two are set here; drawSlot (offset 2) is patched per draw by the
      // command signature. ExecuteIndirect leaves root arguments it touches
      // undefined, so this is re-issued before each ExecuteIndirect below.
      const uint32_t push[2] = {0, cascade};

      // chunkIndex SRV (t4,space1): the VS indexes it with pushConstants.drawSlot
      {
        auto ciDesc = _cbvSrvUavHeap.AllocateDynamic(1);
        D3D12_SHADER_RESOURCE_VIEW_DESC ciSrv = {};
        ciSrv.Format = DXGI_FORMAT_R32_UINT;
        ciSrv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        ciSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        ciSrv.Buffer.NumElements = totalShadowChunks * SHADOW_CASCADE_COUNT * 2;
        _device.GetDevice()->CreateShaderResourceView(fr.shadowChunkIndicesBuffer.Get(), &ciSrv, ciDesc.cpu);
        cmdList->SetGraphicsRootDescriptorTable(2, ciDesc.gpu);
      }

      // Alpha-mask PS tables (params [3]/[4]): materials t0 + meshChunks t3
      // (space1) and bindless textures (t0+ space2). Bound every cascade for
      // simplicity; the opaque VS-only PSO ignores them.
      {
        auto tbl = _cbvSrvUavHeap.AllocateDynamic(2);
        UINT ds = _device.GetDevice()->GetDescriptorHandleIncrementSize(
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        D3D12_SHADER_RESOURCE_VIEW_DESC dm = {};
        dm.Format = DXGI_FORMAT_UNKNOWN;
        dm.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        dm.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        dm.Buffer.NumElements = (UINT)_applMesh->_materialCount;
        dm.Buffer.StructureByteStride = sizeof(AAPLShaderMaterial);
        _device.GetDevice()->CreateShaderResourceView(_materialBuffer.Get(), &dm, tbl.cpu);

        D3D12_SHADER_RESOURCE_VIEW_DESC dc = {};
        dc.Format = DXGI_FORMAT_UNKNOWN;
        dc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        dc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        dc.Buffer.NumElements = (UINT)_applMesh->_chunkCount;
        dc.Buffer.StructureByteStride = MESH_CHUNK_STRIDE;
        _device.GetDevice()->CreateShaderResourceView(_meshChunksBuffer.Get(), &dc, {tbl.cpu.ptr + ds});
        cmdList->SetGraphicsRootDescriptorTable(3, tbl.gpu);
        cmdList->SetGraphicsRootDescriptorTable(4,
            _cbvSrvUavHeap.GetStaticGPU(SRV_BINDLESS_START));
      }

      cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
      cmdList->IASetVertexBuffers(0, 4, vbvs);
      cmdList->IASetIndexBuffer(&ibv);

      // GPU shadow cull shader layout (shadowcull.hlsl):
      //   cascadeBase = c * cascadeMaxChunks * 2  (opaque) or + cascadeMaxChunks (alpha)
      uint32_t shCascadeOpaqueBase = cascade * totalShadowChunks * 2;
      uint32_t shCascadeAlphaBase  = shCascadeOpaqueBase + totalShadowChunks;
      uint32_t writeIdxOffset = cascade * 2 * sizeof(uint32_t);

      // Opaque draws
      cmdList->SetPipelineState(_shadowPSO.Get());
      cmdList->SetGraphicsRoot32BitConstants(1, 2, push, 0);
      cmdList->ExecuteIndirect(_shadowDrawCmdSig.Get(),
          (uint32_t)_applMesh->_opaqueChunkCount,
          fr.shadowDrawParams.Get(),
          (UINT64)shCascadeOpaqueBase * sizeof(DX12DrawIndexedArgs),
          fr.shadowWriteIndex.Get(), writeIdxOffset);

      // Alpha-masked draws (D1: alpha-test PS clips cutout casters)
      if (_shadowAlphaMaskPSO) {
        cmdList->SetPipelineState(_shadowAlphaMaskPSO.Get());
      }
      cmdList->SetGraphicsRoot32BitConstants(1, 2, push, 0);
      cmdList->ExecuteIndirect(_shadowDrawCmdSig.Get(),
          (uint32_t)_applMesh->_alphaMaskedChunkCount,
          fr.shadowDrawParams.Get(),
          (UINT64)shCascadeAlphaBase * sizeof(DX12DrawIndexedArgs),
          fr.shadowWriteIndex.Get(), writeIdxOffset + sizeof(uint32_t));
    }

    // --- Restore resource states ---
    DX12Util::TransitionBarrier(cmdList, fr.shadowDrawParams.Get(),
        D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT, D3D12_RESOURCE_STATE_COMMON);
    DX12Util::TransitionBarrier(cmdList, fr.shadowWriteIndex.Get(),
        D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT, D3D12_RESOURCE_STATE_COMMON);
    DX12Util::TransitionBarrier(cmdList, fr.shadowChunkIndicesBuffer.Get(),
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);

    // Transition shadow maps to SRV for deferred lighting
    DX12Util::TransitionBarrier(cmdList, _shadowMapArray.Get(),
        D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    cmdList->RSSetViewports(1, &viewport);
    cmdList->RSSetScissorRects(1, &scissor);
  }

  // === Spot Shadow Maps (static scene: rendered once, then reused) ===
  RenderSpotShadowMaps(cmdList);
  // Restore the main viewport/scissor (the spot pass shrank it to 256x256).
  cmdList->RSSetViewports(1, &viewport);
  cmdList->RSSetScissorRects(1, &scissor);

  // Set render target for subsequent passes
  cmdList->OMSetRenderTargets(1, &rtvHandle, FALSE, &dsvHandle);

  // === Occluder Depth Pass (into separate occluder depth texture) ===
  if (_occluderPSO && _occluderVertexBuffer) {
    auto occDsv = _gbufferDsvHeap->GetCPUDescriptorHandleForHeapStart();
    cmdList->ClearDepthStencilView(occDsv, D3D12_CLEAR_FLAG_DEPTH, 0.0f, 0, 0, nullptr);
    cmdList->OMSetRenderTargets(0, nullptr, FALSE, &occDsv);

    cmdList->SetPipelineState(_occluderPSO.Get());
    cmdList->SetGraphicsRootSignature(_occluderRootSig.Get());
    cmdList->SetGraphicsRootConstantBufferView(0,
        _frameResources[_currentFrame].uniformBuffer->GetGPUVirtualAddress());
    cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    D3D12_VERTEX_BUFFER_VIEW occVbv = {};
    occVbv.BufferLocation = _occluderVertexBuffer->GetGPUVirtualAddress();
    occVbv.SizeInBytes = (UINT)_occluderVertexBuffer->GetDesc().Width;
    occVbv.StrideInBytes = sizeof(float) * 3;
    cmdList->IASetVertexBuffers(0, 1, &occVbv);

    D3D12_INDEX_BUFFER_VIEW occIbv = {};
    occIbv.BufferLocation = _occluderIndexBuffer->GetGPUVirtualAddress();
    occIbv.SizeInBytes = (UINT)_occluderIndexBuffer->GetDesc().Width;
    occIbv.Format = DXGI_FORMAT_R32_UINT;
    cmdList->IASetIndexBuffer(&occIbv);

    cmdList->DrawIndexedInstanced(_occluderIndexCount, 1, 0, 0, 0);
  }

  // === HiZ Pyramid Generation (from occluder depth) ===
  if (_hizCopyPSO && _hizTexture && _depthTexture) {
    // Transition occluder depth to SRV
    DX12Util::TransitionBarrier(cmdList, _depthTexture.Get(),
        D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    // Transition HiZ to UAV
    DX12Util::TransitionBarrier(cmdList, _hizTexture.Get(),
        D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    uint32_t w = _device.GetWidth(), h = _device.GetHeight();

    // Copy: occluder depth 鈫?HiZ mip 0
    {
      cmdList->SetPipelineState(_hizCopyPSO.Get());
      cmdList->SetComputeRootSignature(_hizRootSig.Get());

      // Allocate dynamic descriptors for SRV + UAV
      auto desc = _cbvSrvUavHeap.AllocateDynamic(2);
      // SRV for occluder depth
      D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
      srvDesc.Format = DXGI_FORMAT_R32_FLOAT;
      srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
      srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
      srvDesc.Texture2D.MipLevels = 1;
      _device.GetDevice()->CreateShaderResourceView(_depthTexture.Get(), &srvDesc,
          {desc.cpu.ptr});
      // UAV for HiZ mip 0
      D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
      uavDesc.Format = DXGI_FORMAT_R32_FLOAT;
      uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
      uavDesc.Texture2D.MipSlice = 0;
      _device.GetDevice()->CreateUnorderedAccessView(_hizTexture.Get(), nullptr, &uavDesc,
          {desc.cpu.ptr + _cbvSrvUavHeap.GetDescriptorSize()});

      cmdList->SetComputeRootDescriptorTable(0, desc.gpu);
      uint32_t pushData[4] = {w, h, w, h};
      cmdList->SetComputeRoot32BitConstants(1, 4, pushData, 0);
      cmdList->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
    }

    // Downsample mip chain
    for (uint32_t mip = 1; mip < _hizMipLevels; ++mip) {
      DX12Util::UAVBarrier(cmdList, _hizTexture.Get());

      uint32_t mipW = (w >> mip) > 1 ? (w >> mip) : 1;
      uint32_t mipH = (h >> mip) > 1 ? (h >> mip) : 1;
      uint32_t prevW = (w >> (mip-1)) > 1 ? (w >> (mip-1)) : 1;
      uint32_t prevH = (h >> (mip-1)) > 1 ? (h >> (mip-1)) : 1;

      cmdList->SetPipelineState(_hizDownsamplePSO.Get());
      cmdList->SetComputeRootSignature(_hizRootSig.Get());

      auto desc = _cbvSrvUavHeap.AllocateDynamic(2);
      // SRV for prev mip
      D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
      srvDesc.Format = DXGI_FORMAT_R32_FLOAT;
      srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
      srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
      srvDesc.Texture2D.MostDetailedMip = mip - 1;
      srvDesc.Texture2D.MipLevels = 1;
      _device.GetDevice()->CreateShaderResourceView(_hizTexture.Get(), &srvDesc,
          {desc.cpu.ptr});
      // UAV for current mip
      D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
      uavDesc.Format = DXGI_FORMAT_R32_FLOAT;
      uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
      uavDesc.Texture2D.MipSlice = mip;
      _device.GetDevice()->CreateUnorderedAccessView(_hizTexture.Get(), nullptr, &uavDesc,
          {desc.cpu.ptr + _cbvSrvUavHeap.GetDescriptorSize()});

      cmdList->SetComputeRootDescriptorTable(0, desc.gpu);
      uint32_t pushData[4] = {prevW, prevH, mipW, mipH};
      cmdList->SetComputeRoot32BitConstants(1, 4, pushData, 0);
      cmdList->Dispatch((mipW + 7) / 8, (mipH + 7) / 8, 1);
    }

    // Transition HiZ to SRV for cull shader
    DX12Util::TransitionBarrier(cmdList, _hizTexture.Get(),
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    // Transition occluder depth back
    DX12Util::TransitionBarrier(cmdList, _depthTexture.Get(),
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE);
  }

  // === GPU Cull Compute Dispatch ===
  if (_gpuCullPSO && _meshChunksBuffer) {
    auto& fr = _frameResources[_currentFrame];
    uint32_t totalChunks = (uint32_t)(_applMesh->_opaqueChunkCount +
                                       _applMesh->_alphaMaskedChunkCount +
                                       _applMesh->_transparentChunkCount);

    // Zero writeIndex counters via copy from upload buffer
    memset(fr.writeIndexUploadMapped, 0, 3 * sizeof(uint32_t));
    DX12Util::TransitionBarrier(cmdList, fr.writeIndexBuffer.Get(),
        D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
    cmdList->CopyBufferRegion(fr.writeIndexBuffer.Get(), 0,
        fr.writeIndexUpload.Get(), 0, 3 * sizeof(uint32_t));
    DX12Util::TransitionBarrier(cmdList, fr.writeIndexBuffer.Get(),
        D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    // Update GPU cull params
    GPUCullParams cullParams = {};
    cullParams.opaqueChunkCount = (uint32_t)_applMesh->_opaqueChunkCount;
    cullParams.alphaMaskedChunkCount = (uint32_t)_applMesh->_alphaMaskedChunkCount;
    cullParams.transparentChunkCount = (uint32_t)_applMesh->_transparentChunkCount;
    cullParams.hizMipLevels = _hizMipLevels;
    cullParams.screenWidth = (float)_device.GetWidth();
    cullParams.screenHeight = (float)_device.GetHeight();
    // mat4 quirk: `A * B` computes the mathematical B*A, so view * proj gives
    // the mathematical P*V that gpucull.hlsl's mul(viewProjMatrix, corner)
    // needs. Writing proj * view here yields V*P and corrupts the Hi-Z
    // occlusion test. Matches GpuScene.cpp's Vulkan path.
    // Uses the JITTERED projection (Vulkan parity): culling against the
    // clean projection lets chunks near frustum edges flicker in/out when
    // the jitter shifts the projection between frames.
    mat4 vp = transpose(_mainCamera->getObjectToCamera() * _jitteredProj);
    memcpy(cullParams.viewProjMatrix, vp.value_ptr(), sizeof(float) * 16);
    memcpy(&cullParams.frustum, &_mainCamera->getFrustum(), sizeof(Frustum));
    memcpy(fr.cullParamsMapped, &cullParams, sizeof(GPUCullParams));

    // Transition drawParams + chunkIndices to UAV
    DX12Util::TransitionBarrier(cmdList, fr.drawParamsBuffer.Get(),
        D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    DX12Util::TransitionBarrier(cmdList, fr.chunkIndicesBuffer.Get(),
        D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    // Allocate dynamic descriptors for cull pass: u0(drawParams), u3(writeIndex), u4(chunkIndices), t2(meshChunks), t6(hizTexture)
    auto cullDescs = _cbvSrvUavHeap.AllocateDynamic(5);
    auto descSize = _cbvSrvUavHeap.GetDescriptorSize();

    // u0: drawParams UAV
    {
      D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
      uavDesc.Format = DXGI_FORMAT_UNKNOWN;
      uavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
      uavDesc.Buffer.NumElements = totalChunks;
      uavDesc.Buffer.StructureByteStride = sizeof(DX12DrawIndexedArgs);
      _device.GetDevice()->CreateUnorderedAccessView(fr.drawParamsBuffer.Get(), nullptr, &uavDesc,
          {cullDescs.cpu.ptr + 0 * descSize});
    }
    // u3: writeIndex UAV
    {
      D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
      uavDesc.Format = DXGI_FORMAT_R32_UINT;
      uavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
      uavDesc.Buffer.NumElements = 3;
      _device.GetDevice()->CreateUnorderedAccessView(fr.writeIndexBuffer.Get(), nullptr, &uavDesc,
          {cullDescs.cpu.ptr + 1 * descSize});
    }
    // u4: chunkIndices UAV
    {
      D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
      uavDesc.Format = DXGI_FORMAT_R32_UINT;
      uavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
      uavDesc.Buffer.NumElements = totalChunks;
      _device.GetDevice()->CreateUnorderedAccessView(fr.chunkIndicesBuffer.Get(), nullptr, &uavDesc,
          {cullDescs.cpu.ptr + 2 * descSize});
    }
    // t2: meshChunks SRV
    {
      D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
      srvDesc.Format = DXGI_FORMAT_UNKNOWN;
      srvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
      srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
      srvDesc.Buffer.NumElements = (UINT)_applMesh->_chunkCount;
      srvDesc.Buffer.StructureByteStride = MESH_CHUNK_STRIDE;
      _device.GetDevice()->CreateShaderResourceView(_meshChunksBuffer.Get(), &srvDesc,
          {cullDescs.cpu.ptr + 3 * descSize});
    }
    // t6: hizTexture SRV
    {
      D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
      srvDesc.Format = DXGI_FORMAT_R32_FLOAT;
      srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
      srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
      srvDesc.Texture2D.MipLevels = _hizMipLevels;
      _device.GetDevice()->CreateShaderResourceView(_hizTexture.Get(), &srvDesc,
          {cullDescs.cpu.ptr + 4 * descSize});
    }

    cmdList->SetPipelineState(_gpuCullPSO.Get());
    cmdList->SetComputeRootSignature(_gpuCullRootSig.Get());
    cmdList->SetComputeRootConstantBufferView(0, fr.cullParamsBuffer->GetGPUVirtualAddress());
    cmdList->SetComputeRootDescriptorTable(1, cullDescs.gpu);

    uint32_t groupX = (totalChunks + 127) / 128;
    cmdList->Dispatch(groupX, 1, 1);

    // Barrier: compute UAV writes 鈫?indirect draw read
    DX12Util::UAVBarrier(cmdList, fr.drawParamsBuffer.Get());
    DX12Util::UAVBarrier(cmdList, fr.writeIndexBuffer.Get());

    // Transition drawParams to INDIRECT_ARGUMENT
    DX12Util::TransitionBarrier(cmdList, fr.drawParamsBuffer.Get(),
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
    DX12Util::TransitionBarrier(cmdList, fr.writeIndexBuffer.Get(),
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
    DX12Util::TransitionBarrier(cmdList, fr.chunkIndicesBuffer.Get(),
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_GENERIC_READ);
    // HiZ was left in NON_PIXEL_SHADER_RESOURCE for cull; transition back so
    // next frame's HiZ generation can start from COMMON.
    if (_hizTexture) {
      DX12Util::TransitionBarrier(cmdList, _hizTexture.Get(),
          D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
    }
  }

  // === Base Pass (G-buffers) with ExecuteIndirect ===
  if (_basePassPSO && _vertexBuffer) {
    // Clear G-buffers
    D3D12_CPU_DESCRIPTOR_HANDLE gbRtvHandles[4];
    auto gbRtvBase = _gbufferRtvHeap->GetCPUDescriptorHandleForHeapStart();
    float blackColor[] = {0, 0, 0, 0};
    for (int i = 0; i < 4; ++i) {
      gbRtvHandles[i] = gbRtvBase;
      gbRtvHandles[i].ptr += (SIZE_T)i * _gbufferRtvSize;
      cmdList->ClearRenderTargetView(gbRtvHandles[i], blackColor, 0, nullptr);
    }

    // Use window depth (already cleared above)
    cmdList->OMSetRenderTargets(4, gbRtvHandles, FALSE, &dsvHandle);

    cmdList->SetPipelineState(_basePassPSO.Get());
    cmdList->SetGraphicsRootSignature(_drawClusterRootSig.Get());
    cmdList->SetGraphicsRootConstantBufferView(0,
        _frameResources[_currentFrame].uniformBuffer->GetGPUVirtualAddress());
    cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    // Bind VBs
    D3D12_VERTEX_BUFFER_VIEW vbvs[4] = {};
    ID3D12Resource* vbs[] = {_vertexBuffer.Get(), _normalBuffer.Get(), _tangentBuffer.Get(), _uvBuffer.Get()};
    uint32_t strides[] = {sizeof(float)*3, sizeof(float)*3, sizeof(float)*3, sizeof(float)*2};
    for (int i = 0; i < 4; ++i) {
      vbvs[i].BufferLocation = vbs[i]->GetGPUVirtualAddress();
      vbvs[i].SizeInBytes = (UINT)vbs[i]->GetDesc().Width;
      vbvs[i].StrideInBytes = strides[i];
    }
    cmdList->IASetVertexBuffers(0, 4, vbvs);

    D3D12_INDEX_BUFFER_VIEW ibv = {};
    ibv.BufferLocation = _indexBuffer->GetGPUVirtualAddress();
    ibv.SizeInBytes = (UINT)_indexBuffer->GetDesc().Width;
    ibv.Format = DXGI_FORMAT_R32_UINT;
    cmdList->IASetIndexBuffer(&ibv);

    // Build dynamic descriptor table: t0=materials, t3=meshChunks, t4=chunkIndex
    {
      auto tableDesc = _cbvSrvUavHeap.AllocateDynamic(3);
      auto ds = _cbvSrvUavHeap.GetDescriptorSize();
      auto* dev = _device.GetDevice();
      // t0,space1: materials
      { D3D12_SHADER_RESOURCE_VIEW_DESC d = {}; d.Format = DXGI_FORMAT_UNKNOWN;
        d.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        d.Buffer.NumElements = (UINT)_applMesh->_materialCount;
        d.Buffer.StructureByteStride = sizeof(AAPLShaderMaterial);
        dev->CreateShaderResourceView(_materialBuffer.Get(), &d, tableDesc.cpu); }
      // t3,space1: meshChunks
      { D3D12_SHADER_RESOURCE_VIEW_DESC d = {}; d.Format = DXGI_FORMAT_UNKNOWN;
        d.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        d.Buffer.NumElements = (UINT)_applMesh->_chunkCount;
        d.Buffer.StructureByteStride = MESH_CHUNK_STRIDE;
        D3D12_CPU_DESCRIPTOR_HANDLE dst = { tableDesc.cpu.ptr + ds };
        dev->CreateShaderResourceView(_meshChunksBuffer.Get(), &d, dst); }
      // t4,space1: chunkIndex (per-frame)
      { D3D12_SHADER_RESOURCE_VIEW_DESC d = {}; d.Format = DXGI_FORMAT_R32_UINT;
        d.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        d.Buffer.NumElements = (UINT)_applMesh->_chunkCount;
        D3D12_CPU_DESCRIPTOR_HANDLE dst = { tableDesc.cpu.ptr + 2 * ds };
        dev->CreateShaderResourceView(_frameResources[_currentFrame].chunkIndicesBuffer.Get(), &d, dst); }
      cmdList->SetGraphicsRootDescriptorTable(1, tableDesc.gpu);
      cmdList->SetGraphicsRootDescriptorTable(2, _cbvSrvUavHeap.GetStaticGPU(SRV_BINDLESS_START));
    }

    auto& fr = _frameResources[_currentFrame];
    uint32_t opaqueCount = (uint32_t)_applMesh->_opaqueChunkCount;
    uint32_t alphaCount = (uint32_t)_applMesh->_alphaMaskedChunkCount;

    // Opaque: ExecuteIndirect with count buffer
    cmdList->ExecuteIndirect(
        _drawIndexedCmdSig.Get(),
        opaqueCount,
        fr.drawParamsBuffer.Get(), 0,
        fr.writeIndexBuffer.Get(), 0);

    // Alpha-masked
    if (_basePassAlphaMaskPSO) {
      cmdList->SetPipelineState(_basePassAlphaMaskPSO.Get());
    }
    cmdList->ExecuteIndirect(
        _drawIndexedCmdSig.Get(),
        alphaCount,
        fr.drawParamsBuffer.Get(), opaqueCount * sizeof(DX12DrawIndexedArgs),
        fr.writeIndexBuffer.Get(), sizeof(uint32_t));
  }

  // === Transition G-buffers to SRV ===
  {
    D3D12_RESOURCE_BARRIER barriers[4];
    for (int i = 0; i < 4; ++i) {
      barriers[i] = {};
      barriers[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
      barriers[i].Transition.pResource = _gbuffers[i].Get();
      barriers[i].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
      barriers[i].Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
      barriers[i].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    }
    cmdList->ResourceBarrier(4, barriers);
  }

  // === SAO Compute (after base pass, before deferred lighting) ===
  if (_saoPSO && _saoDepthPyramid) {
    uint32_t w = _device.GetWidth(), h = _device.GetHeight();

    // Build SAO depth pyramid from window depth
    // Transition window depth to SRV
    DX12Util::TransitionBarrier(cmdList, _device.GetDepthStencilBuffer(),
        D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    DX12Util::TransitionBarrier(cmdList, _saoDepthPyramid.Get(),
        D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    // Copy window depth 鈫?SAO pyramid mip 0
    {
      cmdList->SetPipelineState(_hizCopyPSO.Get());
      cmdList->SetComputeRootSignature(_hizRootSig.Get());
      auto desc = _cbvSrvUavHeap.AllocateDynamic(2);
      D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
      srvDesc.Format = DXGI_FORMAT_R32_FLOAT;
      srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
      srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
      srvDesc.Texture2D.MipLevels = 1;
      _device.GetDevice()->CreateShaderResourceView(_device.GetDepthStencilBuffer(), &srvDesc, {desc.cpu.ptr});
      D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
      uavDesc.Format = DXGI_FORMAT_R32_FLOAT;
      uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
      _device.GetDevice()->CreateUnorderedAccessView(_saoDepthPyramid.Get(), nullptr, &uavDesc,
          {desc.cpu.ptr + _cbvSrvUavHeap.GetDescriptorSize()});
      cmdList->SetComputeRootDescriptorTable(0, desc.gpu);
      uint32_t pushData[4] = {w, h, w, h};
      cmdList->SetComputeRoot32BitConstants(1, 4, pushData, 0);
      cmdList->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
    }

    // Downsample SAO pyramid
    for (uint32_t mip = 1; mip < _saoMipLevels; ++mip) {
      DX12Util::UAVBarrier(cmdList, _saoDepthPyramid.Get());
      uint32_t mipW = (w >> mip) > 1 ? (w >> mip) : 1;
      uint32_t mipH = (h >> mip) > 1 ? (h >> mip) : 1;
      uint32_t prevW = (w >> (mip-1)) > 1 ? (w >> (mip-1)) : 1;
      uint32_t prevH = (h >> (mip-1)) > 1 ? (h >> (mip-1)) : 1;

      cmdList->SetPipelineState(_hizDownsamplePSO.Get());
      cmdList->SetComputeRootSignature(_hizRootSig.Get());
      auto desc = _cbvSrvUavHeap.AllocateDynamic(2);
      D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
      srvDesc.Format = DXGI_FORMAT_R32_FLOAT;
      srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
      srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
      srvDesc.Texture2D.MostDetailedMip = mip - 1;
      srvDesc.Texture2D.MipLevels = 1;
      _device.GetDevice()->CreateShaderResourceView(_saoDepthPyramid.Get(), &srvDesc, {desc.cpu.ptr});
      D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
      uavDesc.Format = DXGI_FORMAT_R32_FLOAT;
      uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
      uavDesc.Texture2D.MipSlice = mip;
      _device.GetDevice()->CreateUnorderedAccessView(_saoDepthPyramid.Get(), nullptr, &uavDesc,
          {desc.cpu.ptr + _cbvSrvUavHeap.GetDescriptorSize()});
      cmdList->SetComputeRootDescriptorTable(0, desc.gpu);
      uint32_t pushData[4] = {prevW, prevH, mipW, mipH};
      cmdList->SetComputeRoot32BitConstants(1, 4, pushData, 0);
      cmdList->Dispatch((mipW + 7) / 8, (mipH + 7) / 8, 1);
    }

    // Transition SAO pyramid to SRV
    DX12Util::TransitionBarrier(cmdList, _saoDepthPyramid.Get(),
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    // Dispatch SAO compute
    {
      DX12Util::TransitionBarrier(cmdList, _aoTexture.Get(),
          D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

      cmdList->SetPipelineState(_saoPSO.Get());
      cmdList->SetComputeRootSignature(_saoRootSig.Get());

      // Allocate descriptors: 2 SRV (depth, pyramid) + 1 UAV (AO output)
      auto desc = _cbvSrvUavHeap.AllocateDynamic(3);
      // SRV: window depth
      D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
      srvDesc.Format = DXGI_FORMAT_R32_FLOAT;
      srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
      srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
      srvDesc.Texture2D.MipLevels = 1;
      _device.GetDevice()->CreateShaderResourceView(_device.GetDepthStencilBuffer(), &srvDesc, {desc.cpu.ptr});
      // SRV: SAO depth pyramid (full mip chain)
      srvDesc.Texture2D.MipLevels = _saoMipLevels;
      _device.GetDevice()->CreateShaderResourceView(_saoDepthPyramid.Get(), &srvDesc,
          {desc.cpu.ptr + _cbvSrvUavHeap.GetDescriptorSize()});
      // UAV: AO output
      D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
      uavDesc.Format = DXGI_FORMAT_R8_UNORM;
      uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
      _device.GetDevice()->CreateUnorderedAccessView(_aoTexture.Get(), nullptr, &uavDesc,
          {desc.cpu.ptr + 2 * _cbvSrvUavHeap.GetDescriptorSize()});

      cmdList->SetComputeRootDescriptorTable(0, desc.gpu);
      cmdList->SetComputeRootConstantBufferView(1,
          _frameResources[_currentFrame].uniformBuffer->GetGPUVirtualAddress());
      uint32_t screenSize[2] = {w, h};
      cmdList->SetComputeRoot32BitConstants(2, 2, screenSize, 0);
      cmdList->Dispatch((w + 7) / 8, (h + 7) / 8, 1);

      // Transition AO to SRV
      DX12Util::TransitionBarrier(cmdList, _aoTexture.Get(),
          D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }

    // Transition window depth back to DEPTH_WRITE (will be transitioned to READ in deferred)
    DX12Util::TransitionBarrier(cmdList, _device.GetDepthStencilBuffer(),
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE);

    // Transition SAO pyramid back to common for next frame
    DX12Util::TransitionBarrier(cmdList, _saoDepthPyramid.Get(),
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
  }

  // === Light Culling (CoarseCull + TraditionalCull, after SAO, before deferred) ===
  DispatchLightCulling(cmdList);

  // === Scatter Volume (froxel volumetrics, after light culling, before deferred) ===
  if (IsScatterReady() && _frameConstants.scatterScale > 0.0f) {
    auto* dev = _device.GetDevice();
    uint32_t w = _device.GetWidth(), h = _device.GetHeight();

    // PushConstants layout: volumeWidth, volumeHeight, screenWidth, screenHeight, resetHistory.
    // Vulkan parity (ScatteringVolume::dispatch): screenWidth/Height are the PADDED
    // size volumeW*8 / volumeH*8, which the kernel uses only for the light-tile
    // count. resetHistory=1 until a frame's result has been copied into the
    // history volume (first frame, toggle-off gap, or resize).
    float sw = (float)(_scatterFroxelW * 8), sh = (float)(_scatterFroxelH * 8);
    uint32_t pcData[5] = {
      _scatterFroxelW, _scatterFroxelH,
      *reinterpret_cast<const uint32_t*>(&sw),
      *reinterpret_cast<const uint32_t*>(&sh),
      (!_scatterHistoryValid) ? 1u : 0u
    };

    // --- ScatterVolume pass ---
    cmdList->SetPipelineState(_scatterVolumePSO.Get());
    cmdList->SetComputeRootSignature(_scatterRootSig.Get());

    // [0] Root constants: PushConstants (5 x uint32)
    cmdList->SetComputeRoot32BitConstants(0, 5, pcData, 0);

    // [1] CBV: uniform buffer (CameraParamsBufferFull + AAPLFrameConstants at b1 space0)
    cmdList->SetComputeRootConstantBufferView(1,
        _frameResources[_currentFrame].uniformBuffer->GetGPUVirtualAddress());

    // [2] Descriptor table: u0 (scatterOut UAV) + t2..t13 SRVs
    // Slot layout (13 descriptors total):
    //   slot0 = u0 scatterOut UAV
    //   slot1 = t2 shadowMaps SRV
    //   slot2 = t3 (null 鈥?s3 is static sampler shadowSampler)
    //   slot3 = t4 scatterPrev SRV (history volume; ignored when resetHistory=1)
    //   slot4 = t5 blueNoiseTex SRV
    //   slot5 = t6 pointLightData SRV
    //   slot6 = t7 pointLightIndices SRV
    //   slot7 = t8 spotLightData SRV
    //   slot8 = t9 spotLightIndices SRV
    //   slot9 = t10 spotShadowMaps SRV
    //   slot10 = t11 (null 鈥?s11 is static sampler spotShadowSampler)
    //   slot11 = t12 spotViewProjMatrices SRV
    //   slot12 = t13 perlinNoiseTex SRV
    // Total: 1 UAV + 12 SRVs = 13 descriptors
    {
      auto scDesc = _cbvSrvUavHeap.AllocateDynamic(13);
      auto ds = _cbvSrvUavHeap.GetDescriptorSize();

      // slot0: u0 scatterOut UAV
      {
        D3D12_UNORDERED_ACCESS_VIEW_DESC uavD = {};
        uavD.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        uavD.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
        uavD.Texture3D.WSize = SCATTER_FROXEL_D;
        dev->CreateUnorderedAccessView(_scatterVolume.Get(), nullptr, &uavD,
            {scDesc.cpu.ptr});
      }

      // slot1: t2 shadowMaps SRV
      dev->CopyDescriptorsSimple(1, {scDesc.cpu.ptr + 1u * ds},
          _cbvSrvUavHeap.GetStaticCPU(SRV_SHADOW_MAPS),
          D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

      // slot2: t3 (shadowSampler static 鈥?fill with null SRV to satisfy range)
      {
        D3D12_SHADER_RESOURCE_VIEW_DESC nd = {};
        nd.Format = DXGI_FORMAT_R8_UNORM;
        nd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        nd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        nd.Texture2D.MipLevels = 1;
        dev->CreateShaderResourceView(nullptr, &nd, {scDesc.cpu.ptr + 2u * ds});
      }

      // slot3: t4 scatterPrev SRV — dedicated history volume (Vulkan parity:
      // copy of LAST FRAME's scatter, not the accumulated integral). Its
      // content is irrelevant when resetHistory=1 (kernel skips the blend),
      // so always bind the static SRV; the resource rests in NPSR.
      dev->CopyDescriptorsSimple(1, {scDesc.cpu.ptr + 3u * ds},
          _cbvSrvUavHeap.GetStaticCPU(SRV_SCATTER_HISTORY),
          D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

      // slots 4-12: t5..t13 (null defaults, overwritten below where data exists)
      for (int k = 4; k <= 12; ++k) {
        D3D12_SHADER_RESOURCE_VIEW_DESC nd = {};
        nd.Format = DXGI_FORMAT_R32_FLOAT;
        nd.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        nd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        dev->CreateShaderResourceView(nullptr, &nd, {scDesc.cpu.ptr + (UINT64)k * ds});
      }
      // slot4 (t5): blueNoiseTex — 64x64 R8, rests in NPSR (compute-only reads)
      dev->CopyDescriptorsSimple(1, {scDesc.cpu.ptr + 4u * ds},
          _cbvSrvUavHeap.GetStaticCPU(SRV_SCATTER_BLUE_NOISE),
          D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
      // slot5 (t6): pointLightData
      if (_pointLightBuffer && !_pointLights.empty()) {
        D3D12_SHADER_RESOURCE_VIEW_DESC d = {};
        d.Format = DXGI_FORMAT_UNKNOWN;
        d.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        d.Buffer.NumElements = (UINT)_pointLights.size();
        d.Buffer.StructureByteStride = sizeof(AAPLPointLightCullingData);
        dev->CreateShaderResourceView(_pointLightBuffer.Get(), &d, {scDesc.cpu.ptr + 5u * ds});
      }
      // slot6 (t7): pointLightIndices (from light cull 鈥?transition to read)
      if (_lightIndicesBuffer && !_pointLights.empty()) {
        uint32_t tW2 = (w + LIGHT_TILE_SIZE - 1) / LIGHT_TILE_SIZE;
        uint32_t tH2 = (h + LIGHT_TILE_SIZE - 1) / LIGHT_TILE_SIZE;
        D3D12_SHADER_RESOURCE_VIEW_DESC d = {};
        d.Format = DXGI_FORMAT_R32_UINT;
        d.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        d.Buffer.NumElements = tW2 * tH2 * MAX_LIGHTS_PER_TILE_LC;
        dev->CreateShaderResourceView(_lightIndicesBuffer.Get(), &d, {scDesc.cpu.ptr + 6u * ds});
      }
      // slot7 (t8): spotLightData
      if (_spotLightBuffer && !_spotLights.empty()) {
        D3D12_SHADER_RESOURCE_VIEW_DESC d = {};
        d.Format = DXGI_FORMAT_UNKNOWN;
        d.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        d.Buffer.NumElements = (UINT)_spotLights.size();
        d.Buffer.StructureByteStride = sizeof(AAPLSpotLightCullingData);
        dev->CreateShaderResourceView(_spotLightBuffer.Get(), &d, {scDesc.cpu.ptr + 7u * ds});
      }
      // slot8 (t9): spotLightIndices (GENERIC_READ covers compute reads)
      if (_spotLightIndicesBuffer && !_spotLights.empty()) {
        uint32_t tW2 = (w + LIGHT_TILE_SIZE - 1) / LIGHT_TILE_SIZE;
        uint32_t tH2 = (h + LIGHT_TILE_SIZE - 1) / LIGHT_TILE_SIZE;
        D3D12_SHADER_RESOURCE_VIEW_DESC d = {};
        d.Format = DXGI_FORMAT_UNKNOWN;
        d.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        d.Buffer.NumElements = tW2 * tH2 * MAX_LIGHTS_PER_TILE_LC;
        d.Buffer.StructureByteStride = sizeof(uint32_t);
        dev->CreateShaderResourceView(_spotLightIndicesBuffer.Get(), &d, {scDesc.cpu.ptr + 8u * ds});
      }
      // slot9 (t10): spotShadowMaps 鈥?compute reads need a non-pixel SRV
      // state, so transition PSR -> NPSR here and back after the dispatch.
      if (_spotShadowMapArray && _spotShadowsRendered && !_spotLights.empty()) {
        DX12Util::TransitionBarrier(cmdList, _spotShadowMapArray.Get(),
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        D3D12_SHADER_RESOURCE_VIEW_DESC d = {};
        d.Format = DXGI_FORMAT_R32_FLOAT;
        d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
        d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        d.Texture2DArray.MipLevels = 1;
        d.Texture2DArray.ArraySize = MAX_SPOT_SHADOWS;
        dev->CreateShaderResourceView(_spotShadowMapArray.Get(), &d,
            {scDesc.cpu.ptr + 9u * ds});
      }
      // slot11 (t12): spotViewProjMatrices
      if (_spotViewProjBuffer && !_spotLights.empty()) {
        D3D12_SHADER_RESOURCE_VIEW_DESC d = {};
        d.Format = DXGI_FORMAT_UNKNOWN;
        d.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        d.Buffer.NumElements = MAX_SPOT_SHADOWS;
        d.Buffer.StructureByteStride = sizeof(mat4);
        dev->CreateShaderResourceView(_spotViewProjBuffer.Get(), &d,
            {scDesc.cpu.ptr + 11u * ds});
      }
      // slot12 (t13): perlinNoiseTex — 32^3 R8, rests in NPSR (compute-only reads)
      dev->CopyDescriptorsSimple(1, {scDesc.cpu.ptr + 12u * ds},
          _cbvSrvUavHeap.GetStaticCPU(SRV_SCATTER_PERLIN),
          D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

      cmdList->SetComputeRootDescriptorTable(2, scDesc.gpu);
    }

    // ScatterVolume kernel is numthreads(4,4,4) — ceil/4 in ALL THREE dims
    // (the old /8 in XY launched half the froxels; Z=64 groups quadrupled it).
    cmdList->Dispatch(
        (_scatterFroxelW + 3) / 4,
        (_scatterFroxelH + 3) / 4,
        (SCATTER_FROXEL_D + 3) / 4);

    // Spot shadow maps back to PSR for the deferred lighting read (the
    // scatter kernel read them as non-pixel SRV).
    if (_spotShadowMapArray && _spotShadowsRendered && !_spotLights.empty()) {
      DX12Util::TransitionBarrier(cmdList, _spotShadowMapArray.Get(),
          D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
          D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }

    // Transition scatterAccumVolume to UAV for the accumulate write. After a
    // scatter frame it rests in PSR (deferred read); on the first frame it is
    // already UAV; the toggle-off path also returns it to UAV.
    if (_scatterAccumIsInPSR) {
      DX12Util::TransitionBarrier(cmdList, _scatterAccumVolume.Get(),
          D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
          D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }

    // --- AccumulateScattering pass ---
    cmdList->SetPipelineState(_accumulatePSO.Get());
    cmdList->SetComputeRootSignature(_accumRootSig.Get());

    // [0] Root constants: PushConstants
    cmdList->SetComputeRoot32BitConstants(0, 5, pcData, 0);

    // [1] Descriptor table: t0 (scatterIn) + u1 (accumOut)
    {
      auto acDesc = _cbvSrvUavHeap.AllocateDynamic(2);
      auto ds = _cbvSrvUavHeap.GetDescriptorSize();

      // slot0: t0 scatterIn SRV (read from scatterVolume)
      // Transition scatterVolume UAV 鈫?SRV
      DX12Util::TransitionBarrier(cmdList, _scatterVolume.Get(),
          D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
          D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
      {
        D3D12_SHADER_RESOURCE_VIEW_DESC srvD = {};
        srvD.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        srvD.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
        srvD.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvD.Texture3D.MipLevels = 1;
        dev->CreateShaderResourceView(_scatterVolume.Get(), &srvD, {acDesc.cpu.ptr});
      }

      // slot1: u1 accumOut UAV (scatterAccumVolume)
      {
        D3D12_UNORDERED_ACCESS_VIEW_DESC uavD = {};
        uavD.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        uavD.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
        uavD.Texture3D.WSize = SCATTER_FROXEL_D;
        dev->CreateUnorderedAccessView(_scatterAccumVolume.Get(), nullptr, &uavD,
            {acDesc.cpu.ptr + ds});
      }

      cmdList->SetComputeRootDescriptorTable(1, acDesc.gpu);
    }

    cmdList->Dispatch(
        (_scatterFroxelW + 7) / 8,
        (_scatterFroxelH + 7) / 8, 1);
    DX12Util::UAVBarrier(cmdList, _scatterAccumVolume.Get());

    // ===== Copy this frame's scatter into the history volume (Vulkan parity:
    // the t4 scatterPrev of frame N+1 is the RAW scatter of frame N, NOT the
    // accumulated integral — blending accum would double-count transmittance
    // and make the temporal blend converge toward the front-slab only).
    // scatterVolume is in NPSR (t0 read above); history rests in NPSR.
    {
      DX12Util::TransitionBarrier(cmdList, _scatterVolume.Get(),
          D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
          D3D12_RESOURCE_STATE_COPY_SOURCE);
      DX12Util::TransitionBarrier(cmdList, _scatterHistoryVolume.Get(),
          D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
          D3D12_RESOURCE_STATE_COPY_DEST);

      D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {};
      dst.pResource = _scatterHistoryVolume.Get();
      dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
      dst.SubresourceIndex = 0;
      src.pResource = _scatterVolume.Get();
      src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
      src.SubresourceIndex = 0;
      cmdList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

      DX12Util::TransitionBarrier(cmdList, _scatterHistoryVolume.Get(),
          D3D12_RESOURCE_STATE_COPY_DEST,
          D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
      // scatterVolume returns to UAV for next frame's scatterOut write
      DX12Util::TransitionBarrier(cmdList, _scatterVolume.Get(),
          D3D12_RESOURCE_STATE_COPY_SOURCE,
          D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
    _scatterHistoryValid = true;

    // Transition scatterAccumVolume 鈫?PIXEL_SHADER_RESOURCE for deferred lighting
    DX12Util::TransitionBarrier(cmdList, _scatterAccumVolume.Get(),
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    _scatterAccumIsInPSR = true; // will be in PIXEL_SHADER_RESOURCE for deferred lighting to read
  }


  // === Deferred Lighting (full-screen triangle into HDR buffer) ===
  {
    // Transition window depth to SRV for deferred lighting read
    DX12Util::TransitionBarrier(cmdList, _device.GetDepthStencilBuffer(),
        D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_DEPTH_READ);

    // Output to the HDR buffer; the resolve pass below tone-maps (+TAA) into
    // the swapchain and writes the new TAA history.
    D3D12_CPU_DESCRIPTOR_HANDLE hdrRtv =
        _hdrRtvHeap->GetCPUDescriptorHandleForHeapStart();
    cmdList->OMSetRenderTargets(1, &hdrRtv, FALSE, nullptr);
    cmdList->SetPipelineState(_deferredLightingPSO.Get());
    cmdList->SetGraphicsRootSignature(_deferredLightingRootSig.Get());
    cmdList->SetGraphicsRootConstantBufferView(0,
        _frameResources[_currentFrame].uniformBuffer->GetGPUVirtualAddress());
    cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    // Build dynamic 18-slot table mapping t0-t17 (space1) to actual resources.
    // The root sig uses a single continuous range t0-t17, so slot offset = register index.
    {
      auto dlDesc = _cbvSrvUavHeap.AllocateDynamic(18);
      auto ds   = _cbvSrvUavHeap.GetDescriptorSize();
      auto* dv  = _device.GetDevice();

      // Helper: write a null 2D SRV at a given slot offset (for unused registers)
      auto nullTex2D = [&](uint32_t off) {
        D3D12_SHADER_RESOURCE_VIEW_DESC d = {};
        d.Format = DXGI_FORMAT_R8_UNORM;
        d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        d.Texture2D.MipLevels = 1;
        dv->CreateShaderResourceView(nullptr, &d, {dlDesc.cpu.ptr + off * ds});
      };
      // Helper: write a null structured-buffer SRV (for StructuredBuffer<> slots)
      auto nullBuf = [&](uint32_t off, uint32_t stride) {
        D3D12_SHADER_RESOURCE_VIEW_DESC d = {};
        d.Format = DXGI_FORMAT_UNKNOWN;
        d.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        d.Buffer.NumElements = 0;
        d.Buffer.StructureByteStride = stride;
        dv->CreateShaderResourceView(nullptr, &d, {dlDesc.cpu.ptr + off * ds});
      };

      // t0-t3: G-buffers (albedo, normals, emissive, F0/Roughness)
      static const DXGI_FORMAT kGbFmt[4] = {
        DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, DXGI_FORMAT_R16G16B16A16_FLOAT,
        DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB };
      for (int i = 0; i < 4; ++i) {
        D3D12_SHADER_RESOURCE_VIEW_DESC d = {};
        d.Format = kGbFmt[i];
        d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        d.Texture2D.MipLevels = 1;
        dv->CreateShaderResourceView(_gbuffers[i].Get(), &d, {dlDesc.cpu.ptr + (UINT64)i * ds});
      }
      // t4: window depth
      { D3D12_SHADER_RESOURCE_VIEW_DESC d = {};
        d.Format = DXGI_FORMAT_R32_FLOAT;
        d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        d.Texture2D.MipLevels = 1;
        dv->CreateShaderResourceView(_device.GetDepthStencilBuffer(), &d, {dlDesc.cpu.ptr + 4 * ds}); }
      // t5: unused
      nullTex2D(5);
      // t6: shadow maps (Texture2DArray)
      if (_shadowMapArray) {
        D3D12_SHADER_RESOURCE_VIEW_DESC d = {};
        d.Format = DXGI_FORMAT_R32_FLOAT;
        d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
        d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        d.Texture2DArray.MipLevels = 1;
        d.Texture2DArray.ArraySize = SHADOW_CASCADE_COUNT;
        dv->CreateShaderResourceView(_shadowMapArray.Get(), &d, {dlDesc.cpu.ptr + 6 * ds});
      } else { nullTex2D(6); }
      // t7: unused
      nullTex2D(7);
      // t8: point light culling data
      if (_pointLightBuffer && !_pointLights.empty()) {
        D3D12_SHADER_RESOURCE_VIEW_DESC d = {};
        d.Format = DXGI_FORMAT_UNKNOWN;
        d.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        d.Buffer.NumElements = (UINT)_pointLights.size();
        d.Buffer.StructureByteStride = sizeof(AAPLPointLightCullingData);
        dv->CreateShaderResourceView(_pointLightBuffer.Get(), &d, {dlDesc.cpu.ptr + 8 * ds});
      } else { nullBuf(8, sizeof(AAPLPointLightCullingData)); }
      // t9: light indices (StructuredBuffer<int>)
      if (_lightIndicesBuffer && !_pointLights.empty()) {
        D3D12_SHADER_RESOURCE_VIEW_DESC d = {};
        d.Format = DXGI_FORMAT_UNKNOWN;
        d.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        d.Buffer.NumElements = (UINT)(_lightIndicesBuffer->GetDesc().Width / sizeof(uint32_t));
        d.Buffer.StructureByteStride = sizeof(uint32_t);
        dv->CreateShaderResourceView(_lightIndicesBuffer.Get(), &d, {dlDesc.cpu.ptr + 9 * ds});
      } else { nullBuf(9, sizeof(uint32_t)); }
      // t10: AO texture 鈥?must be at offset 10, was incorrectly at offset 5 via static table
      { D3D12_SHADER_RESOURCE_VIEW_DESC d = {};
        d.Format = DXGI_FORMAT_R8_UNORM;
        d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        d.Texture2D.MipLevels = 1;
        dv->CreateShaderResourceView(_aoTexture.Get(), &d, {dlDesc.cpu.ptr + 10 * ds}); }
      // t11: spot light culling data
      if (_spotLightBuffer && !_spotLights.empty()) {
        D3D12_SHADER_RESOURCE_VIEW_DESC d = {};
        d.Format = DXGI_FORMAT_UNKNOWN;
        d.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        d.Buffer.NumElements = (UINT)_spotLights.size();
        d.Buffer.StructureByteStride = sizeof(AAPLSpotLightCullingData);
        dv->CreateShaderResourceView(_spotLightBuffer.Get(), &d, {dlDesc.cpu.ptr + 11 * ds});
      } else { nullBuf(11, sizeof(AAPLSpotLightCullingData)); }
      // t12: spot light tile indices
      if (_spotLightIndicesBuffer && !_spotLights.empty()) {
        D3D12_SHADER_RESOURCE_VIEW_DESC d = {};
        d.Format = DXGI_FORMAT_UNKNOWN;
        d.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        d.Buffer.NumElements = (UINT)(_spotLightIndicesBuffer->GetDesc().Width / sizeof(uint32_t));
        d.Buffer.StructureByteStride = sizeof(uint32_t);
        dv->CreateShaderResourceView(_spotLightIndicesBuffer.Get(), &d, {dlDesc.cpu.ptr + 12 * ds});
      } else { nullBuf(12, sizeof(uint32_t)); }
      // t13: spot shadow maps (Texture2DArray, rendered once per light)
      if (_spotShadowMapArray && _spotShadowsRendered) {
        D3D12_SHADER_RESOURCE_VIEW_DESC d = {};
        d.Format = DXGI_FORMAT_R32_FLOAT;
        d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
        d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        d.Texture2DArray.MipLevels = 1;
        d.Texture2DArray.ArraySize = MAX_SPOT_SHADOWS;
        dv->CreateShaderResourceView(_spotShadowMapArray.Get(), &d, {dlDesc.cpu.ptr + 13 * ds});
      } else { nullTex2D(13); }
      // t14: unused texture slot
      nullTex2D(14);
      // t15: spot view-proj matrices (StructuredBuffer<float4x4>)
      if (_spotViewProjBuffer && !_spotLights.empty()) {
        D3D12_SHADER_RESOURCE_VIEW_DESC d = {};
        d.Format = DXGI_FORMAT_UNKNOWN;
        d.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        d.Buffer.NumElements = MAX_SPOT_SHADOWS;
        d.Buffer.StructureByteStride = sizeof(mat4);
        dv->CreateShaderResourceView(_spotViewProjBuffer.Get(), &d, {dlDesc.cpu.ptr + 15 * ds});
      } else { nullBuf(15, sizeof(mat4)); }
      // t16: scatter accum volume (Texture3D)
      if (_scatterAccumVolume && _scatterAccumIsInPSR) {
        D3D12_SHADER_RESOURCE_VIEW_DESC d = {};
        d.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
        d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        d.Texture3D.MipLevels = 1;
        dv->CreateShaderResourceView(_scatterAccumVolume.Get(), &d, {dlDesc.cpu.ptr + 16 * ds});
      } else { nullTex2D(16); }
      // t17: unused
      nullTex2D(17);

      cmdList->SetGraphicsRootDescriptorTable(1, dlDesc.gpu);
    }

    // Draw fullscreen triangle (3 vertices, no VB)
    cmdList->DrawInstanced(3, 1, 0, 0);

    // Transition window depth back to write
    DX12Util::TransitionBarrier(cmdList, _device.GetDepthStencilBuffer(),
        D3D12_RESOURCE_STATE_DEPTH_READ, D3D12_RESOURCE_STATE_DEPTH_WRITE);

    // Restore lightIndices back to COMMON for next frame
    if (_lightIndicesBuffer && !_pointLights.empty()) {
      DX12Util::TransitionBarrier(cmdList, _lightIndicesBuffer.Get(),
          D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_COMMON);
    }
    if (_spotLightIndicesBuffer && !_spotLights.empty()) {
      DX12Util::TransitionBarrier(cmdList, _spotLightIndicesBuffer.Get(),
          D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_COMMON);
    }
  }

  // === Copy writeIndex GPU 鈫?readback buffer for CPU stats ===
  {
    auto& fr = _frameResources[_currentFrame];
    DX12Util::TransitionBarrier(cmdList, fr.writeIndexBuffer.Get(),
        D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT, D3D12_RESOURCE_STATE_COPY_SOURCE);
    cmdList->CopyBufferRegion(fr.writeIndexReadback.Get(), 0,
        fr.writeIndexBuffer.Get(), 0, 3 * sizeof(uint32_t));
    DX12Util::TransitionBarrier(cmdList, fr.writeIndexBuffer.Get(),
        D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
    DX12Util::TransitionBarrier(cmdList, fr.drawParamsBuffer.Get(),
        D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT, D3D12_RESOURCE_STATE_COMMON);
    DX12Util::TransitionBarrier(cmdList, fr.chunkIndicesBuffer.Get(),
        D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_COMMON);
  }

  // === Forward Pass (transparent objects) ===
  if (_forwardPSO && _vertexBuffer) {
    auto& fr = _frameResources[_currentFrame];
    uint32_t opaqueCount = (uint32_t)_applMesh->_opaqueChunkCount;
    uint32_t alphaCount = (uint32_t)_applMesh->_alphaMaskedChunkCount;
    uint32_t transpCount = (uint32_t)_applMesh->_transparentChunkCount;
    uint32_t transpOffset = opaqueCount + alphaCount;

    if (transpCount > 0) {
      // Render transparent objects onto the HDR buffer with alpha blending
      // (the resolve pass tone-maps everything to the swapchain afterwards).
      D3D12_CPU_DESCRIPTOR_HANDLE hdrRtv =
          _hdrRtvHeap->GetCPUDescriptorHandleForHeapStart();
      cmdList->OMSetRenderTargets(1, &hdrRtv, FALSE, &dsvHandle);
      cmdList->SetPipelineState(_forwardPSO.Get());
      cmdList->SetGraphicsRootSignature(_drawClusterRootSig.Get());
      cmdList->SetGraphicsRootConstantBufferView(0,
          _frameResources[_currentFrame].uniformBuffer->GetGPUVirtualAddress());
      cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

      D3D12_VERTEX_BUFFER_VIEW vbvs[4] = {};
      ID3D12Resource* vbs[] = {_vertexBuffer.Get(), _normalBuffer.Get(), _tangentBuffer.Get(), _uvBuffer.Get()};
      uint32_t strides[] = {sizeof(float)*3, sizeof(float)*3, sizeof(float)*3, sizeof(float)*2};
      for (int i = 0; i < 4; ++i) {
        vbvs[i].BufferLocation = vbs[i]->GetGPUVirtualAddress();
        vbvs[i].SizeInBytes = (UINT)vbs[i]->GetDesc().Width;
        vbvs[i].StrideInBytes = strides[i];
      }
      cmdList->IASetVertexBuffers(0, 4, vbvs);

      D3D12_INDEX_BUFFER_VIEW ibv = {};
      ibv.BufferLocation = _indexBuffer->GetGPUVirtualAddress();
      ibv.SizeInBytes = (UINT)_indexBuffer->GetDesc().Width;
      ibv.Format = DXGI_FORMAT_R32_UINT;
      cmdList->IASetIndexBuffer(&ibv);

      // Build dynamic descriptor table: t0=materials, t3=meshChunks, t4=chunkIndex
      {
        auto tableDesc = _cbvSrvUavHeap.AllocateDynamic(3);
        auto dds = _cbvSrvUavHeap.GetDescriptorSize();
        auto* ddev = _device.GetDevice();
        // t0,space1: materials
        { D3D12_SHADER_RESOURCE_VIEW_DESC dm = {}; dm.Format = DXGI_FORMAT_UNKNOWN;
          dm.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
          dm.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
          dm.Buffer.NumElements = (UINT)_applMesh->_materialCount;
          dm.Buffer.StructureByteStride = sizeof(AAPLShaderMaterial);
          ddev->CreateShaderResourceView(_materialBuffer.Get(), &dm, tableDesc.cpu); }
        { D3D12_SHADER_RESOURCE_VIEW_DESC d = {}; d.Format = DXGI_FORMAT_UNKNOWN;
          d.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
          d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
          d.Buffer.NumElements = (UINT)_applMesh->_chunkCount;
          d.Buffer.StructureByteStride = MESH_CHUNK_STRIDE;
          D3D12_CPU_DESCRIPTOR_HANDLE dd = { tableDesc.cpu.ptr + dds };
          ddev->CreateShaderResourceView(_meshChunksBuffer.Get(), &d, dd); }
        { D3D12_SHADER_RESOURCE_VIEW_DESC d = {}; d.Format = DXGI_FORMAT_R32_UINT;
          d.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
          d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
          d.Buffer.NumElements = (UINT)_applMesh->_chunkCount;
          D3D12_CPU_DESCRIPTOR_HANDLE dd = { tableDesc.cpu.ptr + 2 * dds };
          ddev->CreateShaderResourceView(fr.chunkIndicesBuffer.Get(), &d, dd); }
        cmdList->SetGraphicsRootDescriptorTable(1, tableDesc.gpu);
        cmdList->SetGraphicsRootDescriptorTable(2, _cbvSrvUavHeap.GetStaticGPU(SRV_BINDLESS_START));
      }

      // Transition drawParams/writeIndex back to readable for forward indirect
      DX12Util::TransitionBarrier(cmdList, fr.drawParamsBuffer.Get(),
          D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
      DX12Util::TransitionBarrier(cmdList, fr.writeIndexBuffer.Get(),
          D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);

      cmdList->ExecuteIndirect(
          _drawIndexedCmdSig.Get(),
          transpCount,
          fr.drawParamsBuffer.Get(), transpOffset * sizeof(DX12DrawIndexedArgs),
          fr.writeIndexBuffer.Get(), 2 * sizeof(uint32_t));

      DX12Util::TransitionBarrier(cmdList, fr.drawParamsBuffer.Get(),
          D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT, D3D12_RESOURCE_STATE_COMMON);
      DX12Util::TransitionBarrier(cmdList, fr.writeIndexBuffer.Get(),
          D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT, D3D12_RESOURCE_STATE_COMMON);
    }
  }

  // === Resolve pass: HDR -> swapchain (+ TAA history write) ===
  // Tone-maps (ACES) the HDR buffer into the swapchain and, when TAA is on,
  // blends against/reprojects the previous frame's history. MRT layout:
  //   SV_Target0 = swapchain, SV_Target1 = TAA history write slot.
  {
    auto* dev = _device.GetDevice();
    auto ds = _cbvSrvUavHeap.GetDescriptorSize();

    // HDR: RT (written by deferred + forward) -> PSR for the resolve read.
    DX12Util::TransitionBarrier(cmdList, _hdrBuffer.Get(),
        D3D12_RESOURCE_STATE_RENDER_TARGET,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    // History ping-pong: `_taaHistoryIndex` is the write slot this frame,
    // `_taaHistoryIndex ^ 1` is the read (stays in PSR).
    ID3D12Resource* histWrite = _taaHistory[_taaHistoryIndex].Get();
    ID3D12Resource* histRead  = _taaHistory[_taaHistoryIndex ^ 1].Get();
    DX12Util::TransitionBarrier(cmdList, histWrite,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
        D3D12_RESOURCE_STATE_RENDER_TARGET);

    // Window depth: DEPTH_WRITE (forward pass just used the DSV) -> DEPTH_READ
    // so the resolve PS can reconstruct world positions from it.
    DX12Util::TransitionBarrier(cmdList, _device.GetDepthStencilBuffer(),
        D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_DEPTH_READ);

    // (Re)create the write-slot RTV at heap slot [1] 鈥?it follows the
    // ping-pong index, so it cannot be a one-time static descriptor.
    {
      D3D12_CPU_DESCRIPTOR_HANDLE histRtv =
          _hdrRtvHeap->GetCPUDescriptorHandleForHeapStart();
      histRtv.ptr += _hdrRtvSize;
      dev->CreateRenderTargetView(histWrite, nullptr, histRtv);
    }

    D3D12_CPU_DESCRIPTOR_HANDLE rtvs[2] = {rtvHandle, {}};
    rtvs[1] = _hdrRtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtvs[1].ptr += _hdrRtvSize;
    cmdList->OMSetRenderTargets(2, rtvs, FALSE, nullptr);

    cmdList->SetPipelineState(_resolvePSO.Get());
    cmdList->SetGraphicsRootSignature(_resolveRootSig.Get());
    cmdList->SetGraphicsRootConstantBufferView(0,
        _frameResources[_currentFrame].uniformBuffer->GetGPUVirtualAddress());
    cmdList->RSSetViewports(1, &viewport);
    cmdList->RSSetScissorRects(1, &scissor);
    cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    // Descriptor table: t0=hdrBuffer, t1=history(read), t2=window depth
    {
      auto rDesc = _cbvSrvUavHeap.AllocateDynamic(3);
      D3D12_SHADER_RESOURCE_VIEW_DESC d = {};
      d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
      // t0: HDR
      d.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
      d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
      d.Texture2D.MipLevels = 1;
      dev->CreateShaderResourceView(_hdrBuffer.Get(), &d, {rDesc.cpu.ptr});
      // t1: history (read slot)
      dev->CreateShaderResourceView(histRead, &d, {rDesc.cpu.ptr + ds});
      // t2: window depth (SRV view of D32)
      d.Format = DXGI_FORMAT_R32_FLOAT;
      dev->CreateShaderResourceView(_device.GetDepthStencilBuffer(), &d,
          {rDesc.cpu.ptr + 2 * ds});
      cmdList->SetGraphicsRootDescriptorTable(1, rDesc.gpu);
    }

    cmdList->DrawInstanced(3, 1, 0, 0);

    // Restore states: HDR back to RT for the next frame's deferred pass.
    DX12Util::TransitionBarrier(cmdList, _hdrBuffer.Get(),
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
        D3D12_RESOURCE_STATE_RENDER_TARGET);
    // History write: RT -> PSR (becomes next frame's read).
    DX12Util::TransitionBarrier(cmdList, histWrite,
        D3D12_RESOURCE_STATE_RENDER_TARGET,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    // Window depth back to DEPTH_WRITE for the next frame's clear + base pass.
    DX12Util::TransitionBarrier(cmdList, _device.GetDepthStencilBuffer(),
        D3D12_RESOURCE_STATE_DEPTH_READ, D3D12_RESOURCE_STATE_DEPTH_WRITE);

    // Flip the ping-pong so the next frame reads what this one wrote.
    _taaHistoryIndex ^= 1;

    // Back to a single render target (the ImGui overlay draws to the
    // swapchain with a 1-RT pipeline state).
    cmdList->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);
  }

  // === Transition G-buffers back to RTV for next frame ===
  {
    D3D12_RESOURCE_BARRIER barriers[4];
    for (int i = 0; i < 4; ++i) {
      barriers[i] = {};
      barriers[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
      barriers[i].Transition.pResource = _gbuffers[i].Get();
      barriers[i].Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
      barriers[i].Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
      barriers[i].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    }
    cmdList->ResourceBarrier(4, barriers);
  }

  // Transition AO back to common for next frame
  if (_aoTexture) {
    DX12Util::TransitionBarrier(cmdList, _aoTexture.Get(),
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
  }
  // Transition shadow maps back to DEPTH_WRITE for next frame
  if (_shadowMapArray) {
    DX12Util::TransitionBarrier(cmdList, _shadowMapArray.Get(),
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE);
  }

  // If scatter was active last frame but is disabled now, transition scatterAccumVolume
  // back to UAV so it starts in a known state when scatter is re-enabled, and
  // invalidate the temporal history (Vulkan parity: invalidateHistory()).
  if (_scatterAccumIsInPSR && _frameConstants.scatterScale <= 0.0f && _scatterAccumVolume) {
    DX12Util::TransitionBarrier(cmdList, _scatterAccumVolume.Get(),
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    _scatterAccumIsInPSR = false;
    _scatterHistoryValid = false;
  }

  // ImGui overlay
  RenderImGuiOverlay();

  _device.EndFrameAndPresent();
  if (_imguiInitialized) EditorUi::RenderPlatformWindows();

  // One-shot debug dump (AVR_DUMP=<dir>) after the frame has settled.
  ++_drawCount;
  if (!_dumpDone && _drawCount > 60) {
    DebugDumpFrame();
    _dumpDone = true;
  }
}

// ---- ImGui ----
void DX12GpuScene::InitImGui(SDL_Window* window) {
  _sdlWindow = window;
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  EditorUi::Configure(EditorUi::SupportsDesktopViewports());

  ImGui_ImplSDL2_InitForD3D(window);

  // Reserve a descriptor for ImGui font texture
  auto fontDesc = _cbvSrvUavHeap.GetStaticCPU(0);
  auto fontDescGPU = _cbvSrvUavHeap.GetStaticGPU(0);

  ImGui_ImplDX12_Init(_device.GetDevice(), _device.GetFrameCount(),
                       _device.GetSwapChainFormat(),
                       _cbvSrvUavHeap.GetHeap(),
                       fontDesc, fontDescGPU);

  // Build font atlas on CPU side 鈥?required by legacy Init path which
  // removes ImGuiBackendFlags_RendererHasTextures.
  {
    unsigned char* pixels; int width, height;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
  }

  _imguiInitialized = true;
  spdlog::info("DX12: ImGui initialized");
}

void DX12GpuScene::ProcessImGuiEvent(SDL_Event* event) {
  if (_imguiInitialized)
    ImGui_ImplSDL2_ProcessEvent(event);
}

void DX12GpuScene::BuildImGuiFrame() {
  if (!_imguiInitialized) return;

  ImGui_ImplDX12_NewFrame();
  ImGui_ImplSDL2_NewFrame();
  ImGui::NewFrame();

  uint32_t totalChunks = _cullingStats.totalOpaque + _cullingStats.totalAlphaMask + _cullingStats.totalTransparent;
  uint32_t totalVisible = _cullingStats.visibleOpaque + _cullingStats.visibleAlphaMask + _cullingStats.visibleTransparent;
  uint32_t culled = totalChunks > totalVisible ? totalChunks - totalVisible : 0;
  float pct = totalChunks > 0 ? 100.0f * culled / totalChunks : 0.0f;

  if (!_editorUi.Begin()) {
    ImGui::Render();
    return;
  }

  ImGui::Text("Backend: DX12");
  ImGui::Text("Total Chunks: %u", totalChunks);
  ImGui::Text("Visible: %u", totalVisible);
  ImGui::Text("Culled:  %u (%.1f%%)", culled, pct);
  ImGui::Separator();
  ImGui::Text("Opaque:    %u / %u", _cullingStats.visibleOpaque, _cullingStats.totalOpaque);
  ImGui::Text("AlphaMask: %u / %u", _cullingStats.visibleAlphaMask, _cullingStats.totalAlphaMask);
  ImGui::Text("Transp:    %u / %u", _cullingStats.visibleTransparent, _cullingStats.totalTransparent);
  ImGui::Separator();
  ImGui::Checkbox("TAA", &_taaEnabled);
  ImGui::SliderFloat("Scatter", &_frameConstants.scatterScale, 0.0f, 4.0f);

  // Texture streaming stats
  if (!_streamEntries.empty()) {
    ImGui::Separator();
    uint32_t fullRes = 0, halfRes = 0, qtrRes = 0;
    for (auto& e : _streamEntries) {
      if (e.currentMip == 0) fullRes++;
      else if (e.currentMip <= 2) halfRes++;
      else qtrRes++;
    }
    ImGui::Text("Textures: %u full | %u half | %u low", fullRes, halfRes, qtrRes);
    ImGui::Text("Stream want-swap: %u", _streamWantUpgrade);
    // 甯搁┗鏄惧瓨浼扮畻(CPU 渚ц繎浼?涓嶅惈 256 琛岃窛 padding)
    uint64_t residentBytes = 0;
    for (auto& e : _streamEntries) {
      uint64_t bs, bpb;
      DX12TextureStreamingPolicy::GetPixelFormatBlockDesc(e.desc->_pixelFormat, bs, bpb);
      for (uint32_t m = e.currentMip; m < e.totalMips; ++m) {
        residentBytes += DX12TextureStreamingPolicy::CalculateMipSizeInBlocks(
                             e.desc->_width, bs, m) *
                         DX12TextureStreamingPolicy::CalculateMipSizeInBlocks(
                             e.desc->_height, bs, m) * bpb;
      }
    }
    size_t pending = 0, completed = 0, inFlightCount = 0;
    { std::lock_guard<std::mutex> l(_pendingMutex); pending = _pendingWorks.size(); }
    { std::lock_guard<std::mutex> l(_cpuWorkMutex); completed = _cpuCompletedWorks.size(); }
    for (auto& e : _streamEntries) inFlightCount += e.inFlight ? 1 : 0;
    ImGui::Text("TexMem ~%.1f MB", (double)residentBytes / (1024.0 * 1024.0));
    ImGui::Text("Stream: %zu pend / %zu done / %zu inflight",
                pending, completed, inFlightCount);
  }

  ImGui::End();

  ImGui::Render();
}

void DX12GpuScene::RenderImGuiOverlay() {
  if (!_imguiInitialized) return;
  ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), _device.GetCommandList());
}

// ---- OnResize: recreate all screen-size-dependent resources ----
void DX12GpuScene::OnResize(uint32_t newWidth, uint32_t newHeight) {
  // Wait for GPU to finish all in-flight work before releasing resources
  _device.WaitForGpu();

  // Resize the swapchain and device-level depth buffer
  _device.OnResize(newWidth, newHeight);

  // Release G-buffer resources
  for (int k = 0; k < 4; ++k)
    _gbuffers[k].Reset();
  _depthTexture.Reset();
  _aoTexture.Reset();

  // Release HDR + TAA history resources
  _hdrBuffer.Reset();
  for (int k = 0; k < 2; ++k)
    _taaHistory[k].Reset();
  _hdrRtvHeap.Reset();
  _taaHistoryIndex = 0;

  // Release HiZ / SAO pyramid resources
  _hizTexture.Reset();
  _saoDepthPyramid.Reset();

  // Release light-tile resources (sized by tileW 脳 tileH)
  _lightIndicesBuffer.Reset();
  _lightIndicesTransparentBuffer.Reset();
  _spotLightIndicesBuffer.Reset();
  _spotLightIndicesTransparentBuffer.Reset();
  _lightXZRangeBuffer.Reset();
  _spotXZRangeBuffer.Reset();
  _lightDebugTexture.Reset();

  // Recreate all screen-size resources
  CreateGBuffers();
  CreateHDRResources();
  CreateHiZResources();

  // Recreate light tile buffers at new tile resolution
  {
    auto* dev = _device.GetDevice();
    uint32_t w = _device.GetWidth(), h = _device.GetHeight();
    uint32_t tileW = (w + LIGHT_TILE_SIZE - 1) / LIGHT_TILE_SIZE;
    uint32_t tileH = (h + LIGHT_TILE_SIZE - 1) / LIGHT_TILE_SIZE;
    uint32_t totalTiles = tileW * tileH;
    uint32_t maxPL = (uint32_t)_pointLights.size();
    uint32_t maxSL = (uint32_t)_spotLights.size();
    if (maxSL == 0) maxSL = 1;

    _lightXZRangeBuffer = DX12Util::CreateGPUBuffer(dev,
        (UINT64)maxPL * 8,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
    _spotXZRangeBuffer = DX12Util::CreateGPUBuffer(dev,
        (UINT64)maxSL * 8,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);

    UINT64 idxBufSize = (UINT64)totalTiles * MAX_LIGHTS_PER_TILE_LC * sizeof(uint32_t);
    _lightIndicesBuffer = DX12Util::CreateGPUBuffer(dev, idxBufSize,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
    _lightIndicesTransparentBuffer = DX12Util::CreateGPUBuffer(dev, idxBufSize,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
    _spotLightIndicesBuffer = DX12Util::CreateGPUBuffer(dev, idxBufSize,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
    _spotLightIndicesTransparentBuffer = DX12Util::CreateGPUBuffer(dev, idxBufSize,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);

    _lightDebugTexture = DX12Util::CreateTexture2D(dev, tileW, tileH,
        DXGI_FORMAT_R32_UINT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, 1, 1,
        D3D12_RESOURCE_STATE_COMMON);

    spdlog::info("DX12GpuScene: light tile buffers resized to {}x{} tiles", tileW, tileH);
  }

  // Re-create static SRV/UAV descriptors for changed resources
  CreateStaticDescriptors();

  // Scatter volume: froxel dims derive from the screen (ceil(w/8) x ceil(h/8)),
  // so the three volumes must be re-created on resize. Stale temporal history
  // is discarded (Vulkan parity: invalidateHistory on re-create).
  if (_scatterRootSig && _accumulatePSO) {
    _scatterVolume.Reset();
    _scatterAccumVolume.Reset();
    _scatterHistoryVolume.Reset();
    // Recreate with the same state machine as init (CreateScatterResources
    // re-derives dims from the NEW screen size and rewrites the static SRVs).
    // Noise textures are resolution-independent and stay alive.
    auto* dev = _device.GetDevice();
    _scatterFroxelW = (_device.GetWidth() + 7) / 8;
    _scatterFroxelH = (_device.GetHeight() + 7) / 8;
    auto makeVol = [&](ComPtr<ID3D12Resource>& res, D3D12_RESOURCE_FLAGS flags,
                       D3D12_RESOURCE_STATES initial) {
      D3D12_RESOURCE_DESC d = {};
      d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
      d.Width = _scatterFroxelW;
      d.Height = _scatterFroxelH;
      d.DepthOrArraySize = (UINT16)SCATTER_FROXEL_D;
      d.MipLevels = 1;
      d.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
      d.Flags = flags;
      d.SampleDesc.Count = 1;
      D3D12_HEAP_PROPERTIES hp = {};
      hp.Type = D3D12_HEAP_TYPE_DEFAULT;
      dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d,
          initial, nullptr, IID_PPV_ARGS(&res));
    };
    makeVol(_scatterVolume, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    makeVol(_scatterAccumVolume, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    makeVol(_scatterHistoryVolume, D3D12_RESOURCE_FLAG_NONE,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture3D.MipLevels = 1;
    dev->CreateShaderResourceView(_scatterAccumVolume.Get(), &srvDesc,
        _cbvSrvUavHeap.GetStaticCPU(SRV_SCATTER_ACCUM));
    dev->CreateShaderResourceView(_scatterHistoryVolume.Get(), &srvDesc,
        _cbvSrvUavHeap.GetStaticCPU(SRV_SCATTER_HISTORY));
  }
  _scatterAccumIsInPSR = false;
  _scatterHistoryValid = false;

  spdlog::info("DX12GpuScene resized to {}x{}", newWidth, newHeight);
}

#endif // ENABLE_DX12
