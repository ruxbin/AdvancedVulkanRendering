#pragma once
#include "Camera.h"
#include "Common.h"
#include "AssetLoader.h"
#include "Light.h"
#include "Matrix.h"
#include "ScatteringVolume.h"
#include "VulkanSetup.h"
#include "nlohmann/json.hpp"
#include "spdlog/spdlog.h"
#include "vulkan/vulkan.h"
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <stdexcept>
#include <stdio.h>
#include <string_view>
#include <thread>
#include <tuple>
#include <utility>
#include <unordered_map>
#include <vector>


class Shadow;
union SDL_Event;

struct AAPLTextureData {
  std::string _path;
  uint32_t _pathHash;
  unsigned long long _width;
  unsigned long long _height;
  unsigned long long _mipmapLevelCount;
  uint32_t _pixelFormat;
  unsigned long long _pixelDataOffset;
  unsigned long long _pixelDataLength;
  std::vector<unsigned long> _mipOffsets;
  std::vector<unsigned long> _mipLengths;
  AAPLTextureData() = delete;
  AAPLTextureData(FILE *f);
#ifdef __ANDROID__
  AAPLTextureData(AssetLoader::BinaryFileReader &reader);
#endif
  AAPLTextureData(const AAPLTextureData &) = delete;
  AAPLTextureData(AAPLTextureData &&rhs);
};

// Texture streaming: mirrors Metal's AAPLTextureManager mip-streaming design
static constexpr unsigned int PERMANENT_TEXTURE_SIZE = 64;
static constexpr unsigned int MAX_TEXTURE_SIZE = 4096;
static constexpr int TEXTURE_RETENTION_FRAMES = 3;

struct TextureStreamingEntry {
  const AAPLTextureData* desc = nullptr;
  VkImage image = VK_NULL_HANDLE;
  VkImageView imageView = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  int currentMip = 0;    // base mip currently resident on GPU (0 = finest)
  int requiredMip = 0;   // finest mip desired this frame (clamped between topMip and botMip)
  size_t textureIndex = 0; // index into the bindless textures array
  // True while a work item for this entry is in pendingBlits or cpuCompletedWork.
  // Prevents duplicate dispatches: image can only be freed by processStreamingWork
  // (which sets inFlight=false), so sourceImage is guaranteed live until used.
  bool inFlight = false;
};

struct AAPLMeshData {
  unsigned long long _vertexCount, _indexCount, _indexType, _chunkCount,
      _meshCount, _opaqueChunkCount, _opaqueMeshCount, _alphaMaskedChunkCount,
      _alphaMaskedMeshCount, _transparentChunkCount, _transparentMeshCount,
      _materialCount;
  void *_vertexData;
  void *_normalData;
  void *_tangentData;
  void *_uvData;
  void *_indexData;
  void *_chunkData;
  void *_meshData;
  void *_materialData;
  uint64_t compressedVertexDataLength;
  uint64_t compressedNormalDataLength;
  uint64_t compressedTangentDataLength;
  uint64_t compressedUvDataLength;
  uint64_t compressedIndexDataLength;
  uint64_t compressedChunkDataLength;
  uint64_t compressedMeshDataLength;
  uint64_t compressedMaterialDataLength;
  std::vector<AAPLTextureData> _textures;
  void *_textureData;
  AAPLMeshData(const char *filepath);
  AAPLMeshData() = delete;
  AAPLMeshData(const AAPLMeshData &) = delete;
  ~AAPLMeshData();
};

struct alignas(16) AAPLShaderMaterial {
  uint32_t albedo_texture_index;
  uint32_t roughness_texture_index;
  uint32_t normal_texture_index;
  uint32_t emissive_texture_index;
  float alpha;
  uint32_t hasMetallicRoughness;
  uint32_t hasEmissive;
  // #if SUPPORT_SPARSE_TEXTURES //TODO:
  //     uint baseColorMip;
  //     uint metallicRoughnessMip;
  //     uint normalMip;
  //     uint emissiveMip;
  // #endif
};

// A SubMesh represents a group of chunks that share a material.
//  The indices for the chunks in this submesh are contiguous in the index
//  buffer.
struct AAPLSubMesh {
  uint32_t materialIndex; // Material index for this submesh.

  AAPLBoundingBox3
      boundingBox; // Combined bounding box for the chunks in this submesh.
  AAPLSphere boundingSphere; // Combined bounding sphere for the chunks in this.
                             // submesh.

  unsigned int indexBegin; // Offset in mesh index buffer to the indices for
                           // this. submesh.
  unsigned int
      indexCount; // Number of indices for this submesh in mesh index. buffer.

  unsigned int chunkStart; // Offset in mesh index buffer to the chunks for
                           // this. submesh.
  unsigned int
      chunkCount; // Number of chunks for this submesh in mesh index buffer.
};

