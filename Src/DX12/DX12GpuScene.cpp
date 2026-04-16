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

  // Init descriptor heaps
  _cbvSrvUavHeap.Init(_device.GetDevice(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
                       2048, 4096, true);
  _samplerHeap.Init(_device.GetDevice(), D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER,
                     16, 0, true);

  // Create per-frame resources
  _frameResources.resize(_device.GetFrameCount());
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
  }

  LoadMeshData();
  CreateBuffers();
  CreateTextures();
  CreateRootSignatures();
  CreatePipelineStates();
  CreateGBuffers();
  CreateStaticDescriptors();
  CreateHiZResources();
  CreateShadowResources();
  CreateCommandSignature();

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
  std::string meshPath = (_rootPath / "debug1.bin").generic_string();
  _applMesh = new AAPLMeshData(meshPath.c_str());

  // Load scene file for occluder data
  std::string scenePath = (_rootPath / "debug1.bin.json").generic_string();
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

    // Create texture resource
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

  // 2. DrawCluster root signature:
  //   [0] CBV b0 space0 (FrameData) - vertex
  //   [1] Descriptor table space1 (materials SRV, sampler, textures[], meshChunks, chunkIndex) - pixel
  //   [2] Root constants b1 (push constants: materialIndex) - pixel
  {
    D3D12_ROOT_PARAMETER params[3] = {};

    // [0] CBV
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[0].Descriptor.ShaderRegister = 0;
    params[0].Descriptor.RegisterSpace = 0;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    // [1] Descriptor table: materials(t0,space1), meshChunks(t3,space1), chunkIndex(t4,space1)
    D3D12_DESCRIPTOR_RANGE ranges[2] = {};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = 5; // t0-t4 in space1
    ranges[0].BaseShaderRegister = 0;
    ranges[0].RegisterSpace = 1;
    ranges[0].OffsetInDescriptorsFromTableStart = 0;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[1].NumDescriptors = UINT_MAX; // unbounded textures
    ranges[1].BaseShaderRegister = 0;
    ranges[1].RegisterSpace = 2;
    ranges[1].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    // Note: Vulkan uses bindless in space1 binding2, DX12 uses space2

    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 2;
    params[1].DescriptorTable.pDescriptorRanges = ranges;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    // [2] Root constants (push constants)
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[2].Constants.ShaderRegister = 1;
    params[2].Constants.RegisterSpace = 0;
    params[2].Constants.Num32BitValues = 2;
    params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

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
    desc.NumParameters = 3;
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
    D3D12_DESCRIPTOR_RANGE ranges[3] = {};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[0].NumDescriptors = 5; // u0-u4
    ranges[0].BaseShaderRegister = 0;
    ranges[0].RegisterSpace = 0;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[1].NumDescriptors = 3; // t2, t6 (meshChunks, hizTexture)
    ranges[1].BaseShaderRegister = 2;
    ranges[1].RegisterSpace = 0;
    ranges[1].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    ranges[2].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
    ranges[2].NumDescriptors = 1;
    ranges[2].BaseShaderRegister = 7;
    ranges[2].RegisterSpace = 0;
    ranges[2].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 2; // UAV + SRV only (sampler via static)
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

    // [1] Descriptor table: SRVs (G-buffers, depth, shadow, AO)
    D3D12_DESCRIPTOR_RANGE range = {};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 11; // t0-t10 in space1
    range.BaseShaderRegister = 0;
    range.RegisterSpace = 1;

    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 1;
    params[1].DescriptorTable.pDescriptorRanges = &range;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC samplers[2] = {};
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

    D3D12_ROOT_SIGNATURE_DESC desc = {};
    desc.NumParameters = 2;
    desc.pParameters = params;
    desc.NumStaticSamplers = 2;
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
    srvDesc.Buffer.StructureByteStride = 80; // sizeof(AAPLMeshChunk) in shader (must match)
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

  // Shadow depth PSO (reuses occluder root sig, occluder layout, depth-only)
  {
    auto vs = DX12Util::ReadShaderFile((_rootPath / "shaders/drawclusterShadow.vs.cso").generic_string());
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};
    desc.pRootSignature = _occluderRootSig.Get(); // just needs CBV for VP matrix
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
  frameData.frameConstants = _frameConstants;

  memcpy(frame.uniformMapped, &frameData, sizeof(FrameData));
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

// ---- Draw (main frame) ----
void DX12GpuScene::Draw() {
  _currentFrame = _device.GetFrameIndex();

  UpdateUniforms();
  ReadbackCullingStats();

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

  // === Shadow Pass (3 cascades) ===
  if (_shadowPSO && _vertexBuffer && _shadowMapArray) {
    uint32_t dsvSize = _device.GetDevice()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);

    D3D12_VIEWPORT shadowViewport = {};
    shadowViewport.Width = (float)SHADOW_MAP_SIZE;
    shadowViewport.Height = (float)SHADOW_MAP_SIZE;
    shadowViewport.MinDepth = 0.0f;
    shadowViewport.MaxDepth = 1.0f;
    D3D12_RECT shadowScissor = {};
    shadowScissor.right = SHADOW_MAP_SIZE;
    shadowScissor.bottom = SHADOW_MAP_SIZE;

    // Bind mesh VBs/IB
    D3D12_VERTEX_BUFFER_VIEW vbvs[4] = {};
    ID3D12Resource* vbs[] = {_vertexBuffer.Get(), _normalBuffer.Get(), _tangentBuffer.Get(), _uvBuffer.Get()};
    uint32_t strides[] = {sizeof(float)*3, sizeof(float)*3, sizeof(float)*3, sizeof(float)*2};
    for (int i = 0; i < 4; ++i) {
      vbvs[i].BufferLocation = vbs[i]->GetGPUVirtualAddress();
      vbvs[i].SizeInBytes = (UINT)vbs[i]->GetDesc().Width;
      vbvs[i].StrideInBytes = strides[i];
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
      cmdList->SetGraphicsRootSignature(_occluderRootSig.Get());
      // Bind uniform buffer which contains shadow VP matrices
      cmdList->SetGraphicsRootConstantBufferView(0,
          _frameResources[_currentFrame].uniformBuffer->GetGPUVirtualAddress());
      cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
      cmdList->IASetVertexBuffers(0, 4, vbvs);
      cmdList->IASetIndexBuffer(&ibv);

      // Draw all opaque chunks for this cascade (CPU-driven for shadow)
      // TODO: Use shadow cull compute + ExecuteIndirect for shadows
      // For now draw all opaque chunks directly
      uint32_t opaqueCount = (uint32_t)_applMesh->_opaqueChunkCount;
      cmdList->DrawIndexedInstanced(
          (UINT)(_applMesh->_indexCount), 1, 0, 0, 0);
    }

    // Transition shadow maps to SRV for deferred lighting
    DX12Util::TransitionBarrier(cmdList, _shadowMapArray.Get(),
        D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    // Reset viewport/scissor to main
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
      srvDesc.Buffer.StructureByteStride = 80;
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

    // Bind descriptor table [1] for materials/meshChunks
    cmdList->SetGraphicsRootDescriptorTable(1, _cbvSrvUavHeap.GetStaticGPU(SRV_MATERIALS));

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

  // === Deferred Lighting (full-screen triangle into swap chain) ===
  {
    // Transition window depth to SRV for deferred lighting read
    DX12Util::TransitionBarrier(cmdList, _device.GetDepthStencilBuffer(),
        D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_DEPTH_READ);

    cmdList->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);
    cmdList->SetPipelineState(_deferredLightingPSO.Get());
    cmdList->SetGraphicsRootSignature(_deferredLightingRootSig.Get());
    cmdList->SetGraphicsRootConstantBufferView(0,
        _frameResources[_currentFrame].uniformBuffer->GetGPUVirtualAddress());
    cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    // Bind G-buffer SRVs at root param [1] — static descriptors [1..6]
    // The deferred lighting shader expects SRVs t0-t10 in space1
    // We map: static[1]=albedo(t0), [2]=normal(t1), [3]=emissive(t2), [4]=F0R(t3), [5]=depth(t4), [6]=AO(t6→mapped to t10)
    // For now bind starting from SRV_GBUFFER_START which has contiguous G-buffer+depth+AO
    cmdList->SetGraphicsRootDescriptorTable(1, _cbvSrvUavHeap.GetStaticGPU(SRV_GBUFFER_START));

    // Draw fullscreen triangle (3 vertices, no VB)
    cmdList->DrawInstanced(3, 1, 0, 0);

    // Transition window depth back to write
    DX12Util::TransitionBarrier(cmdList, _device.GetDepthStencilBuffer(),
        D3D12_RESOURCE_STATE_DEPTH_READ, D3D12_RESOURCE_STATE_DEPTH_WRITE);
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

      cmdList->SetGraphicsRootDescriptorTable(1, _cbvSrvUavHeap.GetStaticGPU(SRV_MATERIALS));

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

  ImGui::End();

  ImGui::Render();
  ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), _device.GetCommandList());
}

#endif // ENABLE_DX12
