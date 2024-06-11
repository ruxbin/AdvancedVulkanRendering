#pragma once
#include "Matrix.h"
#include "Common.h"
#include "VulkanSetup.h"
#include "vulkan/vulkan.h"
#include "Camera.h"
#include "spdlog/spdlog.h"
#include "nlohmann/json.hpp"
#include <stdexcept>
#include <string_view>
#include <vector>
#include <utility>
#include <stdio.h>
#include <filesystem>

struct AAPLTextureData
{
  std::string _path;
  uint32_t _pathHash;
  unsigned long long _width;
  unsigned long long _height;
  unsigned long long _mipmapLevelCount;
  uint32_t _pixelFormat;
  unsigned long long _pixelDataOffset;
  unsigned long long _pixelDataLength;
  std::vector<unsigned long>  _mipOffsets;
  std::vector<unsigned long> _mipLengths;
  AAPLTextureData()=delete;
  AAPLTextureData(FILE* f);
  AAPLTextureData(const AAPLTextureData&)=delete;
  AAPLTextureData(AAPLTextureData&& rhs);
};

struct AAPLMeshData
{
  unsigned long long _vertexCount,_indexCount,_indexType,_chunkCount,_meshCount,_opaqueChunkCount,_opaqueMeshCount,_alphaMaskedChunkCount,_alphaMaskedMeshCount,_transparentChunkCount,_transparentMeshCount,_materialCount;
  void * _vertexData;
  void * _normalData;
  void * _tangentData;
  void * _uvData;
  void * _indexData;
  void * _chunkData;
  void * _meshData;
  void * _materialData;
  uint64_t compressedVertexDataLength;
  uint64_t compressedNormalDataLength;
  uint64_t compressedTangentDataLength;
  uint64_t compressedUvDataLength;
  uint64_t compressedIndexDataLength;
  uint64_t compressedChunkDataLength;
  uint64_t compressedMeshDataLength;
  uint64_t compressedMaterialDataLength;
  std::vector<AAPLTextureData> _textures;
  void * _textureData;
  AAPLMeshData(const char * filepath);
  AAPLMeshData()=delete;
  AAPLMeshData(const AAPLMeshData&)=delete;
  ~AAPLMeshData();
};





struct alignas(16) AAPLShaderMaterial
{
    uint32_t albedo_texture_index;
    uint32_t roughness_texture_index;
    uint32_t normal_texture_index;
    uint32_t emissive_texture_index;
    float alpha;
    uint32_t hasMetallicRoughness;
    uint32_t hasEmissive;
//#if SUPPORT_SPARSE_TEXTURES //TODO:
//    uint baseColorMip;
//    uint metallicRoughnessMip;
//    uint normalMip;
//    uint emissiveMip;
//#endif
};

// A SubMesh represents a group of chunks that share a material.
//  The indices for the chunks in this submesh are contiguous in the index
//  buffer.
struct AAPLSubMesh
{
    uint32_t            materialIndex;          // Material index for this submesh.

    AAPLBoundingBox3    boundingBox;            // Combined bounding box for the chunks in this submesh.
    AAPLSphere          boundingSphere;         // Combined bounding sphere for the chunks in this. submesh.

    unsigned int        indexBegin;             // Offset in mesh index buffer to the indices for this. submesh.
    unsigned int        indexCount;             // Number of indices for this submesh in mesh index. buffer.

    unsigned int        chunkStart;             // Offset in mesh index buffer to the chunks for this. submesh.
    unsigned int        chunkCount;             // Number of chunks for this submesh in mesh index buffer.
};

// Data only structure storing encoded material information.
//TODO: vec4换成vec3后,虽然size还是96，但是uncompress之后的数据全乱了
struct AAPLMaterial
{
    alignas(16) vec4 baseColor;                    // Fallback diffuse color.
    unsigned int  baseColorTextureHash;         // Hash of diffuse texture.
    bool          hasBaseColorTexture;          // Flag indicating a valid diffuse texture index.
    bool          hasDiffuseMask;               // Flag indicating an alpha mask in the diffuse texture.
    alignas(16) vec4 metallicRoughness;            // Fallback metallic roughness.
    unsigned int  metallicRoughnessHash;        // Hash of specular texture.
    bool          hasMetallicRoughnessTexture;  // Flag indicating a valid metallic roughness texture index.
    unsigned int  normalMapHash;                // Hash of normal map texture.
    bool          hasNormalMap;                 // Flag indicating a valid normal map texture index.
    alignas(16) vec4 emissiveColor;                // Fallback emissive color.
    unsigned int  emissiveTextureHash;          // Hash of emissive texture.
    bool          hasEmissiveTexture;           // Flag indicating a valid emissive texture index.
    float         opacity;
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
  VkDescriptorSet globalDescriptor;


