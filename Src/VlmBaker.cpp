#include "VlmBaker.h"

#include "GpuScene.h"
#include "Light.h"
#include "Raytracing.h"
#include "SphericalHarmonics.h"
#include "VlmSh.h"
#include "VlmEnvironment.h"
#include "stb_image.h"

#include <spdlog/spdlog.h>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>

// windows.h(经 vulkan.h)的 min/max 宏会打爆 std::min/std::max(同 Plane.h 的处理)。
#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

// 与 vlm_bake.hlsl 的 VlmBakePushConsts 逐字段一致(112 B)。
struct VlmBakePC {
  uint32_t probeCount, samplesThisBatch, batchSeed, maxBounces;      // 0..15
  uint32_t pointLightCount, spotLightCount, flags, cellsX;           // 16..31
  uint32_t cellsY, cellsZ; float sunConeRadius, sunScale;            // 32..47
  float envScale, localLightScale; uint32_t probeBase, envWidth;      // 48..63
  float constEnvRGB[3]; uint32_t envHeight;                            // 64..79
  float bmin[3]; float pad3;                                         // 80..95
  float step[3]; float pad4;                                         // 96..111
};
static_assert(sizeof(VlmBakePC) == 112, "VlmBakePC must stay 112 bytes (push constant)");
static_assert(offsetof(VlmBakePC, flags) == 24);
static_assert(offsetof(VlmBakePC, sunConeRadius) == 40);
static_assert(offsetof(VlmBakePC, envWidth) == 60);
static_assert(offsetof(VlmBakePC, envHeight) == 76);
static_assert(offsetof(VlmBakePC, constEnvRGB) == 64);
static_assert(offsetof(VlmBakePC, bmin) == 80);
static_assert(offsetof(VlmBakePC, step) == 96);

namespace {
constexpr uint32_t kFlagSkyOnly = 1u, kFlagConstEnv = 2u, kFlagSunIsEnvironment = 4u;

std::vector<char> readSpv(const std::filesystem::path& p) {
  std::ifstream f(p, std::ios::binary | std::ios::ate);
  if (!f.good()) return {};
  const auto size = f.tellg();
  std::vector<char> bytes((size_t)size);
  f.seekg(0);
  f.read(bytes.data(), size);
  return bytes;
}

constexpr VkDeviceSize alignUp(VkDeviceSize x, VkDeviceSize a) {
  return (x + a - 1) & ~(a - 1);
}
} // namespace

VlmBaker::VlmBaker(VulkanDevice& device, GpuScene& scene) : _device(device), _scene(scene) {}

