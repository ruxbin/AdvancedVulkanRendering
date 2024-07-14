
#include "GpuScene.h"
#include "ObjLoader.h"
#include "VulkanSetup.h"
#include "ThirdParty/lzfse.h"

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#include <fstream>
#include <vector>
#include <array>
#include <functional>
#include <cstdlib>
#include <cstddef>

#define USE_CPU_ENCODE_DRAWPARAM


//TODO: move to common.cpp
std::vector<char> readFile(const std::string &filename) {
  std::ifstream file(filename, std::ios::ate | std::ios::binary);

  if (!file.is_open()) {
    throw std::runtime_error("failed to open file!");
  }

  size_t fileSize = (size_t)file.tellg();
  std::vector<char> buffer(fileSize);

  file.seekg(0);
  file.read(buffer.data(), fileSize);

  file.close();

  return buffer;
}


// Header for compressed blocks.
struct AAPLCompressionHeader
{
    uint32_t compressionMode;       // Compression mode in block - of type compression_algorithm.
    uint64_t uncompressedSize;      // Size of uncompressed data.
    uint64_t compressedSize;        // Size of compressed data.
};

AAPLCompressionHeader* getCompressionHeader(void* data, size_t length)
{
    assert(data != nullptr);

    if (length < sizeof(AAPLCompressionHeader))
    {
        spdlog::error("Data is too small");
        
        exit(1);
    }

    AAPLCompressionHeader* header = ((AAPLCompressionHeader*)data);

    if (length != sizeof(AAPLCompressionHeader) + header->compressedSize)
    {
        spdlog::error("Data length mismatch");
        exit(1);
    }

    return header;
}

size_t uncompressedDataSize(void* data,size_t datalength)
{
    return getCompressionHeader(data,datalength)->uncompressedSize;
}

void uncompressData(const AAPLCompressionHeader& header, const void* data, void* dstBuffer)
{


    if (header.compressionMode != 2049)
    {
        spdlog::error("something that are not compressed using apple format");
    }

    //size_t a = compression_decode_buffer((uint8_t*)dstBuffer, header.uncompressedSize,
    //                                     (const uint8_t*)data, header.compressedSize,
    //                                     NULL, (compression_algorithm)header.compressionMode);
    lzfse_decode_buffer((uint8_t*)dstBuffer, header.uncompressedSize, (const uint8_t*)data, header.compressedSize, NULL);

}

void* uncompressData(unsigned char* data, size_t dataLength, uint64_t expectedsize)
{
        
    AAPLCompressionHeader* header = getCompressionHeader(data,dataLength);
    if (expectedsize != header->uncompressedSize)
        spdlog::warn("texture mipmap data corrputed");

    void* decompressedData = malloc(header->uncompressedSize);

    uncompressData(*header, (header + 1), decompressedData);

    return decompressedData;
}

//typedef void* (*AllocatorCallback)(size_t);

void* uncompressData(void* data, size_t dataLength,std::function<void*(uint64_t)> allocatorCallback)
{
    AAPLCompressionHeader* header = getCompressionHeader(data,dataLength);

    void* dstBuffer = allocatorCallback(header->uncompressedSize);

    uncompressData(*header, (header + 1), dstBuffer);
    return dstBuffer;
}

VkShaderModule GpuScene::createShaderModule(const std::vector<char> &code)const{
  VkShaderModuleCreateInfo createInfo{};
  createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
  createInfo.codeSize = code.size();
  createInfo.pCode = reinterpret_cast<const uint32_t *>(code.data());

  VkShaderModule shaderModule;
  if (vkCreateShaderModule(device.getLogicalDevice(), &createInfo, nullptr, &shaderModule) !=
      VK_SUCCESS) {
    throw std::runtime_error("failed to create shader module!");
  }

  return shaderModule;
}

//TODO: cache the pso
void GpuScene::createRenderOccludersPipeline(VkRenderPass renderPass)
{
    auto occludersVSShaderCode = readFile((_rootPath / "shaders/occluders.vs.spv").generic_string());
    VkShaderModule occludersVSShaderModule = createShaderModule(occludersVSShaderCode);
    VkPipelineShaderStageCreateInfo drawOccludersVSShaderStageInfo{};
    drawOccludersVSShaderStageInfo.sType =
        VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    drawOccludersVSShaderStageInfo.stage = VK_SHADER_STAGE_VERTEX_BIT;
    drawOccludersVSShaderStageInfo.module = occludersVSShaderModule;
    drawOccludersVSShaderStageInfo.pName = "RenderSceneVS";

    //we don't need fragment stage 
    VkPipelineShaderStageCreateInfo shaderStages[] = { drawOccludersVSShaderStageInfo };


    VkVertexInputBindingDescription occluderInputBinding = {
    .binding = 0,
    .stride = sizeof(float) * 3,
    .inputRate = VK_VERTEX_INPUT_RATE_VERTEX };

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
    occluderVertexInputInfo.vertexAttributeDescriptionCount = sizeof(occluderInputAttributes)/sizeof(occluderInputAttributes[0]);
    occluderVertexInputInfo.pVertexAttributeDescriptions = occluderInputAttributes;


    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.sType =
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;//change to strip
    inputAssembly.primitiveRestartEnable = VK_FALSE;

    const VkExtent2D& swapChainExtentRef = device.getSwapChainExtent();
    VkViewport viewport{};
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = (float)swapChainExtentRef.width;
    viewport.height = (float)swapChainExtentRef.height;
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;

    VkRect2D scissor{};
    scissor.offset = { 0, 0 };
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

    VkPushConstantRange pushconstantRange = { .stageFlags =
                                              VK_SHADER_STAGE_VERTEX_BIT,
                                          .offset = 0,
                                          .size = sizeof(mat4) };

    VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
     pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
     pipelineLayoutInfo.setLayoutCount = 1;
     pipelineLayoutInfo.pSetLayouts = &globalSetLayout;//TODO: use seperate layout??
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
    pipelineInfo.layout = pipelineLayout;//TODO: seperate layout? currently just reuse
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

    if (vkCreateGraphicsPipelines(device.getLogicalDevice(), VK_NULL_HANDLE, 1, &pipelineInfo,
        nullptr, &drawOccluderPipeline) != VK_SUCCESS) {
        throw std::runtime_error("failed to create graphics pipeline!");
    }
}

void GpuScene::createComputePipeline()
{
    auto computeShaderCode = readFile((_rootPath / "shaders/gpucull.scc.spv").generic_string());
    VkShaderModule computeShaderModule = createShaderModule(computeShaderCode);
    VkPipelineShaderStageCreateInfo computeStageInfo{};
    computeStageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    computeStageInfo.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    computeStageInfo.module = computeShaderModule;
    computeStageInfo.pName = "EncodeDrawBuffer";


    VkPipelineLayoutCreateInfo encodeDrawBufferPipelineLayoutInfo{};
    encodeDrawBufferPipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    encodeDrawBufferPipelineLayoutInfo.setLayoutCount = 1;
    encodeDrawBufferPipelineLayoutInfo.pSetLayouts = &gpuCullSetLayout;
    encodeDrawBufferPipelineLayoutInfo.pushConstantRangeCount = 0;

    if (vkCreatePipelineLayout(device.getLogicalDevice(), &encodeDrawBufferPipelineLayoutInfo,
        nullptr, &encodeDrawBufferPipelineLayout) != VK_SUCCESS) {
        throw std::runtime_error("failed to create drawcluster pipeline layout!");
    }
    VkComputePipelineCreateInfo computePipelineCreateInfo{};
    computePipelineCreateInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    computePipelineCreateInfo.layout = encodeDrawBufferPipelineLayout;
    computePipelineCreateInfo.stage = computeStageInfo;
    computePipelineCreateInfo.basePipelineHandle = VK_NULL_HANDLE;

    vkCreateComputePipelines(device.getLogicalDevice(), VK_NULL_HANDLE, 1, &computePipelineCreateInfo, nullptr, &encodeDrawBufferPipeline);

}

