
#include "GpuScene.h"
#include "AssetLoader.h"
#include "Light.h"
#include "ObjLoader.h"
#include "Shadow.h"
#include "ThirdParty/lzfse.h"
#include "VulkanCompat.h"
#include "VulkanSetup.h"
#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_vulkan.h"


#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#include <array>
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
                                             aoBinding};

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
      {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 12 * framesInFlight},
      {VK_DESCRIPTOR_TYPE_SAMPLER, 3 * framesInFlight},
      {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3 * framesInFlight},
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
  camBufferBinding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT;

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

void GpuScene::CreateTextures() {
  for (auto &texture : applMesh->_textures) {
    textureHashMap[texture._pathHash] = textures.size();
    textures.push_back(createTexture(texture));
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

  if (sizeof(AAPLMaterial) != 96) {
    spdlog::error("layout mismatch with apple sizeof(AAPLMaterial) is {}, "
                  "while apple is 96",
                  sizeof(AAPLMaterial));
  }

  AAPLMaterial *decompressedMaterial = (AAPLMaterial *)uncompressData(
      (unsigned char *)applMesh->_materialData,
      applMesh->compressedMaterialDataLength,
      applMesh->_materialCount * sizeof(AAPLMaterial));
  materials.resize(applMesh->_materialCount);
  for (int i = 0; i < applMesh->_materialCount; i++) {
    ConfigureMaterial(decompressedMaterial[i], materials[i]);
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
        bufferInfo.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
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
        bufferInfo.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
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
        bufferInfo.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
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
        bufferInfo.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
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
        bufferInfo.usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
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

  AAPLSubMesh *submeshes = (AAPLSubMesh *)uncompressData(
      (unsigned char *)applMesh->_meshData, applMesh->compressedMeshDataLength,
      applMesh->_meshCount * sizeof(AAPLSubMesh));

  CreateDepthTexture();
  CreateZdepthView();
  CreateGBuffers();
  CreateOccluderZPass();
  CreateOccluderZPassFrameBuffer();
  CreateDeferredBasePass();
  CreateDeferredLightingPass();
  CreateForwardLightingPass();
  CreateBasePassFrameBuffer();
  CreateDeferredLightingFrameBuffer(device.getSwapChainImageCount());
  CreateForwardLightingFrameBuffer(device.getSwapChainImageCount());

  createTextureSampler();
  createNearestClampSampler();

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
  createGraphicsPipeline(deviceref.getMainRenderPass());
  createRenderOccludersPipeline(occluderZPass);
  createOccluderWireframePipeline();
  createComputePipeline();

  _shadow = new Shadow(device,*this,1024);
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
    }
  }
}

