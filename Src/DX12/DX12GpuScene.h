#pragma once
#ifdef ENABLE_DX12

#include "DX12Setup.h"
#include "DX12DescriptorHeap.h"
#include "DX12ResourceHelper.h"
#include "Common.h"
#include "Camera.h"
#include "nlohmann/json.hpp"

#include <d3d12.h>
#include <wrl/client.h>
#include <filesystem>
#include <vector>
#include <unordered_map>

using Microsoft::WRL::ComPtr;

struct AAPLMeshData;

class DX12GpuScene {
public:
  DX12GpuScene(std::filesystem::path& root, DX12Device& device);
  ~DX12GpuScene();
  DX12GpuScene() = delete;
  DX12GpuScene(const DX12GpuScene&) = delete;

  void Draw();
  void InitImGui(struct SDL_Window* window);
  void ProcessImGuiEvent(union SDL_Event* event);

  Camera* GetMainCamera() { return _mainCamera; }

private:
  DX12Device& _device;
  std::filesystem::path _rootPath;
  Camera* _mainCamera = nullptr;

  // Mesh data
  AAPLMeshData* _applMesh = nullptr;

  // Vertex/Index buffers
  ComPtr<ID3D12Resource> _vertexBuffer;
  ComPtr<ID3D12Resource> _normalBuffer;
  ComPtr<ID3D12Resource> _tangentBuffer;
  ComPtr<ID3D12Resource> _uvBuffer;
  ComPtr<ID3D12Resource> _indexBuffer;

  // Mesh chunks + materials
  ComPtr<ID3D12Resource> _meshChunksBuffer;
  ComPtr<ID3D12Resource> _materialBuffer;

  // Per-frame uniform buffers (upload heap, persistently mapped)
  struct PerFrameResources {
    ComPtr<ID3D12Resource> uniformBuffer;
    void* uniformMapped = nullptr;
    ComPtr<ID3D12Resource> drawParamsBuffer;      // indirect draw args (default heap, UAV)
    ComPtr<ID3D12Resource> writeIndexBuffer;       // visibility counters (default heap, UAV)
    ComPtr<ID3D12Resource> writeIndexUpload;       // for zeroing writeIndex (upload heap)
    void* writeIndexUploadMapped = nullptr;
    ComPtr<ID3D12Resource> writeIndexReadback;     // for CPU readback (readback heap)
    ComPtr<ID3D12Resource> chunkIndicesBuffer;     // visible chunk indices (default heap, UAV)
    ComPtr<ID3D12Resource> cullParamsBuffer;        // GPU cull params (upload)
    void* cullParamsMapped = nullptr;
  };
  std::vector<PerFrameResources> _frameResources;

  // Descriptor heaps
  DX12DescriptorHeap _cbvSrvUavHeap;
  DX12DescriptorHeap _samplerHeap;

  // Static descriptor indices in _cbvSrvUavHeap
  // [0] ImGui font SRV
  // [1..4] G-buffer SRVs (albedo, normals, emissive, F0Roughness)
  // [5] Window depth SRV
  // [6] AO texture SRV
  // [7..9] reserved (shadow maps etc.)
  // [10] materials SRV
  // [11] meshChunks SRV
  // [12] chunkIndex SRV (per-frame, but we use dynamic for that)
  // [100..1099] bindless textures
  static constexpr uint32_t SRV_GBUFFER_START = 1;   // 4 slots
  static constexpr uint32_t SRV_DEPTH = 5;
  static constexpr uint32_t SRV_AO = 6;
  static constexpr uint32_t SRV_SHADOW_MAPS = 7;
  static constexpr uint32_t SRV_MATERIALS = 10;
  static constexpr uint32_t SRV_MESH_CHUNKS = 11;
  static constexpr uint32_t SRV_CHUNK_INDEX = 12;
  static constexpr uint32_t SRV_BINDLESS_START = 100;

  // Textures
  std::vector<ComPtr<ID3D12Resource>> _textures;
  std::unordered_map<uint32_t, size_t> _textureHashMap;

  // Occluder data
  ComPtr<ID3D12Resource> _occluderVertexBuffer;
  ComPtr<ID3D12Resource> _occluderIndexBuffer;
  uint32_t _occluderIndexCount = 0;
  nlohmann::json _sceneFile;