// Data only structure storing encoded material information.
// TODO: vec4换成vec3后,虽然size还是96，但是uncompress之后的数据全乱了
struct AAPLMaterial {
  alignas(16) vec4 baseColor;        // Fallback diffuse color.
  unsigned int baseColorTextureHash; // Hash of diffuse texture.
  bool hasBaseColorTexture; // Flag indicating a valid diffuse texture index.
  bool hasDiffuseMask; // Flag indicating an alpha mask in the diffuse texture.
  alignas(16) vec4 metallicRoughness; // Fallback metallic roughness.
  unsigned int metallicRoughnessHash; // Hash of specular texture.
  bool hasMetallicRoughnessTexture;   // Flag indicating a valid metallic
                                      // roughness texture index.
  unsigned int normalMapHash;         // Hash of normal map texture.
  bool hasNormalMap; // Flag indicating a valid normal map texture index.
  alignas(16) vec4 emissiveColor;   // Fallback emissive color.
  unsigned int emissiveTextureHash; // Hash of emissive texture.
  bool hasEmissiveTexture; // Flag indicating a valid emissive texture index.
  float opacity;
};

class GpuScene {
private:
  VkBuffer vb;
  VkBuffer ib;
  VkBuffer texcoordBuffer;
  VkBuffer normalBuffer;
  // VkDevice device;
  const VulkanDevice &device;

  VkDescriptorSetLayout globalSetLayout;
  VkDescriptorPool descriptorPool;
  std::vector<VkDescriptorSet> globalDescriptorSets; // per-frame

  VkDescriptorSetLayout applSetLayout;
  VkDescriptorPool applDescriptorPool;
  std::vector<VkDescriptorSet> applDescriptorSets; // per-frame

  VkDescriptorSetLayout gpuCullSetLayout;
  VkDescriptorPool gpuCullDescriptorPool;
  std::vector<VkDescriptorSet> gpuCullDescriptorSets; // per-frame

  VkDescriptorSetLayout deferredLightingSetLayout;
  VkDescriptorPool deferredLightingDescriptorPool;
  std::vector<VkDescriptorSet> deferredLightingDescriptorSet; // per-frame

  std::vector<VkBuffer> uniformBuffers;         // per-frame
  std::vector<VkDeviceMemory> uniformBufferMemories; // per-frame

  std::vector<VkBuffer> drawParamsBuffers;       // per-frame
  std::vector<VkDeviceMemory> drawParamsBufferMemories;
  std::vector<VkBuffer> cullParamsBuffers;        // per-frame
  std::vector<VkDeviceMemory> cullParamsBufferMemories;
  VkBuffer meshChunksBuffer;
  VkDeviceMemory meshChunksBufferMemory;
  std::vector<VkBuffer> writeIndexBuffers;       // per-frame
  std::vector<VkDeviceMemory> writeIndexBufferMemories;
  std::vector<VkBuffer> chunkIndicesBuffers;     // per-frame
  std::vector<VkDeviceMemory> chunkIndicesBufferMemories;

  VkBuffer vertexBuffer;
  VkDeviceMemory vertexBufferMemory;

  VkBuffer indexBuffer;
  VkDeviceMemory indexBufferMemory;

  VkPipelineLayout pipelineLayout;
  VkPipeline graphicsPipeline;

  // 多帧并行 (Frames in Flight)
  // framesInFlight 由 swapchain 图像数量决定，在初始化时设置
  uint32_t framesInFlight = 0;
  std::vector<VkCommandBuffer> commandBuffers;
  std::vector<VkSemaphore> imageAvailableSemaphores;
  std::vector<VkSemaphore> renderFinishedSemaphores;
  std::vector<VkFence> inFlightFences;
  // Per-swapchain-image fence tracking (size = swapChainImageCount).
  // imagesInFlight[N] == fence currently associated with submitting work for
  // swapchain image N. NULL means the image has not been used yet. Used by the
  // imagesInFlight pattern: after vkAcquireNextImageKHR returns imageIndex, we
  // wait on imagesInFlight[imageIndex] before writing to per-image resources
  // (uniform buffers, descriptor data) that the GPU might still be reading.
  std::vector<VkFence> imagesInFlight;
  // Cycles 0..framesInFlight-1 each frame. Drives sync slot allocation
  // (cmd buffer / semaphore / fence). Decoupled from currentFrame, which is
  // tied to imageIndex (set after acquire) so that per-image record-path
  // accesses index the SAME swapchain-scoped resource that
  // _deferredFrameBuffer[imageIndex] / _forwardFrameBuffer[imageIndex] are
  // bound to.
  uint32_t _syncSlot = 0;
  // After vkAcquireNextImageKHR returns imageIndex, we set currentFrame =
  // imageIndex. The record path then accesses any per-frame-or-per-image
  // resource via currentFrame and stays consistent with the deferred/forward
  // framebuffers (both indexed by imageIndex).
  uint32_t currentFrame = 0;
  
