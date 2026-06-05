
#include "GpuScene.h"
#include "AssetLoader.h"
#include "Light.h"
#include "ObjLoader.h"
#include "Raytracing.h"
#include "Shadow.h"
#include "ThirdParty/lzfse.h"
#include "VulkanCompat.h"
#include "VulkanSetup.h"
#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_vulkan.h"


#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <vector>


// USE_CPU_ENCODE_DRAWPARAM removed: GPU indirect draw is now the default path
#define LIGHT_FOR_TRANSPARENT_FLAG (0x00000001)

// TODO: move to common.cpp
std::vector<char> readFile(const std::string &filename) {
  return AssetLoader::readFileAsset(filename);
}

// Header for compressed blocks.
struct AAPLCompressionHeader {
  uint32_t compressionMode;  // Compression mode in block - of type
                             // compression_algorithm.
  uint64_t uncompressedSize; // Size of uncompressed data.
  uint64_t compressedSize;   // Size of compressed data.
};

AAPLCompressionHeader *getCompressionHeader(void *data, size_t length) {
  assert(data != nullptr);

  if (length < sizeof(AAPLCompressionHeader)) {
    spdlog::error("Data is too small");

    exit(1);
  }

  AAPLCompressionHeader *header = ((AAPLCompressionHeader *)data);

  if (length != sizeof(AAPLCompressionHeader) + header->compressedSize) {
    spdlog::error("Data length mismatch");
    exit(1);
  }

  return header;
}

size_t uncompressedDataSize(void *data, size_t datalength) {
  return getCompressionHeader(data, datalength)->uncompressedSize;
}

void uncompressData(const AAPLCompressionHeader &header, const void *data,
                    void *dstBuffer) {

  if (header.compressionMode != 2049) {
    spdlog::error("something that are not compressed using apple format");
  }

  // size_t a = compression_decode_buffer((uint8_t*)dstBuffer,
  // header.uncompressedSize,
  //                                      (const uint8_t*)data,
  //                                      header.compressedSize, NULL,
  //                                      (compression_algorithm)header.compressionMode);
  lzfse_decode_buffer((uint8_t *)dstBuffer, header.uncompressedSize,
                      (const uint8_t *)data, header.compressedSize, NULL);
}

void *uncompressData(unsigned char *data, size_t dataLength,
                     uint64_t expectedsize) {

  AAPLCompressionHeader *header = getCompressionHeader(data, dataLength);
  if (expectedsize != header->uncompressedSize)
    spdlog::warn("texture mipmap data corrputed");

  void *decompressedData = malloc(header->uncompressedSize);

  uncompressData(*header, (header + 1), decompressedData);

  return decompressedData;
}

// typedef void* (*AllocatorCallback)(size_t);

void *uncompressData(void *data, size_t dataLength,
                     std::function<void *(uint64_t)> allocatorCallback) {
  AAPLCompressionHeader *header = getCompressionHeader(data, dataLength);

  void *dstBuffer = allocatorCallback(header->uncompressedSize);

  uncompressData(*header, (header + 1), dstBuffer);
  return dstBuffer;
}

VkShaderModule
GpuScene::createShaderModule(const std::vector<char> &code) const {
  VkShaderModuleCreateInfo createInfo{};
  createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
  createInfo.codeSize = code.size();
  createInfo.pCode = reinterpret_cast<const uint32_t *>(code.data());

  VkShaderModule shaderModule;
  if (vkCreateShaderModule(device.getLogicalDevice(), &createInfo, nullptr,
                           &shaderModule) != VK_SUCCESS) {
    throw std::runtime_error("failed to create shader module!");
  }

  return shaderModule;
}

// TODO: cache the pso
void GpuScene::createRenderOccludersPipeline(VkRenderPass renderPass) {
  auto occludersVSShaderCode =
      readFile((_rootPath / "shaders/occluders.vs.spv").generic_string());
  VkShaderModule occludersVSShaderModule =
      createShaderModule(occludersVSShaderCode);
  VkPipelineShaderStageCreateInfo drawOccludersVSShaderStageInfo{};
  drawOccludersVSShaderStageInfo.sType =
      VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  drawOccludersVSShaderStageInfo.stage = VK_SHADER_STAGE_VERTEX_BIT;
  drawOccludersVSShaderStageInfo.module = occludersVSShaderModule;
  drawOccludersVSShaderStageInfo.pName = "RenderSceneVS";

  // we don't need fragment stage
  VkPipelineShaderStageCreateInfo shaderStages[] = {
      drawOccludersVSShaderStageInfo};

  VkVertexInputBindingDescription occluderInputBinding = {
      .binding = 0,
      .stride = sizeof(float) * 3,
      .inputRate = VK_VERTEX_INPUT_RATE_VERTEX};

  VkVertexInputAttributeDescription occluderInputAttributes[] = {
      {.location = 0,
       .binding = 0,
       .format = VK_FORMAT_R32G32B32_SFLOAT,
       .offset = 0},
  };

  VkPipelineVertexInputStateCreateInfo occluderVertexInputInfo{};
  occluderVertexInputInfo.sType =
      VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
  occluderVertexInputInfo.vertexBindingDescriptionCount = 1;
  occluderVertexInputInfo.pVertexBindingDescriptions = &occluderInputBinding;
  occluderVertexInputInfo.vertexAttributeDescriptionCount =
      sizeof(occluderInputAttributes) / sizeof(occluderInputAttributes[0]);
  occluderVertexInputInfo.pVertexAttributeDescriptions =
      occluderInputAttributes;

  VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
  inputAssembly.sType =
      VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
  inputAssembly.topology =
      VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; // change to strip
  inputAssembly.primitiveRestartEnable = VK_FALSE;

  const VkExtent2D &swapChainExtentRef = device.getSwapChainExtent();
  VkViewport viewport{};
  viewport.x = 0.0f;
  viewport.y = 0.0f;
  viewport.width = (float)swapChainExtentRef.width;
  viewport.height = (float)swapChainExtentRef.height;
  viewport.minDepth = 0.0f;
  viewport.maxDepth = 1.0f;

  VkRect2D scissor{};
  scissor.offset = {0, 0};
  scissor.extent = swapChainExtentRef;

  VkPipelineViewportStateCreateInfo viewportState{};
  viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
  viewportState.viewportCount = 1;
  viewportState.pViewports = &viewport;
  viewportState.scissorCount = 1;
  viewportState.pScissors = &scissor;

  VkPipelineRasterizationStateCreateInfo rasterizer{};
  rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
  rasterizer.depthClampEnable = VK_FALSE;
  rasterizer.rasterizerDiscardEnable = VK_FALSE;
  rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
  rasterizer.lineWidth = 1.0f;
  rasterizer.cullMode = VK_CULL_MODE_NONE;
  rasterizer.frontFace = VK_FRONT_FACE_CLOCKWISE;
  rasterizer.depthBiasEnable = VK_FALSE;

  VkPipelineMultisampleStateCreateInfo multisampling{};
  multisampling.sType =
      VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
  multisampling.sampleShadingEnable = VK_FALSE;
  multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

  VkPipelineColorBlendAttachmentState colorBlendAttachment{};
  colorBlendAttachment.colorWriteMask =
      VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
      VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
  colorBlendAttachment.blendEnable = VK_FALSE;
  VkPipelineColorBlendStateCreateInfo colorBlending{};
  colorBlending.sType =
      VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
  colorBlending.logicOpEnable = VK_FALSE;
  colorBlending.logicOp = VK_LOGIC_OP_COPY;
  colorBlending.attachmentCount = 1;
  colorBlending.pAttachments = &colorBlendAttachment;
  colorBlending.blendConstants[0] = 0.0f;
  colorBlending.blendConstants[1] = 0.0f;
  colorBlending.blendConstants[2] = 0.0f;
  colorBlending.blendConstants[3] = 0.0f;

  VkPushConstantRange pushconstantRange = {.stageFlags =
                                               VK_SHADER_STAGE_VERTEX_BIT,
                                           .offset = 0,
                                           .size = sizeof(mat4)};

  VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
  pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  pipelineLayoutInfo.setLayoutCount = 1;
  pipelineLayoutInfo.pSetLayouts =
      &globalSetLayout; // TODO: use seperate layout??
  pipelineLayoutInfo.pushConstantRangeCount = 0;

  if (vkCreatePipelineLayout(device.getLogicalDevice(), &pipelineLayoutInfo,
                             nullptr, &pipelineLayout) != VK_SUCCESS) {
    throw std::runtime_error("failed to create pipeline layout!");
  }

  VkGraphicsPipelineCreateInfo pipelineInfo{};
  pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
  pipelineInfo.stageCount = 1;
  pipelineInfo.pStages = shaderStages;
  pipelineInfo.pVertexInputState = &occluderVertexInputInfo;
  pipelineInfo.pInputAssemblyState = &inputAssembly;
  pipelineInfo.pViewportState = &viewportState;
  pipelineInfo.pRasterizationState = &rasterizer;
  pipelineInfo.pMultisampleState = &multisampling;
  pipelineInfo.pColorBlendState = &colorBlending;
  pipelineInfo.layout =
      pipelineLayout; // TODO: seperate layout? currently just reuse
  pipelineInfo.renderPass = occluderZPass;
  pipelineInfo.subpass = 0;
  pipelineInfo.basePipelineHandle = VK_NULL_HANDLE;

  VkPipelineDepthStencilStateCreateInfo depthStencilState1{};
  depthStencilState1.sType =
      VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
  depthStencilState1.depthWriteEnable = VK_TRUE;
  depthStencilState1.depthTestEnable = VK_TRUE;
  depthStencilState1.stencilTestEnable = VK_FALSE;
  depthStencilState1.depthCompareOp = VK_COMPARE_OP_GREATER;
  depthStencilState1.depthBoundsTestEnable = VK_FALSE;
  // The Vulkan spec states: If renderPass is not VK_NULL_HANDLE, the pipeline
  // is being created with fragment shader state, and subpass uses a
  // depth/stencil attachment, pDepthStencilState must be a valid pointer to a
  // valid VkPipelineDepthStencilStateCreateInfo structure
  pipelineInfo.pDepthStencilState = &depthStencilState1;

  if (vkCreateGraphicsPipelines(device.getLogicalDevice(), VK_NULL_HANDLE, 1,
                                &pipelineInfo, nullptr,
                                &drawOccluderPipeline) != VK_SUCCESS) {
    throw std::runtime_error("failed to create graphics pipeline!");
  }
}

void GpuScene::createComputePipeline() {
  auto computeShaderCode =
      readFile((_rootPath / "shaders/gpucull.cs.spv").generic_string());
  VkShaderModule computeShaderModule = createShaderModule(computeShaderCode);
  VkPipelineShaderStageCreateInfo computeStageInfo{};
  computeStageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  computeStageInfo.stage = VK_SHADER_STAGE_COMPUTE_BIT;
  computeStageInfo.module = computeShaderModule;
  computeStageInfo.pName = "EncodeDrawBuffer";

  VkPipelineLayoutCreateInfo encodeDrawBufferPipelineLayoutInfo{};
  encodeDrawBufferPipelineLayoutInfo.sType =
      VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  encodeDrawBufferPipelineLayoutInfo.setLayoutCount = 1;
  encodeDrawBufferPipelineLayoutInfo.pSetLayouts = &gpuCullSetLayout;
  encodeDrawBufferPipelineLayoutInfo.pushConstantRangeCount = 0;

  if (vkCreatePipelineLayout(device.getLogicalDevice(),
                             &encodeDrawBufferPipelineLayoutInfo, nullptr,
                             &encodeDrawBufferPipelineLayout) != VK_SUCCESS) {
    throw std::runtime_error("failed to create drawcluster pipeline layout!");
  }
  VkComputePipelineCreateInfo computePipelineCreateInfo{};
  computePipelineCreateInfo.sType =
      VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
  computePipelineCreateInfo.layout = encodeDrawBufferPipelineLayout;
  computePipelineCreateInfo.stage = computeStageInfo;
  computePipelineCreateInfo.basePipelineHandle = VK_NULL_HANDLE;

  vkCreateComputePipelines(device.getLogicalDevice(), VK_NULL_HANDLE, 1,
                           &computePipelineCreateInfo, nullptr,
                           &encodeDrawBufferPipeline);
}

void GpuScene::createGraphicsPipeline(VkRenderPass renderPass) {
  // TODO: shader management -- hot reload

  auto drawClusterVSShaderCode =
      readFile((_rootPath / "shaders/drawcluster.vs.spv").generic_string());
  auto drawClusterPSShaderCode =
      readFile((_rootPath / "shaders/drawcluster.ps.spv").generic_string());

  auto drawClusterBasePSShaderCode = readFile(
      (_rootPath / "shaders/drawcluster.base.ps.spv").generic_string());

  auto drawClusterForwardPsShaderCode = readFile(
      (_rootPath / "shaders/drawcluster.forward.ps.spv").generic_string());

  auto drawClusterBasePassAlphaMaskPSCode = readFile(
      (_rootPath / "shaders/drawcluster.base.alphamask.ps.spv").generic_string());

  auto drawClusterForwardIndirectPSCode = readFile(
      (_rootPath / "shaders/drawcluster.forward.indirect.ps.spv").generic_string());

  auto deferredLightingVSShaderCode = readFile(
      (_rootPath / "shaders/deferredlighting.vs.spv").generic_string());
  auto deferredLightingPSShaderCode = readFile(
      (_rootPath / "shaders/deferredlighting.ps.spv").generic_string());



  VkShaderModule drawclusterVSShaderModule =
      createShaderModule(drawClusterVSShaderCode);
  VkShaderModule drawclusterPSShaderModule =
      createShaderModule(drawClusterPSShaderCode);
  VkShaderModule drawclusterBasePSShaderModule =
      createShaderModule(drawClusterBasePSShaderCode);
  VkShaderModule drawclusterForwardPSShaderModule =
      createShaderModule(drawClusterForwardPsShaderCode);

  VkShaderModule drawclusterBasePassAlphaMaskPSModule =
      createShaderModule(drawClusterBasePassAlphaMaskPSCode);

  VkShaderModule drawclusterForwardIndirectPSModule =
      createShaderModule(drawClusterForwardIndirectPSCode);

  VkShaderModule deferredLightingVSShaderModule =
      createShaderModule(deferredLightingVSShaderCode);
  VkShaderModule deferredLightingPSShaderModule =
      createShaderModule(deferredLightingPSShaderCode);


  VkPipelineShaderStageCreateInfo drawclusterVSShaderStageInfo{};
  drawclusterVSShaderStageInfo.sType =
      VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  drawclusterVSShaderStageInfo.stage = VK_SHADER_STAGE_VERTEX_BIT;
  drawclusterVSShaderStageInfo.module = drawclusterVSShaderModule;
  drawclusterVSShaderStageInfo.pName = "RenderSceneVS";

  VkPipelineShaderStageCreateInfo deferredLightingVSShaderStageInfo{};
  deferredLightingVSShaderStageInfo.sType =
      VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  deferredLightingVSShaderStageInfo.stage = VK_SHADER_STAGE_VERTEX_BIT;
  deferredLightingVSShaderStageInfo.module = deferredLightingVSShaderModule;
  deferredLightingVSShaderStageInfo.pName =
      "AAPLSimpleTexVertexOutFSQuadVertexShader";

  VkPipelineShaderStageCreateInfo drawclusterPSShaderStageInfo{};
  drawclusterPSShaderStageInfo.sType =
      VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  drawclusterPSShaderStageInfo.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  drawclusterPSShaderStageInfo.module = drawclusterPSShaderModule;
  drawclusterPSShaderStageInfo.pName = "RenderSceneBasePS";

  VkSpecializationMapEntry mapEntry = {};
  mapEntry.constantID = 0; // matches constant_id in GLSL and SpecId in SPIR-V
  mapEntry.offset = 0;
  mapEntry.size = sizeof(VkBool32);

  VkBool32 alphaMask = true;
  VkSpecializationInfo specializationInfo = {};
  specializationInfo.mapEntryCount = 1;
  specializationInfo.pMapEntries = &mapEntry;
  specializationInfo.dataSize = sizeof(VkBool32);
  specializationInfo.pData = &alphaMask;

  VkPipelineShaderStageCreateInfo drawclusterPSShaderStageInfoAlphaMask{};
  drawclusterPSShaderStageInfoAlphaMask.sType =
      VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  drawclusterPSShaderStageInfoAlphaMask.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  drawclusterPSShaderStageInfoAlphaMask.module = drawclusterPSShaderModule;
  drawclusterPSShaderStageInfoAlphaMask.pName = "RenderSceneBasePS";
  drawclusterPSShaderStageInfoAlphaMask.pSpecializationInfo =
      &specializationInfo;

  VkPipelineShaderStageCreateInfo drawclusterBasePSShaderStageInfo{};
  drawclusterBasePSShaderStageInfo.sType =
      VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  drawclusterBasePSShaderStageInfo.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  drawclusterBasePSShaderStageInfo.module = drawclusterBasePSShaderModule;
  drawclusterBasePSShaderStageInfo.pName = "RenderSceneBasePass";

  VkPipelineShaderStageCreateInfo drawclusterForwardPSShaderStageInfo{};
  drawclusterForwardPSShaderStageInfo.sType =
      VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  drawclusterForwardPSShaderStageInfo.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  drawclusterForwardPSShaderStageInfo.module =
      drawclusterForwardPSShaderModule; // TODO:
                                        // 这几个module应该可以合并，在dxc中添加适当的参数？
  drawclusterForwardPSShaderStageInfo.pName = "RenderSceneForwardPS";

  VkPipelineShaderStageCreateInfo drawclusterBaseAlphaMaskPSStageInfo{};
  drawclusterBaseAlphaMaskPSStageInfo.sType =
      VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  drawclusterBaseAlphaMaskPSStageInfo.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  drawclusterBaseAlphaMaskPSStageInfo.module = drawclusterBasePassAlphaMaskPSModule;
  drawclusterBaseAlphaMaskPSStageInfo.pName = "RenderSceneBasePassAlphaMask";

  VkPipelineShaderStageCreateInfo drawclusterForwardIndirectPSStageInfo{};
  drawclusterForwardIndirectPSStageInfo.sType =
      VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  drawclusterForwardIndirectPSStageInfo.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  drawclusterForwardIndirectPSStageInfo.module = drawclusterForwardIndirectPSModule;
  drawclusterForwardIndirectPSStageInfo.pName = "RenderSceneForwardPSIndirect";

  VkBool32 useClusterLighting = true;
  VkSpecializationInfo specializationInfo_clusterlighting = {};
  specializationInfo_clusterlighting.mapEntryCount = 1;
  specializationInfo_clusterlighting.pMapEntries = &mapEntry;
  specializationInfo_clusterlighting.dataSize = sizeof(VkBool32);
  specializationInfo_clusterlighting.pData = &useClusterLighting;

  VkPipelineShaderStageCreateInfo
      deferredLightingPSShaderStageInfo_clusterlighting{};
  deferredLightingPSShaderStageInfo_clusterlighting.sType =
      VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  deferredLightingPSShaderStageInfo_clusterlighting.stage =
      VK_SHADER_STAGE_FRAGMENT_BIT;
  deferredLightingPSShaderStageInfo_clusterlighting.module =
      deferredLightingPSShaderModule;
  deferredLightingPSShaderStageInfo_clusterlighting.pName = "DeferredLighting";
  deferredLightingPSShaderStageInfo_clusterlighting.pSpecializationInfo =
      &specializationInfo_clusterlighting;

  VkPipelineShaderStageCreateInfo drawclusterShaderStages[] = {
      drawclusterVSShaderStageInfo, drawclusterPSShaderStageInfo};

  VkPipelineShaderStageCreateInfo drawclusterShaderStagesAlphaMask[] = {
      drawclusterVSShaderStageInfo, drawclusterPSShaderStageInfoAlphaMask};

  VkPipelineShaderStageCreateInfo drawclusterBasePassStages[] = {
      drawclusterVSShaderStageInfo, drawclusterBasePSShaderStageInfo};

  VkPipelineShaderStageCreateInfo drawclusterForwardStages[] = {
      drawclusterVSShaderStageInfo, drawclusterForwardPSShaderStageInfo};

  VkPipelineShaderStageCreateInfo deferredLightingPassStages[] = {
      deferredLightingVSShaderStageInfo, deferredLightingPSShaderStageInfo_clusterlighting};

  VkPipelineShaderStageCreateInfo deferredLightingPassStages_clusterlighting[] =
      {deferredLightingVSShaderStageInfo,
       deferredLightingPSShaderStageInfo_clusterlighting};

  VkPipelineVertexInputStateCreateInfo vertexInputInfo{};
  vertexInputInfo.sType =
      VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
  vertexInputInfo.vertexBindingDescriptionCount = 0;
  vertexInputInfo.vertexAttributeDescriptionCount = 0;

  VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
  inputAssembly.sType =
      VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
  inputAssembly.topology =
      VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; // change to strip
  inputAssembly.primitiveRestartEnable = VK_FALSE;

  const VkExtent2D &swapChainExtentRef = device.getSwapChainExtent();
  VkViewport viewport{};
  viewport.x = 0.0f;
  viewport.y = 0.0f;
  viewport.width = (float)swapChainExtentRef.width;
  viewport.height = (float)swapChainExtentRef.height;
  viewport.minDepth = 0.0f;
  viewport.maxDepth = 1.0f;

  VkRect2D scissor{};
  scissor.offset = {0, 0};
  scissor.extent = swapChainExtentRef;

  VkPipelineViewportStateCreateInfo viewportState{};
  viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
  viewportState.viewportCount = 1;
  viewportState.pViewports = &viewport;
  viewportState.scissorCount = 1;
  viewportState.pScissors = &scissor;

  VkPipelineRasterizationStateCreateInfo rasterizer{};
  rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
  rasterizer.depthClampEnable = VK_FALSE;
  rasterizer.rasterizerDiscardEnable = VK_FALSE;
  rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
  rasterizer.lineWidth = 1.0f;
  rasterizer.cullMode = VK_CULL_MODE_BACK_BIT;
  rasterizer.frontFace = VK_FRONT_FACE_CLOCKWISE;
  rasterizer.depthBiasEnable = VK_FALSE;

  VkPipelineRasterizationStateCreateInfo rasterizerBackFace{};
  rasterizerBackFace.sType =
      VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
  rasterizerBackFace.depthClampEnable = VK_FALSE;
  rasterizerBackFace.rasterizerDiscardEnable = VK_FALSE;
  rasterizerBackFace.polygonMode = VK_POLYGON_MODE_FILL;
  rasterizerBackFace.lineWidth = 1.0f;
  rasterizerBackFace.cullMode = VK_CULL_MODE_BACK_BIT;
  rasterizerBackFace.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
  rasterizerBackFace.depthBiasEnable = VK_FALSE;

  VkPipelineRasterizationStateCreateInfo rasterizer_wireframe{};
  rasterizer_wireframe.sType =
      VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
  rasterizer_wireframe.depthClampEnable = VK_FALSE;
  rasterizer_wireframe.rasterizerDiscardEnable = VK_FALSE;
  rasterizer_wireframe.polygonMode = VK_POLYGON_MODE_FILL;
  rasterizer_wireframe.lineWidth = 1.0f;
  rasterizer_wireframe.cullMode = VK_CULL_MODE_NONE;
  rasterizer_wireframe.frontFace = VK_FRONT_FACE_CLOCKWISE;
  rasterizer_wireframe.depthBiasEnable = VK_FALSE;

  VkPipelineMultisampleStateCreateInfo multisampling{};
  multisampling.sType =
      VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
  multisampling.sampleShadingEnable = VK_FALSE;
  multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

  VkPipelineColorBlendAttachmentState colorBlendAttachment{};
  colorBlendAttachment.colorWriteMask =
      VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
      VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
  colorBlendAttachment.blendEnable = VK_FALSE;

  VkPipelineColorBlendAttachmentState colorBlendAttachment1{};
  colorBlendAttachment1.colorWriteMask =
      VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
      VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
  colorBlendAttachment1.blendEnable = VK_TRUE;
  colorBlendAttachment1.colorBlendOp = VK_BLEND_OP_ADD;
  colorBlendAttachment1.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
  colorBlendAttachment1.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
  colorBlendAttachment1.alphaBlendOp = VK_BLEND_OP_ADD;
  colorBlendAttachment1.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
  colorBlendAttachment1.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
  // colorBlendAttachment1.alphaBlendOp = VK_BLEND_OP_SRC_EXT;
  // colorBlendAttachment1.srcAlphaBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;

  VkPipelineColorBlendStateCreateInfo colorBlending{};
  colorBlending.sType =
      VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
  colorBlending.logicOpEnable = VK_FALSE;
  colorBlending.logicOp = VK_LOGIC_OP_COPY;
  colorBlending.attachmentCount = 1;
  colorBlending.pAttachments = &colorBlendAttachment;
  colorBlending.blendConstants[0] = 0.0f;
  colorBlending.blendConstants[1] = 0.0f;
  colorBlending.blendConstants[2] = 0.0f;
  colorBlending.blendConstants[3] = 0.0f;

  VkPipelineColorBlendStateCreateInfo colorBlendingAlpha{};
  colorBlendingAlpha.sType =
      VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
  colorBlendingAlpha.logicOpEnable = VK_FALSE;
  colorBlendingAlpha.logicOp = VK_LOGIC_OP_COPY;
  colorBlendingAlpha.attachmentCount = 1;
  colorBlendingAlpha.pAttachments = &colorBlendAttachment1;
  colorBlendingAlpha.blendConstants[0] = 1.0f;
  colorBlendingAlpha.blendConstants[1] = 1.0f;
  colorBlendingAlpha.blendConstants[2] = 1.0f;
  colorBlendingAlpha.blendConstants[3] = 1.0f;

  VkPipelineColorBlendAttachmentState colorBlendAttachments[4]{};

  for (int i = 0; i < 4; i++) {
    colorBlendAttachments[i].colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    colorBlendAttachments[i].blendEnable = VK_FALSE;
  }

  VkPipelineColorBlendStateCreateInfo colorBlendings{};
  colorBlendings.sType =
      VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
  colorBlendings.logicOpEnable = VK_FALSE;
  colorBlendings.logicOp = VK_LOGIC_OP_COPY;
  colorBlendings.attachmentCount = 4;
  colorBlendings.pAttachments = colorBlendAttachments;
  colorBlendings.blendConstants[0] = 0.0f;
  colorBlendings.blendConstants[1] = 0.0f;
  colorBlendings.blendConstants[2] = 0.0f;
  colorBlendings.blendConstants[3] = 0.0f;

  VkDescriptorSetLayout drawclusterLayouts[] = {globalSetLayout, applSetLayout};
  VkPushConstantRange drawclusterpushconstantRange = {
      .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
      .offset = 0,
      .size = sizeof(PerObjPush)};
  VkPipelineLayoutCreateInfo drawclusterpipelineLayoutInfo{};
  drawclusterpipelineLayoutInfo.sType =
      VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  drawclusterpipelineLayoutInfo.setLayoutCount = 2;
  drawclusterpipelineLayoutInfo.pSetLayouts = drawclusterLayouts;
  drawclusterpipelineLayoutInfo.pushConstantRangeCount = 1;
  drawclusterpipelineLayoutInfo.pPushConstantRanges =
      &drawclusterpushconstantRange;

  if (vkCreatePipelineLayout(device.getLogicalDevice(),
                             &drawclusterpipelineLayoutInfo, nullptr,
                             &drawclusterPipelineLayout) != VK_SUCCESS) {
    throw std::runtime_error("failed to create drawcluster pipeline layout!");
  }

  VkPipelineLayoutCreateInfo drawclusterBasePipelineLayoutInfo{};
  drawclusterBasePipelineLayoutInfo.sType =
      VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  drawclusterBasePipelineLayoutInfo.setLayoutCount = 2;
  drawclusterBasePipelineLayoutInfo.pSetLayouts = drawclusterLayouts;
  drawclusterBasePipelineLayoutInfo.pushConstantRangeCount = 0;

  if (vkCreatePipelineLayout(device.getLogicalDevice(),
                             &drawclusterBasePipelineLayoutInfo, nullptr,
                             &drawclusterBasePipelineLayout) != VK_SUCCESS) {
    throw std::runtime_error("failed to create drawcluster pipeline layout!");
  }

  VkDescriptorSetLayout deferredLayouts[] = {globalSetLayout, deferredLightingSetLayout};
  VkPipelineLayoutCreateInfo deferredLightingPipelineLayoutInfo{};
  deferredLightingPipelineLayoutInfo.sType =
      VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  deferredLightingPipelineLayoutInfo.setLayoutCount = 2;
  deferredLightingPipelineLayoutInfo.pSetLayouts = deferredLayouts;
  deferredLightingPipelineLayoutInfo.pushConstantRangeCount = 0;

  if (vkCreatePipelineLayout(device.getLogicalDevice(),
                             &deferredLightingPipelineLayoutInfo, nullptr,
                             &deferredLightingPipelineLayout) != VK_SUCCESS) {
    throw std::runtime_error("failed to create drawcluster pipeline layout!");
  }

  VkPipelineDepthStencilStateCreateInfo depthStencilState1{};
  depthStencilState1.sType =
      VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
  depthStencilState1.depthWriteEnable = VK_FALSE;
  depthStencilState1.depthTestEnable = VK_FALSE;
  depthStencilState1.stencilTestEnable = VK_FALSE;

  VkPipelineVertexInputStateCreateInfo emptyVertexInputInfo{};
  emptyVertexInputInfo.sType =
      VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
  emptyVertexInputInfo.vertexBindingDescriptionCount = 0;
  emptyVertexInputInfo.pVertexBindingDescriptions = nullptr;
  emptyVertexInputInfo.vertexAttributeDescriptionCount = 0;
  emptyVertexInputInfo.pVertexAttributeDescriptions = nullptr;

  VkPipelineDepthStencilStateCreateInfo depthStencilState{};
  depthStencilState.sType =
      VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
  depthStencilState.depthWriteEnable = VK_TRUE;
  depthStencilState.depthTestEnable = VK_TRUE;
  depthStencilState.stencilTestEnable = VK_FALSE;
  depthStencilState.depthCompareOp = VK_COMPARE_OP_GREATER;
  depthStencilState.depthBoundsTestEnable = VK_FALSE;

  VkPipelineDepthStencilStateCreateInfo depthStencilStateDisable{};
  depthStencilStateDisable.sType =
      VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
  depthStencilStateDisable.depthWriteEnable = VK_FALSE;
  depthStencilStateDisable.depthTestEnable = VK_FALSE;
  depthStencilStateDisable.stencilTestEnable = VK_FALSE;
  depthStencilStateDisable.depthCompareOp = VK_COMPARE_OP_GREATER;
  depthStencilStateDisable.depthBoundsTestEnable = VK_FALSE;

  /* VkGraphicsPipelineCreateInfo edwardpipelineInfo{};
   edwardpipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
   edwardpipelineInfo.stageCount = 2;
   edwardpipelineInfo.pStages = eshaderStages;
   edwardpipelineInfo.pVertexInputState = &edwardVertexInputInfo;
   edwardpipelineInfo.pInputAssemblyState = &inputAssembly;
   edwardpipelineInfo.pViewportState = &viewportState;
   edwardpipelineInfo.pRasterizationState = &rasterizer_wireframe;
   edwardpipelineInfo.pMultisampleState = &multisampling;
   edwardpipelineInfo.pColorBlendState = &colorBlending;
   edwardpipelineInfo.layout = epipelineLayout;
   edwardpipelineInfo.renderPass = renderPass;
   edwardpipelineInfo.subpass = 0;
   edwardpipelineInfo.basePipelineHandle = VK_NULL_HANDLE;
   edwardpipelineInfo.pDepthStencilState = &depthStencilState;

   if (vkCreateGraphicsPipelines(device.getLogicalDevice(), VK_NULL_HANDLE, 1,
   &edwardpipelineInfo, nullptr, &egraphicsPipeline) != VK_SUCCESS) { throw
   std::runtime_error("failed to create edward graphics pipeline!");
   }*/

  constexpr VkVertexInputBindingDescription drawClusterInputBindingPosition = {
      .binding = 0,
      .stride = sizeof(float) * 3,
      .inputRate = VK_VERTEX_INPUT_RATE_VERTEX};
  constexpr VkVertexInputBindingDescription drawClusterInputBindingNormal = {
      .binding = 1,
      .stride = sizeof(float) * 3,
      .inputRate = VK_VERTEX_INPUT_RATE_VERTEX};
  constexpr VkVertexInputBindingDescription drawClusterInputBindingTangent = {
      .binding = 2,
      .stride = sizeof(float) * 3,
      .inputRate = VK_VERTEX_INPUT_RATE_VERTEX};
  constexpr VkVertexInputBindingDescription drawClusterInputBindingUV = {
      .binding = 3,
      .stride = sizeof(float) * 2,
      .inputRate = VK_VERTEX_INPUT_RATE_VERTEX};

  VkVertexInputAttributeDescription drawclusterInputAttributes[] = {
      {.location = 0,
       .binding = 0,
       .format = VK_FORMAT_R32G32B32_SFLOAT,
       .offset = 0},
      {.location = 1,
       .binding = 1,
       .format = VK_FORMAT_R32G32B32_SFLOAT,
       .offset = 0},
      {.location = 2,
       .binding = 2,
       .format = VK_FORMAT_R32G32B32_SFLOAT,
       .offset = 0},
      {.location = 3,
       .binding = 3,
       .format = VK_FORMAT_R32G32_SFLOAT,
       .offset = 0}};

  constexpr int inputChannelCount = sizeof(drawclusterInputAttributes) /
                                    sizeof(drawclusterInputAttributes[0]);

  constexpr std::array<VkVertexInputBindingDescription, inputChannelCount>
      drawculsterinputs = {
          drawClusterInputBindingPosition, drawClusterInputBindingNormal,
          drawClusterInputBindingTangent, drawClusterInputBindingUV};

  VkPipelineVertexInputStateCreateInfo drawclusterVertexInputInfo{};
  drawclusterVertexInputInfo.sType =
      VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
  drawclusterVertexInputInfo.vertexBindingDescriptionCount =
      drawculsterinputs.size();
  drawclusterVertexInputInfo.pVertexBindingDescriptions =
      drawculsterinputs.data();
  drawclusterVertexInputInfo.vertexAttributeDescriptionCount =
      inputChannelCount;
  drawclusterVertexInputInfo.pVertexAttributeDescriptions =
      drawclusterInputAttributes;

  VkGraphicsPipelineCreateInfo drawclusterpipelineInfo{};
  drawclusterpipelineInfo.sType =
      VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
  drawclusterpipelineInfo.stageCount = 2;
  drawclusterpipelineInfo.pStages = drawclusterShaderStages;
  drawclusterpipelineInfo.pVertexInputState = &drawclusterVertexInputInfo;
  drawclusterpipelineInfo.pInputAssemblyState = &inputAssembly;
  drawclusterpipelineInfo.pViewportState = &viewportState;
  drawclusterpipelineInfo.pRasterizationState = &rasterizer;
  drawclusterpipelineInfo.pMultisampleState = &multisampling;
  drawclusterpipelineInfo.pColorBlendState = &colorBlendings;
  drawclusterpipelineInfo.layout = drawclusterPipelineLayout;
  drawclusterpipelineInfo.renderPass = _basePass;
  drawclusterpipelineInfo.subpass = 0;
  drawclusterpipelineInfo.basePipelineHandle = VK_NULL_HANDLE;
  drawclusterpipelineInfo.pDepthStencilState = &depthStencilState;

  if (vkCreateGraphicsPipelines(device.getLogicalDevice(), VK_NULL_HANDLE, 1,
                                &drawclusterpipelineInfo, nullptr,
                                &drawclusterPipeline) != VK_SUCCESS) {
    throw std::runtime_error("failed to create drawcluster graphics pipeline!");
  }

  drawclusterpipelineInfo.pStages = drawclusterShaderStagesAlphaMask;
  if (vkCreateGraphicsPipelines(device.getLogicalDevice(), VK_NULL_HANDLE, 1,
                                &drawclusterpipelineInfo, nullptr,
                                &drawclusterPipelineAlphaMask) != VK_SUCCESS) {
    throw std::runtime_error("failed to create drawcluster graphics pipeline!");
  }

  VkGraphicsPipelineCreateInfo drawclusterBasePipelineInfo{};
  drawclusterBasePipelineInfo.sType =
      VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
  drawclusterBasePipelineInfo.stageCount = 2;
  drawclusterBasePipelineInfo.pStages = drawclusterBasePassStages;
  drawclusterBasePipelineInfo.pVertexInputState = &drawclusterVertexInputInfo;
  drawclusterBasePipelineInfo.pInputAssemblyState = &inputAssembly;
  drawclusterBasePipelineInfo.pViewportState = &viewportState;
  drawclusterBasePipelineInfo.pRasterizationState = &rasterizer;
  drawclusterBasePipelineInfo.pMultisampleState = &multisampling;
  drawclusterBasePipelineInfo.pColorBlendState = &colorBlendings;
  drawclusterBasePipelineInfo.layout = drawclusterBasePipelineLayout;
  drawclusterBasePipelineInfo.renderPass = _basePass;
  drawclusterBasePipelineInfo.subpass = 0;
  drawclusterBasePipelineInfo.basePipelineHandle = VK_NULL_HANDLE;
  drawclusterBasePipelineInfo.pDepthStencilState = &depthStencilState;

  if (vkCreateGraphicsPipelines(device.getLogicalDevice(), VK_NULL_HANDLE, 1,
                                &drawclusterBasePipelineInfo, nullptr,
                                &drawclusterBasePipeline) != VK_SUCCESS) {
    throw std::runtime_error(
        "failed to create drawcluster base graphics pipeline!");
  }

  // Alpha-mask base pass pipeline (GPU indirect, reads material from SSBO)
  {
    VkPipelineShaderStageCreateInfo baseAlphaMaskStages[] = {
        drawclusterVSShaderStageInfo, drawclusterBaseAlphaMaskPSStageInfo};

    VkGraphicsPipelineCreateInfo baseAlphaMaskPipelineInfo = drawclusterBasePipelineInfo;
    baseAlphaMaskPipelineInfo.pStages = baseAlphaMaskStages;
    if (vkCreateGraphicsPipelines(device.getLogicalDevice(), VK_NULL_HANDLE, 1,
                                  &baseAlphaMaskPipelineInfo, nullptr,
                                  &drawclusterBasePipelineAlphaMask) != VK_SUCCESS) {
      throw std::runtime_error(
          "failed to create drawcluster base alpha-mask graphics pipeline!");
    }
  }

  // Forward pass indirect pipeline (GPU indirect, reads material from SSBO, no push constants)
  {
    VkPipelineShaderStageCreateInfo forwardIndirectStages[] = {
        drawclusterVSShaderStageInfo, drawclusterForwardIndirectPSStageInfo};

    VkGraphicsPipelineCreateInfo forwardIndirectPipelineInfo{};
    forwardIndirectPipelineInfo.sType =
        VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    forwardIndirectPipelineInfo.stageCount = 2;
    forwardIndirectPipelineInfo.pStages = forwardIndirectStages;
    forwardIndirectPipelineInfo.pVertexInputState = &drawclusterVertexInputInfo;
    forwardIndirectPipelineInfo.pInputAssemblyState = &inputAssembly;
    forwardIndirectPipelineInfo.pViewportState = &viewportState;
    forwardIndirectPipelineInfo.pRasterizationState = &rasterizer;
    forwardIndirectPipelineInfo.pMultisampleState = &multisampling;
    forwardIndirectPipelineInfo.pColorBlendState = &colorBlendingAlpha;
    forwardIndirectPipelineInfo.layout = drawclusterBasePipelineLayout;
    forwardIndirectPipelineInfo.renderPass = _forwardLightingPass;
    forwardIndirectPipelineInfo.subpass = 0;
    forwardIndirectPipelineInfo.basePipelineHandle = VK_NULL_HANDLE;
    forwardIndirectPipelineInfo.pDepthStencilState = &depthStencilState;

    if (vkCreateGraphicsPipelines(device.getLogicalDevice(), VK_NULL_HANDLE, 1,
                                  &forwardIndirectPipelineInfo, nullptr,
                                  &drawclusterForwardPipelineIndirect) != VK_SUCCESS) {
      throw std::runtime_error(
          "failed to create drawcluster forward indirect pipeline!");
    }
  }

  VkGraphicsPipelineCreateInfo drawclusterForwardPipelineInfo{};
  drawclusterForwardPipelineInfo.sType =
      VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
  drawclusterForwardPipelineInfo.stageCount = 2;
  drawclusterForwardPipelineInfo.pStages = drawclusterForwardStages;
  drawclusterForwardPipelineInfo.pVertexInputState =
      &drawclusterVertexInputInfo;
  drawclusterForwardPipelineInfo.pInputAssemblyState = &inputAssembly;
  drawclusterForwardPipelineInfo.pViewportState = &viewportState;
  drawclusterForwardPipelineInfo.pRasterizationState = &rasterizer;
  drawclusterForwardPipelineInfo.pMultisampleState = &multisampling;
  drawclusterForwardPipelineInfo.pColorBlendState = &colorBlendingAlpha;
  drawclusterForwardPipelineInfo.layout = drawclusterPipelineLayout;
  drawclusterForwardPipelineInfo.renderPass = _forwardLightingPass;
  drawclusterForwardPipelineInfo.subpass = 0;
  drawclusterForwardPipelineInfo.basePipelineHandle = VK_NULL_HANDLE;
  drawclusterForwardPipelineInfo.pDepthStencilState = &depthStencilState;

  if (vkCreateGraphicsPipelines(device.getLogicalDevice(), VK_NULL_HANDLE, 1,
                                &drawclusterForwardPipelineInfo, nullptr,
                                &drawclusterForwardPipeline) != VK_SUCCESS) {
    throw std::runtime_error(
        "failed to create drawcluster base graphics pipeline!");
  }

  VkGraphicsPipelineCreateInfo deferredLightingPipelineInfo{};
  deferredLightingPipelineInfo.sType =
      VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
  deferredLightingPipelineInfo.stageCount = 2;
  deferredLightingPipelineInfo.pStages = deferredLightingPassStages;
  deferredLightingPipelineInfo.pVertexInputState =
      &emptyVertexInputInfo; 
  deferredLightingPipelineInfo.pInputAssemblyState = &inputAssembly;
  deferredLightingPipelineInfo.pViewportState = &viewportState;
  deferredLightingPipelineInfo.pRasterizationState = &rasterizerBackFace;
  deferredLightingPipelineInfo.pMultisampleState = &multisampling;
  deferredLightingPipelineInfo.pColorBlendState = &colorBlending;
  deferredLightingPipelineInfo.layout = deferredLightingPipelineLayout;
  deferredLightingPipelineInfo.renderPass = _deferredLightingPass;
  deferredLightingPipelineInfo.subpass = 0;
  deferredLightingPipelineInfo.basePipelineHandle = VK_NULL_HANDLE;
  deferredLightingPipelineInfo.pDepthStencilState = &depthStencilStateDisable;

  if (vkCreateGraphicsPipelines(device.getLogicalDevice(), VK_NULL_HANDLE, 1,
                                &deferredLightingPipelineInfo, nullptr,
                                &deferredLightingPipeline) != VK_SUCCESS) {
    throw std::runtime_error(
        "failed to create drawcluster base graphics pipeline!");
  }

  VkGraphicsPipelineCreateInfo deferredLightingPipelineInfo_clusterlighting{};
  deferredLightingPipelineInfo_clusterlighting.sType =
      VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
  deferredLightingPipelineInfo_clusterlighting.stageCount = 2;
  deferredLightingPipelineInfo_clusterlighting.pStages =
      deferredLightingPassStages_clusterlighting;
  deferredLightingPipelineInfo_clusterlighting.pVertexInputState =
      &emptyVertexInputInfo; 
  deferredLightingPipelineInfo_clusterlighting.pInputAssemblyState =
      &inputAssembly;
  deferredLightingPipelineInfo_clusterlighting.pViewportState = &viewportState;
  deferredLightingPipelineInfo_clusterlighting.pRasterizationState =
      &rasterizerBackFace;
  deferredLightingPipelineInfo_clusterlighting.pMultisampleState =
      &multisampling;
  deferredLightingPipelineInfo_clusterlighting.pColorBlendState =
      &colorBlending;
  deferredLightingPipelineInfo_clusterlighting.layout =
      deferredLightingPipelineLayout;
  deferredLightingPipelineInfo_clusterlighting.renderPass =
      _deferredLightingPass;
  deferredLightingPipelineInfo_clusterlighting.subpass = 0;
  deferredLightingPipelineInfo_clusterlighting.basePipelineHandle =
      VK_NULL_HANDLE;
  deferredLightingPipelineInfo_clusterlighting.pDepthStencilState =
      &depthStencilStateDisable;

  if (vkCreateGraphicsPipelines(
          device.getLogicalDevice(), VK_NULL_HANDLE, 1,
          &deferredLightingPipelineInfo_clusterlighting, nullptr,
          &deferredLightingPipeline_clusterlighting) != VK_SUCCESS) {
    throw std::runtime_error(
        "failed to create drawcluster base graphics pipeline!");
  }


  vkDestroyShaderModule(device.getLogicalDevice(), drawclusterVSShaderModule,
                        nullptr);
  vkDestroyShaderModule(device.getLogicalDevice(), drawclusterPSShaderModule,
                        nullptr);

  // TODO : Destroy shader modules
}

enum MTLPixelFormat {
  MTLPixelFormatInvalid = 0,

  /* Normal 8 bit formats */

  MTLPixelFormatA8Unorm = 1,

  MTLPixelFormatR8Unorm = 10,
  MTLPixelFormatR8Unorm_sRGB = 11,
  MTLPixelFormatR8Snorm = 12,
  MTLPixelFormatR8Uint = 13,
  MTLPixelFormatR8Sint = 14,

  /* Normal 16 bit formats */

  MTLPixelFormatR16Unorm = 20,
  MTLPixelFormatR16Snorm = 22,
  MTLPixelFormatR16Uint = 23,
  MTLPixelFormatR16Sint = 24,
  MTLPixelFormatR16Float = 25,

  MTLPixelFormatRG8Unorm = 30,
  MTLPixelFormatRG8Unorm_sRGB = 31,
  MTLPixelFormatRG8Snorm = 32,
  MTLPixelFormatRG8Uint = 33,
  MTLPixelFormatRG8Sint = 34,

  /* Packed 16 bit formats */

  MTLPixelFormatB5G6R5Unorm = 40,
  MTLPixelFormatA1BGR5Unorm = 41,
  MTLPixelFormatABGR4Unorm = 42,
  MTLPixelFormatBGR5A1Unorm = 43,

  /* Normal 32 bit formats */

  MTLPixelFormatR32Uint = 53,
  MTLPixelFormatR32Sint = 54,
  MTLPixelFormatR32Float = 55,

  MTLPixelFormatRG16Unorm = 60,
  MTLPixelFormatRG16Snorm = 62,
  MTLPixelFormatRG16Uint = 63,
  MTLPixelFormatRG16Sint = 64,
  MTLPixelFormatRG16Float = 65,

  MTLPixelFormatRGBA8Unorm = 70,
  MTLPixelFormatRGBA8Unorm_sRGB = 71,
  MTLPixelFormatRGBA8Snorm = 72,
  MTLPixelFormatRGBA8Uint = 73,
  MTLPixelFormatRGBA8Sint = 74,

  MTLPixelFormatBGRA8Unorm = 80,
  MTLPixelFormatBGRA8Unorm_sRGB = 81,

  /* Packed 32 bit formats */

  MTLPixelFormatRGB10A2Unorm = 90,
  MTLPixelFormatRGB10A2Uint = 91,

  MTLPixelFormatRG11B10Float = 92,
  MTLPixelFormatRGB9E5Float = 93,

  MTLPixelFormatBGR10A2Unorm = 94,

  MTLPixelFormatBGR10_XR = 554,
  MTLPixelFormatBGR10_XR_sRGB = 555,

  /* Normal 64 bit formats */

  MTLPixelFormatRG32Uint = 103,
  MTLPixelFormatRG32Sint = 104,
  MTLPixelFormatRG32Float = 105,

  MTLPixelFormatRGBA16Unorm = 110,
  MTLPixelFormatRGBA16Snorm = 112,
  MTLPixelFormatRGBA16Uint = 113,
  MTLPixelFormatRGBA16Sint = 114,
  MTLPixelFormatRGBA16Float = 115,

  MTLPixelFormatBGRA10_XR = 552,
  MTLPixelFormatBGRA10_XR_sRGB = 553,

  /* Normal 128 bit formats */

  MTLPixelFormatRGBA32Uint = 123,
  MTLPixelFormatRGBA32Sint = 124,
  MTLPixelFormatRGBA32Float = 125,

  /* Compressed formats. */

  /* S3TC/DXT */
  MTLPixelFormatBC1_RGBA = 130,
  MTLPixelFormatBC1_RGBA_sRGB = 131,
  MTLPixelFormatBC2_RGBA = 132,
  MTLPixelFormatBC2_RGBA_sRGB = 133,
  MTLPixelFormatBC3_RGBA = 134,
  MTLPixelFormatBC3_RGBA_sRGB = 135,

  /* RGTC */
  MTLPixelFormatBC4_RUnorm = 140,
  MTLPixelFormatBC4_RSnorm = 141,
  MTLPixelFormatBC5_RGUnorm = 142,
  MTLPixelFormatBC5_RGSnorm = 143,

  /* BPTC */
  MTLPixelFormatBC6H_RGBFloat = 150,
  MTLPixelFormatBC6H_RGBUfloat = 151,
  MTLPixelFormatBC7_RGBAUnorm = 152,
  MTLPixelFormatBC7_RGBAUnorm_sRGB = 153,

  /* PVRTC */
  MTLPixelFormatPVRTC_RGB_2BPP = 160,
  MTLPixelFormatPVRTC_RGB_2BPP_sRGB = 161,
  MTLPixelFormatPVRTC_RGB_4BPP = 162,
  MTLPixelFormatPVRTC_RGB_4BPP_sRGB = 163,
  MTLPixelFormatPVRTC_RGBA_2BPP = 164,
  MTLPixelFormatPVRTC_RGBA_2BPP_sRGB = 165,
  MTLPixelFormatPVRTC_RGBA_4BPP = 166,
  MTLPixelFormatPVRTC_RGBA_4BPP_sRGB = 167,

  /* ETC2 */
  MTLPixelFormatEAC_R11Unorm = 170,
  MTLPixelFormatEAC_R11Snorm = 172,
  MTLPixelFormatEAC_RG11Unorm = 174,
  MTLPixelFormatEAC_RG11Snorm = 176,
  MTLPixelFormatEAC_RGBA8 = 178,
  MTLPixelFormatEAC_RGBA8_sRGB = 179,

  MTLPixelFormatETC2_RGB8 = 180,
  MTLPixelFormatETC2_RGB8_sRGB = 181,
  MTLPixelFormatETC2_RGB8A1 = 182,
  MTLPixelFormatETC2_RGB8A1_sRGB = 183,

  /* ASTC */
  MTLPixelFormatASTC_4x4_sRGB = 186,
  MTLPixelFormatASTC_5x4_sRGB = 187,
  MTLPixelFormatASTC_5x5_sRGB = 188,
  MTLPixelFormatASTC_6x5_sRGB = 189,
  MTLPixelFormatASTC_6x6_sRGB = 190,
  MTLPixelFormatASTC_8x5_sRGB = 192,
  MTLPixelFormatASTC_8x6_sRGB = 193,
  MTLPixelFormatASTC_8x8_sRGB = 194,
  MTLPixelFormatASTC_10x5_sRGB = 195,
  MTLPixelFormatASTC_10x6_sRGB = 196,
  MTLPixelFormatASTC_10x8_sRGB = 197,
  MTLPixelFormatASTC_10x10_sRGB = 198,
  MTLPixelFormatASTC_12x10_sRGB = 199,
  MTLPixelFormatASTC_12x12_sRGB = 200,

  MTLPixelFormatASTC_4x4_LDR = 204,
  MTLPixelFormatASTC_5x4_LDR = 205,
  MTLPixelFormatASTC_5x5_LDR = 206,
  MTLPixelFormatASTC_6x5_LDR = 207,
  MTLPixelFormatASTC_6x6_LDR = 208,
  MTLPixelFormatASTC_8x5_LDR = 210,
  MTLPixelFormatASTC_8x6_LDR = 211,
  MTLPixelFormatASTC_8x8_LDR = 212,
  MTLPixelFormatASTC_10x5_LDR = 213,
  MTLPixelFormatASTC_10x6_LDR = 214,
  MTLPixelFormatASTC_10x8_LDR = 215,
  MTLPixelFormatASTC_10x10_LDR = 216,
  MTLPixelFormatASTC_12x10_LDR = 217,
  MTLPixelFormatASTC_12x12_LDR = 218,

  // ASTC HDR (High Dynamic Range) Formats
  MTLPixelFormatASTC_4x4_HDR = 222,
  MTLPixelFormatASTC_5x4_HDR = 223,
  MTLPixelFormatASTC_5x5_HDR = 224,
  MTLPixelFormatASTC_6x5_HDR = 225,
  MTLPixelFormatASTC_6x6_HDR = 226,
  MTLPixelFormatASTC_8x5_HDR = 228,
  MTLPixelFormatASTC_8x6_HDR = 229,
  MTLPixelFormatASTC_8x8_HDR = 230,
  MTLPixelFormatASTC_10x5_HDR = 231,
  MTLPixelFormatASTC_10x6_HDR = 232,
  MTLPixelFormatASTC_10x8_HDR = 233,
  MTLPixelFormatASTC_10x10_HDR = 234,
  MTLPixelFormatASTC_12x10_HDR = 235,
  MTLPixelFormatASTC_12x12_HDR = 236,
  /*!
   @constant MTLPixelFormatGBGR422
   @abstract A pixel format where the red and green channels are subsampled
   horizontally.  Two pixels are stored in 32 bits, with shared red and blue
   values, and unique green values.
   @discussion This format is equivalent to YUY2, YUYV, yuvs, or
   GL_RGB_422_APPLE/GL_UNSIGNED_SHORT_8_8_REV_APPLE.   The component order, from
   lowest addressed byte to highest, is Y0, Cb, Y1, Cr.  There is no implicit
   colorspace conversion from YUV to RGB, the shader will receive (Cr, Y, Cb,
   1).  422 textures must have a width that is a multiple of 2, and can only be
   used for 2D non-mipmap textures.  When sampling, ClampToEdge is the only
   usable wrap mode.
   */
  MTLPixelFormatGBGR422 = 240,

  /*!
   @constant MTLPixelFormatBGRG422
   @abstract A pixel format where the red and green channels are subsampled
   horizontally.  Two pixels are stored in 32 bits, with shared red and blue
   values, and unique green values.
   @discussion This format is equivalent to UYVY, 2vuy, or
   GL_RGB_422_APPLE/GL_UNSIGNED_SHORT_8_8_APPLE. The component order, from
   lowest addressed byte to highest, is Cb, Y0, Cr, Y1.  There is no implicit
   colorspace conversion from YUV to RGB, the shader will receive (Cr, Y, Cb,
   1).  422 textures must have a width that is a multiple of 2, and can only be
   used for 2D non-mipmap textures.  When sampling, ClampToEdge is the only
   usable wrap mode.
   */
  MTLPixelFormatBGRG422 = 241,

  /* Depth */

  MTLPixelFormatDepth16Unorm = 250,
  MTLPixelFormatDepth32Float = 252,

  /* Stencil */

  MTLPixelFormatStencil8 = 253,

  /* Depth Stencil */

  MTLPixelFormatDepth24Unorm_Stencil8 = 255,
  MTLPixelFormatDepth32Float_Stencil8 = 260,

  MTLPixelFormatX32_Stencil8 = 261,
  MTLPixelFormatX24_Stencil8 = 262,

};

VkFormat mapFromApple(MTLPixelFormat appleformat) {
  switch (appleformat) {
  case MTLPixelFormatBC3_RGBA_sRGB:
    return VK_FORMAT_BC3_SRGB_BLOCK;
  case MTLPixelFormatBC5_RGUnorm:
    return VK_FORMAT_BC5_UNORM_BLOCK;
  case MTLPixelFormatBC1_RGBA_sRGB:
    return VK_FORMAT_BC1_RGBA_SRGB_BLOCK;
  default:
    spdlog::error("unsupported appleformat {}", appleformat);
  }
  return VK_FORMAT_UNDEFINED;
}

void GpuScene::init_deferredlighting_descriptors() {
  VkDescriptorSetLayoutBinding albedoBinding = {};
  albedoBinding.binding = 0;
  albedoBinding.descriptorCount = 1;
  albedoBinding.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  albedoBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

  VkDescriptorSetLayoutBinding normalBinding = {};
  normalBinding.binding = 1;
  normalBinding.descriptorCount = 1;
  normalBinding.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  normalBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

  VkDescriptorSetLayoutBinding emessiveBinding = {};
  emessiveBinding.binding = 2;
  emessiveBinding.descriptorCount = 1;
  emessiveBinding.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  emessiveBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

  VkDescriptorSetLayoutBinding f0RoughnessBinding = {};
  f0RoughnessBinding.binding = 3;
  f0RoughnessBinding.descriptorCount = 1;
  f0RoughnessBinding.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  f0RoughnessBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

  VkDescriptorSetLayoutBinding depthBinding = {};
  depthBinding.binding = 4;
  depthBinding.descriptorCount = 1;
  depthBinding.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  depthBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

  VkDescriptorSetLayoutBinding nearestClampSamplerBinding = {};
  nearestClampSamplerBinding.binding = 5;
  nearestClampSamplerBinding.descriptorCount = 1;
  nearestClampSamplerBinding.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
  nearestClampSamplerBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

  VkDescriptorSetLayoutBinding shadowMapsBinding = {};
  shadowMapsBinding.binding = 6;
  shadowMapsBinding.descriptorCount = 1;
  shadowMapsBinding.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  shadowMapsBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

  VkDescriptorSetLayoutBinding shadowMapsSamplerBinding = {};
  shadowMapsSamplerBinding.binding = 7;
  shadowMapsSamplerBinding.descriptorCount = 1;
  shadowMapsSamplerBinding.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
  shadowMapsSamplerBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

  VkDescriptorSetLayoutBinding pointLightCullingDataBinding = {};
  pointLightCullingDataBinding.binding = 8;
  pointLightCullingDataBinding.descriptorCount = 1;
  pointLightCullingDataBinding.descriptorType =
      VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  pointLightCullingDataBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

  VkDescriptorSetLayoutBinding lightIndicesBinding = {};
  lightIndicesBinding.binding = 9;
  lightIndicesBinding.descriptorCount = 1;
  lightIndicesBinding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  lightIndicesBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

  VkDescriptorSetLayoutBinding aoBinding = {};
  aoBinding.binding = 10;
  aoBinding.descriptorCount = 1;
  aoBinding.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  aoBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

  // Spot light bindings for the deferred lighting fullscreen pass
  // (see deferredlighting.hlsl).
  VkDescriptorSetLayoutBinding spotLightCullingDataBinding = {};
  spotLightCullingDataBinding.binding = 11;
  spotLightCullingDataBinding.descriptorCount = 1;
  spotLightCullingDataBinding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  spotLightCullingDataBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

  VkDescriptorSetLayoutBinding spotLightIndicesBindingDef = {};
  spotLightIndicesBindingDef.binding = 12;
  spotLightIndicesBindingDef.descriptorCount = 1;
  spotLightIndicesBindingDef.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  spotLightIndicesBindingDef.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

  // Spot shadow bindings.
  VkDescriptorSetLayoutBinding spotShadowMapsBinding = {};
  spotShadowMapsBinding.binding = 13;
  spotShadowMapsBinding.descriptorCount = 1;
  spotShadowMapsBinding.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  spotShadowMapsBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

  VkDescriptorSetLayoutBinding spotShadowSamplerBinding = {};
  spotShadowSamplerBinding.binding = 14;
  spotShadowSamplerBinding.descriptorCount = 1;
  spotShadowSamplerBinding.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
  spotShadowSamplerBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

  VkDescriptorSetLayoutBinding spotViewProjBinding = {};
  spotViewProjBinding.binding = 15;
  spotViewProjBinding.descriptorCount = 1;
  spotViewProjBinding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  spotViewProjBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

  // Scatter volume bindings (16 = Texture3D accumVolume, 17 = linear sampler).
  VkDescriptorSetLayoutBinding scatterVolumeBinding = {};
  scatterVolumeBinding.binding = 16;
  scatterVolumeBinding.descriptorCount = 1;
  scatterVolumeBinding.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  scatterVolumeBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

  VkDescriptorSetLayoutBinding linearSamplerBinding = {};
  linearSamplerBinding.binding = 17;
  linearSamplerBinding.descriptorCount = 1;
  linearSamplerBinding.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
  linearSamplerBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

  VkDescriptorSetLayoutBinding bindings[] = {albedoBinding,
                                             normalBinding,
                                             emessiveBinding,
                                             f0RoughnessBinding,
                                             depthBinding,
                                             nearestClampSamplerBinding,
                                             shadowMapsBinding,
                                             shadowMapsSamplerBinding,
                                             pointLightCullingDataBinding,
                                             lightIndicesBinding,
                                             aoBinding,
                                             spotLightCullingDataBinding,
                                             spotLightIndicesBindingDef,
                                             spotShadowMapsBinding,
                                             spotShadowSamplerBinding,
                                             spotViewProjBinding,
                                             scatterVolumeBinding,
                                             linearSamplerBinding};

  VkDescriptorSetLayoutCreateInfo setinfo = {};
  setinfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  setinfo.pNext = nullptr;

  // we are going to have 1 binding
  setinfo.bindingCount = sizeof(bindings) / sizeof(bindings[0]);
  // no flags
  setinfo.flags = 0;
  // point to the camera buffer binding
  setinfo.pBindings = bindings;

  vkCreateDescriptorSetLayout(device.getLogicalDevice(), &setinfo, nullptr,
                              &deferredLightingSetLayout);

  // other code ....
  // create a descriptor pool that will hold 10 uniform buffers
  std::vector<VkDescriptorPoolSize> sizes = {
      {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 14 * framesInFlight}, // 0-4,6,10,13,16
      {VK_DESCRIPTOR_TYPE_SAMPLER, 4 * framesInFlight},        // 5,7,14,17
      {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 6 * framesInFlight}, // 8,9,11,12,15 + headroom
  };

  VkDescriptorPoolCreateInfo pool_info = {};
  pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  pool_info.flags = 0;
  pool_info.maxSets = framesInFlight + 1;
  pool_info.poolSizeCount = (uint32_t)sizes.size();
  pool_info.pPoolSizes = sizes.data();

  vkCreateDescriptorPool(device.getLogicalDevice(), &pool_info, nullptr,
                         &deferredLightingDescriptorPool);

  deferredLightingDescriptorSet.resize(framesInFlight);
  std::vector<VkDescriptorSetLayout> deferredLayouts(framesInFlight, deferredLightingSetLayout);

  VkDescriptorSetAllocateInfo allocInfo = {};
  allocInfo.pNext = nullptr;
  allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  // using the pool we just set
  allocInfo.descriptorPool = deferredLightingDescriptorPool;
  allocInfo.descriptorSetCount = framesInFlight;
  allocInfo.pSetLayouts = deferredLayouts.data();

  vkAllocateDescriptorSets(device.getLogicalDevice(), &allocInfo,
                           deferredLightingDescriptorSet.data());

  for (uint32_t f = 0; f < framesInFlight; ++f) {
  VkDescriptorImageInfo samplerinfo;
  samplerinfo.sampler = nearestClampSampler;
  VkWriteDescriptorSet setSampler = {};
  setSampler.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  setSampler.dstBinding = 5;
  setSampler.pNext = nullptr;
  setSampler.dstSet = deferredLightingDescriptorSet[f];
  setSampler.dstArrayElement = 0;
  setSampler.descriptorCount = 1;
  setSampler.pImageInfo = &samplerinfo;
  setSampler.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;

  std::array<VkDescriptorImageInfo, 4> imageinfo{};
  // imageinfo.resize(textures.size());
  for (int texturei = 0; texturei < 4; ++texturei) {
    imageinfo[texturei].imageView = _gbuffersView[texturei][f];
    imageinfo[texturei].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    // imageinfo[texturei].sampler = textureSampler;
  }

  VkWriteDescriptorSet setWriteTexture[4] = {};
  for (int i = 0; i < 4; i++) {
    setWriteTexture[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    setWriteTexture[i].pNext = nullptr;

    setWriteTexture[i].dstBinding = i;
    // of the global descriptor
    setWriteTexture[i].dstSet = deferredLightingDescriptorSet[f];
    setWriteTexture[i].dstArrayElement = 0;

    setWriteTexture[i].descriptorCount = 1;
    setWriteTexture[i].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    setWriteTexture[i].pImageInfo = &imageinfo[i];
  }
  // need transform layout?--yes!
  VkDescriptorImageInfo depthImageInfo{};
  depthImageInfo.imageView = device.getWindowDepthOnlyImageView(f);
  depthImageInfo.imageLayout =
      VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL;
  VkWriteDescriptorSet setWriteDepth;
  setWriteDepth.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  setWriteDepth.pNext = nullptr;
  setWriteDepth.dstBinding = 4;
  setWriteDepth.dstSet = deferredLightingDescriptorSet[f];
  setWriteDepth.dstArrayElement = 0;
  setWriteDepth.descriptorCount = 1;
  setWriteDepth.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  setWriteDepth.pImageInfo = &depthImageInfo;

  std::array<VkWriteDescriptorSet, 6> writes = {setWriteTexture[0],
                                                setWriteTexture[1],
                                                setWriteTexture[2],
                                                setWriteTexture[3],
                                                setWriteDepth,
                                                setSampler};

  vkUpdateDescriptorSets(device.getLogicalDevice(), writes.size(),
                         writes.data(), 0, nullptr);
  } // end per-frame loop

  
}

void GpuScene::init_drawparams_descriptors() {
  VkDescriptorSetLayoutBinding drawParamsBinding = {};
  drawParamsBinding.binding = 0;
  drawParamsBinding.descriptorCount = 1;
  drawParamsBinding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  drawParamsBinding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

  VkDescriptorSetLayoutBinding cullParamsBinding = {};
  cullParamsBinding.binding = 1;
  cullParamsBinding.descriptorCount = 1;
  cullParamsBinding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
  cullParamsBinding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

  VkDescriptorSetLayoutBinding meshChunksBinding = {};
  meshChunksBinding.binding = 2;
  meshChunksBinding.descriptorCount = 1;
  meshChunksBinding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  meshChunksBinding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

  VkDescriptorSetLayoutBinding writeIndexBinding = {};
  writeIndexBinding.binding = 3;
  writeIndexBinding.descriptorCount = 1;
  writeIndexBinding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  writeIndexBinding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

  VkDescriptorSetLayoutBinding chunkIndicesBinding = {};
  chunkIndicesBinding.binding = 4;
  chunkIndicesBinding.descriptorCount = 1;
  chunkIndicesBinding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  chunkIndicesBinding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

  // Hi-Z texture (Stage 3)
  VkDescriptorSetLayoutBinding hizTextureBinding = {};
  hizTextureBinding.binding = 6;
  hizTextureBinding.descriptorCount = 1;
  hizTextureBinding.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  hizTextureBinding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

  VkDescriptorSetLayoutBinding hizSamplerBinding = {};
  hizSamplerBinding.binding = 7;
  hizSamplerBinding.descriptorCount = 1;
  hizSamplerBinding.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
  hizSamplerBinding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

  VkDescriptorSetLayoutBinding bindings[] = {
      drawParamsBinding, cullParamsBinding,   meshChunksBinding,
      writeIndexBinding, chunkIndicesBinding,
      hizTextureBinding, hizSamplerBinding};

  constexpr int bindingcount = sizeof(bindings) / sizeof(bindings[0]);

  VkDescriptorSetLayoutCreateInfo setinfo = {};
  setinfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  // setinfo.pNext = &flag_info;
  setinfo.pNext = nullptr;

  setinfo.bindingCount = bindingcount;
  setinfo.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
  // point to the camera buffer binding
  setinfo.pBindings = bindings;

  vkCreateDescriptorSetLayout(device.getLogicalDevice(), &setinfo, nullptr,
                              &gpuCullSetLayout);

  std::vector<VkDescriptorPoolSize> sizes = {
      {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 10 * framesInFlight},
      {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 10 * framesInFlight},
      {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 10 * framesInFlight},
      {VK_DESCRIPTOR_TYPE_SAMPLER, 10 * framesInFlight},
  };

  VkDescriptorPoolCreateInfo pool_info = {};
  pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT_EXT;
  pool_info.maxSets = 10 * framesInFlight;
  pool_info.poolSizeCount = (uint32_t)sizes.size();
  pool_info.pPoolSizes = sizes.data();

  vkCreateDescriptorPool(device.getLogicalDevice(), &pool_info, nullptr,
                         &gpuCullDescriptorPool);

  gpuCullDescriptorSets.resize(framesInFlight);
  std::vector<VkDescriptorSetLayout> layouts(framesInFlight, gpuCullSetLayout);

  VkDescriptorSetAllocateInfo allocInfo = {};
  allocInfo.pNext = nullptr;
  allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  allocInfo.descriptorPool = gpuCullDescriptorPool;
  allocInfo.descriptorSetCount = framesInFlight;
  allocInfo.pSetLayouts = layouts.data();

  vkAllocateDescriptorSets(device.getLogicalDevice(), &allocInfo,
                           gpuCullDescriptorSets.data());

  for (uint32_t f = 0; f < framesInFlight; ++f) {
    VkDescriptorBufferInfo drawParamsBufferInfo;
    drawParamsBufferInfo.buffer = drawParamsBuffers[f];
    drawParamsBufferInfo.offset = 0;
    drawParamsBufferInfo.range =
        applMesh->_chunkCount * sizeof(VkDrawIndexedIndirectCommand);

    VkWriteDescriptorSet drawParamsWrite = {};
    drawParamsWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    drawParamsWrite.pNext = nullptr;
    drawParamsWrite.dstBinding = 0;
    drawParamsWrite.dstSet = gpuCullDescriptorSets[f];
    drawParamsWrite.descriptorCount = 1;
    drawParamsWrite.dstArrayElement = 0;
    drawParamsWrite.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    drawParamsWrite.pBufferInfo = &drawParamsBufferInfo;

    VkDescriptorBufferInfo cullParamsBufferInfo;
    cullParamsBufferInfo.buffer = cullParamsBuffers[f];
    cullParamsBufferInfo.offset = 0;
    cullParamsBufferInfo.range = sizeof(GPUCullParams);

    VkWriteDescriptorSet cullParamsWrite = {};
    cullParamsWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    cullParamsWrite.pNext = nullptr;
    cullParamsWrite.dstBinding = 1;
    cullParamsWrite.dstSet = gpuCullDescriptorSets[f];
    cullParamsWrite.descriptorCount = 1;
    cullParamsWrite.dstArrayElement = 0;
    cullParamsWrite.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    cullParamsWrite.pBufferInfo = &cullParamsBufferInfo;

    VkDescriptorBufferInfo meshChunksBufferInfo;
    meshChunksBufferInfo.buffer = meshChunksBuffer;
    meshChunksBufferInfo.offset = 0;
    meshChunksBufferInfo.range = sizeof(AAPLMeshChunk) * applMesh->_chunkCount;

    VkWriteDescriptorSet meshChunksBufferWrite = {};
    meshChunksBufferWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    meshChunksBufferWrite.pNext = nullptr;
    meshChunksBufferWrite.dstBinding = 2;
    meshChunksBufferWrite.dstSet = gpuCullDescriptorSets[f];
    meshChunksBufferWrite.descriptorCount = 1;
    meshChunksBufferWrite.dstArrayElement = 0;
    meshChunksBufferWrite.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    meshChunksBufferWrite.pBufferInfo = &meshChunksBufferInfo;

    VkDescriptorBufferInfo writeIndexBufferDescInfo;
    writeIndexBufferDescInfo.buffer = writeIndexBuffers[f];
    writeIndexBufferDescInfo.offset = 0;
    writeIndexBufferDescInfo.range = 3 * sizeof(uint32_t);

    VkWriteDescriptorSet writeIndexBufferWrite = {};
    writeIndexBufferWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writeIndexBufferWrite.pNext = nullptr;
    writeIndexBufferWrite.dstBinding = 3;
    writeIndexBufferWrite.dstSet = gpuCullDescriptorSets[f];
    writeIndexBufferWrite.descriptorCount = 1;
    writeIndexBufferWrite.dstArrayElement = 0;
    writeIndexBufferWrite.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writeIndexBufferWrite.pBufferInfo = &writeIndexBufferDescInfo;

    VkDescriptorBufferInfo chunkIndicesBufferInfo;
    chunkIndicesBufferInfo.buffer = chunkIndicesBuffers[f];
    chunkIndicesBufferInfo.offset = 0;
    chunkIndicesBufferInfo.range = sizeof(uint32_t) * applMesh->_chunkCount * 2 *SHADOW_CASCADE_COUNT;

    VkWriteDescriptorSet chunkIndicesBufferWrite = {};
    chunkIndicesBufferWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    chunkIndicesBufferWrite.pNext = nullptr;
    chunkIndicesBufferWrite.dstBinding = 4;
    chunkIndicesBufferWrite.dstSet = gpuCullDescriptorSets[f];
    chunkIndicesBufferWrite.descriptorCount = 1;
    chunkIndicesBufferWrite.dstArrayElement = 0;
    chunkIndicesBufferWrite.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    chunkIndicesBufferWrite.pBufferInfo = &chunkIndicesBufferInfo;

    // Hi-Z descriptor writes (will be updated later when Hi-Z resources are created)
    VkDescriptorImageInfo hizImageInfo{};
    hizImageInfo.imageView = _hizTextureView; // may be VK_NULL_HANDLE initially
    hizImageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkWriteDescriptorSet hizTextureWrite = {};
    hizTextureWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    hizTextureWrite.pNext = nullptr;
    hizTextureWrite.dstBinding = 6;
    hizTextureWrite.dstSet = gpuCullDescriptorSets[f];
    hizTextureWrite.descriptorCount = 1;
    hizTextureWrite.dstArrayElement = 0;
    hizTextureWrite.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    hizTextureWrite.pImageInfo = &hizImageInfo;

    VkDescriptorImageInfo hizSamplerInfo{};
    hizSamplerInfo.sampler = _hizSampler; // may be VK_NULL_HANDLE initially

    VkWriteDescriptorSet hizSamplerWrite = {};
    hizSamplerWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    hizSamplerWrite.pNext = nullptr;
    hizSamplerWrite.dstBinding = 7;
    hizSamplerWrite.dstSet = gpuCullDescriptorSets[f];
    hizSamplerWrite.descriptorCount = 1;
    hizSamplerWrite.dstArrayElement = 0;
    hizSamplerWrite.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    hizSamplerWrite.pImageInfo = &hizSamplerInfo;

    std::vector<VkWriteDescriptorSet> writes = {
        drawParamsWrite,       cullParamsWrite,
        meshChunksBufferWrite, writeIndexBufferWrite,
        chunkIndicesBufferWrite};

    // Only write Hi-Z descriptors if resources are ready
    if (_hizTextureView != VK_NULL_HANDLE && _hizSampler != VK_NULL_HANDLE) {
      writes.push_back(hizTextureWrite);
      writes.push_back(hizSamplerWrite);
    }

    vkUpdateDescriptorSets(device.getLogicalDevice(), writes.size(),
                           writes.data(), 0, nullptr);
  }
}

void GpuScene::init_appl_descriptors() {
  // information about the binding.
  VkDescriptorSetLayoutBinding uniformBufferBinding = {};
  uniformBufferBinding.binding = 0;
  uniformBufferBinding.descriptorCount = 1;
  // it's a uniform buffer binding
  uniformBufferBinding.descriptorType =
      VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;

  // we use it from the vertex shader
  uniformBufferBinding.stageFlags =
      VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

  VkDescriptorSetLayoutBinding matBinding = {};
  matBinding.binding = 0;
  matBinding.descriptorCount = 1;
  // it's a uniform buffer binding
  matBinding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  // we use it from the vertex shader
  matBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

  VkDescriptorSetLayoutBinding samplerBinding = {};
  samplerBinding.binding = 1;
  samplerBinding.descriptorCount = 1;
  samplerBinding.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
  samplerBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

  VkDescriptorSetLayoutBinding textureBinding = {};
  textureBinding.binding = 2;
  textureBinding.descriptorCount = textures.size();
  textureBinding.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  textureBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

  VkDescriptorSetLayoutBinding meshChunksBinding = {};
  meshChunksBinding.binding = 3;
  meshChunksBinding.descriptorCount = 1;
  // it's a uniform buffer binding
  meshChunksBinding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  meshChunksBinding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

  VkDescriptorSetLayoutBinding chunkIndexBinding = {};
  chunkIndexBinding.binding = 4;
  chunkIndexBinding.descriptorCount = 1;
  // it's a uniform buffer binding
  chunkIndexBinding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  // we use it from the vertex shader
  chunkIndexBinding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

  VkDescriptorSetLayoutBinding bindings[] = {
      matBinding,        samplerBinding,
      textureBinding,       meshChunksBinding, chunkIndexBinding};

  constexpr int bindingcount = sizeof(bindings) / sizeof(bindings[0]);

  std::array<VkDescriptorBindingFlags, bindingcount> bindingFlags = {
      0, 0, VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT_EXT, 0, 0};

  // VkDescriptorBindingFlags flag =
  // VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT_EXT |
  // VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT_EXT;

  VkDescriptorSetLayoutBindingFlagsCreateInfo flag_info = {
      .sType =
          VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
  flag_info.bindingCount = bindingcount;
  flag_info.pBindingFlags = bindingFlags.data();

  VkDescriptorSetLayoutCreateInfo setinfo = {};
  setinfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  // setinfo.pNext = &flag_info;
  setinfo.pNext = nullptr;

  setinfo.bindingCount = bindingcount;
  setinfo.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
  // point to the camera buffer binding
  setinfo.pBindings = bindings;

  vkCreateDescriptorSetLayout(device.getLogicalDevice(), &setinfo, nullptr,
                              &applSetLayout);

  std::vector<VkDescriptorPoolSize> sizes = {
      {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 10 * framesInFlight},
      {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 10 * framesInFlight},
      {VK_DESCRIPTOR_TYPE_SAMPLER, 10 * framesInFlight},
      {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 4096}};

  VkDescriptorPoolCreateInfo pool_info = {};
  pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT_EXT;
  pool_info.maxSets = 10 * framesInFlight;
  pool_info.poolSizeCount = (uint32_t)sizes.size();
  pool_info.pPoolSizes = sizes.data();

  vkCreateDescriptorPool(device.getLogicalDevice(), &pool_info, nullptr,
                         &applDescriptorPool);

  applDescriptorSets.resize(framesInFlight);
  std::vector<VkDescriptorSetLayout> layouts(framesInFlight, applSetLayout);

  VkDescriptorSetAllocateInfo allocInfo = {};
  allocInfo.pNext = nullptr;
  allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  allocInfo.descriptorPool = applDescriptorPool;
  allocInfo.descriptorSetCount = framesInFlight;
  allocInfo.pSetLayouts = layouts.data();

  vkAllocateDescriptorSets(device.getLogicalDevice(), &allocInfo,
                           applDescriptorSets.data());

  for (uint32_t f = 0; f < framesInFlight; ++f) {
    VkDescriptorBufferInfo binfo;
    binfo.buffer = applMaterialBuffer;
    binfo.offset = 0;
    binfo.range = sizeof(AAPLShaderMaterial) * materials.size();

    VkWriteDescriptorSet WriteMaterialToSet = {};
    WriteMaterialToSet.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    WriteMaterialToSet.pNext = nullptr;
    WriteMaterialToSet.dstBinding = 0;
    WriteMaterialToSet.dstSet = applDescriptorSets[f];
    WriteMaterialToSet.descriptorCount = 1;
    WriteMaterialToSet.dstArrayElement = 0;
    WriteMaterialToSet.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    WriteMaterialToSet.pBufferInfo = &binfo;

    VkDescriptorImageInfo samplerinfo;
    samplerinfo.sampler = textureSampler;
    VkWriteDescriptorSet setSampler = {};
    setSampler.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    setSampler.dstBinding = 1;
    setSampler.pNext = nullptr;
    setSampler.dstSet = applDescriptorSets[f];
    setSampler.dstArrayElement = 0;
    setSampler.descriptorCount = 1;
    setSampler.pImageInfo = &samplerinfo;
    setSampler.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;

    std::vector<VkDescriptorImageInfo> imageinfo;
    for (int texturei = 0; texturei < textures.size(); ++texturei) {
      VkDescriptorImageInfo dii = {.imageView = textures[texturei].second,
                                   .imageLayout =
                                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
      imageinfo.push_back(dii);
    }

    VkWriteDescriptorSet setWriteTexture = {};
    setWriteTexture.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    setWriteTexture.pNext = nullptr;
    setWriteTexture.dstBinding = 2;
    setWriteTexture.dstSet = applDescriptorSets[f];
    setWriteTexture.dstArrayElement = 0;
    setWriteTexture.descriptorCount = textures.size();
    setWriteTexture.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    setWriteTexture.pImageInfo = imageinfo.data();

    VkDescriptorBufferInfo meshChunksBufferInfo;
    meshChunksBufferInfo.buffer = meshChunksBuffer;
    meshChunksBufferInfo.offset = 0;
    meshChunksBufferInfo.range = sizeof(AAPLMeshChunk) * applMesh->_chunkCount;

    VkWriteDescriptorSet meshChunksWrite = {};
    meshChunksWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    meshChunksWrite.pNext = nullptr;
    meshChunksWrite.dstBinding = 3;
    meshChunksWrite.dstSet = applDescriptorSets[f];
    meshChunksWrite.descriptorCount = 1;
    meshChunksWrite.dstArrayElement = 0;
    meshChunksWrite.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    meshChunksWrite.pBufferInfo = &meshChunksBufferInfo;

    VkDescriptorBufferInfo chunkIndexBufferInfo;
    chunkIndexBufferInfo.buffer = chunkIndicesBuffers[f];
    chunkIndexBufferInfo.offset = 0;
    chunkIndexBufferInfo.range = sizeof(uint32_t) * applMesh->_chunkCount * 2 * SHADOW_CASCADE_COUNT;

    VkWriteDescriptorSet chunkIndexBufferWrite = {};
    chunkIndexBufferWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    chunkIndexBufferWrite.pNext = nullptr;
    chunkIndexBufferWrite.dstBinding = 4;
    chunkIndexBufferWrite.dstSet = applDescriptorSets[f];
    chunkIndexBufferWrite.descriptorCount = 1;
    chunkIndexBufferWrite.dstArrayElement = 0;
    chunkIndexBufferWrite.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    chunkIndexBufferWrite.pBufferInfo = &chunkIndexBufferInfo;

    std::array<VkWriteDescriptorSet, bindingcount> writes = {
        WriteMaterialToSet,        setSampler,
        setWriteTexture, meshChunksWrite, chunkIndexBufferWrite};

    vkUpdateDescriptorSets(device.getLogicalDevice(), writes.size(),
                           writes.data(), 0, nullptr);
  }
}

void GpuScene::init_GlobaldescriptorSet() {
  // information about the binding.
  VkDescriptorSetLayoutBinding camBufferBinding = {};
  camBufferBinding.binding = 0;
  camBufferBinding.descriptorCount = 1;
  // it's a uniform buffer binding
  camBufferBinding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;

  // we use it from the vertex shader
  camBufferBinding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT |
                                VK_SHADER_STAGE_FRAGMENT_BIT |
                                VK_SHADER_STAGE_COMPUTE_BIT |
                                VK_SHADER_STAGE_RAYGEN_BIT_KHR |
                                VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR |
                                VK_SHADER_STAGE_ANY_HIT_BIT_KHR |
                                VK_SHADER_STAGE_MISS_BIT_KHR;

  VkDescriptorSetLayoutBinding bindings[] = {camBufferBinding};

  VkDescriptorSetLayoutCreateInfo setinfo = {};
  setinfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  setinfo.pNext = nullptr;

  // we are going to have 1 binding
  setinfo.bindingCount = sizeof(bindings) / sizeof(bindings[0]);
  // no flags
  setinfo.flags = 0;
  // point to the camera buffer binding
  setinfo.pBindings = bindings;

  vkCreateDescriptorSetLayout(device.getLogicalDevice(), &setinfo, nullptr,
                              &globalSetLayout);

  // other code ....
  // create a descriptor pool that will hold 10 uniform buffers
  std::vector<VkDescriptorPoolSize> sizes = {
      {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 10 * framesInFlight},
  };

  VkDescriptorPoolCreateInfo pool_info = {};
  pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  pool_info.flags = 0;
  pool_info.maxSets = 10 * framesInFlight;
  pool_info.poolSizeCount = (uint32_t)sizes.size();
  pool_info.pPoolSizes = sizes.data();

  vkCreateDescriptorPool(device.getLogicalDevice(), &pool_info, nullptr,
                         &descriptorPool);

  globalDescriptorSets.resize(framesInFlight);
  std::vector<VkDescriptorSetLayout> layouts(framesInFlight, globalSetLayout);

  VkDescriptorSetAllocateInfo allocInfo = {};
  allocInfo.pNext = nullptr;
  allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  allocInfo.descriptorPool = descriptorPool;
  allocInfo.descriptorSetCount = framesInFlight;
  allocInfo.pSetLayouts = layouts.data();

  vkAllocateDescriptorSets(device.getLogicalDevice(), &allocInfo,
                           globalDescriptorSets.data());

  for (uint32_t i = 0; i < framesInFlight; ++i) {
    VkDescriptorBufferInfo binfo;
    binfo.buffer = uniformBuffers[i];
    binfo.offset = 0;
    binfo.range = sizeof(FrameData);

    VkWriteDescriptorSet setWrite = {};
    setWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    setWrite.pNext = nullptr;
    setWrite.dstBinding = 0;
    setWrite.dstSet = globalDescriptorSets[i];
    setWrite.descriptorCount = 1;
    setWrite.dstArrayElement = 0;
    setWrite.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    setWrite.pBufferInfo = &binfo;

    vkUpdateDescriptorSets(device.getLogicalDevice(), 1, &setWrite, 0, nullptr);
  }
}

void GpuScene::CreateGBuffers() {

  int width = device.getSwapChainExtent().width;
  int height = device.getSwapChainExtent().height;

  for (int i = 0; i < 4; i++) {
    _gbuffers[i].resize(framesInFlight);
    _gbuffersView[i].resize(framesInFlight);

    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.extent.width = width;
    imageInfo.extent.height = height;
    imageInfo.extent.depth = 1;
    imageInfo.mipLevels = 1; // texturedata._mipmapLevelCount;
    imageInfo.arrayLayers = 1;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL; // TODO: switch to linear with
    // initiallayout=preinitialized?
    imageInfo.initialLayout =
        VK_IMAGE_LAYOUT_UNDEFINED; // VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    imageInfo.format =
        _gbufferFormat[i]; // TODO:or VK_FORMAT_D32_SFLOAT_S8_UINT? we don't
    // need stencil currently anyway
    imageInfo.usage =
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.flags = 0; // Optional

    for (uint32_t f = 0; f < framesInFlight; ++f) {
    if (vkCreateImage(device.getLogicalDevice(), &imageInfo, nullptr,
                      &_gbuffers[i][f]) != VK_SUCCESS) {
      throw std::runtime_error("failed to create depth rt!");
    }
    VkMemoryRequirements memRequirements;
    vkGetImageMemoryRequirements(device.getLogicalDevice(), _gbuffers[i][f],
                                 &memRequirements);

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memRequirements.size;
    allocInfo.memoryTypeIndex = device.findMemoryType(
        memRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    VkDeviceMemory textureImageMemory;
    if (vkAllocateMemory(device.getLogicalDevice(), &allocInfo, nullptr,
                         &textureImageMemory) != VK_SUCCESS) {
      throw std::runtime_error("failed to allocate image memory!");
    }

    vkBindImageMemory(device.getLogicalDevice(), _gbuffers[i][f],
                      textureImageMemory, 0);

    VkImageViewCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    createInfo.image = _gbuffers[i][f];
    createInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    createInfo.format = _gbufferFormat[i];
    createInfo.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
    createInfo.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
    createInfo.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
    createInfo.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
    createInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    createInfo.subresourceRange.baseMipLevel = 0;
    createInfo.subresourceRange.levelCount = 1;
    createInfo.subresourceRange.baseArrayLayer = 0;
    createInfo.subresourceRange.layerCount = 1;

    if (vkCreateImageView(device.getLogicalDevice(), &createInfo, nullptr,
                          &_gbuffersView[i][f]) != VK_SUCCESS) {
      throw std::runtime_error("failed to create gbuffers image views!");
    }
    } // end per-frame
  }
}

void GpuScene::CreateDepthTexture() {
  int width = device.getSwapChainExtent().width;
  int height = device.getSwapChainExtent().height;
  VkImageCreateInfo imageInfo{};
  imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  imageInfo.imageType = VK_IMAGE_TYPE_2D;
  imageInfo.extent.width = width;
  imageInfo.extent.height = height;
  imageInfo.extent.depth = 1;
  imageInfo.mipLevels = 1; // texturedata._mipmapLevelCount;
  imageInfo.arrayLayers = 1;
  imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL; // TODO: switch to linear with
                                              // initiallayout=preinitialized?
  imageInfo.initialLayout =
      VK_IMAGE_LAYOUT_UNDEFINED;   // VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
  imageInfo.format = VK_FORMAT_D32_SFLOAT;
  imageInfo.usage =
      VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
  imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
  imageInfo.flags = 0; // Optional

  if (vkCreateImage(device.getLogicalDevice(), &imageInfo, nullptr,
                    &_depthTexture) != VK_SUCCESS) {
    throw std::runtime_error("failed to create depth rt!");
  }

  VkMemoryRequirements memRequirements;
  vkGetImageMemoryRequirements(device.getLogicalDevice(), _depthTexture,
                               &memRequirements);

  VkMemoryAllocateInfo allocInfo{};
  allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  allocInfo.allocationSize = memRequirements.size;
  allocInfo.memoryTypeIndex = device.findMemoryType(
      memRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

  VkDeviceMemory textureImageMemory;
  if (vkAllocateMemory(device.getLogicalDevice(), &allocInfo, nullptr,
                       &textureImageMemory) != VK_SUCCESS) {
    throw std::runtime_error("failed to allocate image memory!");
  }

  vkBindImageMemory(device.getLogicalDevice(), _depthTexture,
                    textureImageMemory, 0);

  // TODO: change to vk_image_layout_depth_attachment_optimal
  // transition param will be specified in renderpass
  // device.transitionImageLayout(_depthTexture, _depthFormat,
  // VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL
  // );

  // TODO: must be power of 2
  int bigger = width > height ? width : height;
  int log_bigger = floor(log2f(bigger));
  int mip_level = log_bigger;

  imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  imageInfo.imageType = VK_IMAGE_TYPE_2D;
  imageInfo.extent.width = width;
  imageInfo.extent.height = height;
  imageInfo.extent.depth = 1;
  imageInfo.arrayLayers = 1;
  imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL; // TODO: switch to linear with
                                              // initiallayout=preinitialized?
  imageInfo.initialLayout =
      VK_IMAGE_LAYOUT_UNDEFINED; // VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
  imageInfo.format =
      VK_FORMAT_D32_SFLOAT; // TODO:or VK_FORMAT_D32_SFLOAT_S8_UINT? we don't
                            // need stencil currently anyway
  imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
  imageInfo.flags = 0; // Optional

  imageInfo.mipLevels = mip_level;
  imageInfo.usage =
      VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
  // VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT;

  if (vkCreateImage(device.getLogicalDevice(), &imageInfo, nullptr,
                    &_depthPyramidTexture) != VK_SUCCESS) {
    throw std::runtime_error("failed to create depth pyramid!");
  }
}

void GpuScene::ConfigureMaterial(const AAPLMaterial &input,
                                 AAPLShaderMaterial &output) {
  if (input.hasBaseColorTexture &&
      !textureHashMap.contains(input.baseColorTextureHash)) {
    spdlog::error("texture hash {} is invalid", input.baseColorTextureHash);
  }
  if (input.hasNormalMap && !textureHashMap.contains(input.normalMapHash)) {
    spdlog::error("texture hash {} is invalid", input.normalMapHash);
  }
  if (input.hasEmissiveTexture &&
      !textureHashMap.contains(input.emissiveTextureHash)) {
    spdlog::error("texture hash {} is invalid", input.emissiveTextureHash);
  }
  if (input.hasMetallicRoughnessTexture &&
      !textureHashMap.contains(input.metallicRoughnessHash)) {
    spdlog::error("texture hash {} is invalid", input.metallicRoughnessHash);
  }

  const uint32_t INVALID_TEXTURE_INDEX = 0xffffffff;
  output.albedo_texture_index = input.hasBaseColorTexture
                                    ? textureHashMap[input.baseColorTextureHash]
                                    : INVALID_TEXTURE_INDEX;
  output.normal_texture_index = input.hasNormalMap
                                    ? textureHashMap[input.normalMapHash]
                                    : INVALID_TEXTURE_INDEX;
  output.emissive_texture_index =
      input.hasEmissiveTexture ? textureHashMap[input.emissiveTextureHash]
                               : INVALID_TEXTURE_INDEX;
  output.roughness_texture_index =
      input.hasMetallicRoughnessTexture
          ? textureHashMap[input.metallicRoughnessHash]
          : INVALID_TEXTURE_INDEX;
  output.hasEmissive = input.hasEmissiveTexture;
  output.hasMetallicRoughness = input.hasMetallicRoughnessTexture;
  output.alpha = input.opacity;
}

GpuScene::~GpuScene() {
  shutdownTextureStreaming();
  free(cpuMaterials);
  free(m_SubMeshes);
}

GpuScene::GpuScene(std::filesystem::path &root, const VulkanDevice &deviceref)
    : device(deviceref), modelScale(1.f), _rootPath(root) {

  createSyncObjects();
  createUniformBuffer();

  createCommandBuffers(deviceref.getCommandPool());

  applMesh =
#ifdef __ANDROID__
      new AAPLMeshData("bistro.astc.bin");
#else
      new AAPLMeshData((_rootPath / "bistro.dxt.bin").generic_string().c_str());
#endif

#ifdef __ANDROID__
  {
    auto sceneBytes = AssetLoader::loadAssetBytes("scene.scene");
    std::string sceneStr(sceneBytes.begin(), sceneBytes.end());
    sceneFile = nlohmann::json::parse(sceneStr);
  }
#else
  std::ifstream f(root / "scene.scene");
  sceneFile = nlohmann::json::parse(f);
#endif

  // maincamera = new Camera(60 * 3.1414926f / 180.f, 0.1, 100, vec3(0, 0, -2),
  //                         deviceref.getSwapChainExtent().width /
  //                             float(deviceref.getSwapChainExtent().height));
  vec3 camera_pos = vec3(sceneFile["camera_position"][0].template get<float>(),
                         sceneFile["camera_position"][1].template get<float>(),
                         sceneFile["camera_position"][2].template get<float>());
  vec3 camera_up = vec3(sceneFile["camera_up"][0].template get<float>(),
                        sceneFile["camera_up"][1].template get<float>(),
                        sceneFile["camera_up"][2].template get<float>());
  vec3 camera_dir =
      vec3(sceneFile["camera_direction"][0].template get<float>(),
           sceneFile["camera_direction"][1].template get<float>(),
           sceneFile["camera_direction"][2].template get<float>());
  maincamera = new Camera(65 * 3.1414926f / 180.f, 0.1, 100, camera_pos,
                          deviceref.getSwapChainExtent().width /
                              float(deviceref.getSwapChainExtent().height),
                          camera_dir, camera_up * -1);

  // maincamera = new Camera(90 * 3.1414926f / 180.f, 1, 100, vec3(0, 0, 0),
  //     deviceref.getSwapChainExtent().width /
  //     float(deviceref.getSwapChainExtent().height), vec3(0,0,1), vec3(0,1,
  //     0));

  // Sun defaults: scene.scene provides direction, not color.
  // AAPLFrameConstants is zero-initialised by its member ctors; set
  // sun values once in the constructor so the per-frame memcpy to the
  // GPU uniform always carries valid sun data.
  if (sceneFile.contains("sun_direction")) {
    frameConstants.sunDirection = vec3(
        sceneFile["sun_direction"][0].template get<float>(),
        sceneFile["sun_direction"][1].template get<float>(),
        sceneFile["sun_direction"][2].template get<float>());
  }
  // Default warm-white equivalent to ~6500K sky + sun.
  if (frameConstants.sunColor.x == 0.0f && frameConstants.sunColor.y == 0.0f &&
      frameConstants.sunColor.z == 0.0f) {
    frameConstants.sunColor = vec3(1.0f, 0.95f, 0.85f);
  }
  if (frameConstants.localLightIntensity == 0.0f) {
    frameConstants.localLightIntensity = 1.0f;
  }

  {
    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = sceneFile["occluder_indices"].size() *
                      sizeof(uint32_t); // TODO uint16_t
    bufferInfo.usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    bufferInfo.flags = 0;
    if (vkCreateBuffer(device.getLogicalDevice(), &bufferInfo, nullptr,
                       &_occludersIndexBuffer) != VK_SUCCESS) {
      throw std::runtime_error("failed to create vertex buffer!");
    }
    VkMemoryRequirements memRequirements;
    vkGetBufferMemoryRequirements(device.getLogicalDevice(),
                                  _occludersIndexBuffer, &memRequirements);

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memRequirements.size;
    allocInfo.memoryTypeIndex =
        device.findMemoryType(memRequirements.memoryTypeBits,
                              VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                  VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

    if (vkAllocateMemory(device.getLogicalDevice(), &allocInfo, nullptr,
                         &_occludersIndexBufferMemory) != VK_SUCCESS) {
      throw std::runtime_error("failed to allocate vertex buffer memory!");
    }
    vkBindBufferMemory(device.getLogicalDevice(), _occludersIndexBuffer,
                       _occludersIndexBufferMemory, 0);
    uint32_t *data;
    vkMapMemory(device.getLogicalDevice(), _occludersIndexBufferMemory, 0,
                bufferInfo.size, 0, (void **)&data);
    for (int i = 0; i < sceneFile["occluder_indices"].size(); ++i) {
      *data++ = sceneFile["occluder_indices"][i].template get<uint32_t>();
    }

    vkUnmapMemory(device.getLogicalDevice(), _occludersIndexBufferMemory);
  }
  VkBufferCreateInfo bufferInfo{};
  bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  bufferInfo.size = sceneFile["occluder_verts"].size() * sizeof(float) * 3;
  bufferInfo.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
  bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  bufferInfo.flags = 0;
  if (vkCreateBuffer(device.getLogicalDevice(), &bufferInfo, nullptr,
                     &_occludersVertBuffer) != VK_SUCCESS) {
    throw std::runtime_error("failed to create vertex buffer!");
  }
  VkMemoryRequirements memRequirements;
  vkGetBufferMemoryRequirements(device.getLogicalDevice(), _occludersVertBuffer,
                                &memRequirements);

  VkMemoryAllocateInfo allocInfo{};
  allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  allocInfo.allocationSize = memRequirements.size;
  allocInfo.memoryTypeIndex = device.findMemoryType(
      memRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

  if (vkAllocateMemory(device.getLogicalDevice(), &allocInfo, nullptr,
                       &_occludersBufferMemory) != VK_SUCCESS) {
    throw std::runtime_error("failed to allocate vertex buffer memory!");
  }
  vkBindBufferMemory(device.getLogicalDevice(), _occludersVertBuffer,
                     _occludersBufferMemory, 0);
  float *data;
  float x_offset = sceneFile["center_offset"][0].template get<float>();
  float y_offset = sceneFile["center_offset"][1].template get<float>();
  float z_offset = sceneFile["center_offset"][2].template get<float>();
  vkMapMemory(device.getLogicalDevice(), _occludersBufferMemory, 0,
              bufferInfo.size, 0, (void **)&data);
  for (int i = 0; i < sceneFile["occluder_verts"].size(); ++i) {
    // for (int j = 0; j < 3; j++)
    //{
    // z和y需要换下顺序
    float x = sceneFile["occluder_verts"][i][0].template get<float>();
    float z = sceneFile["occluder_verts"][i][1].template get<float>();
    float y = sceneFile["occluder_verts"][i][2].template get<float>();
    //}
    *data++ = x - x_offset;
    *data++ = y - y_offset;
    *data++ = z - z_offset;
  }

  vkUnmapMemory(device.getLogicalDevice(), _occludersBufferMemory);

  CreateTextures();

  // Start texture streaming background thread
  initTextureStreaming();

  if (sizeof(AAPLMaterial) != 96) {
    spdlog::error("layout mismatch with apple sizeof(AAPLMaterial) is {}, "
                  "while apple is 96",
                  sizeof(AAPLMaterial));
  }

  cpuMaterials = (AAPLMaterial *)uncompressData(
      (unsigned char *)applMesh->_materialData,
      applMesh->compressedMaterialDataLength,
      applMesh->_materialCount * sizeof(AAPLMaterial));
  materials.resize(applMesh->_materialCount);
  for (int i = 0; i < applMesh->_materialCount; i++) {
    ConfigureMaterial(cpuMaterials[i], materials[i]);
  };

  // create material buffer
  {
    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = applMesh->_materialCount * sizeof(AAPLShaderMaterial);
    bufferInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    bufferInfo.flags = 0;
    if (vkCreateBuffer(device.getLogicalDevice(), &bufferInfo, nullptr,
                       &applMaterialBuffer) != VK_SUCCESS) {
      throw std::runtime_error("failed to create vertex buffer!");
    }
    VkMemoryRequirements memRequirements;
    vkGetBufferMemoryRequirements(device.getLogicalDevice(), applMaterialBuffer,
                                  &memRequirements);

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memRequirements.size;
    allocInfo.memoryTypeIndex =
        device.findMemoryType(memRequirements.memoryTypeBits,
                              VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                  VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

    if (vkAllocateMemory(device.getLogicalDevice(), &allocInfo, nullptr,
                         &applMaterialBufferMemory) != VK_SUCCESS) {
      throw std::runtime_error("failed to allocate vertex buffer memory!");
    }
    vkBindBufferMemory(device.getLogicalDevice(), applMaterialBuffer,
                       applMaterialBufferMemory, 0);
    void *data;
    vkMapMemory(device.getLogicalDevice(), applMaterialBufferMemory, 0,
                bufferInfo.size, 0, &data);
    memcpy(data, materials.data(), bufferInfo.size);
    vkUnmapMemory(device.getLogicalDevice(), applMaterialBufferMemory);
  }

  vec3 *vertexs = (vec3 *)uncompressData(
      (unsigned char *)applMesh->_vertexData,
      applMesh->compressedVertexDataLength, [this](uint64_t buffersize) {
        VkBufferCreateInfo bufferInfo{};
        bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.size = buffersize;
        bufferInfo.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                           VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
                           VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                           VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        bufferInfo.flags = 0;
        if (vkCreateBuffer(device.getLogicalDevice(), &bufferInfo, nullptr,
                           &applVertexBuffer) != VK_SUCCESS) {
          throw std::runtime_error("failed to create vertex buffer!");
        }
        VkMemoryRequirements memRequirements;
        vkGetBufferMemoryRequirements(device.getLogicalDevice(),
                                      applVertexBuffer, &memRequirements);

        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = memRequirements.size;
        allocInfo.memoryTypeIndex =
            device.findMemoryType(memRequirements.memoryTypeBits,
                                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                      VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        VkMemoryAllocateFlagsInfo allocFlagsInfo{};
        allocFlagsInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO;
        allocFlagsInfo.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT_KHR;
        allocInfo.pNext = &allocFlagsInfo;

        if (vkAllocateMemory(device.getLogicalDevice(), &allocInfo, nullptr,
                             &applVertexBufferMemory) != VK_SUCCESS) {
          throw std::runtime_error("failed to allocate vertex buffer memory!");
        }
        vkBindBufferMemory(device.getLogicalDevice(), applVertexBuffer,
                           applVertexBufferMemory, 0);
        void *data;
        vkMapMemory(device.getLogicalDevice(), applVertexBufferMemory, 0,
                    bufferInfo.size, 0, &data);
        return data;
      }); // applMesh->_vertexCount * sizeof(vec3));
  // TODO: ugly
  vkUnmapMemory(device.getLogicalDevice(), applVertexBufferMemory);

  vec3 *normals = (vec3 *)uncompressData(
      (unsigned char *)applMesh->_normalData,
      applMesh->compressedNormalDataLength, [this](uint64_t buffersize) {
        VkBufferCreateInfo bufferInfo{};
        bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.size = buffersize;
        bufferInfo.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                           VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                           VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        bufferInfo.flags = 0;
        if (vkCreateBuffer(device.getLogicalDevice(), &bufferInfo, nullptr,
                           &applNormalBuffer) != VK_SUCCESS) {
          throw std::runtime_error("failed to create vertex buffer!");
        }
        VkMemoryRequirements memRequirements;
        vkGetBufferMemoryRequirements(device.getLogicalDevice(),
                                      applNormalBuffer, &memRequirements);

        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = memRequirements.size;
        allocInfo.memoryTypeIndex =
            device.findMemoryType(memRequirements.memoryTypeBits,
                                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                      VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        VkMemoryAllocateFlagsInfo allocFlagsInfo{};
        allocFlagsInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO;
        allocFlagsInfo.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT_KHR;
        allocInfo.pNext = &allocFlagsInfo;

        if (vkAllocateMemory(device.getLogicalDevice(), &allocInfo, nullptr,
                             &applNormalBufferMemory) != VK_SUCCESS) {
          throw std::runtime_error("failed to allocate vertex buffer memory!");
        }
        vkBindBufferMemory(device.getLogicalDevice(), applNormalBuffer,
                           applNormalBufferMemory, 0);
        void *data;
        vkMapMemory(device.getLogicalDevice(), applNormalBufferMemory, 0,
                    bufferInfo.size, 0, &data);
        return data;
      });
  vkUnmapMemory(device.getLogicalDevice(), applNormalBufferMemory);

  vec3 *tangents = (vec3 *)uncompressData(
      (unsigned char *)applMesh->_tangentData,
      applMesh->compressedTangentDataLength, [this](uint64_t buffersize) {
        VkBufferCreateInfo bufferInfo{};
        bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.size = buffersize;
        bufferInfo.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                           VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                           VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        bufferInfo.flags = 0;
        if (vkCreateBuffer(device.getLogicalDevice(), &bufferInfo, nullptr,
                           &applTangentBuffer) != VK_SUCCESS) {
          throw std::runtime_error("failed to create vertex buffer!");
        }
        VkMemoryRequirements memRequirements;
        vkGetBufferMemoryRequirements(device.getLogicalDevice(),
                                      applTangentBuffer, &memRequirements);

        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = memRequirements.size;
        allocInfo.memoryTypeIndex =
            device.findMemoryType(memRequirements.memoryTypeBits,
                                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                      VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        VkMemoryAllocateFlagsInfo allocFlagsInfo{};
        allocFlagsInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO;
        allocFlagsInfo.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT_KHR;
        allocInfo.pNext = &allocFlagsInfo;

        if (vkAllocateMemory(device.getLogicalDevice(), &allocInfo, nullptr,
                             &applTangentBufferMemory) != VK_SUCCESS) {
          throw std::runtime_error("failed to allocate vertex buffer memory!");
        }
        vkBindBufferMemory(device.getLogicalDevice(), applTangentBuffer,
                           applTangentBufferMemory, 0);
        void *data;
        vkMapMemory(device.getLogicalDevice(), applTangentBufferMemory, 0,
                    bufferInfo.size, 0, &data);
        return data;
      });
  vkUnmapMemory(device.getLogicalDevice(), applTangentBufferMemory);

  vec2 *uvs = (vec2 *)uncompressData(
      (unsigned char *)applMesh->_uvData, applMesh->compressedUvDataLength,
      [this](uint64_t buffersize) {
        VkBufferCreateInfo bufferInfo{};
        bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.size = buffersize;
        bufferInfo.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                           VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                           VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        bufferInfo.flags = 0;
        if (vkCreateBuffer(device.getLogicalDevice(), &bufferInfo, nullptr,
                           &applUVBuffer) != VK_SUCCESS) {
          throw std::runtime_error("failed to create vertex buffer!");
        }
        VkMemoryRequirements memRequirements;
        vkGetBufferMemoryRequirements(device.getLogicalDevice(), applUVBuffer,
                                      &memRequirements);

        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = memRequirements.size;
        allocInfo.memoryTypeIndex =
            device.findMemoryType(memRequirements.memoryTypeBits,
                                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                      VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        VkMemoryAllocateFlagsInfo allocFlagsInfo{};
        allocFlagsInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO;
        allocFlagsInfo.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT_KHR;
        allocInfo.pNext = &allocFlagsInfo;

        if (vkAllocateMemory(device.getLogicalDevice(), &allocInfo, nullptr,
                             &applUVBufferMemory) != VK_SUCCESS) {
          throw std::runtime_error("failed to allocate vertex buffer memory!");
        }
        vkBindBufferMemory(device.getLogicalDevice(), applUVBuffer,
                           applUVBufferMemory, 0);
        void *data;
        vkMapMemory(device.getLogicalDevice(), applUVBufferMemory, 0,
                    bufferInfo.size, 0, &data);
        return data;
      });
  vkUnmapMemory(device.getLogicalDevice(), applUVBufferMemory);

  uint32_t *indices = (uint32_t *)uncompressData(
      (unsigned char *)applMesh->_indexData,
      applMesh->compressedIndexDataLength, [this](uint64_t buffersize) {
        VkBufferCreateInfo bufferInfo{};
        bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.size = buffersize;
        bufferInfo.usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                           VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
                           VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                           VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        bufferInfo.flags = 0;
        if (vkCreateBuffer(device.getLogicalDevice(), &bufferInfo, nullptr,
                           &applIndexBuffer) != VK_SUCCESS) {
          throw std::runtime_error("failed to create index buffer!");
        }
        VkMemoryRequirements memRequirements;
        vkGetBufferMemoryRequirements(device.getLogicalDevice(),
                                      applIndexBuffer, &memRequirements);

        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = memRequirements.size;
        allocInfo.memoryTypeIndex =
            device.findMemoryType(memRequirements.memoryTypeBits,
                                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                      VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        VkMemoryAllocateFlagsInfo allocFlagsInfo{};
        allocFlagsInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO;
        allocFlagsInfo.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT_KHR;
        allocInfo.pNext = &allocFlagsInfo;

        if (vkAllocateMemory(device.getLogicalDevice(), &allocInfo, nullptr,
                             &applIndexMemory) != VK_SUCCESS) {
          throw std::runtime_error("failed to allocate vertex buffer memory!");
        }
        vkBindBufferMemory(device.getLogicalDevice(), applIndexBuffer,
                           applIndexMemory, 0);
        void *data;
        vkMapMemory(device.getLogicalDevice(), applIndexMemory, 0,
                    bufferInfo.size, 0, &data);

        return data;
      });
  vkUnmapMemory(device.getLogicalDevice(), applIndexMemory);

  m_Chunks = (AAPLMeshChunk *)uncompressData(
      (unsigned char *)applMesh->_chunkData,
      applMesh->compressedChunkDataLength,
      applMesh->_chunkCount * sizeof(AAPLMeshChunk));

  {
    VkBufferCreateInfo meshChunkBufferCreateInfo{};
    meshChunkBufferCreateInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    meshChunkBufferCreateInfo.size =
        applMesh->_chunkCount * sizeof(AAPLMeshChunk);
    meshChunkBufferCreateInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    meshChunkBufferCreateInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    meshChunkBufferCreateInfo.flags = 0;

    if (vkCreateBuffer(device.getLogicalDevice(), &meshChunkBufferCreateInfo,
                       nullptr, &meshChunksBuffer) != VK_SUCCESS) {
      throw std::runtime_error("failed to allocate mesh chunk buffer");
    }

    VkMemoryRequirements memRequirements;
    vkGetBufferMemoryRequirements(device.getLogicalDevice(), meshChunksBuffer,
                                  &memRequirements);

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memRequirements.size;
    allocInfo.memoryTypeIndex =
        device.findMemoryType(memRequirements.memoryTypeBits,
                              VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                  VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

    if (vkAllocateMemory(device.getLogicalDevice(), &allocInfo, nullptr,
                         &meshChunksBufferMemory) != VK_SUCCESS) {
      throw std::runtime_error("failed to allocate mesh chunks buffer memory!");
    }
    vkBindBufferMemory(device.getLogicalDevice(), meshChunksBuffer,
                       meshChunksBufferMemory, 0);
    void *data;
    vkMapMemory(device.getLogicalDevice(), meshChunksBufferMemory, 0,
                meshChunkBufferCreateInfo.size, 0, &data);
    std::memcpy(data, (void *)m_Chunks, meshChunkBufferCreateInfo.size);
    vkUnmapMemory(device.getLogicalDevice(), meshChunksBufferMemory);

    // free(m_Chunks);
  }

  {
    drawParamsBuffers.resize(framesInFlight);
    drawParamsBufferMemories.resize(framesInFlight);
    for (uint32_t f = 0; f < framesInFlight; ++f) {
      VkBufferCreateInfo drawParamsBufferInfo{};
      drawParamsBufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
      drawParamsBufferInfo.size =
          sizeof(VkDrawIndexedIndirectCommand) * applMesh->_chunkCount;
      drawParamsBufferInfo.usage = VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
                                   VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
      drawParamsBufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
      drawParamsBufferInfo.flags = 0;
      if (vkCreateBuffer(device.getLogicalDevice(), &drawParamsBufferInfo,
                         nullptr, &drawParamsBuffers[f]) != VK_SUCCESS) {
        throw std::runtime_error("failed to create drawparams buffer!");
      }
      VkMemoryRequirements memRequirements;
      vkGetBufferMemoryRequirements(device.getLogicalDevice(), drawParamsBuffers[f],
                                    &memRequirements);

      VkMemoryAllocateInfo allocInfo{};
      allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
      allocInfo.allocationSize = memRequirements.size;
      allocInfo.memoryTypeIndex =
          device.findMemoryType(memRequirements.memoryTypeBits,
                                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                    VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

      if (vkAllocateMemory(device.getLogicalDevice(), &allocInfo, nullptr,
                           &drawParamsBufferMemories[f]) != VK_SUCCESS) {
        throw std::runtime_error("failed to allocate drawparams buffer memory!");
      }
      vkBindBufferMemory(device.getLogicalDevice(), drawParamsBuffers[f],
                         drawParamsBufferMemories[f], 0);
    }
  }

  {
    cullParamsBuffers.resize(framesInFlight);
    cullParamsBufferMemories.resize(framesInFlight);
    for (uint32_t f = 0; f < framesInFlight; ++f) {
      VkBufferCreateInfo cullParamsBufferInfo{};
      cullParamsBufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
      cullParamsBufferInfo.size = sizeof(GPUCullParams);
      cullParamsBufferInfo.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
      cullParamsBufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
      cullParamsBufferInfo.flags = 0;
      if (vkCreateBuffer(device.getLogicalDevice(), &cullParamsBufferInfo,
                         nullptr, &cullParamsBuffers[f]) != VK_SUCCESS) {
        throw std::runtime_error("failed to create cull params buffer!");
      }
      VkMemoryRequirements memRequirements;
      vkGetBufferMemoryRequirements(device.getLogicalDevice(), cullParamsBuffers[f],
                                    &memRequirements);

      VkMemoryAllocateInfo allocInfo{};
      allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
      allocInfo.allocationSize = memRequirements.size;
      allocInfo.memoryTypeIndex =
          device.findMemoryType(memRequirements.memoryTypeBits,
                                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                    VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

      if (vkAllocateMemory(device.getLogicalDevice(), &allocInfo, nullptr,
                           &cullParamsBufferMemories[f]) != VK_SUCCESS) {
        throw std::runtime_error("failed to allocate cull params buffer memory!");
      }
      vkBindBufferMemory(device.getLogicalDevice(), cullParamsBuffers[f],
                         cullParamsBufferMemories[f], 0);
    }
  }

  {
    writeIndexBuffers.resize(framesInFlight);
    writeIndexBufferMemories.resize(framesInFlight);
    for (uint32_t f = 0; f < framesInFlight; ++f) {
      VkBufferCreateInfo writeIndexBufferInfo{};
      writeIndexBufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
      writeIndexBufferInfo.size = 3 * sizeof(uint32_t); // opaque, alphaMask, transparent counts
      writeIndexBufferInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                   VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
      writeIndexBufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
      writeIndexBufferInfo.flags = 0;
      if (vkCreateBuffer(device.getLogicalDevice(), &writeIndexBufferInfo,
                         nullptr, &writeIndexBuffers[f]) != VK_SUCCESS) {
        throw std::runtime_error("failed to create writeindex buffer!");
      }
      VkMemoryRequirements memRequirements;
      vkGetBufferMemoryRequirements(device.getLogicalDevice(), writeIndexBuffers[f],
                                    &memRequirements);

      VkMemoryAllocateInfo allocInfo{};
      allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
      allocInfo.allocationSize = memRequirements.size;
      allocInfo.memoryTypeIndex =
          device.findMemoryType(memRequirements.memoryTypeBits,
                                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                    VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

      if (vkAllocateMemory(device.getLogicalDevice(), &allocInfo, nullptr,
                           &writeIndexBufferMemories[f]) != VK_SUCCESS) {
        throw std::runtime_error("failed to allocate writeindex buffer memory!");
      }
      vkBindBufferMemory(device.getLogicalDevice(), writeIndexBuffers[f],
                         writeIndexBufferMemories[f], 0);
    }
  }

  {
    chunkIndicesBuffers.resize(framesInFlight);
    chunkIndicesBufferMemories.resize(framesInFlight);
    for (uint32_t f = 0; f < framesInFlight; ++f) {
      VkBufferCreateInfo chunkIndicesBufferInfo{};
      chunkIndicesBufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
      chunkIndicesBufferInfo.size = sizeof(uint32_t) * applMesh->_chunkCount * SHADOW_CASCADE_COUNT * 2;// *2 is for 2 different sets (opqaue and masked)
      chunkIndicesBufferInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
      chunkIndicesBufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
      chunkIndicesBufferInfo.flags = 0;
      if (vkCreateBuffer(device.getLogicalDevice(), &chunkIndicesBufferInfo,
                         nullptr, &chunkIndicesBuffers[f]) != VK_SUCCESS) {
        throw std::runtime_error("failed to create chunkindices buffer!");
      }
      VkMemoryRequirements memRequirements;
      vkGetBufferMemoryRequirements(device.getLogicalDevice(), chunkIndicesBuffers[f],
                                    &memRequirements);

      VkMemoryAllocateInfo allocInfo{};
      allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
      allocInfo.allocationSize = memRequirements.size;
      allocInfo.memoryTypeIndex =
          device.findMemoryType(memRequirements.memoryTypeBits,
                                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                    VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

      if (vkAllocateMemory(device.getLogicalDevice(), &allocInfo, nullptr,
                           &chunkIndicesBufferMemories[f]) != VK_SUCCESS) {
        throw std::runtime_error("failed to allocate chunkindices buffer memory!");
      }
      vkBindBufferMemory(device.getLogicalDevice(), chunkIndicesBuffers[f],
                         chunkIndicesBufferMemories[f], 0);
    }
  }

  m_SubMeshes = (AAPLSubMesh *)uncompressData(
      (unsigned char *)applMesh->_meshData, applMesh->compressedMeshDataLength,
      applMesh->_meshCount * sizeof(AAPLSubMesh));

  CreateDepthTexture();
  CreateZdepthView();
  CreateGBuffers();
  createHDRLightingBuffer();
  createTAAHistoryBuffer();
  CreateOccluderZPass();
  CreateOccluderZPassFrameBuffer();
  CreateDeferredBasePass();
  CreateDeferredLightingPass();
  CreateForwardLightingPass();
  createResolvePass();
  CreateBasePassFrameBuffer();
  CreateDeferredLightingFrameBuffer(device.getSwapChainImageCount());
  CreateForwardLightingFrameBuffer(device.getSwapChainImageCount());
  createResolveFrameBuffer(device.getSwapChainImageCount());

  createTextureSampler();
  createNearestClampSampler();
  createLinearClampSampler();

  // auto textureRes = createTexture(applMesh->_textures[13]);
  // auto textureRes =
  // createTexture("G:\\AdvancedVulkanRendering\\textures\\texture.jpg");
  // currentImage = textureRes.first;
  init_GlobaldescriptorSet();
  // init_descriptors(textureRes.second);
  // init_descriptors(textures[13].second);
  init_appl_descriptors();
  init_drawparams_descriptors();
  createHiZResources(); // Stage 3: creates Hi-Z texture and updates gpuCullDescriptorSet
  init_deferredlighting_descriptors();
  createSAOResources();
  createDecalResources();
  createResolveDescriptors();
  createLinearClampSampler();
  createGraphicsPipeline(deviceref.getMainRenderPass());
  createRenderOccludersPipeline(occluderZPass);
  createOccluderWireframePipeline();
  createResolvePipeline();
  createComputePipeline();

  _shadow = new Shadow(device,*this,1024);
  // createScatterVolume() is called after scene loading (needs LightCuller + light counts)
  // create point light
  {
    size_t pointlightCount = sceneFile["point_lights"].size();
    PointLight::pointLightData.reserve(pointlightCount);
    _pointLights.reserve(pointlightCount);
    for (int i = 0; i < pointlightCount; i++) {
      float posx =
          sceneFile["point_lights"][i]["position_x"].template get<float>();
      float posy =
          sceneFile["point_lights"][i]["position_y"].template get<float>();
      float posz =
          sceneFile["point_lights"][i]["position_z"].template get<float>();
      float color_r =
          sceneFile["point_lights"][i]["color_r"].template get<float>();
      float color_g =
          sceneFile["point_lights"][i]["color_g"].template get<float>();
      float color_b =
          sceneFile["point_lights"][i]["color_b"].template get<float>();
      float radius =
          sceneFile["point_lights"][i]["sqrt_radius"].template get<float>();
      uint32_t flags =
          sceneFile["point_lights"][i]["for_transparent"].template get<bool>()
              ? LIGHT_FOR_TRANSPARENT_FLAG
              : 0;
      PointLight::pointLightData.emplace_back(PointLightData(
          posx, posy, posz, radius, color_r, color_g, color_b, flags));
      _pointLights.emplace_back(PointLight(i * sizeof(PointLightData),
                                           &PointLight::pointLightData.back()));
    }

    VkBufferCreateInfo pointLightBufferInfo{};
    pointLightBufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    pointLightBufferInfo.size = pointlightCount * sizeof(PointLightData);
    pointLightBufferInfo.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    pointLightBufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    pointLightBufferInfo.flags = 0;
    if (vkCreateBuffer(device.getLogicalDevice(), &pointLightBufferInfo,
                       nullptr, &PointLight::pointLightDynamicUniformBuffer) !=
        VK_SUCCESS) {
      throw std::runtime_error("failed to create pointLight buffer!");
    }
    VkMemoryRequirements memRequirements;
    vkGetBufferMemoryRequirements(device.getLogicalDevice(),
                                  PointLight::pointLightDynamicUniformBuffer,
                                  &memRequirements);

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memRequirements.size;
    allocInfo.memoryTypeIndex =
        device.findMemoryType(memRequirements.memoryTypeBits,
                              VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                  VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkDeviceMemory pointLightBufferMemory;
    if (vkAllocateMemory(device.getLogicalDevice(), &allocInfo, nullptr,
                         &pointLightBufferMemory) != VK_SUCCESS) {
      throw std::runtime_error("failed to allocate pointLight buffer memory!");
    }
    vkBindBufferMemory(device.getLogicalDevice(),
                       PointLight::pointLightDynamicUniformBuffer,
                       pointLightBufferMemory, 0);
    void *data;
    vkMapMemory(device.getLogicalDevice(), pointLightBufferMemory, 0,
                pointLightBufferInfo.size, 0, &data);
    std::memcpy(data, (void *)PointLight::pointLightData.data(),
                pointLightBufferInfo.size);
    vkUnmapMemory(device.getLogicalDevice(), pointLightBufferMemory);

    PointLight::InitRHI(device, *this);
    _shadow->InitGPUShadowResources(device,*this);
  }

  // spot light
  {
    size_t spotlightCount = sceneFile["spot_lights"].size();
    SpotLight::spotLightData.reserve(spotlightCount);
    _spotLights.reserve(spotlightCount);
    for (int i = 0; i < spotlightCount; i++) {
      float posx =
          sceneFile["spot_lights"][i]["position_x"].template get<float>();
      float posy =
          sceneFile["spot_lights"][i]["position_y"].template get<float>();
      float posz =
          sceneFile["spot_lights"][i]["position_z"].template get<float>();
      float color_r =
          sceneFile["spot_lights"][i]["color_r"].template get<float>();
      float color_g =
          sceneFile["spot_lights"][i]["color_g"].template get<float>();
      float color_b =
          sceneFile["spot_lights"][i]["color_b"].template get<float>();
      float coneRad =
          sceneFile["spot_lights"][i]["coneRad"].template get<float>();
      float height =
          sceneFile["spot_lights"][i]["height"].template get<float>();
      float direction_x =
          sceneFile["spot_lights"][i]["direction_x"].template get<float>();
      float direction_y =
          sceneFile["spot_lights"][i]["direction_y"].template get<float>();
      float direction_z =
          sceneFile["spot_lights"][i]["direction_z"].template get<float>();
      uint32_t flags =
          sceneFile["spot_lights"][i]["for_transparent"].template get<bool>()
              ? LIGHT_FOR_TRANSPARENT_FLAG
              : 0;

      vec4 boundingSphere;
      if (coneRad > M_PI_F / 4.0f) {
        float R = height * tanf(coneRad);
        boundingSphere =
            vec4(vec3(posx, posy, posz) +
                     vec3(direction_x, direction_y, direction_z) * height,
                 R);
      } else {
        float R = height / (2 * cos(coneRad) * cos(coneRad));
        boundingSphere =
            vec4(vec3(posx, posy, posz) +
                     vec3(direction_x, direction_y, direction_z) * R,
                 R);
      }
      SpotLight::spotLightData.emplace_back(SpotLightData(
          boundingSphere, vec4(posx, posy, posz, height),
          vec4(color_r, color_g, color_b, coneRad * SPOT_LIGHT_INNER_SCALE),
          vec4(direction_x, direction_y, direction_z, coneRad), flags));
      _spotLights.emplace_back(SpotLight(i * sizeof(SpotLightData),
                                         &SpotLight::spotLightData.back()));

      // Compute view-proj matrix for spot shadow rendering.
      // Upload format matches CSM: transpose(P) * transpose(V), so the HLSL
      // shader can do mul(spotViewProjMatrices[idx], worldPos) to get clip space.
      {
        vec3 spotDir = normalize(vec3(direction_x, direction_y, direction_z));
        vec3 up = (fabsf(spotDir.y) < 0.99f) ? vec3(0.f, 1.f, 0.f) : vec3(0.f, 0.f, 1.f);
        mat4 spotV = invLookAt(vec3(posx, posy, posz), up, spotDir);
        mat4 spotP = perspective(2.0f * coneRad, 1.0f, 0.1f, height);
        SpotLight::spotLightData.back().viewProjMatrix = transpose(spotP) * transpose(spotV);
      }
    }
  }

  // Initialize LightCuller and ScatterVolume after scene loading so that
  // point/spot light buffers are sized correctly (needs non-zero light counts).
  if (!_lightCuller) {
    _lightCuller = new LightCuller();
    _lightCuller->InitRHI(device, *this, device.getSwapChainExtent().width,
                          device.getSwapChainExtent().height);
  }
  createScatterVolume();
}

void GpuScene::CreateForwardLightingPass() {

  VkAttachmentDescription deferredLightingAttachments = {};

  deferredLightingAttachments.format = VK_FORMAT_R16G16B16A16_SFLOAT;
  deferredLightingAttachments.samples = VK_SAMPLE_COUNT_1_BIT;
  deferredLightingAttachments.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
  deferredLightingAttachments.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  deferredLightingAttachments.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  deferredLightingAttachments.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  deferredLightingAttachments.initialLayout =
      VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  deferredLightingAttachments.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

  VkAttachmentDescription deferredLightingDepthAttachments = {};

  deferredLightingDepthAttachments.format = device.getWindowDepthFormat();
  deferredLightingDepthAttachments.samples = VK_SAMPLE_COUNT_1_BIT;
  deferredLightingDepthAttachments.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
  deferredLightingDepthAttachments.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  deferredLightingDepthAttachments.stencilLoadOp =
      VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  deferredLightingDepthAttachments.stencilStoreOp =
      VK_ATTACHMENT_STORE_OP_DONT_CARE;
  deferredLightingDepthAttachments.initialLayout =
      VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
  // Resolve pass samples depth as DEPTH_READ_ONLY; let the render pass perform
  // the implicit attachment->read-only transition here so we don't need a
  // standalone vkCmdPipelineBarrier between forward and resolve.
  deferredLightingDepthAttachments.finalLayout =
      VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL;

  VkAttachmentReference deferredLightingAttachmentRefs{};
  deferredLightingAttachmentRefs.attachment = 0;
  deferredLightingAttachmentRefs.layout =
      VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

  VkAttachmentReference deferredLightingDepthAttachmentRef{};
  deferredLightingDepthAttachmentRef.attachment = 1;
  deferredLightingDepthAttachmentRef.layout =
      VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

  VkSubpassDescription subpass{};
  subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  subpass.colorAttachmentCount = 1;
  subpass.pColorAttachments = &deferredLightingAttachmentRefs;
  subpass.pDepthStencilAttachment = &deferredLightingDepthAttachmentRef;

  VkSubpassDependency dependency{};
  dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
  dependency.dstSubpass = 0;
  dependency.srcAccessMask = 0;
  dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                            VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
  dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                            VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
  dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                             VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

  std::array<VkAttachmentDescription, 2> attachments = {
      deferredLightingAttachments, deferredLightingDepthAttachments};
  VkRenderPassCreateInfo renderPassInfo{};
  renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
  renderPassInfo.attachmentCount = static_cast<uint32_t>(attachments.size());
  renderPassInfo.pAttachments = attachments.data();
  renderPassInfo.subpassCount = 1;
  renderPassInfo.pSubpasses = &subpass;
  renderPassInfo.dependencyCount = 1;
  renderPassInfo.pDependencies = &dependency;

  if (vkCreateRenderPass(device.getLogicalDevice(), &renderPassInfo, nullptr,
                         &_forwardLightingPass) != VK_SUCCESS) {
    throw std::runtime_error("failed to create base pass!");
  }
}

void GpuScene::CreateDeferredLightingPass() {
  VkAttachmentDescription deferredLightingAttachments = {};

  deferredLightingAttachments.format = VK_FORMAT_R16G16B16A16_SFLOAT;
  deferredLightingAttachments.samples = VK_SAMPLE_COUNT_1_BIT;
  deferredLightingAttachments.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  deferredLightingAttachments.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  deferredLightingAttachments.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  deferredLightingAttachments.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  deferredLightingAttachments.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  deferredLightingAttachments.finalLayout =
      VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

  VkAttachmentDescription deferredLightingDepthAttachments = {};

  deferredLightingDepthAttachments.format = device.getWindowDepthFormat();
  deferredLightingDepthAttachments.samples = VK_SAMPLE_COUNT_1_BIT;
  deferredLightingDepthAttachments.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
  deferredLightingDepthAttachments.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  deferredLightingDepthAttachments.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  deferredLightingDepthAttachments.stencilStoreOp =
      VK_ATTACHMENT_STORE_OP_DONT_CARE;
  deferredLightingDepthAttachments.initialLayout =
      VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL;
  deferredLightingDepthAttachments.finalLayout =
      VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

  VkAttachmentReference deferredLightingAttachmentRefs{};
  deferredLightingAttachmentRefs.attachment = 0;
  deferredLightingAttachmentRefs.layout =
      VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

  VkAttachmentReference deferredLightingDepthStencilAttachmentRefs{};
  deferredLightingDepthStencilAttachmentRefs.attachment = 1;
  deferredLightingDepthStencilAttachmentRefs.layout =
      VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL;

  VkSubpassDescription subpass{};
  subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  subpass.colorAttachmentCount = 1;
  subpass.pColorAttachments = &deferredLightingAttachmentRefs;
  subpass.pDepthStencilAttachment = &deferredLightingDepthStencilAttachmentRefs;

  VkSubpassDependency dependency{};
  dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
  dependency.dstSubpass = 0;
  dependency.srcAccessMask = 0;
  dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                            VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
  dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                            VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
  dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                             VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

  std::array<VkAttachmentDescription, 2> attachments = {
      deferredLightingAttachments, deferredLightingDepthAttachments};
  VkRenderPassCreateInfo renderPassInfo{};
  renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
  renderPassInfo.attachmentCount = static_cast<uint32_t>(attachments.size());
  renderPassInfo.pAttachments = attachments.data();
  renderPassInfo.subpassCount = 1;
  renderPassInfo.pSubpasses = &subpass;
  renderPassInfo.dependencyCount = 1;
  renderPassInfo.pDependencies = &dependency;

  if (vkCreateRenderPass(device.getLogicalDevice(), &renderPassInfo, nullptr,
                         &_deferredLightingPass) != VK_SUCCESS) {
    throw std::runtime_error("failed to create base pass!");
  }
}

void GpuScene::CreateDeferredBasePass() {
  VkAttachmentDescription gbufferAttachments[4] = {};
  for (int i = 0; i < 4; i++) {
    gbufferAttachments[i] = {};
    gbufferAttachments[i].format = _gbufferFormat[i];
    gbufferAttachments[i].samples = VK_SAMPLE_COUNT_1_BIT;
    gbufferAttachments[i].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    gbufferAttachments[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    gbufferAttachments[i].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    gbufferAttachments[i].finalLayout =
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  }

  VkAttachmentDescription depthAttachment{};
  depthAttachment.format = device.getWindowDepthFormat();
  depthAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
  depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  depthAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  depthAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  depthAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  depthAttachment.finalLayout =
      VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

  VkAttachmentReference gbufferAttachmentRefs[4] = {};
  for (int i = 0; i < 4; i++) {
    gbufferAttachmentRefs[i].attachment = i;
    gbufferAttachmentRefs[i].layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  }

  VkAttachmentReference depthAttachmentRef{};
  depthAttachmentRef.attachment = 4;
  depthAttachmentRef.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

  VkSubpassDescription subpass{};
  subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  subpass.colorAttachmentCount = 4;
  subpass.pColorAttachments = gbufferAttachmentRefs;
  subpass.pDepthStencilAttachment = &depthAttachmentRef;

  VkSubpassDependency dependencies[2] = {};
  // Entry dependency: external → subpass 0
  dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
  dependencies[0].dstSubpass = 0;
  dependencies[0].srcAccessMask = 0;
  dependencies[0].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                            VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
  dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                            VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
  dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                             VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
  // Exit dependency: subpass 0 → external (GBuffer finalLayout transition → shader read)
  dependencies[1].srcSubpass = 0;
  dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
  dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  dependencies[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
  dependencies[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

  std::array<VkAttachmentDescription, 5> attachments = {
      gbufferAttachments[0], gbufferAttachments[1], gbufferAttachments[2],
      gbufferAttachments[3], depthAttachment};
  VkRenderPassCreateInfo renderPassInfo{};
  renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
  renderPassInfo.attachmentCount = static_cast<uint32_t>(attachments.size());
  renderPassInfo.pAttachments = attachments.data();
  renderPassInfo.subpassCount = 1;
  renderPassInfo.pSubpasses = &subpass;
  renderPassInfo.dependencyCount = 2;
  renderPassInfo.pDependencies = dependencies;

  if (vkCreateRenderPass(device.getLogicalDevice(), &renderPassInfo, nullptr,
                         &_basePass) != VK_SUCCESS) {
    throw std::runtime_error("failed to create base pass!");
  }
}

void GpuScene::CreateOccluderZPass() {
  // no color attachment only depth attachment
  VkAttachmentDescription depthAttachment{};
  depthAttachment.format = VK_FORMAT_D32_SFLOAT;
  depthAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
  depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  depthAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  depthAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  depthAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  depthAttachment.finalLayout =
      VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;

  VkAttachmentReference depthAttachmentRef{};
  depthAttachmentRef.attachment = 0;
  depthAttachmentRef.layout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;

  VkSubpassDescription subpass{};
  subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  subpass.colorAttachmentCount = 0;
  subpass.pColorAttachments = nullptr;
  subpass.pDepthStencilAttachment = &depthAttachmentRef;

  VkSubpassDependency dependency{};
  dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
  dependency.dstSubpass = 0;
  dependency.srcAccessMask = 0;
  dependency.srcStageMask =
      VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT; // TODO: is dependency mask
                                                  // right?
  dependency.dstStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
  dependency.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

  std::array<VkAttachmentDescription, 1> attachments = {depthAttachment};
  VkRenderPassCreateInfo renderPassInfo{};
  renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
  renderPassInfo.attachmentCount = static_cast<uint32_t>(attachments.size());
  renderPassInfo.pAttachments = attachments.data();
  renderPassInfo.subpassCount = 1;
  renderPassInfo.pSubpasses = &subpass;
  renderPassInfo.dependencyCount = 1;
  renderPassInfo.pDependencies = &dependency;

  if (vkCreateRenderPass(device.getLogicalDevice(), &renderPassInfo, nullptr,
                         &occluderZPass) != VK_SUCCESS) {
    throw std::runtime_error("failed to create render pass!");
  }
}

void GpuScene::CreateForwardLightingFrameBuffer(uint32_t count) {
  _forwardFrameBuffer.resize(count);
  for (int i = 0; i < count; i++) {
    std::array<VkImageView, 2> attachments = {_hdrLightingBufferView,
                                              device.getWindowDepthImageView(i)};

    VkFramebufferCreateInfo framebufferInfo{};
    framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    framebufferInfo.renderPass = _forwardLightingPass;
    framebufferInfo.attachmentCount = static_cast<uint32_t>(attachments.size());
    ;
    framebufferInfo.pAttachments = attachments.data();
    framebufferInfo.width = device.getSwapChainExtent().width;
    framebufferInfo.height = device.getSwapChainExtent().height;
    framebufferInfo.layers = 1;

    if (vkCreateFramebuffer(device.getLogicalDevice(), &framebufferInfo,
                            nullptr, &_forwardFrameBuffer[i]) != VK_SUCCESS) {
      throw std::runtime_error(
          "failed to create deferred lighting framebuffer!");
    }
  }
}

void GpuScene::CreateDeferredLightingFrameBuffer(uint32_t count) {
  _deferredFrameBuffer.resize(count);
  for (int i = 0; i < count; i++) {
    std::array<VkImageView, 2> attachments = {_hdrLightingBufferView,
                                              device.getWindowDepthImageView(i)};

    VkFramebufferCreateInfo framebufferInfo{};
    framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    framebufferInfo.renderPass = _deferredLightingPass;
    framebufferInfo.attachmentCount = static_cast<uint32_t>(attachments.size());
    ;
    framebufferInfo.pAttachments = attachments.data();
    framebufferInfo.width = device.getSwapChainExtent().width;
    framebufferInfo.height = device.getSwapChainExtent().height;
    framebufferInfo.layers = 1;

    if (vkCreateFramebuffer(device.getLogicalDevice(), &framebufferInfo,
                            nullptr, &_deferredFrameBuffer[i]) != VK_SUCCESS) {
      throw std::runtime_error(
          "failed to create deferred lighting framebuffer!");
    }
  }
}

void GpuScene::CreateBasePassFrameBuffer() {

  _basePassFrameBuffer.resize(framesInFlight);
  for (uint32_t f = 0; f < framesInFlight; ++f) {
  std::array<VkImageView, 5> attachments = {_gbuffersView[0][f], _gbuffersView[1][f],
                                            _gbuffersView[2][f], _gbuffersView[3][f],
                                            device.getWindowDepthImageView(f)};

  VkFramebufferCreateInfo framebufferInfo{};
  framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
  framebufferInfo.renderPass = _basePass;
  framebufferInfo.attachmentCount = static_cast<uint32_t>(attachments.size());
  framebufferInfo.pAttachments = attachments.data();
  framebufferInfo.width = device.getSwapChainExtent().width;
  framebufferInfo.height = device.getSwapChainExtent().height;
  framebufferInfo.layers = 1;

  if (vkCreateFramebuffer(device.getLogicalDevice(), &framebufferInfo, nullptr,
                          &_basePassFrameBuffer[f]) != VK_SUCCESS) {
    throw std::runtime_error("failed to create basepass framebuffer!");
  }
  }
}

void GpuScene::CreateOccluderZPassFrameBuffer() {
  std::array<VkImageView, 1> attachments = {_depthTextureView};

  VkFramebufferCreateInfo framebufferInfo{};
  framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
  framebufferInfo.renderPass = occluderZPass;
  framebufferInfo.attachmentCount = static_cast<uint32_t>(attachments.size());
  ;
  framebufferInfo.pAttachments = attachments.data();
  framebufferInfo.width = device.getSwapChainExtent().width;
  framebufferInfo.height = device.getSwapChainExtent().height;
  framebufferInfo.layers = 1;

  if (vkCreateFramebuffer(device.getLogicalDevice(), &framebufferInfo, nullptr,
                          &_depthFrameBuffer) != VK_SUCCESS) {
    throw std::runtime_error("failed to create z framebuffer!");
  }
}

void GpuScene::CreateZdepthView() {
  VkImageViewCreateInfo createInfo{};
  createInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  createInfo.image = _depthTexture;
  createInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
  createInfo.format = VK_FORMAT_D32_SFLOAT;
  createInfo.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
  createInfo.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
  createInfo.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
  createInfo.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
  createInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
  createInfo.subresourceRange.baseMipLevel = 0;
  createInfo.subresourceRange.levelCount = 1;
  createInfo.subresourceRange.baseArrayLayer = 0;
  createInfo.subresourceRange.layerCount = 1;

  if (vkCreateImageView(device.getLogicalDevice(), &createInfo, nullptr,
                        &_depthTextureView) != VK_SUCCESS) {
    throw std::runtime_error("failed to create z image views!");
  }
}

void GpuScene::DrawOccluders(VkCommandBuffer commandBuffer) {
  VkRenderPassBeginInfo renderPassInfo{};
  renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
  renderPassInfo.renderPass = occluderZPass;
  renderPassInfo.framebuffer = _depthFrameBuffer;
  renderPassInfo.renderArea.offset = {0, 0};
  renderPassInfo.renderArea.extent = device.getSwapChainExtent();

  std::array<VkClearValue, 1> clearValues{};

  clearValues[0].depthStencil = {0.0f, 0};
  renderPassInfo.clearValueCount = static_cast<uint32_t>(clearValues.size());
  renderPassInfo.pClearValues = clearValues.data();

  vkCmdBeginRenderPass(
      commandBuffer, &renderPassInfo,
      VK_SUBPASS_CONTENTS_INLINE); // TODO: seperate commandbuffer?

  VkDeviceSize offsets[] = {0};
  VkBuffer vertexBuffers[] = {_occludersVertBuffer};
  vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                    drawOccluderPipeline);
  vkCmdBindVertexBuffers(commandBuffer, 0, 1, vertexBuffers, offsets);
  vkCmdBindIndexBuffer(commandBuffer, _occludersIndexBuffer, 0,
                       VK_INDEX_TYPE_UINT32);

  // mat4 objtocamera = transpose(maincamera->getObjectToCamera());

  //uint32_t dynamic_offset = sizeof(mat4) * 2 * SHADOW_CASCADE_COUNT;

  vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          pipelineLayout, 0, 1, &globalDescriptorSets[currentFrame], 0,
                          nullptr);
  vkCmdDrawIndexed(commandBuffer, sceneFile["occluder_indices"].size(), 1, 0, 0,
                   0);
  vkCmdEndRenderPass(commandBuffer);
}

void GpuScene::recordCommandBuffer(int imageIndex, VkCommandBuffer commandBuffer) {
  // 注意：commandBuffer 参数会遮蔽任何同名的成员变量，
  // 这样函数内部所有对 commandBuffer 的引用都会使用这个参数

  // If streaming swaps happened, update the descriptor set for currentFrame.
  // Safe here because Draw() has already waited on all fences.
  if (streamingDescriptorsDirtyMask & (1u << currentFrame)) {
    std::vector<VkDescriptorImageInfo> imageInfos;
    imageInfos.reserve(textures.size());
    for (size_t i = 0; i < textures.size(); ++i) {
      VkDescriptorImageInfo dii{};
      dii.imageView = textures[i].second;
      dii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
      imageInfos.push_back(dii);
    }
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = applDescriptorSets[currentFrame];
    write.dstBinding = 2;
    write.descriptorCount = static_cast<uint32_t>(imageInfos.size());
    write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    write.pImageInfo = imageInfos.data();
    vkUpdateDescriptorSets(device.getLogicalDevice(), 1, &write, 0, nullptr);
    streamingDescriptorsDirtyMask &= ~(1u << currentFrame);
  }

  _shadow->UpdateShadowMatrices(*this);
  {
    frameConstants.nearPlane = maincamera->Near();
    frameConstants.farPlane = maincamera->Far();
    static uint32_t sFrameCounter = 0;
    frameConstants.frameCounter = sFrameCounter++;
    // Accumulate wind offset for Perlin-noise fog animation (~0.5 m/s horizontal wind)
    float t = float(frameConstants.frameCounter) * 0.016f;
    frameConstants.globalNoiseOffset = vec3(t * 0.5f, 0.0f, t * 0.3f);
    frameConstants.physicalSize = vec2(device.getSwapChainExtent().width,
                                       device.getSwapChainExtent().height);
    frameConstants.invPhysicalSize = vec2(1.0f / frameConstants.physicalSize.x,
                                          1.0f / frameConstants.physicalSize.y);

    // TAA: compute Halton jitter (only applied when enabled).
    auto halton = [](uint32_t index, uint32_t base) -> float {
      float result = 0.0f, f = 1.0f;
      while (index > 0) { f /= base; result += (index % base) * f; index /= base; }
      return result;
    };
    float jx = 0.0f, jy = 0.0f;
    if (_taaEnabled) {
      const uint32_t TAA_JITTER_COUNT = 8;
      uint32_t jitterIndex = (_taaFrameIndex % TAA_JITTER_COUNT) + 1;
      jx = halton(jitterIndex, 2) * 2.0f - 1.0f;
      jy = halton(jitterIndex, 3) * 2.0f - 1.0f;
      jx /= (float)device.getSwapChainExtent().width;
      jy /= (float)device.getSwapChainExtent().height;
    }
    frameConstants.taaJitter = vec2(jx, jy);
    frameConstants.exposure = 1.0f;
    // TAA reads the previous frame's history; on the first TAA-enabled frame
    // (or when freshly toggled on) skip blending so we don't sample garbage.
    frameConstants.taaEnabled = (_taaEnabled && !_taaFirstFrame) ? 1 : 0;

    // Clean (un-jittered) matrices for reprojection and inverse calculations.
    mat4 cleanProj = maincamera->getProjectMatrix();
    mat4 cleanView = maincamera->getObjectToCamera();
    mat4 cleanInvView = maincamera->getInvViewMatrix();
    mat4 cleanInvViewProj = maincamera->getInvViewProjectionMatrix();
    mat4 cleanInvProj = inverse(cleanProj);

    // Apply sub-pixel jitter to the projection used for vertex shading.
    mat4 jitteredProj = cleanProj;
    if (_taaEnabled) {
      // P[3][2] (m[2].w) scales z into w; multiplying the jitter by it
      // gives the exact sub-pixel NDC offset regardless of depth.
      float Zt = cleanProj[2].w;
      jitteredProj[2][0] += jx * Zt;
      jitteredProj[2][1] += jy * Zt;
    }

    void *data1;
    vkMapMemory(device.getLogicalDevice(), uniformBufferMemories[currentFrame], 0,
                sizeof(FrameData), 0, &data1);
    for (int i = 0; i < SHADOW_CASCADE_COUNT; ++i) {
      memcpy(data1,
             transpose(_shadow->_shadowProjectionMatrices[i]).value_ptr(),
             (size_t)sizeof(mat4));
      data1 = ((mat4 *)data1) + 1;
      memcpy(data1, transpose(_shadow->_shadowViewMatrices[i]).value_ptr(),
             (size_t)sizeof(mat4));
      data1 = ((mat4 *)data1) + 1;
    }
    // Upload jittered projection (for vertex shading sub-pixel offset)
    memcpy(data1, transpose(jitteredProj).value_ptr(),
           (size_t)sizeof(mat4));
    data1 = ((mat4 *)data1) + 1;
    memcpy(data1, transpose(cleanView).value_ptr(),
           (size_t)sizeof(mat4));
    data1 = ((mat4 *)data1) + 1;
    memcpy(data1, transpose(cleanInvView).value_ptr(),
           (size_t)sizeof(mat4));
    data1 = ((mat4 *)data1) + 1;
    // Use jittered inverse VP so world position reconstruction is consistent
    // with the depth buffer (which was rendered with jitteredProj). Using the
    // clean invVP with jittered depth gives a per-frame-varying world position
    // for angled surfaces, which propagates to the shadow UV and causes flicker.
    mat4 jitteredInvViewProj = inverse(cleanView * jitteredProj);
    memcpy(data1, transpose(jitteredInvViewProj).value_ptr(),
           (size_t)sizeof(mat4));
    data1 = ((mat4 *)data1) + 1;
    memcpy(data1, transpose(cleanInvProj).value_ptr(), (size_t)sizeof(mat4));
    data1 = ((mat4 *)data1) + 1;
    // prevViewProjectionMatrix
    memcpy(data1, transpose(_prevViewProjectionMatrix).value_ptr(), (size_t)sizeof(mat4));
    data1 = ((mat4 *)data1) + 1;
    memcpy(data1, &frameConstants, sizeof(FrameConstants));
    vkUnmapMemory(device.getLogicalDevice(), uniformBufferMemories[currentFrame]);

    // Store current jittered VP for next frame's reprojection.
    // Code `view * proj` evaluates to math P*V (see mat4 quirk / Camera.cpp:74).
    // Use jittered projection so that the history lookup compensates for the
    // previous frame's sub-pixel offset, giving sharper temporal accumulation.
    _prevViewProjectionMatrix = cleanView * jitteredProj;
    if (_taaEnabled) {
      _taaFirstFrame = false;
      _taaFrameIndex++;
    } else {
      // Reset so re-enabling TAA does not blend against stale history.
      _taaFirstFrame = true;
      _taaFrameIndex = 0;
    }

  // --- Lazy init of LightCuller and RayTracing on first frame ---
  if (!_lightCuller) {
    _lightCuller = new LightCuller();
    _lightCuller->InitRHI(device, *this, device.getSwapChainExtent().width,
                          device.getSwapChainExtent().height);
  }
  if (!_raytracing) {
    _raytracing = new RayTracing(const_cast<VulkanDevice &>(device), *this);
    _raytracing->Init();
    _raytracing->BuildAccelerationStructures();
    _raytracing->CreateOutputImagesAndDescriptorSet();
    _raytracing->CreatePipelineAndSBT();
  }

    {
  uint32_t opaqueCount = applMesh->_opaqueChunkCount;
  uint32_t alphaMaskedCount = applMesh->_alphaMaskedChunkCount;
  uint32_t cascadeMaxChunks = opaqueCount + alphaMaskedCount;
  uint32_t cascadeCount = SHADOW_CASCADE_COUNT;
    // Upload shadow cull params
    {
      void *data;
      vkMapMemory(device.getLogicalDevice(), _shadow->_shadowCullParamsMemories[currentFrame], 0,
                  sizeof(Shadow::ShadowCullParams), 0, &data);
      char *p = (char *)data;
      memcpy(p + 0,  &opaqueCount,      sizeof(uint32_t));
      memcpy(p + 4,  &alphaMaskedCount, sizeof(uint32_t));
      memcpy(p + 8,  &cascadeMaxChunks, sizeof(uint32_t));
      memcpy(p + 12, &cascadeCount,     sizeof(uint32_t));
      // Per-cascade min boundingSphere radius — drop chunks whose sphere is
      // smaller than ~1.5 shadow texels, so far cascades skip tiny props.
      for (int i = 0; i < SHADOW_CASCADE_COUNT; ++i) {
        float texelWS = _shadow->_cascadeSphereRadii[i] * 2.0f
                        / (float)_shadow->_shadowResolution;
        vec4 threshold(texelWS * 1.5f, 0.f, 0.f, 0.f);
        memcpy(p + 16 + i * (int)sizeof(vec4), &threshold, sizeof(vec4));
      }
      memcpy(p + 16 + SHADOW_CASCADE_COUNT * (int)sizeof(vec4),
             _shadow->_cascadeFrustums.data(),
             sizeof(Frustum) * SHADOW_CASCADE_COUNT);
      vkUnmapMemory(device.getLogicalDevice(), _shadow->_shadowCullParamsMemories[currentFrame]);
    }

    // Reset write counters for this cascade
    for (int cascade = 0; cascade < SHADOW_CASCADE_COUNT; ++cascade)
    {
      void *data;
      vkMapMemory(device.getLogicalDevice(), _shadow->_shadowWriteIndexMemories[currentFrame],
                  cascade * 2 * sizeof(uint32_t), 2 * sizeof(uint32_t), 0, &data);
      uint32_t zeros[2] = {0, 0};
      memcpy(data, zeros, 2 * sizeof(uint32_t));
      vkUnmapMemory(device.getLogicalDevice(), _shadow->_shadowWriteIndexMemories[currentFrame]);
    }
    }

    // spdlog::info("{} {}", sizeof(gpuCullParams), offsetof(gpuCullParams,
    // frustum));
    {
      uint32_t totalPointLights = _pointLights.size();
      uint32_t totalSpotLights = _spotLights.size();

      // Compute view-projection matrix for Hi-Z AABB projection.
      // Must match the jittered VP used by the vertex shader; otherwise
      // chunks near frustum edges flicker in/out of cull when jitter
      // shifts the projection between frames.
      mat4 viewProj =
          maincamera->getObjectToCamera() * jitteredProj;
      mat4 viewProjT = transpose(viewProj);

      GPUCullParams params;
      params.opaqueChunkCount = applMesh->_opaqueChunkCount;
      params.alphaMaskedChunkCount = applMesh->_alphaMaskedChunkCount;
      params.transparentChunkCount = applMesh->_transparentChunkCount;
      params.totalPointLights = totalPointLights;
      params.totalSpotLights = totalSpotLights;
      params.hizMipLevels = _hizMipLevels;
      params.screenWidth = (float)device.getSwapChainExtent().width;
      params.screenHeight = (float)device.getSwapChainExtent().height;
      memcpy(params.viewProjMatrix, viewProjT.value_ptr(), sizeof(mat4));
      params.frustum = maincamera->getFrustum();

      vkMapMemory(device.getLogicalDevice(), cullParamsBufferMemories[currentFrame], 0,
                  sizeof(GPUCullParams), 0, &data1);
      memcpy(data1, &params, sizeof(GPUCullParams));
      vkUnmapMemory(device.getLogicalDevice(), cullParamsBufferMemories[currentFrame]);
    }

    // Reset all 3 write counters (opaque, alphaMask, transparent)
    uint32_t zeroCounters[3] = {0, 0, 0};
    vkMapMemory(device.getLogicalDevice(), writeIndexBufferMemories[currentFrame], 0,
                3 * sizeof(uint32_t), 0, &data1);
    memcpy(data1, zeroCounters, 3 * sizeof(uint32_t));
    vkUnmapMemory(device.getLogicalDevice(), writeIndexBufferMemories[currentFrame]);
  }

  VkCommandBufferBeginInfo beginInfo{};
  beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;

  if (vkBeginCommandBuffer(commandBuffer, &beginInfo) != VK_SUCCESS) {
    throw std::runtime_error("failed to begin recording command buffer!");
  }

  // --- Fast RT path: skip the entire raster pipeline ---
  if (useRayTracing) {
    VkExtent2D extent = device.getSwapChainExtent();
    _raytracing->RecordTraceRays(commandBuffer, (uint32_t)imageIndex, extent);
    _raytracing->RecordBlitToSwapchain(commandBuffer, (uint32_t)imageIndex);
    _raytracing->BeginImGuiCompositePass(commandBuffer, (uint32_t)imageIndex, extent);
    renderImGuiOverlay(commandBuffer, (uint32_t)imageIndex);
    _raytracing->EndImGuiCompositePass(commandBuffer);
    if (vkEndCommandBuffer(commandBuffer) != VK_SUCCESS) {
      throw std::runtime_error("failed to record command buffer!");
    }
    return;
  }

  {_shadow->RenderShadowMap(commandBuffer, *this, device); }
  {_shadow->RenderSpotShadowMaps(commandBuffer, *this, device); }

  // Scatter volume: runs after shadow maps, before GBuffer (needs shadow map + camera UBO)
  _scatterVolume.dispatch(commandBuffer, currentFrame);

  // Draw occluders first for Hi-Z generation
  DrawOccluders(commandBuffer);

  // Stage 3: Generate Hi-Z pyramid from occluder depth
  generateHiZPyramid(commandBuffer);

  // GPU Frustum + Hi-Z Culling (compute shader)
  uint32_t totalChunks = applMesh->_opaqueChunkCount + applMesh->_alphaMaskedChunkCount + applMesh->_transparentChunkCount;
  uint32_t groupx = (totalChunks + 127) / 128;
  vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                    encodeDrawBufferPipeline);
  vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                          encodeDrawBufferPipelineLayout, 0, 1,
                          &gpuCullDescriptorSets[currentFrame], 0, nullptr);
  vkCmdDispatch(commandBuffer, groupx, 1, 1);

  VkMemoryBarrier2 memoryBarrier = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
      .pNext = nullptr,
      .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT_KHR,
      .srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT_KHR,
      .dstStageMask = VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT_KHR |
                      VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT_KHR,
      .dstAccessMask = VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT_KHR |
                       VK_ACCESS_2_SHADER_READ_BIT_KHR};

  VkDependencyInfo dependencyInfo = {
      .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
      .pNext = nullptr,
      .memoryBarrierCount = 1,
      .pMemoryBarriers = &memoryBarrier,
  };

  vkCmdPipelineBarrier2(commandBuffer, &dependencyInfo);


  {
    VkRenderPassBeginInfo renderPassInfo{};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    renderPassInfo.renderPass = _basePass; // device.getMainRenderPass();
    renderPassInfo.framebuffer =
        _basePassFrameBuffer[currentFrame]; // device.getSwapChainFrameBuffer(imageIndex);
    renderPassInfo.renderArea.offset = {0, 0};
    renderPassInfo.renderArea.extent = device.getSwapChainExtent();

    std::array<VkClearValue, 5> clearValues{};
    clearValues[0].color = {{0.0f, 0.0f, 0.0f, 1.0f}};
    clearValues[1].color = {{0.0f, 0.0f, 0.0f, 1.0f}};
    clearValues[2].color = {{0.0f, 0.0f, 0.0f, 1.0f}};
    clearValues[3].color = {{0.0f, 0.0f, 0.0f, 1.0f}};
    clearValues[4].depthStencil = {0.0f, 0};
    renderPassInfo.clearValueCount = static_cast<uint32_t>(clearValues.size());
    renderPassInfo.pClearValues = clearValues.data();

    vkCmdBeginRenderPass(commandBuffer, &renderPassInfo,
                         VK_SUBPASS_CONTENTS_INLINE);

    // vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
    // graphicsPipeline); vkCmdBindDescriptorSets(commandBuffer,
    // VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout, 0, 1, &globalDescriptorSet,
    // 0, nullptr); vkCmdDraw(commandBuffer, 6, 1, 0, 0);

    DrawChunksBasePass(commandBuffer);

    vkCmdEndRenderPass(commandBuffer);
  }

  // Screen-space decal pass: modify GBuffer before deferred lighting.
  // drawDecals() owns the depth layout transition (only fires if _decals
  // is non-empty), so callers don't pay for it when there are no decals.
  drawDecals(commandBuffer, (uint32_t)imageIndex);

  // Ensure depth is in DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL for ALL
  // downstream passes. Use imageIndex (swapchain-scoped) to match the depth
  // image that the deferred-lighting framebuffer is bound to.
  if (_decals.empty()) {
    transitionImageLayout(
        device.getWindowDepthImage((uint32_t)imageIndex), device.getWindowDepthFormat(),
        VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL, commandBuffer);
  }

  {
    if (useClusterLighting) {
      if (!_lightCuller) {
        _lightCuller = new LightCuller();
        _lightCuller->InitRHI(device, *this, device.getSwapChainExtent().width,
                              device.getSwapChainExtent().height);
      }
      transitionImageLayout(_lightCuller->GetXZDebugImage(), VK_FORMAT_R32_UINT,
                            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, commandBuffer);
      transitionImageLayout(_lightCuller->GetTraditionalDebugImage(),
                            VK_FORMAT_R32G32B32A32_SFLOAT,
                            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, commandBuffer);

      _lightCuller->ClusterLightForScreen(commandBuffer, device, *this,
                                          device.getSwapChainExtent().width,
                                          device.getSwapChainExtent().height);
    }
  }

  {
    // GBuffer layout transition is now handled by render pass finalLayout
    // (SHADER_READ_ONLY_OPTIMAL) + exit subpass dependency
    // Shadow map layout transition is handled by shadow render pass finalLayout
    // Depth was already transitioned to READ_ONLY_STENCIL_ATTACHMENT above
    // (either by drawDecals or the conditional transition).

    if (useClusterLighting) {
      // Memory barrier between cluster-lighting compute shader (writes light
      // indices SSBO) and deferred lighting fragment shader (reads it).
      // No image layout change needed — pure memory/execution sync.
      VkMemoryBarrier memBarrier{};
      memBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
      memBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
      memBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
      vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                           VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
                           1, &memBarrier, 0, nullptr, 0, nullptr);
    }
  }

  // SAO: generate depth pyramid from full scene depth, then dispatch SAO compute
  {
    // Window depth is already in DEPTH_READ_ONLY layout from above transition
    generateSAODepthPyramid(commandBuffer);
    dispatchSAO(commandBuffer);
  }

  // deferred lighting pass
  {
    if (useClusterLighting) {
      // wait for the lightindices to read
      VkMemoryBarrier2 memoryBarrier = {
          .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
          .pNext = nullptr,
          .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT_KHR,
          .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
          .dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
          .dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT_KHR};

      VkDependencyInfo dependencyInfo = {
          .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
          .pNext = nullptr,
          .memoryBarrierCount = 1,
          .pMemoryBarriers = &memoryBarrier,
      };

      vkCmdPipelineBarrier2(commandBuffer, &dependencyInfo);
    }
    uint32_t dynamic_offset = 0;
    std::array<VkClearValue, 2> clearValues{};
    clearValues[0].color = {{0.0f, 0.0f, 0.0f, 1.0f}};
    clearValues[1].depthStencil.depth = 0;
    clearValues[1].depthStencil.stencil = 0;
    // don't clear depth stencil

    VkRenderPassBeginInfo blitPassInfo{};
    blitPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    blitPassInfo.renderPass = _deferredLightingPass;
    blitPassInfo.framebuffer = _deferredFrameBuffer[imageIndex];
    blitPassInfo.renderArea.offset = {0, 0};
    blitPassInfo.renderArea.extent = device.getSwapChainExtent();
    blitPassInfo.clearValueCount = static_cast<uint32_t>(clearValues.size());
    blitPassInfo.pClearValues = clearValues.data();

    vkCmdBeginRenderPass(commandBuffer, &blitPassInfo,
                         VK_SUBPASS_CONTENTS_INLINE);

    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      useClusterLighting
                          ? deferredLightingPipeline_clusterlighting
                          : deferredLightingPipeline);

    VkDescriptorSet deferredSets[] = {globalDescriptorSets[currentFrame], deferredLightingDescriptorSet[currentFrame]};
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            deferredLightingPipelineLayout, 0, 2,
                            deferredSets, 0, nullptr);
    vkCmdDraw(commandBuffer, 3, 1, 0, 0);
    if (!useClusterLighting) {
      // point light
      PointLight::CommonDrawSetup(commandBuffer);
      for (auto &pl : _pointLights) {
        if (maincamera->getFrustum().FrustumCull(pl.getPointLightData()))
          continue;
        pl.Draw(commandBuffer, *this);
      }
    }

    vkCmdEndRenderPass(commandBuffer);

    // forward pass
    {

      dynamic_offset = sizeof(mat4) * 2 * SHADOW_CASCADE_COUNT;

      VkRenderPassBeginInfo forwardPassInfo{};
      forwardPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
      forwardPassInfo.renderPass = _forwardLightingPass;
      forwardPassInfo.framebuffer = _forwardFrameBuffer[imageIndex];
      forwardPassInfo.renderArea.offset = {0, 0};
      forwardPassInfo.renderArea.extent = device.getSwapChainExtent();
      forwardPassInfo.clearValueCount =
          0; // static_cast<uint32_t>(clearValues.size());
      // forwardPassInfo.pClearValues = clearValues.data();
      VkBuffer vertexBuffers[] = {applVertexBuffer, applNormalBuffer,
                                  applTangentBuffer, applUVBuffer};
      VkDeviceSize offsets[] = {0, 0, 0, 0};

      vkCmdBindVertexBuffers(commandBuffer, 0,
                             sizeof(vertexBuffers) / sizeof(vertexBuffers[0]),
                             vertexBuffers, offsets);
      vkCmdBindIndexBuffer(commandBuffer, applIndexBuffer, 0,
                           VK_INDEX_TYPE_UINT32);

      vkCmdBeginRenderPass(commandBuffer, &forwardPassInfo,
                           VK_SUBPASS_CONTENTS_INLINE);

      // Stage 4: GPU indirect forward pass for transparent objects
      vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                        drawclusterForwardPipelineIndirect);
      VkDescriptorSet forwardDescriptorSets[] = {globalDescriptorSets[currentFrame],applDescriptorSets[currentFrame]};
      vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                              drawclusterBasePipelineLayout, 0, 2,
                              forwardDescriptorSets, 0, nullptr);

      uint32_t transparentOffset = (applMesh->_opaqueChunkCount + applMesh->_alphaMaskedChunkCount)
                                   * sizeof(VkDrawIndexedIndirectCommand);
      vkCmdDrawIndexedIndirectCount(commandBuffer, drawParamsBuffers[currentFrame],
                                    transparentOffset,
                                    writeIndexBuffers[currentFrame], 2 * sizeof(uint32_t),
                                    applMesh->_transparentChunkCount,
                                    sizeof(VkDrawIndexedIndirectCommand));

      // Debug: draw occluder wireframe overlay
      drawOccludersWireframe(commandBuffer);

      // ImGui overlay (rendered last in forward pass)
      renderImGuiOverlay(commandBuffer, imageIndex);

      vkCmdEndRenderPass(commandBuffer);
    }
  }

  // post process - Resolve pass (TAA + ACES Tone Mapping)
  {
    uint32_t writeSlot = _taaFrameIndex & 1;

    VkRenderPassBeginInfo resolvePassInfo{};
    resolvePassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    resolvePassInfo.renderPass = _resolvePass;
    resolvePassInfo.framebuffer = _resolveFrameBuffer[imageIndex * 2 + writeSlot];
    resolvePassInfo.renderArea.offset = {0, 0};
    resolvePassInfo.renderArea.extent = device.getSwapChainExtent();
    resolvePassInfo.clearValueCount = 0;

    vkCmdBeginRenderPass(commandBuffer, &resolvePassInfo, VK_SUBPASS_CONTENTS_INLINE);

    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, _resolvePipeline);
    VkDescriptorSet resolveSets[] = {globalDescriptorSets[currentFrame],
                                     _resolveDescriptorSets[currentFrame * 2 + writeSlot]};
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            _resolvePipelineLayout, 0, 2, resolveSets, 0, nullptr);
    vkCmdDraw(commandBuffer, 3, 1, 0, 0);

    vkCmdEndRenderPass(commandBuffer);
  }

  if (vkEndCommandBuffer(commandBuffer) != VK_SUCCESS) {
    throw std::runtime_error("failed to record command buffer!");
  }
}



void GpuScene::DrawChunksBasePass(VkCommandBuffer commandBuffer) {

  VkBuffer vertexBuffers[] = {applVertexBuffer, applNormalBuffer,
                              applTangentBuffer, applUVBuffer};
  VkDeviceSize offsets[] = {0, 0, 0, 0};

  vkCmdBindVertexBuffers(commandBuffer, 0,
                         sizeof(vertexBuffers) / sizeof(vertexBuffers[0]),
                         vertexBuffers, offsets);
  vkCmdBindIndexBuffer(commandBuffer, applIndexBuffer, 0, VK_INDEX_TYPE_UINT32);

  // Dynamic offset: skip past shadow cascade matrices to reach camera matrices
  uint32_t dynamic_offset = sizeof(mat4) * 2 * SHADOW_CASCADE_COUNT;
  VkDescriptorSet baseDescriptorSets[] = {globalDescriptorSets[currentFrame],applDescriptorSets[currentFrame]};
  vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          drawclusterBasePipelineLayout, 0, 2,
                          baseDescriptorSets, 0, nullptr);

  // Opaque chunks: indirect draw from region [0, opaqueChunkCount)
  vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                    drawclusterBasePipeline);
  vkCmdDrawIndexedIndirectCount(commandBuffer, drawParamsBuffers[currentFrame], 0,
                                writeIndexBuffers[currentFrame], 0,
                                applMesh->_opaqueChunkCount,
                                sizeof(VkDrawIndexedIndirectCommand));

  // Alpha-masked chunks: indirect draw from region [opaqueChunkCount, opaqueChunkCount + alphaMaskedChunkCount)
  vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                    drawclusterBasePipelineAlphaMask);
  vkCmdDrawIndexedIndirectCount(commandBuffer, drawParamsBuffers[currentFrame],
                                applMesh->_opaqueChunkCount * sizeof(VkDrawIndexedIndirectCommand),
                                writeIndexBuffers[currentFrame], sizeof(uint32_t),
                                applMesh->_alphaMaskedChunkCount,
                                sizeof(VkDrawIndexedIndirectCommand));
}

void GpuScene::DrawChunks(VkCommandBuffer commandBuffer) {
  vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                    drawclusterPipelineAlphaMask);
  VkBuffer vertexBuffers[] = {applVertexBuffer, applNormalBuffer,
                              applTangentBuffer, applUVBuffer};
  VkDeviceSize offsets[] = {0, 0, 0, 0};

  vkCmdBindVertexBuffers(commandBuffer, 0,
                         sizeof(vertexBuffers) / sizeof(vertexBuffers[0]),
                         vertexBuffers, offsets);
  vkCmdBindIndexBuffer(commandBuffer, applIndexBuffer, 0, VK_INDEX_TYPE_UINT32);

  // if the descriptor set data isn't change we can omit this?

  VkDescriptorSet drawSets[] = {globalDescriptorSets[currentFrame], applDescriptorSets[currentFrame]};
  vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          drawclusterPipelineLayout, 0, 2, drawSets,
                          0, nullptr);
  constexpr int beginindex = 0;
  constexpr int indexClamp = 0xffffff;
  uint32_t occluded = 0;

  // static std::vector<bool> debug_frustum_cull(applMesh->_chunkCount,false);
  // static bool captured = false;
#ifdef CPU_DRAW
  for (int i = beginindex; i < applMesh->_chunkCount && i < indexClamp; ++i) {
    PerObjPush perobj = {.matindex = m_Chunks[i].materialIndex};
    vkCmdPushConstants(commandBuffer, drawclusterPipelineLayout,
                       VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(perobj),
                       &perobj);
    // if (captured)
    //{
    //     if (debug_frustum_cull[i])
    //         continue;
    // }
    // else
    {
      if (maincamera->getFrustum().FrustumCull(m_Chunks[i].boundingBox)) {
        // debug_frustum_cull[i] = true;
        ++occluded;
        continue;
      }
    }

    vkCmdDrawIndexed(commandBuffer, m_Chunks[i].indexCount, 1,
                     m_Chunks[i].indexBegin, 0, 0);
  }
  // captured = true;
  spdlog::log(spdlog::level::info, "occlued chunks: {}", occluded);
#else
  vkCmdDrawIndexedIndirectCount(commandBuffer, drawParamsBuffers[currentFrame], 0,
                                writeIndexBuffers[currentFrame], 0, applMesh->_chunkCount,
                                sizeof(VkDrawIndexedIndirectCommand));
#endif
}

void GpuScene::Draw() {
  // begin command buffer record
  // bind graphics pipeline
  // update uniform buffer
  // draw mesh
  // submit commandbuffer

  // 等待当前 sync slot 的 fence 完成（保证 cmd buffer / semaphore / fence 可复用）
  VkResult fenceResult = vkWaitForFences(device.getLogicalDevice(), 1,
                  &inFlightFences[_syncSlot], VK_TRUE, UINT64_MAX);
  if (fenceResult != VK_SUCCESS) {
    spdlog::error("failed to wait for fence! {}", static_cast<int>(fenceResult));
    return;
  }

  // 获取下一个 swapchain image
  uint32_t imageIndex;
  VkResult acquireResult = vkAcquireNextImageKHR(device.getLogicalDevice(), device.getSwapChain(),
                        UINT64_MAX, imageAvailableSemaphores[_syncSlot], VK_NULL_HANDLE,
                        &imageIndex);

  // 处理 swapchain 过期的情况（例如窗口大小改变）
  if (acquireResult == VK_ERROR_OUT_OF_DATE_KHR) {
    spdlog::warn("Swapchain out of date, needs recreation");
    recreateSwapChain();
    return;
  } else if (acquireResult != VK_SUCCESS && acquireResult != VK_SUBOPTIMAL_KHR) {
    spdlog::error("failed to acquire swap chain image! {}", static_cast<int>(acquireResult));
    throw std::runtime_error("failed to acquire swap chain image!");
  }

  // imagesInFlight 模式：同一个 swapchain image 可能上一次是被另一个 sync slot
  // 提交的；它的 GPU 工作可能尚未完成。在我们 CPU 写 per-image 资源（uniform、
  // SSBO、descriptor 引用的 buffer）之前要先等 image 自己的 fence。
  if (imagesInFlight[imageIndex] != VK_NULL_HANDLE) {
    vkWaitForFences(device.getLogicalDevice(), 1,
                    &imagesInFlight[imageIndex], VK_TRUE, UINT64_MAX);
  }
  // 这一帧用 _syncSlot 的 fence 提交工作；记录 image 现在被这个 fence 跟踪。
  imagesInFlight[imageIndex] = inFlightFences[_syncSlot];

  // 只有在成功获取 image 后才 reset fence，避免死锁
  vkResetFences(device.getLogicalDevice(), 1, &inFlightFences[_syncSlot]);

  // 让 record 路径里所有 currentFrame 引用都正确指向这个 swapchain image。
  // 这是本次重构的核心：currentFrame == imageIndex 永远成立，所以
  // _basePassFrameBuffer[currentFrame] / depth[currentFrame] / 各 descriptor set
  // 都和 _deferredFrameBuffer[imageIndex] 用的是同一份资源。
  currentFrame = imageIndex;

  // Readback culling stats from THIS image's PREVIOUS submission
  // (imagesInFlight[imageIndex] fence guaranteed it's done above).
  readbackCullingStats(currentFrame);

  // 使用当前 sync slot 的 command buffer
  VkCommandBuffer& currentCmdBuffer = commandBuffers[_syncSlot];
  vkResetCommandBuffer(currentCmdBuffer, /*VkCommandBufferResetFlagBits*/ 0);

  // Texture streaming: compute required mips and apply completed swaps
  UpdateTextureStreaming(_syncSlot);

  recordCommandBuffer(imageIndex, currentCmdBuffer);

  VkSubmitInfo submitInfo{};
  submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;

  VkSemaphore waitSemaphores[] = {imageAvailableSemaphores[_syncSlot]};
  VkPipelineStageFlags waitStages[] = {
      VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
  submitInfo.waitSemaphoreCount = 1;
  submitInfo.pWaitSemaphores = waitSemaphores;
  submitInfo.pWaitDstStageMask = waitStages;

  submitInfo.commandBufferCount = 1;
  submitInfo.pCommandBuffers = &currentCmdBuffer;

  VkSemaphore signalSemaphores[] = {renderFinishedSemaphores[_syncSlot]};
  submitInfo.signalSemaphoreCount = 1;
  submitInfo.pSignalSemaphores = signalSemaphores;
  VkResult submitResult =
      vkQueueSubmit(device.getGraphicsQueue(), 1, &submitInfo, inFlightFences[_syncSlot]);
  if (submitResult != VK_SUCCESS) {
    spdlog::error("failed to submit draw command buffer! {}", static_cast<int>(submitResult));
    throw std::runtime_error("failed to submit draw command buffer!");
  }

  VkPresentInfoKHR presentInfo{};
  presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;

  presentInfo.waitSemaphoreCount = 1;
  presentInfo.pWaitSemaphores = signalSemaphores;

  VkSwapchainKHR swapChains[] = {device.getSwapChain()};
  presentInfo.swapchainCount = 1;
  presentInfo.pSwapchains = swapChains;

  presentInfo.pImageIndices = &imageIndex;

  VkResult presentResult = vkQueuePresentKHR(device.getPresentQueue(), &presentInfo);

  // 检查呈现结果，处理 swapchain 需要重建的情况
  if (presentResult == VK_ERROR_OUT_OF_DATE_KHR || presentResult == VK_SUBOPTIMAL_KHR || framebufferResized) {
    framebufferResized = false;
    spdlog::warn("Swapchain suboptimal or out of date after present, needs recreation");
    recreateSwapChain();
  } else if (presentResult != VK_SUCCESS) {
    spdlog::error("failed to present swap chain image! {}", static_cast<int>(presentResult));
    throw std::runtime_error("failed to present swap chain image!");
  }

  // 推进 sync slot（独立于 currentFrame，currentFrame 下次 acquire 时会被重置）
  _currentFrameIndex = currentFrame;
  _syncSlot = (_syncSlot + 1) % framesInFlight;
}

void GpuScene::cleanupSwapChainResources() {
  // 清理依赖 swapchain 尺寸的资源
  // 这些资源需要在 swapchain 重建时重新创建

  // 清理 deferred framebuffers
  for (auto framebuffer : _deferredFrameBuffer) {
    vkDestroyFramebuffer(device.getLogicalDevice(), framebuffer, nullptr);
  }
  _deferredFrameBuffer.clear();

  // 清理 forward framebuffers
  for (auto framebuffer : _forwardFrameBuffer) {
    vkDestroyFramebuffer(device.getLogicalDevice(), framebuffer, nullptr);
  }
  _forwardFrameBuffer.clear();

  // 清理 resolve framebuffers
  for (auto framebuffer : _resolveFrameBuffer) {
    vkDestroyFramebuffer(device.getLogicalDevice(), framebuffer, nullptr);
  }
  _resolveFrameBuffer.clear();

  // 清理 HDR lighting buffer
  if (_hdrLightingBufferView != VK_NULL_HANDLE) {
    vkDestroyImageView(device.getLogicalDevice(), _hdrLightingBufferView, nullptr);
    _hdrLightingBufferView = VK_NULL_HANDLE;
  }
  if (_hdrLightingBuffer != VK_NULL_HANDLE) {
    vkDestroyImage(device.getLogicalDevice(), _hdrLightingBuffer, nullptr);
    _hdrLightingBuffer = VK_NULL_HANDLE;
  }
  if (_hdrLightingBufferMemory != VK_NULL_HANDLE) {
    vkFreeMemory(device.getLogicalDevice(), _hdrLightingBufferMemory, nullptr);
    _hdrLightingBufferMemory = VK_NULL_HANDLE;
  }

  // 清理 TAA history buffer
  for (int s = 0; s < 2; ++s) {
    if (_taaHistoryBufferView[s] != VK_NULL_HANDLE) {
      vkDestroyImageView(device.getLogicalDevice(), _taaHistoryBufferView[s], nullptr);
      _taaHistoryBufferView[s] = VK_NULL_HANDLE;
    }
    if (_taaHistoryBuffer[s] != VK_NULL_HANDLE) {
      vkDestroyImage(device.getLogicalDevice(), _taaHistoryBuffer[s], nullptr);
      _taaHistoryBuffer[s] = VK_NULL_HANDLE;
    }
    if (_taaHistoryBufferMemory[s] != VK_NULL_HANDLE) {
      vkFreeMemory(device.getLogicalDevice(), _taaHistoryBufferMemory[s], nullptr);
      _taaHistoryBufferMemory[s] = VK_NULL_HANDLE;
    }
  }

  _taaFirstFrame = true;
  _taaFrameIndex = 0;

  // 清理 GBuffer 资源
  if (_gbufferAlbedoAlphaTextureView != VK_NULL_HANDLE) {
    vkDestroyImageView(device.getLogicalDevice(), _gbufferAlbedoAlphaTextureView, nullptr);
  }
  if (_gbufferNormalsTextureView != VK_NULL_HANDLE) {
    vkDestroyImageView(device.getLogicalDevice(), _gbufferNormalsTextureView, nullptr);
  }
  if (_gbufferEmissiveTextureView != VK_NULL_HANDLE) {
    vkDestroyImageView(device.getLogicalDevice(), _gbufferEmissiveTextureView, nullptr);
  }
  if (_gbufferF0RoughnessTextureView != VK_NULL_HANDLE) {
    vkDestroyImageView(device.getLogicalDevice(), _gbufferF0RoughnessTextureView, nullptr);
  }

  // 清理 decal framebuffers
  for (auto framebuffer : _decalFramebuffers) {
    vkDestroyFramebuffer(device.getLogicalDevice(), framebuffer, nullptr);
  }
  _decalFramebuffers.clear();

  // 注意：如果 GBuffer images 也是独立创建的，也需要在这里销毁
  // vkDestroyImage, vkFreeMemory 等

  // RT path's size-dependent resources (output / accum images + composite
  // framebuffers). Pipeline / SBT / descriptor pool / set layout are kept.
  if (_raytracing && _raytracing->IsBuilt()) {
    _raytracing->destroySizeDependentResources();
  }
}

void GpuScene::recreateSwapChainResources() {
  // 重新创建依赖 swapchain 尺寸的资源
  createHDRLightingBuffer();
  createTAAHistoryBuffer();

  // 重新创建 deferred/forward framebuffers
  CreateDeferredLightingFrameBuffer(device.getSwapChainImageCount());
  CreateForwardLightingFrameBuffer(device.getSwapChainImageCount());
  createResolveFrameBuffer(device.getSwapChainImageCount());

  // Update resolve descriptor sets with new image views
  if (!_resolveDescriptorSets.empty()) {
    VkDescriptorImageInfo hdrInfo{};
    hdrInfo.imageView = _hdrLightingBufferView;
    hdrInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    // Re-point descriptor sets at the (possibly resized) history views.
    const uint32_t setCount = (uint32_t)_resolveDescriptorSets.size();
    for (uint32_t idx = 0; idx < setCount; ++idx) {
      uint32_t f = idx / 2;
      uint32_t w = idx % 2;

      VkDescriptorImageInfo depthInfo{};
      depthInfo.imageView = device.getWindowDepthOnlyImageView(f);
      depthInfo.imageLayout = VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL;

      VkDescriptorImageInfo historyInfoLocal{};
      historyInfoLocal.imageView = _taaHistoryBufferView[1 - w];
      historyInfoLocal.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

      VkWriteDescriptorSet writes[3] = {};
      writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      writes[0].dstSet = _resolveDescriptorSets[idx];
      writes[0].dstBinding = 0;
      writes[0].descriptorCount = 1;
      writes[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
      writes[0].pImageInfo = &hdrInfo;

      writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      writes[1].dstSet = _resolveDescriptorSets[idx];
      writes[1].dstBinding = 1;
      writes[1].descriptorCount = 1;
      writes[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
      writes[1].pImageInfo = &historyInfoLocal;

      writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      writes[2].dstSet = _resolveDescriptorSets[idx];
      writes[2].dstBinding = 2;
      writes[2].descriptorCount = 1;
      writes[2].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
      writes[2].pImageInfo = &depthInfo;

      vkUpdateDescriptorSets(device.getLogicalDevice(), 3, writes, 0, nullptr);
    }
  }

  _taaFirstFrame = true;
  _taaFrameIndex = 0;

  // 重新创建 decal render pass + framebuffers
  if (_decalRenderPass != VK_NULL_HANDLE) {
    vkDestroyRenderPass(device.getLogicalDevice(), _decalRenderPass, nullptr);
    _decalRenderPass = VK_NULL_HANDLE;
    createDecalRenderPass();
  }

  // 更新 camera aspect ratio
  if (maincamera) {
    float aspect = device.getSwapChainExtent().width /
                   static_cast<float>(device.getSwapChainExtent().height);
    // 如果 Camera 类有设置 aspect ratio 的方法，在这里调用
    // maincamera->setAspectRatio(aspect);
  }

  // RT path resize: rebuild output / accum images + composite framebuffers,
  // rewrite size-dependent descriptor bindings, reset accumulation counter.
  if (_raytracing && _raytracing->IsBuilt()) {
    _raytracing->createSizeDependentResources();
  }

  spdlog::info("Swapchain resources recreated: {}x{}",
               device.getSwapChainExtent().width,
               device.getSwapChainExtent().height);
}

void GpuScene::recreateSwapChain() {
  // 等待设备空闲
  vkDeviceWaitIdle(device.getLogicalDevice());

  // 清理旧的 GpuScene swapchain 相关资源
  cleanupSwapChainResources();
  
  // 调用 VulkanDevice 的重建方法（重建 swapchain、image views、depth 资源）
  const_cast<VulkanDevice&>(device).recreateSwapChain();
  
  // 检查 swapchain 图像数量是否变化，如果变化则需要重建同步对象和命令缓冲区
  uint32_t newImageCount = device.getSwapChainImageCount();
  if (newImageCount != framesInFlight) {
    spdlog::info("Swapchain image count changed from {} to {}", framesInFlight, newImageCount);
    
    // 清理旧的同步对象
    for (size_t i = 0; i < framesInFlight; i++) {
      vkDestroySemaphore(device.getLogicalDevice(), imageAvailableSemaphores[i], nullptr);
      vkDestroySemaphore(device.getLogicalDevice(), renderFinishedSemaphores[i], nullptr);
      vkDestroyFence(device.getLogicalDevice(), inFlightFences[i], nullptr);
    }
    
    // 重新创建同步对象（这会更新 framesInFlight）
    createSyncObjects();
    
    // 重新创建命令缓冲区
    createCommandBuffers(device.getCommandPool());

    // 重置 frame 索引以避免越界访问
    _syncSlot = 0;
    currentFrame = 0;
  } else {
    // 即便 image count 没变，imagesInFlight 中存的 fence 句柄也已失效（被
    // cleanupSwapChainResources 间接销毁的资源不包含 fence，但下次 wait 之前
    // 重置一下更安全）。
    std::fill(imagesInFlight.begin(), imagesInFlight.end(), VK_NULL_HANDLE);
  }
  
  // 重新创建 GpuScene 中依赖 swapchain 的资源
  recreateSwapChainResources();

  spdlog::info("Swapchain recreated successfully");
}

void GpuScene::createHiZResources() {
  uint32_t width = device.getSwapChainExtent().width;
  uint32_t height = device.getSwapChainExtent().height;

  // Compute mip levels
  uint32_t bigger = width > height ? width : height;
  _hizMipLevels = static_cast<uint32_t>(floor(log2f(static_cast<float>(bigger)))) + 1;
  _hizWidth = width;
  _hizHeight = height;

  // Create Hi-Z texture (R32_SFLOAT for storage + sampled)
  VkImageCreateInfo imageInfo{};
  imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  imageInfo.imageType = VK_IMAGE_TYPE_2D;
  imageInfo.extent = {width, height, 1};
  imageInfo.mipLevels = _hizMipLevels;
  imageInfo.arrayLayers = 1;
  imageInfo.format = VK_FORMAT_R32_SFLOAT;
  imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
  imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  imageInfo.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
  imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;

  vkCreateImage(device.getLogicalDevice(), &imageInfo, nullptr, &_hizTexture);

  VkMemoryRequirements memReqs;
  vkGetImageMemoryRequirements(device.getLogicalDevice(), _hizTexture, &memReqs);

  VkMemoryAllocateInfo allocInfo{};
  allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  allocInfo.allocationSize = memReqs.size;
  allocInfo.memoryTypeIndex = device.findMemoryType(memReqs.memoryTypeBits,
      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  vkAllocateMemory(device.getLogicalDevice(), &allocInfo, nullptr, &_hizMemory);
  vkBindImageMemory(device.getLogicalDevice(), _hizTexture, _hizMemory, 0);

  // Full mip chain view for sampling in cull shader
  VkImageViewCreateInfo viewInfo{};
  viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  viewInfo.image = _hizTexture;
  viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
  viewInfo.format = VK_FORMAT_R32_SFLOAT;
  viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  viewInfo.subresourceRange.baseMipLevel = 0;
  viewInfo.subresourceRange.levelCount = _hizMipLevels;
  viewInfo.subresourceRange.baseArrayLayer = 0;
  viewInfo.subresourceRange.layerCount = 1;
  vkCreateImageView(device.getLogicalDevice(), &viewInfo, nullptr, &_hizTextureView);

  // Per-mip views for compute shader writes
  _hizMipViews.resize(_hizMipLevels);
  for (uint32_t i = 0; i < _hizMipLevels; ++i) {
    VkImageViewCreateInfo mipViewInfo = viewInfo;
    mipViewInfo.subresourceRange.baseMipLevel = i;
    mipViewInfo.subresourceRange.levelCount = 1;
    vkCreateImageView(device.getLogicalDevice(), &mipViewInfo, nullptr, &_hizMipViews[i]);
  }

  // Nearest clamp sampler for Hi-Z
  VkSamplerCreateInfo samplerInfo{};
  samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
  samplerInfo.magFilter = VK_FILTER_NEAREST;
  samplerInfo.minFilter = VK_FILTER_NEAREST;
  samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
  samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  samplerInfo.minLod = 0.0f;
  samplerInfo.maxLod = static_cast<float>(_hizMipLevels);
  vkCreateSampler(device.getLogicalDevice(), &samplerInfo, nullptr, &_hizSampler);

  // Update all per-frame gpuCullDescriptorSets with Hi-Z bindings
  for (uint32_t f = 0; f < framesInFlight; ++f) {
    VkDescriptorImageInfo hizImageInfo{};
    hizImageInfo.imageView = _hizTextureView;
    hizImageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkWriteDescriptorSet hizTexWrite{};
    hizTexWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    hizTexWrite.dstBinding = 6;
    hizTexWrite.dstSet = gpuCullDescriptorSets[f];
    hizTexWrite.descriptorCount = 1;
    hizTexWrite.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    hizTexWrite.pImageInfo = &hizImageInfo;

    VkDescriptorImageInfo hizSampInfo{};
    hizSampInfo.sampler = _hizSampler;

    VkWriteDescriptorSet hizSampWrite{};
    hizSampWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    hizSampWrite.dstBinding = 7;
    hizSampWrite.dstSet = gpuCullDescriptorSets[f];
    hizSampWrite.descriptorCount = 1;
    hizSampWrite.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    hizSampWrite.pImageInfo = &hizSampInfo;

    VkWriteDescriptorSet writes[] = {hizTexWrite, hizSampWrite};
    vkUpdateDescriptorSets(device.getLogicalDevice(), 2, writes, 0, nullptr);
  }

  spdlog::info("Hi-Z resources created: {}x{}, {} mip levels", width, height, _hizMipLevels);

  // --- Hi-Z compute pipelines and descriptor sets ---

  // Both CopyDepthToHiZ (set 0) and DownsampleHiZ (set 1) use the same layout:
  //   binding 0: Texture2D<float>  (sampled image)
  //   binding 1: RWTexture2D<float> (storage image)
  VkDescriptorSetLayoutBinding hizBindings[2] = {};
  hizBindings[0].binding = 0;
  hizBindings[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  hizBindings[0].descriptorCount = 1;
  hizBindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  hizBindings[1].binding = 1;
  hizBindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
  hizBindings[1].descriptorCount = 1;
  hizBindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

  VkDescriptorSetLayoutCreateInfo hizSetLayoutInfo{};
  hizSetLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  hizSetLayoutInfo.bindingCount = 2;
  hizSetLayoutInfo.pBindings = hizBindings;

  // Both sets share the same layout
  vkCreateDescriptorSetLayout(device.getLogicalDevice(), &hizSetLayoutInfo, nullptr, &_hizCopySetLayout);
  vkCreateDescriptorSetLayout(device.getLogicalDevice(), &hizSetLayoutInfo, nullptr, &_hizDownsampleSetLayout);

  // Pipeline layout: set 0 (copy) + set 1 (downsample) + push constant {uint2 mipSize}
  VkDescriptorSetLayout hizSetLayouts[] = {_hizCopySetLayout, _hizDownsampleSetLayout};

  VkPushConstantRange hizPushRange{};
  hizPushRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  hizPushRange.offset = 0;
  hizPushRange.size = sizeof(uint32_t) * 4; // uint4 {srcSize, dstSize}

  VkPipelineLayoutCreateInfo hizPipeLayoutInfo{};
  hizPipeLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  hizPipeLayoutInfo.setLayoutCount = 2;
  hizPipeLayoutInfo.pSetLayouts = hizSetLayouts;
  hizPipeLayoutInfo.pushConstantRangeCount = 1;
  hizPipeLayoutInfo.pPushConstantRanges = &hizPushRange;
  vkCreatePipelineLayout(device.getLogicalDevice(), &hizPipeLayoutInfo, nullptr, &_hizPipelineLayout);

  // Descriptor pool — need 1 copy set + (_hizMipLevels-1) downsample sets
  uint32_t totalSets = 1 + (_hizMipLevels > 0 ? _hizMipLevels - 1 : 0);
  VkDescriptorPoolSize hizPoolSizes[] = {
      {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, totalSets},
      {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, totalSets},
  };
  VkDescriptorPoolCreateInfo hizPoolInfo{};
  hizPoolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  hizPoolInfo.maxSets = totalSets;
  hizPoolInfo.poolSizeCount = 2;
  hizPoolInfo.pPoolSizes = hizPoolSizes;
  vkCreateDescriptorPool(device.getLogicalDevice(), &hizPoolInfo, nullptr, &_hizDescriptorPool);

  // Allocate copy descriptor set (set 0): depthTexture → hizMip0
  {
    VkDescriptorSetAllocateInfo dsAlloc{};
    dsAlloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsAlloc.descriptorPool = _hizDescriptorPool;
    dsAlloc.descriptorSetCount = 1;
    dsAlloc.pSetLayouts = &_hizCopySetLayout;
    vkAllocateDescriptorSets(device.getLogicalDevice(), &dsAlloc, &_hizCopyDescriptorSet);

    VkDescriptorImageInfo srcInfo{};
    srcInfo.imageView = _depthTextureView; // occluder depth (D32_SFLOAT aspect view)
    srcInfo.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;

    VkDescriptorImageInfo dstInfo{};
    dstInfo.imageView = _hizMipViews[0];
    dstInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    VkWriteDescriptorSet copyWrites[2] = {};
    copyWrites[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    copyWrites[0].dstSet = _hizCopyDescriptorSet;
    copyWrites[0].dstBinding = 0;
    copyWrites[0].descriptorCount = 1;
    copyWrites[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    copyWrites[0].pImageInfo = &srcInfo;

    copyWrites[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    copyWrites[1].dstSet = _hizCopyDescriptorSet;
    copyWrites[1].dstBinding = 1;
    copyWrites[1].descriptorCount = 1;
    copyWrites[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    copyWrites[1].pImageInfo = &dstInfo;

    vkUpdateDescriptorSets(device.getLogicalDevice(), 2, copyWrites, 0, nullptr);
  }

  // Allocate downsample descriptor sets (set 1): prevMip → currentMip, for each mip 1..N-1
  _hizDownsampleDescriptorSets.resize(_hizMipLevels > 1 ? _hizMipLevels - 1 : 0);
  for (uint32_t m = 1; m < _hizMipLevels; ++m) {
    VkDescriptorSetAllocateInfo dsAlloc{};
    dsAlloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsAlloc.descriptorPool = _hizDescriptorPool;
    dsAlloc.descriptorSetCount = 1;
    dsAlloc.pSetLayouts = &_hizDownsampleSetLayout;
    vkAllocateDescriptorSets(device.getLogicalDevice(), &dsAlloc, &_hizDownsampleDescriptorSets[m - 1]);

    VkDescriptorImageInfo prevInfo{};
    prevInfo.imageView = _hizMipViews[m - 1];
    prevInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkDescriptorImageInfo curInfo{};
    curInfo.imageView = _hizMipViews[m];
    curInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    VkWriteDescriptorSet dsWrites[2] = {};
    dsWrites[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    dsWrites[0].dstSet = _hizDownsampleDescriptorSets[m - 1];
    dsWrites[0].dstBinding = 0;
    dsWrites[0].descriptorCount = 1;
    dsWrites[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    dsWrites[0].pImageInfo = &prevInfo;

    dsWrites[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    dsWrites[1].dstSet = _hizDownsampleDescriptorSets[m - 1];
    dsWrites[1].dstBinding = 1;
    dsWrites[1].descriptorCount = 1;
    dsWrites[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    dsWrites[1].pImageInfo = &curInfo;

    vkUpdateDescriptorSets(device.getLogicalDevice(), 2, dsWrites, 0, nullptr);
  }

  // Create compute pipelines
  auto hizCopyCode = readFile((_rootPath / "shaders/hiz_copy.cs.spv").generic_string());
  auto hizDownsampleCode = readFile((_rootPath / "shaders/hiz_downsample.cs.spv").generic_string());

  VkShaderModule hizCopyModule = createShaderModule(hizCopyCode);
  VkShaderModule hizDownsampleModule = createShaderModule(hizDownsampleCode);

  VkPipelineShaderStageCreateInfo hizCopyStage{};
  hizCopyStage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  hizCopyStage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
  hizCopyStage.module = hizCopyModule;
  hizCopyStage.pName = "CopyDepthToHiZ";

  VkComputePipelineCreateInfo hizCopyPipeInfo{};
  hizCopyPipeInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
  hizCopyPipeInfo.layout = _hizPipelineLayout;
  hizCopyPipeInfo.stage = hizCopyStage;
  vkCreateComputePipelines(device.getLogicalDevice(), VK_NULL_HANDLE, 1,
                           &hizCopyPipeInfo, nullptr, &_hizCopyPipeline);

  VkPipelineShaderStageCreateInfo hizDownsampleStage{};
  hizDownsampleStage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  hizDownsampleStage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
  hizDownsampleStage.module = hizDownsampleModule;
  hizDownsampleStage.pName = "DownsampleHiZ";

  VkComputePipelineCreateInfo hizDownPipeInfo{};
  hizDownPipeInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
  hizDownPipeInfo.layout = _hizPipelineLayout;
  hizDownPipeInfo.stage = hizDownsampleStage;
  vkCreateComputePipelines(device.getLogicalDevice(), VK_NULL_HANDLE, 1,
                           &hizDownPipeInfo, nullptr, &_hizDownsamplePipeline);

  vkDestroyShaderModule(device.getLogicalDevice(), hizCopyModule, nullptr);
  vkDestroyShaderModule(device.getLogicalDevice(), hizDownsampleModule, nullptr);

  spdlog::info("Hi-Z compute pipelines created");
}

void GpuScene::generateHiZPyramid(VkCommandBuffer commandBuffer) {
  if (_hizCopyPipeline == VK_NULL_HANDLE || _hizMipLevels == 0)
    return;

  // 1. Transition occluder depth to SHADER_READ_ONLY for sampling
  {
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = _depthTexture;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;
    barrier.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

    vkCmdPipelineBarrier(commandBuffer,
        VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &barrier);
  }

  // 2. Transition Hi-Z mip 0 to GENERAL for storage write
  {
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = _hizTexture;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = _hizMipLevels;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;

    vkCmdPipelineBarrier(commandBuffer,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &barrier);
  }

  // 3. Dispatch CopyDepthToHiZ: occluder depth → Hi-Z mip 0
  {
    // Push {srcSize, dstSize} — for copy, src=screen resolution, dst=hiz mip0
    uint32_t pushData[4] = {_hizWidth, _hizHeight, _hizWidth, _hizHeight};

    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, _hizCopyPipeline);
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
        _hizPipelineLayout, 0, 1, &_hizCopyDescriptorSet, 0, nullptr);
    vkCmdPushConstants(commandBuffer, _hizPipelineLayout,
        VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pushData), pushData);
    vkCmdDispatch(commandBuffer, (_hizWidth + 7) / 8, (_hizHeight + 7) / 8, 1);
  }

  // 4. Downsample mip chain: for each mip 1..N-1
  for (uint32_t mip = 1; mip < _hizMipLevels; ++mip) {
    uint32_t mipW = (_hizWidth >> mip) > 1 ? (_hizWidth >> mip) : 1;
    uint32_t mipH = (_hizHeight >> mip) > 1 ? (_hizHeight >> mip) : 1;
    uint32_t prevW = (_hizWidth >> (mip - 1)) > 1 ? (_hizWidth >> (mip - 1)) : 1;
    uint32_t prevH = (_hizHeight >> (mip - 1)) > 1 ? (_hizHeight >> (mip - 1)) : 1;

    // Barrier: previous mip write → current mip read
    {
      VkImageMemoryBarrier barrier{};
      barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
      barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
      barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
      barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      barrier.image = _hizTexture;
      barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
      barrier.subresourceRange.baseMipLevel = mip - 1;
      barrier.subresourceRange.levelCount = 1;
      barrier.subresourceRange.baseArrayLayer = 0;
      barrier.subresourceRange.layerCount = 1;
      barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
      barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

      vkCmdPipelineBarrier(commandBuffer,
          VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
          VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
          0, 0, nullptr, 0, nullptr, 1, &barrier);
    }

    // Push {srcSize, dstSize} for edge handling of odd-sized mips
    uint32_t pushData[4] = {prevW, prevH, mipW, mipH};

    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, _hizDownsamplePipeline);
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
        _hizPipelineLayout, 1, 1, &_hizDownsampleDescriptorSets[mip - 1], 0, nullptr);
    vkCmdPushConstants(commandBuffer, _hizPipelineLayout,
        VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pushData), pushData);
    vkCmdDispatch(commandBuffer, (mipW + 7) / 8, (mipH + 7) / 8, 1);
  }

  // 5. Transition last mip to SHADER_READ_ONLY, then full Hi-Z to SHADER_READ_ONLY for cull shader
  {
    // Last mip was written in GENERAL, transition it too
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = _hizTexture;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = _hizMipLevels - 1;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

    vkCmdPipelineBarrier(commandBuffer,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &barrier);
  }

  // 6. Transition occluder depth back to attachment for subsequent passes
  {
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = _depthTexture;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;
    barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barrier.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

    vkCmdPipelineBarrier(commandBuffer,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
        0, 0, nullptr, 0, nullptr, 1, &barrier);
  }
}

void GpuScene::createSyncObjects() {
  // framesInFlight 由 swapchain 图像数量决定
    framesInFlight = device.getSwapChainImageCount();

  imageAvailableSemaphores.resize(framesInFlight);
  renderFinishedSemaphores.resize(framesInFlight);
  inFlightFences.resize(framesInFlight);
  // imagesInFlight tracks which fence is currently associated with each
  // swapchain image. Since framesInFlight == swapChainImageCount in this
  // project, we size it the same. Initially all NULL (no image used yet).
  imagesInFlight.assign(framesInFlight, VK_NULL_HANDLE);
  _syncSlot = 0;
  currentFrame = 0;

  VkSemaphoreCreateInfo semaphoreInfo{};
  semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

  VkFenceCreateInfo fenceInfo{};
  fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
  fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

  for (size_t i = 0; i < framesInFlight; i++) {
    if (vkCreateSemaphore(device.getLogicalDevice(), &semaphoreInfo, nullptr,
                          &imageAvailableSemaphores[i]) != VK_SUCCESS ||
        vkCreateSemaphore(device.getLogicalDevice(), &semaphoreInfo, nullptr,
                          &renderFinishedSemaphores[i]) != VK_SUCCESS ||
        vkCreateFence(device.getLogicalDevice(), &fenceInfo, nullptr,
                      &inFlightFences[i]) != VK_SUCCESS) {
      throw std::runtime_error(
          "failed to create synchronization objects for a frame!");
    }
  }
}

AAPLTextureData::AAPLTextureData(AAPLTextureData &&rhs) {
  _path = std::move(rhs._path);
  _pathHash = rhs._pathHash;
  _width = rhs._width;
  _height = rhs._height;
  _mipmapLevelCount = rhs._mipmapLevelCount;
  _pixelFormat = rhs._pixelFormat;
  _pixelDataOffset = rhs._pixelDataOffset;
  _pixelDataLength = rhs._pixelDataLength;
  _mipOffsets = std::move(rhs._mipOffsets);
  _mipLengths = std::move(rhs._mipLengths);
}

AAPLTextureData::AAPLTextureData(FILE *f) {
  int path_length = 0;
  fread(&path_length, sizeof(int), 1, f);
  char *cstring = (char *)malloc(path_length + 1);
  fread(cstring, 1, path_length, f);
  cstring[path_length] = 0;
  _path = std::string(cstring);
  free(cstring);
  fread(&_pathHash, sizeof(uint32_t), 1, f);
  fread(&_width, sizeof(unsigned long long), 1, f);
  fread(&_height, sizeof(unsigned long long), 1, f);
  fread(&_mipmapLevelCount, sizeof(unsigned long long), 1, f);
  fread(&_pixelFormat, sizeof(uint32_t), 1, f);
  fread(&_pixelDataOffset, sizeof(unsigned long long), 1, f);
  fread(&_pixelDataLength, sizeof(unsigned long long), 1, f);

  for (int i = 0; i < _mipmapLevelCount; i++) {
    unsigned long long offset_c;
    fread(&offset_c, sizeof(offset_c), 1, f);
    _mipOffsets.push_back(offset_c);
  }

  for (int i = 0; i < _mipmapLevelCount; i++) {
    unsigned long long length_c;
    fread(&length_c, sizeof(length_c), 1, f);
    _mipLengths.push_back(length_c);
  }
}

#ifdef __ANDROID__
AAPLTextureData::AAPLTextureData(AssetLoader::BinaryFileReader &reader) {
  int path_length = 0;
  reader.read(&path_length, sizeof(int), 1);
  char *cstring = (char *)malloc(path_length + 1);
  reader.read(cstring, 1, path_length);
  cstring[path_length] = 0;
  _path = std::string(cstring);
  free(cstring);
  reader.read(&_pathHash, sizeof(uint32_t), 1);
  reader.read(&_width, sizeof(unsigned long long), 1);
  reader.read(&_height, sizeof(unsigned long long), 1);
  reader.read(&_mipmapLevelCount, sizeof(unsigned long long), 1);
  reader.read(&_pixelFormat, sizeof(uint32_t), 1);
  reader.read(&_pixelDataOffset, sizeof(unsigned long long), 1);
  reader.read(&_pixelDataLength, sizeof(unsigned long long), 1);

  for (int i = 0; i < _mipmapLevelCount; i++) {
    unsigned long long offset_c;
    reader.read(&offset_c, sizeof(offset_c), 1);
    _mipOffsets.push_back(offset_c);
  }

  for (int i = 0; i < _mipmapLevelCount; i++) {
    unsigned long long length_c;
    reader.read(&length_c, sizeof(length_c), 1);
    _mipLengths.push_back(length_c);
  }
}
#endif

AAPLMeshData::~AAPLMeshData() {
  if (_vertexData)
    free(_vertexData);
  if (_normalData)
    free(_normalData);
  if (_tangentData)
    free(_tangentData);
  if (_uvData)
    free(_uvData);
  if (_indexData)
    free(_indexData);
  if (_chunkData)
    free(_chunkData);
  if (_meshData)
    free(_meshData);
  if (_materialData)
    free(_materialData);
  if (_textureData)
    free(_textureData);
}

enum MTLIndexType { MTLIndexTypeUInt16 = 0, MTLIndexTypeUInt32 = 1 };

AAPLMeshData::AAPLMeshData(const char *filepath) {
#ifdef __ANDROID__
  AssetLoader::BinaryFileReader reader(filepath);
  if (reader.isOpen()) {
    reader.read(&_vertexCount, sizeof(_vertexCount), 1);
    reader.read(&_indexCount, sizeof(_indexCount), 1);
    reader.read(&_indexType, sizeof(_indexType), 1);
    if (_indexType != MTLIndexTypeUInt32)
      spdlog::error("index type error!!!");
    reader.read(&_chunkCount, sizeof(_chunkCount), 1);
    reader.read(&_meshCount, sizeof(_meshCount), 1);
    reader.read(&_opaqueChunkCount, sizeof(_opaqueChunkCount), 1);
    reader.read(&_opaqueMeshCount, sizeof(_opaqueMeshCount), 1);
    reader.read(&_alphaMaskedChunkCount, sizeof(_alphaMaskedChunkCount), 1);
    reader.read(&_alphaMaskedMeshCount, sizeof(_alphaMaskedMeshCount), 1);
    reader.read(&_transparentChunkCount, sizeof(_transparentChunkCount), 1);
    reader.read(&_transparentMeshCount, sizeof(_transparentMeshCount), 1);
    reader.read(&_materialCount, sizeof(_materialCount), 1);

    unsigned long long bytes_length = 0;
    reader.read(&bytes_length, sizeof(bytes_length), 1);
    compressedVertexDataLength = bytes_length;
    _vertexData = malloc(bytes_length);
    reader.read(_vertexData, 1, bytes_length);

    reader.read(&bytes_length, sizeof(bytes_length), 1);
    compressedNormalDataLength = bytes_length;
    _normalData = malloc(bytes_length);
    reader.read(_normalData, 1, bytes_length);

    reader.read(&bytes_length, sizeof(bytes_length), 1);
    compressedTangentDataLength = bytes_length;
    _tangentData = malloc(bytes_length);
    reader.read(_tangentData, 1, bytes_length);

    reader.read(&bytes_length, sizeof(bytes_length), 1);
    compressedUvDataLength = bytes_length;
    _uvData = malloc(bytes_length);
    reader.read(_uvData, 1, bytes_length);

    reader.read(&bytes_length, sizeof(bytes_length), 1);
    compressedIndexDataLength = bytes_length;
    _indexData = malloc(bytes_length);
    reader.read(_indexData, 1, bytes_length);

    reader.read(&bytes_length, sizeof(bytes_length), 1);
    compressedChunkDataLength = bytes_length;
    _chunkData = malloc(bytes_length);
    reader.read(_chunkData, 1, bytes_length);

    reader.read(&bytes_length, sizeof(bytes_length), 1);
    compressedMeshDataLength = bytes_length;
    _meshData = malloc(bytes_length);
    reader.read(_meshData, 1, bytes_length);

    reader.read(&bytes_length, sizeof(bytes_length), 1);
    compressedMaterialDataLength = bytes_length;
    _materialData = malloc(bytes_length);
    reader.read(_materialData, 1, bytes_length);

    unsigned long long texture_count = 0;
    reader.read(&texture_count, sizeof(bytes_length), 1);

    for (int i = 0; i < texture_count; ++i) {
      _textures.push_back(AAPLTextureData(reader));
    }

    reader.read(&bytes_length, sizeof(bytes_length), 1);
    _textureData = malloc(bytes_length);
    reader.read(_textureData, 1, bytes_length);

    reader.close();
  }

  else {
    spdlog::error("file not found {}", filepath);
  }
#else
  FILE *rawFile = fopen(filepath, "rb");
  if (rawFile) {
    // unsigned long
    // _vertexCount,_indexCount,_indexType,_chunkCount,_meshCount,_opaqueChunkCount,_opaqueMeshCount,_alphaMaskedChunkCount,_alphaMaskedMeshCount,_transparentChunkCount,_transparentMeshCount,_materialCount;
    fread(&_vertexCount, sizeof(_vertexCount), 1, rawFile);
    fread(&_indexCount, sizeof(_indexCount), 1, rawFile);
    fread(&_indexType, sizeof(_indexType), 1, rawFile);
    if (_indexType != MTLIndexTypeUInt32)
      spdlog::error("index type error!!!");
    fread(&_chunkCount, sizeof(_chunkCount), 1, rawFile);
    fread(&_meshCount, sizeof(_meshCount), 1, rawFile);
    fread(&_opaqueChunkCount, sizeof(_opaqueChunkCount), 1, rawFile);
    fread(&_opaqueMeshCount, sizeof(_opaqueMeshCount), 1, rawFile);
    fread(&_alphaMaskedChunkCount, sizeof(_alphaMaskedChunkCount), 1, rawFile);
    fread(&_alphaMaskedMeshCount, sizeof(_alphaMaskedMeshCount), 1, rawFile);
    fread(&_transparentChunkCount, sizeof(_transparentChunkCount), 1, rawFile);
    fread(&_transparentMeshCount, sizeof(_transparentMeshCount), 1, rawFile);
    fread(&_materialCount, sizeof(_materialCount), 1, rawFile);

    unsigned long long bytes_length = 0;
    fread(&bytes_length, sizeof(bytes_length), 1, rawFile);
    compressedVertexDataLength = bytes_length;
    _vertexData = malloc(bytes_length);
    fread(_vertexData, 1, bytes_length, rawFile);

    fread(&bytes_length, sizeof(bytes_length), 1, rawFile);
    compressedNormalDataLength = bytes_length;
    _normalData = malloc(bytes_length);
    fread(_normalData, 1, bytes_length, rawFile);

    fread(&bytes_length, sizeof(bytes_length), 1, rawFile);
    compressedTangentDataLength = bytes_length;
    _tangentData = malloc(bytes_length);
    fread(_tangentData, 1, bytes_length, rawFile);

    fread(&bytes_length, sizeof(bytes_length), 1, rawFile);
    compressedUvDataLength = bytes_length;
    _uvData = malloc(bytes_length);
    fread(_uvData, 1, bytes_length, rawFile);

    fread(&bytes_length, sizeof(bytes_length), 1, rawFile);
    compressedIndexDataLength = bytes_length;
    _indexData = malloc(bytes_length);
    fread(_indexData, 1, bytes_length, rawFile);

    fread(&bytes_length, sizeof(bytes_length), 1, rawFile);
    compressedChunkDataLength = bytes_length;
    _chunkData = malloc(bytes_length);
    fread(_chunkData, 1, bytes_length, rawFile);

    fread(&bytes_length, sizeof(bytes_length), 1, rawFile);
    compressedMeshDataLength = bytes_length;
    _meshData = malloc(bytes_length);
    fread(_meshData, 1, bytes_length, rawFile);

    fread(&bytes_length, sizeof(bytes_length), 1, rawFile);
    compressedMaterialDataLength = bytes_length;
    _materialData = malloc(bytes_length);
    fread(_materialData, 1, bytes_length, rawFile);

    unsigned long long texture_count = 0;
    fread(&texture_count, sizeof(bytes_length), 1, rawFile);

    //[[NSArray alloc] initWithObjects:[[AAPLTextureData alloc] init]
    //count:texture_count];

    for (int i = 0; i < texture_count; ++i) {
      _textures.push_back(AAPLTextureData(rawFile));
    }

    fread(&bytes_length, sizeof(bytes_length), 1, rawFile);
    _textureData = malloc(bytes_length);
    fread(_textureData, 1, bytes_length, rawFile);

    fclose(rawFile);
  }

  else {
    spdlog::error("file not found {}", filepath);
  }
#endif // !__ANDROID__
}

// Helper to get the properties of block compressed pixel formats used by this
// sample.
void getBCProperties(MTLPixelFormat pixelFormat, unsigned long long &blockSize,
                     unsigned long long &bytesPerBlock,
                     unsigned long long &channels, int &alpha) {
  if (pixelFormat == MTLPixelFormatBC5_RGUnorm ||
      pixelFormat == MTLPixelFormatBC5_RGSnorm) {
    blockSize = 4;
    bytesPerBlock = 16;
    channels = 2;
    alpha = 0;
  } else if (pixelFormat == MTLPixelFormatBC4_RUnorm) {
    blockSize = 4;
    bytesPerBlock = 8;
    channels = 1;
    alpha = 0;
  } else if (pixelFormat == MTLPixelFormatBC1_RGBA_sRGB ||
             pixelFormat == MTLPixelFormatBC1_RGBA) {
    blockSize = 4;
    bytesPerBlock = 8;
    channels = 4;
    alpha = 0;
  } else if (pixelFormat == MTLPixelFormatBC3_RGBA_sRGB ||
             pixelFormat == MTLPixelFormatBC3_RGBA) {
    blockSize = 4;
    bytesPerBlock = 16;
    channels = 4;
    alpha = 1;
  }
}

void getPixelFormatBlockDesc(MTLPixelFormat pixelFormat,
                             unsigned long long &blockSize,
                             unsigned long long &bytesPerBlock) {
  blockSize = 4;
  bytesPerBlock = 16;

  unsigned long long channels_UNUSED = 0;
  int alpha_UNUSED = 1;
  getBCProperties(pixelFormat, blockSize, bytesPerBlock, channels_UNUSED,
                  alpha_UNUSED);
}

#define MAX(a, b) ((a) > (b) ? (a) : (b))

unsigned long long calculateMipSizeInBlocks(unsigned long long size,
                                            unsigned long long blockSize,
                                            unsigned long long mip) {
  unsigned long long blocksWide = MAX(size / blockSize, 1);

  return MAX(blocksWide >> mip, 1U);
}

void *GpuScene::loadMipTexture(const AAPLTextureData &texturedata, int mip,
                               unsigned int &bytesPerImage) {

  void *texturedataRaw =
      (unsigned char *)applMesh->_textureData + texturedata._pixelDataOffset;

  unsigned long long blockSize, bytesPerBlock;
  getPixelFormatBlockDesc((MTLPixelFormat)texturedata._pixelFormat, blockSize,
                          bytesPerBlock);

  unsigned long long blocksWide =
      calculateMipSizeInBlocks(texturedata._width, blockSize, mip);
  unsigned long long blocksHigh =
      calculateMipSizeInBlocks(texturedata._height, blockSize, mip);

  unsigned long long tempbufferSize = 0;
  unsigned long long bytesPerRow = MAX(blocksWide >> 0, 1U) * bytesPerBlock;
  bytesPerImage = MAX(blocksHigh >> 0, 1U) * bytesPerRow;
  // if (bytesPerImage != texturedata._mipLengths[mip])
  //     spdlog::warn("texture data may be corrupted");
  void *uncompresseddata = uncompressData(
      (unsigned char *)texturedataRaw + texturedata._mipOffsets[mip],
      texturedata._mipLengths[mip], bytesPerImage);

  return uncompresseddata;
}

void GpuScene::CreateTextures() {
  // --- Phase 1: batch all texture uploads into one command buffer ---
  // Only permanent (coarsest) mips are loaded at startup.
  // Higher-resolution mips are streamed in on demand by UpdateTextureStreaming().

  // Step 1: pre-calculate total staging buffer size (permanent mips only)
  //         and prepare streaming entries.
  streamingEntries.clear();
  streamingEntries.reserve(applMesh->_textures.size());
  streamingEntryMap.clear();

  size_t totalStagingSize = 0;
  for (auto &texture : applMesh->_textures) {
    int permanentMip = calculateMinMip(texture, PERMANENT_TEXTURE_SIZE);
    unsigned long long blockSize, bytesPerBlock;
    getPixelFormatBlockDesc((MTLPixelFormat)texture._pixelFormat, blockSize,
                            bytesPerBlock);
    for (int mip = permanentMip; mip < texture._mipmapLevelCount; ++mip) {
      unsigned long long blocksWide =
          calculateMipSizeInBlocks(texture._width, blockSize, mip);
      unsigned long long blocksHigh =
          calculateMipSizeInBlocks(texture._height, blockSize, mip);
      unsigned long long bytesPerRow =
          MAX(blocksWide >> 0, 1U) * bytesPerBlock;
      unsigned long long bytesPerImage =
          MAX(blocksHigh >> 0, 1U) * bytesPerRow;
      totalStagingSize += bytesPerImage;
    }
    // Align to 16 bytes for BC3/BC5 block alignment
    totalStagingSize = (totalStagingSize + 15) & ~15ull;
  }

  // Step 2: create one large staging buffer and map it once
  VkBuffer stagingBuffer;
  VkDeviceMemory stagingMemory;
  createBuffer(totalStagingSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                   VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
               stagingBuffer, stagingMemory);

  void *mappedData = nullptr;
  vkMapMemory(device.getLogicalDevice(), stagingMemory, 0, totalStagingSize, 0,
              &mappedData);

  // Step 3: create command buffer and fence
  VkCommandBuffer cmdBuf = device.beginSingleTimeCommands();

  VkFence uploadFence;
  VkFenceCreateInfo fenceInfo{};
  fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
  vkCreateFence(device.getLogicalDevice(), &fenceInfo, nullptr, &uploadFence);

  // Step 4: record upload commands for every texture (permanent mips only)
  size_t stagingOffset = 0;
  for (auto &texture : applMesh->_textures) {
    int permanentMip = calculateMinMip(texture, PERMANENT_TEXTURE_SIZE);
    int loadedMipCount = texture._mipmapLevelCount - permanentMip;

    VkImage textureImage;
    VkImageView textureView;
    VkDeviceMemory textureImageMemory;

    VkFormat vkFormat =
        mapFromApple((MTLPixelFormat)texture._pixelFormat);

    // --- create image with only the permanent+ mips ---
    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.extent.width = static_cast<uint32_t>(texture._width) >> permanentMip;
    imageInfo.extent.height = static_cast<uint32_t>(texture._height) >> permanentMip;
    imageInfo.extent.depth = 1;
    imageInfo.mipLevels = loadedMipCount;
    imageInfo.arrayLayers = 1;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.format = vkFormat;
    imageInfo.usage =
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.flags = 0;

    if (vkCreateImage(device.getLogicalDevice(), &imageInfo, nullptr,
                      &textureImage) != VK_SUCCESS) {
      throw std::runtime_error("failed to create image!");
    }

    VkMemoryRequirements memRequirements;
    vkGetImageMemoryRequirements(device.getLogicalDevice(), textureImage,
                                 &memRequirements);

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memRequirements.size;
    allocInfo.memoryTypeIndex = device.findMemoryType(
        memRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    if (vkAllocateMemory(device.getLogicalDevice(), &allocInfo, nullptr,
                         &textureImageMemory) != VK_SUCCESS) {
      throw std::runtime_error("failed to allocate image memory!");
    }
    vkBindImageMemory(device.getLogicalDevice(), textureImage,
                      textureImageMemory, 0);

    // --- record UNDEFINED -> TRANSFER_DST barrier (all loaded mips) ---
    device.cmdTransitionImageLayout(cmdBuf, textureImage, vkFormat,
                                    VK_IMAGE_LAYOUT_UNDEFINED,
                                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                    loadedMipCount);

    // --- upload permanent mips ---
    // Source mip N goes into VkImage mip level (N - permanentMip)
    for (int srcMip = permanentMip; srcMip < texture._mipmapLevelCount; ++srcMip) {
      int dstMip = srcMip - permanentMip;
      unsigned int rawDataLength = 0;
      void *pixelDataRaw = loadMipTexture(texture, srcMip, rawDataLength);

      memcpy((char *)mappedData + stagingOffset, pixelDataRaw, rawDataLength);
      free(pixelDataRaw);

      VkBufferImageCopy region{};
      region.bufferOffset = stagingOffset;
      region.bufferRowLength = 0;
      region.bufferImageHeight = 0;
      region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
      region.imageSubresource.mipLevel = static_cast<uint32_t>(dstMip);
      region.imageSubresource.baseArrayLayer = 0;
      region.imageSubresource.layerCount = 1;
      region.imageOffset = {0, 0, 0};
      region.imageExtent = {
          MAX(static_cast<uint32_t>(texture._width) >> srcMip, 1u),
          MAX(static_cast<uint32_t>(texture._height) >> srcMip, 1u),
          1};

      vkCmdCopyBufferToImage(cmdBuf, stagingBuffer, textureImage,
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

      stagingOffset += rawDataLength;
    }
    // Align to 16 bytes — BC1 block size is 8, BC3/BC5 are 16. 16 covers all.
    stagingOffset = (stagingOffset + 15) & ~15ull;

    // --- record TRANSFER_DST -> SHADER_READ barrier (all loaded mips) ---
    device.cmdTransitionImageLayout(cmdBuf, textureImage, vkFormat,
                                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                    loadedMipCount);

    // --- create image view covering all loaded mips ---
    VkImageViewCreateInfo imageviewInfo{};
    imageviewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    imageviewInfo.image = textureImage;
    imageviewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    imageviewInfo.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
    imageviewInfo.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
    imageviewInfo.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
    imageviewInfo.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
    imageviewInfo.format = vkFormat;
    imageviewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    imageviewInfo.subresourceRange.baseMipLevel = 0;
    imageviewInfo.subresourceRange.levelCount = loadedMipCount;
    imageviewInfo.subresourceRange.baseArrayLayer = 0;
    imageviewInfo.subresourceRange.layerCount = 1;

    if (vkCreateImageView(device.getLogicalDevice(), &imageviewInfo, nullptr,
                          &textureView) != VK_SUCCESS) {
      throw std::runtime_error("failed to create texture image view!");
    }

    size_t texIndex = textures.size();
    textureHashMap[texture._pathHash] = texIndex;
    textures.push_back({textureImage, textureView});
    textureMemory.push_back(textureImageMemory);

    // --- register streaming entry ---
    TextureStreamingEntry entry;
    entry.desc = &texture;
    entry.image = textureImage;
    entry.imageView = textureView;
    entry.memory = textureImageMemory;
    entry.currentMip = permanentMip;
    entry.requiredMip = permanentMip;
    entry.textureIndex = texIndex;
    streamingEntries.push_back(entry);
    streamingEntryMap[texture._pathHash] = streamingEntries.size() - 1;
  }

  // Step 5: submit once, wait once
  device.endSingleTimeCommands(cmdBuf, uploadFence);
  vkWaitForFences(device.getLogicalDevice(), 1, &uploadFence, VK_TRUE,
                  UINT64_MAX);

  // Step 6: cleanup
  vkDestroyFence(device.getLogicalDevice(), uploadFence, nullptr);
  vkFreeCommandBuffers(device.getLogicalDevice(), device.getCommandPool(), 1,
                       &cmdBuf);
  vkUnmapMemory(device.getLogicalDevice(), stagingMemory);
  vkDestroyBuffer(device.getLogicalDevice(), stagingBuffer, nullptr);
  vkFreeMemory(device.getLogicalDevice(), stagingMemory, nullptr);
}

// ============================================================================
// Texture Streaming — mirrors Metal's AAPLTextureManager mip-streaming design
// ============================================================================

// Simple sphere-in-frustum test using the existing Frustum class
static bool sphereInFrustum(const Frustum& frustum, const AAPLSphere& sphere) {
  // Convert sphere to AABB and use existing AABB culling
  AAPLBoundingBox3 aabb;
  aabb.min.x = sphere.data.x - sphere.data.w;
  aabb.min.y = sphere.data.y - sphere.data.w;
  aabb.min.z = sphere.data.z - sphere.data.w;
  aabb.max.x = sphere.data.x + sphere.data.w;
  aabb.max.y = sphere.data.y + sphere.data.w;
  aabb.max.z = sphere.data.z + sphere.data.w;
  return !frustum.FrustumCull(aabb);
}

int GpuScene::calculateMinMip(const AAPLTextureData& desc, unsigned int maxSize) const {
  unsigned long long texSize = (desc._width > desc._height) ? desc._width : desc._height;
  unsigned long long ratio = texSize / maxSize;
  if (ratio < 1) ratio = 1;
  int minMip = static_cast<int>(log2(static_cast<float>(ratio)));
  if (minMip >= static_cast<int>(desc._mipmapLevelCount))
    minMip = static_cast<int>(desc._mipmapLevelCount) - 1;
  return minMip;
}

int GpuScene::calculateRequiredMip(const AAPLTextureData& desc, float screenArea) const {
  float topMipTexelArea = static_cast<float>(desc._width * desc._height);
  int topMip = calculateMinMip(desc, MAX_TEXTURE_SIZE);
  int botMip = calculateMinMip(desc, PERMANENT_TEXTURE_SIZE);

  if (screenArea <= 0.0f) return botMip;
  int mipLevel = static_cast<int>(0.5f * log2(topMipTexelArea / screenArea));
  if (mipLevel < topMip) mipLevel = topMip;
  if (mipLevel > botMip) mipLevel = botMip;
  return mipLevel;
}

void GpuScene::setRequiredMip(uint32_t textureHash, float screenArea) {
  auto it = streamingEntryMap.find(textureHash);
  if (it == streamingEntryMap.end()) return;

  auto& entry = streamingEntries[it->second];
  int mip = calculateRequiredMip(*entry.desc, screenArea);
  // MIN accumulation: closest chunk "wins" (finest mip across all callers)
  if (mip < entry.requiredMip) entry.requiredMip = mip;
}

void GpuScene::initTextureStreaming() {
  textureToDelete.resize(TEXTURE_RETENTION_FRAMES);
  blitThreadRunning = true;
  blitThread = std::thread(&GpuScene::blitThreadFunc, this);
}

void GpuScene::shutdownTextureStreaming() {
  blitThreadRunning = false;
  blitCondition.notify_all();
  if (blitThread.joinable()) blitThread.join();

  // Clean up any unprocessed CPU work
  for (auto& work : cpuCompletedWork) {
    if (work.stagingBuf != VK_NULL_HANDLE) {
      vkDestroyBuffer(device.getLogicalDevice(), work.stagingBuf, nullptr);
      vkFreeMemory(device.getLogicalDevice(), work.stagingMem, nullptr);
    }
    vkDestroyImage(device.getLogicalDevice(), work.newImage, nullptr);
    vkFreeMemory(device.getLogicalDevice(), work.newMemory, nullptr);
  }
  cpuCompletedWork.clear();

  // Clean up retention ring
  for (auto& frameRetain : textureToDelete) {
    for (auto& [img, view, mem] : frameRetain) {
      vkDestroyImageView(device.getLogicalDevice(), view, nullptr);
      vkDestroyImage(device.getLogicalDevice(), img, nullptr);
      vkFreeMemory(device.getLogicalDevice(), mem, nullptr);
    }
    frameRetain.clear();
  }
}

void GpuScene::dispatchStreamingRequest(size_t entryIndex) {
  auto& entry = streamingEntries[entryIndex];
  PendingBlit blit;
  blit.textureHash = entry.desc->_pathHash;
  blit.entryIndex = entryIndex;
  blit.targetMip = entry.requiredMip;
  // Snapshot fields read by the background thread to avoid data race with processStreamingWork
  blit.snapshotCurrentMip = entry.currentMip;
  blit.snapshotImage = entry.image;

  // Mark in-flight BEFORE pushing to pendingBlits so UpdateTextureStreaming won't
  // dispatch a duplicate next frame.  Cleared by processStreamingWork after the GPU
  // copy completes, guaranteeing entry.image is live for the entire lifetime of the
  // work item (since only processStreamingWork retires entry.image into textureToDelete).
  entry.inFlight = true;

  {
    std::lock_guard<std::mutex> lock(pendingBlitsMutex);
    pendingBlits.push_back(blit);
  }
  blitCondition.notify_one();
}

void GpuScene::blitThreadFunc() {
  // CPU-only work: create images, decompress mips, fill staging buffers.
  // GPU command recording and submission happens on the main thread.
  while (blitThreadRunning) {
    std::unique_lock<std::mutex> lock(pendingBlitsMutex);
    blitCondition.wait(lock, [this] {
      return !pendingBlits.empty() || !blitThreadRunning;
    });

    if (!blitThreadRunning) break;

    std::vector<PendingBlit> workItems;
    std::swap(workItems, pendingBlits);
    lock.unlock();

    for (auto& work : workItems) {
      auto& entry = streamingEntries[work.entryIndex];
      const AAPLTextureData& desc = *entry.desc;
      int targetMip = work.targetMip;
      int currentMip = work.snapshotCurrentMip;   // use snapshot — avoids data race
      VkImage sourceImage = work.snapshotImage;   // use snapshot — avoids data race
      VkFormat vkFormat = mapFromApple((MTLPixelFormat)desc._pixelFormat);

      int newMipCount = desc._mipmapLevelCount - targetMip;
      bool addingMips = (targetMip < currentMip);

      // --- Create new image ---
      VkImage newImage;
      VkImageCreateInfo imageInfo{};
      imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
      imageInfo.imageType = VK_IMAGE_TYPE_2D;
      imageInfo.extent.width = static_cast<uint32_t>(desc._width) >> targetMip;
      imageInfo.extent.height = static_cast<uint32_t>(desc._height) >> targetMip;
      imageInfo.extent.depth = 1;
      imageInfo.mipLevels = static_cast<uint32_t>(newMipCount);
      imageInfo.arrayLayers = 1;
      imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
      imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      imageInfo.format = vkFormat;
      imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
      imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
      imageInfo.flags = 0;

      if (vkCreateImage(device.getLogicalDevice(), &imageInfo, nullptr, &newImage) != VK_SUCCESS) {
        continue;
      }

      VkMemoryRequirements memReq;
      vkGetImageMemoryRequirements(device.getLogicalDevice(), newImage, &memReq);
      VkMemoryAllocateInfo allocInfo{};
      allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
      allocInfo.allocationSize = memReq.size;
      allocInfo.memoryTypeIndex = device.findMemoryType(memReq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

      VkDeviceMemory newMemory;
      if (vkAllocateMemory(device.getLogicalDevice(), &allocInfo, nullptr, &newMemory) != VK_SUCCESS) {
        vkDestroyImage(device.getLogicalDevice(), newImage, nullptr);
        continue;
      }
      vkBindImageMemory(device.getLogicalDevice(), newImage, newMemory, 0);

      // --- Calculate staging buffer and mip info ---
      unsigned long long blockSize, bytesPerBlock;
      getPixelFormatBlockDesc((MTLPixelFormat)desc._pixelFormat, blockSize, bytesPerBlock);

      int mipStart, mipEnd;
      if (addingMips) {
        mipStart = targetMip;
        mipEnd = currentMip;
      } else {
        mipStart = targetMip;
        mipEnd = targetMip;
      }

      size_t stagingSize = 0;
      for (int m = mipStart; m < mipEnd; ++m) {
        unsigned long long bw = calculateMipSizeInBlocks(desc._width, blockSize, m);
        unsigned long long bh = calculateMipSizeInBlocks(desc._height, blockSize, m);
        stagingSize += (bw > 1 ? bw : 1) * bytesPerBlock * (bh > 1 ? bh : 1);
      }

      VkBuffer stagingBuf = VK_NULL_HANDLE;
      VkDeviceMemory stagingMem = VK_NULL_HANDLE;

      if (stagingSize > 0) {
        VkBufferCreateInfo bufInfo{};
        bufInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufInfo.size = stagingSize;
        bufInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        bufInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        vkCreateBuffer(device.getLogicalDevice(), &bufInfo, nullptr, &stagingBuf);

        VkMemoryRequirements smr;
        vkGetBufferMemoryRequirements(device.getLogicalDevice(), stagingBuf, &smr);
        VkMemoryAllocateInfo saInfo{};
        saInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        saInfo.allocationSize = smr.size;
        saInfo.memoryTypeIndex = device.findMemoryType(smr.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        vkAllocateMemory(device.getLogicalDevice(), &saInfo, nullptr, &stagingMem);
        vkBindBufferMemory(device.getLogicalDevice(), stagingBuf, stagingMem, 0);

        void* mappedData = nullptr;
        vkMapMemory(device.getLogicalDevice(), stagingMem, 0, stagingSize, 0, &mappedData);
        size_t offset = 0;
        for (int m = mipStart; m < mipEnd; ++m) {
          unsigned int rawLen = 0;
          void* pixels = loadMipTexture(desc, m, rawLen);
          memcpy((char*)mappedData + offset, pixels, rawLen);
          free(pixels);
          offset += rawLen;
        }
        vkUnmapMemory(device.getLogicalDevice(), stagingMem);
      }

      // --- Build copy regions (CPU-side, used later by main thread) ---
      CpuStreamingWork cpuWork;
      cpuWork.entryIndex = work.entryIndex;
      cpuWork.newImage = newImage;
      cpuWork.newMemory = newMemory;
      cpuWork.stagingBuf = stagingBuf;
      cpuWork.stagingMem = stagingMem;
      cpuWork.targetMip = targetMip;
      cpuWork.currentMip = currentMip;
      cpuWork.sourceImage = sourceImage;
      cpuWork.oldMipCount = desc._mipmapLevelCount - currentMip;
      cpuWork.newMipCount = newMipCount;
      cpuWork.format = vkFormat;
      cpuWork.blockSize = blockSize;
      cpuWork.bytesPerBlock = bytesPerBlock;
      cpuWork.mipStart = mipStart;
      cpuWork.mipEnd = mipEnd;

      // Buffer→Image copy regions for new mips
      if (stagingSize > 0) {
        size_t sOffset = 0;
        for (int m = mipStart; m < mipEnd; ++m) {
          unsigned long long bw = calculateMipSizeInBlocks(desc._width, blockSize, m);
          unsigned long long bh = calculateMipSizeInBlocks(desc._height, blockSize, m);

          VkBufferImageCopy region{};
          region.bufferOffset = sOffset;
          region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
          region.imageSubresource.mipLevel = static_cast<uint32_t>(m - targetMip);
          region.imageSubresource.baseArrayLayer = 0;
          region.imageSubresource.layerCount = 1;
          region.imageOffset = {0, 0, 0};
          unsigned int mipW = static_cast<uint32_t>(desc._width) >> m;
          unsigned int mipH = static_cast<uint32_t>(desc._height) >> m;
          region.imageExtent = {mipW > 0 ? mipW : 1u, mipH > 0 ? mipH : 1u, 1};
          cpuWork.bufferCopyRegions.push_back(region);
          sOffset += (bw > 1 ? bw : 1) * bytesPerBlock * (bh > 1 ? bh : 1);
        }
      }

      // Image→Image copy regions for shared mips
      if (addingMips && sourceImage != VK_NULL_HANDLE) {
        // Upgrading to finer mips: copy all old mips into the new (larger) image.
        // Old image mip k → new image mip (k + sharedOffset).
        int sharedOffset = currentMip - targetMip;
        for (int m = 0; m < cpuWork.oldMipCount; ++m) {
          VkImageCopy region{};
          region.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
          region.srcSubresource.mipLevel = static_cast<uint32_t>(m);
          region.srcSubresource.baseArrayLayer = 0;
          region.srcSubresource.layerCount = 1;
          region.srcOffset = {0, 0, 0};
          region.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
          region.dstSubresource.mipLevel = static_cast<uint32_t>(m + sharedOffset);
          region.dstSubresource.baseArrayLayer = 0;
          region.dstSubresource.layerCount = 1;
          region.dstOffset = {0, 0, 0};
          unsigned int mipW = static_cast<uint32_t>(desc._width) >> (m + currentMip);
          unsigned int mipH = static_cast<uint32_t>(desc._height) >> (m + currentMip);
          region.extent = {mipW > 0 ? mipW : 1u, mipH > 0 ? mipH : 1u, 1};
          cpuWork.imageCopyRegions.push_back(region);
        }
      } else if (!addingMips && sourceImage != VK_NULL_HANDLE) {
        // Evicting to coarser mips: all new mips exist in the old image.
        // Old image mip (m + dstOffset) → new image mip m.
        int dstOffset = targetMip - currentMip;
        for (int m = 0; m < newMipCount; ++m) {
          VkImageCopy region{};
          region.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
          region.srcSubresource.mipLevel = static_cast<uint32_t>(m + dstOffset);
          region.srcSubresource.baseArrayLayer = 0;
          region.srcSubresource.layerCount = 1;
          region.srcOffset = {0, 0, 0};
          region.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
          region.dstSubresource.mipLevel = static_cast<uint32_t>(m);
          region.dstSubresource.baseArrayLayer = 0;
          region.dstSubresource.layerCount = 1;
          region.dstOffset = {0, 0, 0};
          unsigned int mipW = static_cast<uint32_t>(desc._width) >> (m + targetMip);
          unsigned int mipH = static_cast<uint32_t>(desc._height) >> (m + targetMip);
          region.extent = {mipW > 0 ? mipW : 1u, mipH > 0 ? mipH : 1u, 1};
          cpuWork.imageCopyRegions.push_back(region);
        }
      }

      {
        std::lock_guard<std::mutex> cLock(cpuWorkMutex);
        cpuCompletedWork.push_back(std::move(cpuWork));
      }
    }
  }
}

void GpuScene::processStreamingWork(int frameIndex) {
  std::vector<CpuStreamingWork> workItems;
  {
    std::lock_guard<std::mutex> lock(cpuWorkMutex);
    std::swap(workItems, cpuCompletedWork);
  }

  if (workItems.empty()) return;

  // Record all GPU commands on the main thread using the main command pool
  VkCommandBuffer cmd = device.beginSingleTimeCommands();

  for (auto& work : workItems) {
    auto& entry = streamingEntries[work.entryIndex];

    // Barrier: new image UNDEFINED -> TRANSFER_DST
    device.cmdTransitionImageLayout(cmd, work.newImage, work.format,
                                    VK_IMAGE_LAYOUT_UNDEFINED,
                                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                    static_cast<uint32_t>(work.newMipCount));

    // Barrier: old image SHADER_READ -> TRANSFER_SRC (both upgrade and eviction paths)
    bool needsSourceCopy = !work.imageCopyRegions.empty() && work.sourceImage != VK_NULL_HANDLE;
    if (needsSourceCopy) {
      device.cmdTransitionImageLayout(cmd, work.sourceImage, work.format,
                                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                      static_cast<uint32_t>(work.oldMipCount));
    }

    // Copy shared mips from old to new
    if (needsSourceCopy) {
      vkCmdCopyImage(cmd, work.sourceImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    work.newImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    static_cast<uint32_t>(work.imageCopyRegions.size()),
                    work.imageCopyRegions.data());
    }

    // Upload new mips from staging
    if (!work.bufferCopyRegions.empty()) {
      vkCmdCopyBufferToImage(cmd, work.stagingBuf, work.newImage,
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                             static_cast<uint32_t>(work.bufferCopyRegions.size()),
                             work.bufferCopyRegions.data());
    }

    // Barrier: new image TRANSFER_DST -> SHADER_READ
    device.cmdTransitionImageLayout(cmd, work.newImage, work.format,
                                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                    static_cast<uint32_t>(work.newMipCount));

    // Barrier: old image TRANSFER_SRC -> SHADER_READ
    if (needsSourceCopy) {
      device.cmdTransitionImageLayout(cmd, work.sourceImage, work.format,
                                      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                      static_cast<uint32_t>(work.oldMipCount));
    }
  }

  // Submit and wait (main thread — uses the wait-inside version for safety)
  device.endSingleTimeCommands(cmd);

  // Create image views and swap textures
  for (auto& work : workItems) {
    auto& entry = streamingEntries[work.entryIndex];
    size_t texIndex = entry.textureIndex;

    VkImageView newView;
    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = work.newImage;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
    viewInfo.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
    viewInfo.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
    viewInfo.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
    viewInfo.format = work.format;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = static_cast<uint32_t>(work.newMipCount);
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 1;
    vkCreateImageView(device.getLogicalDevice(), &viewInfo, nullptr, &newView);

    // Retain old texture
    if (entry.image != VK_NULL_HANDLE) {
      textureToDelete[frameIndex].push_back({entry.image, entry.imageView, entry.memory});
    }

    // Swap
    entry.image = work.newImage;
    entry.imageView = newView;
    entry.memory = work.newMemory;
    entry.currentMip = work.targetMip;
    entry.inFlight = false;  // work item consumed; next frame may dispatch again

    // Update bindless array
    textures[texIndex].first = work.newImage;
    textures[texIndex].second = newView;
    textureMemory[texIndex] = work.newMemory;

    // Clean up staging
    if (work.stagingBuf != VK_NULL_HANDLE) {
      vkDestroyBuffer(device.getLogicalDevice(), work.stagingBuf, nullptr);
      vkFreeMemory(device.getLogicalDevice(), work.stagingMem, nullptr);
    }
  }

  streamingDescriptorsDirtyMask = (framesInFlight >= 32) ? ~0u : ((1u << framesInFlight) - 1u);
}

void GpuScene::UpdateTextureStreaming(int frameIndex) {
  // 1. Reset all requiredMip to botMip (coarsest)
  for (auto& entry : streamingEntries) {
    entry.requiredMip = calculateMinMip(*entry.desc, PERMANENT_TEXTURE_SIZE);
  }

  // 2. Per-submesh coarse path (mirrors Metal's AAPLRenderer.mm:2318-2353).
  //    Uses pre-computed submesh bounding spheres instead of per-chunk spheres —
  //    meshCount is typically ~60x smaller than chunkCount for Bistro.
  Camera* cam = maincamera;
  if (!cam || !m_SubMeshes || !applMesh || !cpuMaterials) return;

  mat4 viewMatrix = cam->getObjectToCamera();
  mat4 projMatrix = cam->getProjectMatrix();
  float focalLength = projMatrix[0][0];
  float focalLengthSquared = focalLength * focalLength;
  float viewW = static_cast<float>(device.getSwapChainExtent().width);
  float viewH = static_cast<float>(device.getSwapChainExtent().height);
  Frustum frustum = cam->getFrustum();

  for (unsigned int i = 0; i < applMesh->_meshCount; ++i) {
    const AAPLSubMesh& mesh = m_SubMeshes[i];
    const AAPLSphere& sphere = mesh.boundingSphere;

    // Frustum cull the submesh bounding sphere
    if (!sphereInFrustum(frustum, sphere)) continue;

    // Metal sphere-to-screen-area projection
    vec4 viewPos = viewMatrix * vec4(sphere.data.x, sphere.data.y, sphere.data.z, 1.0f);
    float area;

    if (viewPos.z <= sphere.data.w) {
      area = viewW * viewH;
    } else {
      float radiusSquared = sphere.data.w * sphere.data.w;
      float z2 = viewPos.z * viewPos.z;
      float l2 = viewPos.x * viewPos.x + viewPos.y * viewPos.y + viewPos.z * viewPos.z;

      area = -M_PI_F * focalLengthSquared * radiusSquared
             * sqrt(fabsf((l2 - radiusSquared) / (radiusSquared - z2)))
             / (radiusSquared - z2);
      area *= viewW * viewH * 0.25f;
    }

    uint32_t matIndex = mesh.materialIndex;
    if (matIndex >= applMesh->_materialCount) continue;

    const AAPLMaterial& cpuMat = cpuMaterials[matIndex];

    if (cpuMat.hasBaseColorTexture)
      setRequiredMip(cpuMat.baseColorTextureHash, area);
    if (cpuMat.hasNormalMap)
      setRequiredMip(cpuMat.normalMapHash, area);
    if (cpuMat.hasMetallicRoughnessTexture)
      setRequiredMip(cpuMat.metallicRoughnessHash, area);
    if (cpuMat.hasEmissiveTexture)
      setRequiredMip(cpuMat.emissiveTextureHash, area);
  }

  // Snapshot the images scheduled for retirement this frame and clear the slot
  // BEFORE dispatching or calling processStreamingWork.  processStreamingWork
  // may reference these same images as sourceImage (if the background thread was
  // slow), so they must stay alive until after endSingleTimeCommands returns.
  // processStreamingWork also adds newly-retired images to textureToDelete[frameIndex],
  // so clearing the slot first ensures those are NOT freed this frame.
  std::vector<std::tuple<VkImage, VkImageView, VkDeviceMemory>> toFreeThisFrame;
  std::swap(toFreeThisFrame, textureToDelete[frameIndex]);

  // 4. Dispatch streaming requests (skip entries that already have a work item in flight)
  for (size_t i = 0; i < streamingEntries.size(); ++i) {
    auto& entry = streamingEntries[i];
    if (entry.currentMip != entry.requiredMip && !entry.inFlight) {
      dispatchStreamingRequest(i);
    }
  }

  // 5. Process completed streaming work (GPU commands on main thread).
  //    May use images from toFreeThisFrame as copy sources — they are still valid.
  processStreamingWork(frameIndex);

  // 3. Release old textures now that the GPU is done with them
  //    (endSingleTimeCommands inside processStreamingWork called vkQueueWaitIdle).
  for (auto& [img, view, mem] : toFreeThisFrame) {
    vkDestroyImageView(device.getLogicalDevice(), view, nullptr);
    vkDestroyImage(device.getLogicalDevice(), img, nullptr);
    vkFreeMemory(device.getLogicalDevice(), mem, nullptr);
  }
}

std::pair<VkImageView, VkDeviceMemory>
GpuScene::createTexture(const std::string &path) {
  VkImage textureImage;
  VkImageView currentImage;
  int texWidth, texHeight, texChannels;
#ifdef __ANDROID__
  auto texData = AssetLoader::loadAssetBytes(path);
  stbi_uc *pixels = stbi_load_from_memory(texData.data(), (int)texData.size(),
                                          &texWidth, &texHeight, &texChannels,
                                          STBI_rgb_alpha);
#else
  stbi_uc *pixels = stbi_load(path.c_str(), &texWidth, &texHeight, &texChannels,
                              STBI_rgb_alpha);
#endif
  VkDeviceSize imageSize = texWidth * texHeight * 4;

  if (!pixels) {
    throw std::runtime_error("failed to load texture image!");
  }

  VkBuffer stagingBuffer;
  VkDeviceMemory stagingBufferMemory;
  createBuffer(imageSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                   VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
               stagingBuffer, stagingBufferMemory);

  void *data;
  vkMapMemory(device.getLogicalDevice(), stagingBufferMemory, 0, imageSize, 0,
              &data);
  memcpy(data, pixels, static_cast<size_t>(imageSize));
  vkUnmapMemory(device.getLogicalDevice(), stagingBufferMemory);

  stbi_image_free(pixels);

  VkImageCreateInfo imageInfo{};
  imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  imageInfo.imageType = VK_IMAGE_TYPE_2D;
  imageInfo.extent.width = texWidth;
  imageInfo.extent.height = texHeight;
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

  if (vkCreateImage(device.getLogicalDevice(), &imageInfo, nullptr,
                    &textureImage) != VK_SUCCESS) {
    throw std::runtime_error("failed to create image!");
  }

  VkMemoryRequirements memRequirements;
  vkGetImageMemoryRequirements(device.getLogicalDevice(), textureImage,
                               &memRequirements);

  VkMemoryAllocateInfo allocInfo{};
  allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  allocInfo.allocationSize = memRequirements.size;
  allocInfo.memoryTypeIndex = device.findMemoryType(
      memRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

  VkDeviceMemory textureImageMemory;
  if (vkAllocateMemory(device.getLogicalDevice(), &allocInfo, nullptr,
                       &textureImageMemory) != VK_SUCCESS) {
    throw std::runtime_error("failed to allocate image memory!");
  }

  vkBindImageMemory(device.getLogicalDevice(), textureImage, textureImageMemory,
                    0);

  device.transitionImageLayout(textureImage, VK_FORMAT_R8G8B8A8_SRGB,
                               VK_IMAGE_LAYOUT_UNDEFINED,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
  device.copyBufferToImage(stagingBuffer, textureImage,
                           static_cast<uint32_t>(texWidth),
                           static_cast<uint32_t>(texHeight));
  device.transitionImageLayout(textureImage, VK_FORMAT_R8G8B8A8_SRGB,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

  vkDestroyBuffer(device.getLogicalDevice(), stagingBuffer, nullptr);
  vkFreeMemory(device.getLogicalDevice(), stagingBufferMemory, nullptr);

  VkImageViewCreateInfo imageviewInfo{};
  imageviewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  imageviewInfo.image = textureImage;
  imageviewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;

  imageviewInfo.format = VK_FORMAT_R8G8B8A8_SRGB;
  imageviewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  imageviewInfo.subresourceRange.baseMipLevel = 0;
  imageviewInfo.subresourceRange.levelCount =
      1; // texturedata._mipmapLevelCount;
  imageviewInfo.subresourceRange.baseArrayLayer = 0;
  imageviewInfo.subresourceRange.layerCount = 1;

  // VkImageView imageView;
  if (vkCreateImageView(device.getLogicalDevice(), &imageviewInfo, nullptr,
                        &currentImage) != VK_SUCCESS) {
    throw std::runtime_error("failed to create texture image view!");
  }
  return std::make_pair(currentImage, textureImageMemory);
}

std::pair<VkImage, VkImageView>
GpuScene::createTexture(const AAPLTextureData &texturedata) {
  VkImageView currentImage;
  VkImage textureImage;

  VkImageCreateInfo imageInfo{};
  imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  imageInfo.imageType = VK_IMAGE_TYPE_2D;
  imageInfo.extent.width = texturedata._width;
  imageInfo.extent.height = texturedata._height;
  imageInfo.extent.depth = 1;
  imageInfo.mipLevels = texturedata._mipmapLevelCount;
  imageInfo.arrayLayers = 1;
  imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL; // TODO: switch to linear with
                                              // initiallayout=preinitialized?
  imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  imageInfo.format = mapFromApple((MTLPixelFormat)(texturedata._pixelFormat));
  imageInfo.usage =
      VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
  imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
  imageInfo.flags = 0; // Optional

  if (vkCreateImage(device.getLogicalDevice(), &imageInfo, nullptr,
                    &textureImage) != VK_SUCCESS) {
    throw std::runtime_error("failed to create image!");
  }

  VkMemoryRequirements memRequirements;
  vkGetImageMemoryRequirements(device.getLogicalDevice(), textureImage,
                               &memRequirements);

  VkMemoryAllocateInfo allocInfo{};
  allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  allocInfo.allocationSize = memRequirements.size;
  allocInfo.memoryTypeIndex = device.findMemoryType(
      memRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

  VkDeviceMemory textureImageMemory;
  if (vkAllocateMemory(device.getLogicalDevice(), &allocInfo, nullptr,
                       &textureImageMemory) != VK_SUCCESS) {
    throw std::runtime_error("failed to allocate image memory!");
  }
  vkBindImageMemory(device.getLogicalDevice(), textureImage, textureImageMemory,
                    0);

  device.transitionImageLayout(
      textureImage, mapFromApple((MTLPixelFormat)(texturedata._pixelFormat)),
      VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
      texturedata._mipmapLevelCount);
  for (int miplevel = 0; miplevel < texturedata._mipmapLevelCount; ++miplevel) {
    unsigned int rawDataLength = 0;
    void *pixelDataRaw = loadMipTexture(texturedata, miplevel, rawDataLength);

    // dds_image_t ddsimage = dds_load_from_memory((const char*)pixelDataRaw,
    // rawDataLength); spdlog::info("ddsimage info {}, {}",
    // ddsimage->header.width, ddsimage->header.height);

    VkBuffer stagingBuffer;
    VkDeviceMemory stagingBufferMemory;
    createBuffer(rawDataLength, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                     VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                 stagingBuffer, stagingBufferMemory);

    void *mappedData;
    vkMapMemory(device.getLogicalDevice(), stagingBufferMemory, 0,
                rawDataLength, 0, &mappedData);
    memcpy(mappedData, pixelDataRaw, static_cast<size_t>(rawDataLength));
    vkUnmapMemory(device.getLogicalDevice(), stagingBufferMemory);

    free(pixelDataRaw);

    device.copyBufferToImage(
        stagingBuffer, textureImage,
        static_cast<uint32_t>(texturedata._width) >> miplevel,
        static_cast<uint32_t>(texturedata._height) >> miplevel, miplevel);

    vkDestroyBuffer(device.getLogicalDevice(), stagingBuffer, nullptr);
    vkFreeMemory(device.getLogicalDevice(), stagingBufferMemory, nullptr);
  }
  device.transitionImageLayout(
      textureImage, mapFromApple((MTLPixelFormat)(texturedata._pixelFormat)),
      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, texturedata._mipmapLevelCount);

  VkImageViewCreateInfo imageviewInfo{};
  imageviewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  imageviewInfo.image = textureImage;
  imageviewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
  imageviewInfo.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
  imageviewInfo.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
  imageviewInfo.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
  imageviewInfo.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
  imageviewInfo.format =
      mapFromApple((MTLPixelFormat)(texturedata._pixelFormat));
  ;
  imageviewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  imageviewInfo.subresourceRange.baseMipLevel = 0;
  imageviewInfo.subresourceRange.levelCount = texturedata._mipmapLevelCount;
  imageviewInfo.subresourceRange.baseArrayLayer = 0;
  imageviewInfo.subresourceRange.layerCount = 1;

  // VkImageView imageView;
  if (vkCreateImageView(device.getLogicalDevice(), &imageviewInfo, nullptr,
                        &currentImage) != VK_SUCCESS) {
    throw std::runtime_error("failed to create texture image view!");
  }

  return std::make_pair(textureImage, currentImage);
}

bool updated = false;
void GpuScene::updateSamplerInDescriptors(VkImageView currentImage) {
  if (updated)
    return;
  updated = true;
  for (uint32_t f = 0; f < framesInFlight; ++f) {
    VkDescriptorImageInfo imageinfo;
    imageinfo.imageView = currentImage;
    imageinfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imageinfo.sampler = textureSampler;

    VkWriteDescriptorSet setWrite = {};
    setWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    setWrite.pNext = nullptr;
    setWrite.dstBinding = 1;
    setWrite.dstSet = globalDescriptorSets[f];
    setWrite.descriptorCount = 1;
    setWrite.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    setWrite.pImageInfo = &imageinfo;

    vkUpdateDescriptorSets(device.getLogicalDevice(), 1, &setWrite, 0, nullptr);
  }
}

void GpuScene::createOccluderWireframePipeline() {
  auto vsCode =
      readFile((_rootPath / "shaders/occluders.wireframe.vs.spv").generic_string());
  auto psCode = readFile(
      (_rootPath / "shaders/occluders.wireframe.ps.spv").generic_string());

  VkShaderModule vsModule = createShaderModule(vsCode);
  VkShaderModule psModule = createShaderModule(psCode);

  VkPipelineShaderStageCreateInfo vsStage{};
  vsStage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  vsStage.stage = VK_SHADER_STAGE_VERTEX_BIT;
  vsStage.module = vsModule;
  vsStage.pName = "RenderSceneVS";

  VkPipelineShaderStageCreateInfo psStage{};
  psStage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  psStage.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  psStage.module = psModule;
  psStage.pName = "WireframePS";

  VkPipelineShaderStageCreateInfo stages[] = {vsStage, psStage};

  VkVertexInputBindingDescription binding = {
      .binding = 0,
      .stride = sizeof(float) * 3,
      .inputRate = VK_VERTEX_INPUT_RATE_VERTEX};
  VkVertexInputAttributeDescription attr = {
      .location = 0,
      .binding = 0,
      .format = VK_FORMAT_R32G32B32_SFLOAT,
      .offset = 0};

  VkPipelineVertexInputStateCreateInfo vertexInput{};
  vertexInput.sType =
      VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
  vertexInput.vertexBindingDescriptionCount = 1;
  vertexInput.pVertexBindingDescriptions = &binding;
  vertexInput.vertexAttributeDescriptionCount = 1;
  vertexInput.pVertexAttributeDescriptions = &attr;

  VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
  inputAssembly.sType =
      VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
  inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

  VkViewport viewport{};
  viewport.width = (float)device.getSwapChainExtent().width;
  viewport.height = (float)device.getSwapChainExtent().height;
  viewport.maxDepth = 1.0f;
  VkRect2D scissor{{0, 0}, device.getSwapChainExtent()};

  VkPipelineViewportStateCreateInfo viewportState{};
  viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
  viewportState.viewportCount = 1;
  viewportState.pViewports = &viewport;
  viewportState.scissorCount = 1;
  viewportState.pScissors = &scissor;

  VkPipelineRasterizationStateCreateInfo rasterizer{};
  rasterizer.sType =
      VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
  rasterizer.polygonMode = VK_POLYGON_MODE_LINE;
  rasterizer.lineWidth = 1.0f;
  rasterizer.cullMode = VK_CULL_MODE_NONE;
  rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;

  VkPipelineMultisampleStateCreateInfo multisampling{};
  multisampling.sType =
      VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
  multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

  VkPipelineDepthStencilStateCreateInfo depthStencil{};
  depthStencil.sType =
      VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
  depthStencil.depthTestEnable = VK_TRUE;
  depthStencil.depthWriteEnable = VK_FALSE;
  depthStencil.depthCompareOp = VK_COMPARE_OP_GREATER;

  VkPipelineColorBlendAttachmentState blendAttachment{};
  blendAttachment.colorWriteMask =
      VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
      VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
  blendAttachment.blendEnable = VK_FALSE;

  VkPipelineColorBlendStateCreateInfo colorBlending{};
  colorBlending.sType =
      VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
  colorBlending.attachmentCount = 1;
  colorBlending.pAttachments = &blendAttachment;

  VkGraphicsPipelineCreateInfo pipelineInfo{};
  pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
  pipelineInfo.stageCount = 2;
  pipelineInfo.pStages = stages;
  pipelineInfo.pVertexInputState = &vertexInput;
  pipelineInfo.pInputAssemblyState = &inputAssembly;
  pipelineInfo.pViewportState = &viewportState;
  pipelineInfo.pRasterizationState = &rasterizer;
  pipelineInfo.pMultisampleState = &multisampling;
  pipelineInfo.pDepthStencilState = &depthStencil;
  pipelineInfo.pColorBlendState = &colorBlending;
  pipelineInfo.layout = pipelineLayout; // reuse existing (globalSetLayout only)
  pipelineInfo.renderPass = _forwardLightingPass;
  pipelineInfo.subpass = 0;

  if (vkCreateGraphicsPipelines(device.getLogicalDevice(), VK_NULL_HANDLE, 1,
                                &pipelineInfo, nullptr,
                                &occluderWireframePipeline) != VK_SUCCESS) {
    throw std::runtime_error(
        "failed to create occluder wireframe pipeline!");
  }

  vkDestroyShaderModule(device.getLogicalDevice(), vsModule, nullptr);
  vkDestroyShaderModule(device.getLogicalDevice(), psModule, nullptr);
}

void GpuScene::drawOccludersWireframe(VkCommandBuffer commandBuffer) {
  if (occluderWireframePipeline == VK_NULL_HANDLE)
    return;

  vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                    occluderWireframePipeline);

  VkBuffer vertexBuffers[] = {_occludersVertBuffer};
  VkDeviceSize offsets[] = {0};
  vkCmdBindVertexBuffers(commandBuffer, 0, 1, vertexBuffers, offsets);
  vkCmdBindIndexBuffer(commandBuffer, _occludersIndexBuffer, 0,
                       VK_INDEX_TYPE_UINT32);
  vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          pipelineLayout, 0, 1, &globalDescriptorSets[currentFrame], 0,
                          nullptr);
  vkCmdDrawIndexed(commandBuffer, sceneFile["occluder_indices"].size(), 1, 0, 0,
                   0);
}

// --- Scalable Ambient Obscurance (SAO) ---

void GpuScene::createSAOResources() {
  uint32_t width = device.getSwapChainExtent().width;
  uint32_t height = device.getSwapChainExtent().height;
  _saoWidth = width;
  _saoHeight = height;
  _saoMipLevels = (uint32_t)floor(log2f((float)(width > height ? width : height))) + 1;

  // --- 1. SAO depth pyramid texture (same resolution as screen, R32_SFLOAT, full mip chain) ---
  {
    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = VK_FORMAT_R32_SFLOAT;
    imageInfo.extent = {width, height, 1};
    imageInfo.mipLevels = _saoMipLevels;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    _saoDepthPyramid.resize(framesInFlight);
    _saoDepthPyramidMemory.resize(framesInFlight);
    _saoDepthPyramidView.resize(framesInFlight);
    _saoMipViews.resize(framesInFlight);

    for (uint32_t f = 0; f < framesInFlight; ++f) {
    vkCreateImage(device.getLogicalDevice(), &imageInfo, nullptr, &_saoDepthPyramid[f]);

    VkMemoryRequirements memReq;
    vkGetImageMemoryRequirements(device.getLogicalDevice(), _saoDepthPyramid[f], &memReq);
    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memReq.size;
    allocInfo.memoryTypeIndex = device.findMemoryType(memReq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    vkAllocateMemory(device.getLogicalDevice(), &allocInfo, nullptr, &_saoDepthPyramidMemory[f]);
    vkBindImageMemory(device.getLogicalDevice(), _saoDepthPyramid[f], _saoDepthPyramidMemory[f], 0);

    // Full mip chain view for sampling in SAO shader
    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = _saoDepthPyramid[f];
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = VK_FORMAT_R32_SFLOAT;
    viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, _saoMipLevels, 0, 1};
    vkCreateImageView(device.getLogicalDevice(), &viewInfo, nullptr, &_saoDepthPyramidView[f]);

    // Per-mip views for compute writes
    _saoMipViews[f].resize(_saoMipLevels);
    for (uint32_t i = 0; i < _saoMipLevels; ++i) {
      viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, i, 1, 0, 1};
      vkCreateImageView(device.getLogicalDevice(), &viewInfo, nullptr, &_saoMipViews[f][i]);
    }
    } // end per-frame
  }

  // --- 2. AO output texture (R8_UNORM, full resolution) ---
  {
    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = VK_FORMAT_R8_UNORM;
    imageInfo.extent = {width, height, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    _aoTexture.resize(framesInFlight);
    _aoTextureMemory.resize(framesInFlight);
    _aoTextureView.resize(framesInFlight);

    for (uint32_t f = 0; f < framesInFlight; ++f) {
    vkCreateImage(device.getLogicalDevice(), &imageInfo, nullptr, &_aoTexture[f]);

    VkMemoryRequirements memReq;
    vkGetImageMemoryRequirements(device.getLogicalDevice(), _aoTexture[f], &memReq);
    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memReq.size;
    allocInfo.memoryTypeIndex = device.findMemoryType(memReq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    vkAllocateMemory(device.getLogicalDevice(), &allocInfo, nullptr, &_aoTextureMemory[f]);
    vkBindImageMemory(device.getLogicalDevice(), _aoTexture[f], _aoTextureMemory[f], 0);

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = _aoTexture[f];
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = VK_FORMAT_R8_UNORM;
    viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCreateImageView(device.getLogicalDevice(), &viewInfo, nullptr, &_aoTextureView[f]);
    } // end per-frame
  }

  // --- 3. Descriptor sets for SAO depth pyramid building (reuse HiZ pipeline) ---
  {
    uint32_t setsPerFrame = _saoMipLevels; // 1 copy + (mipLevels-1) downsample
    VkDescriptorPoolSize poolSizes[] = {
      {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, _saoMipLevels * framesInFlight},
      {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, _saoMipLevels * framesInFlight},
    };
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.poolSizeCount = 2;
    poolInfo.pPoolSizes = poolSizes;
    poolInfo.maxSets = setsPerFrame * framesInFlight;

    VkDescriptorPool saoDepthPool;
    vkCreateDescriptorPool(device.getLogicalDevice(), &poolInfo, nullptr, &saoDepthPool);

    // Copy descriptor sets: depth texture → SAO pyramid mip 0
    _saoCopyDescriptorSet.resize(framesInFlight);
    for (uint32_t f = 0; f < framesInFlight; ++f) {
      VkDescriptorSetAllocateInfo allocInfo{};
      allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
      allocInfo.descriptorPool = saoDepthPool;
      allocInfo.descriptorSetCount = 1;
      allocInfo.pSetLayouts = &_hizCopySetLayout;
      vkAllocateDescriptorSets(device.getLogicalDevice(), &allocInfo, &_saoCopyDescriptorSet[f]);

      VkDescriptorImageInfo srcInfo{};
      srcInfo.imageView = device.getWindowDepthOnlyImageView(f);
      srcInfo.imageLayout = VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL;

      VkDescriptorImageInfo dstInfo{};
      dstInfo.imageView = _saoMipViews[f][0];
      dstInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

      VkWriteDescriptorSet writes[2] = {};
      writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      writes[0].dstSet = _saoCopyDescriptorSet[f];
      writes[0].dstBinding = 0;
      writes[0].descriptorCount = 1;
      writes[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
      writes[0].pImageInfo = &srcInfo;
      writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      writes[1].dstSet = _saoCopyDescriptorSet[f];
      writes[1].dstBinding = 1;
      writes[1].descriptorCount = 1;
      writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
      writes[1].pImageInfo = &dstInfo;
      vkUpdateDescriptorSets(device.getLogicalDevice(), 2, writes, 0, nullptr);
    }

    // Downsample descriptor sets: mip[n-1] → mip[n]
    _saoDownsampleDescriptorSets.resize(framesInFlight);
    for (uint32_t f = 0; f < framesInFlight; ++f) {
      _saoDownsampleDescriptorSets[f].resize(_saoMipLevels - 1);
      for (uint32_t m = 1; m < _saoMipLevels; ++m) {
        VkDescriptorSetAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocInfo.descriptorPool = saoDepthPool;
        allocInfo.descriptorSetCount = 1;
        allocInfo.pSetLayouts = &_hizDownsampleSetLayout;
        vkAllocateDescriptorSets(device.getLogicalDevice(), &allocInfo, &_saoDownsampleDescriptorSets[f][m - 1]);

        VkDescriptorImageInfo srcInfo{};
        srcInfo.imageView = _saoMipViews[f][m - 1];
        srcInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkDescriptorImageInfo dstInfo{};
        dstInfo.imageView = _saoMipViews[f][m];
        dstInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        VkWriteDescriptorSet writes[2] = {};
        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = _saoDownsampleDescriptorSets[f][m - 1];
        writes[0].dstBinding = 0;
        writes[0].descriptorCount = 1;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        writes[0].pImageInfo = &srcInfo;
        writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet = _saoDownsampleDescriptorSets[f][m - 1];
        writes[1].dstBinding = 1;
        writes[1].descriptorCount = 1;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[1].pImageInfo = &dstInfo;
        vkUpdateDescriptorSets(device.getLogicalDevice(), 2, writes, 0, nullptr);
      }
    }
  }

  // --- 4. SAO compute pipeline ---
  {
    // Descriptor set layout: depth pyramid (0), camera cbuffer (1), AO output (2)
    VkDescriptorSetLayoutBinding bindings[3] = {};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    bindings[2].binding = 2;
    bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[2].descriptorCount = 1;
    bindings[2].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 3;
    layoutInfo.pBindings = bindings;
    vkCreateDescriptorSetLayout(device.getLogicalDevice(), &layoutInfo, nullptr, &_saoSetLayout);

    // Push constants: uint2 screenSize
    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pushRange.offset = 0;
    pushRange.size = sizeof(uint32_t) * 2;

    VkPipelineLayoutCreateInfo pipeLayoutInfo{};
    pipeLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipeLayoutInfo.setLayoutCount = 1;
    pipeLayoutInfo.pSetLayouts = &_saoSetLayout;
    pipeLayoutInfo.pushConstantRangeCount = 1;
    pipeLayoutInfo.pPushConstantRanges = &pushRange;
    vkCreatePipelineLayout(device.getLogicalDevice(), &pipeLayoutInfo, nullptr, &_saoPipelineLayout);

    // Compute pipeline
    auto shaderCode = readFile((_rootPath / "shaders/sao.cs.spv").generic_string());
    VkShaderModule shaderModule = createShaderModule(shaderCode);

    VkPipelineShaderStageCreateInfo stageInfo{};
    stageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stageInfo.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stageInfo.module = shaderModule;
    stageInfo.pName = "ScalableAmbientObscurance";

    VkComputePipelineCreateInfo pipeInfo{};
    pipeInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipeInfo.layout = _saoPipelineLayout;
    pipeInfo.stage = stageInfo;
    vkCreateComputePipelines(device.getLogicalDevice(), VK_NULL_HANDLE, 1, &pipeInfo, nullptr, &_saoPipeline);
    vkDestroyShaderModule(device.getLogicalDevice(), shaderModule, nullptr);

    // Descriptor pool and set for SAO compute
    VkDescriptorPoolSize poolSizes[] = {
      {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1 * framesInFlight},
      {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1 * framesInFlight},
      {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1 * framesInFlight},
    };
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.poolSizeCount = 3;
    poolInfo.pPoolSizes = poolSizes;
    poolInfo.maxSets = framesInFlight;
    vkCreateDescriptorPool(device.getLogicalDevice(), &poolInfo, nullptr, &_saoDescriptorPool);

    std::vector<VkDescriptorSetLayout> layouts(framesInFlight, _saoSetLayout);
    _saoDescriptorSets.resize(framesInFlight);
    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = _saoDescriptorPool;
    allocInfo.descriptorSetCount = framesInFlight;
    allocInfo.pSetLayouts = layouts.data();
    vkAllocateDescriptorSets(device.getLogicalDevice(), &allocInfo, _saoDescriptorSets.data());

    for (uint32_t i = 0; i < framesInFlight; i++) {
      // Binding 0: SAO depth pyramid (per-frame, R32_SFLOAT)
      VkDescriptorImageInfo pyramidInfo{};
      pyramidInfo.imageView = _saoDepthPyramidView[i];
      pyramidInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

      // Binding 1: camera params uniform buffer (per-frame)
      VkDescriptorBufferInfo bufferInfo{};
      bufferInfo.buffer = uniformBuffers[i];
      bufferInfo.offset = 0;
      bufferInfo.range = sizeof(FrameData);

      // Binding 2: AO output (per-frame)
      VkDescriptorImageInfo aoInfo{};
      aoInfo.imageView = _aoTextureView[i];
      aoInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

      VkWriteDescriptorSet writes[3] = {};
      writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      writes[0].dstSet = _saoDescriptorSets[i];
      writes[0].dstBinding = 0;
      writes[0].descriptorCount = 1;
      writes[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
      writes[0].pImageInfo = &pyramidInfo;
      writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      writes[1].dstSet = _saoDescriptorSets[i];
      writes[1].dstBinding = 1;
      writes[1].descriptorCount = 1;
      writes[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
      writes[1].pBufferInfo = &bufferInfo;
      writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      writes[2].dstSet = _saoDescriptorSets[i];
      writes[2].dstBinding = 2;
      writes[2].descriptorCount = 1;
      writes[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
      writes[2].pImageInfo = &aoInfo;
      vkUpdateDescriptorSets(device.getLogicalDevice(), 3, writes, 0, nullptr);
    }
  }

  spdlog::info("SAO resources created: {}x{}, {} mip levels", width, height, _saoMipLevels);

  // --- 5. Update deferred lighting descriptor set with AO texture at binding 10 ---
  {
    for (uint32_t f = 0; f < framesInFlight; ++f) {
      VkDescriptorImageInfo aoImageInfo{};
      aoImageInfo.imageView = _aoTextureView[f];
      aoImageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

      VkWriteDescriptorSet write{};
      write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      write.dstSet = deferredLightingDescriptorSet[f];
      write.dstBinding = 10;
      write.descriptorCount = 1;
      write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
      write.pImageInfo = &aoImageInfo;
      vkUpdateDescriptorSets(device.getLogicalDevice(), 1, &write, 0, nullptr);
    }
  }
}

// =============================================================================
// Resolve Pass (TAA + ACES Tone Mapping)
// =============================================================================

void GpuScene::createHDRLightingBuffer() {
  uint32_t width = device.getSwapChainExtent().width;
  uint32_t height = device.getSwapChainExtent().height;

  VkImageCreateInfo imageInfo{};
  imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  imageInfo.imageType = VK_IMAGE_TYPE_2D;
  imageInfo.extent = {width, height, 1};
  imageInfo.mipLevels = 1;
  imageInfo.arrayLayers = 1;
  imageInfo.format = VK_FORMAT_R16G16B16A16_SFLOAT;
  imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
  imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  imageInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
  imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
  imageInfo.flags = 0;

  if (vkCreateImage(device.getLogicalDevice(), &imageInfo, nullptr,
                    &_hdrLightingBuffer) != VK_SUCCESS) {
    throw std::runtime_error("failed to create HDR lighting buffer!");
  }

  VkMemoryRequirements memReq;
  vkGetImageMemoryRequirements(device.getLogicalDevice(), _hdrLightingBuffer, &memReq);

  VkMemoryAllocateInfo allocInfo{};
  allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  allocInfo.allocationSize = memReq.size;
  allocInfo.memoryTypeIndex = device.findMemoryType(
      memReq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

  if (vkAllocateMemory(device.getLogicalDevice(), &allocInfo, nullptr,
                       &_hdrLightingBufferMemory) != VK_SUCCESS) {
    throw std::runtime_error("failed to allocate HDR lighting buffer memory!");
  }

  vkBindImageMemory(device.getLogicalDevice(), _hdrLightingBuffer,
                    _hdrLightingBufferMemory, 0);

  VkImageViewCreateInfo viewInfo{};
  viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  viewInfo.image = _hdrLightingBuffer;
  viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
  viewInfo.format = VK_FORMAT_R16G16B16A16_SFLOAT;
  viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

  if (vkCreateImageView(device.getLogicalDevice(), &viewInfo, nullptr,
                        &_hdrLightingBufferView) != VK_SUCCESS) {
    throw std::runtime_error("failed to create HDR lighting buffer view!");
  }
}

void GpuScene::createTAAHistoryBuffer() {
  uint32_t width = device.getSwapChainExtent().width;
  uint32_t height = device.getSwapChainExtent().height;

  VkImageCreateInfo imageInfo{};
  imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  imageInfo.imageType = VK_IMAGE_TYPE_2D;
  imageInfo.extent = {width, height, 1};
  imageInfo.mipLevels = 1;
  imageInfo.arrayLayers = 1;
  imageInfo.format = VK_FORMAT_R8G8B8A8_SRGB;
  imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
  imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  imageInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
  imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
  imageInfo.flags = 0;

  // Ping-pong: two history slots.
  for (int s = 0; s < 2; ++s) {
    if (vkCreateImage(device.getLogicalDevice(), &imageInfo, nullptr,
                      &_taaHistoryBuffer[s]) != VK_SUCCESS) {
      throw std::runtime_error("failed to create TAA history buffer!");
    }

    VkMemoryRequirements memReq;
    vkGetImageMemoryRequirements(device.getLogicalDevice(), _taaHistoryBuffer[s], &memReq);

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memReq.size;
    allocInfo.memoryTypeIndex = device.findMemoryType(
        memReq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    if (vkAllocateMemory(device.getLogicalDevice(), &allocInfo, nullptr,
                         &_taaHistoryBufferMemory[s]) != VK_SUCCESS) {
      throw std::runtime_error("failed to allocate TAA history buffer memory!");
    }

    vkBindImageMemory(device.getLogicalDevice(), _taaHistoryBuffer[s],
                      _taaHistoryBufferMemory[s], 0);

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = _taaHistoryBuffer[s];
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = VK_FORMAT_R8G8B8A8_SRGB;
    viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    if (vkCreateImageView(device.getLogicalDevice(), &viewInfo, nullptr,
                          &_taaHistoryBufferView[s]) != VK_SUCCESS) {
      throw std::runtime_error("failed to create TAA history buffer view!");
    }

    // Both slots must be in SHADER_READ_ONLY_OPTIMAL on the first frame, since
    // the resolve descriptor declares that layout for binding 1 and the
    // validator checks it on bind even if the shader skips the sample (it does
    // on _taaFirstFrame). The resolve render pass takes them from
    // SHADER_READ_ONLY -> COLOR_ATTACHMENT for the write subpass and back to
    // SHADER_READ_ONLY at finalLayout so the steady-state loop is consistent.
    device.transitionImageLayout(_taaHistoryBuffer[s], VK_FORMAT_R8G8B8A8_SRGB,
                                 VK_IMAGE_LAYOUT_UNDEFINED,
                                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  }
}

void GpuScene::createLinearClampSampler() {
  VkSamplerCreateInfo samplerInfo{};
  samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
  samplerInfo.magFilter = VK_FILTER_LINEAR;
  samplerInfo.minFilter = VK_FILTER_LINEAR;
  samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  samplerInfo.anisotropyEnable = VK_FALSE;
  samplerInfo.unnormalizedCoordinates = VK_FALSE;
  samplerInfo.compareEnable = VK_FALSE;
  samplerInfo.compareOp = VK_COMPARE_OP_ALWAYS;
  samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;

  if (vkCreateSampler(device.getLogicalDevice(), &samplerInfo, nullptr,
                      &_linearClampSampler) != VK_SUCCESS) {
    throw std::runtime_error("failed to create linear clamp sampler!");
  }
}

void GpuScene::createResolvePass() {
  // Attachment 0: swapchain image
  VkAttachmentDescription swapchainAttachment{};
  swapchainAttachment.format = device.getSwapChainImageFormat();
  swapchainAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
  swapchainAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  swapchainAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  swapchainAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  swapchainAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  swapchainAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  swapchainAttachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

  // Attachment 1: TAA history buffer
  VkAttachmentDescription historyAttachment{};
  historyAttachment.format = VK_FORMAT_R8G8B8A8_SRGB;
  historyAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
  historyAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  historyAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  historyAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  historyAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  // Ping-pong: createTAAHistoryBuffer transitions both slots to SHADER_READ_ONLY
  // on init and finalLayout below puts them back there each frame, so initialLayout
  // can require it. (We don't preserve contents — DONT_CARE — but the read slot
  // is the other image, which keeps its contents from the previous frame.)
  historyAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  historyAttachment.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

  VkAttachmentReference colorRefs[2] = {};
  colorRefs[0].attachment = 0;
  colorRefs[0].layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  colorRefs[1].attachment = 1;
  colorRefs[1].layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

  VkSubpassDescription subpass{};
  subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  subpass.colorAttachmentCount = 2;
  subpass.pColorAttachments = colorRefs;

  VkSubpassDependency dependency{};
  dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
  dependency.dstSubpass = 0;
  dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
  dependency.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
  dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

  VkAttachmentDescription attachments[] = {swapchainAttachment, historyAttachment};

  VkRenderPassCreateInfo renderPassInfo{};
  renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
  renderPassInfo.attachmentCount = 2;
  renderPassInfo.pAttachments = attachments;
  renderPassInfo.subpassCount = 1;
  renderPassInfo.pSubpasses = &subpass;
  renderPassInfo.dependencyCount = 1;
  renderPassInfo.pDependencies = &dependency;

  if (vkCreateRenderPass(device.getLogicalDevice(), &renderPassInfo, nullptr,
                         &_resolvePass) != VK_SUCCESS) {
    throw std::runtime_error("failed to create resolve render pass!");
  }
}

void GpuScene::createResolveFrameBuffer(uint32_t count) {
  _resolveFrameBuffer.resize(count * 2);
  for (uint32_t i = 0; i < count; i++) {
    for (uint32_t s = 0; s < 2; ++s) {
      VkImageView attachments[] = {device.getSwapChainImageView(i),
                                   _taaHistoryBufferView[s]};

      VkFramebufferCreateInfo framebufferInfo{};
      framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
      framebufferInfo.renderPass = _resolvePass;
      framebufferInfo.attachmentCount = 2;
      framebufferInfo.pAttachments = attachments;
      framebufferInfo.width = device.getSwapChainExtent().width;
      framebufferInfo.height = device.getSwapChainExtent().height;
      framebufferInfo.layers = 1;

      if (vkCreateFramebuffer(device.getLogicalDevice(), &framebufferInfo,
                              nullptr, &_resolveFrameBuffer[i * 2 + s]) != VK_SUCCESS) {
        throw std::runtime_error("failed to create resolve framebuffer!");
      }
    }
  }
}

void GpuScene::createResolveDescriptors() {
  // Set layout: bindings for resolve pass textures
  VkDescriptorSetLayoutBinding bindings[5] = {};

  bindings[0].binding = 0; // HDR lighting buffer
  bindings[0].descriptorCount = 1;
  bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  bindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

  bindings[1].binding = 1; // History texture
  bindings[1].descriptorCount = 1;
  bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

  bindings[2].binding = 2; // Depth texture (per-frame)
  bindings[2].descriptorCount = 1;
  bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  bindings[2].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

  bindings[3].binding = 3; // Nearest clamp sampler
  bindings[3].descriptorCount = 1;
  bindings[3].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
  bindings[3].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

  bindings[4].binding = 4; // Linear clamp sampler
  bindings[4].descriptorCount = 1;
  bindings[4].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
  bindings[4].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

  VkDescriptorSetLayoutCreateInfo layoutInfo{};
  layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  layoutInfo.bindingCount = 5;
  layoutInfo.pBindings = bindings;

  vkCreateDescriptorSetLayout(device.getLogicalDevice(), &layoutInfo, nullptr,
                              &_resolveSetLayout);

  // Descriptor pool sized for per-frame-in-flight x per-history-slot sets.
  uint32_t frameCount = framesInFlight;
  uint32_t setCount = frameCount * 2;
  VkDescriptorPoolSize poolSizes[] = {
      {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 3 * setCount},
      {VK_DESCRIPTOR_TYPE_SAMPLER, 2 * setCount},
  };

  VkDescriptorPoolCreateInfo poolInfo{};
  poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  poolInfo.maxSets = setCount;
  poolInfo.poolSizeCount = 2;
  poolInfo.pPoolSizes = poolSizes;

  vkCreateDescriptorPool(device.getLogicalDevice(), &poolInfo, nullptr,
                         &_resolveDescriptorPool);

  // Allocate one descriptor set per (inflight frame, history write slot).
  // Index layout: frame * 2 + writeSlot. historyTex on slot W reads slot 1-W.
  std::vector<VkDescriptorSetLayout> layouts(setCount, _resolveSetLayout);
  VkDescriptorSetAllocateInfo allocInfo{};
  allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  allocInfo.descriptorPool = _resolveDescriptorPool;
  allocInfo.descriptorSetCount = setCount;
  allocInfo.pSetLayouts = layouts.data();

  _resolveDescriptorSets.resize(setCount);
  vkAllocateDescriptorSets(device.getLogicalDevice(), &allocInfo,
                           _resolveDescriptorSets.data());

  VkDescriptorImageInfo hdrInfo{};
  hdrInfo.imageView = _hdrLightingBufferView;
  hdrInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

  VkDescriptorImageInfo nearestSamplerInfo{};
  nearestSamplerInfo.sampler = nearestClampSampler;

  VkDescriptorImageInfo linearSamplerInfo{};
  linearSamplerInfo.sampler = _linearClampSampler;

  for (uint32_t f = 0; f < frameCount; ++f) {
    VkDescriptorImageInfo depthInfo{};
    depthInfo.imageView = device.getWindowDepthOnlyImageView(f);
    depthInfo.imageLayout = VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL;

    for (uint32_t w = 0; w < 2; ++w) {
      VkDescriptorImageInfo historyInfoLocal{};
      historyInfoLocal.imageView = _taaHistoryBufferView[1 - w]; // read other
      historyInfoLocal.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

      VkWriteDescriptorSet writes[5] = {};
      writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      writes[0].dstSet = _resolveDescriptorSets[f * 2 + w];
      writes[0].dstBinding = 0;
      writes[0].descriptorCount = 1;
      writes[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
      writes[0].pImageInfo = &hdrInfo;

      writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      writes[1].dstSet = _resolveDescriptorSets[f * 2 + w];
      writes[1].dstBinding = 1;
      writes[1].descriptorCount = 1;
      writes[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
      writes[1].pImageInfo = &historyInfoLocal;

      writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      writes[2].dstSet = _resolveDescriptorSets[f * 2 + w];
      writes[2].dstBinding = 2;
      writes[2].descriptorCount = 1;
      writes[2].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
      writes[2].pImageInfo = &depthInfo;

      writes[3].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      writes[3].dstSet = _resolveDescriptorSets[f * 2 + w];
      writes[3].dstBinding = 3;
      writes[3].descriptorCount = 1;
      writes[3].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
      writes[3].pImageInfo = &nearestSamplerInfo;

      writes[4].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      writes[4].dstSet = _resolveDescriptorSets[f * 2 + w];
      writes[4].dstBinding = 4;
      writes[4].descriptorCount = 1;
      writes[4].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
      writes[4].pImageInfo = &linearSamplerInfo;

      vkUpdateDescriptorSets(device.getLogicalDevice(), 5, writes, 0, nullptr);
    }
  }
}

void GpuScene::createResolvePipeline() {
  auto resolvePSCode = readFile(
      (_rootPath / "shaders/resolve.ps.spv").generic_string());
  auto resolveVSCode = readFile(
      (_rootPath / "shaders/deferredlighting.vs.spv").generic_string());

  VkShaderModule resolveVSModule = createShaderModule(resolveVSCode);
  VkShaderModule resolvePSModule = createShaderModule(resolvePSCode);

  VkPipelineShaderStageCreateInfo vsStage{};
  vsStage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  vsStage.stage = VK_SHADER_STAGE_VERTEX_BIT;
  vsStage.module = resolveVSModule;
  vsStage.pName = "AAPLSimpleTexVertexOutFSQuadVertexShader";

  VkPipelineShaderStageCreateInfo psStage{};
  psStage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  psStage.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  psStage.module = resolvePSModule;
  psStage.pName = "ResolvePS";

  VkPipelineShaderStageCreateInfo stages[] = {vsStage, psStage};

  // Pipeline layout: {globalSetLayout, _resolveSetLayout}
  VkDescriptorSetLayout resolveLayouts[] = {globalSetLayout, _resolveSetLayout};
  VkPipelineLayoutCreateInfo layoutInfo{};
  layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  layoutInfo.setLayoutCount = 2;
  layoutInfo.pSetLayouts = resolveLayouts;

  if (vkCreatePipelineLayout(device.getLogicalDevice(), &layoutInfo, nullptr,
                             &_resolvePipelineLayout) != VK_SUCCESS) {
    throw std::runtime_error("failed to create resolve pipeline layout!");
  }

  // Empty vertex input
  VkPipelineVertexInputStateCreateInfo vertexInput{};
  vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

  VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
  inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
  inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  inputAssembly.primitiveRestartEnable = VK_FALSE;

  VkViewport viewport{};
  viewport.x = 0.0f;
  viewport.y = 0.0f;
  viewport.width = (float)device.getSwapChainExtent().width;
  viewport.height = (float)device.getSwapChainExtent().height;
  viewport.minDepth = 0.0f;
  viewport.maxDepth = 1.0f;

  VkRect2D scissor{};
  scissor.offset = {0, 0};
  scissor.extent = device.getSwapChainExtent();

  VkPipelineViewportStateCreateInfo viewportState{};
  viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
  viewportState.viewportCount = 1;
  viewportState.pViewports = &viewport;
  viewportState.scissorCount = 1;
  viewportState.pScissors = &scissor;

  VkPipelineRasterizationStateCreateInfo rasterizer{};
  rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
  rasterizer.depthClampEnable = VK_FALSE;
  rasterizer.rasterizerDiscardEnable = VK_FALSE;
  rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
  rasterizer.lineWidth = 1.0f;
  rasterizer.cullMode = VK_CULL_MODE_BACK_BIT;
  rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
  rasterizer.depthBiasEnable = VK_FALSE;

  VkPipelineMultisampleStateCreateInfo multisampling{};
  multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
  multisampling.sampleShadingEnable = VK_FALSE;
  multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

  // No depth test
  VkPipelineDepthStencilStateCreateInfo depthStencil{};
  depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
  depthStencil.depthTestEnable = VK_FALSE;
  depthStencil.depthWriteEnable = VK_FALSE;

  // Two color blend attachments (MRT)
  VkPipelineColorBlendAttachmentState blendAttachments[2] = {};
  blendAttachments[0].colorWriteMask = VK_COLOR_COMPONENT_R_BIT |
      VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
  blendAttachments[0].blendEnable = VK_FALSE;
  blendAttachments[1] = blendAttachments[0];

  VkPipelineColorBlendStateCreateInfo colorBlendState{};
  colorBlendState.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
  colorBlendState.logicOpEnable = VK_FALSE;
  colorBlendState.attachmentCount = 2;
  colorBlendState.pAttachments = blendAttachments;

  VkGraphicsPipelineCreateInfo pipelineInfo{};
  pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
  pipelineInfo.stageCount = 2;
  pipelineInfo.pStages = stages;
  pipelineInfo.pVertexInputState = &vertexInput;
  pipelineInfo.pInputAssemblyState = &inputAssembly;
  pipelineInfo.pViewportState = &viewportState;
  pipelineInfo.pRasterizationState = &rasterizer;
  pipelineInfo.pMultisampleState = &multisampling;
  pipelineInfo.pDepthStencilState = &depthStencil;
  pipelineInfo.pColorBlendState = &colorBlendState;
  pipelineInfo.layout = _resolvePipelineLayout;
  pipelineInfo.renderPass = _resolvePass;
  pipelineInfo.subpass = 0;
  pipelineInfo.basePipelineHandle = VK_NULL_HANDLE;

  if (vkCreateGraphicsPipelines(device.getLogicalDevice(), VK_NULL_HANDLE, 1,
                                &pipelineInfo, nullptr,
                                &_resolvePipeline) != VK_SUCCESS) {
    throw std::runtime_error("failed to create resolve pipeline!");
  }

  vkDestroyShaderModule(device.getLogicalDevice(), resolveVSModule, nullptr);
  vkDestroyShaderModule(device.getLogicalDevice(), resolvePSModule, nullptr);
}

void GpuScene::generateSAODepthPyramid(VkCommandBuffer commandBuffer) {
  if (_hizCopyPipeline == VK_NULL_HANDLE || _saoMipLevels == 0)
    return;

  // Transition SAO pyramid to GENERAL for writes
  {
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = _saoDepthPyramid[currentFrame];
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, _saoMipLevels, 0, 1};
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
  }

  // Copy depth → mip 0
  {
    uint32_t pushData[4] = {_saoWidth, _saoHeight, _saoWidth, _saoHeight};
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, _hizCopyPipeline);
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
        _hizPipelineLayout, 0, 1, &_saoCopyDescriptorSet[currentFrame], 0, nullptr);
    vkCmdPushConstants(commandBuffer, _hizPipelineLayout,
        VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pushData), pushData);
    vkCmdDispatch(commandBuffer, (_saoWidth + 7) / 8, (_saoHeight + 7) / 8, 1);
  }

  // Downsample mip chain
  for (uint32_t mip = 1; mip < _saoMipLevels; ++mip) {
    uint32_t mipW = (_saoWidth >> mip) > 1 ? (_saoWidth >> mip) : 1;
    uint32_t mipH = (_saoHeight >> mip) > 1 ? (_saoHeight >> mip) : 1;
    uint32_t prevW = (_saoWidth >> (mip - 1)) > 1 ? (_saoWidth >> (mip - 1)) : 1;
    uint32_t prevH = (_saoHeight >> (mip - 1)) > 1 ? (_saoHeight >> (mip - 1)) : 1;

    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = _saoDepthPyramid[currentFrame];
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, mip - 1, 1, 0, 1};
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);

    uint32_t pushData[4] = {prevW, prevH, mipW, mipH};
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, _hizDownsamplePipeline);
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
        _hizPipelineLayout, 1, 1, &_saoDownsampleDescriptorSets[currentFrame][mip - 1], 0, nullptr);
    vkCmdPushConstants(commandBuffer, _hizPipelineLayout,
        VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pushData), pushData);
    vkCmdDispatch(commandBuffer, (mipW + 7) / 8, (mipH + 7) / 8, 1);
  }

  // Transition last mip + full pyramid to SHADER_READ_ONLY
  {
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = _saoDepthPyramid[currentFrame];
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, _saoMipLevels - 1, 1, 0, 1};
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
  }
}

void GpuScene::dispatchSAO(VkCommandBuffer commandBuffer) {
  if (_saoPipeline == VK_NULL_HANDLE)
    return;

  // Transition AO texture to GENERAL for write
  {
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = _aoTexture[currentFrame];
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
  }

  // Use the per-frame descriptor set (already has correct uniform buffer bound)
  uint32_t screenSize[2] = {_saoWidth, _saoHeight};
  vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, _saoPipeline);
  vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
      _saoPipelineLayout, 0, 1, &_saoDescriptorSets[currentFrame], 0, nullptr);
  vkCmdPushConstants(commandBuffer, _saoPipelineLayout,
      VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(screenSize), screenSize);
  vkCmdDispatch(commandBuffer, (_saoWidth + 7) / 8, (_saoHeight + 7) / 8, 1);

  // Transition AO texture to SHADER_READ_ONLY for sampling in deferred pass
  {
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = _aoTexture[currentFrame];
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
  }
}

// --- Screen-space Decals ---

void GpuScene::createDecalRenderPass() {
  uint32_t w = device.getSwapChainExtent().width;
  uint32_t h = device.getSwapChainExtent().height;

  // Fix #10: decal shader only writes SV_Target0 (albedo). Use a single color
  // attachment instead of all 4 GBuffer slots. Depth is bound read-only for
  // sampling (the shader reconstructs world position from it).
  VkAttachmentDescription colorAttachment{};
  colorAttachment.format = _gbufferFormat[0];
  colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
  colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
  colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  colorAttachment.initialLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  colorAttachment.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

  VkAttachmentDescription depthAttachment{};
  depthAttachment.format = device.getWindowDepthFormat();
  depthAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
  depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
  depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  depthAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
  depthAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
  depthAttachment.initialLayout = VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL;
  depthAttachment.finalLayout = VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL;

  VkAttachmentReference colorRef{};
  colorRef.attachment = 0;
  colorRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

  VkAttachmentReference depthRef{};
  depthRef.attachment = 1;
  depthRef.layout = VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL;

  VkSubpassDescription subpass{};
  subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  subpass.colorAttachmentCount = 1;
  subpass.pColorAttachments = &colorRef;
  subpass.pDepthStencilAttachment = &depthRef;

  // Entry: shader read → color attachment write; depth attachment → depth read
  VkSubpassDependency inDep{};
  inDep.srcSubpass = VK_SUBPASS_EXTERNAL;
  inDep.dstSubpass = 0;
  inDep.srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
  inDep.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
  inDep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                       VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
  inDep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;

  // Exit: color write → shader read
  VkSubpassDependency outDep{};
  outDep.srcSubpass = 0;
  outDep.dstSubpass = VK_SUBPASS_EXTERNAL;
  outDep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  outDep.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  outDep.dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
  outDep.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

  VkAttachmentDescription allAttachments[2] = {colorAttachment, depthAttachment};
  VkSubpassDependency deps[2] = {inDep, outDep};

  VkRenderPassCreateInfo rpInfo{};
  rpInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
  rpInfo.attachmentCount = 2;
  rpInfo.pAttachments = allAttachments;
  rpInfo.subpassCount = 1;
  rpInfo.pSubpasses = &subpass;
  rpInfo.dependencyCount = 2;
  rpInfo.pDependencies = deps;

  vkCreateRenderPass(device.getLogicalDevice(), &rpInfo, nullptr, &_decalRenderPass);

  // Create per-frame framebuffers (only albedo + depth)
  _decalFramebuffers.resize(framesInFlight);
  for (uint32_t f = 0; f < framesInFlight; ++f) {
    VkImageView attachments[2] = {
        _gbuffersView[0][f], device.getWindowDepthImageView(f)};
    VkFramebufferCreateInfo fbInfo{};
    fbInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fbInfo.renderPass = _decalRenderPass;
    fbInfo.attachmentCount = 2;
    fbInfo.pAttachments = attachments;
    fbInfo.width = w;
    fbInfo.height = h;
    fbInfo.layers = 1;
    vkCreateFramebuffer(device.getLogicalDevice(), &fbInfo, nullptr, &_decalFramebuffers[f]);
  }
}

void GpuScene::createDecalResources() {
  uint32_t w = device.getSwapChainExtent().width;
  uint32_t h = device.getSwapChainExtent().height;

  // --- 1. Unit cube geometry ---
  {
    float vertices[] = {
        // Back face (Z = -1)
        -1, -1, -1,    1, -1, -1,    1,  1, -1,   -1,  1, -1,
        // Front face (Z = 1)
        -1, -1,  1,    1, -1,  1,    1,  1,  1,   -1,  1,  1,
        // Left face (X = -1)
        -1, -1, -1,   -1, -1,  1,   -1,  1,  1,   -1,  1, -1,
        // Right face (X = 1)
         1, -1, -1,    1, -1,  1,    1,  1,  1,    1,  1, -1,
        // Bottom face (Y = -1)
        -1, -1, -1,    1, -1, -1,    1, -1,  1,   -1, -1,  1,
        // Top face (Y = 1)
        -1,  1, -1,    1,  1, -1,    1,  1,  1,   -1,  1,  1,
    };

    uint32_t indices[] = {
         0,  1,  2,  2,  3,  0,   // back
         4,  5,  6,  6,  7,  4,   // front
         8,  9, 10, 10, 11,  8,   // left
        12, 13, 14, 14, 15, 12,   // right
        16, 17, 18, 18, 19, 16,   // bottom
        20, 21, 22, 22, 23, 20,   // top
    };
    _decalIndexCount = 36;

    VkDeviceSize vbSize = sizeof(vertices);
    createBuffer(vbSize, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                 _decalVertexBuffer, _decalVertexBufferMemory);
    void *data;
    vkMapMemory(device.getLogicalDevice(), _decalVertexBufferMemory, 0, vbSize, 0, &data);
    memcpy(data, vertices, vbSize);
    vkUnmapMemory(device.getLogicalDevice(), _decalVertexBufferMemory);

    VkDeviceSize ibSize = sizeof(indices);
    createBuffer(ibSize, VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                 _decalIndexBuffer, _decalIndexBufferMemory);
    vkMapMemory(device.getLogicalDevice(), _decalIndexBufferMemory, 0, ibSize, 0, &data);
    memcpy(data, indices, ibSize);
    vkUnmapMemory(device.getLogicalDevice(), _decalIndexBufferMemory);
  }

  // --- 2. Per-frame decal SSBO (MAX_DECALS entries; shader reads via push-constant index) ---
  {
    _decalDataBuffers.resize(framesInFlight);
    _decalDataBufferMemories.resize(framesInFlight);
    const VkDeviceSize bufSize = sizeof(DecalData) * MAX_DECALS;
    for (uint32_t i = 0; i < framesInFlight; ++i) {
      VkBufferCreateInfo bufInfo{};
      bufInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
      bufInfo.size = bufSize;
      bufInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
      bufInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
      vkCreateBuffer(device.getLogicalDevice(), &bufInfo, nullptr, &_decalDataBuffers[i]);

      VkMemoryRequirements memReqs;
      vkGetBufferMemoryRequirements(device.getLogicalDevice(), _decalDataBuffers[i], &memReqs);
      VkMemoryAllocateInfo allocInfo{};
      allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
      allocInfo.allocationSize = memReqs.size;
      allocInfo.memoryTypeIndex = device.findMemoryType(
          memReqs.memoryTypeBits,
          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
      vkAllocateMemory(device.getLogicalDevice(), &allocInfo, nullptr, &_decalDataBufferMemories[i]);
      vkBindBufferMemory(device.getLogicalDevice(), _decalDataBuffers[i],
                         _decalDataBufferMemories[i], 0);
    }
  }

  // --- 3. Descriptor set layout and pipeline ---
  {
    VkDescriptorSetLayoutBinding bindings[4] = {};
    // Binding 0: depth texture (sampled)
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    // Binding 1: nearest clamp sampler
    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    // Binding 2: DecalData SSBO (array of MAX_DECALS)
    bindings[2].binding = 2;
    bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[2].descriptorCount = 1;
    bindings[2].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_VERTEX_BIT;
    // Binding 3: decal albedo texture
    bindings[3].binding = 3;
    bindings[3].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    bindings[3].descriptorCount = 1;
    bindings[3].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 4;
    layoutInfo.pBindings = bindings;
    vkCreateDescriptorSetLayout(device.getLogicalDevice(), &layoutInfo, nullptr, &_decalSetLayout);

    // Pipeline layout: set 0 = global (camera), set 1 = decal resources;
    // push constant carries decal index (uint).
    VkDescriptorSetLayout setLayouts[2] = {globalSetLayout, _decalSetLayout};
    VkPushConstantRange pcRange{};
    pcRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pcRange.offset = 0;
    pcRange.size = sizeof(uint32_t);
    VkPipelineLayoutCreateInfo pipeLayoutInfo{};
    pipeLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipeLayoutInfo.setLayoutCount = 2;
    pipeLayoutInfo.pSetLayouts = setLayouts;
    pipeLayoutInfo.pushConstantRangeCount = 1;
    pipeLayoutInfo.pPushConstantRanges = &pcRange;
    vkCreatePipelineLayout(device.getLogicalDevice(), &pipeLayoutInfo, nullptr, &_decalPipelineLayout);

    // Load shaders
    auto vsCode = readFile((_rootPath / "shaders/decal.vs.spv").generic_string());
    auto psCode = readFile((_rootPath / "shaders/decal.ps.spv").generic_string());
    VkShaderModule vsModule = createShaderModule(vsCode);
    VkShaderModule psModule = createShaderModule(psCode);

    VkPipelineShaderStageCreateInfo vsStage{};
    vsStage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    vsStage.stage = VK_SHADER_STAGE_VERTEX_BIT;
    vsStage.module = vsModule;
    vsStage.pName = "DecalVS";

    VkPipelineShaderStageCreateInfo psStage{};
    psStage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    psStage.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    psStage.module = psModule;
    psStage.pName = "DecalPS";

    VkPipelineShaderStageCreateInfo stages[] = {vsStage, psStage};

    VkVertexInputBindingDescription vtxBind{};
    vtxBind.binding = 0;
    vtxBind.stride = 3 * sizeof(float);
    vtxBind.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    VkVertexInputAttributeDescription vtxAttr{};
    vtxAttr.binding = 0;
    vtxAttr.location = 0;
    vtxAttr.format = VK_FORMAT_R32G32B32_SFLOAT;
    vtxAttr.offset = 0;

    VkPipelineVertexInputStateCreateInfo vtxInput{};
    vtxInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vtxInput.vertexBindingDescriptionCount = 1;
    vtxInput.pVertexBindingDescriptions = &vtxBind;
    vtxInput.vertexAttributeDescriptionCount = 1;
    vtxInput.pVertexAttributeDescriptions = &vtxAttr;

    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkViewport viewport{};
    viewport.x = 0; viewport.y = (float)h;
    viewport.width = (float)w; viewport.height = -(float)h;
    viewport.minDepth = 0; viewport.maxDepth = 1;

    VkRect2D scissor{};
    scissor.extent = {w, h};

    VkPipelineViewportStateCreateInfo viewportState{};
    viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.pViewports = &viewport;
    viewportState.scissorCount = 1;
    viewportState.pScissors = &scissor;

    VkPipelineRasterizationStateCreateInfo rasterizer{};
    rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.cullMode = VK_CULL_MODE_NONE; // render backfaces for decals
    rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterizer.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo msaa{};
    msaa.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    msaa.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    // Fix #9: depth test disabled — shader's bbox discard handles in/out tests
    // accurately for any camera position relative to the decal volume.
    // Depth-write is also off (decal must not modify scene depth).
    VkPipelineDepthStencilStateCreateInfo depthStencil{};
    depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable = VK_FALSE;
    depthStencil.depthWriteEnable = VK_FALSE;
    depthStencil.depthCompareOp = VK_COMPARE_OP_ALWAYS;
    depthStencil.stencilTestEnable = VK_FALSE;

    // Fix #10: only 1 color attachment now (albedo). Single blend slot.
    VkPipelineColorBlendAttachmentState blendAttachment{};
    blendAttachment.blendEnable = VK_TRUE;
    blendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    blendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
    blendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    blendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;
    blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT |
                                      VK_COLOR_COMPONENT_G_BIT |
                                      VK_COLOR_COMPONENT_B_BIT |
                                      VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendStateCreateInfo colorBlend{};
    colorBlend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlend.attachmentCount = 1;
    colorBlend.pAttachments = &blendAttachment;

    VkDynamicState dynStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynState{};
    dynState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynState.dynamicStateCount = 2;
    dynState.pDynamicStates = dynStates;

    // Create render pass first
    createDecalRenderPass();

    VkGraphicsPipelineCreateInfo pipeInfo{};
    pipeInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipeInfo.stageCount = 2;
    pipeInfo.pStages = stages;
    pipeInfo.pVertexInputState = &vtxInput;
    pipeInfo.pInputAssemblyState = &inputAssembly;
    pipeInfo.pViewportState = &viewportState;
    pipeInfo.pRasterizationState = &rasterizer;
    pipeInfo.pMultisampleState = &msaa;
    pipeInfo.pDepthStencilState = &depthStencil;
    pipeInfo.pColorBlendState = &colorBlend;
    pipeInfo.pDynamicState = &dynState;
    pipeInfo.layout = _decalPipelineLayout;
    pipeInfo.renderPass = _decalRenderPass;
    pipeInfo.subpass = 0;

    vkCreateGraphicsPipelines(device.getLogicalDevice(), VK_NULL_HANDLE, 1, &pipeInfo,
                               nullptr, &_decalPipeline);

    vkDestroyShaderModule(device.getLogicalDevice(), vsModule, nullptr);
    vkDestroyShaderModule(device.getLogicalDevice(), psModule, nullptr);
  }

  // --- 4. Descriptor pool and per-frame descriptor sets ---
  {
    VkDescriptorPoolSize poolSizes[] = {
        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 2 * framesInFlight},
        {VK_DESCRIPTOR_TYPE_SAMPLER, framesInFlight},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, framesInFlight},
    };
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.poolSizeCount = 3;
    poolInfo.pPoolSizes = poolSizes;
    poolInfo.maxSets = framesInFlight;
    vkCreateDescriptorPool(device.getLogicalDevice(), &poolInfo, nullptr, &_decalDescriptorPool);

    std::vector<VkDescriptorSetLayout> layouts(framesInFlight, _decalSetLayout);
    _decalDescriptorSets.resize(framesInFlight);
    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = _decalDescriptorPool;
    allocInfo.descriptorSetCount = framesInFlight;
    allocInfo.pSetLayouts = layouts.data();
    vkAllocateDescriptorSets(device.getLogicalDevice(), &allocInfo, _decalDescriptorSets.data());
  // --- 5. Load default decal texture ---
  {
    loadDecalTexture(std::string(_rootPath.generic_string() + "/" + _decalTexPaths[0]).c_str());
  }
    for (uint32_t i = 0; i < framesInFlight; i++) {
      VkDescriptorImageInfo depthInfo{};
      depthInfo.imageView = device.getWindowDepthOnlyImageView(i);
      depthInfo.imageLayout = VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL;

      VkDescriptorImageInfo samplerInfo{};
      samplerInfo.sampler = nearestClampSampler;

      VkDescriptorBufferInfo bufInfo{};
      bufInfo.buffer = _decalDataBuffers[i];
      bufInfo.offset = 0;
      bufInfo.range = sizeof(DecalData) * MAX_DECALS;

      VkDescriptorImageInfo texInfo{};
      texInfo.imageView = _decalTextureView;
      texInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

      VkWriteDescriptorSet writes[4] = {};
      writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      writes[0].dstSet = _decalDescriptorSets[i];
      writes[0].dstBinding = 0;
      writes[0].descriptorCount = 1;
      writes[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
      writes[0].pImageInfo = &depthInfo;
      writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      writes[1].dstSet = _decalDescriptorSets[i];
      writes[1].dstBinding = 1;
      writes[1].descriptorCount = 1;
      writes[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
      writes[1].pImageInfo = &samplerInfo;
      writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      writes[2].dstSet = _decalDescriptorSets[i];
      writes[2].dstBinding = 2;
      writes[2].descriptorCount = 1;
      writes[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
      writes[2].pBufferInfo = &bufInfo;
      writes[3].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      writes[3].dstSet = _decalDescriptorSets[i];
      writes[3].dstBinding = 3;
      writes[3].descriptorCount = 1;
      writes[3].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
      writes[3].pImageInfo = &texInfo;
      vkUpdateDescriptorSets(device.getLogicalDevice(), 4, writes, 0, nullptr);
    }
  }



  // --- 6. Create depth readback buffer (single float for decal placement) ---
  {
    VkBufferCreateInfo bufInfo{};
    bufInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufInfo.size = sizeof(float);
    bufInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bufInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateBuffer(device.getLogicalDevice(), &bufInfo, nullptr, &_depthReadbackBuffer) != VK_SUCCESS) {
      throw std::runtime_error("failed to create depth readback buffer!");
    }

    VkMemoryRequirements memReqs;
    vkGetBufferMemoryRequirements(device.getLogicalDevice(), _depthReadbackBuffer, &memReqs);

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memReqs.size;
    allocInfo.memoryTypeIndex = device.findMemoryType(memReqs.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

    if (vkAllocateMemory(device.getLogicalDevice(), &allocInfo, nullptr, &_depthReadbackBufferMemory) != VK_SUCCESS) {
      throw std::runtime_error("failed to allocate depth readback buffer memory!");
    }
    vkBindBufferMemory(device.getLogicalDevice(), _depthReadbackBuffer, _depthReadbackBufferMemory, 0);
  }

  spdlog::info("Decal resources created: {}x{}, ready for {} decals", w, h, MAX_DECALS);
}

void GpuScene::loadDecalTexture(const char *path) {
  // Fix #7, #8: any in-flight frame might still reference the old texture
  // image and the per-frame descriptor sets that point to it. Stall until
  // the GPU is fully idle before destroying / rewriting them.
  vkDeviceWaitIdle(device.getLogicalDevice());

  if (_decalTextureView != VK_NULL_HANDLE) {
    vkDestroyImageView(device.getLogicalDevice(), _decalTextureView, nullptr);
    _decalTextureView = VK_NULL_HANDLE;
  }
  if (_decalTextureImage != VK_NULL_HANDLE) {
    vkDestroyImage(device.getLogicalDevice(), _decalTextureImage, nullptr);
    _decalTextureImage = VK_NULL_HANDLE;
  }
  if (_decalTextureMemory != VK_NULL_HANDLE) {
    vkFreeMemory(device.getLogicalDevice(), _decalTextureMemory, nullptr);
    _decalTextureMemory = VK_NULL_HANDLE;
  }

  int texW, texH, texCh;
  stbi_uc *pixels = stbi_load(path, &texW, &texH, &texCh, STBI_rgb_alpha);
  if (!pixels) {
    spdlog::warn("Failed to load decal texture: {}", path);
    return;
  }
  VkDeviceSize imageSize = texW * texH * 4;

  VkBuffer stagingBuf;
  VkDeviceMemory stagingMem;
  createBuffer(imageSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
               stagingBuf, stagingMem);
  void *d;
  vkMapMemory(device.getLogicalDevice(), stagingMem, 0, imageSize, 0, &d);
  memcpy(d, pixels, (size_t)imageSize);
  vkUnmapMemory(device.getLogicalDevice(), stagingMem);
  stbi_image_free(pixels);

  VkImageCreateInfo imgInfo{};
  imgInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  imgInfo.imageType = VK_IMAGE_TYPE_2D;
  imgInfo.extent = {(uint32_t)texW, (uint32_t)texH, 1};
  imgInfo.mipLevels = 1;
  imgInfo.arrayLayers = 1;
  imgInfo.format = VK_FORMAT_R8G8B8A8_SRGB;
  imgInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
  imgInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  imgInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
  imgInfo.samples = VK_SAMPLE_COUNT_1_BIT;
  imgInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  vkCreateImage(device.getLogicalDevice(), &imgInfo, nullptr, &_decalTextureImage);

  VkMemoryRequirements memReqs;
  vkGetImageMemoryRequirements(device.getLogicalDevice(), _decalTextureImage, &memReqs);
  VkMemoryAllocateInfo alloc{};
  alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  alloc.allocationSize = memReqs.size;
  alloc.memoryTypeIndex = device.findMemoryType(memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  vkAllocateMemory(device.getLogicalDevice(), &alloc, nullptr, &_decalTextureMemory);
  vkBindImageMemory(device.getLogicalDevice(), _decalTextureImage, _decalTextureMemory, 0);

  // Transition and copy using device helpers
  device.transitionImageLayout(_decalTextureImage, VK_FORMAT_R8G8B8A8_SRGB,
                               VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
  device.copyBufferToImage(stagingBuf, _decalTextureImage, texW, texH);

  // Need to transition to SHADER_READ_ONLY — use a one-shot cmd buffer
  {
    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandPool = const_cast<VulkanDevice&>(device).getCommandPool();
    allocInfo.commandBufferCount = 1;
    VkCommandBuffer tmpCmd;
    vkAllocateCommandBuffers(device.getLogicalDevice(), &allocInfo, &tmpCmd);
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(tmpCmd, &begin);
    transitionImageLayout(_decalTextureImage, VK_FORMAT_R8G8B8A8_SRGB,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, tmpCmd);
    vkEndCommandBuffer(tmpCmd);
    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &tmpCmd;
    vkQueueSubmit(device.getGraphicsQueue(), 1, &submitInfo, VK_NULL_HANDLE);
    vkQueueWaitIdle(device.getGraphicsQueue());
    vkFreeCommandBuffers(device.getLogicalDevice(),
                         const_cast<VulkanDevice&>(device).getCommandPool(), 1, &tmpCmd);
  }

  vkDestroyBuffer(device.getLogicalDevice(), stagingBuf, nullptr);
  vkFreeMemory(device.getLogicalDevice(), stagingMem, nullptr);

  VkImageViewCreateInfo viewInfo{};
  viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  viewInfo.image = _decalTextureImage;
  viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
  viewInfo.format = VK_FORMAT_R8G8B8A8_SRGB;
  viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  vkCreateImageView(device.getLogicalDevice(), &viewInfo, nullptr, &_decalTextureView);

  // Update descriptor sets with new texture view
  for (uint32_t i = 0; i < framesInFlight; ++i) {
    VkDescriptorImageInfo texInfo{};
    texInfo.imageView = _decalTextureView;
    texInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = _decalDescriptorSets[i];
    write.dstBinding = 3;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    write.pImageInfo = &texInfo;
    vkUpdateDescriptorSets(device.getLogicalDevice(), 1, &write, 0, nullptr);
  }
  spdlog::info("Decal texture loaded: {} ({}x{})", path, texW, texH);
}

void GpuScene::drawDecals(VkCommandBuffer commandBuffer, uint32_t imageIndex) {
  if (_decalPipeline == VK_NULL_HANDLE || _decals.empty())
    return;

  // Fix #5: Layout transition only when we actually have decals to draw.
  // The transition is owned by drawDecals so callers don't pay the cost when
  // _decals is empty.
  // NOTE: depth image and decal framebuffer are indexed by SWAPCHAIN imageIndex
  // (not currentFrame) — the depth image array and the framebuffer's bound
  // depth view are both swapchain-scoped, while the deferred-lighting framebuffer
  // also uses imageIndex. Indexing by currentFrame here can transition the wrong
  // depth image when imageIndex != currentFrame, causing layout-mismatch
  // validation errors at the deferred lighting render pass begin.
  transitionImageLayout(
      device.getWindowDepthImage(imageIndex), device.getWindowDepthFormat(),
      VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
      VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL, commandBuffer);

  // Fix #1: Upload the entire decal array ONCE before recording any draws.
  // The previous per-iteration mapMemory inside the recorded loop overwrote
  // the same memory, so all draws ended up using the last decal's data.
  // Per-frame uniform buffer is currentFrame-scoped (in-flight resource).
  const uint32_t decalCount = std::min<uint32_t>((uint32_t)_decals.size(), MAX_DECALS);
  {
    void *data;
    vkMapMemory(device.getLogicalDevice(), _decalDataBufferMemories[currentFrame],
                0, sizeof(DecalData) * decalCount, 0, &data);
    memcpy(data, _decals.data(), sizeof(DecalData) * decalCount);
    vkUnmapMemory(device.getLogicalDevice(), _decalDataBufferMemories[currentFrame]);
  }

  VkRenderPassBeginInfo rpInfo{};
  rpInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
  rpInfo.renderPass = _decalRenderPass;
  // Framebuffer must be imageIndex-scoped to match the depth image transitioned above.
  rpInfo.framebuffer = _decalFramebuffers[imageIndex];
  rpInfo.renderArea.offset = {0, 0};
  rpInfo.renderArea.extent = device.getSwapChainExtent();
  rpInfo.clearValueCount = 0;
  rpInfo.pClearValues = nullptr;

  vkCmdBeginRenderPass(commandBuffer, &rpInfo, VK_SUBPASS_CONTENTS_INLINE);

  vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, _decalPipeline);

  VkDescriptorSet sets[2] = {globalDescriptorSets[currentFrame],
                             _decalDescriptorSets[currentFrame]};
  vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          _decalPipelineLayout, 0, 2, sets, 0, nullptr);

  VkDeviceSize vtxOffset = 0;
  vkCmdBindVertexBuffers(commandBuffer, 0, 1, &_decalVertexBuffer, &vtxOffset);
  vkCmdBindIndexBuffer(commandBuffer, _decalIndexBuffer, 0, VK_INDEX_TYPE_UINT32);

  VkExtent2D extent = device.getSwapChainExtent();
  VkViewport viewport{};
  viewport.x = 0; viewport.y = (float)extent.height;
  viewport.width = (float)extent.width; viewport.height = -(float)extent.height;
  viewport.minDepth = 0; viewport.maxDepth = 1;
  vkCmdSetViewport(commandBuffer, 0, 1, &viewport);

  VkRect2D scissor{};
  scissor.extent = extent;
  vkCmdSetScissor(commandBuffer, 0, 1, &scissor);

  // Fix #1: per-decal index via push constant; data was already uploaded above.
  for (uint32_t d = 0; d < decalCount; ++d) {
    vkCmdPushConstants(commandBuffer, _decalPipelineLayout,
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(uint32_t), &d);
    vkCmdDrawIndexed(commandBuffer, _decalIndexCount, 1, 0, 0, 0);
  }

  vkCmdEndRenderPass(commandBuffer);

  // Log once per ~60 frames so we know drawDecals is actually being recorded.
  static uint32_t s_decalDrawCounter = 0;
  if ((s_decalDrawCounter++ % 60) == 0) {
    spdlog::info("drawDecals recorded {} decal(s)", decalCount);
  }
}

// --- Interactive decal helpers ---

vec3 GpuScene::getWorldPosFromDepth(float mouseX, float mouseY) {
  Camera *cam = GetMainCamera();
  float w = (float)device.getSwapChainExtent().width;
  float h = (float)device.getSwapChainExtent().height;

  int px = (int)mouseX;
  int py = (int)mouseY;
  if (px < 0) px = 0;
  if (px >= (int)w) px = (int)w - 1;
  if (py < 0) py = 0;
  if (py >= (int)h) py = (int)h - 1;

  // DEBUG: also probe center and 4 quadrant midpoints to see where the depth
  // buffer actually has non-zero data. This tells us the Y orientation and
  // whether the depth image is being written at all.
  {
    auto probeDepth = [&](int qx, int qy) -> uint32_t {
      uint32_t sentinel = 0xDEADBEEF;
      void *seed;
      vkMapMemory(device.getLogicalDevice(), _depthReadbackBufferMemory, 0, sizeof(uint32_t), 0, &seed);
      memcpy(seed, &sentinel, sizeof(uint32_t));
      vkUnmapMemory(device.getLogicalDevice(), _depthReadbackBufferMemory);

      VkCommandBufferAllocateInfo a{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
      a.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
      a.commandPool = const_cast<VulkanDevice&>(device).getCommandPool();
      a.commandBufferCount = 1;
      VkCommandBuffer c;
      vkAllocateCommandBuffers(device.getLogicalDevice(), &a, &c);
      VkCommandBufferBeginInfo b{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
      b.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
      vkBeginCommandBuffer(c, &b);

      VkImage img = device.getWindowDepthImage(_currentFrameIndex);
      VkImageMemoryBarrier br{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
      br.oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
      br.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
      br.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      br.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      br.image = img;
      br.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
      br.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
      br.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
      vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                           VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &br);
      VkBufferImageCopy r{};
      r.imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
      r.imageOffset = {qx, qy, 0};
      r.imageExtent = {1, 1, 1};
      vkCmdCopyImageToBuffer(c, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                             _depthReadbackBuffer, 1, &r);
      br.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
      br.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
      br.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
      br.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                         VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
      vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_TRANSFER_BIT,
                           VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
                           0, 0, nullptr, 0, nullptr, 1, &br);
      vkEndCommandBuffer(c);

      VkFence f;
      VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
      vkCreateFence(device.getLogicalDevice(), &fi, nullptr, &f);
      VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
      si.commandBufferCount = 1;
      si.pCommandBuffers = &c;
      vkQueueSubmit(device.getGraphicsQueue(), 1, &si, f);
      vkWaitForFences(device.getLogicalDevice(), 1, &f, VK_TRUE, UINT64_MAX);
      vkDestroyFence(device.getLogicalDevice(), f, nullptr);
      vkFreeCommandBuffers(device.getLogicalDevice(),
                           const_cast<VulkanDevice&>(device).getCommandPool(), 1, &c);

      uint32_t out = 0;
      void *d;
      vkMapMemory(device.getLogicalDevice(), _depthReadbackBufferMemory, 0, sizeof(uint32_t), 0, &d);
      memcpy(&out, d, sizeof(uint32_t));
      vkUnmapMemory(device.getLogicalDevice(), _depthReadbackBufferMemory);
      return out;
    };
    int W = (int)w, H = (int)h;
    spdlog::info("DEPTH PROBE corners + center on imageIndex={}:", _currentFrameIndex);
    spdlog::info("  TL(50,50)        = 0x{:08x}", probeDepth(50, 50));
    spdlog::info("  TR({},50)        = 0x{:08x}", W - 50, probeDepth(W - 50, 50));
    spdlog::info("  CENTER({},{})    = 0x{:08x}", W/2, H/2, probeDepth(W/2, H/2));
    spdlog::info("  BL(50,{})        = 0x{:08x}", H - 50, probeDepth(50, H - 50));
    spdlog::info("  BR({},{})        = 0x{:08x}", W - 50, H - 50, probeDepth(W - 50, H - 50));
  }

  // Pre-fill the readback buffer with a sentinel so we can tell whether
  // vkCmdCopyImageToBuffer actually wrote anything (vs. depth being literally 0).
  {
    void *seed;
    vkMapMemory(device.getLogicalDevice(), _depthReadbackBufferMemory, 0, sizeof(uint32_t), 0, &seed);
    uint32_t sentinel = 0xDEADBEEF;
    memcpy(seed, &sentinel, sizeof(uint32_t));
    vkUnmapMemory(device.getLogicalDevice(), _depthReadbackBufferMemory);
  }

  // Fix #3: this is invoked from a mouse callback between frames. The depth
  // image's last-seen layout is the finalLayout of the LAST render pass that
  // touched it. After all the deferred / forward passes, deferred lighting's
  // finalLayout = DEPTH_STENCIL_ATTACHMENT_OPTIMAL is what leaves the image
  // in. We restore to the same layout at the end so subsequent frames don't
  // see a different starting state.
  const VkImageLayout depthCurrentLayout =
      VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

  // Fix #4: avoid vkQueueWaitIdle (full-queue stall). Use a per-call fence
  // so we only block on this single submit.
  VkCommandBufferAllocateInfo cmdAlloc{};
  cmdAlloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  cmdAlloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  cmdAlloc.commandPool = const_cast<VulkanDevice&>(device).getCommandPool();
  cmdAlloc.commandBufferCount = 1;
  VkCommandBuffer cmd;
  vkAllocateCommandBuffers(device.getLogicalDevice(), &cmdAlloc, &cmd);

  VkCommandBufferBeginInfo begin{};
  begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vkBeginCommandBuffer(cmd, &begin);

  VkImage depthImg = device.getWindowDepthImage(_currentFrameIndex);

  // depth DEPTH_STENCIL_ATTACHMENT_OPTIMAL → TRANSFER_SRC_OPTIMAL
  {
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = depthCurrentLayout;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = depthImg;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;
    barrier.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;

    vkCmdPipelineBarrier(cmd,
                         VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &barrier);
  }

  // Copy single pixel to staging buffer
  {
    VkBufferImageCopy region{};
    region.bufferOffset = 0;
    region.bufferRowLength = 0;
    region.bufferImageHeight = 0;
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount = 1;
    region.imageOffset = {px, py, 0};
    region.imageExtent = {1, 1, 1};

    vkCmdCopyImageToBuffer(cmd, depthImg, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           _depthReadbackBuffer, 1, &region);
  }

  // TRANSFER_SRC_OPTIMAL → DEPTH_STENCIL_ATTACHMENT_OPTIMAL (restore)
  {
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.newLayout = depthCurrentLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = depthImg;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    barrier.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

    vkCmdPipelineBarrier(cmd,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &barrier);
  }

  vkEndCommandBuffer(cmd);

  // Submit with per-call fence (Fix #4). vkWaitForFences blocks ONLY this
  // submit instead of stalling the whole graphics queue (vkQueueWaitIdle).
  VkFence fence;
  VkFenceCreateInfo fenceInfo{};
  fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
  vkCreateFence(device.getLogicalDevice(), &fenceInfo, nullptr, &fence);

  VkSubmitInfo submitInfo{};
  submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  submitInfo.commandBufferCount = 1;
  submitInfo.pCommandBuffers = &cmd;
  vkQueueSubmit(device.getGraphicsQueue(), 1, &submitInfo, fence);

  vkWaitForFences(device.getLogicalDevice(), 1, &fence, VK_TRUE, UINT64_MAX);
  vkDestroyFence(device.getLogicalDevice(), fence, nullptr);
  vkFreeCommandBuffers(device.getLogicalDevice(),
                       const_cast<VulkanDevice&>(device).getCommandPool(), 1, &cmd);

  // Read back depth value
  float depth = 0.0f;
  uint32_t rawBytes = 0;
  void *data;
  vkMapMemory(device.getLogicalDevice(), _depthReadbackBufferMemory, 0, sizeof(float), 0, &data);
  memcpy(&depth, data, sizeof(float));
  memcpy(&rawBytes, data, sizeof(uint32_t));
  vkUnmapMemory(device.getLogicalDevice(), _depthReadbackBufferMemory);

  // The depth format may be D24_UNORM_S8_UINT (not D32_SFLOAT_S8_UINT) on some
  // drivers. In that case the 4-byte depth aspect is a 24-bit unsigned-normalized
  // value packed in the low (or high) bits. Reading it as float gives garbage.
  // Detect and reinterpret if needed.
  VkFormat depthFmt = device.getWindowDepthFormat();
  if (depthFmt == VK_FORMAT_D24_UNORM_S8_UINT) {
    // Vulkan packs D24 in the LOW 24 bits of the 32-bit element (per spec for
    // VK_FORMAT_X8_D24_UNORM_PACK32-style copy). Mask and normalize.
    uint32_t d24 = rawBytes & 0x00FFFFFFu;
    depth = (float)d24 / (float)0x00FFFFFFu;
  }
  // Try sampling the center pixel + a few alternatives, to rule out coordinate / index bugs.
  spdlog::info("getWorldPosFromDepth raw read: pixel=({},{}) extent=({}x{}) "
               "_currentFrameIndex={} useRayTracing={} depthFmt={} depth={} rawBytes=0x{:08x}",
               px, py, (uint32_t)w, (uint32_t)h, _currentFrameIndex, useRayTracing,
               (int)depthFmt, depth, rawBytes);

  // Reverse-Z: depth=0 means far plane (no geometry rendered at this pixel).
  // Common cause: RT mode is on (raster pipeline skipped → depth never written).
  // Putting a decal at the far plane makes it microscopic (and possibly off-
  // screen). Fall back to "place 5 units in front of the camera" so the user
  // sees something they can then drag onto real geometry.
  if (depth <= 0.0f) {
    vec3 origin = cam->GetOrigin();
    vec3 fallback = origin + cam->GetCameraDir() * 5.0f;
    spdlog::warn("getWorldPosFromDepth: depth=0 at px=({},{}). useRayTracing={}, "
                 "_currentFrameIndex={}. Placing decal at camera+5 ({},{},{}) "
                 "instead. (If RT mode is on, depth buffer isn't written — "
                 "turn RT off in the ImGui overlay or click on close geometry.)",
                 px, py, useRayTracing, _currentFrameIndex,
                 fallback.x, fallback.y, fallback.z);
    return fallback;
  }

  vec3 result = cam->ScreenToWorldPos(mouseX, mouseY, w, h, depth);
  spdlog::info("getWorldPosFromDepth: mouse=({},{}) px=({},{}) depth={} → world=({},{},{})",
               mouseX, mouseY, px, py, depth, result.x, result.y, result.z);
  return result;
}

void GpuScene::setDecalPreview(const vec3 &worldPos, const vec3 &normal,
                               const vec3 &scale) {
  // Build localToWorld from position, normal, and scale
  vec3 up(0, 1, 0);
  if (fabsf(normal.dot(up)) > 0.99f)
    up = vec3(1, 0, 0);
  vec3 tangent = normalize(up.cross(normal));
  vec3 bitangent = normal.cross(tangent);

  mat4 localToWorld;
  localToWorld.x = vec4(tangent * scale.x, 0);
  localToWorld.y = vec4(bitangent * scale.y, 0);
  localToWorld.z = vec4(normal * scale.z, 0);
  localToWorld.w = vec4(worldPos, 1);

  DecalData d;
  d.localToWorld = localToWorld;
  d.worldToLocal = inverse(localToWorld);
  d.albedoTint = vec4(1, 1, 1, 0.5f);
  d.hasTexture = (_decalTexIndex > 0) ? 1 : 0;

  _decals.push_back(d);

  spdlog::info("Decal placed: pos=({},{},{}) normal=({},{},{}) scale=({},{},{}) total={}",
               worldPos.x, worldPos.y, worldPos.z,
               normal.x, normal.y, normal.z,
               scale.x, scale.y, scale.z,
               _decals.size());
}

void GpuScene::updateDecalFromMouse(float mouseX, float mouseY, bool placeNew) {
  Camera *cam = GetMainCamera();
  float w = (float)device.getSwapChainExtent().width;
  float h = (float)device.getSwapChainExtent().height;

  if (placeNew && _decals.size() < MAX_DECALS) {
    vec3 hitPos = getWorldPosFromDepth(mouseX, mouseY);
    setDecalPreview(hitPos, vec3(0, 1, 0), vec3(1.0f, 1.f, 1.f));
  } else if (_selectedDecal >= 0 && (size_t)_selectedDecal < _decals.size()) {
    DecalData &d = _decals[_selectedDecal];
    vec3 pos = d.localToWorld.w.xyz();

    if (_isDraggingDecal) {
      vec3 rayOrigin, rayDir;
      cam->ScreenToWorldRay(mouseX, mouseY, w, h, 0.5f, rayOrigin, rayDir);
      vec3 planeNormal = cam->GetCameraDir();
      float denom = rayDir.dot(planeNormal);
      if (fabsf(denom) > 0.0001f) {
        float t = (pos - rayOrigin).dot(planeNormal) / denom;
        if (t > 0.0f) {
          vec3 hitPos = rayOrigin + rayDir * t;
          vec3 offset = hitPos - pos;
          d.localToWorld.w.x += offset.x;
          d.localToWorld.w.y += offset.y;
          d.localToWorld.w.z += offset.z;
          d.worldToLocal = inverse(d.localToWorld);
        }
      }
    } else if (_isRotatingDecal) {
      float dx = mouseX - _lastMouseX;
      float angle = dx * 0.5f;
      float c = cosf(angle * 3.1415926f / 180.0f);
      float s = sinf(angle * 3.1415926f / 180.0f);
      vec3 xCol = d.localToWorld.x.xyz();
      vec3 zCol = d.localToWorld.z.xyz();
      vec3 newX = xCol * c + zCol * s;
      vec3 newZ = normalize(xCol.cross(vec3(0, 1, 0))) * c + zCol * s;
      d.localToWorld.x = vec4(newX, 0);
      d.localToWorld.z = vec4(normalize(newZ), 0);
      d.worldToLocal = inverse(d.localToWorld);
    }
  }
}

void GpuScene::onMouseDownDecal(float mouseX, float mouseY) {
  // Check if clicking on an existing decal (simple 2D screen-space pick)
  _selectedDecal = -1;
  Camera *cam = GetMainCamera();
  float w = (float)device.getSwapChainExtent().width;
  float h = (float)device.getSwapChainExtent().height;

  for (int i = (int)_decals.size() - 1; i >= 0; --i) {
    DecalData &d = _decals[i];
    vec4 worldCenter = d.localToWorld * vec4(0, 0, 0, 1);
    vec3 center = worldCenter.xyz_w();

    // Project center to screen
    mat4 viewProj = cam->getProjectMatrix() * cam->getObjectToCamera();
    vec4 clip = viewProj * vec4(center, 1);
    if (clip.w <= 0.001f) continue;
    float sx = (clip.x / clip.w * 0.5f + 0.5f) * w;
    float sy = (1.0f - (clip.y / clip.w * 0.5f + 0.5f)) * h;

    float dist = sqrtf((sx - mouseX) * (sx - mouseX) +
                       (sy - mouseY) * (sy - mouseY));
    if (dist < 50.0f) {
      _selectedDecal = i;
      _isDraggingDecal = true;
      break;
    }
  }

  if (_selectedDecal < 0 && _decals.size() < MAX_DECALS) {
    // Place new decal at mouse position
    updateDecalFromMouse(mouseX, mouseY, true);
    _selectedDecal = (int)_decals.size() - 1;
    _isDraggingDecal = true;
  }
  _lastMouseX = mouseX;
  _lastMouseY = mouseY;
}

void GpuScene::onMouseMoveDecal(float mouseX, float mouseY) {
  if (_isDraggingDecal || _isRotatingDecal) {
    updateDecalFromMouse(mouseX, mouseY, false);
  }
  _lastMouseX = mouseX;
  _lastMouseY = mouseY;
}

void GpuScene::onMouseUpDecal() {
  _isDraggingDecal = false;
  _isRotatingDecal = false;
}

void GpuScene::onScrollDecal(float deltaY) {
  if (_selectedDecal >= 0 && (size_t)_selectedDecal < _decals.size()) {
    DecalData &d = _decals[_selectedDecal];
    float s = (deltaY > 0) ? 1.1f : 0.9f;
    d.localToWorld.x = vec4(d.localToWorld.x.xyz() * s, 0);
    d.localToWorld.y = vec4(d.localToWorld.y.xyz() * s, 0);
    d.localToWorld.z = vec4(d.localToWorld.z.xyz() * s, 0);
    d.worldToLocal = inverse(d.localToWorld);
  }
}

void GpuScene::cycleDecalTexture() {
  _decalTexIndex = (_decalTexIndex + 1) % DECAL_TEX_COUNT;
  if (_decalTexIndex > 0) {
    std::string path = _rootPath.generic_string() + "/" + _decalTexPaths[_decalTexIndex];
    loadDecalTexture(path.c_str());
    // Update hasTexture flag on selected decal
    if (_selectedDecal >= 0 && (size_t)_selectedDecal < _decals.size()) {
      _decals[_selectedDecal].hasTexture = 1;
    }
  } else {
    if (_selectedDecal >= 0 && (size_t)_selectedDecal < _decals.size()) {
      _decals[_selectedDecal].hasTexture = 0;
    }
  }
}

// --- ImGui Integration ---

void GpuScene::initImGui(SDL_Window *window) {
  // Create descriptor pool for ImGui
  VkDescriptorPoolSize pool_sizes[] = {
      {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1},
  };
  VkDescriptorPoolCreateInfo pool_info{};
  pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
  pool_info.maxSets = 1;
  pool_info.poolSizeCount = 1;
  pool_info.pPoolSizes = pool_sizes;
  vkCreateDescriptorPool(device.getLogicalDevice(), &pool_info, nullptr, &_imguiDescriptorPool);

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO &io = ImGui::GetIO();
  io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
  ImGui::StyleColorsDark();

  ImGui_ImplSDL2_InitForVulkan(window);

  ImGui_ImplVulkan_InitInfo init_info{};
  init_info.ApiVersion = VK_API_VERSION_1_2;
  init_info.Instance = device.getInstance();
  init_info.PhysicalDevice = device.getPhysicalDevice();
  init_info.Device = device.getLogicalDevice();
  init_info.QueueFamily = 0;
  init_info.Queue = device.getGraphicsQueue();
  init_info.DescriptorPool = _imguiDescriptorPool;
  init_info.MinImageCount = framesInFlight;
  init_info.ImageCount = framesInFlight;
  init_info.PipelineInfoMain.RenderPass = _forwardLightingPass;
  init_info.PipelineInfoMain.Subpass = 0;
  init_info.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;

  ImGui_ImplVulkan_Init(&init_info);

  _imguiInitialized = true;
  spdlog::info("ImGui initialized for Vulkan + SDL2");
}

void GpuScene::ProcessImGuiEvent(SDL_Event *event) {
  if (_imguiInitialized) {
    ImGui_ImplSDL2_ProcessEvent(event);
  }
}

void GpuScene::readbackCullingStats(uint32_t previousFrame) {
  if (!applMesh) return;

  _cullingStats.totalOpaque = (uint32_t)applMesh->_opaqueChunkCount;
  _cullingStats.totalAlphaMask = (uint32_t)applMesh->_alphaMaskedChunkCount;
  _cullingStats.totalTransparent = (uint32_t)applMesh->_transparentChunkCount;

  // Read from previous frame's writeIndexBuffer (already completed on GPU)
  void *data;
  if (vkMapMemory(device.getLogicalDevice(), writeIndexBufferMemories[previousFrame],
                  0, 3 * sizeof(uint32_t), 0, &data) == VK_SUCCESS) {
    uint32_t counts[3];
    memcpy(counts, data, 3 * sizeof(uint32_t));
    vkUnmapMemory(device.getLogicalDevice(), writeIndexBufferMemories[previousFrame]);

    _cullingStats.visibleOpaque = counts[0];
    _cullingStats.visibleAlphaMask = counts[1];
    _cullingStats.visibleTransparent = counts[2];
  }
}

void GpuScene::renderImGuiOverlay(VkCommandBuffer commandBuffer, uint32_t imageIndex) {
  if (!_imguiInitialized) return;

  ImGui_ImplVulkan_NewFrame();
  ImGui_ImplSDL2_NewFrame();
  ImGui::NewFrame();

  uint32_t totalChunks = _cullingStats.totalOpaque + _cullingStats.totalAlphaMask + _cullingStats.totalTransparent;
  uint32_t totalVisible = _cullingStats.visibleOpaque + _cullingStats.visibleAlphaMask + _cullingStats.visibleTransparent;
  uint32_t culledCount = totalChunks > totalVisible ? totalChunks - totalVisible : 0;
  float cullPercent = totalChunks > 0 ? 100.0f * culledCount / totalChunks : 0.0f;

  ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_Always);
  ImGui::SetNextWindowBgAlpha(0.5f);
  ImGui::Begin("Culling Stats", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
               ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
               ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove);

  ImGui::Text("Total Chunks: %u", totalChunks);
  ImGui::Text("Visible: %u", totalVisible);
  ImGui::Text("Culled:  %u (%.1f%%)", culledCount, cullPercent);
  ImGui::Separator();
  ImGui::Text("Opaque:    %u / %u", _cullingStats.visibleOpaque, _cullingStats.totalOpaque);
  ImGui::Text("AlphaMask: %u / %u", _cullingStats.visibleAlphaMask, _cullingStats.totalAlphaMask);
  ImGui::Text("Transp:    %u / %u", _cullingStats.visibleTransparent, _cullingStats.totalTransparent);

  ImGui::Separator();
  ImGui::Checkbox("Ray Tracing", &useRayTracing);
  if (useRayTracing) {
    ImGui::TextDisabled("(raster pipeline skipped)");
    if (_raytracing) {
      ImGui::Text("Samples accumulated: %u", _raytracing->getAccumCount());
      int bounces = (int)_raytracing->maxBounces;
      if (ImGui::SliderInt("Max Bounces", &bounces, 1, 8))
        _raytracing->maxBounces = (uint32_t)bounces;
      if (ImGui::Button("Reset Accumulation"))
        _raytracing->resetAccumulation();
    }
  }
  ImGui::Checkbox("TAA", &_taaEnabled);

  ImGui::End();

  ImGui::Render();
  ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), commandBuffer);
}

// =============================================================================
//  Scatter Volume – GpuScene integration
// =============================================================================

void GpuScene::createScatterVolume() {
  uint32_t screenW = device.getSwapChainExtent().width;
  uint32_t screenH = device.getSwapChainExtent().height;

  // Build per-frame light index buffer lists for scatter shader
  ScatterLightResources lightRes{};
  lightRes.pointLightDataBuffer = _lightCuller->GetPointLightCullingDataBuffer();
  lightRes.spotLightDataBuffer  = _lightCuller->GetSpotLightCullingDataBuffer();
  lightRes.spotViewProjBuffer   = _lightCuller->GetSpotViewProjBuffer();
  lightRes.spotShadowMapView    = _shadow->GetSpotShadowArrayView();
  lightRes.spotShadowSampler    = _shadow->GetSpotShadowSampler();
  for (uint32_t f = 0; f < framesInFlight; ++f) {
    lightRes.pointLightIndexBuffers.push_back(_lightCuller->GetPointLightIndicesBuffer(f));
    lightRes.spotLightIndexBuffers.push_back(_lightCuller->GetSpotLightIndicesBuffer(f));
  }

  _scatterVolume.create(
    device, _rootPath, screenW, screenH, framesInFlight,
    uniformBuffers,
    _shadow->_shadowSliceViewFull, _shadow->_shadowMapSampler,
    lightRes
  );

  // Bind the accumulated scatter volume at bindings 16/17 in each per-frame
  // deferred lighting descriptor set.
  for (uint32_t f = 0; f < framesInFlight; ++f) {
    VkDescriptorImageInfo accumInfo{};
    accumInfo.imageView   = _scatterVolume.accumVolumeView();
    accumInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkWriteDescriptorSet accumWrite{};
    accumWrite.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    accumWrite.dstSet          = deferredLightingDescriptorSet[f];
    accumWrite.dstBinding      = 16;
    accumWrite.descriptorCount = 1;
    accumWrite.descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    accumWrite.pImageInfo      = &accumInfo;

    VkDescriptorImageInfo samplerInfo{};
    samplerInfo.sampler = _linearClampSampler;

    VkWriteDescriptorSet samplerWrite{};
    samplerWrite.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    samplerWrite.dstSet          = deferredLightingDescriptorSet[f];
    samplerWrite.dstBinding      = 17;
    samplerWrite.descriptorCount = 1;
    samplerWrite.descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLER;
    samplerWrite.pImageInfo      = &samplerInfo;

    VkWriteDescriptorSet writes[] = {accumWrite, samplerWrite};
    vkUpdateDescriptorSets(device.getLogicalDevice(), 2, writes, 0, nullptr);
  }

  spdlog::info("ScatteringVolume: created and bound to deferred lighting");
}
