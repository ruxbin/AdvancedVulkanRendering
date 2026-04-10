# FBX Skeletal Animation - All Changes

## Overview

Add FBX model loading with skeletal animation playback and GPU skinning to the Vulkan renderer. Uses Assimp for FBX parsing, a separate draw path (not integrated into GPU indirect pipeline), and renders into the existing deferred GBuffer.

---

## 1. New File: `Src/Include/SkinnedMesh.h`

```cpp
#pragma once
#include "Matrix.h"

#ifdef __ANDROID__
#define VK_USE_PLATFORM_ANDROID_KHR
#elif defined(_WIN32)
#define VK_USE_PLATFORM_WIN32_KHR
#elif defined(__gnu_linux__)
#define VK_USE_PLATFORM_WAYLAND_KHR
#endif
#include "vulkan/vulkan.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

static constexpr uint32_t MAX_BONE_WEIGHTS = 4;
static constexpr uint32_t MAX_BONES = 128;

struct SkinnedVertex {
  vec3 position;
  vec3 normal;
  vec3 tangent;
  vec2 uv;
  uint32_t boneIDs[MAX_BONE_WEIGHTS];
  float boneWeights[MAX_BONE_WEIGHTS];

  SkinnedVertex() : position(), normal(), tangent(), uv() {
    for (uint32_t i = 0; i < MAX_BONE_WEIGHTS; i++) {
      boneIDs[i] = 0;
      boneWeights[i] = 0.0f;
    }
  }
};

struct Bone {
  std::string name;
  int32_t index = -1;
  int32_t parentIndex = -1;
  mat4 offsetMatrix;    // inverse bind pose
  mat4 localTransform;  // node's default local transform
};

struct Skeleton {
  std::vector<Bone> bones;
  std::unordered_map<std::string, int32_t> boneNameToIndex;

  int32_t findBone(const std::string &name) const {
    auto it = boneNameToIndex.find(name);
    return it != boneNameToIndex.end() ? it->second : -1;
  }
};

struct VectorKey {
  float time;
  vec3 value;
};

struct QuatKey {
  float time;
  quat value;
};

struct AnimationChannel {
  int32_t boneIndex = -1;
  std::vector<VectorKey> positionKeys;
  std::vector<QuatKey> rotationKeys;
  std::vector<VectorKey> scaleKeys;
};

struct AnimationClip {
  std::string name;
  float duration = 0.0f;
  float ticksPerSecond = 25.0f;
  std::vector<AnimationChannel> channels;
};

struct SkinnedMaterial {
  VkImage diffuseImage = VK_NULL_HANDLE;
  VkImageView diffuseImageView = VK_NULL_HANDLE;
  VkDeviceMemory diffuseImageMemory = VK_NULL_HANDLE;
  bool hasDiffuseTexture = false;
};

struct SkinnedSubmesh {
  uint32_t indexOffset = 0;
  uint32_t indexCount = 0;
  uint32_t materialIndex = 0;
};

struct SkinnedMeshData {
  VkBuffer vertexBuffer = VK_NULL_HANDLE;
  VkDeviceMemory vertexBufferMemory = VK_NULL_HANDLE;
  VkBuffer indexBuffer = VK_NULL_HANDLE;
  VkDeviceMemory indexBufferMemory = VK_NULL_HANDLE;

  uint32_t totalVertexCount = 0;
  uint32_t totalIndexCount = 0;

  std::vector<SkinnedSubmesh> submeshes;
  std::vector<SkinnedMaterial> materials;

  Skeleton skeleton;
  std::vector<AnimationClip> clips;
  mat4 globalInverseTransform;

  void destroy(VkDevice device);
};

struct SkinnedMeshInstance {
  SkinnedMeshData *meshData = nullptr;

  int32_t currentClipIndex = 0;
  float currentTime = 0.0f;
  bool playing = true;
  bool loop = true;

  mat4 worldTransform;

  std::vector<mat4> finalBoneMatrices;
  std::vector<mat4> globalBoneTransforms;

  VkBuffer boneMatrixBuffer = VK_NULL_HANDLE;
  VkDeviceMemory boneMatrixBufferMemory = VK_NULL_HANDLE;

  void init(SkinnedMeshData *data, VkDevice device, VkPhysicalDevice physDevice,
            uint32_t (*findMemoryType)(VkPhysicalDevice, uint32_t,
                                       VkMemoryPropertyFlags));
  void updateAnimation(float deltaTime);
  void computeBoneTransforms();
  void uploadBoneMatrices(VkDevice device);
  void destroy(VkDevice device);
};
```