void GpuScene::CreateForwardLightingPass() {

  VkAttachmentDescription deferredLightingAttachments = {};

  deferredLightingAttachments.format = device.getSwapChainImageFormat();
  deferredLightingAttachments.samples = VK_SAMPLE_COUNT_1_BIT;
  deferredLightingAttachments.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
  deferredLightingAttachments.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  deferredLightingAttachments.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  deferredLightingAttachments.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  deferredLightingAttachments.initialLayout =
      VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  deferredLightingAttachments.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

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
  deferredLightingDepthAttachments.finalLayout =
      VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

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

  deferredLightingAttachments.format = device.getSwapChainImageFormat();
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
    std::array<VkImageView, 2> attachments = {device.getSwapChainImageView(i),
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
    std::array<VkImageView, 2> attachments = {device.getSwapChainImageView(i),
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
  
  _shadow->UpdateShadowMatrices(*this);
  {
    frameConstants.nearPlane = maincamera->Near();
    frameConstants.farPlane = maincamera->Far();
    static uint32_t sFrameCounter = 0;
    frameConstants.frameCounter = sFrameCounter++;
    frameConstants.physicalSize = vec2(device.getSwapChainExtent().width,
                                       device.getSwapChainExtent().height);
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
    memcpy(data1, transpose(maincamera->getProjectMatrix()).value_ptr(),
           (size_t)sizeof(mat4));
    data1 = ((mat4 *)data1) + 1;
    memcpy(data1, transpose(maincamera->getObjectToCamera()).value_ptr(),
           (size_t)sizeof(mat4));
    data1 = ((mat4 *)data1) + 1;
    memcpy(data1, transpose(maincamera->getInvViewMatrix()).value_ptr(),
           (size_t)sizeof(mat4));
    data1 = ((mat4 *)data1) + 1;
    memcpy(data1,
           transpose(maincamera->getInvViewProjectionMatrix()).value_ptr(),
           (size_t)sizeof(mat4));
    data1 = ((mat4 *)data1) + 1;
    // invProjectionMatrix
    mat4 invProj = inverse(maincamera->getProjectMatrix());
    memcpy(data1, transpose(invProj).value_ptr(), (size_t)sizeof(mat4));
    data1 = ((mat4 *)data1) + 1;
    memcpy(data1, &frameConstants, sizeof(FrameConstants));
    vkUnmapMemory(device.getLogicalDevice(), uniformBufferMemories[currentFrame]);

  const Frustum &cascadeFrustum = maincamera->getFrustum();
    {
            
    // TODO: compute proper cascade frustum from shadow VP matrix
  uint32_t opaqueCount = applMesh->_opaqueChunkCount;
  uint32_t alphaMaskedCount = applMesh->_alphaMaskedChunkCount;
  uint32_t cascadeMaxChunks = opaqueCount + alphaMaskedCount;
    // Upload shadow cull params
    {
      void *data;
      vkMapMemory(device.getLogicalDevice(), _shadow->_shadowCullParamsMemories[currentFrame], 0,
                  sizeof(Shadow::ShadowCullParams), 0, &data);
      memcpy((char *)data + 0, &opaqueCount, sizeof(uint32_t));
      memcpy((char *)data + 4, &alphaMaskedCount, sizeof(uint32_t));
      memcpy((char *)data + 8, &cascadeMaxChunks, sizeof(uint32_t));
      uint32_t cascadeIdx = SHADOW_CASCADE_COUNT;
      memcpy((char *)data + 12, &cascadeIdx, sizeof(uint32_t));
      memcpy((char *)data + 16, &cascadeFrustum, sizeof(Frustum));
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

      // Compute view-projection matrix for Hi-Z AABB projection
      mat4 viewProj = maincamera->getProjectMatrix() * maincamera->getObjectToCamera();
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

  {_shadow->RenderShadowMap(commandBuffer, *this, device); }

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
  // vkCmdBindPipeline(commandBuffer,VK_PIPELINE_BIND_POINT_GRAPHICS,
  // egraphicsPipeline); VkBuffer vertexBuffers[] = {vertexBuffer}; VkDeviceSize
  // offsets[] = {0};

  // vkCmdBindVertexBuffers(commandBuffer, 0, 1, vertexBuffers, offsets);
  // vkCmdBindIndexBuffer(commandBuffer,indexBuffer,0,VK_INDEX_TYPE_UINT16);
  // if the descriptor set data isn't change we can omit this?
  // vkCmdBindDescriptorSets(commandBuffer,VK_PIPELINE_BIND_POINT_GRAPHICS,epipelineLayout,0,1,&globalDescriptorSet,0,nullptr);
  // if the constant isn't changed we can omit this?
  // mat4 scaleM = scale(modelScale);
  // mat4 withScale = transpose(maincamera->getObjectToCamera()) * scaleM;
  // vkCmdPushConstants(commandBuffer,epipelineLayout,VK_SHADER_STAGE_VERTEX_BIT,0,sizeof(mat4),withScale.value_ptr());
  // vkCmdDrawIndexed(commandBuffer,getIndexSize()/sizeof(unsigned
  // short),1,0,0,0);

  {
    // GBuffer layout transition is now handled by render pass finalLayout
    // (SHADER_READ_ONLY_OPTIMAL) + exit subpass dependency
    // Shadow map layout transition is handled by shadow render pass finalLayout

    if (useClusterLighting) {
      // don't need to write stencil anymore
      VkImageMemoryBarrier barrier{};
      barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
      barrier.oldLayout = VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL;
      barrier.newLayout = VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL;
      barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      barrier.image = device.getWindowDepthImage(currentFrame);
      barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
      barrier.subresourceRange.baseMipLevel = 0;
      barrier.subresourceRange.levelCount = 1;
      barrier.subresourceRange.baseArrayLayer = 0;
      barrier.subresourceRange.layerCount = 1;
      barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
      barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
      vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                           VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr,
                           0, nullptr, 1, &barrier);
    } else {
      transitionImageLayout(
          device.getWindowDepthImage(currentFrame), device.getWindowDepthFormat(),
          VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
          VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL, commandBuffer); // read depth while write stencil in the point lighting pass
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

  // post process

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

  // 等待当前帧的 fence 完成
  VkResult fenceResult = vkWaitForFences(device.getLogicalDevice(), 1, 
                  &inFlightFences[currentFrame], VK_TRUE, UINT64_MAX);
  if (fenceResult != VK_SUCCESS) {
    spdlog::error("failed to wait for fence! {}", static_cast<int>(fenceResult));
    return;
  }

  // 获取下一个 swapchain image
  uint32_t imageIndex;
  VkResult acquireResult = vkAcquireNextImageKHR(device.getLogicalDevice(), device.getSwapChain(),
                        UINT64_MAX, imageAvailableSemaphores[currentFrame], VK_NULL_HANDLE,
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

  // 只有在成功获取 image 后才 reset fence，避免死锁
  vkResetFences(device.getLogicalDevice(), 1, &inFlightFences[currentFrame]);

  // Readback culling stats from current frame's PREVIOUS submission
  // (fence guarantees currentFrame's last GPU work is done)
  readbackCullingStats(currentFrame);

  // 使用当前帧的 command buffer
  VkCommandBuffer& currentCmdBuffer = commandBuffers[currentFrame];
  vkResetCommandBuffer(currentCmdBuffer, /*VkCommandBufferResetFlagBits*/ 0);

  recordCommandBuffer(imageIndex, currentCmdBuffer);

  VkSubmitInfo submitInfo{};
  submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;

  VkSemaphore waitSemaphores[] = {imageAvailableSemaphores[currentFrame]};
  VkPipelineStageFlags waitStages[] = {
      VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
  submitInfo.waitSemaphoreCount = 1;
  submitInfo.pWaitSemaphores = waitSemaphores;
  submitInfo.pWaitDstStageMask = waitStages;

  submitInfo.commandBufferCount = 1;
  submitInfo.pCommandBuffers = &currentCmdBuffer;

  VkSemaphore signalSemaphores[] = {renderFinishedSemaphores[currentFrame]};
  submitInfo.signalSemaphoreCount = 1;
  submitInfo.pSignalSemaphores = signalSemaphores;
  VkResult submitResult =
      vkQueueSubmit(device.getGraphicsQueue(), 1, &submitInfo, inFlightFences[currentFrame]);
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

  // 前进到下一帧
  currentFrame = (currentFrame + 1) % framesInFlight;
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

  // 注意：如果 GBuffer images 也是独立创建的，也需要在这里销毁
  // vkDestroyImage, vkFreeMemory 等
}

void GpuScene::recreateSwapChainResources() {
  // 重新创建依赖 swapchain 尺寸的资源
  
  // 重新创建 deferred/forward framebuffers
  CreateDeferredLightingFrameBuffer(device.getSwapChainImageCount());
  CreateForwardLightingFrameBuffer(device.getSwapChainImageCount());
  
  // 更新 camera aspect ratio
  if (maincamera) {
    float aspect = device.getSwapChainExtent().width / 
                   static_cast<float>(device.getSwapChainExtent().height);
    // 如果 Camera 类有设置 aspect ratio 的方法，在这里调用
    // maincamera->setAspectRatio(aspect);
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
    
    // 重置 currentFrame 以避免越界访问
    currentFrame = 0;
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
    // Descriptor set layout: depth (0), pyramid (1), camera cbuffer (2), AO output (3)
    VkDescriptorSetLayoutBinding bindings[4] = {};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    bindings[2].binding = 2;
    bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[2].descriptorCount = 1;
    bindings[2].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    bindings[3].binding = 3;
    bindings[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[3].descriptorCount = 1;
    bindings[3].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 4;
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
      {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 2 * framesInFlight},
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
      // Binding 0: depth texture (per-frame)
      VkDescriptorImageInfo depthInfo{};
      depthInfo.imageView = device.getWindowDepthOnlyImageView(i);
      depthInfo.imageLayout = VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL;

      // Binding 1: SAO depth pyramid (per-frame)
      VkDescriptorImageInfo pyramidInfo{};
      pyramidInfo.imageView = _saoDepthPyramidView[i];
      pyramidInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

      // Binding 2: camera params uniform buffer (per-frame)
      VkDescriptorBufferInfo bufferInfo{};
      bufferInfo.buffer = uniformBuffers[i];
      bufferInfo.offset = 0;
      bufferInfo.range = sizeof(FrameData);

      // Binding 3: AO output (per-frame)
      VkDescriptorImageInfo aoInfo{};
      aoInfo.imageView = _aoTextureView[i];
      aoInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

      VkWriteDescriptorSet writes[4] = {};
      writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      writes[0].dstSet = _saoDescriptorSets[i];
      writes[0].dstBinding = 0;
      writes[0].descriptorCount = 1;
      writes[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
      writes[0].pImageInfo = &depthInfo;
      writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      writes[1].dstSet = _saoDescriptorSets[i];
      writes[1].dstBinding = 1;
      writes[1].descriptorCount = 1;
      writes[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
      writes[1].pImageInfo = &pyramidInfo;
      writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      writes[2].dstSet = _saoDescriptorSets[i];
      writes[2].dstBinding = 2;
      writes[2].descriptorCount = 1;
      writes[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
      writes[2].pBufferInfo = &bufferInfo;
      writes[3].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      writes[3].dstSet = _saoDescriptorSets[i];
      writes[3].dstBinding = 3;
      writes[3].descriptorCount = 1;
      writes[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
      writes[3].pImageInfo = &aoInfo;
      vkUpdateDescriptorSets(device.getLogicalDevice(), 4, writes, 0, nullptr);
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

  ImGui::End();

  ImGui::Render();
  ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), commandBuffer);
}