  // Get the command buffer for the current sync slot.
  // (Command buffers are sync-resources, indexed by _syncSlot.)
  VkCommandBuffer& getCurrentCommandBuffer() { return commandBuffers[_syncSlot]; }
  
  float modelScale;

  Camera *maincamera;

  // 标记是否需要重建 swapchain
  bool framebufferResized = false;

  AAPLMeshData *applMesh;

  VkBuffer applVertexBuffer;
  VkDeviceMemory applVertexBufferMemory;
  VkBuffer applNormalBuffer;
  VkDeviceMemory applNormalBufferMemory;
  VkBuffer applTangentBuffer;
  VkDeviceMemory applTangentBufferMemory;
  VkBuffer applUVBuffer;
  VkDeviceMemory applUVBufferMemory;


  VkBuffer applIndexBuffer;
  VkDeviceMemory applIndexMemory;

  VkBuffer applMaterialBuffer;
  VkDeviceMemory applMaterialBufferMemory;

  VkPipelineLayout drawclusterPipelineLayout;
  VkPipeline drawclusterPipeline;
  VkPipeline drawclusterPipelineAlphaMask;

  VkPipelineLayout encodeDrawBufferPipelineLayout;
  VkPipeline encodeDrawBufferPipeline;

  VkPipelineLayout drawclusterBasePipelineLayout;
  VkPipeline drawclusterBasePipeline;
  VkPipeline drawclusterBasePipelineAlphaMask; // GPU indirect alpha-mask base pass
  VkPipeline drawclusterForwardPipeline;
  VkPipeline drawclusterForwardPipelineIndirect; // GPU indirect forward pass (no push constants)

  VkPipelineLayout deferredLightingPipelineLayout;
  VkPipeline deferredLightingPipeline;
  VkPipeline deferredLightingPipeline_clusterlighting;

  VkBuffer occluderVertexBuffer;
  VkDeviceMemory occluderVertexBufferMemory;
  VkBuffer occluderIndexBuffer;
  VkDeviceMemory occluderIndexMemory;

  AAPLMeshChunk *m_Chunks;
  AAPLSubMesh *m_SubMeshes = nullptr;   // per-mesh bounding spheres for streaming

  // VkImageView currentImage;
  VkSampler textureSampler;
  VkSampler nearestClampSampler;

  // VkImage textureImage;
  // VkDeviceMemory textureImageMemory;

  std::vector<std::pair<VkImage, VkImageView>> textures;
  std::vector<VkDeviceMemory> textureMemory;
  std::unordered_map<uint32_t, size_t> textureHashMap;

  // --- Texture streaming (mirrors AAPLTextureManager mip streaming) ---
  std::vector<TextureStreamingEntry> streamingEntries;
  std::unordered_map<uint32_t, size_t> streamingEntryMap;

  // Background blit thread
  std::thread blitThread;
  std::mutex pendingBlitsMutex;
  std::condition_variable blitCondition;
  bool blitThreadRunning = false;

  // Pending blit requests for background thread
  struct PendingBlit {
    uint32_t textureHash;
    size_t entryIndex;
    int targetMip;
    // Snapshot of entry state at dispatch time — avoids data race with processStreamingWork
    int snapshotCurrentMip;
    VkImage snapshotImage;
  };
  std::vector<PendingBlit> pendingBlits;

  // CPU-completed work from background thread (GPU processing on main thread)
  struct CpuStreamingWork {
    size_t entryIndex;
    VkImage newImage;
    VkDeviceMemory newMemory;
    VkBuffer stagingBuf;
    VkDeviceMemory stagingMem;
    int targetMip;
    int currentMip;       // old mip before this swap
    VkImage sourceImage;  // old image handle at dispatch time (for image→image copy)
    int oldMipCount;      // mip count of sourceImage
    std::vector<VkBufferImageCopy> bufferCopyRegions;
    std::vector<VkImageCopy> imageCopyRegions;
    int newMipCount;
    VkFormat format;
    unsigned long long blockSize, bytesPerBlock;
    int mipStart, mipEnd; // new mips uploaded to staging
  };
  std::vector<CpuStreamingWork> cpuCompletedWork;
  std::mutex cpuWorkMutex;

  // Old texture retention ring buffer (TEXTURE_RETENTION_FRAMES ring)
  std::vector<std::vector<std::tuple<VkImage, VkImageView, VkDeviceMemory>>> textureToDelete;

  // Dedicated transfer queue for async texture uploads
  VkQueue streamingTransferQueue = VK_NULL_HANDLE;
  VkCommandPool streamingTransferCmdPool = VK_NULL_HANDLE;

  // Cached uncompressed CPU-side materials (for streaming texture hash lookups)
  AAPLMaterial* cpuMaterials = nullptr;

  // Flag: descriptors need re-writing because streaming swaps happened.
  // One bit per frame slot — set all bits on swap, cleared per-slot after update.
  uint32_t streamingDescriptorsDirtyMask = 0;

  std::vector<AAPLShaderMaterial> materials;