void GpuScene::createGraphicsPipeline(VkRenderPass renderPass) {
    // TODO: shader management -- hot reload
    auto vertShaderCode = readFile((_rootPath / "shaders/vert.spv").generic_string());
    auto fragShaderCode = readFile((_rootPath / "shaders/frag.spv").generic_string());

    auto evertShaderCode = readFile((_rootPath / "shaders/edward.vs.spv").generic_string());
    auto efragShaderCode = readFile((_rootPath / "shaders/edward.ps.spv").generic_string());

    auto drawClusterVSShaderCode = readFile((_rootPath / "shaders/drawcluster.vs.spv").generic_string());
    auto drawClusterPSShaderCode = readFile((_rootPath / "shaders/drawcluster.ps.spv").generic_string());

    auto drawClusterBasePSShaderCode = readFile((_rootPath / "shaders/drawcluster.base.ps.spv").generic_string());

    auto drawClusterForwardPsShaderCode = readFile((_rootPath / "shaders/drawcluster.forward.ps.spv").generic_string());

    auto deferredLightingVSShaderCode = readFile((_rootPath / "shaders/deferredlighting.vs.spv").generic_string());
    auto deferredLightingPSShaderCode = readFile((_rootPath / "shaders/deferredlighting.ps.spv").generic_string());

    VkShaderModule vertShaderModule = createShaderModule(vertShaderCode);
    VkShaderModule fragShaderModule = createShaderModule(fragShaderCode);

    VkShaderModule evertShaderModule = createShaderModule(evertShaderCode);
    VkShaderModule efragShaderModule = createShaderModule(efragShaderCode);

    VkShaderModule drawclusterVSShaderModule = createShaderModule(drawClusterVSShaderCode);
    VkShaderModule drawclusterPSShaderModule = createShaderModule(drawClusterPSShaderCode);
    VkShaderModule drawclusterBasePSShaderModule = createShaderModule(drawClusterBasePSShaderCode);
    VkShaderModule drawclusterForwardPSShaderModule = createShaderModule(drawClusterForwardPsShaderCode);

    VkShaderModule deferredLightingVSShaderModule = createShaderModule(deferredLightingVSShaderCode);
    VkShaderModule deferredLightingPSShaderModule = createShaderModule(deferredLightingPSShaderCode);

    VkPipelineShaderStageCreateInfo vertShaderStageInfo{};
    vertShaderStageInfo.sType =
        VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    vertShaderStageInfo.stage = VK_SHADER_STAGE_VERTEX_BIT;
    vertShaderStageInfo.module = vertShaderModule;
    vertShaderStageInfo.pName = "main";

    VkPipelineShaderStageCreateInfo fragShaderStageInfo{};
    fragShaderStageInfo.sType =
        VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    fragShaderStageInfo.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    fragShaderStageInfo.module = fragShaderModule;
    fragShaderStageInfo.pName = "main";

    VkPipelineShaderStageCreateInfo evertShaderStageInfo{};
    evertShaderStageInfo.sType =
        VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    evertShaderStageInfo.stage = VK_SHADER_STAGE_VERTEX_BIT;
    evertShaderStageInfo.module = evertShaderModule;
    evertShaderStageInfo.pName = "RenderSceneVS";

    VkPipelineShaderStageCreateInfo efragShaderStageInfo{};
    efragShaderStageInfo.sType =
        VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    efragShaderStageInfo.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    efragShaderStageInfo.module = efragShaderModule;
    efragShaderStageInfo.pName = "RenderScenePS";

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
    deferredLightingVSShaderStageInfo.pName = "AAPLSimpleTexVertexOutFSQuadVertexShader";

    VkPipelineShaderStageCreateInfo drawclusterPSShaderStageInfo{};
    drawclusterPSShaderStageInfo.sType =
        VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    drawclusterPSShaderStageInfo.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    drawclusterPSShaderStageInfo.module = drawclusterPSShaderModule;
    drawclusterPSShaderStageInfo.pName = "RenderSceneBasePS";



    VkSpecializationMapEntry mapEntry = {};
    mapEntry.constantID = 0; // matches constant_id in GLSL and SpecId in SPIR-V
    mapEntry.offset     = 0;
    mapEntry.size       = sizeof(VkBool32);

    VkBool32 alphaMask = true;
    VkSpecializationInfo specializationInfo = {};
    specializationInfo.mapEntryCount = 1;
    specializationInfo.pMapEntries   = &mapEntry;
    specializationInfo.dataSize      = sizeof(VkBool32);
    specializationInfo.pData         = &alphaMask;
    
    VkPipelineShaderStageCreateInfo drawclusterPSShaderStageInfoAlphaMask{};
    drawclusterPSShaderStageInfoAlphaMask.sType =
        VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    drawclusterPSShaderStageInfoAlphaMask.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    drawclusterPSShaderStageInfoAlphaMask.module = drawclusterPSShaderModule;
    drawclusterPSShaderStageInfoAlphaMask.pName = "RenderSceneBasePS";
    drawclusterPSShaderStageInfoAlphaMask.pSpecializationInfo = &specializationInfo;


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
    drawclusterForwardPSShaderStageInfo.module = drawclusterForwardPSShaderModule;//TODO: 这几个module应该可以合并，在dxc中添加适当的参数？
    drawclusterForwardPSShaderStageInfo.pName = "RenderSceneForwardPS";

    VkPipelineShaderStageCreateInfo deferredLightingPSShaderStageInfo{};
    deferredLightingPSShaderStageInfo.sType =
        VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    deferredLightingPSShaderStageInfo.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    deferredLightingPSShaderStageInfo.module = deferredLightingPSShaderModule;
    deferredLightingPSShaderStageInfo.pName = "DeferredLighting";

    VkPipelineShaderStageCreateInfo shaderStages[] = { vertShaderStageInfo,
                                                      fragShaderStageInfo };
    VkPipelineShaderStageCreateInfo eshaderStages[] = { evertShaderStageInfo,
                                                       efragShaderStageInfo };
    VkPipelineShaderStageCreateInfo drawclusterShaderStages[] = { drawclusterVSShaderStageInfo,
                                                      drawclusterPSShaderStageInfo };

    VkPipelineShaderStageCreateInfo drawclusterShaderStagesAlphaMask[] = { drawclusterVSShaderStageInfo,
                                                      drawclusterPSShaderStageInfoAlphaMask };


    VkPipelineShaderStageCreateInfo drawclusterBasePassStages[] = { drawclusterVSShaderStageInfo,
                                                      drawclusterBasePSShaderStageInfo };


    VkPipelineShaderStageCreateInfo drawclusterForwardStages[] = { drawclusterVSShaderStageInfo,
                                                      drawclusterForwardPSShaderStageInfo };


    VkPipelineShaderStageCreateInfo deferredLightingPassStages[] = { deferredLightingVSShaderStageInfo,
                                                   deferredLightingPSShaderStageInfo };


    VkPipelineVertexInputStateCreateInfo vertexInputInfo{};
    vertexInputInfo.sType =
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInputInfo.vertexBindingDescriptionCount = 0;
    vertexInputInfo.vertexAttributeDescriptionCount = 0;

    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.sType =
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;//change to strip
    inputAssembly.primitiveRestartEnable = VK_FALSE;

    const VkExtent2D& swapChainExtentRef = device.getSwapChainExtent();
    VkViewport viewport{};
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = (float)swapChainExtentRef.width;
    viewport.height = (float)swapChainExtentRef.height;
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;

    VkRect2D scissor{};
    scissor.offset = { 0, 0 };
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
    rasterizerBackFace.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
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
    //colorBlendAttachment1.alphaBlendOp = VK_BLEND_OP_SRC_EXT;
    //colorBlendAttachment1.srcAlphaBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;


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

    for (int i = 0; i < 4; i ++ )
    {
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
   





    VkPushConstantRange drawclusterpushconstantRange = { .stageFlags =
                                                  VK_SHADER_STAGE_FRAGMENT_BIT,
                                              .offset = 0,
                                              .size = sizeof(uint32_t) };
    VkPipelineLayoutCreateInfo drawclusterpipelineLayoutInfo{};
    drawclusterpipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    drawclusterpipelineLayoutInfo.setLayoutCount = 1;
    drawclusterpipelineLayoutInfo.pSetLayouts = &applSetLayout;
    drawclusterpipelineLayoutInfo.pushConstantRangeCount = 1;
    drawclusterpipelineLayoutInfo.pPushConstantRanges = &drawclusterpushconstantRange;

    if (vkCreatePipelineLayout(device.getLogicalDevice(), &drawclusterpipelineLayoutInfo,
        nullptr, &drawclusterPipelineLayout) != VK_SUCCESS) {
        throw std::runtime_error("failed to create drawcluster pipeline layout!");
    }

    VkPipelineLayoutCreateInfo drawclusterBasePipelineLayoutInfo{};
    drawclusterBasePipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    drawclusterBasePipelineLayoutInfo.setLayoutCount = 1;
    drawclusterBasePipelineLayoutInfo.pSetLayouts = &applSetLayout;
    drawclusterBasePipelineLayoutInfo.pushConstantRangeCount = 0;

    if (vkCreatePipelineLayout(device.getLogicalDevice(), &drawclusterBasePipelineLayoutInfo,
        nullptr, &drawclusterBasePipelineLayout) != VK_SUCCESS) {
        throw std::runtime_error("failed to create drawcluster pipeline layout!");
    }

    VkPipelineLayoutCreateInfo deferredLightingPipelineLayoutInfo{};
    deferredLightingPipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    deferredLightingPipelineLayoutInfo.setLayoutCount = 1;
    deferredLightingPipelineLayoutInfo.pSetLayouts = &deferredLightingSetLayout;
    deferredLightingPipelineLayoutInfo.pushConstantRangeCount = 0;

    if (vkCreatePipelineLayout(device.getLogicalDevice(), &deferredLightingPipelineLayoutInfo,
        nullptr, &deferredLightingPipelineLayout) != VK_SUCCESS) {
        throw std::runtime_error("failed to create drawcluster pipeline layout!");
    }

    VkPipelineDepthStencilStateCreateInfo depthStencilState1{};
    depthStencilState1.sType =
        VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencilState1.depthWriteEnable = VK_FALSE;
    depthStencilState1.depthTestEnable = VK_FALSE;
    depthStencilState1.stencilTestEnable = VK_FALSE;

    VkVertexInputBindingDescription edwardInputBinding = {
       .binding = 0,
       .stride = sizeof(float) * 3 * 2 + sizeof(float) * 2,
       .inputRate = VK_VERTEX_INPUT_RATE_VERTEX };

    VkVertexInputAttributeDescription edwardInputAttributes[] = {
        {.location = 0,
         .binding = 0,
         .format = VK_FORMAT_R32G32B32_SFLOAT,
         .offset = 0},
        {.location = 1,
         .binding = 0,
         .format = VK_FORMAT_R32G32B32_SFLOAT,
         .offset = sizeof(float) * 3},
        {.location = 2,
         .binding = 0,
         .format = VK_FORMAT_R32G32_SFLOAT,
         .offset = sizeof(float) * 3 * 2} };

    VkPipelineVertexInputStateCreateInfo edwardVertexInputInfo{};
    edwardVertexInputInfo.sType =
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    edwardVertexInputInfo.vertexBindingDescriptionCount = 1;
    edwardVertexInputInfo.pVertexBindingDescriptions = &edwardInputBinding;
    edwardVertexInputInfo.vertexAttributeDescriptionCount = 3;
    edwardVertexInputInfo.pVertexAttributeDescriptions = edwardInputAttributes;

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

     if (vkCreateGraphicsPipelines(device.getLogicalDevice(), VK_NULL_HANDLE, 1, &edwardpipelineInfo,
                                   nullptr, &egraphicsPipeline) != VK_SUCCESS) {
       throw std::runtime_error("failed to create edward graphics pipeline!");
     }*/


    constexpr VkVertexInputBindingDescription drawClusterInputBindingPosition = {
      .binding = 0,
      .stride = sizeof(float) * 3,
      .inputRate = VK_VERTEX_INPUT_RATE_VERTEX };
    constexpr VkVertexInputBindingDescription drawClusterInputBindingNormal = {
    .binding = 1,
    .stride = sizeof(float) * 3,
    .inputRate = VK_VERTEX_INPUT_RATE_VERTEX };
    constexpr VkVertexInputBindingDescription drawClusterInputBindingTangent = {
    .binding = 2,
    .stride = sizeof(float) * 3,
    .inputRate = VK_VERTEX_INPUT_RATE_VERTEX };
    constexpr VkVertexInputBindingDescription drawClusterInputBindingUV = {
    .binding = 3,
    .stride = sizeof(float) * 2,
    .inputRate = VK_VERTEX_INPUT_RATE_VERTEX };

    constexpr VkVertexInputBindingDescription drawClusterInputBindingInstance = {
        .binding = 4,
        .stride = sizeof(uint32_t),
        .inputRate = VK_VERTEX_INPUT_RATE_INSTANCE
    };

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
         .offset = 0},


         {.location = 4,
         .binding = 4,
         .format = VK_FORMAT_R32_UINT,
         .offset = 0}
    };

    constexpr int inputChannelCount = sizeof(drawclusterInputAttributes) / sizeof(drawclusterInputAttributes[0]);

    constexpr std::array<VkVertexInputBindingDescription, inputChannelCount> drawculsterinputs = { drawClusterInputBindingPosition ,drawClusterInputBindingNormal ,drawClusterInputBindingTangent ,drawClusterInputBindingUV,drawClusterInputBindingInstance };

    VkPipelineVertexInputStateCreateInfo drawclusterVertexInputInfo{};
    drawclusterVertexInputInfo.sType =
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    drawclusterVertexInputInfo.vertexBindingDescriptionCount = drawculsterinputs.size();
    drawclusterVertexInputInfo.pVertexBindingDescriptions = drawculsterinputs.data();
    drawclusterVertexInputInfo.vertexAttributeDescriptionCount = inputChannelCount;
    drawclusterVertexInputInfo.pVertexAttributeDescriptions = drawclusterInputAttributes;

    VkGraphicsPipelineCreateInfo drawclusterpipelineInfo{};
    drawclusterpipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
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

    if (vkCreateGraphicsPipelines(device.getLogicalDevice(), VK_NULL_HANDLE, 1, &drawclusterpipelineInfo,
        nullptr, &drawclusterPipeline) != VK_SUCCESS) {
        throw std::runtime_error("failed to create drawcluster graphics pipeline!");
    }

    drawclusterpipelineInfo.pStages = drawclusterShaderStagesAlphaMask;
    if (vkCreateGraphicsPipelines(device.getLogicalDevice(), VK_NULL_HANDLE, 1, &drawclusterpipelineInfo,
        nullptr, &drawclusterPipelineAlphaMask) != VK_SUCCESS) {
        throw std::runtime_error("failed to create drawcluster graphics pipeline!");
    }

    VkGraphicsPipelineCreateInfo drawclusterBasePipelineInfo{};
    drawclusterBasePipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
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

    if (vkCreateGraphicsPipelines(device.getLogicalDevice(), VK_NULL_HANDLE, 1, &drawclusterBasePipelineInfo,
        nullptr, &drawclusterBasePipeline) != VK_SUCCESS) {
        throw std::runtime_error("failed to create drawcluster base graphics pipeline!");
    }

    VkGraphicsPipelineCreateInfo drawclusterForwardPipelineInfo{};
    drawclusterForwardPipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    drawclusterForwardPipelineInfo.stageCount = 2;
    drawclusterForwardPipelineInfo.pStages = drawclusterForwardStages;
    drawclusterForwardPipelineInfo.pVertexInputState = &drawclusterVertexInputInfo;
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

    if (vkCreateGraphicsPipelines(device.getLogicalDevice(), VK_NULL_HANDLE, 1, &drawclusterForwardPipelineInfo,
        nullptr, &drawclusterForwardPipeline) != VK_SUCCESS) {
        throw std::runtime_error("failed to create drawcluster base graphics pipeline!");
    }


    VkGraphicsPipelineCreateInfo deferredLightingPipelineInfo{};
    deferredLightingPipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    deferredLightingPipelineInfo.stageCount = 2;
    deferredLightingPipelineInfo.pStages = deferredLightingPassStages;
    deferredLightingPipelineInfo.pVertexInputState = &edwardVertexInputInfo;//TODO: don't need that?
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

    if (vkCreateGraphicsPipelines(device.getLogicalDevice(), VK_NULL_HANDLE, 1, &deferredLightingPipelineInfo,
        nullptr, &deferredLightingPipeline) != VK_SUCCESS) {
        throw std::runtime_error("failed to create drawcluster base graphics pipeline!");
    }


    vkDestroyShaderModule(device.getLogicalDevice(), fragShaderModule, nullptr);
    vkDestroyShaderModule(device.getLogicalDevice(), vertShaderModule, nullptr);

    vkDestroyShaderModule(device.getLogicalDevice(), evertShaderModule, nullptr);
    vkDestroyShaderModule(device.getLogicalDevice(), efragShaderModule, nullptr);


    vkDestroyShaderModule(device.getLogicalDevice(), drawclusterVSShaderModule, nullptr);
    vkDestroyShaderModule(device.getLogicalDevice(), drawclusterPSShaderModule, nullptr);

    //TODO : Destroy shader modules
}

enum MTLPixelFormat
{
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
     @abstract A pixel format where the red and green channels are subsampled horizontally.  Two pixels are stored in 32 bits, with shared red and blue values, and unique green values.
     @discussion This format is equivalent to YUY2, YUYV, yuvs, or GL_RGB_422_APPLE/GL_UNSIGNED_SHORT_8_8_REV_APPLE.   The component order, from lowest addressed byte to highest, is Y0, Cb, Y1, Cr.  There is no implicit colorspace conversion from YUV to RGB, the shader will receive (Cr, Y, Cb, 1).  422 textures must have a width that is a multiple of 2, and can only be used for 2D non-mipmap textures.  When sampling, ClampToEdge is the only usable wrap mode.
     */
    MTLPixelFormatGBGR422 = 240,