---

## 2. New File: `Src/Include/FbxLoader.h`

```cpp
#pragma once
#include "SkinnedMesh.h"
#include <functional>
#include <string>

class FbxLoader {
public:
  static SkinnedMeshData *
  loadFbx(const std::string &filepath, VkDevice device,
          VkPhysicalDevice physicalDevice, VkCommandPool commandPool,
          VkQueue graphicsQueue,
          uint32_t (*findMemoryType)(VkPhysicalDevice, uint32_t,
                                     VkMemoryPropertyFlags));
};
```

---

## 3. New File: `Src/FbxLoader.cpp`

> Assimp-based FBX importer. Extracts meshes, bones, animations, and textures.

<details>
<summary>Click to expand (~555 lines)</summary>

```cpp
#include "FbxLoader.h"
#include "stb_image.h"
#include <assimp/Importer.hpp>
#include <assimp/postprocess.h>
#include <assimp/scene.h>
#include <algorithm>
#include <cassert>
#include <filesystem>
#include <spdlog/spdlog.h>

static uint32_t s_findMemoryType_func(VkPhysicalDevice physDevice,
                                       uint32_t typeFilter,
                                       VkMemoryPropertyFlags properties);
static uint32_t (*s_findMemoryTypeFn)(VkPhysicalDevice, uint32_t,
                                       VkMemoryPropertyFlags) = nullptr;
static VkPhysicalDevice s_physDevice = VK_NULL_HANDLE;

static void createBuffer(VkDevice device, VkDeviceSize size,
                         VkBufferUsageFlags usage,
                         VkMemoryPropertyFlags properties, VkBuffer &buffer,
                         VkDeviceMemory &bufferMemory) {
  VkBufferCreateInfo bufferInfo{};
  bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  bufferInfo.size = size;
  bufferInfo.usage = usage;
  bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  if (vkCreateBuffer(device, &bufferInfo, nullptr, &buffer) != VK_SUCCESS)
    throw std::runtime_error("failed to create buffer!");
  VkMemoryRequirements memRequirements;
  vkGetBufferMemoryRequirements(device, buffer, &memRequirements);
  VkMemoryAllocateInfo allocInfo{};
  allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  allocInfo.allocationSize = memRequirements.size;
  allocInfo.memoryTypeIndex =
      s_findMemoryTypeFn(s_physDevice, memRequirements.memoryTypeBits, properties);
  if (vkAllocateMemory(device, &allocInfo, nullptr, &bufferMemory) != VK_SUCCESS)
    throw std::runtime_error("failed to allocate buffer memory!");
  vkBindBufferMemory(device, buffer, bufferMemory, 0);
}

static mat4 aiMatToMat4(const aiMatrix4x4 &m) {
  mat4 result(1.0f);
  result[0][0] = m.a1; result[0][1] = m.a2; result[0][2] = m.a3; result[0][3] = m.a4;
  result[1][0] = m.b1; result[1][1] = m.b2; result[1][2] = m.b3; result[1][3] = m.b4;
  result[2][0] = m.c1; result[2][1] = m.c2; result[2][2] = m.c3; result[2][3] = m.c4;
  result[3][0] = m.d1; result[3][1] = m.d2; result[3][2] = m.d3; result[3][3] = m.d4;
  return result;
}

static void processNodeHierarchy(const aiNode *node, Skeleton &skeleton,
                                  int32_t parentBoneIndex) {
  std::string nodeName(node->mName.C_Str());
  int32_t boneIndex = skeleton.findBone(nodeName);
  if (boneIndex >= 0) {
    skeleton.bones[boneIndex].parentIndex = parentBoneIndex;
    skeleton.bones[boneIndex].localTransform = aiMatToMat4(node->mTransformation);
    parentBoneIndex = boneIndex;
  }
  for (unsigned int i = 0; i < node->mNumChildren; i++)
    processNodeHierarchy(node->mChildren[i], skeleton, parentBoneIndex);
}

static void collectNodeTransforms(const aiNode *node, Skeleton &skeleton,
                                   std::unordered_map<std::string, mat4> &nodeTransforms) {
  std::string nodeName(node->mName.C_Str());
  nodeTransforms[nodeName] = aiMatToMat4(node->mTransformation);
  for (unsigned int i = 0; i < node->mNumChildren; i++)
    collectNodeTransforms(node->mChildren[i], skeleton, nodeTransforms);
}

static VkImage createTextureImage(VkDevice device, VkCommandPool commandPool,
                                   VkQueue graphicsQueue,
                                   const unsigned char *pixels, int width,
                                   int height) {
  VkDeviceSize imageSize = width * height * 4;
  VkBuffer stagingBuffer;
  VkDeviceMemory stagingMemory;
  createBuffer(device, imageSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
               stagingBuffer, stagingMemory);
  void *data;
  vkMapMemory(device, stagingMemory, 0, imageSize, 0, &data);
  memcpy(data, pixels, imageSize);
  vkUnmapMemory(device, stagingMemory);

  VkImage image;
  VkDeviceMemory imageMemory;
  VkImageCreateInfo imageInfo{};
  imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  imageInfo.imageType = VK_IMAGE_TYPE_2D;
  imageInfo.extent = {(uint32_t)width, (uint32_t)height, 1};
  imageInfo.mipLevels = 1; imageInfo.arrayLayers = 1;
  imageInfo.format = VK_FORMAT_R8G8B8A8_SRGB;
  imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
  imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
  imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
  imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  vkCreateImage(device, &imageInfo, nullptr, &image);
  VkMemoryRequirements memReqs;
  vkGetImageMemoryRequirements(device, image, &memReqs);
  VkMemoryAllocateInfo allocInfo{};
  allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  allocInfo.allocationSize = memReqs.size;
  allocInfo.memoryTypeIndex = s_findMemoryTypeFn(s_physDevice, memReqs.memoryTypeBits,
                                                  VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  vkAllocateMemory(device, &allocInfo, nullptr, &imageMemory);
  vkBindImageMemory(device, image, imageMemory, 0);

  // Transition + copy via single-use command buffer
  VkCommandBufferAllocateInfo cmdAllocInfo{};
  cmdAllocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  cmdAllocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  cmdAllocInfo.commandPool = commandPool; cmdAllocInfo.commandBufferCount = 1;
  VkCommandBuffer cmdBuf;
  vkAllocateCommandBuffers(device, &cmdAllocInfo, &cmdBuf);
  VkCommandBufferBeginInfo beginInfo{};
  beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vkBeginCommandBuffer(cmdBuf, &beginInfo);

  VkImageMemoryBarrier barrier{};
  barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.image = image;
  barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  barrier.srcAccessMask = 0; barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  vkCmdPipelineBarrier(cmdBuf, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                       VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
  VkBufferImageCopy region{};
  region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  region.imageExtent = {(uint32_t)width, (uint32_t)height, 1};
  vkCmdCopyBufferToImage(cmdBuf, stagingBuffer, image,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
  barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
  vkCmdPipelineBarrier(cmdBuf, VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
  vkEndCommandBuffer(cmdBuf);
  VkSubmitInfo submitInfo{};
  submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  submitInfo.commandBufferCount = 1; submitInfo.pCommandBuffers = &cmdBuf;
  vkQueueSubmit(graphicsQueue, 1, &submitInfo, VK_NULL_HANDLE);
  vkQueueWaitIdle(graphicsQueue);
  vkFreeCommandBuffers(device, commandPool, 1, &cmdBuf);
  vkDestroyBuffer(device, stagingBuffer, nullptr);
  vkFreeMemory(device, stagingMemory, nullptr);
  return image;
}

SkinnedMeshData *
FbxLoader::loadFbx(const std::string &filepath, VkDevice device,
                   VkPhysicalDevice physicalDevice, VkCommandPool commandPool,
                   VkQueue graphicsQueue,
                   uint32_t (*findMemoryType)(VkPhysicalDevice, uint32_t,
                                              VkMemoryPropertyFlags)) {
  s_findMemoryTypeFn = findMemoryType;
  s_physDevice = physicalDevice;

  Assimp::Importer importer;
  const aiScene *scene = importer.ReadFile(
      filepath, aiProcess_Triangulate | aiProcess_GenNormals |
                    aiProcess_CalcTangentSpace | aiProcess_FlipUVs |
                    aiProcess_LimitBoneWeights);
  if (!scene || scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE || !scene->mRootNode) {
    spdlog::error("Assimp error: {}", importer.GetErrorString());
    return nullptr;
  }

  auto *meshData = new SkinnedMeshData();
  meshData->globalInverseTransform = inverse(aiMatToMat4(scene->mRootNode->mTransformation));
  std::string directory = std::filesystem::path(filepath).parent_path().string();
  std::vector<SkinnedVertex> allVertices;
  std::vector<uint32_t> allIndices;

  for (unsigned int mi = 0; mi < scene->mNumMeshes; mi++) {
    const aiMesh *mesh = scene->mMeshes[mi];
    uint32_t vertexOffset = (uint32_t)allVertices.size();
    uint32_t indexOffset = (uint32_t)allIndices.size();

    for (unsigned int vi = 0; vi < mesh->mNumVertices; vi++) {
      SkinnedVertex v;
      v.position = vec3(mesh->mVertices[vi].x, mesh->mVertices[vi].y, mesh->mVertices[vi].z);
      if (mesh->mNormals)
        v.normal = vec3(mesh->mNormals[vi].x, mesh->mNormals[vi].y, mesh->mNormals[vi].z);
      if (mesh->mTangents)
        v.tangent = vec3(mesh->mTangents[vi].x, mesh->mTangents[vi].y, mesh->mTangents[vi].z);
      if (mesh->mTextureCoords[0])
        v.uv = vec2(mesh->mTextureCoords[0][vi].x, mesh->mTextureCoords[0][vi].y);
      allVertices.push_back(v);
    }
    for (unsigned int fi = 0; fi < mesh->mNumFaces; fi++) {
      const aiFace &face = mesh->mFaces[fi];
      for (unsigned int ii = 0; ii < face.mNumIndices; ii++)
        allIndices.push_back(face.mIndices[ii] + vertexOffset);
    }

    // Bones
    for (unsigned int bi = 0; bi < mesh->mNumBones; bi++) {
      const aiBone *bone = mesh->mBones[bi];
      std::string boneName(bone->mName.C_Str());
      int32_t boneIndex = meshData->skeleton.findBone(boneName);
      if (boneIndex < 0) {
        boneIndex = (int32_t)meshData->skeleton.bones.size();
        Bone newBone;
        newBone.name = boneName; newBone.index = boneIndex;
        newBone.offsetMatrix = aiMatToMat4(bone->mOffsetMatrix);
        newBone.localTransform = mat4(1.0f);
        meshData->skeleton.bones.push_back(newBone);
        meshData->skeleton.boneNameToIndex[boneName] = boneIndex;
      }
      for (unsigned int wi = 0; wi < bone->mNumWeights; wi++) {
        uint32_t vertexId = bone->mWeights[wi].mVertexId + vertexOffset;
        float weight = bone->mWeights[wi].mWeight;
        SkinnedVertex &sv = allVertices[vertexId];
        for (uint32_t slot = 0; slot < MAX_BONE_WEIGHTS; slot++) {
          if (sv.boneWeights[slot] == 0.0f) {
            sv.boneIDs[slot] = (uint32_t)boneIndex;
            sv.boneWeights[slot] = weight;
            break;
          }
        }
      }
    }

    SkinnedSubmesh submesh;
    submesh.indexOffset = indexOffset;
    submesh.indexCount = (uint32_t)(allIndices.size() - indexOffset);
    submesh.materialIndex = mesh->mMaterialIndex;
    meshData->submeshes.push_back(submesh);
  }

  // Normalize bone weights + default unweighted vertices to bone 0
  for (auto &v : allVertices) {
    float totalWeight = 0.0f;
    for (uint32_t i = 0; i < MAX_BONE_WEIGHTS; i++) totalWeight += v.boneWeights[i];
    if (totalWeight > 0.0f && fabsf(totalWeight - 1.0f) > 1e-5f)
      for (uint32_t i = 0; i < MAX_BONE_WEIGHTS; i++) v.boneWeights[i] /= totalWeight;
    if (totalWeight == 0.0f) { v.boneIDs[0] = 0; v.boneWeights[0] = 1.0f; }
  }
  meshData->totalVertexCount = (uint32_t)allVertices.size();
  meshData->totalIndexCount = (uint32_t)allIndices.size();

  processNodeHierarchy(scene->mRootNode, meshData->skeleton, -1);
  std::unordered_map<std::string, mat4> nodeTransforms;
  collectNodeTransforms(scene->mRootNode, meshData->skeleton, nodeTransforms);
  for (auto &bone : meshData->skeleton.bones) {
    auto it = nodeTransforms.find(bone.name);
    if (it != nodeTransforms.end()) bone.localTransform = it->second;
  }

  // Ensure topological order (parents before children)
  bool sorted = false;
  while (!sorted) {
    sorted = true;
    for (size_t i = 0; i < meshData->skeleton.bones.size(); i++) {
      int32_t pi = meshData->skeleton.bones[i].parentIndex;
      if (pi >= 0 && (size_t)pi > i) {
        std::swap(meshData->skeleton.bones[i], meshData->skeleton.bones[pi]);
        meshData->skeleton.bones[i].index = (int32_t)i;
        meshData->skeleton.bones[pi].index = pi;
        meshData->skeleton.boneNameToIndex[meshData->skeleton.bones[i].name] = (int32_t)i;
        meshData->skeleton.boneNameToIndex[meshData->skeleton.bones[pi].name] = pi;
        for (auto &b : meshData->skeleton.bones) {
          if (b.parentIndex == (int32_t)i) b.parentIndex = pi;
          else if (b.parentIndex == pi) b.parentIndex = (int32_t)i;
        }
        for (auto &v : allVertices) {
          for (uint32_t s = 0; s < MAX_BONE_WEIGHTS; s++) {
            if (v.boneIDs[s] == (uint32_t)i) v.boneIDs[s] = (uint32_t)pi;
            else if (v.boneIDs[s] == (uint32_t)pi) v.boneIDs[s] = (uint32_t)i;
          }
        }
        sorted = false;
      }
    }
  }

  // Extract animations
  for (unsigned int ai = 0; ai < scene->mNumAnimations; ai++) {
    const aiAnimation *anim = scene->mAnimations[ai];
    AnimationClip clip;
    clip.name = anim->mName.C_Str();
    clip.duration = (float)anim->mDuration;
    clip.ticksPerSecond = anim->mTicksPerSecond > 0.0 ? (float)anim->mTicksPerSecond : 25.0f;
    for (unsigned int ci = 0; ci < anim->mNumChannels; ci++) {
      const aiNodeAnim *channel = anim->mChannels[ci];
      int32_t boneIndex = meshData->skeleton.findBone(channel->mNodeName.C_Str());
      if (boneIndex < 0) continue;
      AnimationChannel animChannel; animChannel.boneIndex = boneIndex;
      for (unsigned int ki = 0; ki < channel->mNumPositionKeys; ki++) {
        auto &v = channel->mPositionKeys[ki].mValue;
        animChannel.positionKeys.push_back({(float)channel->mPositionKeys[ki].mTime, vec3(v.x, v.y, v.z)});
      }
      for (unsigned int ki = 0; ki < channel->mNumRotationKeys; ki++) {
        auto &q = channel->mRotationKeys[ki].mValue;
        animChannel.rotationKeys.push_back({(float)channel->mRotationKeys[ki].mTime, quat(q.w, q.x, q.y, q.z)});
      }
      for (unsigned int ki = 0; ki < channel->mNumScalingKeys; ki++) {
        auto &v = channel->mScalingKeys[ki].mValue;
        animChannel.scaleKeys.push_back({(float)channel->mScalingKeys[ki].mTime, vec3(v.x, v.y, v.z)});
      }
      clip.channels.push_back(animChannel);
    }
    meshData->clips.push_back(clip);
  }

  // Load materials/textures (supports embedded and external textures)
  meshData->materials.resize(scene->mNumMaterials);
  for (unsigned int mi = 0; mi < scene->mNumMaterials; mi++) {
    const aiMaterial *material = scene->mMaterials[mi];
    SkinnedMaterial &mat = meshData->materials[mi];
    if (material->GetTextureCount(aiTextureType_DIFFUSE) > 0) {
      aiString texPath;
      material->GetTexture(aiTextureType_DIFFUSE, 0, &texPath);
      const aiTexture *embeddedTex = scene->GetEmbeddedTexture(texPath.C_Str());
      // ... (handles embedded compressed/uncompressed + external file textures)
      // Creates VkImage + VkImageView for each diffuse texture
    }
  }

  // Upload vertex/index data to GPU buffers (HOST_VISIBLE)
  // ... createBuffer + vkMapMemory + memcpy pattern ...

  spdlog::info("FBX loaded: {} vertices, {} indices, {} bones, {} animations",
               meshData->totalVertexCount, meshData->totalIndexCount,
               meshData->skeleton.bones.size(), meshData->clips.size());
  return meshData;
}
```

