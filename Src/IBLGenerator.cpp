#include "IBLGenerator.h"

#include "Common.h"       // readFile
#include "VulkanSetup.h"  // VulkanDevice

#include <array>
#include <cstring>
#include <stdexcept>

namespace {

VkShaderModule loadModule(VkDevice dev, const std::filesystem::path& path) {
  std::vector<char> code = readFile(path.generic_string());
  VkShaderModuleCreateInfo ci{};
  ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
  ci.codeSize = code.size();
  ci.pCode = reinterpret_cast<const uint32_t*>(code.data());
  VkShaderModule m;
  if (vkCreateShaderModule(dev, &ci, nullptr, &m) != VK_SUCCESS)
    throw std::runtime_error("IBL: failed to create shader module");
  return m;
}

void createImage2D(const VulkanDevice& device, VkFormat format, uint32_t w, uint32_t h,
                   uint32_t mips, uint32_t layers, VkImageCreateFlags flags,
                   VkImageUsageFlags usage, VkImage& image, VkDeviceMemory& memory) {
  VkImageCreateInfo ci{};
  ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  ci.imageType = VK_IMAGE_TYPE_2D;
  ci.extent = {w, h, 1};
  ci.mipLevels = mips;
  ci.arrayLayers = layers;
  ci.format = format;
  ci.tiling = VK_IMAGE_TILING_OPTIMAL;
  ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  ci.usage = usage;
  ci.samples = VK_SAMPLE_COUNT_1_BIT;
  ci.flags = flags;
  if (vkCreateImage(device.getLogicalDevice(), &ci, nullptr, &image) != VK_SUCCESS)
    throw std::runtime_error("IBL: failed to create image");
  VkMemoryRequirements mr;
  vkGetImageMemoryRequirements(device.getLogicalDevice(), image, &mr);
  VkMemoryAllocateInfo ai{};
  ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  ai.allocationSize = mr.size;
  ai.memoryTypeIndex = device.findMemoryType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  if (vkAllocateMemory(device.getLogicalDevice(), &ai, nullptr, &memory) != VK_SUCCESS)
    throw std::runtime_error("IBL: failed to allocate image memory");
  vkBindImageMemory(device.getLogicalDevice(), image, memory, 0);
}

void transitionRange(VkCommandBuffer cmd, VkImage image,
                     VkImageLayout oldLayout, VkImageLayout newLayout,
                     uint32_t baseMip, uint32_t mipCount, uint32_t layerCount,
                     VkAccessFlags srcAccess, VkAccessFlags dstAccess,
                     VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage) {
  VkImageMemoryBarrier b{};
  b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  b.oldLayout = oldLayout;
  b.newLayout = newLayout;
  b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.image = image;
  b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, baseMip, mipCount, 0, layerCount};
  b.srcAccessMask = srcAccess;
  b.dstAccessMask = dstAccess;
  vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &b);
}

} // namespace

