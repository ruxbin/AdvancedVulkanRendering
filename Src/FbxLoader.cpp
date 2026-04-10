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

  if (vkCreateBuffer(device, &bufferInfo, nullptr, &buffer) != VK_SUCCESS) {
    throw std::runtime_error("failed to create buffer!");
  }

  VkMemoryRequirements memRequirements;
  vkGetBufferMemoryRequirements(device, buffer, &memRequirements);

  VkMemoryAllocateInfo allocInfo{};
  allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  allocInfo.allocationSize = memRequirements.size;
  allocInfo.memoryTypeIndex =
      s_findMemoryTypeFn(s_physDevice, memRequirements.memoryTypeBits, properties);

  if (vkAllocateMemory(device, &allocInfo, nullptr, &bufferMemory) !=
      VK_SUCCESS) {
    throw std::runtime_error("failed to allocate buffer memory!");
  }

  vkBindBufferMemory(device, buffer, bufferMemory, 0);
}

static mat4 aiMatToMat4(const aiMatrix4x4 &m) {
  mat4 result(1.0f);
  // Assimp is row-major: m[row][col]
  // Engine mat4 stores m[col][row]
  result[0][0] = m.a1; result[0][1] = m.a2; result[0][2] = m.a3; result[0][3] = m.a4;
  result[1][0] = m.b1; result[1][1] = m.b2; result[1][2] = m.b3; result[1][3] = m.b4;
  result[2][0] = m.c1; result[2][1] = m.c2; result[2][2] = m.c3; result[2][3] = m.c4;
  result[3][0] = m.d1; result[3][1] = m.d2; result[3][2] = m.d3; result[3][3] = m.d4;
  return result;
}

static void processNodeHierarchy(const aiNode *node,
                                  Skeleton &skeleton,
                                  int32_t parentBoneIndex) {
  std::string nodeName(node->mName.C_Str());
  int32_t boneIndex = skeleton.findBone(nodeName);

  if (boneIndex >= 0) {
    skeleton.bones[boneIndex].parentIndex = parentBoneIndex;
    skeleton.bones[boneIndex].localTransform = aiMatToMat4(node->mTransformation);
    parentBoneIndex = boneIndex;
  }

  for (unsigned int i = 0; i < node->mNumChildren; i++) {
    processNodeHierarchy(node->mChildren[i], skeleton, parentBoneIndex);
  }
}

// Collect all node names that are in the bone hierarchy path
static void collectNodeTransforms(const aiNode *node, Skeleton &skeleton,
                                   std::unordered_map<std::string, mat4> &nodeTransforms) {
  std::string nodeName(node->mName.C_Str());
  nodeTransforms[nodeName] = aiMatToMat4(node->mTransformation);

  // If this node is not a bone but is on the path to bones, we still need it
  // for computing global transforms
  for (unsigned int i = 0; i < node->mNumChildren; i++) {
    collectNodeTransforms(node->mChildren[i], skeleton, nodeTransforms);
  }
}