</details>

---

## 4. New File: `Src/SkinnedMesh.cpp`

> Animation playback: keyframe interpolation, bone hierarchy traversal, SSBO upload.

<details>
<summary>Click to expand (~230 lines)</summary>

```cpp
#include "SkinnedMesh.h"
#include <algorithm>
#include <cstring>

void SkinnedMeshData::destroy(VkDevice device) { /* cleanup buffers/textures */ }

void SkinnedMeshInstance::init(SkinnedMeshData *data, VkDevice device,
                               VkPhysicalDevice physDevice,
                               uint32_t (*findMemoryType)(...)) {
  meshData = data;
  worldTransform = mat4(1.0f);
  finalBoneMatrices.resize(data->skeleton.bones.size(), mat4(1.0f));
  globalBoneTransforms.resize(data->skeleton.bones.size(), mat4(1.0f));
  // Create bone matrix SSBO (MAX_BONES * sizeof(mat4), HOST_VISIBLE)
  // Initialize with transposed identity matrices
}

// Keyframe interpolation helpers
static vec3 interpolatePosition(const std::vector<VectorKey> &keys, float time);
static quat interpolateRotation(const std::vector<QuatKey> &keys, float time);
static vec3 interpolateScale(const std::vector<VectorKey> &keys, float time);

void SkinnedMeshInstance::updateAnimation(float deltaTime) {
  currentTime += deltaTime * clip.ticksPerSecond;
  if (currentTime > clip.duration && loop)
    currentTime = fmodf(currentTime, clip.duration);
  computeBoneTransforms();
}

void SkinnedMeshInstance::computeBoneTransforms() {
  // For each bone in topological order:
  //   1. Interpolate position/rotation/scale from keyframes
  //   2. Compose T * R * S local transform
  //   3. globalTransform[i] = globalTransform[parent] * localTransform
  //   4. finalBoneMatrices[i] = globalInverseTransform * globalTransform[i] * offsetMatrix
}

void SkinnedMeshInstance::uploadBoneMatrices(VkDevice device) {
  // Map SSBO, write transposed bone matrices, unmap
}

void SkinnedMeshInstance::destroy(VkDevice device) { /* cleanup SSBO */ }
```