  // Root signatures
  ComPtr<ID3D12RootSignature> _occluderRootSig;
  ComPtr<ID3D12RootSignature> _drawClusterRootSig;
  ComPtr<ID3D12RootSignature> _gpuCullRootSig;
  ComPtr<ID3D12RootSignature> _deferredLightingRootSig;
  ComPtr<ID3D12RootSignature> _hizRootSig;
  ComPtr<ID3D12RootSignature> _saoRootSig;

  // Pipeline states
  ComPtr<ID3D12PipelineState> _occluderPSO;
  ComPtr<ID3D12PipelineState> _basePassPSO;
  ComPtr<ID3D12PipelineState> _basePassAlphaMaskPSO;
  ComPtr<ID3D12PipelineState> _forwardPSO;
  ComPtr<ID3D12PipelineState> _deferredLightingPSO;
  ComPtr<ID3D12PipelineState> _gpuCullPSO;
  ComPtr<ID3D12PipelineState> _hizCopyPSO;
  ComPtr<ID3D12PipelineState> _hizDownsamplePSO;
  ComPtr<ID3D12PipelineState> _saoPSO;

  // Command signature for ExecuteIndirect
  ComPtr<ID3D12CommandSignature> _drawIndexedCmdSig;

  // G-Buffers
  ComPtr<ID3D12Resource> _gbuffers[4]; // albedo, normals, emissive, F0Roughness
  ComPtr<ID3D12Resource> _depthTexture; // occluder depth
  ComPtr<ID3D12Resource> _aoTexture;
  ComPtr<ID3D12DescriptorHeap> _gbufferRtvHeap; // 4 RTV descriptors
  ComPtr<ID3D12DescriptorHeap> _gbufferDsvHeap; // 1 DSV for occluder depth
  uint32_t _gbufferRtvSize = 0;

  // HiZ pyramid
  ComPtr<ID3D12Resource> _hizTexture;
  uint32_t _hizMipLevels = 0;

  // SAO pyramid
  ComPtr<ID3D12Resource> _saoDepthPyramid;
  uint32_t _saoMipLevels = 0;

  // Shadow maps
  ComPtr<ID3D12Resource> _shadowMapArray;  // Texture2DArray, 3 cascades
  ComPtr<ID3D12DescriptorHeap> _shadowDsvHeap; // 3 DSV descriptors
  ComPtr<ID3D12PipelineState> _shadowPSO;
  static constexpr uint32_t SHADOW_MAP_SIZE = 1024;
  static constexpr uint32_t SHADOW_CASCADE_COUNT = 3;
  mat4 _shadowProjectionMatrices[SHADOW_CASCADE_COUNT];
  mat4 _shadowViewMatrices[SHADOW_CASCADE_COUNT];

  // Frame state
  FrameConstants _frameConstants{};
  uint32_t _currentFrame = 0;
  bool _imguiInitialized = false;

  // Culling stats (readback)
  struct CullingStats {
    uint32_t visibleOpaque = 0;
    uint32_t visibleAlphaMask = 0;
    uint32_t visibleTransparent = 0;
    uint32_t totalOpaque = 0;
    uint32_t totalAlphaMask = 0;
    uint32_t totalTransparent = 0;
  } _cullingStats;

  // GPU cull params (matches gpucull.hlsl cbuffer layout)
  struct GPUCullParams {
    uint32_t opaqueChunkCount;
    uint32_t alphaMaskedChunkCount;
    uint32_t transparentChunkCount;
    uint32_t totalPointLights;
    uint32_t totalSpotLights;
    uint32_t hizMipLevels;
    float screenWidth;
    float screenHeight;
    float viewProjMatrix[16];
    Frustum frustum;
  };

  // Init helpers
  void LoadMeshData();
  void CreateBuffers();
  void CreateTextures();
  void CreateRootSignatures();
  void CreatePipelineStates();
  void CreateGBuffers();
  void CreateHiZResources();
  void CreateSAOResources();
  void CreateShadowResources();
  void CreateCommandSignature();
  void CreateStaticDescriptors();
  void FlushCommandQueue();

  // Per-frame
  void UpdateUniforms();
  void RecordCommandBuffer();
  void ReadbackCullingStats();
  void RenderImGuiOverlay();
};

#endif // ENABLE_DX12
