#include "SkinnedMesh.h"
#include <algorithm>
#include <cstring>

void SkinnedMeshData::destroy(VkDevice device) {
  if (vertexBuffer != VK_NULL_HANDLE) {
    vkDestroyBuffer(device, vertexBuffer, nullptr);
    vkFreeMemory(device, vertexBufferMemory, nullptr);
  }
  if (indexBuffer != VK_NULL_HANDLE) {
    vkDestroyBuffer(device, indexBuffer, nullptr);
    vkFreeMemory(device, indexBufferMemory, nullptr);
  }
  for (auto &mat : materials) {
    if (mat.diffuseImageView != VK_NULL_HANDLE)
      vkDestroyImageView(device, mat.diffuseImageView, nullptr);
    if (mat.diffuseImage != VK_NULL_HANDLE)
      vkDestroyImage(device, mat.diffuseImage, nullptr);
    if (mat.diffuseImageMemory != VK_NULL_HANDLE)
      vkFreeMemory(device, mat.diffuseImageMemory, nullptr);
  }
}

static uint32_t findMemoryTypeHelper(VkPhysicalDevice physDevice,
                                      uint32_t typeFilter,
                                      VkMemoryPropertyFlags properties) {
  VkPhysicalDeviceMemoryProperties memProperties;
  vkGetPhysicalDeviceMemoryProperties(physDevice, &memProperties);
  for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++) {
    if ((typeFilter & (1 << i)) &&
        (memProperties.memoryTypes[i].propertyFlags & properties) ==
            properties) {
      return i;
    }
  }
  return 0;
}