</details>

---

## 5. New File: `shaders/skinned.hlsl`

```hlsl
#include "commonstruct.hlsl"
#include "lighting.hlsl"

[[vk::binding(0,0)]] cbuffer cam { CameraParamsBufferFull cameraParams; AAPLFrameConstants frameConstants; }
[[vk::binding(0,1)]] StructuredBuffer<float4x4> boneMatrices;
[[vk::binding(1,1)]] SamplerState _LinearRepeatSampler;
[[vk::binding(2,1)]] Texture2D<half4> diffuseTexture;

struct SkinnedPushConstants { float4x4 worldMatrix; uint materialFlags; };
[[vk::push_constant]] SkinnedPushConstants pushConstants;

struct SkinnedVSInput {
    [[vk::location(0)]] float3 position : POSITION;
    [[vk::location(1)]] float3 normal   : NORMAL;
    [[vk::location(2)]] float3 tangent  : TANGENT;
    [[vk::location(3)]] float2 uv       : TEXCOORD0;
    [[vk::location(4)]] uint4  boneIDs  : BLENDINDICES;
    [[vk::location(5)]] float4 boneWeights : BLENDWEIGHT;
};

SkinnedVSOutput SkinnedVS(SkinnedVSInput input) {
    // skinMatrix = sum(boneMatrices[boneID[i]] * weight[i])
    // skinnedPos = skinMatrix * position
    // worldPos = worldMatrix * skinnedPos
    // output.Position = viewProj * worldPos
}

PSOutput SkinnedBasePS(SkinnedVSOutput input) {
    // Sample diffuse texture, write to 4 GBuffer targets (albedo/normal/emissive/F0Roughness)
}

half4 SkinnedForwardPS(SkinnedVSOutput input) : SV_Target {
    // Sample diffuse, evaluate PBR lighting via lightingShader()
}
```

