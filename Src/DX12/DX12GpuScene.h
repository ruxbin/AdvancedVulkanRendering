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
  void OnResize(uint32_t newWidth, uint32_t newHeight);

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
    // Shadow cull resources (per cascade × 2 buckets)
    ComPtr<ID3D12Resource> shadowDrawParams;         // DrawIndexedIndirectCommand × (CASCADE * maxChunks)
    ComPtr<ID3D12Resource> shadowWriteIndex;          // uint × (CASCADE * 2): [c*2+0]=opaque, [c*2+1]=alpha
    ComPtr<ID3D12Resource> shadowWriteIndexUpload;   // for zeroing (upload)
    void* shadowWriteIndexUploadMapped = nullptr;
    ComPtr<ID3D12Resource> shadowCullParams;          // upload, ShadowCullParams cbuffer
    void* shadowCullParamsMapped = nullptr;
    ComPtr<ID3D12Resource> shadowChunkIndicesBuffer;  // shadow chunk indices (UAV)
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
  // [15..1014] bindless textures
  static constexpr uint32_t SRV_GBUFFER_START = 1;   // 4 slots
  static constexpr uint32_t SRV_DEPTH = 5;
  static constexpr uint32_t SRV_AO = 6;
  static constexpr uint32_t SRV_SHADOW_MAPS = 7;
  static constexpr uint32_t SRV_MATERIALS = 10;
  static constexpr uint32_t SRV_MESH_CHUNKS = 11;
  static constexpr uint32_t SRV_CHUNK_INDEX = 12;
  static constexpr uint32_t SRV_BINDLESS_START = 15;
  // sizeof(AAPLMeshChunk) in HLSL — must stay in sync with _padEnd in commonstruct.hlsl
  static constexpr uint32_t MESH_CHUNK_STRIDE = 96;

  // Textures
  std::vector<ComPtr<ID3D12Resource>> _textures;
  std::unordered_map<uint32_t, size_t> _textureHashMap;

  // Texture streaming state (parallel to _textures)
  struct TextureStreamEntry {
    uint32_t totalMips  = 1;  // total mip levels in the full texture
    uint32_t currentMip = 0;  // lowest mip currently resident (0 = full res)
    uint32_t requiredMip = 0; // mip requested by coverage computation
  };
  std::vector<TextureStreamEntry> _streamEntries;
  ComPtr<ID3D12Resource> _streamingStagingBuffer; // large upload heap for mip streaming
  void* _streamingStagingMapped = nullptr;
  static constexpr uint32_t STREAMING_STAGING_SIZE = 32 * 1024 * 1024; // 32 MB staging

  // Occluder data
  ComPtr<ID3D12Resource> _occluderVertexBuffer;
  ComPtr<ID3D12Resource> _occluderIndexBuffer;
  uint32_t _occluderIndexCount = 0;
  nlohmann::json _sceneFile;

  // Root signatures
  ComPtr<ID3D12RootSignature> _occluderRootSig;
  ComPtr<ID3D12RootSignature> _shadowRootSig;  // like occluder but with root const for cascade index
  ComPtr<ID3D12RootSignature> _drawClusterRootSig;
  ComPtr<ID3D12RootSignature> _gpuCullRootSig;
  ComPtr<ID3D12RootSignature> _deferredLightingRootSig;
  ComPtr<ID3D12RootSignature> _hizRootSig;
  ComPtr<ID3D12RootSignature> _saoRootSig;
  ComPtr<ID3D12RootSignature> _shadowCullRootSig;
  ComPtr<ID3D12RootSignature> _lightCullRootSig;

  // HDR intermediate + TAA
  ComPtr<ID3D12Resource> _hdrBuffer;        // R16G16B16A16_FLOAT, same size as swapchain
  ComPtr<ID3D12Resource> _taaHistory[2];    // ping-pong TAA history
  uint32_t _taaHistoryIndex = 0;
  ComPtr<ID3D12DescriptorHeap> _hdrRtvHeap; // 2 RTVs: [0]=HDR buffer, [1]=TAA history write target
  uint32_t _hdrRtvSize = 0;
  ComPtr<ID3D12RootSignature> _resolveRootSig;
  ComPtr<ID3D12PipelineState> _resolvePSO;
  bool _taaEnabled = true;
  // Fixed static slots well above the bindless range (SRV_BINDLESS_START=15, up to 1000 textures = slot 1014)
  static constexpr uint32_t SRV_HDR_BUFFER  = 2050;
  static constexpr uint32_t SRV_TAA_HISTORY = 2051;

  // Scatter volume (froxel-based volumetrics)
  static constexpr uint32_t SCATTER_FROXEL_W = 160;
  static constexpr uint32_t SCATTER_FROXEL_H = 90;
  static constexpr uint32_t SCATTER_FROXEL_D = 64;
  static constexpr uint32_t SRV_SCATTER_ACCUM = 2052; // just after SRV_TAA_HISTORY
  ComPtr<ID3D12Resource> _scatterVolume;        // R16G16B16A16_FLOAT 3D UAV (scatter result)
  ComPtr<ID3D12Resource> _scatterAccumVolume;   // R16G16B16A16_FLOAT 3D SRV for deferred lighting
  ComPtr<ID3D12RootSignature> _scatterRootSig;
  ComPtr<ID3D12RootSignature> _accumRootSig;
  ComPtr<ID3D12PipelineState> _scatterVolumePSO;
  ComPtr<ID3D12PipelineState> _accumulatePSO;
  bool _scatterAccumIsInPSR = false; // tracks whether _scatterAccumVolume is in PSR vs UAV state

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
  ComPtr<ID3D12PipelineState> _shadowCullPSO;
  ComPtr<ID3D12PipelineState> _coarseCullPSO;
  ComPtr<ID3D12PipelineState> _traditionalCullPSO;
  ComPtr<ID3D12PipelineState> _clearIndicesPSO;

  // Command signature for ExecuteIndirect
  ComPtr<ID3D12CommandSignature> _drawIndexedCmdSig;
  ComPtr<ID3D12CommandSignature> _shadowDrawCmdSig;

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

  // Per-cascade sun-view box, kept so the GPU cull frustum can be built from
  // explicit corners instead of extracting planes out of the VP matrix (the
  // matrix route is easy to get wrong given this codebase's storage
  // convention — see docs/dx12-matrix-and-binding-audit.md).
  // The box is [-halfExtent, +halfExtent]^2 in x/y and [0, farZ] in z,
  // anchored at `eye` and oriented by (axisX, axisY, axisZ).
  struct ShadowCascadeBox {
    vec3 eye;
    vec3 axisX, axisY, axisZ;
    float halfExtent = 0.0f;
    float farZ = 0.0f;
  };
  ShadowCascadeBox _shadowCascadeBoxes[SHADOW_CASCADE_COUNT];

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

  // Light data
  std::vector<AAPLPointLightCullingData> _pointLights;
  std::vector<AAPLSpotLightCullingData>  _spotLights;
  ComPtr<ID3D12Resource> _pointLightBuffer;
  ComPtr<ID3D12Resource> _spotLightBuffer;

  // Light culling output buffers
  static constexpr uint32_t LIGHT_TILE_SIZE    = 32;
  static constexpr uint32_t MAX_LIGHTS_PER_TILE_LC = 64; // matches MAX_LIGHTS_PER_TILE in commonstruct.hlsl
  ComPtr<ID3D12Resource> _lightXZRangeBuffer;
  ComPtr<ID3D12Resource> _spotXZRangeBuffer;
  ComPtr<ID3D12Resource> _lightIndicesBuffer;
  ComPtr<ID3D12Resource> _spotLightIndicesBuffer;
  ComPtr<ID3D12Resource> _lightIndicesTransparentBuffer;
  ComPtr<ID3D12Resource> _spotLightIndicesTransparentBuffer;
  ComPtr<ID3D12Resource> _lightDebugTexture;     // RWTexture2D<uint>
  ComPtr<ID3D12Resource> _lightCullParamsBuffer; // upload, persistently mapped
  void* _lightCullParamsMapped = nullptr;

  // Init helpers
  void LoadSceneFile();
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
  void CreateHDRResources();
  void CreateLights();
  void CreateLightCullPipelines();
  void CreateScatterResources();
  bool IsScatterReady() const;
  void FlushCommandQueue();

  // Per-frame
  void UpdateUniforms();
  void UpdateTextureStreaming();
  void RecordCommandBuffer();
  void ReadbackCullingStats();
  void RenderImGuiOverlay();
  void DispatchLightCulling(ID3D12GraphicsCommandList* cmdList);
};

#endif // ENABLE_DX12