  VkDescriptorSetLayout applSetLayout;
  VkDescriptorPool applDescriptorPool;
  VkDescriptorSet applDescriptorSet;


  VkDescriptorSetLayout gpuCullSetLayout;
  VkDescriptorPool gpuCullDescriptorPool;
  VkDescriptorSet gpuCullDescriptorSet;

  VkBuffer uniformBuffer;
  VkDeviceMemory uniformBufferMemory;

  VkBuffer drawParamsBuffer;
  VkDeviceMemory drawParamsBufferMemory;
  VkBuffer cullParamsBuffer;
  VkDeviceMemory cullParamsBufferMemory;
  VkBuffer meshChunksBuffer;
  VkDeviceMemory meshChunksBufferMemory;
  VkBuffer writeIndexBuffer;
  VkDeviceMemory writeIndexBufferMemory;
  VkBuffer chunkIndicesBuffer;
  VkDeviceMemory chunkIndicesBufferMemory;


  VkBuffer vertexBuffer;
  VkDeviceMemory vertexBufferMemory;

  VkBuffer indexBuffer;
  VkDeviceMemory indexBufferMemory;

  VkPipelineLayout pipelineLayout;
  VkPipeline graphicsPipeline;

  VkPipelineLayout epipelineLayout;
  VkPipeline egraphicsPipeline;

  VkCommandBuffer commandBuffer;
  float modelScale;

  Camera *maincamera;

  VkSemaphore imageAvailableSemaphore;
  VkSemaphore renderFinishedSemaphore;
  VkFence inFlightFence;

  AAPLMeshData* applMesh;

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

  VkPipelineLayout encodeDrawBufferPipelineLayout;
  VkPipeline encodeDrawBufferPipeline;


  VkBuffer occluderVertexBuffer;
  VkDeviceMemory occluderVertexBufferMemory;
  VkBuffer occluderIndexBuffer;
  VkDeviceMemory occluderIndexMemory;



  AAPLMeshChunk* m_Chunks;

  //VkImageView currentImage;
  VkSampler textureSampler;
  //VkImage textureImage;
  //VkDeviceMemory textureImageMemory;


  std::vector<std::pair<VkImage, VkImageView>> textures;
  std::unordered_map<uint32_t, size_t> textureHashMap;

  std::vector<AAPLShaderMaterial> materials;

  nlohmann::json sceneFile;

  VkRenderPass occluderZPass;

  VkImage            _depthTexture;
  VkImage            _depthPyramidTexture;
  VkImageView           _depthTextureView;
  VkFramebuffer         _depthFrameBuffer;
  VkFormat		_depthFormat=VK_FORMAT_D32_SFLOAT_S8_UINT;
  VkBuffer		_occludersVertBuffer;
  VkBuffer		_occludersIndexBuffer;
  VkDeviceMemory	_occludersBufferMemory;
  VkDeviceMemory	_occludersIndexBufferMemory;

  VkShaderModule createShaderModule(const std::vector<char> &code);
  void createGraphicsPipeline(VkRenderPass renderPass);
  void createComputePipeline();


  VkPipelineLayout drawOccluderPipelineLayout;
  VkPipeline drawOccluderPipeline;


  std::filesystem::path _rootPath;

  void createRenderOccludersPipeline(VkRenderPass renderPass);

  void createCommandBuffer(VkCommandPool commandPool) {
    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool = commandPool;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = 1;

    if (vkAllocateCommandBuffers(device.getLogicalDevice(), &allocInfo,
                                 &commandBuffer) != VK_SUCCESS) {
      throw std::runtime_error("failed to allocate command buffers!");
    }
    }

    void createSyncObjects();

  public:
    GpuScene(std::filesystem::path& root, const VulkanDevice &deviceref);
    GpuScene() = delete;
    GpuScene(const GpuScene &) = delete;
    void Draw();


    Camera* GetMainCamera() { return maincamera; }
    void init_descriptors(VkImageView);

    void init_descriptorsV2();

    void init_appl_descriptors();

    void init_drawparams_descriptors();
    void DrawChunk(const AAPLMeshChunk&);
    void DrawChunks();

        void CreateTextures();

    void updateSamplerInDescriptors(VkImageView currentImage);

    void ConfigureMaterial(const AAPLMaterial&, AAPLShaderMaterial&);

	
    void CreateDepthTexture();
    void DrawOccluders();
    void CreateOccluderZPass();
    void CreateOccluderZPassFrameBuffer();
    void CreateZdepthView();

    struct uniformBufferData {
      mat4 projectionMatrix;
      mat4 viewMatrix;
    };

    struct gpuCullParams{
        alignas(16) uint32_t totalChunks;
	    Frustum frustum;
    };