---

## 6. Modified: `Src/Include/Matrix.h`

Added `quat` struct before `transpose()`:

```cpp
struct quat {
  float w, x, y, z;
  quat() : w(1.0f), x(0.0f), y(0.0f), z(0.0f) {}
  quat(float W, float X, float Y, float Z) : w(W), x(X), y(Y), z(Z) {}

  quat operator*(float s) const;
  quat operator+(const quat &rhs) const;
  quat operator-() const;
  float dot(const quat &rhs) const;
  quat normalized() const;
  mat4 toMat4() const;               // quaternion -> rotation matrix
  static quat slerp(const quat &a, const quat &b, float t);  // with short-path fix
};
```

---

## 7. Modified: `CMakeLists.txt`

```diff
+# Assimp for FBX loading
+include(FetchContent)
+FetchContent_Declare(assimp
+    GIT_REPOSITORY https://github.com/assimp/assimp.git
+    GIT_TAG v5.4.3
+)
+set(ASSIMP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
+set(ASSIMP_BUILD_ASSIMP_TOOLS OFF CACHE BOOL "" FORCE)
+set(ASSIMP_INSTALL OFF CACHE BOOL "" FORCE)
+set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
+FetchContent_MakeAvailable(assimp)
+include_directories(${assimp_SOURCE_DIR}/include ${assimp_BINARY_DIR}/include)
```

