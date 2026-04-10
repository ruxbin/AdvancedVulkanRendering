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
