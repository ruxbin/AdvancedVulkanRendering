#ifdef ENABLE_DX12

#include "DX12GpuScene.h"
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

// Shadow cull params - POD structs mirroring shadowcull.hlsl cbuffer layout.
// Kept here (not in header) so buildFrustumFromMatrix can access them as file-scope types.
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
struct AAPLCompressionHeader {
  uint32_t compressionMode;
  uint32_t dataSize;
  uint32_t uncompressedSize;
  uint32_t compressedSize;
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

  _mainCamera = new Camera(
      60.0f, 0.1f, 1000.0f,
      vec3(0, 2, 5),
      (float)_device.GetWidth() / _device.GetHeight(),
      vec3(0, 0, -1),
      vec3(1, 0, 0));

  // Default frame constants
  _frameConstants.sunDirection = vec3(0.3f, 1.0f, 0.5f); // reasonable default sun direction
  _frameConstants.sunColor     = vec3(1.0f, 0.95f, 0.8f);
  _frameConstants.skyColor     = vec3(0.2f, 0.3f, 0.5f);
  _frameConstants.exposure     = 1.0f;
  _frameConstants.emissiveScale     = 1.0f;
  _frameConstants.localLightIntensity = 1.0f;

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

    // Write index buffer — upload (for zeroing), GPU default (UAV), readback (CPU read)
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

    // Shadow cull resources
    _frameResources[i].shadowDrawParams = DX12Util::CreateGPUBuffer(
        _device.GetDevice(),
        (UINT64)maxShadowDraws * SHADOW_CASCADE_COUNT * sizeof(D3D12_DRAW_INDEXED_ARGUMENTS),
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
        (UINT64)maxShadowDraws * SHADOW_CASCADE_COUNT * sizeof(uint32_t),
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
  CreateCommandSignature();
  CreateLights();
  CreateLightCullPipelines();
  CreateScatterResources();

  spdlog::info("DX12GpuScene initialized");
}

DX12GpuScene::~DX12GpuScene() {
  _device.WaitForGpu();
  if (_imguiInitialized) {
    ImGui_ImplDX12_Shutdown();
  }
  delete _mainCamera;
  delete _applMesh;
}

// ---- Load Mesh Data ----
void DX12GpuScene::LoadMeshData() {
  std::string meshPath = (_rootPath / "bistro.dxt.bin").generic_string();
  _applMesh = new AAPLMeshData(meshPath.c_str());

  // Load scene file for occluder data
  std::string scenePath = (_rootPath / "bistro.dxt.bin.json").generic_string();
  std::ifstream sceneFileStream(scenePath);
  if (sceneFileStream.is_open()) {
    sceneFileStream >> _sceneFile;
  }

  spdlog::info("DX12: Loaded mesh - {} vertices, {} indices, {} chunks",
               _applMesh->_vertexCount, _applMesh->_indexCount, _applMesh->_chunkCount);
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
    free(matData);

    _materialBuffer = uploadBuffer(shaderMats.data(), shaderMats.size() * sizeof(AAPLShaderMaterial));
  }