IBLResources IBLGenerator::generate(const VulkanDevice& device, VkImageView equirectView,
                                    const std::filesystem::path& rootPath) {
  VkDevice dev = device.getLogicalDevice();
  IBLResources out;

  // --- sampler(生成与光照共用:三线性 + clamp) ---
  VkSamplerCreateInfo si{};
  si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
  si.magFilter = VK_FILTER_LINEAR;
  si.minFilter = VK_FILTER_LINEAR;
  si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
  si.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  si.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  si.maxLod = (float)kEnvMipCount;
  if (vkCreateSampler(dev, &si, nullptr, &out.sampler) != VK_SUCCESS)
    throw std::runtime_error("IBL: failed to create sampler");

  // --- 目标资源 ---
  createImage2D(device, VK_FORMAT_R16G16B16A16_SFLOAT, kEnvMapSize, kEnvMapSize,
                kEnvMipCount, 6, VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT,
                VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT,
                out.envCube, out.envCubeMemory);

  // mip0 的 2D-array storage view(CS1 输出)
  VkImageViewCreateInfo vi{};
  vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  vi.image = out.envCube;
  vi.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
  vi.format = VK_FORMAT_R16G16B16A16_SFLOAT;
  vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 6};
  VkImageView mip0StorageView;
  vkCreateImageView(dev, &vi, nullptr, &mip0StorageView);

  // cube 采样 view(全 mip;Task 4 预过滤读 mip0、光照 SampleLevel 用)
  vi.viewType = VK_IMAGE_VIEW_TYPE_CUBE;
  vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, kEnvMipCount, 0, 6};
  vkCreateImageView(dev, &vi, nullptr, &out.envCubeView);

  // --- CS1 pipeline ---
  VkDescriptorSetLayoutBinding cs1Bindings[3] = {};
  cs1Bindings[0] = {0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
  cs1Bindings[1] = {1, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
  cs1Bindings[2] = {2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
  VkDescriptorSetLayoutCreateInfo slci{};
  slci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  slci.bindingCount = 3;
  slci.pBindings = cs1Bindings;
  VkDescriptorSetLayout cs1SetLayout;
  vkCreateDescriptorSetLayout(dev, &slci, nullptr, &cs1SetLayout);

  VkPushConstantRange pcRange{VK_SHADER_STAGE_COMPUTE_BIT, 0, 16};
  VkPipelineLayoutCreateInfo plci{};
  plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  plci.setLayoutCount = 1;
  plci.pSetLayouts = &cs1SetLayout;
  plci.pushConstantRangeCount = 1;
  plci.pPushConstantRanges = &pcRange;
  VkPipelineLayout cs1Layout;
  vkCreatePipelineLayout(dev, &plci, nullptr, &cs1Layout);

  VkShaderModule cs1Module = loadModule(dev, rootPath / "shaders" / "ibl_equirect.cs.spv");
  VkComputePipelineCreateInfo cpci{};
  cpci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
  cpci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                VK_SHADER_STAGE_COMPUTE_BIT, cs1Module, "EquirectToCubeCS", nullptr};
  cpci.layout = cs1Layout;
  VkPipeline cs1Pipeline;
  if (vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpci, nullptr, &cs1Pipeline) != VK_SUCCESS)
    throw std::runtime_error("IBL: failed to create equirect-to-cube pipeline");

  // --- descriptor pool/set(生成专用,一次性) ---
  VkDescriptorPoolSize poolSizes[] = {
      {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 4},
      {VK_DESCRIPTOR_TYPE_SAMPLER, 4},
      {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 12}, // Task 4 会用到 9 个 mip view
  };
  VkDescriptorPoolCreateInfo dpci{};
  dpci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  dpci.maxSets = 12;
  dpci.poolSizeCount = 3;
  dpci.pPoolSizes = poolSizes;
  VkDescriptorPool pool;
  vkCreateDescriptorPool(dev, &dpci, nullptr, &pool);

  VkDescriptorSetAllocateInfo dsai{};
  dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  dsai.descriptorPool = pool;
  dsai.descriptorSetCount = 1;
  dsai.pSetLayouts = &cs1SetLayout;
  VkDescriptorSet cs1Set;
  vkAllocateDescriptorSets(dev, &dsai, &cs1Set);

  // 注意:VkDescriptorImageInfo 字段序为 (sampler, imageView, imageLayout),
  // 与 GpuScene.cpp:1383 一致用成员赋值,避免 brace 顺序错误。
  VkDescriptorImageInfo eqInfo{};
  eqInfo.imageView = equirectView;
  eqInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  VkDescriptorImageInfo sampInfo{};
  sampInfo.sampler = out.sampler;
  VkDescriptorImageInfo outInfo{};
  outInfo.imageView = mip0StorageView;
  outInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
  VkWriteDescriptorSet writes[3] = {};
  writes[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, cs1Set, 0, 0, 1,
               VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &eqInfo, nullptr, nullptr};
  writes[1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, cs1Set, 1, 0, 1,
               VK_DESCRIPTOR_TYPE_SAMPLER, &sampInfo, nullptr, nullptr};
  writes[2] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, cs1Set, 2, 0, 1,
               VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &outInfo, nullptr, nullptr};
  vkUpdateDescriptorSets(dev, 3, writes, 0, nullptr);

  // --- 录制 one-shot 命令 ---
  VkCommandBuffer cmd = device.beginSingleTimeCommands();

  // 全 mip → GENERAL(生成期统一;Task 4 写 mip1-8 无需再转)
  transitionRange(cmd, out.envCube, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                  0, kEnvMipCount, 6,
                  0, VK_ACCESS_SHADER_WRITE_BIT,
                  VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, cs1Pipeline);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, cs1Layout, 0, 1, &cs1Set, 0, nullptr);
  uint32_t pc[4] = {kEnvMapSize, 0, 0, 0};
  vkCmdPushConstants(cmd, cs1Layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 16, pc);
  vkCmdDispatch(cmd, kEnvMapSize / 8, kEnvMapSize / 8, 6);

  // mip0: STORAGE_WRITE → SHADER_READ(Task 4 的预过滤从 mip0 采样)
  transitionRange(cmd, out.envCube, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                  0, 1, 6,
                  VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                  VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

  // === Task 4 锚点:在此插入 PrefilterSpecularCS 的 mip1-8 dispatch ===
  // === Task 5 锚点:在此插入 DfgLutCS dispatch ===

  // 最终:全 mip → SHADER_READ_ONLY
  transitionRange(cmd, out.envCube, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                  0, kEnvMipCount, 6,
                  VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_SHADER_READ_BIT,
                  VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);

  device.endSingleTimeCommands(cmd);

  // 清理生成期临时对象(输出资源保留)
  vkDestroyImageView(dev, mip0StorageView, nullptr);
  vkDestroyShaderModule(dev, cs1Module, nullptr);
  vkDestroyPipeline(dev, cs1Pipeline, nullptr);
  vkDestroyPipelineLayout(dev, cs1Layout, nullptr);
  vkDestroyDescriptorSetLayout(dev, cs1SetLayout, nullptr);
  vkDestroyDescriptorPool(dev, pool, nullptr);

  return out;
}