    uint32_t findMemoryType(uint32_t typeFilter,
                            VkMemoryPropertyFlags properties) {
      VkPhysicalDeviceMemoryProperties memProperties;
      vkGetPhysicalDeviceMemoryProperties(device.getPhysicalDevice(),
                                          &memProperties);
      for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++) {
        if ((typeFilter & (1 << i)) &&
            (memProperties.memoryTypes[i].propertyFlags & properties) ==
                properties) {
          return i;
        }
      }

      throw std::runtime_error("failed to find suitable memory type!");
    }

    void* loadMipTexture(const AAPLTextureData& texturedata,int,unsigned int&);

    void createUniformBuffer() {
      VkBufferCreateInfo bufferInfo{};
      bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
      bufferInfo.size = sizeof(uniformBufferData);
      bufferInfo.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
      bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
      bufferInfo.flags = 0;
      if (vkCreateBuffer(device.getLogicalDevice(), &bufferInfo, nullptr,
                         &uniformBuffer) != VK_SUCCESS) {
        throw std::runtime_error("failed to create uniform buffer!");
      }
      VkMemoryRequirements memRequirements;
      vkGetBufferMemoryRequirements(device.getLogicalDevice(), uniformBuffer,
                                    &memRequirements);

      VkMemoryAllocateInfo allocInfo{};
      allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
      allocInfo.allocationSize = memRequirements.size;
      allocInfo.memoryTypeIndex =
          findMemoryType(memRequirements.memoryTypeBits,
                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                             VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

      if (vkAllocateMemory(device.getLogicalDevice(), &allocInfo, nullptr,
                           &uniformBufferMemory) != VK_SUCCESS) {
        throw std::runtime_error("failed to allocate uniform buffer memory!");
      }
      vkBindBufferMemory(device.getLogicalDevice(), uniformBuffer,
                         uniformBufferMemory, 0);
    }

    void createTextureSampler()
    {
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(device.getPhysicalDevice(), &properties);
        VkSamplerCreateInfo samplerInfo{};
        samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        samplerInfo.magFilter = VK_FILTER_LINEAR;
        samplerInfo.minFilter = VK_FILTER_LINEAR;
        samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        samplerInfo.anisotropyEnable = VK_TRUE;
        samplerInfo.maxAnisotropy = properties.limits.maxSamplerAnisotropy;
        samplerInfo.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
        samplerInfo.unnormalizedCoordinates = VK_FALSE;
        samplerInfo.compareEnable = VK_FALSE;
        samplerInfo.compareOp = VK_COMPARE_OP_ALWAYS;
        samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;

        if (vkCreateSampler(device.getLogicalDevice(), &samplerInfo, nullptr, &textureSampler) != VK_SUCCESS) {
            throw std::runtime_error("failed to create texture sampler!");
        }
    }
    void recordCommandBuffer(int frameindex);
    std::pair<VkImage, VkImageView> createTexture(const AAPLTextureData&);

    std::pair<VkImageView, VkDeviceMemory> createTexture(const std::string& path);

    void createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties, VkBuffer& buffer, VkDeviceMemory& bufferMemory) {
        VkBufferCreateInfo bufferInfo{};
        bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.size = size;
        bufferInfo.usage = usage;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        if (vkCreateBuffer(device.getLogicalDevice(), &bufferInfo, nullptr, &buffer) != VK_SUCCESS) {
            throw std::runtime_error("failed to create buffer!");
        }

        VkMemoryRequirements memRequirements;
        vkGetBufferMemoryRequirements(device.getLogicalDevice(), buffer, &memRequirements);

        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = memRequirements.size;
        allocInfo.memoryTypeIndex = findMemoryType(memRequirements.memoryTypeBits, properties);

        if (vkAllocateMemory(device.getLogicalDevice(), &allocInfo, nullptr, &bufferMemory) != VK_SUCCESS) {
            throw std::runtime_error("failed to allocate buffer memory!");
        }

        vkBindBufferMemory(device.getLogicalDevice(), buffer, bufferMemory, 0);
    }


};

template<>
struct fmt::formatter<vec4> : fmt::formatter<std::string>
{
    auto format(vec4 my, format_context& ctx) const -> decltype(ctx.out())
    {
        return format_to(ctx.out(), "[vec4 ={} {} {} {}]", my.x, my.y, my.z,my.w);
    }
};

template<>
struct fmt::formatter<vec3> : fmt::formatter<std::string>
{
    auto format(vec3 my, format_context& ctx) const -> decltype(ctx.out())
    {
        return format_to(ctx.out(), "[vec3 ={} {} {}]", my.x, my.y, my.z);
    }
};

template<>
struct fmt::formatter<vec2> : fmt::formatter<std::string>
{
    auto format(vec2 my, format_context& ctx) const -> decltype(ctx.out())
    {
        return format_to(ctx.out(), "[vec2 ={} {}]", my.x, my.y);
    }
};