  nlohmann::json sceneFile;

  VkRenderPass occluderZPass;

  VkImage _depthTexture;
  VkImage _depthPyramidTexture;
  VkImageView _depthTextureView;
  VkFramebuffer _depthFrameBuffer;
  VkFormat _depthFormat = VK_FORMAT_D32_SFLOAT_S8_UINT;
  VkBuffer _occludersVertBuffer;
  VkBuffer _occludersIndexBuffer;
  VkDeviceMemory _occludersBufferMemory;
  VkDeviceMemory _occludersIndexBufferMemory;

  VkImage _gbufferAlbedoAlpha;
  VkImage _gbufferNormals;
  VkImage _gbufferEmissive;
  VkImage _gbufferF0Roughness;
  VkImageView _gbufferAlbedoAlphaTextureView;
  VkImageView _gbufferNormalsTextureView;
  VkImageView _gbufferEmissiveTextureView;
  VkImageView _gbufferF0RoughnessTextureView;
  VkFormat _gbufferFormat[4] = {
      VK_FORMAT_B8G8R8A8_SRGB, VK_FORMAT_R16G16B16A16_SFLOAT,
      VK_FORMAT_B8G8R8A8_SRGB, VK_FORMAT_B8G8R8A8_SRGB};
  std::vector<VkImage> _gbuffers[4];       // per-frame, [channel][frame]
  std::vector<VkImageView> _gbuffersView[4]; // per-frame, [channel][frame]
  std::vector<VkFramebuffer> _basePassFrameBuffer; // per-frame
  VkRenderPass _basePass;

  std::vector<VkFramebuffer> _deferredFrameBuffer;
  VkRenderPass _deferredLightingPass;

  std::vector<VkFramebuffer> _forwardFrameBuffer;
  VkRenderPass _forwardLightingPass;

  void createGraphicsPipeline(VkRenderPass renderPass);
  void createComputePipeline();

  VkPipelineLayout drawOccluderPipelineLayout;
  VkPipeline drawOccluderPipeline;

  // Debug wireframe occluder overlay
  VkPipeline occluderWireframePipeline = VK_NULL_HANDLE;
  void createOccluderWireframePipeline();
  void drawOccludersWireframe(VkCommandBuffer commandBuffer);

  Shadow *_shadow;
  std::vector<PointLight> _pointLights;
  std::vector<SpotLight> _spotLights;

  std::filesystem::path _rootPath;

  LightCuller *_lightCuller = nullptr;

  bool useClusterLighting = true;
  bool useRayTracing = false;     // ImGui toggle: switch to full RT path

  // Hardware ray tracing (optional path).
  class RayTracing *_raytracing = nullptr;

  // Hi-Z Occlusion Culling (Stage 3)
  VkImage _hizTexture = VK_NULL_HANDLE;
  VkDeviceMemory _hizMemory = VK_NULL_HANDLE;
  VkImageView _hizTextureView = VK_NULL_HANDLE;  // full mip chain for sampling
  std::vector<VkImageView> _hizMipViews;          // per-mip views for compute writes
  VkSampler _hizSampler = VK_NULL_HANDLE;
  uint32_t _hizMipLevels = 0;
  uint32_t _hizWidth = 0;
  uint32_t _hizHeight = 0;

  VkPipelineLayout _hizPipelineLayout = VK_NULL_HANDLE;
  VkPipeline _hizCopyPipeline = VK_NULL_HANDLE;
  VkPipeline _hizDownsamplePipeline = VK_NULL_HANDLE;

  VkDescriptorSetLayout _hizCopySetLayout = VK_NULL_HANDLE;
  VkDescriptorSetLayout _hizDownsampleSetLayout = VK_NULL_HANDLE;
  VkDescriptorPool _hizDescriptorPool = VK_NULL_HANDLE;
  VkDescriptorSet _hizCopyDescriptorSet = VK_NULL_HANDLE;
  std::vector<VkDescriptorSet> _hizDownsampleDescriptorSets;

  void createHiZResources();
  void generateHiZPyramid(VkCommandBuffer commandBuffer);

  // Scalable Ambient Obscurance (SAO)
  std::vector<VkImage> _saoDepthPyramid;                     // per-frame
  std::vector<VkDeviceMemory> _saoDepthPyramidMemory;        // per-frame
  std::vector<VkImageView> _saoDepthPyramidView;             // per-frame
  std::vector<std::vector<VkImageView>> _saoMipViews;        // [frame][mip]
  uint32_t _saoMipLevels = 0;
  uint32_t _saoWidth = 0, _saoHeight = 0;
  std::vector<VkDescriptorSet> _saoCopyDescriptorSet;        // per-frame
  std::vector<std::vector<VkDescriptorSet>> _saoDownsampleDescriptorSets; // [frame][mip-1]

  std::vector<VkImage> _aoTexture;                           // per-frame
  std::vector<VkDeviceMemory> _aoTextureMemory;              // per-frame
  std::vector<VkImageView> _aoTextureView;                   // per-frame