  // --- Occluder data from scene file ---
  if (_sceneFile.contains("occluder_verts") && _sceneFile.contains("occluder_indices")) {
    auto& verts = _sceneFile["occluder_verts"];
    auto& indices = _sceneFile["occluder_indices"];

    std::vector<float> vertData;
    for (auto& v : verts) vertData.push_back(v.get<float>());
    std::vector<uint32_t> idxData;
    for (auto& i : indices) idxData.push_back(i.get<uint32_t>());

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
        dev, totalChunks * sizeof(D3D12_DRAW_INDEXED_ARGUMENTS),
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
  // MTLPixelFormat values from Apple headers
  constexpr uint32_t MTL_BC3_RGBA_sRGB = 302;
  constexpr uint32_t MTL_BC5_RGUnorm = 312;
  constexpr uint32_t MTL_BC1_RGBA_sRGB = 292;
  switch (mtlFormat) {
  case MTL_BC3_RGBA_sRGB: return DXGI_FORMAT_BC3_UNORM_SRGB;
  case MTL_BC5_RGUnorm:   return DXGI_FORMAT_BC5_UNORM;
  case MTL_BC1_RGBA_sRGB: return DXGI_FORMAT_BC1_UNORM_SRGB;
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

    // Auto-detect BC format from compressed mip 0 size before creating resource
    {
      size_t dataOffset0 = texData._mipOffsets.size() > 0 ? texData._mipOffsets[0] : 0;
      size_t dataLen0 = texData._mipLengths.size() > 0 ? texData._mipLengths[0] : 0;
      if (dataLen0 > 0) {
        uint8_t* cs = (uint8_t*)_applMesh->_textureData + texData._pixelDataOffset + dataOffset0;
        auto [mip0, mip0Size] = decompressToHeap(cs, dataLen0);
        uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4;
        if (mip0Size == bw * bh * 8)       format = DXGI_FORMAT_BC1_UNORM_SRGB;
        else if (mip0Size == bw * bh * 16) format = DXGI_FORMAT_BC3_UNORM_SRGB;
        free(mip0);
      }
    }

    // Create texture resource (now with auto-corrected format)
    auto tex = DX12Util::CreateTexture2D(dev, w, h, format,
        D3D12_RESOURCE_FLAG_NONE, mipCount, 1, D3D12_RESOURCE_STATE_COPY_DEST);

    // Upload each mip level
    for (uint32_t mip = 0; mip < mipCount; ++mip) {
      uint32_t mipW = (w >> mip) > 1 ? (w >> mip) : 1;
      uint32_t mipH = (h >> mip) > 1 ? (h >> mip) : 1;

      // Get mip data offset and length
      size_t dataOffset = texData._mipOffsets.size() > mip ? texData._mipOffsets[mip] : 0;
      size_t dataLen = texData._mipLengths.size() > mip ? texData._mipLengths[mip] : 0;
      if (dataLen == 0) continue;

      // Decompress mip data
      uint8_t* compressedSrc = (uint8_t*)_applMesh->_textureData + texData._pixelDataOffset + dataOffset;
      auto [mipData, mipSize] = decompressToHeap(compressedSrc, dataLen);

      // Calculate row pitch for BC formats (4x4 block compressed)
      uint32_t blockW = (mipW + 3) / 4;
      uint32_t blockH = (mipH + 3) / 4;
      uint32_t bytesPerBlock = (format == DXGI_FORMAT_BC1_UNORM_SRGB) ? 8 : 16;
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
      dst.SubresourceIndex = mip;

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
    srvDesc.Texture2D.MipLevels = mipCount;
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

  // Initialize texture streaming entries
  _streamEntries.resize(_textures.size());
  for (size_t k = 0; k < _textures.size(); ++k) {
    _streamEntries[k].totalMips  = (uint32_t)_textures[k]->GetDesc().MipLevels;
    _streamEntries[k].currentMip = 0;
    _streamEntries[k].requiredMip = 0;
  }
  // Allocate staging buffer (stub — actual transfer via SRV MostDetailedMip, not copy)
  _streamingStagingBuffer = DX12Util::CreateUploadBuffer(_device.GetDevice(),
      STREAMING_STAGING_SIZE, &_streamingStagingMapped);

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
  {
    D3D12_ROOT_PARAMETER params[3] = {};
    // [0] CBV for FrameData (shadow VP matrices live here)
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[0].Descriptor.ShaderRegister = 0;
    params[0].Descriptor.RegisterSpace = 0;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    // [1] Root constant: cascadeIndex (b1, space0, 1 uint32)
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[1].Constants.ShaderRegister = 1;
    params[1].Constants.RegisterSpace = 0;
    params[1].Constants.Num32BitValues = 1;
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

    D3D12_ROOT_SIGNATURE_DESC desc = {};
    desc.NumParameters = 3;
    desc.pParameters = params;
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

    // [3] Root constants (push constants)
    params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[3].Constants.ShaderRegister = 1;
    params[3].Constants.RegisterSpace = 0;
    params[3].Constants.Num32BitValues = 2;
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
  // [0] CBV b0 space0 (frameData — camera params)
  // [1] CBV b1 space0 (cullParams — light counts, screen size, VP matrix, frustum)
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
    // SRVs: t1 and t2 (pointLight, depth) and t8 (spotLight) — cover t1..t8 contiguously
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
    // s14 space0: linearSampler (trilinear clamp)
    scatterSamplers[2].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    scatterSamplers[2].AddressU = scatterSamplers[2].AddressV = scatterSamplers[2].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
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
    desc.RasterizerState.FrontCounterClockwise = TRUE;
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
    desc.RasterizerState.FrontCounterClockwise = TRUE;
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
    desc.RasterizerState.FrontCounterClockwise = TRUE;
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
    desc.RasterizerState.FrontCounterClockwise = TRUE;
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
    desc.RTVFormats[0] = _device.GetSwapChainFormat();
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
    desc.RTVFormats[0] = _device.GetSwapChainFormat();
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
  depthClear.DepthStencil.Depth = 0.0f; // reverse-Z
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
    desc.RasterizerState.FrontCounterClockwise = TRUE;
    desc.RasterizerState.DepthClipEnable = TRUE;
    desc.RasterizerState.DepthBias = 1000;
    desc.RasterizerState.DepthBiasClamp = 0.0f;
    desc.RasterizerState.SlopeScaledDepthBias = 1.0f;
    desc.DepthStencilState.DepthEnable = TRUE;
    desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER;
    desc.BlendState.RenderTarget[0].RenderTargetWriteMask = 0;
    desc.SampleMask = UINT_MAX;
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.NumRenderTargets = 0;
    desc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    desc.SampleDesc.Count = 1;

    DX12Util::ThrowIfFailed(
        dev->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&_shadowPSO)),
        "Failed to create shadow PSO");
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

  // Shadow ExecuteIndirect command signature (DrawIndexed, same stride as _drawIndexedCmdSig)
  {
    D3D12_INDIRECT_ARGUMENT_DESC argDesc = {};
    argDesc.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED;
    D3D12_COMMAND_SIGNATURE_DESC sigDesc = {};
    sigDesc.ByteStride = sizeof(D3D12_DRAW_INDEXED_ARGUMENTS);
    sigDesc.NumArgumentDescs = 1;
    sigDesc.pArgumentDescs = &argDesc;
    DX12Util::ThrowIfFailed(
        dev->CreateCommandSignature(&sigDesc, nullptr, IID_PPV_ARGS(&_shadowDrawCmdSig)),
        "Failed to create shadow draw command signature");
  }
}

// ---- Create Command Signature for ExecuteIndirect ----
void DX12GpuScene::CreateCommandSignature() {
  D3D12_INDIRECT_ARGUMENT_DESC argDesc = {};
  argDesc.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED;

  D3D12_COMMAND_SIGNATURE_DESC sigDesc = {};
  sigDesc.ByteStride = sizeof(D3D12_DRAW_INDEXED_ARGUMENTS); // 20 bytes
  sigDesc.NumArgumentDescs = 1;
  sigDesc.pArgumentDescs = &argDesc;

  DX12Util::ThrowIfFailed(
      _device.GetDevice()->CreateCommandSignature(
          &sigDesc, nullptr, IID_PPV_ARGS(&_drawIndexedCmdSig)),
      "Failed to create draw indexed command signature");
}

// ---- Flush Command Queue ----
void DX12GpuScene::FlushCommandQueue() {
  _device.WaitForGpu();
}

// ---- Update Texture Streaming (mip LOD selection) ----
void DX12GpuScene::UpdateTextureStreaming() {
  if (_streamEntries.empty()) return;

  // Distance heuristic: mip 0 when close, mip 2 at mid range, mip 4 at distance
  float camDist = _mainCamera->GetOrigin().length();
  uint32_t targetMip = (camDist > 60.0f) ? 4u : (camDist > 20.0f) ? 2u : 0u;

  // --- Pass 1: scan all entries to detect if any mip change is needed ---
  // We must NOT write descriptors into a static (shader-visible) heap while
  // earlier in-flight frames may still be sampling those same slots. Scanning
  // first lets us avoid a GPU stall when nothing changed.
  bool anyChange = false;
  for (size_t k = 0; k < _streamEntries.size(); ++k) {
    auto& e = _streamEntries[k];
    uint32_t newMip = std::min(targetMip, e.totalMips > 0 ? e.totalMips - 1 : 0u);
    e.requiredMip = newMip;
    if (e.requiredMip != e.currentMip) {
      anyChange = true;
      // Don't break — we still need to write requiredMip for every entry.
    }
  }

  if (!anyChange) return;

  // --- Stall: wait for all in-flight GPU work to finish ---
  // Only stall when we actually need to rewrite descriptor slots. With
  // triple-buffering, frames N-1 and N-2 may still reference the current
  // SRV descriptors; writing them now would be a D3D12 spec violation.
  _device.WaitForGpu();

  // --- Pass 2: rewrite SRV descriptors for every entry whose mip changed ---
  auto* dev = _device.GetDevice();
  for (size_t k = 0; k < _streamEntries.size(); ++k) {
    auto& e = _streamEntries[k];
    if (e.requiredMip == e.currentMip) continue;

    auto texDesc = _textures[k]->GetDesc();
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = texDesc.Format;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MostDetailedMip = e.requiredMip;
    srvDesc.Texture2D.MipLevels = texDesc.MipLevels - e.requiredMip;

    dev->CreateShaderResourceView(_textures[k].Get(), &srvDesc,
        _cbvSrvUavHeap.GetStaticCPU(SRV_BINDLESS_START + (uint32_t)k));

    e.currentMip = e.requiredMip;
  }
}

// ---- Update Uniforms ----
void DX12GpuScene::UpdateUniforms() {
  auto& frame = _frameResources[_currentFrame];

  _frameConstants.nearPlane = _mainCamera->Near();
  _frameConstants.farPlane = _mainCamera->Far();
  static uint32_t sFrameCounter = 0;
  _frameConstants.frameCounter = sFrameCounter++;
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
      // Build shadow view matrix (look from sun direction)
      vec3 eye = center + sunDir * range;
      vec3 z = normalize(sunDir * -1.0f); // looking toward -sunDir
      vec3 x = normalize(vec3(0, 1, 0).cross(z));
      vec3 y = z.cross(x);
      mat4 view(1.0f);
      view[0] = vec4(x.x, y.x, z.x, 0);
      view[1] = vec4(x.y, y.y, z.y, 0);
      view[2] = vec4(x.z, y.z, z.z, 0);
      view[3] = vec4(-x.dot(eye), -y.dot(eye), -z.dot(eye), 1);
      _shadowViewMatrices[i] = view;
      _shadowProjectionMatrices[i] = orthographic(range * 2.0f, range * 2.0f,
          0.1f, range * 4.0f, 0, 0);
    }
    frameData.camConstants.shadowProjectionMatrix0 = transpose(_shadowProjectionMatrices[0]);
    frameData.camConstants.shadowViewMatrix0 = transpose(_shadowViewMatrices[0]);
    frameData.camConstants.shadowProjectionMatrix1 = transpose(_shadowProjectionMatrices[1]);
    frameData.camConstants.shadowViewMatrix1 = transpose(_shadowViewMatrices[1]);
    frameData.camConstants.shadowProjectionMatrix2 = transpose(_shadowProjectionMatrices[2]);
    frameData.camConstants.shadowViewMatrix2 = transpose(_shadowViewMatrices[2]);
  }
  frameData.camConstants.projectionMatrix = transpose(_mainCamera->getProjectMatrix());
  frameData.camConstants.viewMatrix = transpose(_mainCamera->getObjectToCamera());
  frameData.camConstants.invViewMatrix = transpose(_mainCamera->getInvViewMatrix());
  frameData.camConstants.invViewProjectionMatrix = transpose(_mainCamera->getInvViewProjectionMatrix());
  frameData.camConstants.invProjectionMatrix = transpose(inverse(_mainCamera->getProjectMatrix()));