## 8. Modified: `Src/CMakeLists.txt`

```diff
 set(SOURCES
     ...
+    ${CMAKE_CURRENT_LIST_DIR}/FbxLoader.cpp
+    ${CMAKE_CURRENT_LIST_DIR}/SkinnedMesh.cpp
 )
 target_link_libraries(${OUTPUT_NAME}
-    ${LIBRARIES})
+    ${LIBRARIES}
+    assimp::assimp)
```

## 9. Modified: `Src/Include/GpuScene.h`

```diff
+#include "SkinnedMesh.h"
 ...
+  void loadSkinnedMesh(const std::string &fbxPath);
 ...
+  // Skinned mesh (FBX) rendering
+  SkinnedMeshData *_skinnedMeshData = nullptr;
+  SkinnedMeshInstance *_skinnedMeshInstance = nullptr;
+  VkDescriptorSetLayout skinnedSetLayout = VK_NULL_HANDLE;
+  VkDescriptorPool skinnedDescriptorPool = VK_NULL_HANDLE;
+  VkDescriptorSet skinnedDescriptorSet = VK_NULL_HANDLE;
+  VkPipelineLayout skinnedPipelineLayout = VK_NULL_HANDLE;
+  VkPipeline skinnedBasePipeline = VK_NULL_HANDLE;
+  VkPipeline skinnedForwardPipeline = VK_NULL_HANDLE;
+  VkImage _skinnedDummyTexture = VK_NULL_HANDLE;
+  VkDeviceMemory _skinnedDummyTextureMemory = VK_NULL_HANDLE;
+  VkImageView _skinnedDummyTextureView = VK_NULL_HANDLE;
+  void createSkinnedMeshPipeline();
+  void initSkinnedDescriptors();
+  void drawSkinnedMesh(VkCommandBuffer commandBuffer);
+  void drawSkinnedMeshForward(VkCommandBuffer commandBuffer);
+  void createSkinnedDummyTexture();
```