  VkDescriptorSetLayout _saoSetLayout = VK_NULL_HANDLE;
  VkDescriptorPool _saoDescriptorPool = VK_NULL_HANDLE;
  std::vector<VkDescriptorSet> _saoDescriptorSets; // per-frame
  VkPipelineLayout _saoPipelineLayout = VK_NULL_HANDLE;
  VkPipeline _saoPipeline = VK_NULL_HANDLE;

  void createSAOResources();
  void generateSAODepthPyramid(VkCommandBuffer cmd);
  void dispatchSAO(VkCommandBuffer cmd);

  // Resolve pass (TAA + Tone Mapping)
  mat4 _prevViewProjectionMatrix;
  bool _taaFirstFrame = true;
  uint32_t _taaFrameIndex = 0;
  bool _taaEnabled = false; // ImGui toggle, default OFF

  VkImage _hdrLightingBuffer = VK_NULL_HANDLE;
  VkDeviceMemory _hdrLightingBufferMemory = VK_NULL_HANDLE;
  VkImageView _hdrLightingBufferView = VK_NULL_HANDLE;

  // TAA ring-buffer history: framesInFlight+1 images so that even with
  // all swapchain images in flight, the oldest slot is guaranteed idle.
  VkImage _taaHistoryBuffer[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
  VkDeviceMemory _taaHistoryBufferMemory[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
  VkImageView _taaHistoryBufferView[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};

  VkRenderPass _resolvePass = VK_NULL_HANDLE;
  std::vector<VkFramebuffer> _resolveFrameBuffer;
  VkDescriptorSetLayout _resolveSetLayout = VK_NULL_HANDLE;
  VkDescriptorPool _resolveDescriptorPool = VK_NULL_HANDLE;
  std::vector<VkDescriptorSet> _resolveDescriptorSets; // per-frame (depth view is per-frame)
  VkPipelineLayout _resolvePipelineLayout = VK_NULL_HANDLE;
  VkPipeline _resolvePipeline = VK_NULL_HANDLE;
  VkSampler _linearClampSampler = VK_NULL_HANDLE;

  void createHDRLightingBuffer();
  void createTAAHistoryBuffer();
  void createResolvePass();
  void createResolveFrameBuffer(uint32_t count);
  void createResolveDescriptors();
  void createResolvePipeline();
  void createLinearClampSampler();

  // Scatter volume (froxel-based volumetric scattering, ported from Metal)
  ScatteringVolume _scatterVolume;
  void createScatterVolume();

  // Screen-space decals
  VkDescriptorSetLayout _decalSetLayout = VK_NULL_HANDLE;
  VkDescriptorPool _decalDescriptorPool = VK_NULL_HANDLE;
  VkPipelineLayout _decalPipelineLayout = VK_NULL_HANDLE;
  VkPipeline _decalPipeline = VK_NULL_HANDLE;
  VkRenderPass _decalRenderPass = VK_NULL_HANDLE;
  std::vector<VkFramebuffer> _decalFramebuffers;
  std::vector<VkDescriptorSet> _decalDescriptorSets;
  // Per-frame SSBO holding MAX_DECALS DecalData entries. CPU writes the whole
  // array once before the render pass; GPU reads via push-constant index.
  std::vector<VkBuffer> _decalDataBuffers;
  std::vector<VkDeviceMemory> _decalDataBufferMemories;
  VkBuffer _decalVertexBuffer = VK_NULL_HANDLE;
  VkDeviceMemory _decalVertexBufferMemory = VK_NULL_HANDLE;
  VkBuffer _decalIndexBuffer = VK_NULL_HANDLE;
  VkDeviceMemory _decalIndexBufferMemory = VK_NULL_HANDLE;
  uint32_t _decalIndexCount = 0;
  std::vector<DecalData> _decals;

  // Decal texture
  VkImage _decalTextureImage = VK_NULL_HANDLE;
  VkDeviceMemory _decalTextureMemory = VK_NULL_HANDLE;
  VkImageView _decalTextureView = VK_NULL_HANDLE;

  // Interactive decal state
  bool _decalEditMode = false;
  int _selectedDecal = -1;           // -1 = none, >=0 = index
  bool _isDraggingDecal = false;
  bool _isRotatingDecal = false;
  float _lastMouseX = 0, _lastMouseY = 0;
  int _decalTexIndex = 0;            // 0=solid color, 1+=texture presets
  uint32_t _currentFrameIndex = 0;
  VkBuffer _depthReadbackBuffer = VK_NULL_HANDLE;
  VkDeviceMemory _depthReadbackBufferMemory = VK_NULL_HANDLE;
  static constexpr int DECAL_TEX_COUNT = 4;
  const char* _decalTexPaths[DECAL_TEX_COUNT] = {
    "textures/1.png",
    "textures/2.png",
    "textures/3.png",
    "textures/texture.jpg"
  };

  void createDecalResources();
  void createDecalRenderPass();
  void drawDecals(VkCommandBuffer commandBuffer, uint32_t imageIndex);
  void loadDecalTexture(const char *path);
  vec3 getWorldPosFromDepth(float mouseX, float mouseY);
  void updateDecalFromMouse(float mouseX, float mouseY, bool placeNew);
  void onMouseDownDecal(float mouseX, float mouseY);
  void onMouseMoveDecal(float mouseX, float mouseY);
  void onMouseUpDecal();
  void onScrollDecal(float deltaY);
  void cycleDecalTexture();
  void setDecalPreview(const vec3 &worldPos, const vec3 &normal,
                       const vec3 &scale);

  // ImGui overlay
  VkDescriptorPool _imguiDescriptorPool = VK_NULL_HANDLE;
  bool _imguiInitialized = false;
  struct CullingStats {
    uint32_t visibleOpaque = 0;
    uint32_t visibleAlphaMask = 0;
    uint32_t visibleTransparent = 0;
    uint32_t totalOpaque = 0;
    uint32_t totalAlphaMask = 0;
    uint32_t totalTransparent = 0;
  } _cullingStats;
  void initImGui(SDL_Window *window);
  void readbackCullingStats(uint32_t previousFrame);
  void renderImGuiOverlay(VkCommandBuffer commandBuffer, uint32_t imageIndex);

  void createRenderOccludersPipeline(VkRenderPass renderPass);

  void createCommandBuffers(VkCommandPool commandPool) {
    commandBuffers.resize(framesInFlight);
    
    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool = commandPool;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = framesInFlight;

    if (vkAllocateCommandBuffers(device.getLogicalDevice(), &allocInfo,
                                 commandBuffers.data()) != VK_SUCCESS) {
      throw std::runtime_error("failed to allocate command buffers!");
    }
  }

  void createSyncObjects();
  void cleanupSwapChainResources();
  void recreateSwapChainResources();

public:
  GpuScene(std::filesystem::path &root, const VulkanDevice &deviceref);
  ~GpuScene();
  GpuScene() = delete;
  GpuScene(const GpuScene &) = delete;
  void Draw();
  void recreateSwapChain();
  void setFramebufferResized(bool resized) { framebufferResized = resized; }
  const std::filesystem::path &RootPath() const { return _rootPath; }
  void InitImGui(SDL_Window *window) { initImGui(window); }
  void ProcessImGuiEvent(SDL_Event *event);

  VkShaderModule createShaderModule(const std::vector<char> &code) const;

  const VkPipelineLayout &getDrawClusterPipelineLayout() const {
    return drawclusterPipelineLayout;
  }

  const VkPipeline &getDrawClusterPipeline() const {
    return drawclusterPipeline;
  }
  const VkPipeline &getDrawClusterPipelineAlphaMask() const {
    return drawclusterPipelineAlphaMask;
  }

  Camera *GetMainCamera() { return maincamera; }

  void init_GlobaldescriptorSet();

  void init_appl_descriptors();

  void init_drawparams_descriptors();

  void init_deferredlighting_descriptors();

  void DrawChunks(VkCommandBuffer commandBuffer);
  void TriggerClusterLighting() { useClusterLighting = !useClusterLighting; }

  // Decal interaction
  void ToggleDecalMode() { _decalEditMode = !_decalEditMode; }
  bool IsDecalMode() const { return _decalEditMode; }
  void OnDecalMouseDown(float mx, float my) {
    if (_decalEditMode) onMouseDownDecal(mx, my);
  }
  void OnDecalMouseMove(float mx, float my) {
    if (_decalEditMode) onMouseMoveDecal(mx, my);
  }
  void OnDecalMouseUp() {
    if (_decalEditMode) onMouseUpDecal();
  }
  void OnDecalScroll(float dy) {
    if (_decalEditMode) onScrollDecal(dy);
  }
  void OnDecalCycleTexture() {
    if (_decalEditMode) cycleDecalTexture();
  }
  void OnDecalStartRotate() {
    if (_decalEditMode) { _isDraggingDecal = false; _isRotatingDecal = true; }
  }
  void OnDecalDelete() {
    if (_decalEditMode && _selectedDecal >= 0 && (size_t)_selectedDecal < _decals.size()) {
      _decals.erase(_decals.begin() + _selectedDecal);
      _selectedDecal = -1;
    }
  }

  void DrawChunksBasePass(VkCommandBuffer commandBuffer);

  void CreateTextures();

  // Texture streaming (mirrors AAPLTextureManager)
  void initTextureStreaming();
  void shutdownTextureStreaming();
  void UpdateTextureStreaming(int frameIndex);
  int calculateMinMip(const AAPLTextureData& desc, unsigned int maxSize) const;
  int calculateRequiredMip(const AAPLTextureData& desc, float screenArea) const;
  void setRequiredMip(uint32_t textureHash, float screenArea);
  void dispatchStreamingRequest(size_t entryIndex);
  void blitThreadFunc();
  void processStreamingWork(int frameIndex);

  void updateSamplerInDescriptors(VkImageView currentImage);

  void ConfigureMaterial(const AAPLMaterial &, AAPLShaderMaterial &);

  void transitionShaderMapLayout(VkImage image, VkFormat format,
                             VkImageLayout oldLayout,
                             VkImageLayout newLayout,
                             VkCommandBuffer commandBuffer) const{
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 3;//SHADOW_CASCADE_COUNT;

    barrier.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
              
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

    VkPipelineStageFlags sourceStage = VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    VkPipelineStageFlags destinationStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;

    vkCmdPipelineBarrier(commandBuffer, sourceStage, destinationStage, 0, 0,
                         nullptr, 0, nullptr, 1, &barrier);

  }

  void transitionImageLayout(VkImage image, VkFormat format,
                             VkImageLayout oldLayout,
                             VkImageLayout newLayout,
                             VkCommandBuffer commandBuffer) const {
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;

    if (newLayout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL ||
        newLayout == VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL) {
      barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;

    } else if (oldLayout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL) {
      barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    } else {
      barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    }

    if (hasStencilComponent(format)) {
      barrier.subresourceRange.aspectMask |= VK_IMAGE_ASPECT_STENCIL_BIT;
    }

    VkPipelineStageFlags sourceStage;
    VkPipelineStageFlags destinationStage;

    if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED &&
        newLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
      barrier.srcAccessMask = 0;
      barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;

      sourceStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
      destinationStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    } else if (oldLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL &&
               newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
      barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
      barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

      sourceStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
      destinationStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    } else if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED &&
               (newLayout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL ||
                newLayout == VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL)) {
      barrier.srcAccessMask = 0;
      barrier.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                              VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

      sourceStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
      destinationStage = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    } else if ((oldLayout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL ||
                oldLayout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL) &&
               newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
      barrier.srcAccessMask =
          oldLayout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL
              ? VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT
              : VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
      barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

      sourceStage =
          oldLayout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL
              ? VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT
              : VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT; // TODO：VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT？？
      destinationStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    } else if (oldLayout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL &&
               newLayout ==
                   VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL) {
      barrier.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
      barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
      sourceStage = VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
      destinationStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    } else if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED &&
               newLayout == VK_IMAGE_LAYOUT_GENERAL) {
      barrier.srcAccessMask = 0;
      barrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;

      sourceStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
      destinationStage = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    } else {
      throw std::invalid_argument("unsupported layout transition!");
    }