    /*!
     @constant MTLPixelFormatBGRG422
     @abstract A pixel format where the red and green channels are subsampled horizontally.  Two pixels are stored in 32 bits, with shared red and blue values, and unique green values.
     @discussion This format is equivalent to UYVY, 2vuy, or GL_RGB_422_APPLE/GL_UNSIGNED_SHORT_8_8_APPLE. The component order, from lowest addressed byte to highest, is Cb, Y0, Cr, Y1.  There is no implicit colorspace conversion from YUV to RGB, the shader will receive (Cr, Y, Cb, 1).  422 textures must have a width that is a multiple of 2, and can only be used for 2D non-mipmap textures.  When sampling, ClampToEdge is the only usable wrap mode.
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

VkFormat mapFromApple(MTLPixelFormat appleformat)
{
    switch (appleformat)
    {
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


void GpuScene::init_deferredlighting_descriptors()
{
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

    VkDescriptorSetLayoutBinding frameDataBinding = {};
    frameDataBinding.binding = 6;
    frameDataBinding.descriptorCount = 1;
    // it's a uniform buffer binding
    frameDataBinding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    frameDataBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;



    VkDescriptorSetLayoutBinding bindings[] = { albedoBinding, normalBinding, emessiveBinding, f0RoughnessBinding,depthBinding, nearestClampSamplerBinding,frameDataBinding };

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
        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 10},
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 3},
        {VK_DESCRIPTOR_TYPE_SAMPLER, 3},
    };

    VkDescriptorPoolCreateInfo pool_info = {};
    pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_info.flags = 0;
    pool_info.maxSets = 2;
    pool_info.poolSizeCount = (uint32_t)sizes.size();
    pool_info.pPoolSizes = sizes.data();

    vkCreateDescriptorPool(device.getLogicalDevice(), &pool_info, nullptr,
        &deferredLightingDescriptorPool);

    VkDescriptorSetAllocateInfo allocInfo = {};
    allocInfo.pNext = nullptr;
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    // using the pool we just set
    allocInfo.descriptorPool = deferredLightingDescriptorPool;
    // only 1 descriptor
    allocInfo.descriptorSetCount = 1;
    // using the global data layout
    allocInfo.pSetLayouts = &deferredLightingSetLayout;

    vkAllocateDescriptorSets(device.getLogicalDevice(), &allocInfo,
        &deferredLightingDescriptorSet);

    // information about the buffer we want to point at in the descriptor
    VkDescriptorBufferInfo binfo;
    // it will be the camera buffer
    binfo.buffer = uniformBuffer;
    // at 0 offset
    binfo.offset = 0;
    // of the size of a camera data struct
    binfo.range = sizeof(FrameData);

    VkWriteDescriptorSet setWrite = {};
    setWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    setWrite.pNext = nullptr;

    // we are going to write into binding number 0
    setWrite.dstBinding = 6;
    // of the global descriptor
    setWrite.dstSet = deferredLightingDescriptorSet;

    setWrite.descriptorCount = 1;
    setWrite.dstArrayElement = 0;
    // and the type is uniform buffer
    setWrite.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    setWrite.pBufferInfo = &binfo;


    VkDescriptorImageInfo samplerinfo;
    samplerinfo.sampler = nearestClampSampler;
    VkWriteDescriptorSet setSampler = {};
    setSampler.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    setSampler.dstBinding = 5;
    setSampler.pNext = nullptr;
    setSampler.dstSet = deferredLightingDescriptorSet;
    setSampler.dstArrayElement = 0;
    setSampler.descriptorCount = 1;
    setSampler.pImageInfo = &samplerinfo;
    setSampler.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;



    std::array<VkDescriptorImageInfo, 4> imageinfo{};
    //imageinfo.resize(textures.size());
    for (int texturei = 0; texturei < 4; ++texturei)
    {
        imageinfo[texturei].imageView = _gbuffersView[texturei];
        imageinfo[texturei].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        //imageinfo[texturei].sampler = textureSampler;
    }


    VkWriteDescriptorSet setWriteTexture[4] = {};
    for (int i = 0; i < 4; i++)
    {
        setWriteTexture[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        setWriteTexture[i].pNext = nullptr;


        setWriteTexture[i].dstBinding = i;
        // of the global descriptor
        setWriteTexture[i].dstSet = deferredLightingDescriptorSet;
        setWriteTexture[i].dstArrayElement = 0;

        setWriteTexture[i].descriptorCount = 1;
        setWriteTexture[i].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        setWriteTexture[i].pImageInfo = &imageinfo[i];
    }
    //need transform layout?--yes!
    VkDescriptorImageInfo depthImageInfo{};
    depthImageInfo.imageView = device.getWindowDepthImageView();
    depthImageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkWriteDescriptorSet setWriteDepth;
    setWriteDepth.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    setWriteDepth.pNext = nullptr;
    setWriteDepth.dstBinding = 4;
    setWriteDepth.dstSet = deferredLightingDescriptorSet;
    setWriteDepth.dstArrayElement = 0;
    setWriteDepth.descriptorCount = 1;
    setWriteDepth.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    setWriteDepth.pImageInfo = &depthImageInfo;

    std::array< VkWriteDescriptorSet, 7> writes = { setWriteTexture[0],setWriteTexture[1],setWriteTexture[2],setWriteTexture[3],setWriteDepth,setSampler,setWrite  };

    vkUpdateDescriptorSets(device.getLogicalDevice(), writes.size(), writes.data(), 0, nullptr);
}

void GpuScene::init_drawparams_descriptors()
{
    VkDescriptorSetLayoutBinding  drawParamsBinding = {};
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

    VkDescriptorSetLayoutBinding instanceToDrawIDMapBinding = {};
    instanceToDrawIDMapBinding.binding = 5;
    instanceToDrawIDMapBinding.descriptorCount = 1;
    instanceToDrawIDMapBinding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    instanceToDrawIDMapBinding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;



    VkDescriptorSetLayoutBinding bindings[] = { drawParamsBinding, cullParamsBinding, meshChunksBinding ,writeIndexBinding,chunkIndicesBinding,instanceToDrawIDMapBinding };

    constexpr int bindingcount = sizeof(bindings) / sizeof(bindings[0]);

    VkDescriptorSetLayoutCreateInfo setinfo = {};
    setinfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    //setinfo.pNext = &flag_info;
    setinfo.pNext = nullptr;

    setinfo.bindingCount = bindingcount;
    setinfo.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
    // point to the camera buffer binding
    setinfo.pBindings = bindings;

    vkCreateDescriptorSetLayout(device.getLogicalDevice(), &setinfo, nullptr,
        &gpuCullSetLayout);

    std::vector<VkDescriptorPoolSize> sizes = {
      {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 10},
      {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 10},
    };

    VkDescriptorPoolCreateInfo pool_info = {};
    pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT_EXT;
    pool_info.maxSets = 10;
    pool_info.poolSizeCount = (uint32_t)sizes.size();
    pool_info.pPoolSizes = sizes.data();

    vkCreateDescriptorPool(device.getLogicalDevice(), &pool_info, nullptr,
        &gpuCullDescriptorPool);

    VkDescriptorSetAllocateInfo allocInfo = {};
    allocInfo.pNext = nullptr;
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    // using the pool we just set
    allocInfo.descriptorPool = gpuCullDescriptorPool;
    // only 1 descriptor
    allocInfo.descriptorSetCount = 1;
    // using the global data layout
    allocInfo.pSetLayouts = &gpuCullSetLayout;

    vkAllocateDescriptorSets(device.getLogicalDevice(), &allocInfo,
        &gpuCullDescriptorSet);



    // information about the buffer we want to point at in the descriptor

    VkDescriptorBufferInfo drawParamsBufferInfo;
    drawParamsBufferInfo.buffer = drawParamsBuffer;
    drawParamsBufferInfo.offset = 0;
    drawParamsBufferInfo.range = applMesh->_chunkCount * sizeof(VkDrawIndexedIndirectCommand);

    VkWriteDescriptorSet drawParamsWrite = {};
    drawParamsWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    drawParamsWrite.pNext = nullptr;

    // we are going to write into binding number 0
    drawParamsWrite.dstBinding = 0;
    // of the global descriptor
    drawParamsWrite.dstSet = gpuCullDescriptorSet;

    drawParamsWrite.descriptorCount = 1;
    drawParamsWrite.dstArrayElement = 0;
    // and the type is uniform buffer
    drawParamsWrite.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    drawParamsWrite.pBufferInfo = &drawParamsBufferInfo;


    VkDescriptorBufferInfo cullParamsBufferInfo;
    cullParamsBufferInfo.buffer = cullParamsBuffer;
    // at 0 offset
    cullParamsBufferInfo.offset = 0;
    cullParamsBufferInfo.range = sizeof(gpuCullParams);

    VkWriteDescriptorSet cullParamsWrite = {};
    cullParamsWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    cullParamsWrite.pNext = nullptr;

    // we are going to write into binding number 0
    cullParamsWrite.dstBinding = 1;
    // of the global descriptor
    cullParamsWrite.dstSet = gpuCullDescriptorSet;

    cullParamsWrite.descriptorCount = 1;
    cullParamsWrite.dstArrayElement = 0;
    // and the type is uniform buffer
    cullParamsWrite.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    cullParamsWrite.pBufferInfo = &cullParamsBufferInfo;


    VkDescriptorBufferInfo meshChunksBufferInfo;
    meshChunksBufferInfo.buffer = meshChunksBuffer;
    // at 0 offset
    meshChunksBufferInfo.offset = 0;
    meshChunksBufferInfo.range = sizeof(AAPLMeshChunk) * applMesh->_chunkCount;

    VkWriteDescriptorSet meshChunksBufferWrite = {};
    meshChunksBufferWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    meshChunksBufferWrite.pNext = nullptr;

    // we are going to write into binding number 0
    meshChunksBufferWrite.dstBinding = 2;
    // of the global descriptor
    meshChunksBufferWrite.dstSet = gpuCullDescriptorSet;

    meshChunksBufferWrite.descriptorCount = 1;
    meshChunksBufferWrite.dstArrayElement = 0;
    // and the type is uniform buffer
    meshChunksBufferWrite.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    meshChunksBufferWrite.pBufferInfo = &meshChunksBufferInfo;



    VkDescriptorBufferInfo writeIndexBufferInfo;
    writeIndexBufferInfo.buffer = writeIndexBuffer;
    // at 0 offset
    writeIndexBufferInfo.offset = 0;
    writeIndexBufferInfo.range = sizeof(uint32_t);

    VkWriteDescriptorSet writeIndexBufferWrite = {};
    writeIndexBufferWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writeIndexBufferWrite.pNext = nullptr;

    // we are going to write into binding number 0
    writeIndexBufferWrite.dstBinding = 3;
    // of the global descriptor
    writeIndexBufferWrite.dstSet = gpuCullDescriptorSet;

    writeIndexBufferWrite.descriptorCount = 1;
    writeIndexBufferWrite.dstArrayElement = 0;
    // and the type is uniform buffer
    writeIndexBufferWrite.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writeIndexBufferWrite.pBufferInfo = &writeIndexBufferInfo;

    VkDescriptorBufferInfo chunkIndicesBufferInfo;
    chunkIndicesBufferInfo.buffer = chunkIndicesBuffer;
    // at 0 offset
    chunkIndicesBufferInfo.offset = 0;
    chunkIndicesBufferInfo.range = sizeof(uint32_t) * applMesh->_chunkCount;

    VkWriteDescriptorSet chunkIndicesBufferWrite = {};
    chunkIndicesBufferWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    chunkIndicesBufferWrite.pNext = nullptr;

    // we are going to write into binding number 0
    chunkIndicesBufferWrite.dstBinding = 4;
    // of the global descriptor
    chunkIndicesBufferWrite.dstSet = gpuCullDescriptorSet;

    chunkIndicesBufferWrite.descriptorCount = 1;
    chunkIndicesBufferWrite.dstArrayElement = 0;
    // and the type is uniform buffer
    chunkIndicesBufferWrite.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    chunkIndicesBufferWrite.pBufferInfo = &chunkIndicesBufferInfo;



    VkDescriptorBufferInfo applInstanceBufferInfo;
    applInstanceBufferInfo.buffer = applInstanceBuffer;
    // at 0 offset
    applInstanceBufferInfo.offset = 0;
    applInstanceBufferInfo.range = sizeof(uint32_t) * applMesh->_chunkCount;

    VkWriteDescriptorSet instanceToDrawIDMapBufferWrite = {};
    instanceToDrawIDMapBufferWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    instanceToDrawIDMapBufferWrite.pNext = nullptr;

    // we are going to write into binding number 0
    instanceToDrawIDMapBufferWrite.dstBinding = 5;
    // of the global descriptor
    instanceToDrawIDMapBufferWrite.dstSet = gpuCullDescriptorSet;

    instanceToDrawIDMapBufferWrite.descriptorCount = 1;
    instanceToDrawIDMapBufferWrite.dstArrayElement = 0;
    // and the type is uniform buffer
    instanceToDrawIDMapBufferWrite.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    instanceToDrawIDMapBufferWrite.pBufferInfo = &applInstanceBufferInfo;



    std::array< VkWriteDescriptorSet, bindingcount> writes = { drawParamsWrite, cullParamsWrite , meshChunksBufferWrite, writeIndexBufferWrite,chunkIndicesBufferWrite,instanceToDrawIDMapBufferWrite };
    // std::array< VkWriteDescriptorSet, 3> writes = { uniformWrite, setWrite , setSampler };

    vkUpdateDescriptorSets(device.getLogicalDevice(), writes.size(), writes.data(), 0, nullptr);


}

void GpuScene::init_appl_descriptors()
{
    // information about the binding.
    VkDescriptorSetLayoutBinding uniformBufferBinding = {};
    uniformBufferBinding.binding = 0;
    uniformBufferBinding.descriptorCount = 1;
    // it's a uniform buffer binding
    uniformBufferBinding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;

    // we use it from the vertex shader
    uniformBufferBinding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutBinding matBinding = {};
    matBinding.binding = 1;
    matBinding.descriptorCount = 1;
    // it's a uniform buffer binding
    matBinding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    // we use it from the vertex shader
    matBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutBinding samplerBinding = {};
    samplerBinding.binding = 2;
    samplerBinding.descriptorCount = 1;
    samplerBinding.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    samplerBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutBinding textureBinding = {};
    textureBinding.binding = 3;
    textureBinding.descriptorCount = textures.size();
    textureBinding.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    textureBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutBinding meshChunksBinding = {};
    meshChunksBinding.binding = 4;
    meshChunksBinding.descriptorCount = 1;
    // it's a uniform buffer binding
    meshChunksBinding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    // we use it from the vertex shader
    meshChunksBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;


    VkDescriptorSetLayoutBinding chunkIndexBinding = {};
    chunkIndexBinding.binding = 5;
    chunkIndexBinding.descriptorCount = 1;
    // it's a uniform buffer binding
    chunkIndexBinding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    // we use it from the vertex shader
    chunkIndexBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;



    VkDescriptorSetLayoutBinding bindings[] = { uniformBufferBinding, matBinding, samplerBinding ,textureBinding,meshChunksBinding,chunkIndexBinding };

    constexpr int bindingcount = sizeof(bindings) / sizeof(bindings[0]);

    std::array<VkDescriptorBindingFlags, bindingcount> bindingFlags = { 0,0,0,VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT_EXT,0,0 };

    //VkDescriptorBindingFlags flag = VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT_EXT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT_EXT;

    VkDescriptorSetLayoutBindingFlagsCreateInfo flag_info = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO };
    flag_info.bindingCount = bindingcount;
    flag_info.pBindingFlags = bindingFlags.data();

    VkDescriptorSetLayoutCreateInfo setinfo = {};
    setinfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    //setinfo.pNext = &flag_info;
    setinfo.pNext = nullptr;

    setinfo.bindingCount = bindingcount;
    setinfo.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
    // point to the camera buffer binding
    setinfo.pBindings = bindings;

    vkCreateDescriptorSetLayout(device.getLogicalDevice(), &setinfo, nullptr,
        &applSetLayout);

    std::vector<VkDescriptorPoolSize> sizes = {
      {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 10},
      {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 10},
      {VK_DESCRIPTOR_TYPE_SAMPLER, 10},
      {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,4096} };

    VkDescriptorPoolCreateInfo pool_info = {};
    pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT_EXT;
    pool_info.maxSets = 10;
    pool_info.poolSizeCount = (uint32_t)sizes.size();
    pool_info.pPoolSizes = sizes.data();

    vkCreateDescriptorPool(device.getLogicalDevice(), &pool_info, nullptr,
        &applDescriptorPool);

    VkDescriptorSetAllocateInfo allocInfo = {};
    allocInfo.pNext = nullptr;
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    // using the pool we just set
    allocInfo.descriptorPool = applDescriptorPool;
    // only 1 descriptor
    allocInfo.descriptorSetCount = 1;
    // using the global data layout
    allocInfo.pSetLayouts = &applSetLayout;

    vkAllocateDescriptorSets(device.getLogicalDevice(), &allocInfo,
        &applDescriptorSet);



    // information about the buffer we want to point at in the descriptor

    VkDescriptorBufferInfo unibinfo;
    // it will be the camera buffer
    unibinfo.buffer = uniformBuffer;
    // at 0 offset
    unibinfo.offset = 0;
    // of the size of a camera data struct
    unibinfo.range = sizeof(FrameData);

    VkWriteDescriptorSet uniformWrite = {};
    uniformWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    uniformWrite.pNext = nullptr;

    // we are going to write into binding number 0
    uniformWrite.dstBinding = 0;
    // of the global descriptor
    uniformWrite.dstSet = applDescriptorSet;

    uniformWrite.descriptorCount = 1;
    uniformWrite.dstArrayElement = 0;
    // and the type is uniform buffer
    uniformWrite.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    uniformWrite.pBufferInfo = &unibinfo;


    VkDescriptorBufferInfo binfo;
    binfo.buffer = applMaterialBuffer;
    // at 0 offset
    binfo.offset = 0;
    binfo.range = sizeof(AAPLShaderMaterial) * materials.size();

    VkWriteDescriptorSet setWrite = {};
    setWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    setWrite.pNext = nullptr;

    // we are going to write into binding number 0
    setWrite.dstBinding = 1;
    // of the global descriptor
    setWrite.dstSet = applDescriptorSet;

    setWrite.descriptorCount = 1;
    setWrite.dstArrayElement = 0;
    // and the type is uniform buffer
    setWrite.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    setWrite.pBufferInfo = &binfo;

    VkDescriptorImageInfo samplerinfo;
    samplerinfo.sampler = textureSampler;
    VkWriteDescriptorSet setSampler = {};
    setSampler.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    setSampler.dstBinding = 2;
    setSampler.pNext = nullptr;
    setSampler.dstSet = applDescriptorSet;
    setSampler.dstArrayElement = 0;
    setSampler.descriptorCount = 1;
    setSampler.pImageInfo = &samplerinfo;
    setSampler.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;

    std::vector<VkDescriptorImageInfo> imageinfo;
    imageinfo.resize(textures.size());
    for (int texturei = 0; texturei < textures.size(); ++texturei)
    {
        imageinfo[texturei].imageView = textures[texturei].second;
        imageinfo[texturei].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        //imageinfo[texturei].sampler = textureSampler;
    }


    VkWriteDescriptorSet setWriteTexture = {};
    setWriteTexture.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    setWriteTexture.pNext = nullptr;


    setWriteTexture.dstBinding = 3;
    // of the global descriptor
    setWriteTexture.dstSet = applDescriptorSet;
    setWriteTexture.dstArrayElement = 0;

    setWriteTexture.descriptorCount = textures.size();
    setWriteTexture.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    setWriteTexture.pImageInfo = imageinfo.data();


    VkDescriptorBufferInfo meshChunksBufferInfo;
    meshChunksBufferInfo.buffer = meshChunksBuffer;
    // at 0 offset
    meshChunksBufferInfo.offset = 0;
    meshChunksBufferInfo.range = sizeof(AAPLMeshChunk) * applMesh->_chunkCount;

    VkWriteDescriptorSet meshChunksWrite = {};
    meshChunksWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    meshChunksWrite.pNext = nullptr;

    // we are going to write into binding number 0
    meshChunksWrite.dstBinding = 4;
    // of the global descriptor
    meshChunksWrite.dstSet = applDescriptorSet;

    meshChunksWrite.descriptorCount = 1;
    meshChunksWrite.dstArrayElement = 0;
    // and the type is uniform buffer
    meshChunksWrite.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    meshChunksWrite.pBufferInfo = &meshChunksBufferInfo;

    VkDescriptorBufferInfo chunkIndexBufferInfo;
    chunkIndexBufferInfo.buffer = chunkIndicesBuffer;
    // at 0 offset
    chunkIndexBufferInfo.offset = 0;
    chunkIndexBufferInfo.range = sizeof(uint32_t) * applMesh->_chunkCount;

    VkWriteDescriptorSet chunkIndexBufferWrite = {};
    chunkIndexBufferWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    chunkIndexBufferWrite.pNext = nullptr;

    // we are going to write into binding number 0
    chunkIndexBufferWrite.dstBinding = 5;
    // of the global descriptor
    chunkIndexBufferWrite.dstSet = applDescriptorSet;

    chunkIndexBufferWrite.descriptorCount = 1;
    chunkIndexBufferWrite.dstArrayElement = 0;
    // and the type is uniform buffer
    chunkIndexBufferWrite.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    chunkIndexBufferWrite.pBufferInfo = &chunkIndexBufferInfo;



    std::array< VkWriteDescriptorSet, bindingcount> writes = { uniformWrite, setWrite , setSampler, setWriteTexture,meshChunksWrite,chunkIndexBufferWrite };
    // std::array< VkWriteDescriptorSet, 3> writes = { uniformWrite, setWrite , setSampler };

    vkUpdateDescriptorSets(device.getLogicalDevice(), writes.size(), writes.data(), 0, nullptr);

}

void GpuScene::init_descriptorsV2()
{
    // information about the binding.
    VkDescriptorSetLayoutBinding camBufferBinding = {};
    camBufferBinding.binding = 0;
    camBufferBinding.descriptorCount = 1;
    // it's a uniform buffer binding
    camBufferBinding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;

    // we use it from the vertex shader
    camBufferBinding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;



    VkDescriptorSetLayoutBinding bindings[] = { camBufferBinding };

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
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 10},
    };

    VkDescriptorPoolCreateInfo pool_info = {};
    pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_info.flags = 0;
    pool_info.maxSets = 10;
    pool_info.poolSizeCount = (uint32_t)sizes.size();
    pool_info.pPoolSizes = sizes.data();

    vkCreateDescriptorPool(device.getLogicalDevice(), &pool_info, nullptr,
        &descriptorPool);

    VkDescriptorSetAllocateInfo allocInfo = {};
    allocInfo.pNext = nullptr;
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    // using the pool we just set
    allocInfo.descriptorPool = descriptorPool;
    // only 1 descriptor
    allocInfo.descriptorSetCount = 1;
    // using the global data layout
    allocInfo.pSetLayouts = &globalSetLayout;

    vkAllocateDescriptorSets(device.getLogicalDevice(), &allocInfo,
        &globalDescriptor);

    // information about the buffer we want to point at in the descriptor
    VkDescriptorBufferInfo binfo;
    // it will be the camera buffer
    binfo.buffer = uniformBuffer;
    // at 0 offset
    binfo.offset = 0;
    // of the size of a camera data struct
    binfo.range = sizeof(FrameData);

    VkWriteDescriptorSet setWrite = {};
    setWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    setWrite.pNext = nullptr;

    // we are going to write into binding number 0
    setWrite.dstBinding = 0;
    // of the global descriptor
    setWrite.dstSet = globalDescriptor;

    setWrite.descriptorCount = 1;
    setWrite.dstArrayElement = 0;
    // and the type is uniform buffer
    setWrite.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    setWrite.pBufferInfo = &binfo;

    //vkUpdateDescriptorSets(device.getLogicalDevice(), 1, &setWrite, 0, nullptr);




    std::array< VkWriteDescriptorSet, 1> writes = { setWrite };

    vkUpdateDescriptorSets(device.getLogicalDevice(), writes.size(), writes.data(), 0, nullptr);
}

void GpuScene::init_descriptors(VkImageView currentImage) {

    // information about the binding.
    VkDescriptorSetLayoutBinding camBufferBinding = {};
    camBufferBinding.binding = 0;
    camBufferBinding.descriptorCount = 1;
    // it's a uniform buffer binding
    camBufferBinding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;

    // we use it from the vertex shader
    camBufferBinding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;

    VkDescriptorSetLayoutBinding samplerBinding = {};
    samplerBinding.binding = 1;
    samplerBinding.descriptorCount = 1;
    samplerBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    samplerBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;


    VkDescriptorSetLayoutBinding bindings[] = { camBufferBinding,samplerBinding };

    VkDescriptorSetLayoutCreateInfo setinfo = {};
    setinfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    setinfo.pNext = nullptr;

    // we are going to have 1 binding
    setinfo.bindingCount = 2;
    // no flags
    setinfo.flags = 0;
    // point to the camera buffer binding
    setinfo.pBindings = bindings;

    vkCreateDescriptorSetLayout(device.getLogicalDevice(), &setinfo, nullptr,
        &globalSetLayout);

    // other code ....
    // create a descriptor pool that will hold 10 uniform buffers
    std::vector<VkDescriptorPoolSize> sizes = {
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 10},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,10} };

    VkDescriptorPoolCreateInfo pool_info = {};
    pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_info.flags = 0;
    pool_info.maxSets = 10;
    pool_info.poolSizeCount = (uint32_t)sizes.size();
    pool_info.pPoolSizes = sizes.data();

    vkCreateDescriptorPool(device.getLogicalDevice(), &pool_info, nullptr,
        &descriptorPool);

    VkDescriptorSetAllocateInfo allocInfo = {};
    allocInfo.pNext = nullptr;
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    // using the pool we just set
    allocInfo.descriptorPool = descriptorPool;
    // only 1 descriptor
    allocInfo.descriptorSetCount = 1;
    // using the global data layout
    allocInfo.pSetLayouts = &globalSetLayout;

    vkAllocateDescriptorSets(device.getLogicalDevice(), &allocInfo,
        &globalDescriptor);

    // information about the buffer we want to point at in the descriptor
    VkDescriptorBufferInfo binfo;
    // it will be the camera buffer
    binfo.buffer = uniformBuffer;
    // at 0 offset
    binfo.offset = 0;
    // of the size of a camera data struct
    binfo.range = sizeof(FrameData);

    VkWriteDescriptorSet setWrite = {};
    setWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    setWrite.pNext = nullptr;

    // we are going to write into binding number 0
    setWrite.dstBinding = 0;
    // of the global descriptor
    setWrite.dstSet = globalDescriptor;

    setWrite.descriptorCount = 1;
    setWrite.dstArrayElement = 0;
    // and the type is uniform buffer
    setWrite.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    setWrite.pBufferInfo = &binfo;

    //vkUpdateDescriptorSets(device.getLogicalDevice(), 1, &setWrite, 0, nullptr);

    VkDescriptorImageInfo imageinfo;
    imageinfo.imageView = currentImage;
    imageinfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imageinfo.sampler = textureSampler;


    VkWriteDescriptorSet setWriteTexture = {};
    setWriteTexture.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    setWriteTexture.pNext = nullptr;

    // we are going to write into binding number 0
    setWriteTexture.dstBinding = 1;
    // of the global descriptor
    setWriteTexture.dstSet = globalDescriptor;
    setWriteTexture.dstArrayElement = 0;

    setWriteTexture.descriptorCount = 1;
    setWriteTexture.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;//TODO: 换成VK_DESCRIPTOR_TYPE_SAMPLER会无效
    setWriteTexture.pImageInfo = &imageinfo;

    std::array< VkWriteDescriptorSet, 2> writes = { setWrite , setWriteTexture };

    vkUpdateDescriptorSets(device.getLogicalDevice(), writes.size(), writes.data(), 0, nullptr);


    //AAPLDescriptorSets
}

void GpuScene::CreateTextures()
{
    for (auto& texture : applMesh->_textures)
    {
        textureHashMap[texture._pathHash] = textures.size();
        textures.push_back(createTexture(texture));
    }
}

void GpuScene::CreateGBuffers() {

    int width = device.getSwapChainExtent().width;
    int height = device.getSwapChainExtent().height;

    for (int i = 0; i < 4; i++) {
        VkImageCreateInfo imageInfo{};
        imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.extent.width = width;
        imageInfo.extent.height = height;
        imageInfo.extent.depth = 1;
        imageInfo.mipLevels = 1; // texturedata._mipmapLevelCount;
        imageInfo.arrayLayers = 1;
        imageInfo.tiling =
            VK_IMAGE_TILING_OPTIMAL; // TODO: switch to linear with
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

        if (vkCreateImage(device.getLogicalDevice(), &imageInfo, nullptr,
            &_gbuffers[i]) != VK_SUCCESS) {
            throw std::runtime_error("failed to create depth rt!");
        }
        VkMemoryRequirements memRequirements;
        vkGetImageMemoryRequirements(device.getLogicalDevice(), _gbuffers[i],
            &memRequirements);

        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = memRequirements.size;
        allocInfo.memoryTypeIndex =
            findMemoryType(memRequirements.memoryTypeBits,
                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

        VkDeviceMemory textureImageMemory;
        if (vkAllocateMemory(device.getLogicalDevice(), &allocInfo, nullptr,
            &textureImageMemory) != VK_SUCCESS) {
            throw std::runtime_error("failed to allocate image memory!");
        }

        vkBindImageMemory(device.getLogicalDevice(), _gbuffers[i],
            textureImageMemory, 0);

        VkImageViewCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        createInfo.image = _gbuffers[i];
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
            &_gbuffersView[i]) != VK_SUCCESS) {
            throw std::runtime_error("failed to create gbuffers image views!");
        }
    }
}

void GpuScene::CreateDepthTexture()
{
    int width = device.getSwapChainExtent().width;
    int height = device.getSwapChainExtent().height;
    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.extent.width = width;
    imageInfo.extent.height = height;
    imageInfo.extent.depth = 1;
    imageInfo.mipLevels = 1;// texturedata._mipmapLevelCount;
    imageInfo.arrayLayers = 1;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;//TODO: switch to linear with initiallayout=preinitialized?
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;//VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    imageInfo.format = _depthFormat;//TODO:or VK_FORMAT_D32_SFLOAT_S8_UINT? we don't need stencil currently anyway
    imageInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.flags = 0; // Optional


    if (vkCreateImage(device.getLogicalDevice(), &imageInfo, nullptr, &_depthTexture) != VK_SUCCESS) {
        throw std::runtime_error("failed to create depth rt!");
    }

    VkMemoryRequirements memRequirements;
    vkGetImageMemoryRequirements(device.getLogicalDevice(), _depthTexture, &memRequirements);

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memRequirements.size;
    allocInfo.memoryTypeIndex = findMemoryType(memRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    VkDeviceMemory textureImageMemory;
    if (vkAllocateMemory(device.getLogicalDevice(), &allocInfo, nullptr, &textureImageMemory) != VK_SUCCESS) {
        throw std::runtime_error("failed to allocate image memory!");
    }

    vkBindImageMemory(device.getLogicalDevice(), _depthTexture, textureImageMemory, 0);

    //TODO: change to vk_image_layout_depth_attachment_optimal
    //transition param will be specified in renderpass
        //device.transitionImageLayout(_depthTexture, _depthFormat, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL );

        //TODO: must be power of 2
    int bigger = width > height ? width : height;
    int log_bigger = floor(log2f(bigger));
    int mip_level = log_bigger;

    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.extent.width = width;
    imageInfo.extent.height = height;
    imageInfo.extent.depth = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;//TODO: switch to linear with initiallayout=preinitialized?
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;//VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    imageInfo.format = VK_FORMAT_D32_SFLOAT;//TODO:or VK_FORMAT_D32_SFLOAT_S8_UINT? we don't need stencil currently anyway
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.flags = 0; // Optional


    imageInfo.mipLevels = mip_level;
    imageInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    //VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT;

    if (vkCreateImage(device.getLogicalDevice(), &imageInfo, nullptr, &_depthPyramidTexture) != VK_SUCCESS) {
        throw std::runtime_error("failed to create depth pyramid!");
    }

}

void GpuScene::ConfigureMaterial(const AAPLMaterial& input, AAPLShaderMaterial& output)
{
    if (input.hasBaseColorTexture && !textureHashMap.contains(input.baseColorTextureHash))
    {
        spdlog::error("texture hash {} is invalid", input.baseColorTextureHash);
    }
    if (input.hasNormalMap && !textureHashMap.contains(input.normalMapHash))
    {
        spdlog::error("texture hash {} is invalid", input.normalMapHash);
    }
    if (input.hasEmissiveTexture && !textureHashMap.contains(input.emissiveTextureHash))
    {
        spdlog::error("texture hash {} is invalid", input.emissiveTextureHash);
    }
    if (input.hasMetallicRoughnessTexture && !textureHashMap.contains(input.metallicRoughnessHash))
    {
        spdlog::error("texture hash {} is invalid", input.metallicRoughnessHash);
    }

    const uint32_t INVALID_TEXTURE_INDEX = 0xffffffff;
    output.albedo_texture_index = input.hasBaseColorTexture ? textureHashMap[input.baseColorTextureHash] : INVALID_TEXTURE_INDEX;
    output.normal_texture_index = input.hasNormalMap ? textureHashMap[input.normalMapHash] : INVALID_TEXTURE_INDEX;
    output.emissive_texture_index = input.hasEmissiveTexture ? textureHashMap[input.emissiveTextureHash] : INVALID_TEXTURE_INDEX;
    output.roughness_texture_index = input.hasMetallicRoughnessTexture ? textureHashMap[input.metallicRoughnessHash] : INVALID_TEXTURE_INDEX;
    output.hasEmissive = input.hasEmissiveTexture;
    output.hasMetallicRoughness = input.hasMetallicRoughnessTexture;
    output.alpha = input.opacity;
}


GpuScene::GpuScene(std::filesystem::path& root, const VulkanDevice& deviceref)
    : device(deviceref), modelScale(1.f), _rootPath(root) {

    createSyncObjects();
    createUniformBuffer();

    createCommandBuffer(deviceref.getCommandPool());

    applMesh = new AAPLMeshData((_rootPath / "debug1.bin").generic_string().c_str());

    std::ifstream f(root / "scene.scene");
    sceneFile = nlohmann::json::parse(f);

    //maincamera = new Camera(60 * 3.1414926f / 180.f, 0.1, 100, vec3(0, 0, -2),
    //                        deviceref.getSwapChainExtent().width /
    //                            float(deviceref.getSwapChainExtent().height));
    vec3 camera_pos = vec3(sceneFile["camera_position"][0].template get<float>(), sceneFile["camera_position"][1].template get<float>(), sceneFile["camera_position"][2].template get<float>());
    vec3 camera_up = vec3(sceneFile["camera_up"][0].template get<float>(), sceneFile["camera_up"][1].template get<float>(), sceneFile["camera_up"][2].template get<float>());
    vec3 camera_dir = vec3(sceneFile["camera_direction"][0].template get<float>(), sceneFile["camera_direction"][1].template get<float>(), sceneFile["camera_direction"][2].template get<float>());
    maincamera = new Camera(65 * 3.1414926f / 180.f, 0.1, 100, camera_pos,
        deviceref.getSwapChainExtent().width /
        float(deviceref.getSwapChainExtent().height), camera_dir, camera_up * -1);

    //maincamera = new Camera(90 * 3.1414926f / 180.f, 1, 100, vec3(0, 0, 0),
    //    deviceref.getSwapChainExtent().width /
    //    float(deviceref.getSwapChainExtent().height), vec3(0,0,1), vec3(0,1, 0));




    {
        VkBufferCreateInfo bufferInfo{};
        bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.size = sceneFile["occluder_indices"].size() * sizeof(uint32_t);//TODO uint16_t
        bufferInfo.usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        bufferInfo.flags = 0;
        if (vkCreateBuffer(device.getLogicalDevice(), &bufferInfo, nullptr,
            &_occludersIndexBuffer) != VK_SUCCESS) {
            throw std::runtime_error("failed to create vertex buffer!");
        }
        VkMemoryRequirements memRequirements;
        vkGetBufferMemoryRequirements(device.getLogicalDevice(), _occludersIndexBuffer,
            &memRequirements);

        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = memRequirements.size;
        allocInfo.memoryTypeIndex = findMemoryType(
            memRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

        if (vkAllocateMemory(device.getLogicalDevice(), &allocInfo, nullptr,
            &_occludersIndexBufferMemory) != VK_SUCCESS) {
            throw std::runtime_error("failed to allocate vertex buffer memory!");
        }
        vkBindBufferMemory(device.getLogicalDevice(), _occludersIndexBuffer,
            _occludersIndexBufferMemory, 0);
        uint32_t* data;
        vkMapMemory(device.getLogicalDevice(), _occludersIndexBufferMemory, 0, bufferInfo.size,
            0, (void**)&data);
        for (int i = 0; i < sceneFile["occluder_indices"].size(); ++i)
        {
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
    allocInfo.memoryTypeIndex = findMemoryType(
        memRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

    if (vkAllocateMemory(device.getLogicalDevice(), &allocInfo, nullptr,
        &_occludersBufferMemory) != VK_SUCCESS) {
        throw std::runtime_error("failed to allocate vertex buffer memory!");
    }
    vkBindBufferMemory(device.getLogicalDevice(), _occludersVertBuffer,
        _occludersBufferMemory, 0);
    float* data;
    float x_offset = sceneFile["center_offset"][0].template get<float>();
    float y_offset = sceneFile["center_offset"][1].template get<float>();
    float z_offset = sceneFile["center_offset"][2].template get<float>();
    vkMapMemory(device.getLogicalDevice(), _occludersBufferMemory, 0, bufferInfo.size,
        0, (void**)&data);
    for (int i = 0; i < sceneFile["occluder_verts"].size(); ++i)
    {
        //for (int j = 0; j < 3; j++)
        //{
        //z和y需要换下顺序
        float x = sceneFile["occluder_verts"][i][0].template get<float>();
        float z = sceneFile["occluder_verts"][i][1].template get<float>();
        float y = sceneFile["occluder_verts"][i][2].template get<float>();
        //}
        *data++ = x - x_offset; *data++ = y - y_offset; *data++ = z - z_offset;
    }

    vkUnmapMemory(device.getLogicalDevice(), _occludersBufferMemory);


    CreateTextures();

    if (sizeof(AAPLMaterial) != 96)
    {
        spdlog::error("layout mismatch with apple sizeof(AAPLMaterial) is {}, while apple is 96", sizeof(AAPLMaterial));
    }

    AAPLMaterial* decompressedMaterial = (AAPLMaterial*)uncompressData((unsigned char*)applMesh->_materialData, applMesh->compressedMaterialDataLength,
        applMesh->_materialCount * sizeof(AAPLMaterial));
    materials.resize(applMesh->_materialCount);
    for (int i = 0; i < applMesh->_materialCount; i++)
    {
        ConfigureMaterial(decompressedMaterial[i], materials[i]);
    };

    //create material buffer
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
        allocInfo.memoryTypeIndex = findMemoryType(
            memRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

        if (vkAllocateMemory(device.getLogicalDevice(), &allocInfo, nullptr,
            &applMaterialBufferMemory) != VK_SUCCESS) {
            throw std::runtime_error("failed to allocate vertex buffer memory!");
        }
        vkBindBufferMemory(device.getLogicalDevice(), applMaterialBuffer,
            applMaterialBufferMemory, 0);
        void* data;
        vkMapMemory(device.getLogicalDevice(), applMaterialBufferMemory, 0, bufferInfo.size,
            0, &data);
        memcpy(data, materials.data(), bufferInfo.size);
        vkUnmapMemory(device.getLogicalDevice(), applMaterialBufferMemory);
    }

    vec3* vertexs = (vec3*)uncompressData((unsigned char*)applMesh->_vertexData, applMesh->compressedVertexDataLength,
        [this](uint64_t buffersize) {
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
            vkGetBufferMemoryRequirements(device.getLogicalDevice(), applVertexBuffer,
                &memRequirements);

            VkMemoryAllocateInfo allocInfo{};
            allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            allocInfo.allocationSize = memRequirements.size;
            allocInfo.memoryTypeIndex = findMemoryType(
                memRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

            if (vkAllocateMemory(device.getLogicalDevice(), &allocInfo, nullptr,
                &applVertexBufferMemory) != VK_SUCCESS) {
                throw std::runtime_error("failed to allocate vertex buffer memory!");
            }
            vkBindBufferMemory(device.getLogicalDevice(), applVertexBuffer,
                applVertexBufferMemory, 0);
            void* data;
            vkMapMemory(device.getLogicalDevice(), applVertexBufferMemory, 0, bufferInfo.size,
                0, &data);
            return data;
        }
    );//applMesh->_vertexCount * sizeof(vec3));
    //TODO: ugly
    vkUnmapMemory(device.getLogicalDevice(), applVertexBufferMemory);

    vec3* normals = (vec3*)uncompressData((unsigned char*)applMesh->_normalData, applMesh->compressedNormalDataLength, [this](uint64_t buffersize) {
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
        vkGetBufferMemoryRequirements(device.getLogicalDevice(), applNormalBuffer,
            &memRequirements);

        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = memRequirements.size;
        allocInfo.memoryTypeIndex = device.findMemoryType(
            memRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

        if (vkAllocateMemory(device.getLogicalDevice(), &allocInfo, nullptr,
            &applNormalBufferMemory) != VK_SUCCESS) {
            throw std::runtime_error("failed to allocate vertex buffer memory!");
        }
        vkBindBufferMemory(device.getLogicalDevice(), applNormalBuffer,
            applNormalBufferMemory, 0);
        void* data;
        vkMapMemory(device.getLogicalDevice(), applNormalBufferMemory, 0, bufferInfo.size,
            0, &data);
        return data;
        });
    vkUnmapMemory(device.getLogicalDevice(), applNormalBufferMemory);


    vec3* tangents = (vec3*)uncompressData((unsigned char*)applMesh->_tangentData, applMesh->compressedTangentDataLength, [this](uint64_t buffersize) {
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
        vkGetBufferMemoryRequirements(device.getLogicalDevice(), applTangentBuffer,
            &memRequirements);

        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = memRequirements.size;
        allocInfo.memoryTypeIndex = device.findMemoryType(
            memRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

        if (vkAllocateMemory(device.getLogicalDevice(), &allocInfo, nullptr,
            &applTangentBufferMemory) != VK_SUCCESS) {
            throw std::runtime_error("failed to allocate vertex buffer memory!");
        }
        vkBindBufferMemory(device.getLogicalDevice(), applTangentBuffer,
            applTangentBufferMemory, 0);
        void* data;
        vkMapMemory(device.getLogicalDevice(), applTangentBufferMemory, 0, bufferInfo.size,
            0, &data);
        return data;
        });
    vkUnmapMemory(device.getLogicalDevice(), applTangentBufferMemory);

    vec2* uvs = (vec2*)uncompressData((unsigned char*)applMesh->_uvData, applMesh->compressedUvDataLength, [this](uint64_t buffersize) {
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
        allocInfo.memoryTypeIndex = findMemoryType(
            memRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

        if (vkAllocateMemory(device.getLogicalDevice(), &allocInfo, nullptr,
            &applUVBufferMemory) != VK_SUCCESS) {
            throw std::runtime_error("failed to allocate vertex buffer memory!");
        }
        vkBindBufferMemory(device.getLogicalDevice(), applUVBuffer,
            applUVBufferMemory, 0);
        void* data;
        vkMapMemory(device.getLogicalDevice(), applUVBufferMemory, 0, bufferInfo.size,
            0, &data);
        return data;
        });
    vkUnmapMemory(device.getLogicalDevice(), applUVBufferMemory);



    uint32_t* indices = (uint32_t*)uncompressData((unsigned char*)applMesh->_indexData, applMesh->compressedIndexDataLength, [this](uint64_t buffersize) {
        VkBufferCreateInfo bufferInfo{};
        bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.size = buffersize;
        bufferInfo.usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        bufferInfo.flags = 0;
        if (vkCreateBuffer(device.getLogicalDevice(), &bufferInfo, nullptr, &applIndexBuffer) !=
            VK_SUCCESS) {
            throw std::runtime_error("failed to create index buffer!");
        }
        VkMemoryRequirements memRequirements;
        vkGetBufferMemoryRequirements(device.getLogicalDevice(), applIndexBuffer, &memRequirements);

        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = memRequirements.size;
        allocInfo.memoryTypeIndex = findMemoryType(
            memRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

        if (vkAllocateMemory(device.getLogicalDevice(), &allocInfo, nullptr, &applIndexMemory) !=
            VK_SUCCESS) {
            throw std::runtime_error("failed to allocate vertex buffer memory!");
        }
        vkBindBufferMemory(device.getLogicalDevice(), applIndexBuffer, applIndexMemory, 0);
        void* data;
        vkMapMemory(device.getLogicalDevice(), applIndexMemory, 0, bufferInfo.size, 0, &data);

        return data;
        });
    vkUnmapMemory(device.getLogicalDevice(), applIndexMemory);



    m_Chunks = (AAPLMeshChunk*)uncompressData((unsigned char*)applMesh->_chunkData, applMesh->compressedChunkDataLength, applMesh->_chunkCount * sizeof(AAPLMeshChunk));

    {
        VkBufferCreateInfo meshChunkBufferCreateInfo{};
        meshChunkBufferCreateInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        meshChunkBufferCreateInfo.size = applMesh->_chunkCount * sizeof(AAPLMeshChunk);
        meshChunkBufferCreateInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        meshChunkBufferCreateInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        meshChunkBufferCreateInfo.flags = 0;

        if (vkCreateBuffer(device.getLogicalDevice(), &meshChunkBufferCreateInfo, nullptr, &meshChunksBuffer) != VK_SUCCESS)
        {
            throw std::runtime_error("failed to allocate mesh chunk buffer");
        }

        VkMemoryRequirements memRequirements;
        vkGetBufferMemoryRequirements(device.getLogicalDevice(), meshChunksBuffer, &memRequirements);

        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = memRequirements.size;
        allocInfo.memoryTypeIndex = findMemoryType(
            memRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

        if (vkAllocateMemory(device.getLogicalDevice(), &allocInfo, nullptr, &meshChunksBufferMemory) !=
            VK_SUCCESS) {
            throw std::runtime_error("failed to allocate mesh chunks buffer memory!");
        }
        vkBindBufferMemory(device.getLogicalDevice(), meshChunksBuffer, meshChunksBufferMemory, 0);
        void* data;
        vkMapMemory(device.getLogicalDevice(), meshChunksBufferMemory, 0, meshChunkBufferCreateInfo.size, 0, &data);
        std::memcpy(data, (void*)m_Chunks, meshChunkBufferCreateInfo.size);
        vkUnmapMemory(device.getLogicalDevice(), meshChunksBufferMemory);

       // free(m_Chunks);

    }

    {
        VkBufferCreateInfo drawParamsBufferInfo{};
        drawParamsBufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        drawParamsBufferInfo.size = sizeof(VkDrawIndexedIndirectCommand) * applMesh->_chunkCount;
        drawParamsBufferInfo.usage = VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        drawParamsBufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        drawParamsBufferInfo.flags = 0;
        if (vkCreateBuffer(device.getLogicalDevice(), &drawParamsBufferInfo, nullptr,
            &drawParamsBuffer) != VK_SUCCESS) {
            throw std::runtime_error("failed to create uniform buffer!");
        }
        VkMemoryRequirements memRequirements;
        vkGetBufferMemoryRequirements(device.getLogicalDevice(), drawParamsBuffer,
            &memRequirements);

        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = memRequirements.size;
        allocInfo.memoryTypeIndex =
            findMemoryType(memRequirements.memoryTypeBits,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

        if (vkAllocateMemory(device.getLogicalDevice(), &allocInfo, nullptr,
            &drawParamsBufferMemory) != VK_SUCCESS) {
            throw std::runtime_error("failed to allocate drawparams buffer memory!");
        }
        vkBindBufferMemory(device.getLogicalDevice(), drawParamsBuffer,
            drawParamsBufferMemory, 0);

    }

    {
        VkBufferCreateInfo cullParamsBufferInfo{};
        cullParamsBufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        cullParamsBufferInfo.size = sizeof(gpuCullParams);
        cullParamsBufferInfo.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
        cullParamsBufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        cullParamsBufferInfo.flags = 0;
        if (vkCreateBuffer(device.getLogicalDevice(), &cullParamsBufferInfo, nullptr,
            &cullParamsBuffer) != VK_SUCCESS) {
            throw std::runtime_error("failed to create uniform buffer!");
        }
        VkMemoryRequirements memRequirements;
        vkGetBufferMemoryRequirements(device.getLogicalDevice(), cullParamsBuffer,
            &memRequirements);

        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = memRequirements.size;
        allocInfo.memoryTypeIndex =
            findMemoryType(memRequirements.memoryTypeBits,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

        if (vkAllocateMemory(device.getLogicalDevice(), &allocInfo, nullptr,
            &cullParamsBufferMemory) != VK_SUCCESS) {
            throw std::runtime_error("failed to allocate uniform buffer memory!");
        }
        vkBindBufferMemory(device.getLogicalDevice(), cullParamsBuffer,
            cullParamsBufferMemory, 0);

    }

    {
        VkBufferCreateInfo writeIndexBufferInfo{};
        writeIndexBufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        writeIndexBufferInfo.size = sizeof(uint32_t);
        writeIndexBufferInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
        writeIndexBufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        writeIndexBufferInfo.flags = 0;
        if (vkCreateBuffer(device.getLogicalDevice(), &writeIndexBufferInfo, nullptr,
            &writeIndexBuffer) != VK_SUCCESS) {
            throw std::runtime_error("failed to create writeindex buffer!");
        }
        VkMemoryRequirements memRequirements;
        vkGetBufferMemoryRequirements(device.getLogicalDevice(), writeIndexBuffer,
            &memRequirements);

        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = memRequirements.size;
        allocInfo.memoryTypeIndex =
            findMemoryType(memRequirements.memoryTypeBits,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

        if (vkAllocateMemory(device.getLogicalDevice(), &allocInfo, nullptr,
            &writeIndexBufferMemory) != VK_SUCCESS) {
            throw std::runtime_error("failed to allocate uniform buffer memory!");
        }
        vkBindBufferMemory(device.getLogicalDevice(), writeIndexBuffer,
            writeIndexBufferMemory, 0);


    }

    {

        VkBufferCreateInfo chunkIndicesBufferInfo{};
        chunkIndicesBufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        chunkIndicesBufferInfo.size = sizeof(uint32_t) * applMesh->_chunkCount;
        chunkIndicesBufferInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        chunkIndicesBufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        chunkIndicesBufferInfo.flags = 0;
        if (vkCreateBuffer(device.getLogicalDevice(), &chunkIndicesBufferInfo, nullptr,
            &chunkIndicesBuffer) != VK_SUCCESS) {
            throw std::runtime_error("failed to create writeindex buffer!");
        }
        VkMemoryRequirements memRequirements;
        vkGetBufferMemoryRequirements(device.getLogicalDevice(), chunkIndicesBuffer,
            &memRequirements);

        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = memRequirements.size;
        allocInfo.memoryTypeIndex =
            findMemoryType(memRequirements.memoryTypeBits,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

        if (vkAllocateMemory(device.getLogicalDevice(), &allocInfo, nullptr,
            &chunkIndicesBufferMemory) != VK_SUCCESS) {
            throw std::runtime_error("failed to allocate uniform buffer memory!");
        }
        vkBindBufferMemory(device.getLogicalDevice(), chunkIndicesBuffer,
            chunkIndicesBufferMemory, 0);


    }


    {

        VkBufferCreateInfo applInstanceBufferInfo{};
        applInstanceBufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        applInstanceBufferInfo.size = sizeof(uint32_t) * applMesh->_chunkCount;
        applInstanceBufferInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
        applInstanceBufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        applInstanceBufferInfo.flags = 0;
        if (vkCreateBuffer(device.getLogicalDevice(), &applInstanceBufferInfo, nullptr,
            &applInstanceBuffer) != VK_SUCCESS) {
            throw std::runtime_error("failed to create writeindex buffer!");
        }
        VkMemoryRequirements memRequirements;
        vkGetBufferMemoryRequirements(device.getLogicalDevice(), applInstanceBuffer,
            &memRequirements);

        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = memRequirements.size;
        allocInfo.memoryTypeIndex =
            findMemoryType(memRequirements.memoryTypeBits,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

        if (vkAllocateMemory(device.getLogicalDevice(), &allocInfo, nullptr,
            &applInstanceBufferMemory) != VK_SUCCESS) {
            throw std::runtime_error("failed to allocate uniform buffer memory!");
        }
        vkBindBufferMemory(device.getLogicalDevice(), applInstanceBuffer,
            applInstanceBufferMemory, 0);


    }

    AAPLSubMesh* submeshes = (AAPLSubMesh*)uncompressData((unsigned char*)applMesh->_meshData, applMesh->compressedMeshDataLength, applMesh->_meshCount * sizeof(AAPLSubMesh));

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

    //auto textureRes = createTexture(applMesh->_textures[13]);
    //auto textureRes = createTexture("G:\\AdvancedVulkanRendering\\textures\\texture.jpg");
    //currentImage = textureRes.first;
    init_descriptorsV2();
    //init_descriptors(textureRes.second);
    //init_descriptors(textures[13].second);
    init_appl_descriptors();
    init_drawparams_descriptors();
    init_deferredlighting_descriptors();
    createGraphicsPipeline(deviceref.getMainRenderPass());
    createRenderOccludersPipeline(occluderZPass);
    createComputePipeline();
}

void GpuScene::CreateForwardLightingPass()
{

	VkAttachmentDescription deferredLightingAttachments = {};

    deferredLightingAttachments.format = device.getSwapChainImageFormat();
    deferredLightingAttachments.samples = VK_SAMPLE_COUNT_1_BIT;
    deferredLightingAttachments.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    deferredLightingAttachments.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    deferredLightingAttachments.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    deferredLightingAttachments.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    deferredLightingAttachments.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    deferredLightingAttachments.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

	
    VkAttachmentDescription deferredLightingDepthAttachments = {};

    deferredLightingDepthAttachments.format = device.getWindowDepthFormat();
    deferredLightingDepthAttachments.samples = VK_SAMPLE_COUNT_1_BIT;
    deferredLightingDepthAttachments.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    deferredLightingDepthAttachments.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    deferredLightingDepthAttachments.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    deferredLightingDepthAttachments.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    deferredLightingDepthAttachments.initialLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    deferredLightingDepthAttachments.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;


    VkAttachmentReference deferredLightingAttachmentRefs{};
    deferredLightingAttachmentRefs.attachment = 0;
    deferredLightingAttachmentRefs.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkAttachmentReference deferredLightingDepthAttachmentRef{};
    deferredLightingDepthAttachmentRef.attachment = 1;
    deferredLightingDepthAttachmentRef.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;


    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &deferredLightingAttachmentRefs;
    subpass.pDepthStencilAttachment = &deferredLightingDepthAttachmentRef; 

    VkSubpassDependency dependency{};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcAccessMask = 0;
    dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

    std::array<VkAttachmentDescription, 2> attachments = { deferredLightingAttachments , deferredLightingDepthAttachments};
    VkRenderPassCreateInfo renderPassInfo{};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    renderPassInfo.attachmentCount = static_cast<uint32_t>(attachments.size());
    renderPassInfo.pAttachments = attachments.data();
    renderPassInfo.subpassCount = 1;
    renderPassInfo.pSubpasses = &subpass;
    renderPassInfo.dependencyCount = 1;
    renderPassInfo.pDependencies = &dependency;

    if (vkCreateRenderPass(device.getLogicalDevice(), &renderPassInfo, nullptr, &_forwardLightingPass) != VK_SUCCESS) {
        throw std::runtime_error("failed to create base pass!");
    }

}

void GpuScene::CreateDeferredLightingPass()
{
    VkAttachmentDescription deferredLightingAttachments = {};

    deferredLightingAttachments.format = device.getSwapChainImageFormat();
    deferredLightingAttachments.samples = VK_SAMPLE_COUNT_1_BIT;
    deferredLightingAttachments.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    deferredLightingAttachments.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    deferredLightingAttachments.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    deferredLightingAttachments.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    deferredLightingAttachments.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    deferredLightingAttachments.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

	
    
    VkAttachmentReference deferredLightingAttachmentRefs{};
    deferredLightingAttachmentRefs.attachment = 0;
    deferredLightingAttachmentRefs.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

   
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &deferredLightingAttachmentRefs;

    VkSubpassDependency dependency{};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcAccessMask = 0;
    dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

    std::array<VkAttachmentDescription, 1> attachments = { deferredLightingAttachments};
    VkRenderPassCreateInfo renderPassInfo{};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    renderPassInfo.attachmentCount = static_cast<uint32_t>(attachments.size());
    renderPassInfo.pAttachments = attachments.data();
    renderPassInfo.subpassCount = 1;
    renderPassInfo.pSubpasses = &subpass;
    renderPassInfo.dependencyCount = 1;
    renderPassInfo.pDependencies = &dependency;

    if (vkCreateRenderPass(device.getLogicalDevice(), &renderPassInfo, nullptr, &_deferredLightingPass) != VK_SUCCESS) {
        throw std::runtime_error("failed to create base pass!");
    }
}

void GpuScene::CreateDeferredBasePass()
{
    VkAttachmentDescription gbufferAttachments[4] = {};
    for (int i = 0; i < 4; i++)
    {
        gbufferAttachments[i] = {};
        gbufferAttachments[i].format = _gbufferFormat[i];
        gbufferAttachments[i].samples = VK_SAMPLE_COUNT_1_BIT;
        gbufferAttachments[i].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        gbufferAttachments[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        gbufferAttachments[i].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        gbufferAttachments[i].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;


    }

    VkAttachmentDescription depthAttachment{};
    depthAttachment.format = device.getWindowDepthFormat();
    depthAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
    depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    depthAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depthAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depthAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    depthAttachment.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkAttachmentReference gbufferAttachmentRefs[4] = {};
    for (int i = 0; i < 4; i++)
    {
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

    VkSubpassDependency dependency{};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcAccessMask = 0;
    dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

    std::array<VkAttachmentDescription, 5> attachments = { gbufferAttachments[0],gbufferAttachments[1],gbufferAttachments[2],gbufferAttachments[3], depthAttachment };
    VkRenderPassCreateInfo renderPassInfo{};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    renderPassInfo.attachmentCount = static_cast<uint32_t>(attachments.size());
    renderPassInfo.pAttachments = attachments.data();
    renderPassInfo.subpassCount = 1;
    renderPassInfo.pSubpasses = &subpass;
    renderPassInfo.dependencyCount = 1;
    renderPassInfo.pDependencies = &dependency;

    if (vkCreateRenderPass(device.getLogicalDevice(), &renderPassInfo, nullptr, &_basePass) != VK_SUCCESS) {
        throw std::runtime_error("failed to create base pass!");
    }

}

void GpuScene::CreateOccluderZPass()
{
    //no color attachment only depth attachment
    VkAttachmentDescription depthAttachment{};
    depthAttachment.format = _depthFormat;//TODO: should use the rt format?
    depthAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
    depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depthAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depthAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depthAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    depthAttachment.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkAttachmentReference depthAttachmentRef{};
    depthAttachmentRef.attachment = 0;
    depthAttachmentRef.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 0;
    subpass.pColorAttachments = nullptr;
    subpass.pDepthStencilAttachment = &depthAttachmentRef;

    VkSubpassDependency dependency{};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcAccessMask = 0;
    dependency.srcStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;//TODO: is dependency mask right?
    dependency.dstStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dependency.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

    std::array<VkAttachmentDescription, 1> attachments = { depthAttachment };
    VkRenderPassCreateInfo renderPassInfo{};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    renderPassInfo.attachmentCount = static_cast<uint32_t>(attachments.size());
    renderPassInfo.pAttachments = attachments.data();
    renderPassInfo.subpassCount = 1;
    renderPassInfo.pSubpasses = &subpass;
    renderPassInfo.dependencyCount = 1;
    renderPassInfo.pDependencies = &dependency;

    if (vkCreateRenderPass(device.getLogicalDevice(), &renderPassInfo, nullptr, &occluderZPass) != VK_SUCCESS) {
        throw std::runtime_error("failed to create render pass!");
    }

}

void GpuScene::CreateForwardLightingFrameBuffer(uint32_t count) {
	_forwardFrameBuffer.resize(count);
    for (int i = 0; i < count; i++)
    {
        std::array<VkImageView, 2> attachments = {
            device.getSwapChainImageView(i),
	    device.getWindowDepthImageView()
        };

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
            throw std::runtime_error("failed to create deferred lighting framebuffer!");
        }
    }
}



void GpuScene::CreateDeferredLightingFrameBuffer(uint32_t count) {
	_deferredFrameBuffer.resize(count);
    for (int i = 0; i < count; i++)
    {
        std::array<VkImageView, 1> attachments = {
            device.getSwapChainImageView(i),
        };

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
            throw std::runtime_error("failed to create deferred lighting framebuffer!");
        }
    }
}

void GpuScene::CreateBasePassFrameBuffer() {


    std::array<VkImageView, 5> attachments = {
        _gbuffersView[0],_gbuffersView[1],_gbuffersView[2],_gbuffersView[3],
               device.getWindowDepthImageView()
    };

    VkFramebufferCreateInfo framebufferInfo{};
    framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    framebufferInfo.renderPass = _basePass;
    framebufferInfo.attachmentCount = static_cast<uint32_t>(attachments.size());
    ;
    framebufferInfo.pAttachments = attachments.data();
    framebufferInfo.width = device.getSwapChainExtent().width;
    framebufferInfo.height = device.getSwapChainExtent().height;
    framebufferInfo.layers = 1;

    if (vkCreateFramebuffer(device.getLogicalDevice(), &framebufferInfo,
        nullptr, &_basePassFrameBuffer) != VK_SUCCESS) {
        throw std::runtime_error("failed to create basepass framebuffer!");
    }
}

void GpuScene::CreateOccluderZPassFrameBuffer()
{
    std::array<VkImageView, 1> attachments = {
               _depthTextureView
    };

    VkFramebufferCreateInfo framebufferInfo{};
    framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    framebufferInfo.renderPass = occluderZPass;
    framebufferInfo.attachmentCount = static_cast<uint32_t>(attachments.size());;
    framebufferInfo.pAttachments = attachments.data();
    framebufferInfo.width = device.getSwapChainExtent().width;
    framebufferInfo.height = device.getSwapChainExtent().height;
    framebufferInfo.layers = 1;

    if (vkCreateFramebuffer(device.getLogicalDevice(), &framebufferInfo, nullptr, &_depthFrameBuffer) != VK_SUCCESS) {
        throw std::runtime_error("failed to create z framebuffer!");
    }
}

void GpuScene::CreateZdepthView()
{
    VkImageViewCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    createInfo.image = _depthTexture;
    createInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    createInfo.format = _depthFormat;
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


void GpuScene::DrawOccluders()
{
    VkRenderPassBeginInfo renderPassInfo{};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    renderPassInfo.renderPass = occluderZPass;
    renderPassInfo.framebuffer = _depthFrameBuffer;
    renderPassInfo.renderArea.offset = { 0, 0 };
    renderPassInfo.renderArea.extent = device.getSwapChainExtent();

    std::array<VkClearValue, 1> clearValues{};

    clearValues[0].depthStencil = { 0.0f, 0 };
    renderPassInfo.clearValueCount = static_cast<uint32_t>(clearValues.size());
    renderPassInfo.pClearValues = clearValues.data();

    vkCmdBeginRenderPass(commandBuffer, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);//TODO: seperate commandbuffer?

    VkDeviceSize offsets[] = { 0 };
    VkBuffer vertexBuffers[] = { _occludersVertBuffer };
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, drawOccluderPipeline);
    vkCmdBindVertexBuffers(commandBuffer, 0, 1, vertexBuffers, offsets);
    vkCmdBindIndexBuffer(commandBuffer, _occludersIndexBuffer, 0, VK_INDEX_TYPE_UINT32);

    //mat4 objtocamera = transpose(maincamera->getObjectToCamera());

    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout, 0, 1, &globalDescriptor, 0, nullptr);

    //vkCmdPushConstants(commandBuffer,pipelineLayout,VK_SHADER_STAGE_VERTEX_BIT,0,sizeof(mat4),objtocamera.value_ptr());
    vkCmdDrawIndexed(commandBuffer, sceneFile["occluder_indices"].size(), 1, 0, 0, 0);
    vkCmdEndRenderPass(commandBuffer);
}

void GpuScene::recordCommandBuffer(int imageIndex) {

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;

    if (vkBeginCommandBuffer(commandBuffer, &beginInfo) != VK_SUCCESS) {
        throw std::runtime_error("failed to begin recording command buffer!");
    }


#ifndef USE_CPU_ENCODE_DRAWPARAM

    uint32_t groupx = (applMesh->_opaqueChunkCount + 127) / 128;
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, encodeDrawBufferPipeline);
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, encodeDrawBufferPipelineLayout, 0, 1, &gpuCullDescriptorSet, 0, nullptr);
    vkCmdDispatch(commandBuffer, groupx, 1, 1);

    VkMemoryBarrier2 memoryBarrier = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
        .pNext = nullptr,
        .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT_KHR,
        .srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT_KHR,
        .dstStageMask = VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT_KHR,
        .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT_KHR
    };

    VkDependencyInfo dependencyInfo = {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .pNext = nullptr,
        .memoryBarrierCount = 1,
        .pMemoryBarriers = &memoryBarrier,
    };
#endif
    DrawOccluders();


#ifndef USE_CPU_ENCODE_DRAWPARAM
    vkCmdPipelineBarrier2(commandBuffer, &dependencyInfo);
#endif
    

    {
        VkRenderPassBeginInfo renderPassInfo{};
        renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        renderPassInfo.renderPass = _basePass;//device.getMainRenderPass();
        renderPassInfo.framebuffer = _basePassFrameBuffer;//device.getSwapChainFrameBuffer(imageIndex);
        renderPassInfo.renderArea.offset = { 0, 0 };
        renderPassInfo.renderArea.extent = device.getSwapChainExtent();

        std::array<VkClearValue, 5> clearValues{};
        clearValues[0].color = { {0.0f, 0.0f, 0.0f, 1.0f} };
        clearValues[1].color = { {0.0f, 0.0f, 0.0f, 1.0f} };
        clearValues[2].color = { {0.0f, 0.0f, 0.0f, 1.0f} };
        clearValues[3].color = { {0.0f, 0.0f, 0.0f, 1.0f} };
        clearValues[4].depthStencil = { 0.0f, 0 };
        renderPassInfo.clearValueCount = static_cast<uint32_t>(clearValues.size());
        renderPassInfo.pClearValues = clearValues.data();

        vkCmdBeginRenderPass(commandBuffer, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);

        //vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, graphicsPipeline);
        //vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout, 0, 1, &globalDescriptor, 0, nullptr);
        //vkCmdDraw(commandBuffer, 6, 1, 0, 0);

        DrawChunksBasePass();

        vkCmdEndRenderPass(commandBuffer);
    }

    //vkCmdBindPipeline(commandBuffer,VK_PIPELINE_BIND_POINT_GRAPHICS, egraphicsPipeline);
    //VkBuffer vertexBuffers[] = {vertexBuffer};
    //VkDeviceSize offsets[] = {0};

    //vkCmdBindVertexBuffers(commandBuffer, 0, 1, vertexBuffers, offsets);
    //vkCmdBindIndexBuffer(commandBuffer,indexBuffer,0,VK_INDEX_TYPE_UINT16);
    //if the descriptor set data isn't change we can omit this?
    //vkCmdBindDescriptorSets(commandBuffer,VK_PIPELINE_BIND_POINT_GRAPHICS,epipelineLayout,0,1,&globalDescriptor,0,nullptr);
    //if the constant isn't changed we can omit this?
    //mat4 scaleM = scale(modelScale);
    //mat4 withScale = transpose(maincamera->getObjectToCamera()) * scaleM;
    //vkCmdPushConstants(commandBuffer,epipelineLayout,VK_SHADER_STAGE_VERTEX_BIT,0,sizeof(mat4),withScale.value_ptr());
    //vkCmdDrawIndexed(commandBuffer,getIndexSize()/sizeof(unsigned short),1,0,0,0);


    {
        //TODO: test with subpass dependency
        //transition the image layout
        //notice:换成device的transitionImageLayout会报错,validation layer 关于barrier只在同一个commandbuffer中记录imageview的layout，跨commandbuffer会报错~
        transitionImageLayout(device.getWindowDepthImage(), device.getWindowDepthFormat(), VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        for (int i = 0; i < 4; i++)
            transitionImageLayout(_gbuffers[i], _gbufferFormat[i], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }

    //deferred lighting pass
    {
        std::array<VkClearValue, 1> clearValues{};
        clearValues[0].color = { {0.0f, 0.0f, 0.0f, 1.0f} };
        
        VkRenderPassBeginInfo blitPassInfo{};
        blitPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        blitPassInfo.renderPass = _deferredLightingPass;
        blitPassInfo.framebuffer = _deferredFrameBuffer[imageIndex];
        blitPassInfo.renderArea.offset = { 0, 0 };
        blitPassInfo.renderArea.extent = device.getSwapChainExtent();
        blitPassInfo.clearValueCount = static_cast<uint32_t>(clearValues.size());
        blitPassInfo.pClearValues = clearValues.data();


        vkCmdBeginRenderPass(commandBuffer, &blitPassInfo, VK_SUBPASS_CONTENTS_INLINE);

        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
            deferredLightingPipeline);
        
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
            deferredLightingPipelineLayout, 0, 1, &deferredLightingDescriptorSet,
            0, nullptr);
        vkCmdDraw(commandBuffer, 3, 1, 0, 0);

        vkCmdEndRenderPass(commandBuffer);


        //forward pass
        {
		
	VkRenderPassBeginInfo forwardPassInfo{};
        forwardPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        forwardPassInfo.renderPass = _forwardLightingPass;
        forwardPassInfo.framebuffer = _forwardFrameBuffer[imageIndex];
        forwardPassInfo.renderArea.offset = { 0, 0 };
        forwardPassInfo.renderArea.extent = device.getSwapChainExtent();
        forwardPassInfo.clearValueCount = 0;//static_cast<uint32_t>(clearValues.size());
        //forwardPassInfo.pClearValues = clearValues.data();

	vkCmdBeginRenderPass(commandBuffer,&forwardPassInfo,VK_SUBPASS_CONTENTS_INLINE);
            vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                drawclusterForwardPipeline);

            vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, drawclusterPipelineLayout, 0, 1, &applDescriptorSet, 0, nullptr);

            if (applMesh->_opaqueChunkCount + applMesh->_alphaMaskedChunkCount + applMesh->_transparentChunkCount != applMesh->_chunkCount)
                spdlog::error("chunk count error {} {}", applMesh->_opaqueChunkCount + applMesh->_alphaMaskedChunkCount + applMesh->_transparentChunkCount, applMesh->_chunkCount);

            for (int i = applMesh->_opaqueChunkCount + applMesh->_alphaMaskedChunkCount; i < applMesh->_chunkCount ; ++i)
            {
                PerObjPush perobj = { .matindex = m_Chunks[i].materialIndex };


                {
                    if (maincamera->getFrustum().FrustumCull(m_Chunks[i].boundingBox))
                    {
                        //debug_frustum_cull[i] = true;
                        continue;
                    }
                }

                vkCmdPushConstants(commandBuffer, drawclusterPipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(perobj), &perobj);
                vkCmdDrawIndexed(commandBuffer, m_Chunks[i].indexCount, 1, m_Chunks[i].indexBegin, 0, 0);
            }

	vkCmdEndRenderPass(commandBuffer);
        }
    }

   


    //post process

   

    if (vkEndCommandBuffer(commandBuffer) != VK_SUCCESS) {
        throw std::runtime_error("failed to record command buffer!");
    }

}




void GpuScene::DrawChunk(const AAPLMeshChunk& chunk)
{
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, drawclusterPipeline);
    VkBuffer vertexBuffers[] = { applVertexBuffer , applNormalBuffer, applTangentBuffer, applUVBuffer };
    VkDeviceSize offsets[] = { 0,0,0,0 };

    vkCmdBindVertexBuffers(commandBuffer, 0, sizeof(vertexBuffers) / sizeof(vertexBuffers[0]), vertexBuffers, offsets);
    vkCmdBindIndexBuffer(commandBuffer, applIndexBuffer, 0, VK_INDEX_TYPE_UINT32);

    //if the descriptor set data isn't change we can omit this?
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, drawclusterPipelineLayout, 0, 1, &applDescriptorSet, 0, nullptr);
    //if the constant isn't changed we can omit this?
    //mat4 scaleM = scale(modelScale);
    //mat4 withScale = transpose(maincamera->getObjectToCamera()) * scaleM;
    PerObjPush perobj = { .matindex = chunk.materialIndex };
    vkCmdPushConstants(commandBuffer, epipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(perobj), &perobj);
    vkCmdDrawIndexed(commandBuffer, chunk.indexCount, 1, chunk.indexBegin, 0, 0);
}

void GpuScene::DrawChunksBasePass() {
   
    VkBuffer vertexBuffers[] = { applVertexBuffer, applNormalBuffer,
                                applTangentBuffer, applUVBuffer,
                                applInstanceBuffer };
    VkDeviceSize offsets[] = { 0, 0, 0, 0, 0 };

    vkCmdBindVertexBuffers(commandBuffer, 0,
        sizeof(vertexBuffers) / sizeof(vertexBuffers[0]),
        vertexBuffers, offsets);
    vkCmdBindIndexBuffer(commandBuffer, applIndexBuffer, 0,
        VK_INDEX_TYPE_UINT32);

    
#ifndef USE_CPU_ENCODE_DRAWPARAM
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
        drawclusterBasePipeline);
    // if the descriptor set data isn't change we can omit this?
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
        drawclusterBasePipelineLayout, 0, 1, &applDescriptorSet,
        0, nullptr);
    vkCmdDrawIndexedIndirectCount(commandBuffer, drawParamsBuffer, 0,
        writeIndexBuffer, 0, applMesh->_chunkCount,
        sizeof(VkDrawIndexedIndirectCommand));
#else
    constexpr int beginindex = 0;
    constexpr int indexClamp = 0xffffff;
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, drawclusterPipelineLayout, 0, 1, &applDescriptorSet, 0, nullptr);


    vkCmdBindPipeline(commandBuffer,VK_PIPELINE_BIND_POINT_GRAPHICS, drawclusterPipeline);
    for (int i=0;i<applMesh->_opaqueChunkCount;++i)
    {
	PerObjPush perobj = { .matindex = m_Chunks[i].materialIndex };
        
        
        {
            if (maincamera->getFrustum().FrustumCull(m_Chunks[i].boundingBox))
            {
                //debug_frustum_cull[i] = true;
                continue;
            }
        }

        vkCmdPushConstants(commandBuffer, drawclusterPipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(perobj), &perobj);
        vkCmdDrawIndexed(commandBuffer, m_Chunks[i].indexCount, 1, m_Chunks[i].indexBegin, 0, 0);

    }
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, drawclusterPipelineAlphaMask);
    for (int i = applMesh->_opaqueChunkCount; i < applMesh->_opaqueChunkCount+applMesh->_alphaMaskedChunkCount && i < indexClamp; ++i)
    {
        PerObjPush perobj = { .matindex = m_Chunks[i].materialIndex };
        
        
        {
            if (maincamera->getFrustum().FrustumCull(m_Chunks[i].boundingBox))
            {
                //debug_frustum_cull[i] = true;
                continue;
            }
        }

        vkCmdPushConstants(commandBuffer, drawclusterPipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(perobj), &perobj);
        vkCmdDrawIndexed(commandBuffer, m_Chunks[i].indexCount, 1, m_Chunks[i].indexBegin, 0, 0);
    }
#endif
}

void GpuScene::DrawChunks()
{
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, drawclusterPipelineAlphaMask);
    VkBuffer vertexBuffers[] = { applVertexBuffer , applNormalBuffer, applTangentBuffer, applUVBuffer, applInstanceBuffer };
    VkDeviceSize offsets[] = { 0,0,0,0,0 };

    vkCmdBindVertexBuffers(commandBuffer, 0, sizeof(vertexBuffers) / sizeof(vertexBuffers[0]), vertexBuffers, offsets);
    vkCmdBindIndexBuffer(commandBuffer, applIndexBuffer, 0, VK_INDEX_TYPE_UINT32);

    //if the descriptor set data isn't change we can omit this?
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, drawclusterPipelineLayout, 0, 1, &applDescriptorSet, 0, nullptr);
    constexpr int beginindex = 0;
    constexpr int indexClamp = 0xffffff;
    uint32_t occluded = 0;

    //static std::vector<bool> debug_frustum_cull(applMesh->_chunkCount,false);
    //static bool captured = false;
#ifdef CPU_DRAW 
    for (int i = beginindex; i < applMesh->_chunkCount && i < indexClamp; ++i)
    {
        PerObjPush perobj = { .matindex = m_Chunks[i].materialIndex };
        vkCmdPushConstants(commandBuffer, drawclusterPipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(perobj), &perobj);
        //if (captured)
        //{
        //    if (debug_frustum_cull[i])
        //        continue;
        //}
        //else
        {
            if (maincamera->getFrustum().FrustumCull(m_Chunks[i].boundingBox))
            {
                //debug_frustum_cull[i] = true;
                ++occluded;
                continue;
            }
        }


        vkCmdDrawIndexed(commandBuffer, m_Chunks[i].indexCount, 1, m_Chunks[i].indexBegin, 0, 0);
    }
    //captured = true;
    spdlog::log(spdlog::level::info, "occlued chunks: {}", occluded);
#else
    vkCmdDrawIndexedIndirectCount(commandBuffer, drawParamsBuffer, 0, writeIndexBuffer, 0, applMesh->_chunkCount, sizeof(VkDrawIndexedIndirectCommand));
#endif
}

void GpuScene::Draw() {
    // begin command buffer record
    // bind graphics pipeline
    // update uniform buffer
    // draw mesh
    // submit commandbuffer

    vkWaitForFences(device.getLogicalDevice(), 1, &inFlightFence, VK_TRUE, UINT64_MAX);
    vkResetFences(device.getLogicalDevice(), 1, &inFlightFence);

    //updateSamplerInDescriptors();

    //dispatch the gpu cull threadgroups



    uint32_t imageIndex;
    vkAcquireNextImageKHR(device.getLogicalDevice(), device.getSwapChain(), UINT64_MAX, imageAvailableSemaphore,
        VK_NULL_HANDLE, &imageIndex);

    void* data1;
    vkMapMemory(device.getLogicalDevice(), uniformBufferMemory, 0, sizeof(FrameData), 0,
        &data1);
    memcpy(data1, &frameConstants, sizeof(FrameConstants));
    void* data = ((char*)data1) + sizeof(FrameConstants);
    memcpy(data, transpose(maincamera->getProjectMatrix()).value_ptr(),
        (size_t)sizeof(mat4));
    memcpy(((mat4*)data) + 1, transpose(maincamera->getObjectToCamera()).value_ptr(), (size_t)sizeof(mat4));

    memcpy(((mat4*)data) + 2, transpose(maincamera->getInvViewMatrix()).value_ptr(), (size_t)sizeof(mat4));

    memcpy(((mat4*)data) + 3, transpose(maincamera->getInvViewProjectionMatrix()).value_ptr(), (size_t)sizeof(mat4));
    vkUnmapMemory(device.getLogicalDevice(), uniformBufferMemory);

  //spdlog::info("{} {}", sizeof(gpuCullParams), offsetof(gpuCullParams, frustum));
  vkMapMemory(device.getLogicalDevice(), cullParamsBufferMemory, 0, sizeof(gpuCullParams), 0, &data);
  memcpy(data, &applMesh->_opaqueChunkCount, sizeof(uint32_t));
  //memcpy((char*)data+offsetof(gpuCullParams,frustum), &maincamera->getFrustum(), sizeof(Frustum));
  //offsetof isn't working as expected
  memcpy((char*)data + 16, &maincamera->getFrustum(), sizeof(Frustum));

  vkUnmapMemory(device.getLogicalDevice(), cullParamsBufferMemory);

  uint32_t startIndex = 0;
  vkMapMemory(device.getLogicalDevice(), writeIndexBufferMemory, 0, sizeof(uint32_t), 0, &data);
  memcpy((uint32_t*)data, &startIndex, sizeof(uint32_t));
  vkUnmapMemory(device.getLogicalDevice(), writeIndexBufferMemory);

  vkResetCommandBuffer(commandBuffer, /*VkCommandBufferResetFlagBits*/ 0);

  recordCommandBuffer( imageIndex);

  VkSubmitInfo submitInfo{};
  submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;

  VkSemaphore waitSemaphores[] = {imageAvailableSemaphore};
  VkPipelineStageFlags waitStages[] = {
      VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
  submitInfo.waitSemaphoreCount = 1;
  submitInfo.pWaitSemaphores = waitSemaphores;
  submitInfo.pWaitDstStageMask = waitStages;

  submitInfo.commandBufferCount = 1;
  submitInfo.pCommandBuffers = &commandBuffer;

  VkSemaphore signalSemaphores[] = {renderFinishedSemaphore};
  submitInfo.signalSemaphoreCount = 1;
  submitInfo.pSignalSemaphores = signalSemaphores;
  VkResult submitResult = vkQueueSubmit(device.getGraphicsQueue(), 1, &submitInfo, inFlightFence);
  if (submitResult !=
      VK_SUCCESS) {
      spdlog::error("failed to submit draw command buffer! {}",submitResult);
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

  vkQueuePresentKHR(device.getPresentQueue(), &presentInfo);
}

void GpuScene::createSyncObjects() {
  VkSemaphoreCreateInfo semaphoreInfo{};
  semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

  VkFenceCreateInfo fenceInfo{};
  fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
  fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

  if (vkCreateSemaphore(device.getLogicalDevice(), &semaphoreInfo, nullptr,
                        &imageAvailableSemaphore) != VK_SUCCESS ||
      vkCreateSemaphore(device.getLogicalDevice(), &semaphoreInfo, nullptr,
                        &renderFinishedSemaphore) != VK_SUCCESS ||
      vkCreateFence(device.getLogicalDevice(), &fenceInfo, nullptr, &inFlightFence) !=
          VK_SUCCESS) {
    throw std::runtime_error(
        "failed to create synchronization objects for a frame!");
  }
}

AAPLTextureData::AAPLTextureData(AAPLTextureData&& rhs)
{
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


AAPLTextureData::AAPLTextureData(FILE * f)
{
    int path_length = 0;
    fread(&path_length,sizeof(int),1,f);
    char * cstring = (char*)malloc(path_length+1);
    fread(cstring,1,path_length,f);
    cstring[path_length]=0;
    _path = std::string(cstring);
    free(cstring);
    fread(&_pathHash, sizeof(uint32_t), 1, f);
    fread(&_width, sizeof(unsigned long long), 1, f);
    fread(&_height, sizeof(unsigned long long), 1, f);
    fread(&_mipmapLevelCount, sizeof(unsigned long long), 1, f);
    fread(&_pixelFormat, sizeof(uint32_t), 1, f);
    fread(&_pixelDataOffset, sizeof(unsigned long long), 1, f);
    fread(&_pixelDataLength, sizeof(unsigned long long), 1, f);
    
    for(int i=0;i<_mipmapLevelCount;i++)
    {
        unsigned long long offset_c;
        fread(&offset_c,sizeof(offset_c),1,f);
        _mipOffsets.push_back(offset_c);
    }

    for(int i=0;i<_mipmapLevelCount;i++)
    {
        unsigned long long length_c;
        fread(&length_c,sizeof(length_c),1,f);
        _mipLengths.push_back(length_c);
    }

}


AAPLMeshData::~AAPLMeshData()
{
  if(_vertexData)
    free(_vertexData);
  if(_normalData)
    free(_normalData);
  if(_tangentData)
    free(_tangentData);
  if(_uvData)
    free(_uvData);
  if(_indexData)
    free(_indexData);
  if(_chunkData)
    free(_chunkData);
  if(_meshData)
    free(_meshData);
  if(_materialData)
    free(_materialData);
  if(_textureData)
    free(_textureData);
}

enum MTLIndexType
{
    MTLIndexTypeUInt16 = 0,
    MTLIndexTypeUInt32 = 1
};

AAPLMeshData::AAPLMeshData(const char* filepath)
{
  FILE * rawFile = fopen(filepath,"rb");
        if(rawFile)
        {
          //unsigned long _vertexCount,_indexCount,_indexType,_chunkCount,_meshCount,_opaqueChunkCount,_opaqueMeshCount,_alphaMaskedChunkCount,_alphaMaskedMeshCount,_transparentChunkCount,_transparentMeshCount,_materialCount;
            fread(&_vertexCount,sizeof(_vertexCount),1,rawFile);
            fread(&_indexCount,sizeof(_indexCount),1,rawFile);
            fread(&_indexType,sizeof(_indexType),1,rawFile);
            if (_indexType != MTLIndexTypeUInt32)
                spdlog::error("index type error!!!");
            fread(&_chunkCount,sizeof(_chunkCount),1,rawFile);
            fread(&_meshCount,sizeof(_meshCount),1,rawFile);
            fread(&_opaqueChunkCount,sizeof(_opaqueChunkCount),1,rawFile);
            fread(&_opaqueMeshCount,sizeof(_opaqueMeshCount),1,rawFile);
            fread(&_alphaMaskedChunkCount,sizeof(_alphaMaskedChunkCount),1,rawFile);
            fread(&_alphaMaskedMeshCount,sizeof(_alphaMaskedMeshCount),1,rawFile);
            fread(&_transparentChunkCount,sizeof(_transparentChunkCount),1,rawFile);
            fread(&_transparentMeshCount,sizeof(_transparentMeshCount),1,rawFile);
            fread(&_materialCount,sizeof(_materialCount),1,rawFile);
            
            unsigned long long bytes_length = 0;
            fread(&bytes_length,sizeof(bytes_length),1,rawFile);
            compressedVertexDataLength = bytes_length;
            _vertexData = malloc(bytes_length);
            fread(_vertexData, 1, bytes_length, rawFile);
            
            
            fread(&bytes_length,sizeof(bytes_length),1,rawFile);
            compressedNormalDataLength = bytes_length;
            _normalData = malloc(bytes_length);
            fread(_normalData, 1, bytes_length, rawFile);
            
            
            fread(&bytes_length,sizeof(bytes_length),1,rawFile);
            compressedTangentDataLength = bytes_length;
            _tangentData = malloc(bytes_length);
            fread(_tangentData, 1, bytes_length, rawFile);
            
            
            fread(&bytes_length,sizeof(bytes_length),1,rawFile);
            compressedUvDataLength = bytes_length;
            _uvData = malloc(bytes_length);
            fread(_uvData, 1, bytes_length, rawFile);
            
            
            fread(&bytes_length,sizeof(bytes_length),1,rawFile);
            compressedIndexDataLength = bytes_length;
            _indexData = malloc(bytes_length);
            fread(_indexData, 1, bytes_length, rawFile);
            
            
            fread(&bytes_length,sizeof(bytes_length),1,rawFile);
            compressedChunkDataLength = bytes_length;
            _chunkData = malloc(bytes_length);
            fread(_chunkData, 1, bytes_length, rawFile);
            
            
            fread(&bytes_length,sizeof(bytes_length),1,rawFile);
            compressedMeshDataLength = bytes_length;
            _meshData = malloc(bytes_length);
            fread(_meshData, 1, bytes_length, rawFile);
            
            
            fread(&bytes_length,sizeof(bytes_length),1,rawFile);
            compressedMaterialDataLength = bytes_length;
            _materialData = malloc(bytes_length);
            fread(_materialData, 1, bytes_length, rawFile);
            
            
            unsigned long long texture_count = 0;
            fread(&texture_count,sizeof(bytes_length),1,rawFile);
            
            //[[NSArray alloc] initWithObjects:[[AAPLTextureData alloc] init] count:texture_count];
            
            for(int i=0;i<texture_count;++i)
            {
                _textures.push_back(AAPLTextureData(rawFile));
            }
            
            
            
            fread(&bytes_length,sizeof(bytes_length),1,rawFile);
            _textureData = malloc(bytes_length);
            fread(_textureData,1,bytes_length,rawFile);
            
            
            fclose(rawFile);
        }

        else
        {
          spdlog::error("file not found {}",filepath);
        }
}







// Helper to get the properties of block compressed pixel formats used by this sample.
void getBCProperties(MTLPixelFormat pixelFormat, unsigned long long& blockSize, unsigned long long& bytesPerBlock, unsigned long long & channels, int& alpha)
{
    if (pixelFormat == MTLPixelFormatBC5_RGUnorm || pixelFormat == MTLPixelFormatBC5_RGSnorm)
    {
        blockSize = 4;
        bytesPerBlock = 16;
        channels = 2;
        alpha = 0;
    }
    else if (pixelFormat == MTLPixelFormatBC4_RUnorm)
    {
        blockSize = 4;
        bytesPerBlock = 8;
        channels = 1;
        alpha = 0;
    }
    else if (pixelFormat == MTLPixelFormatBC1_RGBA_sRGB || pixelFormat == MTLPixelFormatBC1_RGBA)
    {
        blockSize = 4;
        bytesPerBlock = 8;
        channels = 4;
        alpha = 0;
    }
    else if (pixelFormat == MTLPixelFormatBC3_RGBA_sRGB || pixelFormat == MTLPixelFormatBC3_RGBA)
    {
        blockSize = 4;
        bytesPerBlock = 16;
        channels = 4;
        alpha = 1;
    }
}


void getPixelFormatBlockDesc(MTLPixelFormat pixelFormat, unsigned long long & blockSize, unsigned long long& bytesPerBlock)
{
    blockSize = 4;
    bytesPerBlock = 16;


     unsigned long long  channels_UNUSED = 0;
     int alpha_UNUSED = 1;
    getBCProperties(pixelFormat, blockSize, bytesPerBlock, channels_UNUSED, alpha_UNUSED);

}

#define MAX(a,b) ((a)>(b)?(a):(b))

unsigned long long calculateMipSizeInBlocks(unsigned long long size, unsigned long long blockSize, unsigned long long mip)
{
    unsigned long long blocksWide = MAX(size / blockSize, 1);

    return MAX(blocksWide >> mip, 1U);
}

void* GpuScene::loadMipTexture(const AAPLTextureData& texturedata, int mip, unsigned int & bytesPerImage)
{


    void* texturedataRaw = (unsigned char*)applMesh->_textureData + texturedata._pixelDataOffset;

    unsigned long long blockSize, bytesPerBlock;
    getPixelFormatBlockDesc((MTLPixelFormat)texturedata._pixelFormat, blockSize, bytesPerBlock);

    unsigned long long blocksWide = calculateMipSizeInBlocks(texturedata._width, blockSize, mip);
    unsigned long long blocksHigh = calculateMipSizeInBlocks(texturedata._height, blockSize, mip);

    unsigned long long tempbufferSize = 0;
    unsigned long long bytesPerRow = MAX(blocksWide >> 0, 1U) * bytesPerBlock;
    bytesPerImage = MAX(blocksHigh >> 0, 1U) * bytesPerRow;
    //if (bytesPerImage != texturedata._mipLengths[mip])
    //    spdlog::warn("texture data may be corrupted");
    void* uncompresseddata = uncompressData((unsigned char*)texturedataRaw + texturedata._mipOffsets[mip], texturedata._mipLengths[mip], bytesPerImage);


    return uncompresseddata;
}


std::pair<VkImageView, VkDeviceMemory> GpuScene::createTexture(const std::string& path)
{
    VkImage textureImage;
    VkImageView currentImage;
    int texWidth, texHeight, texChannels;
    stbi_uc* pixels = stbi_load(path.c_str(), &texWidth, &texHeight, &texChannels, STBI_rgb_alpha);
    VkDeviceSize imageSize = texWidth * texHeight * 4;

    if (!pixels) {
        throw std::runtime_error("failed to load texture image!");
    }

    VkBuffer stagingBuffer;
    VkDeviceMemory stagingBufferMemory;
    createBuffer(imageSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, stagingBuffer, stagingBufferMemory);

    void* data;
    vkMapMemory(device.getLogicalDevice(), stagingBufferMemory, 0, imageSize, 0, &data);
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
    imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateImage(device.getLogicalDevice(), &imageInfo, nullptr, &textureImage) != VK_SUCCESS) {
        throw std::runtime_error("failed to create image!");
    }

    VkMemoryRequirements memRequirements;
    vkGetImageMemoryRequirements(device.getLogicalDevice(), textureImage, &memRequirements);

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memRequirements.size;
    allocInfo.memoryTypeIndex = findMemoryType(memRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    VkDeviceMemory textureImageMemory;
    if (vkAllocateMemory(device.getLogicalDevice(), &allocInfo, nullptr, &textureImageMemory) != VK_SUCCESS) {
        throw std::runtime_error("failed to allocate image memory!");
    }

    vkBindImageMemory(device.getLogicalDevice(), textureImage, textureImageMemory, 0);

    device.transitionImageLayout(textureImage, VK_FORMAT_R8G8B8A8_SRGB, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    device.copyBufferToImage(stagingBuffer, textureImage, static_cast<uint32_t>(texWidth), static_cast<uint32_t>(texHeight));
    device.transitionImageLayout(textureImage, VK_FORMAT_R8G8B8A8_SRGB, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    vkDestroyBuffer(device.getLogicalDevice(), stagingBuffer, nullptr);
    vkFreeMemory(device.getLogicalDevice(), stagingBufferMemory, nullptr);

    VkImageViewCreateInfo imageviewInfo{};
    imageviewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    imageviewInfo.image = textureImage;
    imageviewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;

    imageviewInfo.format = VK_FORMAT_R8G8B8A8_SRGB;
    imageviewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    imageviewInfo.subresourceRange.baseMipLevel = 0;
    imageviewInfo.subresourceRange.levelCount = 1;// texturedata._mipmapLevelCount;
    imageviewInfo.subresourceRange.baseArrayLayer = 0;
    imageviewInfo.subresourceRange.layerCount = 1;

    //VkImageView imageView;
    if (vkCreateImageView(device.getLogicalDevice(), &imageviewInfo, nullptr, &currentImage) != VK_SUCCESS) {
        throw std::runtime_error("failed to create texture image view!");
    }
    return std::make_pair(currentImage, textureImageMemory);
}

std::pair<VkImage, VkImageView> GpuScene::createTexture(const AAPLTextureData& texturedata)
{
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
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;//TODO: switch to linear with initiallayout=preinitialized?
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.format = mapFromApple((MTLPixelFormat)(texturedata._pixelFormat));
    imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.flags = 0; // Optional

    
    if (vkCreateImage(device.getLogicalDevice(), &imageInfo, nullptr, &textureImage) != VK_SUCCESS) {
        throw std::runtime_error("failed to create image!");
    }

    VkMemoryRequirements memRequirements;
    vkGetImageMemoryRequirements(device.getLogicalDevice(), textureImage, &memRequirements);

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memRequirements.size;
    allocInfo.memoryTypeIndex = findMemoryType(memRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    VkDeviceMemory textureImageMemory;
    if (vkAllocateMemory(device.getLogicalDevice(), &allocInfo, nullptr, &textureImageMemory) != VK_SUCCESS) {
        throw std::runtime_error("failed to allocate image memory!");
    }
    vkBindImageMemory(device.getLogicalDevice(), textureImage, textureImageMemory, 0);

   
    device.transitionImageLayout(textureImage, mapFromApple((MTLPixelFormat)(texturedata._pixelFormat)), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,texturedata._mipmapLevelCount);
    for (int miplevel = 0; miplevel < texturedata._mipmapLevelCount; ++miplevel)
    {
        unsigned int rawDataLength = 0;
        void* pixelDataRaw = loadMipTexture(texturedata, miplevel, rawDataLength);

        //dds_image_t ddsimage = dds_load_from_memory((const char*)pixelDataRaw, rawDataLength);
        //spdlog::info("ddsimage info {}, {}", ddsimage->header.width, ddsimage->header.height);

        VkBuffer stagingBuffer;
        VkDeviceMemory stagingBufferMemory;
        createBuffer(rawDataLength, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, stagingBuffer, stagingBufferMemory);

        void* mappedData;
        vkMapMemory(device.getLogicalDevice(), stagingBufferMemory, 0, rawDataLength, 0, &mappedData);
        memcpy(mappedData, pixelDataRaw, static_cast<size_t>(rawDataLength));
        vkUnmapMemory(device.getLogicalDevice(), stagingBufferMemory);

        free(pixelDataRaw);

        
        device.copyBufferToImage(stagingBuffer, textureImage, static_cast<uint32_t>(texturedata._width)>>miplevel, static_cast<uint32_t>(texturedata._height)>>miplevel,miplevel);
        

        vkDestroyBuffer(device.getLogicalDevice(), stagingBuffer, nullptr);
        vkFreeMemory(device.getLogicalDevice(), stagingBufferMemory, nullptr);
    }
    device.transitionImageLayout(textureImage, mapFromApple((MTLPixelFormat)(texturedata._pixelFormat)), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,texturedata._mipmapLevelCount);

    VkImageViewCreateInfo imageviewInfo{};
    imageviewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    imageviewInfo.image = textureImage;
    imageviewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;

    imageviewInfo.format = mapFromApple((MTLPixelFormat)(texturedata._pixelFormat));;
    imageviewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    imageviewInfo.subresourceRange.baseMipLevel = 0;
    imageviewInfo.subresourceRange.levelCount =  texturedata._mipmapLevelCount;
    imageviewInfo.subresourceRange.baseArrayLayer = 0;
    imageviewInfo.subresourceRange.layerCount = 1;

    //VkImageView imageView;
    if (vkCreateImageView(device.getLogicalDevice(), &imageviewInfo, nullptr, &currentImage) != VK_SUCCESS) {
        throw std::runtime_error("failed to create texture image view!");
    }

    return std::make_pair(textureImage,currentImage);
}

bool updated = false;
void GpuScene::updateSamplerInDescriptors(VkImageView currentImage)
{
    if (updated)
        return;
    updated = true;
    VkDescriptorImageInfo imageinfo;
    imageinfo.imageView = currentImage;
    imageinfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imageinfo.sampler = textureSampler;
   

    VkWriteDescriptorSet setWrite = {};
    setWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    setWrite.pNext = nullptr;

    // we are going to write into binding number 0
    setWrite.dstBinding = 1;
    // of the global descriptor
    setWrite.dstSet = globalDescriptor;

    setWrite.descriptorCount = 1;
    // and the type is uniform buffer
    setWrite.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    setWrite.pImageInfo = &imageinfo;
    //setWrite.pBufferInfo = &binfo;

    vkUpdateDescriptorSets(device.getLogicalDevice(), 1, &setWrite, 0, nullptr);
}