  // prevViewProjectionMatrix for TAA reprojection:
  // With mat4 quirk (A*B = math B*A), view*proj gives the mathematical VP.
  // Must be transposed before uploading (HLSL row-major convention).
  {
    static mat4 sPrevVP = mat4(1.0f);
    frameData.camConstants.prevViewProjectionMatrix = transpose(sPrevVP);
    // Save current VP for next frame: view * proj = mathematical VP (due to quirk)
    mat4 curVP = _mainCamera->getObjectToCamera() * _mainCamera->getProjectMatrix();
    sPrevVP = curVP;
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

  frameData.frameConstants = _frameConstants;

  memcpy(frame.uniformMapped, &frameData, sizeof(FrameData));
}

// ---- Create Lights (place default point lights + alloc GPU buffers) ----
void DX12GpuScene::CreateLights() {
  auto* dev = _device.GetDevice();
  auto* cmdList = _device.GetCommandList();

  // Place a small grid of point lights around the scene
  float positions[][3] = {{2,1,0},{-2,1,0},{0,1,3},{0,1,-3}};
  float colors[][3] = {{1,.8f,.6f},{.6f,.8f,1.f},{1,1,.8f},{.8f,1,.9f}};
  for (int k = 0; k < 4; ++k) {
    AAPLPointLightCullingData pl;
    pl.posRadius = vec4(positions[k][0], positions[k][1], positions[k][2], 5.0f);
    pl.color = vec4(colors[k][0]*3.f, colors[k][1]*3.f, colors[k][2]*3.f, 0.f);
    _pointLights.push_back(pl);
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

  // Light index buffers: [count, idx0, idx1, ...] per tile × MAX_LIGHTS_PER_TILE
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

  spdlog::info("DX12: CreateLights — {} point lights, tileGrid {}x{}", maxPL, tileW, tileH);
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
}

// ---- Create Scatter Volume Resources ----
void DX12GpuScene::CreateScatterResources() {
  if (!_scatterRootSig || !_accumulatePSO) {
    spdlog::warn("DX12: Scatter PSOs/RootSig not ready — skipping CreateScatterResources");
    return;
  }
  auto* dev = _device.GetDevice();

  auto makeVol = [&](ComPtr<ID3D12Resource>& res, const char* name) {
    D3D12_RESOURCE_DESC d = {};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
    d.Width = SCATTER_FROXEL_W;
    d.Height = SCATTER_FROXEL_H;
    d.DepthOrArraySize = (UINT16)SCATTER_FROXEL_D;
    d.MipLevels = 1;
    d.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    HRESULT hr = dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&res));
    if (FAILED(hr)) {
      spdlog::error("DX12: Failed to create scatter volume resource: {}", name);
    } else {
      spdlog::info("DX12: Created scatter volume resource: {} ({}x{}x{})", name,
                   SCATTER_FROXEL_W, SCATTER_FROXEL_H, SCATTER_FROXEL_D);
    }
  };
  makeVol(_scatterVolume,     "scatterVolume");
  makeVol(_scatterAccumVolume, "scatterAccumVolume");