    vkCmdPipelineBarrier(commandBuffer, sourceStage, destinationStage, 0, 0,
                         nullptr, 0, nullptr, 1, &barrier);
  }

  void CreateDepthTexture();
  void DrawOccluders(VkCommandBuffer commandBuffer);
  void CreateOccluderZPass();
  void CreateOccluderZPassFrameBuffer();
  void CreateZdepthView();

  void CreateGBuffers();
  void CreateBasePassFrameBuffer();
  void CreateDeferredLightingFrameBuffer(uint32_t count);
  void CreateDeferredBasePass();
  void CreateDeferredLightingPass();
  void CreateForwardLightingPass();
  void CreateForwardLightingFrameBuffer(uint32_t count);

  struct GPUCullParams {
    uint32_t opaqueChunkCount;
    uint32_t alphaMaskedChunkCount;
    uint32_t transparentChunkCount;
    uint32_t totalPointLights;
    uint32_t totalSpotLights;
    uint32_t hizMipLevels;
    float screenWidth;
    float screenHeight;
    float viewProjMatrix[16]; // 64 bytes, starts at offset 32
    Frustum frustum;
  };

  void *loadMipTexture(const AAPLTextureData &texturedata, int, unsigned int &);

  void createUniformBuffer() {
    uniformBuffers.resize(framesInFlight);
    uniformBufferMemories.resize(framesInFlight);
    for (uint32_t i = 0; i < framesInFlight; ++i) {
      VkBufferCreateInfo bufferInfo{};
      bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
      bufferInfo.size = sizeof(FrameData);
      bufferInfo.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
      bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
      bufferInfo.flags = 0;
      if (vkCreateBuffer(device.getLogicalDevice(), &bufferInfo, nullptr,
                         &uniformBuffers[i]) != VK_SUCCESS) {
        throw std::runtime_error("failed to create uniform buffer!");
      }
      VkMemoryRequirements memRequirements;
      vkGetBufferMemoryRequirements(device.getLogicalDevice(), uniformBuffers[i],
                                    &memRequirements);

      VkMemoryAllocateInfo allocInfo{};
      allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
      allocInfo.allocationSize = memRequirements.size;
      allocInfo.memoryTypeIndex =
          device.findMemoryType(memRequirements.memoryTypeBits,
                                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                    VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

      if (vkAllocateMemory(device.getLogicalDevice(), &allocInfo, nullptr,
                           &uniformBufferMemories[i]) != VK_SUCCESS) {
        throw std::runtime_error("failed to allocate uniform buffer memory!");
      }
      vkBindBufferMemory(device.getLogicalDevice(), uniformBuffers[i],
                         uniformBufferMemories[i], 0);
    }
  }

  void createNearestClampSampler() {
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(device.getPhysicalDevice(), &properties);
    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_NEAREST;
    samplerInfo.minFilter = VK_FILTER_NEAREST;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.anisotropyEnable = VK_FALSE;
    // samplerInfo.maxAnisotropy = properties.limits.maxSamplerAnisotropy;
    // samplerInfo.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
    samplerInfo.unnormalizedCoordinates = VK_FALSE;
    samplerInfo.compareEnable = VK_FALSE;
    samplerInfo.compareOp = VK_COMPARE_OP_ALWAYS;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;

    if (vkCreateSampler(device.getLogicalDevice(), &samplerInfo, nullptr,
                        &nearestClampSampler) != VK_SUCCESS) {
      throw std::runtime_error("failed to create nearest clamp sampler!");
    }
  }

  void createTextureSampler() {
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(device.getPhysicalDevice(), &properties);
    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.anisotropyEnable = VK_FALSE;
    samplerInfo.maxAnisotropy = properties.limits.maxSamplerAnisotropy;
    samplerInfo.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
    samplerInfo.unnormalizedCoordinates = VK_FALSE;
    samplerInfo.compareEnable = VK_FALSE;
    samplerInfo.compareOp = VK_COMPARE_OP_ALWAYS;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    samplerInfo.minLod = 0;
    samplerInfo.maxLod = VK_LOD_CLAMP_NONE;

    if (vkCreateSampler(device.getLogicalDevice(), &samplerInfo, nullptr,
                        &textureSampler) != VK_SUCCESS) {
      throw std::runtime_error("failed to create texture sampler!");
    }
  }

  void recordCommandBuffer(int imageIndex, VkCommandBuffer cmdBuffer);
  std::pair<VkImage, VkImageView> createTexture(const AAPLTextureData &);

  std::pair<VkImageView, VkDeviceMemory> createTexture(const std::string &path);

  void createBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                    VkMemoryPropertyFlags properties, VkBuffer &buffer,
                    VkDeviceMemory &bufferMemory) {
    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = size;
    bufferInfo.usage = usage;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateBuffer(device.getLogicalDevice(), &bufferInfo, nullptr,
                       &buffer) != VK_SUCCESS) {
      throw std::runtime_error("failed to create buffer!");
    }

    VkMemoryRequirements memRequirements;
    vkGetBufferMemoryRequirements(device.getLogicalDevice(), buffer,
                                  &memRequirements);

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memRequirements.size;
    allocInfo.memoryTypeIndex =
        device.findMemoryType(memRequirements.memoryTypeBits, properties);

    if (vkAllocateMemory(device.getLogicalDevice(), &allocInfo, nullptr,
                         &bufferMemory) != VK_SUCCESS) {
      throw std::runtime_error("failed to allocate buffer memory!");
    }

    vkBindBufferMemory(device.getLogicalDevice(), buffer, bufferMemory, 0);
  }
  friend class Shadow;
  friend class PointLight;
  friend class SpotLight;
  friend class LightCuller;
  friend class RayTracing;
  FrameConstants frameConstants{
      vec3(-0.17199061810970306f, 0.81795543432235718f, 0.54897010326385498f),  // sunDirection
      vec3(1.0f, 0.95f, 0.8f),   // sunColor
      vec3(0.4f, 0.6f, 1.0f),    // skyColor
      1.f, 10.f, 1.f,            // wetness, emissiveScale, localLightIntensity
      0.1f, 1000.f,              // nearPlane, farPlane
      1.0f                       // scatterScale (Metal parity: unitless multiplier on scatteringCoeff)
  };
};

template <> struct fmt::formatter<vec4> : fmt::formatter<std::string> {
  auto format(vec4 my, format_context &ctx) const -> decltype(ctx.out()) {
    return fmt::format_to(ctx.out(), "[vec4 ={} {} {} {}]", my.x, my.y, my.z,
                          my.w);
  }
};

template <> struct fmt::formatter<vec3> : fmt::formatter<std::string> {
  auto format(vec3 my, format_context &ctx) const -> decltype(ctx.out()) {
    return fmt::format_to(ctx.out(), "[vec3 ={} {} {}]", my.x, my.y, my.z);
  }
};

template <> struct fmt::formatter<vec2> : fmt::formatter<std::string> {
  auto format(vec2 my, format_context &ctx) const -> decltype(ctx.out()) {
    return fmt::format_to(ctx.out(), "[vec2 ={} {}]", my.x, my.y);
  }
};