void SkinnedMeshInstance::init(SkinnedMeshData *data, VkDevice device,
                               VkPhysicalDevice physDevice,
                               uint32_t (*findMemoryType)(VkPhysicalDevice,
                                                           uint32_t,
                                                           VkMemoryPropertyFlags)) {
  meshData = data;
  worldTransform = mat4(1.0f);

  size_t boneCount = data->skeleton.bones.size();
  finalBoneMatrices.resize(boneCount, mat4(1.0f));
  globalBoneTransforms.resize(boneCount, mat4(1.0f));

  // Create bone matrix SSBO
  VkDeviceSize bufferSize = MAX_BONES * sizeof(mat4);

  VkBufferCreateInfo bufferInfo{};
  bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  bufferInfo.size = bufferSize;
  bufferInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
  bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

  vkCreateBuffer(device, &bufferInfo, nullptr, &boneMatrixBuffer);

  VkMemoryRequirements memReqs;
  vkGetBufferMemoryRequirements(device, boneMatrixBuffer, &memReqs);

  VkMemoryAllocateInfo allocInfo{};
  allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  allocInfo.allocationSize = memReqs.size;
  allocInfo.memoryTypeIndex = findMemoryType(
      physDevice, memReqs.memoryTypeBits,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

  vkAllocateMemory(device, &allocInfo, nullptr, &boneMatrixBufferMemory);
  vkBindBufferMemory(device, boneMatrixBuffer, boneMatrixBufferMemory, 0);

  // Initialize with identity matrices
  void *mapped;
  vkMapMemory(device, boneMatrixBufferMemory, 0, bufferSize, 0, &mapped);
  mat4 identity(1.0f);
  mat4 identityT = transpose(identity);
  for (uint32_t i = 0; i < MAX_BONES; i++) {
    memcpy((char *)mapped + i * sizeof(mat4), identityT.value_ptr(),
           sizeof(mat4));
  }
  vkUnmapMemory(device, boneMatrixBufferMemory);
}

static vec3 lerpVec3(const vec3 &a, const vec3 &b, float t) {
  return a * (1.0f - t) + b * t;
}

template <typename KeyType>
static uint32_t findKeyIndex(const std::vector<KeyType> &keys, float time) {
  for (uint32_t i = 0; i + 1 < (uint32_t)keys.size(); i++) {
    if (time < keys[i + 1].time)
      return i;
  }
  return keys.empty() ? 0 : (uint32_t)(keys.size() - 1);
}

static vec3 interpolatePosition(const std::vector<VectorKey> &keys,
                                 float time) {
  if (keys.size() == 1)
    return keys[0].value;
  uint32_t idx = findKeyIndex(keys, time);
  if (idx + 1 >= keys.size())
    return keys[idx].value;
  float dt = keys[idx + 1].time - keys[idx].time;
  float t = (dt > 0.0f) ? (time - keys[idx].time) / dt : 0.0f;
  t = std::clamp(t, 0.0f, 1.0f);
  return lerpVec3(keys[idx].value, keys[idx + 1].value, t);
}

static quat interpolateRotation(const std::vector<QuatKey> &keys, float time) {
  if (keys.size() == 1)
    return keys[0].value;
  uint32_t idx = findKeyIndex(keys, time);
  if (idx + 1 >= keys.size())
    return keys[idx].value;
  float dt = keys[idx + 1].time - keys[idx].time;
  float t = (dt > 0.0f) ? (time - keys[idx].time) / dt : 0.0f;
  t = std::clamp(t, 0.0f, 1.0f);
  return quat::slerp(keys[idx].value, keys[idx + 1].value, t);
}

static vec3 interpolateScale(const std::vector<VectorKey> &keys, float time) {
  if (keys.size() == 1)
    return keys[0].value;
  uint32_t idx = findKeyIndex(keys, time);
  if (idx + 1 >= keys.size())
    return keys[idx].value;
  float dt = keys[idx + 1].time - keys[idx].time;
  float t = (dt > 0.0f) ? (time - keys[idx].time) / dt : 0.0f;
  t = std::clamp(t, 0.0f, 1.0f);
  return lerpVec3(keys[idx].value, keys[idx + 1].value, t);
}

void SkinnedMeshInstance::updateAnimation(float deltaTime) {
  if (!meshData || meshData->clips.empty() || !playing)
    return;

  const AnimationClip &clip = meshData->clips[currentClipIndex];
  currentTime += deltaTime * clip.ticksPerSecond;

  if (currentTime > clip.duration) {
    if (loop)
      currentTime = fmodf(currentTime, clip.duration);
    else {
      currentTime = clip.duration;
      playing = false;
    }
  }

  computeBoneTransforms();
}

void SkinnedMeshInstance::computeBoneTransforms() {
  if (!meshData || meshData->clips.empty())
    return;

  const AnimationClip &clip = meshData->clips[currentClipIndex];
  const Skeleton &skeleton = meshData->skeleton;

  // Build a map from bone index to its animation channel
  std::unordered_map<int32_t, const AnimationChannel *> channelMap;
  for (const auto &ch : clip.channels) {
    channelMap[ch.boneIndex] = &ch;
  }

  // Walk bones in topological order (parents first)
  for (size_t i = 0; i < skeleton.bones.size(); i++) {
    const Bone &bone = skeleton.bones[i];
    mat4 localTransform = bone.localTransform;

    auto it = channelMap.find(bone.index);
    if (it != channelMap.end()) {
      const AnimationChannel *ch = it->second;

      vec3 pos(0.0f);
      quat rot;
      vec3 scl(1.0f);

      if (!ch->positionKeys.empty())
        pos = interpolatePosition(ch->positionKeys, currentTime);
      if (!ch->rotationKeys.empty())
        rot = interpolateRotation(ch->rotationKeys, currentTime);
      if (!ch->scaleKeys.empty())
        scl = interpolateScale(ch->scaleKeys, currentTime);

      // Compose T * R * S
      mat4 T = translate(pos);
      mat4 R = rot.toMat4();
      mat4 S = scale(scl.x, scl.y, scl.z);
      localTransform = T * R * S;
    }

    if (bone.parentIndex >= 0) {
      globalBoneTransforms[i] =
          globalBoneTransforms[bone.parentIndex] * localTransform;
    } else {
      globalBoneTransforms[i] = localTransform;
    }

    finalBoneMatrices[i] =
        meshData->globalInverseTransform * globalBoneTransforms[i] *
        bone.offsetMatrix;
  }
}

void SkinnedMeshInstance::uploadBoneMatrices(VkDevice device) {
  VkDeviceSize bufferSize = MAX_BONES * sizeof(mat4);
  void *mapped;
  vkMapMemory(device, boneMatrixBufferMemory, 0, bufferSize, 0, &mapped);

  for (size_t i = 0;
       i < finalBoneMatrices.size() && i < MAX_BONES; i++) {
    mat4 m = transpose(finalBoneMatrices[i]);
    memcpy((char *)mapped + i * sizeof(mat4), m.value_ptr(), sizeof(mat4));
  }

  vkUnmapMemory(device, boneMatrixBufferMemory);
}

void SkinnedMeshInstance::destroy(VkDevice device) {
  if (boneMatrixBuffer != VK_NULL_HANDLE) {
    vkDestroyBuffer(device, boneMatrixBuffer, nullptr);
    vkFreeMemory(device, boneMatrixBufferMemory, nullptr);
  }
}