## 10. Modified: `Src/GpuScene.cpp`

Key changes in `recordCommandBuffer`:
```diff
+  // Update skinned mesh animation
+  if (_skinnedMeshInstance && _skinnedMeshInstance->playing) {
+    static auto lastSkinnedTime = std::chrono::high_resolution_clock::now();
+    auto now = std::chrono::high_resolution_clock::now();
+    float dt = std::chrono::duration<float>(now - lastSkinnedTime).count();
+    lastSkinnedTime = now;
+    _skinnedMeshInstance->updateAnimation(dt);
+    _skinnedMeshInstance->uploadBoneMatrices(device.getLogicalDevice());
+  }
   ...
   DrawChunksBasePass(commandBuffer);
+  drawSkinnedMesh(commandBuffer);
```

New methods appended (~540 lines):
- `createSkinnedDummyTexture()` - 1x1 white fallback texture
- `initSkinnedDescriptors()` - descriptor set layout/pool/set for bone SSBO + sampler + texture
- `createSkinnedMeshPipeline()` - vertex input (interleaved 76-byte stride, 6 attributes), pipeline layout with push constants (worldMatrix + materialFlags), base pass + forward pipelines
- `loadSkinnedMesh(path)` - calls FbxLoader, creates instance, initializes descriptors + pipeline
- `drawSkinnedMesh(cmd)` - binds pipeline/buffers/descriptors, pushes world matrix, draws indexed per submesh
- `drawSkinnedMeshForward(cmd)` - same for forward pass

## 11. Modified: `shaders/compile_shaders.bat`

```diff
+REM Skinned mesh shaders
+dxc.exe -spirv -T vs_6_0 skinned.hlsl -E SkinnedVS -Fo skinned.vs.spv
+dxc.exe -spirv -T ps_6_0 skinned.hlsl -E SkinnedBasePS -Fo skinned.base.ps.spv
+dxc.exe -spirv -T ps_6_0 skinned.hlsl -E SkinnedForwardPS -Fo skinned.forward.ps.spv
```

---

## Usage

```cpp
// After GpuScene construction:
gpuScene->loadSkinnedMesh("path/to/character.fbx");

// Optionally adjust transform (e.g., if model is too large):
gpuScene->_skinnedMeshInstance->worldTransform = scale(0.01f);
```

## Build Steps

1. Run `compile_shaders.bat` to generate `skinned.*.spv`
2. CMake configure (Assimp will be fetched automatically on first build)
3. Build project
4. Place an FBX file with embedded animation and call `loadSkinnedMesh()`