static VkImage createTextureImage(VkDevice device, VkCommandPool commandPool,
                                   VkQueue graphicsQueue,
                                   const unsigned char *pixels, int width,
                                   int height) {
  VkDeviceSize imageSize = width * height * 4;

  VkBuffer stagingBuffer;
  VkDeviceMemory stagingMemory;
  createBuffer(device, imageSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                   VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
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
  imageInfo.extent.width = width;
  imageInfo.extent.height = height;
  imageInfo.extent.depth = 1;
  imageInfo.mipLevels = 1;
  imageInfo.arrayLayers = 1;
  imageInfo.format = VK_FORMAT_R8G8B8A8_SRGB;
  imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
  imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  imageInfo.usage =
      VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
  imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
  imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

  vkCreateImage(device, &imageInfo, nullptr, &image);

  VkMemoryRequirements memReqs;
  vkGetImageMemoryRequirements(device, image, &memReqs);

  VkMemoryAllocateInfo allocInfo{};
  allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  allocInfo.allocationSize = memReqs.size;
  allocInfo.memoryTypeIndex = s_findMemoryTypeFn(
      s_physDevice, memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

  vkAllocateMemory(device, &allocInfo, nullptr, &imageMemory);
  vkBindImageMemory(device, image, imageMemory, 0);

  // Transition + copy via single-use command buffer
  VkCommandBufferAllocateInfo cmdAllocInfo{};
  cmdAllocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  cmdAllocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  cmdAllocInfo.commandPool = commandPool;
  cmdAllocInfo.commandBufferCount = 1;

  VkCommandBuffer cmdBuf;
  vkAllocateCommandBuffers(device, &cmdAllocInfo, &cmdBuf);

  VkCommandBufferBeginInfo beginInfo{};
  beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vkBeginCommandBuffer(cmdBuf, &beginInfo);

  // Transition to TRANSFER_DST
  VkImageMemoryBarrier barrier{};
  barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.image = image;
  barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  barrier.subresourceRange.baseMipLevel = 0;
  barrier.subresourceRange.levelCount = 1;
  barrier.subresourceRange.baseArrayLayer = 0;
  barrier.subresourceRange.layerCount = 1;
  barrier.srcAccessMask = 0;
  barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  vkCmdPipelineBarrier(cmdBuf, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                       VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                       nullptr, 1, &barrier);

  VkBufferImageCopy region{};
  region.bufferOffset = 0;
  region.bufferRowLength = 0;
  region.bufferImageHeight = 0;
  region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  region.imageSubresource.mipLevel = 0;
  region.imageSubresource.baseArrayLayer = 0;
  region.imageSubresource.layerCount = 1;
  region.imageOffset = {0, 0, 0};
  region.imageExtent = {(uint32_t)width, (uint32_t)height, 1};
  vkCmdCopyBufferToImage(cmdBuf, stagingBuffer, image,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

  // Transition to SHADER_READ_ONLY
  barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
  vkCmdPipelineBarrier(cmdBuf, VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0,
                       nullptr, 1, &barrier);

  vkEndCommandBuffer(cmdBuf);

  VkSubmitInfo submitInfo{};
  submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  submitInfo.commandBufferCount = 1;
  submitInfo.pCommandBuffers = &cmdBuf;
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
      filepath,
      aiProcess_Triangulate | aiProcess_GenNormals |
          aiProcess_CalcTangentSpace | aiProcess_FlipUVs |
          aiProcess_LimitBoneWeights);

  if (!scene || scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE ||
      !scene->mRootNode) {
    spdlog::error("Assimp error: {}", importer.GetErrorString());
    return nullptr;
  }

  auto *meshData = new SkinnedMeshData();
  meshData->globalInverseTransform =
      inverse(aiMatToMat4(scene->mRootNode->mTransformation));

  std::string directory =
      std::filesystem::path(filepath).parent_path().string();

  // Gather all vertices and indices from all meshes
  std::vector<SkinnedVertex> allVertices;
  std::vector<uint32_t> allIndices;

  for (unsigned int mi = 0; mi < scene->mNumMeshes; mi++) {
    const aiMesh *mesh = scene->mMeshes[mi];

    uint32_t vertexOffset = (uint32_t)allVertices.size();
    uint32_t indexOffset = (uint32_t)allIndices.size();

    // Vertices
    for (unsigned int vi = 0; vi < mesh->mNumVertices; vi++) {
      SkinnedVertex v;
      v.position = vec3(mesh->mVertices[vi].x, mesh->mVertices[vi].y,
                        mesh->mVertices[vi].z);
      if (mesh->mNormals) {
        v.normal = vec3(mesh->mNormals[vi].x, mesh->mNormals[vi].y,
                        mesh->mNormals[vi].z);
      }
      if (mesh->mTangents) {
        v.tangent = vec3(mesh->mTangents[vi].x, mesh->mTangents[vi].y,
                         mesh->mTangents[vi].z);
      }
      if (mesh->mTextureCoords[0]) {
        v.uv = vec2(mesh->mTextureCoords[0][vi].x,
                     mesh->mTextureCoords[0][vi].y);
      }
      allVertices.push_back(v);
    }

    // Indices
    for (unsigned int fi = 0; fi < mesh->mNumFaces; fi++) {
      const aiFace &face = mesh->mFaces[fi];
      for (unsigned int ii = 0; ii < face.mNumIndices; ii++) {
        allIndices.push_back(face.mIndices[ii] + vertexOffset);
      }
    }

    // Bones
    for (unsigned int bi = 0; bi < mesh->mNumBones; bi++) {
      const aiBone *bone = mesh->mBones[bi];
      std::string boneName(bone->mName.C_Str());

      int32_t boneIndex = meshData->skeleton.findBone(boneName);
      if (boneIndex < 0) {
        boneIndex = (int32_t)meshData->skeleton.bones.size();
        Bone newBone;
        newBone.name = boneName;
        newBone.index = boneIndex;
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

    // Submesh
    SkinnedSubmesh submesh;
    submesh.indexOffset = indexOffset;
    submesh.indexCount = (uint32_t)(allIndices.size() - indexOffset);
    submesh.materialIndex = mesh->mMaterialIndex;
    meshData->submeshes.push_back(submesh);
  }

  // Normalize bone weights
  for (auto &v : allVertices) {
    float totalWeight = 0.0f;
    for (uint32_t i = 0; i < MAX_BONE_WEIGHTS; i++)
      totalWeight += v.boneWeights[i];
    if (totalWeight > 0.0f && fabsf(totalWeight - 1.0f) > 1e-5f) {
      for (uint32_t i = 0; i < MAX_BONE_WEIGHTS; i++)
        v.boneWeights[i] /= totalWeight;
    }
    // Vertices with no bone influence: bind to bone 0 with weight 1
    if (totalWeight == 0.0f) {
      v.boneIDs[0] = 0;
      v.boneWeights[0] = 1.0f;
    }
  }

  meshData->totalVertexCount = (uint32_t)allVertices.size();
  meshData->totalIndexCount = (uint32_t)allIndices.size();

  // Build bone hierarchy from scene node tree
  processNodeHierarchy(scene->mRootNode, meshData->skeleton, -1);

  // Store all node transforms for animation evaluation
  std::unordered_map<std::string, mat4> nodeTransforms;
  collectNodeTransforms(scene->mRootNode, meshData->skeleton, nodeTransforms);
  for (auto &bone : meshData->skeleton.bones) {
    auto it = nodeTransforms.find(bone.name);
    if (it != nodeTransforms.end()) {
      bone.localTransform = it->second;
    }
  }

  // Ensure topological order (parents before children)
  // Already guaranteed by processNodeHierarchy if bones are added in DFS order
  // But let's verify - if a bone's parent has a higher index, swap
  bool sorted = false;
  while (!sorted) {
    sorted = true;
    for (size_t i = 0; i < meshData->skeleton.bones.size(); i++) {
      int32_t pi = meshData->skeleton.bones[i].parentIndex;
      if (pi >= 0 && (size_t)pi > i) {
        // Swap bone i and pi
        std::swap(meshData->skeleton.bones[i], meshData->skeleton.bones[pi]);
        meshData->skeleton.bones[i].index = (int32_t)i;
        meshData->skeleton.bones[pi].index = pi;
        meshData->skeleton.boneNameToIndex[meshData->skeleton.bones[i].name] = (int32_t)i;
        meshData->skeleton.boneNameToIndex[meshData->skeleton.bones[pi].name] = pi;
        // Fix parent references
        for (auto &b : meshData->skeleton.bones) {
          if (b.parentIndex == (int32_t)i)
            b.parentIndex = pi;
          else if (b.parentIndex == pi)
            b.parentIndex = (int32_t)i;
        }
        // Fix vertex bone IDs
        for (auto &v : allVertices) {
          for (uint32_t s = 0; s < MAX_BONE_WEIGHTS; s++) {
            if (v.boneIDs[s] == (uint32_t)i)
              v.boneIDs[s] = (uint32_t)pi;
            else if (v.boneIDs[s] == (uint32_t)pi)
              v.boneIDs[s] = (uint32_t)i;
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
    clip.ticksPerSecond =
        anim->mTicksPerSecond > 0.0 ? (float)anim->mTicksPerSecond : 25.0f;

    for (unsigned int ci = 0; ci < anim->mNumChannels; ci++) {
      const aiNodeAnim *channel = anim->mChannels[ci];
      std::string boneName(channel->mNodeName.C_Str());
      int32_t boneIndex = meshData->skeleton.findBone(boneName);

      if (boneIndex < 0)
        continue;

      AnimationChannel animChannel;
      animChannel.boneIndex = boneIndex;

      for (unsigned int ki = 0; ki < channel->mNumPositionKeys; ki++) {
        VectorKey key;
        key.time = (float)channel->mPositionKeys[ki].mTime;
        auto &v = channel->mPositionKeys[ki].mValue;
        key.value = vec3(v.x, v.y, v.z);
        animChannel.positionKeys.push_back(key);
      }

      for (unsigned int ki = 0; ki < channel->mNumRotationKeys; ki++) {
        QuatKey key;
        key.time = (float)channel->mRotationKeys[ki].mTime;
        auto &q = channel->mRotationKeys[ki].mValue;
        key.value = quat(q.w, q.x, q.y, q.z);
        animChannel.rotationKeys.push_back(key);
      }

      for (unsigned int ki = 0; ki < channel->mNumScalingKeys; ki++) {
        VectorKey key;
        key.time = (float)channel->mScalingKeys[ki].mTime;
        auto &v = channel->mScalingKeys[ki].mValue;
        key.value = vec3(v.x, v.y, v.z);
        animChannel.scaleKeys.push_back(key);
      }

      clip.channels.push_back(animChannel);
    }
    meshData->clips.push_back(clip);
  }

  // Load materials/textures
  meshData->materials.resize(scene->mNumMaterials);
  for (unsigned int mi = 0; mi < scene->mNumMaterials; mi++) {
    const aiMaterial *material = scene->mMaterials[mi];
    SkinnedMaterial &mat = meshData->materials[mi];

    if (material->GetTextureCount(aiTextureType_DIFFUSE) > 0) {
      aiString texPath;
      material->GetTexture(aiTextureType_DIFFUSE, 0, &texPath);

      // Check for embedded texture
      const aiTexture *embeddedTex = scene->GetEmbeddedTexture(texPath.C_Str());
      if (embeddedTex) {
        int w, h, channels;
        unsigned char *pixels = nullptr;
        if (embeddedTex->mHeight == 0) {
          // Compressed embedded texture
          pixels = stbi_load_from_memory(
              (const unsigned char *)embeddedTex->pcData,
              embeddedTex->mWidth, &w, &h, &channels, 4);
        } else {
          w = embeddedTex->mWidth;
          h = embeddedTex->mHeight;
          pixels = (unsigned char *)malloc(w * h * 4);
          for (int py = 0; py < h; py++) {
            for (int px = 0; px < w; px++) {
              auto &texel = embeddedTex->pcData[py * w + px];
              int idx = (py * w + px) * 4;
              pixels[idx + 0] = texel.r;
              pixels[idx + 1] = texel.g;
              pixels[idx + 2] = texel.b;
              pixels[idx + 3] = texel.a;
            }
          }
        }
        if (pixels) {
          mat.diffuseImage = createTextureImage(device, commandPool,
                                                 graphicsQueue, pixels, w, h);
          mat.hasDiffuseTexture = true;
          stbi_image_free(pixels);

          VkImageViewCreateInfo viewInfo{};
          viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
          viewInfo.image = mat.diffuseImage;
          viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
          viewInfo.format = VK_FORMAT_R8G8B8A8_SRGB;
          viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
          viewInfo.subresourceRange.baseMipLevel = 0;
          viewInfo.subresourceRange.levelCount = 1;
          viewInfo.subresourceRange.baseArrayLayer = 0;
          viewInfo.subresourceRange.layerCount = 1;
          vkCreateImageView(device, &viewInfo, nullptr, &mat.diffuseImageView);
        }
      } else {
        // External texture file
        std::string texFilePath = directory + "/" + texPath.C_Str();
        // Try backslash path too
        std::replace(texFilePath.begin(), texFilePath.end(), '\\', '/');

        int w, h, channels;
        unsigned char *pixels =
            stbi_load(texFilePath.c_str(), &w, &h, &channels, 4);
        if (pixels) {
          mat.diffuseImage = createTextureImage(device, commandPool,
                                                 graphicsQueue, pixels, w, h);
          mat.hasDiffuseTexture = true;
          stbi_image_free(pixels);

          VkImageViewCreateInfo viewInfo{};
          viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
          viewInfo.image = mat.diffuseImage;
          viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
          viewInfo.format = VK_FORMAT_R8G8B8A8_SRGB;
          viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
          viewInfo.subresourceRange.baseMipLevel = 0;
          viewInfo.subresourceRange.levelCount = 1;
          viewInfo.subresourceRange.baseArrayLayer = 0;
          viewInfo.subresourceRange.layerCount = 1;
          vkCreateImageView(device, &viewInfo, nullptr, &mat.diffuseImageView);
        } else {
          spdlog::warn("Failed to load texture: {}", texFilePath);
        }
      }
    }
  }

  // Create GPU vertex buffer
  {
    VkDeviceSize bufferSize = allVertices.size() * sizeof(SkinnedVertex);
    createBuffer(device, bufferSize, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                     VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                 meshData->vertexBuffer, meshData->vertexBufferMemory);
    void *data;
    vkMapMemory(device, meshData->vertexBufferMemory, 0, bufferSize, 0, &data);
    memcpy(data, allVertices.data(), bufferSize);
    vkUnmapMemory(device, meshData->vertexBufferMemory);
  }

  // Create GPU index buffer
  {
    VkDeviceSize bufferSize = allIndices.size() * sizeof(uint32_t);
    createBuffer(device, bufferSize, VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                     VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                 meshData->indexBuffer, meshData->indexBufferMemory);
    void *data;
    vkMapMemory(device, meshData->indexBufferMemory, 0, bufferSize, 0, &data);
    memcpy(data, allIndices.data(), bufferSize);
    vkUnmapMemory(device, meshData->indexBufferMemory);
  }

  spdlog::info("FBX loaded: {} vertices, {} indices, {} bones, {} animations",
               meshData->totalVertexCount, meshData->totalIndexCount,
               meshData->skeleton.bones.size(), meshData->clips.size());

  return meshData;
}