VlmBaker::~VlmBaker() {
  VkDevice dev = _device.getLogicalDevice();
  auto destroyBuf = [dev](VkBuffer& b, VkDeviceMemory& m) {
    if (b != VK_NULL_HANDLE) vkDestroyBuffer(dev, b, nullptr);
    if (m != VK_NULL_HANDLE) vkFreeMemory(dev, m, nullptr);
    b = VK_NULL_HANDLE;
    m = VK_NULL_HANDLE;
  };
  destroyBuf(_shAccum, _shAccumMemory);
  destroyBuf(_errorFlags, _errorFlagsMemory);
  destroyBuf(_envCdf, _envCdfMemory);
  destroyBuf(_sbtBuffer, _sbtMemory);
  if (_envFallbackView != VK_NULL_HANDLE) vkDestroyImageView(dev, _envFallbackView, nullptr);
  if (_envFallback != VK_NULL_HANDLE) vkDestroyImage(dev, _envFallback, nullptr);
  if (_envFallbackMemory != VK_NULL_HANDLE) vkFreeMemory(dev, _envFallbackMemory, nullptr);
  if (_envSampler != VK_NULL_HANDLE) vkDestroySampler(dev, _envSampler, nullptr);
  if (_pipeline != VK_NULL_HANDLE) vkDestroyPipeline(dev, _pipeline, nullptr);
  if (_pipelineLayout != VK_NULL_HANDLE) vkDestroyPipelineLayout(dev, _pipelineLayout, nullptr);
  if (_shaderModule != VK_NULL_HANDLE) vkDestroyShaderModule(dev, _shaderModule, nullptr);
  if (_pool != VK_NULL_HANDLE) vkDestroyDescriptorPool(dev, _pool, nullptr);
  if (_setLayout != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(dev, _setLayout, nullptr);
}

// host-visible coherent STORAGE buffer;容量不足时销毁重建(模式同
// IBLGenerator.cpp:101 的 createHostBuffer,usage 固定 STORAGE_BUFFER)。
void VlmBaker::ensureHostBuffer(VkDeviceSize bytes, VkBuffer& buffer, VkDeviceMemory& memory) {
  if (buffer != VK_NULL_HANDLE) {
    VkDevice dev = _device.getLogicalDevice();
    vkDestroyBuffer(dev, buffer, nullptr);
    vkFreeMemory(dev, memory, nullptr);
    buffer = VK_NULL_HANDLE;
    memory = VK_NULL_HANDLE;
  }
  VkDevice dev = _device.getLogicalDevice();
  VkBufferCreateInfo bi{};
  bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  bi.size = bytes;
  bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
  bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  if (vkCreateBuffer(dev, &bi, nullptr, &buffer) != VK_SUCCESS)
    throw std::runtime_error("VlmBaker: failed to create buffer");
  VkMemoryRequirements mr;
  vkGetBufferMemoryRequirements(dev, buffer, &mr);
  VkMemoryAllocateInfo ai{};
  ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  ai.allocationSize = mr.size;
  ai.memoryTypeIndex = _device.findMemoryType(
      mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  if (vkAllocateMemory(dev, &ai, nullptr, &memory) != VK_SUCCESS)
    throw std::runtime_error("VlmBaker: failed to allocate buffer memory");
  if(vkBindBufferMemory(dev, buffer, memory, 0)!=VK_SUCCESS)
    throw std::runtime_error("VlmBaker: failed to bind buffer memory");
}

// binding 16/17 重绑(Init 末尾先以 16 B 占位 buffer 调用一次,Bake 重建后再次调用)。
void VlmBaker::writeAccumDescriptors() {
  VkDevice dev = _device.getLogicalDevice();
  VkDescriptorBufferInfo shInfo{_shAccum, 0, VK_WHOLE_SIZE};
  VkDescriptorBufferInfo errInfo{_errorFlags, 0, VK_WHOLE_SIZE};
  VkWriteDescriptorSet w[2]{};
  for (int i = 0; i < 2; ++i) {
    w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w[i].dstSet = _set;
    w[i].descriptorCount = 1;
    w[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  }
  w[0].dstBinding = 16;
  w[0].pBufferInfo = &shInfo;
  w[1].dstBinding = 17;
  w[1].pBufferInfo = &errInfo;
  vkUpdateDescriptorSets(dev, 2, w, 0, nullptr);
}

void VlmBaker::Init(VkImageView equirectView) {
  VkDevice dev = _device.getLogicalDevice();

  // 1) 函数指针(与 RayTracing::loadFunctionPointers 同模式,只取烘焙需要的 4 个)
#define LOAD_DEV(name) \
  pfn##name = reinterpret_cast<PFN_vk##name##KHR>(vkGetDeviceProcAddr(dev, "vk" #name "KHR")); \
  if (!pfn##name) throw std::runtime_error("VlmBaker: failed to load vk" #name "KHR");
  LOAD_DEV(GetBufferDeviceAddress)
  LOAD_DEV(CreateRayTracingPipelines)
  LOAD_DEV(GetRayTracingShaderGroupHandles)
  LOAD_DEV(CmdTraceRays)
#undef LOAD_DEV

  const VkShaderStageFlags rtAllStages = VK_SHADER_STAGE_RAYGEN_BIT_KHR |
                                         VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR |
                                         VK_SHADER_STAGE_ANY_HIT_BIT_KHR |
                                         VK_SHADER_STAGE_MISS_BIT_KHR;

  // 2) env sampler(linear,U repeat / V,W clamp;与 RT 的 _LinearRepeatSampler 区分,
  //    equirect 经度方向必须 wrap,纬度方向 clamp)
  {
    VkSamplerCreateInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    si.magFilter = VK_FILTER_LINEAR;
    si.minFilter = VK_FILTER_LINEAR;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    si.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    si.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.maxLod = 1.0f;
    if (vkCreateSampler(dev, &si, nullptr, &_envSampler) != VK_SUCCESS)
      throw std::runtime_error("VlmBaker: failed to create env sampler");
  }

  // 3) equirect 缺省时的 1×1 占位黑图(仅 constEnv 模式合法;shader 不采样,
  //    但 binding 14 必须指向有效 view 才能通过 descriptor 校验)
  VkImageView envView = equirectView;
  if (envView == VK_NULL_HANDLE) {
    VkImageCreateInfo ii{};
    ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.extent = {1, 1, 1};
    ii.mipLevels = 1;
    ii.arrayLayers = 1;
    ii.format = VK_FORMAT_R32G32B32A32_SFLOAT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    ii.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    if (vkCreateImage(dev, &ii, nullptr, &_envFallback) != VK_SUCCESS)
      throw std::runtime_error("VlmBaker: failed to create env fallback image");
    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(dev, _envFallback, &mr);
    VkMemoryAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = mr.size;
    ai.memoryTypeIndex = _device.findMemoryType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (vkAllocateMemory(dev, &ai, nullptr, &_envFallbackMemory) != VK_SUCCESS)
      throw std::runtime_error("VlmBaker: failed to allocate env fallback memory");
    vkBindImageMemory(dev, _envFallback, _envFallbackMemory, 0);
    _device.transitionImageLayout(_envFallback, ii.format,
                                  VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    VkImageViewCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = _envFallback;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = ii.format;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (vkCreateImageView(dev, &vi, nullptr, &_envFallbackView) != VK_SUCCESS)
      throw std::runtime_error("VlmBaker: failed to create env fallback view");
    envView = _envFallbackView;
  }

  // 4) set1 layout,17 个 binding(编号与 rt_path_common.hlsl/vlm_bake.hlsl 一致;
  //    1/12 为相机 PT 的 storage image,VLM 不用,留空)
  const uint32_t bindlessCount = (uint32_t)_scene.textures.size();
  constexpr uint32_t bindingCount = 17;
  VkDescriptorSetLayoutBinding b[bindingCount]{};
  auto fill = [](VkDescriptorSetLayoutBinding& x, uint32_t binding,
                 VkDescriptorType type, uint32_t count, VkShaderStageFlags stages) {
    x.binding = binding;
    x.descriptorType = type;
    x.descriptorCount = count;
    x.stageFlags = stages;
    x.pImmutableSamplers = nullptr;
  };
  fill(b[0],  0,  VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1, rtAllStages);
  fill(b[1],  2,  VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,             1, rtAllStages);
  fill(b[2],  3,  VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,             1, rtAllStages);
  fill(b[3],  4,  VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,             1, rtAllStages);
  fill(b[4],  5,  VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,             1, rtAllStages);
  fill(b[5],  6,  VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,             1, rtAllStages);
  fill(b[6],  7,  VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,             1, rtAllStages);
  fill(b[7],  8,  VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,             1, rtAllStages);
  fill(b[8],  9,  VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,             1, rtAllStages);
  fill(b[9],  10, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,              bindlessCount, rtAllStages);
  fill(b[10], 11, VK_DESCRIPTOR_TYPE_SAMPLER,                    1, rtAllStages);
  fill(b[11], 13, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,             1, rtAllStages);
  fill(b[12], 14, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,              1, rtAllStages);
  fill(b[13], 15, VK_DESCRIPTOR_TYPE_SAMPLER,                    1, rtAllStages);
  fill(b[14], 16, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,             1, rtAllStages);
  fill(b[15], 17, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,             1, rtAllStages);
  fill(b[16], 18, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,             1, rtAllStages);

  VkDescriptorBindingFlags bf[bindingCount]{};
  bf[9] = VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT_EXT; // binding 10 纹理数组
  VkDescriptorSetLayoutBindingFlagsCreateInfo bfInfo{
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
  bfInfo.bindingCount = bindingCount;
  bfInfo.pBindingFlags = bf;

  VkDescriptorSetLayoutCreateInfo slInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  slInfo.pNext = &bfInfo;
  slInfo.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
  slInfo.bindingCount = bindingCount;
  slInfo.pBindings = b;
  if (vkCreateDescriptorSetLayout(dev, &slInfo, nullptr, &_setLayout) != VK_SUCCESS)
    throw std::runtime_error("VlmBaker: failed to create descriptor set layout");

  // 5) pool(maxSets=1;STORAGE_BUFFER×12 = binding 2..9/13/16/17/18)
  std::vector<VkDescriptorPoolSize> poolSizes = {
      {VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1},
      {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 12},
      {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, bindlessCount + 1},
      {VK_DESCRIPTOR_TYPE_SAMPLER, 2}};
  VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  pi.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT_EXT;
  pi.maxSets = 1;
  pi.poolSizeCount = (uint32_t)poolSizes.size();
  pi.pPoolSizes = poolSizes.data();
  if (vkCreateDescriptorPool(dev, &pi, nullptr, &_pool) != VK_SUCCESS)
    throw std::runtime_error("VlmBaker: failed to create descriptor pool");

  VkDescriptorSetAllocateInfo aInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  aInfo.descriptorPool = _pool;
  aInfo.descriptorSetCount = 1;
  aInfo.pSetLayouts = &_setLayout;
  if (vkAllocateDescriptorSets(dev, &aInfo, &_set) != VK_SUCCESS)
    throw std::runtime_error("VlmBaker: vkAllocateDescriptorSets failed");

  // 6) 写 descriptor(16/17 之外的全部 binding;16/17 由 writeAccumDescriptors 最后写)
  {
    VkAccelerationStructureKHR tlas = _scene._raytracing->GetTlas();
    VkWriteDescriptorSetAccelerationStructureKHR asWrite{
        VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR};
    asWrite.accelerationStructureCount = 1;
    asWrite.pAccelerationStructures = &tlas;

    VkDescriptorBufferInfo bvb {_scene.applVertexBuffer,   0, VK_WHOLE_SIZE};
    VkDescriptorBufferInfo bvn {_scene.applNormalBuffer,   0, VK_WHOLE_SIZE};
    VkDescriptorBufferInfo bvt {_scene.applTangentBuffer,  0, VK_WHOLE_SIZE};
    VkDescriptorBufferInfo bvu {_scene.applUVBuffer,       0, VK_WHOLE_SIZE};
    VkDescriptorBufferInfo bib {_scene.applIndexBuffer,    0, VK_WHOLE_SIZE};
    VkDescriptorBufferInfo bch {_scene.meshChunksBuffer,   0, VK_WHOLE_SIZE};
    VkDescriptorBufferInfo bma {_scene.applMaterialBuffer, 0, VK_WHOLE_SIZE};
    VkDescriptorBufferInfo bpl {
        _scene._lightCuller ? _scene._lightCuller->GetPointLightCullingDataBuffer() : VK_NULL_HANDLE,
        0, VK_WHOLE_SIZE};
    VkDescriptorBufferInfo bsp {
        _scene._lightCuller ? _scene._lightCuller->GetSpotLightCullingDataBuffer() : VK_NULL_HANDLE,
        0, VK_WHOLE_SIZE};

    std::vector<VkDescriptorImageInfo> texImgs(bindlessCount);
    for (uint32_t i = 0; i < bindlessCount; ++i) {
      texImgs[i].imageView = _scene.textures[i].second;
      texImgs[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }
    VkDescriptorImageInfo samplerInfo{};
    samplerInfo.sampler = _scene.textureSampler;
    VkDescriptorImageInfo envImg{};
    envImg.imageView = envView;
    envImg.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkDescriptorImageInfo envSamplerInfo{};
    envSamplerInfo.sampler = _envSampler;

    VkWriteDescriptorSet w[14]{};
    auto bufW = [&](uint32_t i, uint32_t binding, VkDescriptorBufferInfo* bi) {
      w[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
      w[i].dstSet = _set;
      w[i].dstBinding = binding;
      w[i].descriptorCount = 1;
      w[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
      w[i].pBufferInfo = bi;
    };
    w[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, &asWrite};
    w[0].dstSet = _set;
    w[0].dstBinding = 0;
    w[0].descriptorCount = 1;
    w[0].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;

    bufW(1, 2, &bvb);
    bufW(2, 3, &bvn);
    bufW(3, 4, &bvt);
    bufW(4, 5, &bvu);
    bufW(5, 6, &bib);
    bufW(6, 7, &bch);
    bufW(7, 8, &bma);
    bufW(8, 9, &bpl);

    w[9] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w[9].dstSet = _set;
    w[9].dstBinding = 10;
    w[9].descriptorCount = bindlessCount;
    w[9].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    w[9].pImageInfo = texImgs.data();

    w[10] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w[10].dstSet = _set;
    w[10].dstBinding = 11;
    w[10].descriptorCount = 1;
    w[10].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    w[10].pImageInfo = &samplerInfo;

    bufW(11, 13, &bsp);

    w[12] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w[12].dstSet = _set;
    w[12].dstBinding = 14;
    w[12].descriptorCount = 1;
    w[12].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    w[12].pImageInfo = &envImg;

    w[13] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w[13].dstSet = _set;
    w[13].dstBinding = 15;
    w[13].descriptorCount = 1;
    w[13].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    w[13].pImageInfo = &envSamplerInfo;

    vkUpdateDescriptorSets(dev, 14, w, 0, nullptr);
  }

  // 7) 16 B 占位 shAccum/errorFlags,保证 set 内 descriptor 全部有效;
  //    Bake 按 layout 重建后再次重绑。
  ensureHostBuffer(16, _shAccum, _shAccumMemory);
  ensureHostBuffer(16, _errorFlags, _errorFlagsMemory);
  writeAccumDescriptors();
  ensureHostBuffer(16,_envCdf,_envCdfMemory);
  VkDescriptorBufferInfo cdfInfo{_envCdf,0,VK_WHOLE_SIZE};
  VkWriteDescriptorSet cdfWrite{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
  cdfWrite.dstSet=_set; cdfWrite.dstBinding=18; cdfWrite.descriptorCount=1;
  cdfWrite.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; cdfWrite.pBufferInfo=&cdfInfo;
  vkUpdateDescriptorSets(dev,1,&cdfWrite,0,nullptr);

  // 8) pipeline layout:set0 = globalSetLayout(camera UBO),set1 = 本 layout;
  //    push constant 112 B(VlmBakePC)
  VkPushConstantRange pcRange{};
  pcRange.stageFlags = rtAllStages;
  pcRange.offset = 0;
  pcRange.size = sizeof(VlmBakePC);

  VkDescriptorSetLayout setLayouts[] = {_scene.globalSetLayout, _setLayout};
  VkPipelineLayoutCreateInfo plInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  plInfo.setLayoutCount = 2;
  plInfo.pSetLayouts = setLayouts;
  plInfo.pushConstantRangeCount = 1;
  plInfo.pPushConstantRanges = &pcRange;
  if (vkCreatePipelineLayout(dev, &plInfo, nullptr, &_pipelineLayout) != VK_SUCCESS)
    throw std::runtime_error("VlmBaker: failed to create pipeline layout");

  // 9) pipeline:6 stage / 7 group,与 Raytracing.cpp:365-435 同构
  std::vector<char> spv = readSpv(_scene.RootPath() / "shaders" / "vlm_bake.lib.spv");
  if (spv.empty())
    throw std::runtime_error("VlmBaker: failed to read shaders/vlm_bake.lib.spv");
  VkShaderModuleCreateInfo smInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  smInfo.codeSize = spv.size();
  smInfo.pCode = reinterpret_cast<const uint32_t*>(spv.data());
  if (vkCreateShaderModule(dev, &smInfo, nullptr, &_shaderModule) != VK_SUCCESS)
    throw std::runtime_error("VlmBaker: failed to create shader module from vlm_bake.lib.spv");

  enum StageIdx : uint32_t {
    STG_RAYGEN = 0,
    STG_MISS_PRIMARY,
    STG_MISS_SHADOW,
    STG_CHIT_PRIMARY,
    STG_AHIT_PRIMARY,
    STG_AHIT_SHADOW,
    STG_COUNT
  };
  VkPipelineShaderStageCreateInfo stages[STG_COUNT]{};
  auto fillStage = [&](VkShaderStageFlagBits stage, const char* entry,
                       VkPipelineShaderStageCreateInfo& out) {
    out = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    out.stage = stage;
    out.module = _shaderModule;
    out.pName = entry;
  };
  fillStage(VK_SHADER_STAGE_RAYGEN_BIT_KHR,      "VlmProbeRayGen",    stages[STG_RAYGEN]);
  fillStage(VK_SHADER_STAGE_MISS_BIT_KHR,        "VlmMissPrimary",    stages[STG_MISS_PRIMARY]);
  fillStage(VK_SHADER_STAGE_MISS_BIT_KHR,        "MissShadow",        stages[STG_MISS_SHADOW]);
  fillStage(VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR, "ClosestHitPrimary", stages[STG_CHIT_PRIMARY]);
  fillStage(VK_SHADER_STAGE_ANY_HIT_BIT_KHR,     "AnyHitAlpha",       stages[STG_AHIT_PRIMARY]);
  fillStage(VK_SHADER_STAGE_ANY_HIT_BIT_KHR,     "AnyHitAlphaShadow", stages[STG_AHIT_SHADOW]);

  // SBT 顺序与相机 PT 相同:
  //   [0] raygen  [1] miss primary  [2] miss shadow
  //   [3] HG_PRI_OPAQUE(chit)  [4] HG_PRI_ALPHA(chit+ahit)
  //   [5] HG_SHA_OPAQUE(空)    [6] HG_SHA_ALPHA(ahit)
  enum GroupIdx : uint32_t {
    GRP_RAYGEN = 0,
    GRP_MISS_PRIMARY,
    GRP_MISS_SHADOW,
    GRP_HG_PRI_OPAQUE,
    GRP_HG_PRI_ALPHA,
    GRP_HG_SHA_OPAQUE,
    GRP_HG_SHA_ALPHA,
    GRP_COUNT
  };
  VkRayTracingShaderGroupCreateInfoKHR groups[GRP_COUNT]{};
  for (auto& g : groups) {
    g = {VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR};
    g.generalShader = VK_SHADER_UNUSED_KHR;
    g.closestHitShader = VK_SHADER_UNUSED_KHR;
    g.anyHitShader = VK_SHADER_UNUSED_KHR;
    g.intersectionShader = VK_SHADER_UNUSED_KHR;
  }
  groups[GRP_RAYGEN].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
  groups[GRP_RAYGEN].generalShader = STG_RAYGEN;

  groups[GRP_MISS_PRIMARY].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
  groups[GRP_MISS_PRIMARY].generalShader = STG_MISS_PRIMARY;

  groups[GRP_MISS_SHADOW].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
  groups[GRP_MISS_SHADOW].generalShader = STG_MISS_SHADOW;

  groups[GRP_HG_PRI_OPAQUE].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR;
  groups[GRP_HG_PRI_OPAQUE].closestHitShader = STG_CHIT_PRIMARY;

  groups[GRP_HG_PRI_ALPHA].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR;
  groups[GRP_HG_PRI_ALPHA].closestHitShader = STG_CHIT_PRIMARY;
  groups[GRP_HG_PRI_ALPHA].anyHitShader = STG_AHIT_PRIMARY;

  groups[GRP_HG_SHA_OPAQUE].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR;
  // empty -- both shaders unused

  groups[GRP_HG_SHA_ALPHA].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR;
  groups[GRP_HG_SHA_ALPHA].anyHitShader = STG_AHIT_SHADOW;

  VkRayTracingPipelineCreateInfoKHR rtPipelineInfo{
      VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR};
  rtPipelineInfo.stageCount = STG_COUNT;
  rtPipelineInfo.pStages = stages;
  rtPipelineInfo.groupCount = GRP_COUNT;
  rtPipelineInfo.pGroups = groups;
  rtPipelineInfo.maxPipelineRayRecursionDepth = 2; // primary + shadow
  rtPipelineInfo.layout = _pipelineLayout;
  if (pfnCreateRayTracingPipelines(dev, VK_NULL_HANDLE, VK_NULL_HANDLE, 1,
                                   &rtPipelineInfo, nullptr, &_pipeline) != VK_SUCCESS)
    throw std::runtime_error("VlmBaker: failed to create ray tracing pipeline");

  // 10) SBT,与 Raytracing.cpp:471-543 同构。
  //     RT properties 用 vkGetPhysicalDeviceProperties2 自取,不依赖 device 内部成员。
  VkPhysicalDeviceRayTracingPipelinePropertiesKHR rtProps{
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR};
  VkPhysicalDeviceProperties2 props2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &rtProps};
  vkGetPhysicalDeviceProperties2(_device.getPhysicalDevice(), &props2);

  const uint32_t handleSize = rtProps.shaderGroupHandleSize;
  const uint32_t handleAlignment = rtProps.shaderGroupHandleAlignment;
  const uint32_t baseAlignment = rtProps.shaderGroupBaseAlignment;
  const uint32_t handleSizeAligned = (uint32_t)alignUp(handleSize, handleAlignment);

  const uint32_t opaqueCount = (uint32_t)_scene.applMesh->_opaqueChunkCount;
  const uint32_t alphaCount = (uint32_t)_scene.applMesh->_alphaMaskedChunkCount;
  const uint32_t geomCount = opaqueCount + alphaCount;

  _rgenRegion.stride = (uint32_t)alignUp(handleSizeAligned, baseAlignment);
  _rgenRegion.size = _rgenRegion.stride; // raygen size MUST equal stride

  _missRegion.stride = handleSizeAligned;
  _missRegion.size = (uint32_t)alignUp(handleSizeAligned * 2, baseAlignment);

  _hitRegion.stride = handleSizeAligned;
  _hitRegion.size = (uint32_t)alignUp(handleSizeAligned * geomCount * 2, baseAlignment);

  _callRegion = {};

  std::vector<uint8_t> handles(GRP_COUNT * handleSize);
  if (pfnGetRayTracingShaderGroupHandles(dev, _pipeline, 0, GRP_COUNT,
                                         (uint32_t)handles.size(), handles.data()) != VK_SUCCESS)
    throw std::runtime_error("VlmBaker: vkGetRayTracingShaderGroupHandlesKHR failed");
  auto handlePtr = [&](uint32_t idx) { return handles.data() + idx * handleSize; };

  const VkDeviceSize sbtSize = _rgenRegion.size + _missRegion.size + _hitRegion.size;
  {
    // 带 device address 的 host-visible buffer(模式同 RayTracing::allocateBuffer)
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = sbtSize;
    bi.usage = VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR |
               VK_BUFFER_USAGE_TRANSFER_DST_BIT |
               VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(dev, &bi, nullptr, &_sbtBuffer) != VK_SUCCESS)
      throw std::runtime_error("VlmBaker: vkCreateBuffer(SBT) failed");
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(dev, _sbtBuffer, &req);
    VkMemoryAllocateFlagsInfo flagsInfo{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
    flagsInfo.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT_KHR;
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &flagsInfo};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = _device.findMemoryType(
        req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (vkAllocateMemory(dev, &ai, nullptr, &_sbtMemory) != VK_SUCCESS)
      throw std::runtime_error("VlmBaker: vkAllocateMemory(SBT) failed");
    if(vkBindBufferMemory(dev, _sbtBuffer, _sbtMemory, 0)!=VK_SUCCESS)
      throw std::runtime_error("VlmBaker: failed to bind SBT memory");
  }

  uint8_t* sbtMapped = nullptr;
  if(vkMapMemory(dev, _sbtMemory, 0, sbtSize, 0, (void**)&sbtMapped)!=VK_SUCCESS)
    throw std::runtime_error("VlmBaker: SBT map failed");
  std::memset(sbtMapped, 0, sbtSize);

  uint8_t* p = sbtMapped;
  std::memcpy(p, handlePtr(GRP_RAYGEN), handleSize);
  p = sbtMapped + _rgenRegion.size;

  std::memcpy(p + 0 * handleSizeAligned, handlePtr(GRP_MISS_PRIMARY), handleSize);
  std::memcpy(p + 1 * handleSizeAligned, handlePtr(GRP_MISS_SHADOW), handleSize);
  p = sbtMapped + _rgenRegion.size + _missRegion.size;

  for (uint32_t g = 0; g < geomCount; ++g) {
    const bool isAlpha = (g >= opaqueCount);
    uint32_t priGroup = isAlpha ? GRP_HG_PRI_ALPHA : GRP_HG_PRI_OPAQUE;
    uint32_t shaGroup = isAlpha ? GRP_HG_SHA_ALPHA : GRP_HG_SHA_OPAQUE;
    std::memcpy(p + (g * 2 + 0) * handleSizeAligned, handlePtr(priGroup), handleSize);
    std::memcpy(p + (g * 2 + 1) * handleSizeAligned, handlePtr(shaGroup), handleSize);
  }
  vkUnmapMemory(dev, _sbtMemory);

  VkBufferDeviceAddressInfo addrInfo{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, nullptr, _sbtBuffer};
  const VkDeviceAddress sbtAddr = pfnGetBufferDeviceAddress(dev, &addrInfo);
  _rgenRegion.deviceAddress = sbtAddr;
  _missRegion.deviceAddress = sbtAddr + _rgenRegion.size;
  _hitRegion.deviceAddress = sbtAddr + _rgenRegion.size + _missRegion.size;

  spdlog::info(
      "vlm bake: pipeline created. SBT size={} bytes, hit records={} (handleSize={}, aligned={})",
      (uint64_t)sbtSize, geomCount * 2, handleSize, handleSizeAligned);
}

bool VlmBaker::Bake(const Settings& s, VlmAssetData& out) {
  if (!s.layout.ProbeCount() || s.layout.ProbeCount() > 1000000 || !s.samplesPerProbe ||
      s.samplesPerProbe > 16777216 || !s.batchSamples || s.batchSamples > 4096 ||
      !s.maxBounces || s.maxBounces > 32 || out.shFp16.size() != s.layout.ProbeCount() * 28) {
    spdlog::error("vlm bake: FAILED (invalid settings or probe memory budget)");
    return false;
  }
  const uint32_t probes = (uint32_t)s.layout.ProbeCount();
  if (probes == 0 || _pipeline == VK_NULL_HANDLE) {
    spdlog::error("vlm bake: FAILED (not initialized)");
    return false;
  }

  const VkDevice dev = _device.getLogicalDevice();
  VlmEnvironmentDistribution distribution;
  if(!s.constEnv) {
    int width=0,height=0;
    float* pixels=stbi_loadf(s.hdrPathForReference.string().c_str(),&width,&height,nullptr,4);
    const bool valid=VlmBuildEnvironmentDistribution(pixels,width,height,distribution);
    if(pixels) stbi_image_free(pixels);
    if(!valid) { spdlog::error("vlm bake: FAILED (invalid environment distribution)"); return false; }
    const VkDeviceSize bytes=distribution.cdf.size()*sizeof(float);
    ensureHostBuffer(bytes,_envCdf,_envCdfMemory);
    void* mapped=nullptr;
    if(vkMapMemory(dev,_envCdfMemory,0,bytes,0,&mapped)!=VK_SUCCESS)
      throw std::runtime_error("VlmBaker: environment CDF map failed");
    std::memcpy(mapped,distribution.cdf.data(),bytes); vkUnmapMemory(dev,_envCdfMemory);
    VkDescriptorBufferInfo info{_envCdf,0,bytes};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet=_set; write.dstBinding=18; write.descriptorCount=1;
    write.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; write.pBufferInfo=&info;
    vkUpdateDescriptorSets(dev,1,&write,0,nullptr);
    spdlog::info("vlm bake: primary sampling=50% uniform + 50% environment ({}x{})",width,height);
  }
  const VkDeviceSize shBytes = (VkDeviceSize)probes * 27 * sizeof(float);
  // shAccum/errorFlags 尺寸依赖 layout,在 Bake 开头按需(重)建并清零。
  // 注意:buffer 重建后原 descriptor 指向已销毁对象,必须立即重绑 16/17。
  ensureHostBuffer(shBytes, _shAccum, _shAccumMemory);
  ensureHostBuffer(16, _errorFlags, _errorFlagsMemory);
  writeAccumDescriptors();
  {
    void* p = nullptr;
    if(vkMapMemory(dev, _shAccumMemory, 0, shBytes, 0, &p)!=VK_SUCCESS)
      throw std::runtime_error("VlmBaker: accumulation map failed");
    std::memset(p, 0, (size_t)shBytes);
    vkUnmapMemory(dev, _shAccumMemory);
    if(vkMapMemory(dev, _errorFlagsMemory, 0, 16, 0, &p)!=VK_SUCCESS)
      throw std::runtime_error("VlmBaker: error flags map failed");
    std::memset(p, 0, 16);
    vkUnmapMemory(dev, _errorFlagsMemory);
  }

  const uint32_t total = s.samplesPerProbe;
  const uint32_t batches = (total + s.batchSamples - 1) / s.batchSamples;
  spdlog::info("vlm bake: probes={} samples={} batches={} batchSamples={}",
               probes, total, batches, s.batchSamples);
  const auto t0 = std::chrono::steady_clock::now();

  for (uint32_t batch = 0, done = 0; done < total; ++batch) {
    const uint32_t n = std::min(s.batchSamples, total - done);
    VlmBakePC pc{};
    pc.probeCount = probes;
    pc.samplesThisBatch = n;
    pc.batchSeed = batch;
    pc.maxBounces = s.maxBounces;
    pc.pointLightCount = (uint32_t)_scene._pointLights.size();
    pc.spotLightCount = (uint32_t)_scene._spotLights.size();
    pc.flags = (s.skyOnly ? kFlagSkyOnly : 0u) |
               (s.constEnv ? kFlagConstEnv : 0u) |
               (s.sunIsEnvironment ? kFlagSunIsEnvironment : 0u);
    pc.cellsX = s.layout.cells[0]; pc.cellsY = s.layout.cells[1]; pc.cellsZ = s.layout.cells[2];
    pc.sunConeRadius = 0.0087f; // 与 Raytracing.cpp RTPC 一致(tan(0.5°))
    pc.sunScale = s.sunScale;
    pc.envScale = s.envScale;
    pc.localLightScale = s.localLightScale;
    pc.constEnvRGB[0] = s.constEnvRGB[0];
    pc.constEnvRGB[1] = s.constEnvRGB[1];
    pc.constEnvRGB[2] = s.constEnvRGB[2];
    for (int i = 0; i < 3; ++i) { pc.bmin[i] = s.layout.bmin[i]; pc.step[i] = s.layout.step[i]; }

    // Submit bounded probe/sample tiles instead of one enormous command buffer.
    for (uint32_t base = 0; base < probes; base += 256) {
    VkCommandBuffer cmdBuf = _device.beginSingleTimeCommands();
    pc.probeBase = base;
    pc.envWidth=distribution.width; pc.envHeight=distribution.height;
    vkCmdBindPipeline(cmdBuf, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, _pipeline);
    VkDescriptorSet sets[2] = {_scene.globalDescriptorSets[_scene.currentFrame], _set};
    vkCmdBindDescriptorSets(cmdBuf, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR,
                            _pipelineLayout, 0, 2, sets, 0, nullptr);
    vkCmdPushConstants(cmdBuf, _pipelineLayout,
                       VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR |
                           VK_SHADER_STAGE_ANY_HIT_BIT_KHR | VK_SHADER_STAGE_MISS_BIT_KHR,
                       0, sizeof(pc), &pc);
    pfnCmdTraceRays(cmdBuf, &_rgenRegion, &_missRegion, &_hitRegion, &_callRegion,
                    std::min(256u, probes - base), 1, 1);
    {
      // 跨 dispatch 的 RMW 依赖:写 → 读|写
      VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
      mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
      mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_HOST_READ_BIT;
      vkCmdPipelineBarrier(cmdBuf, VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
                           VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR | VK_PIPELINE_STAGE_HOST_BIT,
                           0, 1, &mb, 0, nullptr, 0, nullptr);
    }
    _device.endSingleTimeCommands(cmdBuf);
    }
    done += n;
  }

  const auto t1 = std::chrono::steady_clock::now();
  spdlog::info("vlm bake: gpu dispatch wall time {:.2f}s",
               std::chrono::duration<double>(t1 - t0).count());

  // --- 错误检查(Review Focus #3)---
  uint32_t* flags = nullptr;
  if(vkMapMemory(dev, _errorFlagsMemory, 0, 4, 0, (void**)&flags)!=VK_SUCCESS)
    throw std::runtime_error("VlmBaker: error readback map failed");
  const bool nanFail = flags && *flags != 0;
  vkUnmapMemory(dev, _errorFlagsMemory);
  if (nanFail) {
    spdlog::error("vlm bake: FAILED (NaN/Inf in path samples, batch rejected)");
    return false;
  }

  // --- readback + finalize + pack ---
  float* sums = nullptr;
  if(vkMapMemory(dev, _shAccumMemory, 0, shBytes, 0, (void**)&sums)!=VK_SUCCESS)
    throw std::runtime_error("VlmBaker: accumulation readback map failed");

  std::vector<SH9> finalized(probes);
  for (uint32_t i = 0; i < probes; ++i) {
    VlmShAccumulator acc{};
    acc.sampleCount = total;
    for (int j = 0; j < 9; ++j)
      for (int c = 0; c < 3; ++c)
        acc.sum[j][c] = (double)sums[(size_t)i * 27 + j * 3 + c];
    finalized[i] = VlmShFinalize(acc);
    if (!VlmShPackFp16(finalized[i], &out.shFp16[(size_t)i * 28])) {
      vkUnmapMemory(dev, _shAccumMemory);
      spdlog::error("vlm bake: FAILED (FP16 overflow at probe {}, |c|>65504)", i);
      return false; // Review Focus #2:拒绝发布,不静默饱和
    }
  }

  // --- 验收日志(格式与 Task 7 脚本钉死)---
  const double kPi = 3.1415926535897932;
  {
    // 量化验收(规格 §10 量化行):FP32 重建 E vs FP16 打包解码重建 E,
    // 固定参考亮度 1.0 归一化(零附近不除小量)。
    static const float kNormals[6][3] = {
        {1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}};
    double qMax = 0.0;
    for (uint32_t i = 0; i < probes; ++i) {
      SH9 unpacked{};
      for (int j = 0; j < 9; ++j)
        for (int c = 0; c < 3; ++c)
          unpacked.c[j][c] = VlmHalfToFloat(out.shFp16[(size_t)i * 28 + j * 3 + c]);
      for (auto& n : kNormals) {
        float ef[3], eq[3];
        VlmShEvaluate(finalized[i], n, ef);
        VlmShEvaluate(unpacked, n, eq);
        for (int c = 0; c < 3; ++c)
          qMax = std::max(qMax, (double)std::fabs(ef[c] - eq[c]) /
                                    (double)std::fmax(1.0f, std::fabs(ef[c])));
      }
    }
    spdlog::info("vlm validate quantization: maxRelErr={:.6f} (threshold 0.010000)", qMax);
  }
  if (s.constEnv && s.skyOnly) {
    static const float kNormals[6][3] = {
        {1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}};
    double maxRel = 0.0, sumRel = 0.0;
    uint64_t cnt = 0;
    for (uint32_t i = 0; i < probes; ++i)
      for (auto& n : kNormals) {
        float E[3];
        VlmShEvaluate(finalized[i], n, E);
        for (int c = 0; c < 3; ++c) {
          const double expect = kPi * (double)s.constEnvRGB[c] * s.envScale;
          const double rel = expect > 1e-9 ? std::fabs(E[c] - expect) / expect : std::fabs(E[c]);
          maxRel = std::max(maxRel, rel);
          sumRel += rel;
          ++cnt;
        }
      }
    spdlog::info("vlm validate const-sky: maxRelErr={:.6f} meanRelErr={:.6f} (threshold 0.010000)",
                 maxRel, sumRel / (double)cnt);
  } else if (s.skyOnly) {
    // 方向约定:CPU 参考 = ComputeSH9FromEquirect(同一 hdr)
    int w = 0, h = 0;
    float* px = stbi_loadf(s.hdrPathForReference.generic_string().c_str(), &w, &h, nullptr, 4);
    if (px) {
      SH9 ref = ComputeSH9FromEquirect(px, w, h);
      for (auto& coeff : ref.c) for (float& value : coeff) value *= s.envScale;
      stbi_image_free(px);
      double c0Rel = 0.0, sumRef = 0.0, weightedRel = 0.0;
      for (int j = 0; j < 9; ++j)
        for (int c = 0; c < 3; ++c) sumRef += std::fabs(ref.c[j][c]);
      for(uint32_t i=0;i<probes;++i) {
        double sumDiff=0;
        for(int j=0;j<9;++j) for(int c=0;c<3;++c) {
          sumDiff+=std::fabs(finalized[i].c[j][c]-ref.c[j][c]);
          if(j==0) c0Rel=std::max(c0Rel,(double)std::fabs(finalized[i].c[0][c]-ref.c[0][c]) /
                                                std::max(1e-6,(double)std::fabs(ref.c[0][c])));
        }
        weightedRel=std::max(weightedRel,sumDiff/std::max(1e-6,sumRef));
      }
      spdlog::info("vlm validate direction: c0RelErr={:.6f} weightedRelErr={:.6f} (thresholds 0.010000/0.050000)",
                   c0Rel, weightedRel);
    } else {
      spdlog::warn("vlm validate direction: skipped (hdr not readable: {})",
                   s.hdrPathForReference.generic_string());
    }
  }

  {
    const float up[3] = {0, 1, 0};
    const uint32_t logCount = std::min(8u, probes);
    for (uint32_t i = 0; i < logCount; ++i) {
      float E[3];
      VlmShEvaluate(finalized[i], up, E);
      spdlog::info("vlm probe {} E(+Y)={:.6f},{:.6f},{:.6f}", i, E[0], E[1], E[2]);
    }
  }

  vkUnmapMemory(dev, _shAccumMemory);
  return true;
}