  // Create static SRV for scatterAccumVolume at SRV_SCATTER_ACCUM
  D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
  srvDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
  srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
  srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  srvDesc.Texture3D.MipLevels = 1;
  dev->CreateShaderResourceView(_scatterAccumVolume.Get(), &srvDesc,
      _cbvSrvUavHeap.GetStaticCPU(SRV_SCATTER_ACCUM));

  _scatterAccumIsInPSR = false;
  spdlog::info("DX12: Scatter resources created (froxel: {}x{}x{})",
               SCATTER_FROXEL_W, SCATTER_FROXEL_H, SCATTER_FROXEL_D);
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
    // Frustum: 6 planes × (vec3 normal + float w) = 6 × 16 = 96 bytes
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
  // mat4 quirk: A*B = B*A, same as GPUCull path
  mat4 vp = transpose(_mainCamera->getProjectMatrix() * _mainCamera->getObjectToCamera());
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
  //          slot11=u6 null (tradtionDebug Texture2D — skip)
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
  // UAV u5: lightIndices
  {
    D3D12_UNORDERED_ACCESS_VIEW_DESC d = {};
    d.Format = DXGI_FORMAT_R32_UINT;
    d.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    d.Buffer.NumElements = (UINT)(tileW * tileH * MAX_LIGHTS_PER_TILE_LC);
    dev->CreateUnorderedAccessView(_lightIndicesBuffer.Get(), nullptr, &d, {lcDesc.cpu.ptr + 10 * ds});
  }
  // UAV u6: tradtionDebug — null Texture2D (RWTexture2D<float4> in shader, unused/commented)
  {
    D3D12_UNORDERED_ACCESS_VIEW_DESC nd = {};
    nd.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    nd.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    dev->CreateUnorderedAccessView(nullptr, nullptr, &nd, {lcDesc.cpu.ptr + 11 * ds});
  }
  // UAV u7: lightIndicesTransparent
  {
    D3D12_UNORDERED_ACCESS_VIEW_DESC d = {};
    d.Format = DXGI_FORMAT_R32_UINT;
    d.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    d.Buffer.NumElements = (UINT)(tileW * tileH * MAX_LIGHTS_PER_TILE_LC);
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
    d.Format = DXGI_FORMAT_R32_UINT;
    d.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    d.Buffer.NumElements = (UINT)(tileW * tileH * MAX_LIGHTS_PER_TILE_LC);
    dev->CreateUnorderedAccessView(_spotLightIndicesBuffer.Get(), nullptr, &d, {lcDesc.cpu.ptr + 15 * ds});
  }
  // UAV u11: spotLightIndicesTransparent
  {
    D3D12_UNORDERED_ACCESS_VIEW_DESC d = {};
    d.Format = DXGI_FORMAT_R32_UINT;
    d.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    d.Buffer.NumElements = (UINT)(tileW * tileH * MAX_LIGHTS_PER_TILE_LC);
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

  // --- TraditionalCull: tileW × tileH thread groups (16×16 each) ---
  if (_traditionalCullPSO) {
    cmdList->SetPipelineState(_traditionalCullPSO.Get());
    cmdList->Dispatch(tileW, tileH, 1);
    DX12Util::UAVBarrier(cmdList, _lightIndicesBuffer.Get());
  }

  // --- Transition lightIndices to SRV for deferred lighting ---
  DX12Util::TransitionBarrier(cmdList, _lightIndicesBuffer.Get(),
      D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_GENERIC_READ);
  DX12Util::TransitionBarrier(cmdList, _lightXZRangeBuffer.Get(),
      D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
  DX12Util::TransitionBarrier(cmdList, _lightIndicesTransparentBuffer.Get(),
      D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
  DX12Util::TransitionBarrier(cmdList, _lightDebugTexture.Get(),
      D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
  DX12Util::TransitionBarrier(cmdList, _spotXZRangeBuffer.Get(),
      D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
  DX12Util::TransitionBarrier(cmdList, _spotLightIndicesBuffer.Get(),
      D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
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

// Helper: extract frustum planes from a combined shadow VP matrix.
// Pass: vp = _shadowProjectionMatrices[c] * _shadowViewMatrices[c]
// (with the mat4 quirk, A*B = B*A, this equals view_cpp * proj_cpp_actual)
// The HLSL clip matrix is transpose(vp), so Gribb-Hartmann plane extraction
// uses columns of vp as the effective rows of the HLSL matrix.
static ShadowFrustum buildFrustumFromMatrix(const mat4& vp) {
  // Columns of vp == rows of HLSL VP matrix
  auto col = [&](int ci) -> vec4 {
    return vp[ci]; // mat4::operator[] returns the ci-th column vec4
  };
  vec4 c0 = col(0), c1 = col(1), c2 = col(2), c3 = col(3);

  auto makePlane = [](vec4 p) -> ShadowPlane {
    float len = sqrtf(p.x * p.x + p.y * p.y + p.z * p.z);
    if (len < 1e-8f) len = 1.0f;
    ShadowPlane pl;
    pl.normal = vec3(p.x, p.y, p.z) / len;
    pl.w = p.w / len;
    return pl;
  };

  ShadowFrustum f;
  // Gribb-Hartmann: left, right, bottom, top, near, far
  // Using columns of C++ matrix (= rows of HLSL row-major VP)
  f.borders[0] = makePlane(c3 + c0); // left
  f.borders[1] = makePlane(c3 - c0); // right
  f.borders[2] = makePlane(c3 + c1); // bottom
  f.borders[3] = makePlane(c3 - c1); // top
  f.borders[4] = makePlane(c3 + c2); // near
  f.borders[5] = makePlane(c3 - c2); // far
  return f;
}

// ---- Draw (main frame) ----
void DX12GpuScene::Draw() {
  _currentFrame = _device.GetFrameIndex();

  UpdateUniforms();
  ReadbackCullingStats();
  UpdateTextureStreaming();  // update SRV MostDetailedMip before GPU work begins

  _device.BeginFrame();
  _cbvSrvUavHeap.ResetFrame();

  auto* cmdList = _device.GetCommandList();
  auto rtvHandle = _device.GetRTV(_device.GetFrameIndex());
  auto dsvHandle = _device.GetDSV();

  // Set descriptor heaps
  ID3D12DescriptorHeap* heaps[] = { _cbvSrvUavHeap.GetHeap(), _samplerHeap.GetHeap() };
  cmdList->SetDescriptorHeaps(2, heaps);

  // Set viewport and scissor
  D3D12_VIEWPORT viewport = {};
  viewport.Width = (float)_device.GetWidth();
  viewport.Height = (float)_device.GetHeight();
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
      mat4 shadowVP = _shadowProjectionMatrices[c] * _shadowViewMatrices[c];
      scp.cascadeFrustum[c] = buildFrustumFromMatrix(shadowVP);
    }
    memcpy(fr.shadowCullParamsMapped, &scp, sizeof(ShadowCullParams));

    // --- Allocate 8 descriptors: u0..u4 (5 UAVs) + t2..t4 (3 SRVs) ---
    auto cullDesc = _cbvSrvUavHeap.AllocateDynamic(8);
    // u0: shadowDrawParams
    {
      D3D12_UNORDERED_ACCESS_VIEW_DESC d = {};
      d.Format = DXGI_FORMAT_UNKNOWN;
      d.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
      d.Buffer.NumElements = totalShadowChunks * SHADOW_CASCADE_COUNT;
      d.Buffer.StructureByteStride = sizeof(D3D12_DRAW_INDEXED_ARGUMENTS);
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
      d.Buffer.NumElements = totalShadowChunks * SHADOW_CASCADE_COUNT;
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

    // --- Per-cascade shadow draw via ExecuteIndirect ---
    uint32_t dsvSize = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    D3D12_VIEWPORT shadowViewport = {0, 0, (float)SHADOW_MAP_SIZE, (float)SHADOW_MAP_SIZE, 0.0f, 1.0f};
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
      cmdList->ClearDepthStencilView(cascadeDsv, D3D12_CLEAR_FLAG_DEPTH, 0.0f, 0, 0, nullptr);
      cmdList->OMSetRenderTargets(0, nullptr, FALSE, &cascadeDsv);
      cmdList->RSSetViewports(1, &shadowViewport);
      cmdList->RSSetScissorRects(1, &shadowScissor);

      cmdList->SetPipelineState(_shadowPSO.Get());
      cmdList->SetGraphicsRootSignature(_shadowRootSig.Get());
      cmdList->SetGraphicsRootConstantBufferView(0,
          fr.uniformBuffer->GetGPUVirtualAddress());
      cmdList->SetGraphicsRoot32BitConstants(1, 1, &cascade, 0);

      // Bind chunkIndex SRV (t4,space1) for shadow VS instancing
      {
        auto ciDesc = _cbvSrvUavHeap.AllocateDynamic(1);
        D3D12_SHADER_RESOURCE_VIEW_DESC ciSrv = {};
        ciSrv.Format = DXGI_FORMAT_R32_UINT;
        ciSrv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        ciSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        ciSrv.Buffer.NumElements = totalShadowChunks * SHADOW_CASCADE_COUNT;
        _device.GetDevice()->CreateShaderResourceView(fr.shadowChunkIndicesBuffer.Get(), &ciSrv, ciDesc.cpu);
        cmdList->SetGraphicsRootDescriptorTable(2, ciDesc.gpu);
      }

      cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
      cmdList->IASetVertexBuffers(0, 4, vbvs);
      cmdList->IASetIndexBuffer(&ibv);

      uint32_t cascadeBase = cascade * totalShadowChunks;
      uint32_t writeIdxOffset = cascade * 2 * sizeof(uint32_t);

      // Opaque draws
      cmdList->ExecuteIndirect(_shadowDrawCmdSig.Get(),
          (uint32_t)_applMesh->_opaqueChunkCount,
          fr.shadowDrawParams.Get(),
          (UINT64)cascadeBase * sizeof(D3D12_DRAW_INDEXED_ARGUMENTS),
          fr.shadowWriteIndex.Get(), writeIdxOffset);

      // Alpha-masked draws
      cmdList->ExecuteIndirect(_shadowDrawCmdSig.Get(),
          (uint32_t)_applMesh->_alphaMaskedChunkCount,
          fr.shadowDrawParams.Get(),
          (UINT64)(cascadeBase + _applMesh->_opaqueChunkCount) * sizeof(D3D12_DRAW_INDEXED_ARGUMENTS),
          fr.shadowWriteIndex.Get(), writeIdxOffset + sizeof(uint32_t));
    }

    // --- Restore resource states ---
    DX12Util::TransitionBarrier(cmdList, fr.shadowDrawParams.Get(),
        D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT, D3D12_RESOURCE_STATE_COMMON);
    DX12Util::TransitionBarrier(cmdList, fr.shadowWriteIndex.Get(),
        D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT, D3D12_RESOURCE_STATE_COMMON);
    DX12Util::TransitionBarrier(cmdList, fr.shadowChunkIndicesBuffer.Get(),
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);

    // Transition shadow maps to SRV for deferred lighting
    DX12Util::TransitionBarrier(cmdList, _shadowMapArray.Get(),
        D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    cmdList->RSSetViewports(1, &viewport);
    cmdList->RSSetScissorRects(1, &scissor);
  }

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

    // Copy: occluder depth → HiZ mip 0
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
    mat4 vp = transpose(_mainCamera->getProjectMatrix() * _mainCamera->getObjectToCamera());
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
      uavDesc.Buffer.StructureByteStride = sizeof(D3D12_DRAW_INDEXED_ARGUMENTS);
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

    // Barrier: compute UAV writes → indirect draw read
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
        fr.drawParamsBuffer.Get(), opaqueCount * sizeof(D3D12_DRAW_INDEXED_ARGUMENTS),
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

    // Copy window depth → SAO pyramid mip 0
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
  if (_scatterVolumePSO && _accumulatePSO && _scatterRootSig && _accumRootSig
      && _scatterVolume && _scatterAccumVolume
      && _frameConstants.scatterScale > 0.0f) {
    auto* dev = _device.GetDevice();
    uint32_t w = _device.GetWidth(), h = _device.GetHeight();

    // PushConstants layout: volumeWidth, volumeHeight, screenWidth, screenHeight, resetHistory
    float sw = (float)w, sh = (float)h;
    uint32_t pcData[5] = {
      SCATTER_FROXEL_W, SCATTER_FROXEL_H,
      *reinterpret_cast<const uint32_t*>(&sw),
      *reinterpret_cast<const uint32_t*>(&sh),
      (!_scatterAccumIsInPSR) ? 1u : 0u  // resetHistory = 1 if no valid history (first frame or gap)
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
    //   slot2 = t3 (null — s3 is static sampler shadowSampler)
    //   slot3 = t4 scatterPrev SRV (or null if first frame)
    //   slot4 = t5 blueNoiseTex SRV (null — no blue noise tex)
    //   slot5 = t6 pointLightData SRV
    //   slot6 = t7 pointLightIndices SRV
    //   slot7 = t8 spotLightData SRV
    //   slot8 = t9 spotLightIndices SRV
    //   slot9 = t10 spotShadowMaps SRV (null)
    //   slot10 = t11 (null — s11 is static sampler spotShadowSampler)
    //   slot11 = t12 spotViewProjMatrices SRV (null)
    //   slot12 = t13 perlinNoiseTex SRV (null)
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

      // slot2: t3 (shadowSampler static — fill with null SRV to satisfy range)
      {
        D3D12_SHADER_RESOURCE_VIEW_DESC nd = {};
        nd.Format = DXGI_FORMAT_R8_UNORM;
        nd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        nd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        nd.Texture2D.MipLevels = 1;
        dev->CreateShaderResourceView(nullptr, &nd, {scDesc.cpu.ptr + 2u * ds});
      }

      // slot3: t4 scatterPrev SRV (use scatterAccumVolume as history, or null on first frame / gap)
      if (!_scatterAccumIsInPSR) {
        // No valid history — bind null 3D SRV
        D3D12_SHADER_RESOURCE_VIEW_DESC nd = {};
        nd.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        nd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
        nd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        nd.Texture3D.MipLevels = 1;
        dev->CreateShaderResourceView(nullptr, &nd, {scDesc.cpu.ptr + 3u * ds});
      } else {
        // scatterAccumVolume is in PIXEL_SHADER_RESOURCE — transition to NPSR for compute read
        DX12Util::TransitionBarrier(cmdList, _scatterAccumVolume.Get(),
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        dev->CopyDescriptorsSimple(1, {scDesc.cpu.ptr + 3u * ds},
            _cbvSrvUavHeap.GetStaticCPU(SRV_SCATTER_ACCUM),
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
      }

      // slots 4-12: t5..t13 — null SRVs for blueNoise, lights, spot shadows, perlinNoise
      // (no real data available yet for these; light data would come from light cull buffers)
      // Bind point light data where available
      for (int k = 4; k <= 12; ++k) {
        D3D12_SHADER_RESOURCE_VIEW_DESC nd = {};
        nd.Format = DXGI_FORMAT_R32_FLOAT;
        nd.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        nd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        dev->CreateShaderResourceView(nullptr, &nd, {scDesc.cpu.ptr + (UINT64)k * ds});
      }
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
      // slot6 (t7): pointLightIndices (from light cull — transition to read)
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

      cmdList->SetComputeRootDescriptorTable(2, scDesc.gpu);
    }

    cmdList->Dispatch(
        (SCATTER_FROXEL_W + 7) / 8,
        (SCATTER_FROXEL_H + 7) / 8,
        SCATTER_FROXEL_D);
    DX12Util::UAVBarrier(cmdList, _scatterVolume.Get());

    // Transition scatterAccumVolume to UAV for the accumulate write
    // (if history was used, it's in NPSR; if no history, it's still UAV)
    if (_scatterAccumIsInPSR) {
      DX12Util::TransitionBarrier(cmdList, _scatterAccumVolume.Get(),
          D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
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
      // Transition scatterVolume UAV → SRV
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
        (SCATTER_FROXEL_W + 7) / 8,
        (SCATTER_FROXEL_H + 7) / 8, 1);
    DX12Util::UAVBarrier(cmdList, _scatterAccumVolume.Get());

    // Transition scatterVolume back to UAV for next frame
    DX12Util::TransitionBarrier(cmdList, _scatterVolume.Get(),
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    // Transition scatterAccumVolume → PIXEL_SHADER_RESOURCE for deferred lighting
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

    // Output directly to swapchain
    cmdList->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);
    cmdList->SetPipelineState(_deferredLightingPSO.Get());
    cmdList->SetGraphicsRootSignature(_deferredLightingRootSig.Get());
    cmdList->SetGraphicsRootConstantBufferView(0,
        _frameResources[_currentFrame].uniformBuffer->GetGPUVirtualAddress());
    cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    // Use static heap binding for deferred lighting (simplified path)
    cmdList->SetGraphicsRootDescriptorTable(1, _cbvSrvUavHeap.GetStaticGPU(SRV_GBUFFER_START));

    // Draw fullscreen triangle (3 vertices, no VB)
    cmdList->DrawInstanced(3, 1, 0, 0);

    // === Resolve: HDR + TAA history DISABLED ===
    // Transition window depth back to write
    DX12Util::TransitionBarrier(cmdList, _device.GetDepthStencilBuffer(),
        D3D12_RESOURCE_STATE_DEPTH_READ, D3D12_RESOURCE_STATE_DEPTH_WRITE);

    // Restore lightIndices back to COMMON for next frame
    if (_lightIndicesBuffer && !_pointLights.empty()) {
      DX12Util::TransitionBarrier(cmdList, _lightIndicesBuffer.Get(),
          D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_COMMON);
    }
  }

  // === Copy writeIndex GPU → readback buffer for CPU stats ===
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
      // Render transparent objects onto swap chain with alpha blending
      cmdList->OMSetRenderTargets(1, &rtvHandle, FALSE, &dsvHandle);
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
          fr.drawParamsBuffer.Get(), transpOffset * sizeof(D3D12_DRAW_INDEXED_ARGUMENTS),
          fr.writeIndexBuffer.Get(), 2 * sizeof(uint32_t));

      DX12Util::TransitionBarrier(cmdList, fr.drawParamsBuffer.Get(),
          D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT, D3D12_RESOURCE_STATE_COMMON);
      DX12Util::TransitionBarrier(cmdList, fr.writeIndexBuffer.Get(),
          D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT, D3D12_RESOURCE_STATE_COMMON);
    }
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
  // back to UAV so it starts in a known state when scatter is re-enabled.
  if (_scatterAccumIsInPSR && _frameConstants.scatterScale <= 0.0f && _scatterAccumVolume) {
    DX12Util::TransitionBarrier(cmdList, _scatterAccumVolume.Get(),
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    _scatterAccumIsInPSR = false;
  }

  // ImGui overlay
  RenderImGuiOverlay();

  _device.EndFrameAndPresent();
}

// ---- ImGui ----
void DX12GpuScene::InitImGui(SDL_Window* window) {
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
  ImGui::StyleColorsDark();

  ImGui_ImplSDL2_InitForD3D(window);

  // Reserve a descriptor for ImGui font texture
  auto fontDesc = _cbvSrvUavHeap.GetStaticCPU(0);
  auto fontDescGPU = _cbvSrvUavHeap.GetStaticGPU(0);

  ImGui_ImplDX12_Init(_device.GetDevice(), _device.GetFrameCount(),
                       _device.GetSwapChainFormat(),
                       _cbvSrvUavHeap.GetHeap(),
                       fontDesc, fontDescGPU);

  // Build font atlas on CPU side — required by legacy Init path which
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

void DX12GpuScene::RenderImGuiOverlay() {
  if (!_imguiInitialized) return;

  ImGui_ImplDX12_NewFrame();
  ImGui_ImplSDL2_NewFrame();
  ImGui::NewFrame();

  uint32_t totalChunks = _cullingStats.totalOpaque + _cullingStats.totalAlphaMask + _cullingStats.totalTransparent;
  uint32_t totalVisible = _cullingStats.visibleOpaque + _cullingStats.visibleAlphaMask + _cullingStats.visibleTransparent;
  uint32_t culled = totalChunks > totalVisible ? totalChunks - totalVisible : 0;
  float pct = totalChunks > 0 ? 100.0f * culled / totalChunks : 0.0f;

  ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_Always);
  ImGui::SetNextWindowBgAlpha(0.5f);
  ImGui::Begin("DX12 Culling Stats", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
               ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
               ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove);

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
  ImGui::SliderFloat("Scatter", &_frameConstants.scatterScale, 0.0f, 1.0f);

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
  }

  ImGui::End();

  ImGui::Render();
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

  // Release light-tile resources (sized by tileW × tileH)
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

  // Reset scatter accumulation state.
  // NOTE: froxel volumes (scatter) are intentionally NOT recreated on resize
  // because SCATTER_FROXEL_W, SCATTER_FROXEL_H, and SCATTER_FROXEL_D are
  // compile-time constants independent of screen resolution.
  _scatterAccumIsInPSR = false;

  spdlog::info("DX12GpuScene resized to {}x{}", newWidth, newHeight);
}

#endif // ENABLE_DX12
