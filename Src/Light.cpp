#include "Light.h"
#include "GpuScene.h"
#include "VulkanCompat.h"
#include <math.h>

static VkBuffer sphereBuffer = nullptr;
static VkBuffer sphereIndexBuffer = nullptr;

VkBuffer PointLight::pointLightDynamicUniformBuffer = nullptr;
VkPipelineLayout PointLight::drawPointLightPipelineLayout = nullptr;
VkPipeline PointLight::drawPointLightPipeline = nullptr;
VkPipeline PointLight::drawPointLightPipelineStencil = nullptr;

VkDescriptorPool PointLight::pointLightingDescriptorPool = nullptr;
VkDescriptorSetLayout PointLight::drawPointLightDescriptorSetLayout = nullptr;
std::vector<VkDescriptorSet> PointLight::drawPointLightDescriptorSets;

std::vector<PointLightData> PointLight::pointLightData;
std::vector<SpotLightData> SpotLight::spotLightData;

constexpr uint32_t SPHERE_SLICE = 15;
constexpr uint32_t SPHERE_SLICE_2 = 32;
constexpr uint32_t SPHERE_INDEX_COUNT = SPHERE_SLICE * SPHERE_SLICE_2 * 6;
void PointLight::InitRHI(const VulkanDevice &device, const GpuScene &gpuScene) {
  if (!sphereBuffer) {
    constexpr float deltaPI = M_PI_F / SPHERE_SLICE;
    std::vector<vec3> sphereVertices;
    std::vector<uint16_t> sphereIndices;
    for (int i = 0; i < SPHERE_SLICE + 1; ++i) {
      vec3 vertex(0, sinf(i * deltaPI), cosf(i * deltaPI));
      sphereVertices.push_back(vertex);
    }
    constexpr float delta2PI = M_PI_F * 2 / SPHERE_SLICE_2;
    for (int i = 1; i < SPHERE_SLICE_2; i++) {
      float angle = delta2PI * i;
      // rotate around Y
      // mat4 rotateY = rotateY(angle);
      for (int j = 0; j < SPHERE_SLICE + 1; ++j) {
        vec4 vertex = rotateZ(angle) * vec4(sphereVertices[j], 1);
        sphereVertices.push_back(vertex.xyz());
      }
    }

    for (int i = 0; i < SPHERE_SLICE_2; i++) {
      int i_1 = (i + 1) % SPHERE_SLICE_2;
      for (int j = 0; j < SPHERE_SLICE; j++) {
        int j_1 = j + 1;
        uint16_t primitive_index0 = i * (SPHERE_SLICE + 1) + j;
        uint16_t primitive_index1 = i_1 * (SPHERE_SLICE + 1) + j;
        uint16_t primitive_index2 = i * (SPHERE_SLICE + 1) + j_1;
        uint16_t primitive_index3 = i_1 * (SPHERE_SLICE + 1) + j_1;
        // clock wise
        sphereIndices.push_back(primitive_index0);
        sphereIndices.push_back(primitive_index2);
        sphereIndices.push_back(primitive_index1);

        sphereIndices.push_back(primitive_index1);
        sphereIndices.push_back(primitive_index2);
        sphereIndices.push_back(primitive_index3);
      }
    }

    VkBufferCreateInfo pointLightVertexBufferInfo{};
    pointLightVertexBufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    pointLightVertexBufferInfo.size = sphereVertices.size() * sizeof(vec3);
    pointLightVertexBufferInfo.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    pointLightVertexBufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    pointLightVertexBufferInfo.flags = 0;
    if (vkCreateBuffer(device.getLogicalDevice(), &pointLightVertexBufferInfo,
                       nullptr, &sphereBuffer) != VK_SUCCESS) {
      throw std::runtime_error("failed to create sphere vertex buffer!");
    }
    VkMemoryRequirements memRequirements;
    vkGetBufferMemoryRequirements(device.getLogicalDevice(), sphereBuffer,
                                  &memRequirements);

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memRequirements.size;
    allocInfo.memoryTypeIndex =
        device.findMemoryType(memRequirements.memoryTypeBits,
                              VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                  VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkDeviceMemory sphereBufferMemory;
    if (vkAllocateMemory(device.getLogicalDevice(), &allocInfo, nullptr,
                         &sphereBufferMemory) != VK_SUCCESS) {
      throw std::runtime_error("failed to allocate pointLight buffer memory!");
    }
    vkBindBufferMemory(device.getLogicalDevice(), sphereBuffer,
                       sphereBufferMemory, 0);
    void *data;
    vkMapMemory(device.getLogicalDevice(), sphereBufferMemory, 0,
                pointLightVertexBufferInfo.size, 0, &data);
    std::memcpy(data, (void *)sphereVertices.data(),
                pointLightVertexBufferInfo.size);
    vkUnmapMemory(device.getLogicalDevice(), sphereBufferMemory);

    pointLightVertexBufferInfo.size = sphereIndices.size() * sizeof(uint16_t);
    pointLightVertexBufferInfo.usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
    pointLightVertexBufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    pointLightVertexBufferInfo.flags = 0;
    if (vkCreateBuffer(device.getLogicalDevice(), &pointLightVertexBufferInfo,
                       nullptr, &sphereIndexBuffer) != VK_SUCCESS) {
      throw std::runtime_error("failed to create sphere index buffer!");
    }
    vkGetBufferMemoryRequirements(device.getLogicalDevice(), sphereIndexBuffer,
                                  &memRequirements);
    allocInfo.allocationSize = memRequirements.size;
    allocInfo.memoryTypeIndex =
        device.findMemoryType(memRequirements.memoryTypeBits,
                              VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                  VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkDeviceMemory sphereIndexBufferMemory;
    if (vkAllocateMemory(device.getLogicalDevice(), &allocInfo, nullptr,
                         &sphereIndexBufferMemory) != VK_SUCCESS) {
      throw std::runtime_error(
          "failed to allocate pointLight index buffer memory!");
    }
    vkBindBufferMemory(device.getLogicalDevice(), sphereIndexBuffer,
                       sphereIndexBufferMemory, 0);
    vkMapMemory(device.getLogicalDevice(), sphereIndexBufferMemory, 0,
                pointLightVertexBufferInfo.size, 0, &data);
    std::memcpy(data, (void *)sphereIndices.data(),
                pointLightVertexBufferInfo.size);
    vkUnmapMemory(device.getLogicalDevice(), sphereIndexBufferMemory);
  }

  if (!PointLight::drawPointLightPipelineLayout) {
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
    frameDataBinding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    frameDataBinding.stageFlags =
        VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_VERTEX_BIT;

    VkDescriptorSetLayoutBinding pointLightDataBinding = {};
    pointLightDataBinding.binding = 7;
    pointLightDataBinding.descriptorCount = 1;
    // it's a uniform buffer binding
    pointLightDataBinding.descriptorType =
        VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    pointLightDataBinding.stageFlags =
        VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_VERTEX_BIT;

    VkDescriptorSetLayoutBinding bindings[] = {
        albedoBinding,      normalBinding,        emessiveBinding,
        f0RoughnessBinding, depthBinding,         nearestClampSamplerBinding,
        frameDataBinding,   pointLightDataBinding};

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
                                &PointLight::drawPointLightDescriptorSetLayout);

    std::vector<VkDescriptorPoolSize> sizes = {
        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 12 * gpuScene.framesInFlight},
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 3 * gpuScene.framesInFlight},
        {VK_DESCRIPTOR_TYPE_SAMPLER, 3 * gpuScene.framesInFlight},
    };

    VkDescriptorPoolCreateInfo pool_info = {};
    pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_info.flags = 0;
    pool_info.maxSets = 2 * gpuScene.framesInFlight;
    pool_info.poolSizeCount = (uint32_t)sizes.size();
    pool_info.pPoolSizes = sizes.data();

    vkCreateDescriptorPool(device.getLogicalDevice(), &pool_info, nullptr,
                           &PointLight::pointLightingDescriptorPool);

    PointLight::drawPointLightDescriptorSets.resize(gpuScene.framesInFlight);
    std::vector<VkDescriptorSetLayout> layouts(gpuScene.framesInFlight, PointLight::drawPointLightDescriptorSetLayout);

    VkDescriptorSetAllocateInfo allocInfo = {};
    allocInfo.pNext = nullptr;
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = PointLight::pointLightingDescriptorPool;
    allocInfo.descriptorSetCount = gpuScene.framesInFlight;
    allocInfo.pSetLayouts = layouts.data();

    vkAllocateDescriptorSets(device.getLogicalDevice(), &allocInfo,
                             PointLight::drawPointLightDescriptorSets.data());

    for (uint32_t f = 0; f < gpuScene.framesInFlight; ++f) {
      VkDescriptorBufferInfo binfo;
      binfo.buffer = gpuScene.uniformBuffers[f];
      binfo.offset = 0;
      binfo.range = sizeof(FrameData);

      VkWriteDescriptorSet setWrite = {};
      setWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      setWrite.pNext = nullptr;
      setWrite.dstBinding = 6;
      setWrite.dstSet = PointLight::drawPointLightDescriptorSets[f];
      setWrite.descriptorCount = 1;
      setWrite.dstArrayElement = 0;
      setWrite.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
      setWrite.pBufferInfo = &binfo;

      VkDescriptorBufferInfo binfo1;
      binfo1.buffer = PointLight::pointLightDynamicUniformBuffer;
      binfo1.offset = 0;
      binfo1.range = sizeof(PointLightData);

      VkWriteDescriptorSet setWrite1 = {};
      setWrite1.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      setWrite1.pNext = nullptr;
      setWrite1.dstBinding = 7;
      setWrite1.dstSet = PointLight::drawPointLightDescriptorSets[f];
      setWrite1.descriptorCount = 1;
      setWrite1.dstArrayElement = 0;
      setWrite1.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
      setWrite1.pBufferInfo = &binfo1;

      VkDescriptorImageInfo samplerinfo;
      samplerinfo.sampler = gpuScene.nearestClampSampler;
      VkWriteDescriptorSet setSampler = {};
      setSampler.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      setSampler.dstBinding = 5;
      setSampler.pNext = nullptr;
      setSampler.dstSet = PointLight::drawPointLightDescriptorSets[f];
      setSampler.dstArrayElement = 0;
      setSampler.descriptorCount = 1;
      setSampler.pImageInfo = &samplerinfo;
      setSampler.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;

      std::array<VkDescriptorImageInfo, 4> imageinfo{};
      for (int texturei = 0; texturei < 4; ++texturei) {
        imageinfo[texturei].imageView = gpuScene._gbuffersView[texturei][f];
        imageinfo[texturei].imageLayout =
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
      }

      VkWriteDescriptorSet setWriteTexture[4] = {};
      for (int i = 0; i < 4; i++) {
        setWriteTexture[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        setWriteTexture[i].pNext = nullptr;
        setWriteTexture[i].dstBinding = i;
        setWriteTexture[i].dstSet = PointLight::drawPointLightDescriptorSets[f];
        setWriteTexture[i].dstArrayElement = 0;
        setWriteTexture[i].descriptorCount = 1;
        setWriteTexture[i].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        setWriteTexture[i].pImageInfo = &imageinfo[i];
      }

      VkDescriptorImageInfo depthImageInfo{};
      depthImageInfo.imageView = device.getWindowDepthOnlyImageView(f);
      depthImageInfo.imageLayout =
          VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL;
      VkWriteDescriptorSet setWriteDepth;
      setWriteDepth.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      setWriteDepth.pNext = nullptr;
      setWriteDepth.dstBinding = 4;
      setWriteDepth.dstSet = PointLight::drawPointLightDescriptorSets[f];
      setWriteDepth.dstArrayElement = 0;
      setWriteDepth.descriptorCount = 1;
      setWriteDepth.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
      setWriteDepth.pImageInfo = &depthImageInfo;

      std::array<VkWriteDescriptorSet, 8> writes = {
          setWriteTexture[0], setWriteTexture[1], setWriteTexture[2],
          setWriteTexture[3], setWriteDepth,      setSampler,
          setWrite,           setWrite1};

      vkUpdateDescriptorSets(device.getLogicalDevice(), writes.size(),
                             writes.data(), 0, nullptr);
    }

    VkPipelineLayoutCreateInfo pointLightingPipelineLayoutInfo{};
    pointLightingPipelineLayoutInfo.sType =
        VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pointLightingPipelineLayoutInfo.setLayoutCount = 1;
    pointLightingPipelineLayoutInfo.pSetLayouts =
        &PointLight::drawPointLightDescriptorSetLayout;
    pointLightingPipelineLayoutInfo.pushConstantRangeCount = 0;

    if (vkCreatePipelineLayout(device.getLogicalDevice(),
                               &pointLightingPipelineLayoutInfo, nullptr,
                               &drawPointLightPipelineLayout) != VK_SUCCESS) {
      throw std::runtime_error(
          "failed to create drawpoint light pipeline layout!");
    }

    auto deferredPointLightingVSShaderCode =
        readFile((gpuScene.RootPath() / "shaders/deferredPointLighting.vs.spv")
                     .generic_string());
    auto deferredPointLightingPSShaderCode =
        readFile((gpuScene.RootPath() / "shaders/deferredPointLighting.ps.spv")
                     .generic_string());

    VkShaderModule deferredPointLightingVSShaderModule =
        gpuScene.createShaderModule(deferredPointLightingVSShaderCode);
    VkShaderModule deferredPointLightingPSShaderModule =
        gpuScene.createShaderModule(deferredPointLightingPSShaderCode);

    VkPipelineShaderStageCreateInfo deferredLightingVSShaderStageInfo{};
    deferredLightingVSShaderStageInfo.sType =
        VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    deferredLightingVSShaderStageInfo.stage = VK_SHADER_STAGE_VERTEX_BIT;
    deferredLightingVSShaderStageInfo.module =
        deferredPointLightingVSShaderModule;
    deferredLightingVSShaderStageInfo.pName = "RenderSceneVS";

    VkPipelineShaderStageCreateInfo deferredLightingPSShaderStageInfo{};
    deferredLightingPSShaderStageInfo.sType =
        VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    deferredLightingPSShaderStageInfo.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    deferredLightingPSShaderStageInfo.module =
        deferredPointLightingPSShaderModule;
    deferredLightingPSShaderStageInfo.pName = "DeferredLighting";

    VkPipelineShaderStageCreateInfo deferredPointLightingPassStages[] = {
        deferredLightingVSShaderStageInfo, deferredLightingPSShaderStageInfo};

    VkPipelineShaderStageCreateInfo deferredPointLightingPassStagesStencil[] = {
        deferredLightingVSShaderStageInfo};

    constexpr VkVertexInputBindingDescription
        drawPointLightInputBindingPosition = {.binding = 0,
                                              .stride = sizeof(float) * 3,
                                              .inputRate =
                                                  VK_VERTEX_INPUT_RATE_VERTEX};

    constexpr VkVertexInputAttributeDescription
        drawPointLightInputAttributes[] = {
            {.location = 0,
             .binding = 0,
             .format = VK_FORMAT_R32G32B32_SFLOAT,
             .offset = 0},
        };
    constexpr int inputChannelCount = sizeof(drawPointLightInputAttributes) /
                                      sizeof(drawPointLightInputAttributes[0]);

    constexpr std::array<VkVertexInputBindingDescription, inputChannelCount>
        drawPointLightInputs = {drawPointLightInputBindingPosition};

    VkPipelineVertexInputStateCreateInfo drawPointLightVertexInputInfo{};
    drawPointLightVertexInputInfo.sType =
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    drawPointLightVertexInputInfo.vertexBindingDescriptionCount =
        drawPointLightInputs.size();
    drawPointLightVertexInputInfo.pVertexBindingDescriptions =
        drawPointLightInputs.data();
    drawPointLightVertexInputInfo.vertexAttributeDescriptionCount =
        inputChannelCount;
    drawPointLightVertexInputInfo.pVertexAttributeDescriptions =
        drawPointLightInputAttributes;

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
    rasterizer.sType =
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.depthClampEnable = VK_FALSE;
    rasterizer.rasterizerDiscardEnable = VK_FALSE;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.lineWidth = 1.0f;
    rasterizer.cullMode =
        VK_CULL_MODE_BACK_BIT; // draw the backface of the sphere
    rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterizer.depthBiasEnable = VK_FALSE;

    VkPipelineRasterizationStateCreateInfo rasterizer1{};
    rasterizer1.sType =
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer1.depthClampEnable = VK_FALSE;
    rasterizer1.rasterizerDiscardEnable = VK_FALSE;
    rasterizer1.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer1.lineWidth = 1.0f;
    rasterizer1.cullMode = VK_CULL_MODE_BACK_BIT;
    rasterizer1.frontFace = VK_FRONT_FACE_CLOCKWISE; // stencil pass
    rasterizer1.depthBiasEnable = VK_FALSE;

    VkPipelineMultisampleStateCreateInfo multisampling{};
    multisampling.sType =
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.sampleShadingEnable = VK_FALSE;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

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

    VkPipelineColorBlendStateCreateInfo colorBlendingAlphaAdd{};
    colorBlendingAlphaAdd.sType =
        VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlendingAlphaAdd.logicOpEnable = VK_FALSE;
    colorBlendingAlphaAdd.logicOp = VK_LOGIC_OP_COPY;
    colorBlendingAlphaAdd.attachmentCount = 1;
    colorBlendingAlphaAdd.pAttachments = &colorBlendAttachment1;
    colorBlendingAlphaAdd.blendConstants[0] = 0.0f;
    colorBlendingAlphaAdd.blendConstants[1] = 0.0f;
    colorBlendingAlphaAdd.blendConstants[2] = 0.0f;
    colorBlendingAlphaAdd.blendConstants[3] = 0.0f;

    VkPipelineDepthStencilStateCreateInfo depthStencilState{};
    depthStencilState.sType =
        VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencilState.depthWriteEnable = VK_FALSE;
    depthStencilState.depthTestEnable = VK_TRUE;
    depthStencilState.stencilTestEnable = VK_TRUE;
    depthStencilState.front = {.failOp = VK_STENCIL_OP_KEEP,
                               .passOp = VK_STENCIL_OP_KEEP,
                               .depthFailOp = VK_STENCIL_OP_KEEP,
                               .compareOp = VK_COMPARE_OP_NOT_EQUAL,
                               .compareMask = 0xff,
                               .writeMask = 0x0,
                               .reference = 0x1};
    depthStencilState.back = {};
    depthStencilState.depthCompareOp =
        VK_COMPARE_OP_LESS; // pass the depth test only when depth of the the
                            // backface of the sphere is futher away than the
                            // depth buffer
    depthStencilState.depthBoundsTestEnable = VK_FALSE;

    VkPipelineDepthStencilStateCreateInfo depthStencilState1{};
    depthStencilState1.sType =
        VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencilState1.depthWriteEnable = VK_FALSE;
    depthStencilState1.depthTestEnable = VK_TRUE;
    depthStencilState1.stencilTestEnable = VK_TRUE;
    depthStencilState1.front = {.failOp = VK_STENCIL_OP_KEEP,
                                .passOp = VK_STENCIL_OP_KEEP,
                                .depthFailOp = VK_STENCIL_OP_REPLACE,
                                .compareOp = VK_COMPARE_OP_ALWAYS,
                                .compareMask = 0xff,
                                .writeMask = 0xff,
                                .reference = 0x1};
    depthStencilState1.back = {};
    depthStencilState1.depthCompareOp = VK_COMPARE_OP_GREATER;
    depthStencilState1.depthBoundsTestEnable = VK_FALSE;

    VkGraphicsPipelineCreateInfo pointLightingPipelineInfo{};
    pointLightingPipelineInfo.sType =
        VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pointLightingPipelineInfo.stageCount = 2;
    pointLightingPipelineInfo.pStages = deferredPointLightingPassStages;
    pointLightingPipelineInfo.pVertexInputState =
        &drawPointLightVertexInputInfo;
    pointLightingPipelineInfo.pInputAssemblyState = &inputAssembly;
    pointLightingPipelineInfo.pViewportState = &viewportState;
    pointLightingPipelineInfo.pRasterizationState = &rasterizer;
    pointLightingPipelineInfo.pMultisampleState = &multisampling;
    pointLightingPipelineInfo.pColorBlendState = &colorBlendingAlphaAdd;
    pointLightingPipelineInfo.layout = PointLight::drawPointLightPipelineLayout;
    pointLightingPipelineInfo.renderPass = gpuScene._deferredLightingPass;
    pointLightingPipelineInfo.subpass = 0;
    pointLightingPipelineInfo.basePipelineHandle = VK_NULL_HANDLE;
    pointLightingPipelineInfo.pDepthStencilState = &depthStencilState;

    if (vkCreateGraphicsPipelines(device.getLogicalDevice(), VK_NULL_HANDLE, 1,
                                  &pointLightingPipelineInfo, nullptr,
                                  &PointLight::drawPointLightPipeline) !=
        VK_SUCCESS) {
      throw std::runtime_error(
          "failed to create drawcluster base graphics pipeline!");
    }

    VkGraphicsPipelineCreateInfo pointLightingPipelineInfoStencil{};
    pointLightingPipelineInfoStencil.sType =
        VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pointLightingPipelineInfoStencil.stageCount = 1;
    pointLightingPipelineInfoStencil.pStages =
        deferredPointLightingPassStagesStencil;
    pointLightingPipelineInfoStencil.pVertexInputState =
        &drawPointLightVertexInputInfo;
    pointLightingPipelineInfoStencil.pInputAssemblyState = &inputAssembly;
    pointLightingPipelineInfoStencil.pViewportState = &viewportState;
    pointLightingPipelineInfoStencil.pRasterizationState = &rasterizer1;
    pointLightingPipelineInfoStencil.pMultisampleState = &multisampling;
    pointLightingPipelineInfoStencil.pColorBlendState = &colorBlendingAlphaAdd;
    pointLightingPipelineInfoStencil.layout =
        PointLight::drawPointLightPipelineLayout;
    pointLightingPipelineInfoStencil.renderPass =
        gpuScene._deferredLightingPass;
    pointLightingPipelineInfoStencil.subpass = 0;
    pointLightingPipelineInfoStencil.basePipelineHandle = VK_NULL_HANDLE;
    pointLightingPipelineInfoStencil.pDepthStencilState = &depthStencilState1;

    if (vkCreateGraphicsPipelines(device.getLogicalDevice(), VK_NULL_HANDLE, 1,
                                  &pointLightingPipelineInfoStencil, nullptr,
                                  &PointLight::drawPointLightPipelineStencil) !=
        VK_SUCCESS) {
      throw std::runtime_error(
          "failed to create drawcluster base graphics pipeline!");
    }
  }
}

void PointLight::CommonDrawSetup(VkCommandBuffer &commandBuffer) {
  // only light the area where stencil not equal 1
  VkBuffer vertexBuffers[] = {sphereBuffer};
  VkDeviceSize offsets[] = {0};

  vkCmdBindVertexBuffers(commandBuffer, 0,
                         sizeof(vertexBuffers) / sizeof(vertexBuffers[0]),
                         vertexBuffers, offsets);
  vkCmdBindIndexBuffer(commandBuffer, sphereIndexBuffer, 0,
                       VK_INDEX_TYPE_UINT16);
}

void PointLight::Draw(VkCommandBuffer &commandBuffer,
                      const GpuScene &gpuScene) {
  // draw the stencil buffer
  // mark the invalid area stencil 1
  vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                    drawPointLightPipelineStencil);
  // pDynamicOffsets[1] is 32, but must be a multiple of device limit
  // minUniformBufferOffsetAlignment 64 Each element of pDynamicOffsets which
  // corresponds to a descriptor binding with type
  // VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC must be a multiple
  //  of VkPhysicalDeviceLimits::minUniformBufferOffsetAlignment
  uint32_t dynamicoffsets[2] = {0, _dynamicOffset};
  vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          drawPointLightPipelineLayout, 0, 1,
                          &drawPointLightDescriptorSets[gpuScene.currentFrame], 2, dynamicoffsets);
  vkCmdDrawIndexed(commandBuffer, SPHERE_INDEX_COUNT, 1, 0, 0, 0);

  // lighting
  vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                    drawPointLightPipeline);
  vkCmdDrawIndexed(commandBuffer, SPHERE_INDEX_COUNT, 1, 0, 0, 0);
}

void LightCuller::InitRHI(const VulkanDevice &device, const GpuScene &gpuScene,
                          uint32_t screen_width, uint32_t screen_heigt) {

  uint32_t currentFrame = gpuScene.currentFrame;
  // results buffer & descriptors
  VkBufferCreateInfo pointlightCullingDataBufferInfo{};
  pointlightCullingDataBufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  pointlightCullingDataBufferInfo.size =
      gpuScene._pointLights.size() * sizeof(vec4) * 2;
  pointlightCullingDataBufferInfo.usage =
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT; // TODO: change to uniform buffer
  pointlightCullingDataBufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  pointlightCullingDataBufferInfo.flags = 0;
  if (vkCreateBuffer(device.getLogicalDevice(),
                     &pointlightCullingDataBufferInfo, nullptr,
                     &_pointLightCullingDataBuffer) != VK_SUCCESS) {
    throw std::runtime_error("failed to create sphere vertex buffer!");
  }
  VkMemoryRequirements memRequirements0;
  vkGetBufferMemoryRequirements(device.getLogicalDevice(),
                                _pointLightCullingDataBuffer,
                                &memRequirements0);

  VkMemoryAllocateInfo allocInfo0{};
  allocInfo0.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  allocInfo0.allocationSize = memRequirements0.size;
  allocInfo0.memoryTypeIndex =
      device.findMemoryType(memRequirements0.memoryTypeBits,
                            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  VkDeviceMemory pointlightCullingDataBufferMemory;
  if (vkAllocateMemory(device.getLogicalDevice(), &allocInfo0, nullptr,
                       &pointlightCullingDataBufferMemory) != VK_SUCCESS) {
    throw std::runtime_error("failed to allocate pointLight buffer memory!");
  }
  vkBindBufferMemory(device.getLogicalDevice(), _pointLightCullingDataBuffer,
                     pointlightCullingDataBufferMemory, 0);

  vec4 *data;
  vkMapMemory(device.getLogicalDevice(), pointlightCullingDataBufferMemory, 0,
              pointlightCullingDataBufferInfo.size, 0, (void **)&data);
  uint32_t transparentPointLightCount = 0;
  for (int i = 0; i < gpuScene._pointLights.size(); ++i) {
    *data++ =
        vec4(gpuScene._pointLights[i]._pointLightData->posSqrRadius.xyz(),
             sqrtf(gpuScene._pointLights[i]._pointLightData->posSqrRadius.w));
    // encode tranparent in color.w
    bool isTransparentLight = (gpuScene._pointLights[i]._pointLightData->flags &
                               LIGHT_FOR_TRANSPARENT_FLAG) != 0;
    if (isTransparentLight)
      ++transparentPointLightCount;
    *data++ = vec4(gpuScene._pointLights[i]._pointLightData->color,
                   isTransparentLight ? -1 : 1);
  }

  spdlog::info("transparent point light count:{} total:{}",
               transparentPointLightCount, gpuScene._pointLights.size());

  vkUnmapMemory(device.getLogicalDevice(), pointlightCullingDataBufferMemory);

  VkBufferCreateInfo xzRangeBufferInfo{};
  xzRangeBufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  xzRangeBufferInfo.size = gpuScene._pointLights.size() * sizeof(uint16_t) * 4;
  xzRangeBufferInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
  xzRangeBufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  xzRangeBufferInfo.flags = 0;

  _xzRangeBuffer.resize(gpuScene.framesInFlight);
  for (uint32_t f = 0; f < gpuScene.framesInFlight; ++f) {
    if (vkCreateBuffer(device.getLogicalDevice(), &xzRangeBufferInfo, nullptr,
                       &_xzRangeBuffer[f]) != VK_SUCCESS) {
      throw std::runtime_error("failed to create xzRange buffer!");
    }
    VkMemoryRequirements memRequirements;
    vkGetBufferMemoryRequirements(device.getLogicalDevice(), _xzRangeBuffer[f],
                                  &memRequirements);
    VkMemoryAllocateInfo allocInfo1{};
    allocInfo1.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo1.allocationSize = memRequirements.size;
    allocInfo1.memoryTypeIndex = device.findMemoryType(
        memRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VkDeviceMemory xzRangeBufferMemory;
    if (vkAllocateMemory(device.getLogicalDevice(), &allocInfo1, nullptr,
                         &xzRangeBufferMemory) != VK_SUCCESS) {
      throw std::runtime_error("failed to allocate xzRange buffer memory!");
    }
    vkBindBufferMemory(device.getLogicalDevice(), _xzRangeBuffer[f],
                       xzRangeBufferMemory, 0);
  }

  uint32_t tileXCount = (screen_width + DEFAULT_LIGHT_CULLING_TILE_SIZE - 1) /
                        DEFAULT_LIGHT_CULLING_TILE_SIZE;
  uint32_t tileYCount = (screen_heigt + DEFAULT_LIGHT_CULLING_TILE_SIZE - 1) /
                        DEFAULT_LIGHT_CULLING_TILE_SIZE;

  VkBufferCreateInfo lightIndicesBufferInfo{};
  lightIndicesBufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  lightIndicesBufferInfo.size =
      tileXCount * tileYCount * sizeof(uint32_t) * MAX_LIGHTS_PER_TILE;
  lightIndicesBufferInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
  lightIndicesBufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  lightIndicesBufferInfo.flags = 0;

  VkBufferCreateInfo lightIndicesTransparentBufferInfo{};
  lightIndicesTransparentBufferInfo.sType =
      VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  lightIndicesTransparentBufferInfo.size =
      tileXCount * tileYCount * sizeof(uint32_t) * MAX_LIGHTS_PER_TILE;
  lightIndicesTransparentBufferInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
  lightIndicesTransparentBufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  lightIndicesTransparentBufferInfo.flags = 0;

  _lightIndicesBuffer.resize(gpuScene.framesInFlight);
  _lightIndicesTransparentBuffer.resize(gpuScene.framesInFlight);
  for (uint32_t f = 0; f < gpuScene.framesInFlight; ++f) {
    if (vkCreateBuffer(device.getLogicalDevice(), &lightIndicesBufferInfo,
                       nullptr, &_lightIndicesBuffer[f]) != VK_SUCCESS) {
      throw std::runtime_error("failed to create light indices buffer!");
    }
    if (vkCreateBuffer(device.getLogicalDevice(),
                       &lightIndicesTransparentBufferInfo, nullptr,
                       &_lightIndicesTransparentBuffer[f]) != VK_SUCCESS) {
      throw std::runtime_error(
          "failed to create lighttransparent indices buffer!");
    }

    VkMemoryRequirements lightindicesMemRequirements;
    vkGetBufferMemoryRequirements(device.getLogicalDevice(), _lightIndicesBuffer[f],
                                  &lightindicesMemRequirements);
    VkMemoryAllocateInfo allocInfo2{};
    allocInfo2.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo2.allocationSize = lightindicesMemRequirements.size;
    allocInfo2.memoryTypeIndex =
        device.findMemoryType(lightindicesMemRequirements.memoryTypeBits,
                              VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VkDeviceMemory lightindicesBufferMemory;
    if (vkAllocateMemory(device.getLogicalDevice(), &allocInfo2, nullptr,
                         &lightindicesBufferMemory) != VK_SUCCESS) {
      throw std::runtime_error("failed to allocate light indices buffer memory!");
    }
    vkBindBufferMemory(device.getLogicalDevice(), _lightIndicesBuffer[f],
                       lightindicesBufferMemory, 0);

    vkGetBufferMemoryRequirements(device.getLogicalDevice(),
                                  _lightIndicesTransparentBuffer[f],
                                  &lightindicesMemRequirements);
    allocInfo2.allocationSize = lightindicesMemRequirements.size;
    VkDeviceMemory lightindicesTransparentBufferMemory;
    if (vkAllocateMemory(device.getLogicalDevice(), &allocInfo2, nullptr,
                         &lightindicesTransparentBufferMemory) != VK_SUCCESS) {
      throw std::runtime_error("failed to allocate light indices transparent buffer memory!");
    }
    vkBindBufferMemory(device.getLogicalDevice(), _lightIndicesTransparentBuffer[f],
                       lightindicesTransparentBufferMemory, 0);
  }

  // ---------------- Spot light culling buffers ----------------
  // Mirrors the point light buffer block above; layout matches the
  // AAPLSpotLightCullingData / spotXZRange / spotLightIndices(+Transparent)
  // bindings in lightculling.hlsl. Always allocate at least one element so the
  // descriptor remains valid when the scene has zero spots.
  const uint32_t spotCount = std::max<uint32_t>((uint32_t)gpuScene._spotLights.size(), 1);
  const size_t spotCullDataStride = sizeof(float) * 4 * 4 + sizeof(float) * 4; // matches HLSL struct (16-byte aligned)

  VkBufferCreateInfo spotCullingDataInfo{};
  spotCullingDataInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  spotCullingDataInfo.size = spotCullDataStride * spotCount;
  spotCullingDataInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
  spotCullingDataInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  if (vkCreateBuffer(device.getLogicalDevice(), &spotCullingDataInfo, nullptr,
                     &_spotLightCullingDataBuffer) != VK_SUCCESS) {
    throw std::runtime_error("failed to create spot light culling data buffer!");
  }
  {
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(device.getLogicalDevice(),
                                  _spotLightCullingDataBuffer, &req);
    VkMemoryAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = device.findMemoryType(
        req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkDeviceMemory mem;
    if (vkAllocateMemory(device.getLogicalDevice(), &ai, nullptr, &mem) != VK_SUCCESS) {
      throw std::runtime_error("failed to allocate spot light culling data memory!");
    }
    vkBindBufferMemory(device.getLogicalDevice(), _spotLightCullingDataBuffer, mem, 0);

    // Upload spot light culling data. Layout per AAPLSpotLightCullingData:
    //   float4 posRadius, posAndHeight, dirAndOuterAngle, color, plus
    //   float cosInnerAngle + 12 bytes padding.
    // outerAngle and innerAngle stored as cosine -> shader skips cos().
    // dir normalised here so the shader's cone test reads a unit vector.
    struct GpuSpot {
      vec4 posRadius;
      vec4 posAndHeight;
      vec4 dirAndOuterAngle;
      vec4 color;
      float cosInnerAngle;
      float pad0, pad1, pad2;
    };
    static_assert(sizeof(GpuSpot) == 80, "GpuSpot must match HLSL layout");

    GpuSpot *dst = nullptr;
    vkMapMemory(device.getLogicalDevice(), mem, 0, spotCullingDataInfo.size, 0, (void **)&dst);
    uint32_t transparentSpotCount = 0;
    for (size_t i = 0; i < gpuScene._spotLights.size(); ++i) {
      const SpotLightData *sd = gpuScene._spotLights[i]._spotLightData;
      vec3 dir = sd->dirAndOuterAngle.xyz();
      float dirLen = std::sqrt(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
      if (dirLen > 1e-6f) dir = vec3(dir.x / dirLen, dir.y / dirLen, dir.z / dirLen);

      float outerAngle = sd->dirAndOuterAngle.w;
      float innerAngle = sd->colorAndInnerAngle.w;
      bool isTransparent = (sd->flags & LIGHT_FOR_TRANSPARENT_FLAG) != 0;
      if (isTransparent) ++transparentSpotCount;

      dst[i].posRadius = sd->boundingSphere;
      dst[i].posAndHeight = sd->posAndHeight;
      dst[i].dirAndOuterAngle = vec4(dir, std::cos(outerAngle));
      dst[i].color = vec4(sd->colorAndInnerAngle.xyz(), isTransparent ? -1.0f : 1.0f);
      dst[i].cosInnerAngle = std::cos(innerAngle);
      dst[i].pad0 = dst[i].pad1 = dst[i].pad2 = 0;
    }
    if (gpuScene._spotLights.empty()) {
      // Dummy entry so a 0-spot scene still has a valid buffer to bind.
      std::memset(dst, 0, sizeof(GpuSpot));
    }
    vkUnmapMemory(device.getLogicalDevice(), mem);
    spdlog::info("transparent spot light count:{} total:{}",
                 transparentSpotCount, gpuScene._spotLights.size());
  }

  // spot XZ range (per spot light, per frame)
  VkBufferCreateInfo spotXZRangeInfo{};
  spotXZRangeInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  spotXZRangeInfo.size = spotCount * sizeof(uint16_t) * 4;
  spotXZRangeInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
  spotXZRangeInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  _spotXZRangeBuffer.resize(gpuScene.framesInFlight);
  for (uint32_t f = 0; f < gpuScene.framesInFlight; ++f) {
    if (vkCreateBuffer(device.getLogicalDevice(), &spotXZRangeInfo, nullptr,
                       &_spotXZRangeBuffer[f]) != VK_SUCCESS) {
      throw std::runtime_error("failed to create spot xzRange buffer!");
    }
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(device.getLogicalDevice(), _spotXZRangeBuffer[f], &req);
    VkMemoryAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = device.findMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VkDeviceMemory mem;
    vkAllocateMemory(device.getLogicalDevice(), &ai, nullptr, &mem);
    vkBindBufferMemory(device.getLogicalDevice(), _spotXZRangeBuffer[f], mem, 0);
  }

  // spot light tile indices buffers (opaque + transparent, per frame)
  VkBufferCreateInfo spotIndicesInfo{};
  spotIndicesInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  spotIndicesInfo.size = tileXCount * tileYCount * sizeof(uint32_t) * MAX_LIGHTS_PER_TILE;
  spotIndicesInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
  spotIndicesInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  _spotLightIndicesBuffer.resize(gpuScene.framesInFlight);
  _spotLightIndicesTransparentBuffer.resize(gpuScene.framesInFlight);
  for (uint32_t f = 0; f < gpuScene.framesInFlight; ++f) {
    if (vkCreateBuffer(device.getLogicalDevice(), &spotIndicesInfo, nullptr,
                       &_spotLightIndicesBuffer[f]) != VK_SUCCESS) {
      throw std::runtime_error("failed to create spot light indices buffer!");
    }
    if (vkCreateBuffer(device.getLogicalDevice(), &spotIndicesInfo, nullptr,
                       &_spotLightIndicesTransparentBuffer[f]) != VK_SUCCESS) {
      throw std::runtime_error("failed to create spot light indices transparent buffer!");
    }
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(device.getLogicalDevice(), _spotLightIndicesBuffer[f], &req);
    VkMemoryAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = device.findMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VkDeviceMemory memOpaque;
    vkAllocateMemory(device.getLogicalDevice(), &ai, nullptr, &memOpaque);
    vkBindBufferMemory(device.getLogicalDevice(), _spotLightIndicesBuffer[f], memOpaque, 0);

    vkGetBufferMemoryRequirements(device.getLogicalDevice(), _spotLightIndicesTransparentBuffer[f], &req);
    ai.allocationSize = req.size;
    VkDeviceMemory memTransparent;
    vkAllocateMemory(device.getLogicalDevice(), &ai, nullptr, &memTransparent);
    vkBindBufferMemory(device.getLogicalDevice(), _spotLightIndicesTransparentBuffer[f], memTransparent, 0);
  }

  VkImageCreateInfo imageInfo{};
  imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  imageInfo.imageType = VK_IMAGE_TYPE_2D;
  imageInfo.extent.width = tileXCount;
  imageInfo.extent.height = tileYCount;
  imageInfo.extent.depth = 1;
  imageInfo.mipLevels = 1; // texturedata._mipmapLevelCount;
  imageInfo.arrayLayers = 1;
  imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL; // TODO: switch to linear with
  // initiallayout=preinitialized?
  imageInfo.initialLayout =
      VK_IMAGE_LAYOUT_UNDEFINED; // VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
  imageInfo.format =
      VK_FORMAT_R32_UINT; // atomic operations only support 32bit format
  imageInfo.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
  imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
  imageInfo.flags = 0; // Optional

  if (vkCreateImage(device.getLogicalDevice(), &imageInfo, nullptr,
                    &_xzDebugImage) != VK_SUCCESS) {
    throw std::runtime_error("failed to create depth rt!");
  }
  VkMemoryRequirements memRequirements1;
  vkGetImageMemoryRequirements(device.getLogicalDevice(), _xzDebugImage,
                               &memRequirements1);

  VkMemoryAllocateInfo allocInfo{};
  allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  allocInfo.allocationSize = memRequirements1.size;
  allocInfo.memoryTypeIndex = device.findMemoryType(
      memRequirements1.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

  VkDeviceMemory textureImageMemory;
  if (vkAllocateMemory(device.getLogicalDevice(), &allocInfo, nullptr,
                       &textureImageMemory) != VK_SUCCESS) {
    throw std::runtime_error("failed to allocate image memory!");
  }

  vkBindImageMemory(device.getLogicalDevice(), _xzDebugImage,
                    textureImageMemory, 0);

  VkImageViewCreateInfo createInfo{};
  createInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  createInfo.image = _xzDebugImage;
  createInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
  createInfo.format = VK_FORMAT_R32_UINT; // TODO: could be different with value
                                          // specified in VkImageCreateInfo?
  createInfo.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
  createInfo.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
  createInfo.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
  createInfo.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
  createInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  createInfo.subresourceRange.baseMipLevel = 0;
  createInfo.subresourceRange.levelCount = 1;
  createInfo.subresourceRange.baseArrayLayer = 0;
  createInfo.subresourceRange.layerCount = 1;
  createInfo.flags = 0;

  if (vkCreateImageView(device.getLogicalDevice(), &createInfo, nullptr,
                        &_xzDebugImageView) != VK_SUCCESS) {
    throw std::runtime_error("failed to create xzdebug image views!");
  }

  {
    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.extent.width = tileXCount;
    imageInfo.extent.height = tileYCount;
    imageInfo.extent.depth = 1;
    imageInfo.mipLevels = 1; // texturedata._mipmapLevelCount;
    imageInfo.arrayLayers = 1;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL; // TODO: switch to linear with
    // initiallayout=preinitialized?
    imageInfo.initialLayout =
        VK_IMAGE_LAYOUT_UNDEFINED; // VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    imageInfo.format = VK_FORMAT_R32G32B32A32_SFLOAT; // atomic operations only
                                                      // support 32bit format
    imageInfo.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.flags = 0; // Optional

    if (vkCreateImage(device.getLogicalDevice(), &imageInfo, nullptr,
                      &_traditionalCullDebugImage) != VK_SUCCESS) {
      throw std::runtime_error("failed to create depth rt!");
    }
    VkMemoryRequirements memRequirements1;
    vkGetImageMemoryRequirements(device.getLogicalDevice(),
                                 _traditionalCullDebugImage, &memRequirements1);

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memRequirements1.size;
    allocInfo.memoryTypeIndex = device.findMemoryType(
        memRequirements1.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    VkDeviceMemory textureImageMemory;
    if (vkAllocateMemory(device.getLogicalDevice(), &allocInfo, nullptr,
                         &textureImageMemory) != VK_SUCCESS) {
      throw std::runtime_error("failed to allocate image memory!");
    }

    vkBindImageMemory(device.getLogicalDevice(), _traditionalCullDebugImage,
                      textureImageMemory, 0);

    VkImageViewCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    createInfo.image = _traditionalCullDebugImage;
    createInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    createInfo.format =
        VK_FORMAT_R32G32B32A32_SFLOAT; // TODO: could be different with value
                                       // specified in VkImageCreateInfo?
    createInfo.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
    createInfo.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
    createInfo.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
    createInfo.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
    createInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    createInfo.subresourceRange.baseMipLevel = 0;
    createInfo.subresourceRange.levelCount = 1;
    createInfo.subresourceRange.baseArrayLayer = 0;
    createInfo.subresourceRange.layerCount = 1;
    createInfo.flags = 0;

    if (vkCreateImageView(device.getLogicalDevice(), &createInfo, nullptr,
                          &_traditionalCullDebugImageView) != VK_SUCCESS) {
      throw std::runtime_error("failed to create xzdebug image views!");
    }
  }

  //----------descriptors--------

  VkDescriptorSetLayoutBinding cullParamsBinding = {};
  cullParamsBinding.binding = 0;
  cullParamsBinding.descriptorCount = 1;
  cullParamsBinding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
  cullParamsBinding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

  VkDescriptorSetLayoutBinding pointLightCullingDataBinding = {};
  pointLightCullingDataBinding.binding = 1;
  pointLightCullingDataBinding.descriptorCount = 1;
  pointLightCullingDataBinding.descriptorType =
      VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  pointLightCullingDataBinding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

  VkDescriptorSetLayoutBinding depthTextureBinding = {};
  depthTextureBinding.binding = 2;
  depthTextureBinding.descriptorCount = 1;
  depthTextureBinding.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  depthTextureBinding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

  VkDescriptorSetLayoutBinding xzRangeBinding = {};
  xzRangeBinding.binding = 3;
  xzRangeBinding.descriptorCount = 1;
  xzRangeBinding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  xzRangeBinding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

  VkDescriptorSetLayoutBinding debugViewBinding = {};
  debugViewBinding.binding = 4;
  debugViewBinding.descriptorCount = 1;
  debugViewBinding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
  debugViewBinding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

  VkDescriptorSetLayoutBinding lightIndicesBinding = {};
  lightIndicesBinding.binding = 5;
  lightIndicesBinding.descriptorCount = 1;
  lightIndicesBinding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  lightIndicesBinding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

  VkDescriptorSetLayoutBinding traditionalViewBinding = {};
  traditionalViewBinding.binding = 6;
  traditionalViewBinding.descriptorCount = 1;
  traditionalViewBinding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
  traditionalViewBinding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

  VkDescriptorSetLayoutBinding lightIndicesTransparentBinding = {};
  lightIndicesTransparentBinding.binding = 7;
  lightIndicesTransparentBinding.descriptorCount = 1;
  lightIndicesTransparentBinding.descriptorType =
      VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  lightIndicesTransparentBinding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

  // Spot light bindings 8-11 (parallel to point bindings 1/3/5/7).
  VkDescriptorSetLayoutBinding spotCullDataBinding = {};
  spotCullDataBinding.binding = 8;
  spotCullDataBinding.descriptorCount = 1;
  spotCullDataBinding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  spotCullDataBinding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

  VkDescriptorSetLayoutBinding spotXZRangeBinding = {};
  spotXZRangeBinding.binding = 9;
  spotXZRangeBinding.descriptorCount = 1;
  spotXZRangeBinding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  spotXZRangeBinding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

  VkDescriptorSetLayoutBinding spotLightIndicesBinding = {};
  spotLightIndicesBinding.binding = 10;
  spotLightIndicesBinding.descriptorCount = 1;
  spotLightIndicesBinding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  spotLightIndicesBinding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

  VkDescriptorSetLayoutBinding spotLightIndicesTransparentBinding = {};
  spotLightIndicesTransparentBinding.binding = 11;
  spotLightIndicesTransparentBinding.descriptorCount = 1;
  spotLightIndicesTransparentBinding.descriptorType =
      VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  spotLightIndicesTransparentBinding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

  VkDescriptorSetLayoutBinding bindings[] = {
                                             cullParamsBinding,
                                             pointLightCullingDataBinding,
                                             depthTextureBinding,
                                             xzRangeBinding,
                                             debugViewBinding,
                                             lightIndicesBinding,
                                             lightIndicesTransparentBinding,
                                             traditionalViewBinding,
                                             spotCullDataBinding,
                                             spotXZRangeBinding,
                                             spotLightIndicesBinding,
                                             spotLightIndicesTransparentBinding};

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
                              &coarseCullSetLayout);

  uint32_t poolMultiplier = gpuScene.framesInFlight;
  std::vector<VkDescriptorPoolSize> sizes = {
      {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1 * poolMultiplier},
      {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 8 * poolMultiplier},
      {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1 * poolMultiplier},
      {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2 * poolMultiplier}
  };

  VkDescriptorPoolCreateInfo pool_info = {};
  pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT_EXT;
  pool_info.maxSets = 10;
  pool_info.poolSizeCount = (uint32_t)sizes.size();
  pool_info.pPoolSizes = sizes.data();

  vkCreateDescriptorPool(device.getLogicalDevice(), &pool_info, nullptr,
                         &coarseCullDescriptorPool);

  std::vector<VkDescriptorSetLayout> coarseCullSetLayouts(gpuScene.framesInFlight, coarseCullSetLayout);

  VkDescriptorSetAllocateInfo descAllocInfo = {};
  descAllocInfo.pNext = nullptr;
  descAllocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  // using the pool we just set
  descAllocInfo.descriptorPool = coarseCullDescriptorPool;
  descAllocInfo.descriptorSetCount = gpuScene.framesInFlight;
  descAllocInfo.pSetLayouts = coarseCullSetLayouts.data();

  coarseCullDescriptorSet.resize(gpuScene.framesInFlight);
  vkAllocateDescriptorSets(device.getLogicalDevice(), &descAllocInfo,
                           coarseCullDescriptorSet.data());

for(uint32_t i=0;i<gpuScene.framesInFlight;++i)
{
  VkDescriptorBufferInfo binfo1;
  // it will be the camera buffer
  binfo1.buffer = gpuScene.cullParamsBuffers[i]; // TODO: per-frame LightCuller descriptor sets
  // at 0 offset
  binfo1.offset = 0;
  // of the size of a camera data struct
  binfo1.range = sizeof(GpuScene::GPUCullParams);

  VkWriteDescriptorSet setWrite1 = {};
  setWrite1.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  setWrite1.pNext = nullptr;

  // we are going to write into binding number 0
  setWrite1.dstBinding = 0;
  // of the global descriptor
  setWrite1.dstSet = coarseCullDescriptorSet[i];

  setWrite1.descriptorCount = 1;
  setWrite1.dstArrayElement = 0;
  // and the type is uniform buffer
  setWrite1.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
  setWrite1.pBufferInfo = &binfo1;

  VkDescriptorBufferInfo binfo2;
  // it will be the camera buffer
  binfo2.buffer = _pointLightCullingDataBuffer;
  // at 0 offset
  binfo2.offset = 0;
  // of the size of a camera data struct
  binfo2.range = gpuScene._pointLights.size() * sizeof(vec4) * 2;

  VkWriteDescriptorSet setWrite2 = {};
  setWrite2.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  setWrite2.pNext = nullptr;

  // we are going to write into binding number 0
  setWrite2.dstBinding = 1;
  // of the global descriptor
  setWrite2.dstSet = coarseCullDescriptorSet[i];

  setWrite2.descriptorCount = 1;
  setWrite2.dstArrayElement = 0;
  // and the type is uniform buffer
  setWrite2.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  setWrite2.pBufferInfo = &binfo2;

  VkDescriptorImageInfo depthImageInfo{};
  depthImageInfo.imageView = device.getWindowDepthOnlyImageView(i);
  depthImageInfo.imageLayout =
      VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL;
  VkWriteDescriptorSet setWriteDepth;
  setWriteDepth.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  setWriteDepth.pNext = nullptr;
  setWriteDepth.dstBinding = 2;
  setWriteDepth.dstSet = coarseCullDescriptorSet[i];
  setWriteDepth.dstArrayElement = 0;
  setWriteDepth.descriptorCount = 1;
  setWriteDepth.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  setWriteDepth.pImageInfo = &depthImageInfo;

  VkDescriptorBufferInfo binfo4;
  // it will be the camera buffer
  binfo4.buffer = _xzRangeBuffer[i];
  // at 0 offset
  binfo4.offset = 0;
  // of the size of a camera data struct
  binfo4.range = gpuScene._pointLights.size() * sizeof(uint16_t) * 4;
  VkWriteDescriptorSet setWrite4 = {};
  setWrite4.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  setWrite4.pNext = nullptr;

  // we are going to write into binding number 0
  setWrite4.dstBinding = 3;
  // of the global descriptor
  setWrite4.dstSet = coarseCullDescriptorSet[i];

  setWrite4.descriptorCount = 1;
  setWrite4.dstArrayElement = 0;
  // and the type is uniform buffer
  setWrite4.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  setWrite4.pBufferInfo = &binfo4;

  VkDescriptorImageInfo debugImageInfo{};
  debugImageInfo.imageView = _xzDebugImageView;
  debugImageInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
  VkWriteDescriptorSet setWriteDebug;
  setWriteDebug.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  setWriteDebug.pNext = nullptr;
  setWriteDebug.dstBinding = 4;
  setWriteDebug.dstSet = coarseCullDescriptorSet[i];
  setWriteDebug.dstArrayElement = 0;
  setWriteDebug.descriptorCount = 1;
  setWriteDebug.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
  setWriteDebug.pImageInfo = &debugImageInfo;

  VkDescriptorBufferInfo binfo6;
  // it will be the camera buffer
  binfo6.buffer = _lightIndicesBuffer[i];
  // at 0 offset
  binfo6.offset = 0;
  // of the size of a camera data struct
  binfo6.range =
      tileXCount * tileYCount * sizeof(uint32_t) * MAX_LIGHTS_PER_TILE;
  VkWriteDescriptorSet setWrite6 = {};
  setWrite6.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  setWrite6.pNext = nullptr;
  setWrite6.dstBinding = 5;
  // of the global descriptor
  setWrite6.dstSet = coarseCullDescriptorSet[i];

  setWrite6.descriptorCount = 1;
  setWrite6.dstArrayElement = 0;
  // and the type is uniform buffer
  setWrite6.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  setWrite6.pBufferInfo = &binfo6;

  VkDescriptorBufferInfo binfo7;
  // it will be the camera buffer
  binfo7.buffer = _lightIndicesTransparentBuffer[i];
  // at 0 offset
  binfo7.offset = 0;
  // of the size of a camera data struct
  binfo7.range =
      tileXCount * tileYCount * sizeof(uint32_t) * MAX_LIGHTS_PER_TILE;
  VkWriteDescriptorSet setWrite7 = {};
  setWrite7.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  setWrite7.pNext = nullptr;
  setWrite7.dstBinding = 7;
  // of the global descriptor
  setWrite7.dstSet = coarseCullDescriptorSet[i];

  setWrite7.descriptorCount = 1;
  setWrite7.dstArrayElement = 0;
  // and the type is uniform buffer
  setWrite7.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  setWrite7.pBufferInfo = &binfo6;

  VkDescriptorImageInfo tradtionalImageInfo{};
  tradtionalImageInfo.imageView = _traditionalCullDebugImageView;
  tradtionalImageInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
  VkWriteDescriptorSet setTraditionalDebug;
  setTraditionalDebug.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  setTraditionalDebug.pNext = nullptr;
  setTraditionalDebug.dstBinding = 6;
  setTraditionalDebug.dstSet = coarseCullDescriptorSet[i];
  setTraditionalDebug.dstArrayElement = 0;
  setTraditionalDebug.descriptorCount = 1;
  setTraditionalDebug.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
  setTraditionalDebug.pImageInfo = &tradtionalImageInfo;

  // ---- Spot light writes (bindings 8-11) ----
  VkDescriptorBufferInfo binfoSpotData;
  binfoSpotData.buffer = _spotLightCullingDataBuffer;
  binfoSpotData.offset = 0;
  binfoSpotData.range = spotCullDataStride * spotCount;
  VkWriteDescriptorSet setWriteSpotData = {};
  setWriteSpotData.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  setWriteSpotData.dstBinding = 8;
  setWriteSpotData.dstSet = coarseCullDescriptorSet[i];
  setWriteSpotData.descriptorCount = 1;
  setWriteSpotData.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  setWriteSpotData.pBufferInfo = &binfoSpotData;

  VkDescriptorBufferInfo binfoSpotXZ;
  binfoSpotXZ.buffer = _spotXZRangeBuffer[i];
  binfoSpotXZ.offset = 0;
  binfoSpotXZ.range = spotCount * sizeof(uint16_t) * 4;
  VkWriteDescriptorSet setWriteSpotXZ = {};
  setWriteSpotXZ.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  setWriteSpotXZ.dstBinding = 9;
  setWriteSpotXZ.dstSet = coarseCullDescriptorSet[i];
  setWriteSpotXZ.descriptorCount = 1;
  setWriteSpotXZ.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  setWriteSpotXZ.pBufferInfo = &binfoSpotXZ;

  VkDescriptorBufferInfo binfoSpotIdx;
  binfoSpotIdx.buffer = _spotLightIndicesBuffer[i];
  binfoSpotIdx.offset = 0;
  binfoSpotIdx.range = tileXCount * tileYCount * sizeof(uint32_t) * MAX_LIGHTS_PER_TILE;
  VkWriteDescriptorSet setWriteSpotIdx = {};
  setWriteSpotIdx.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  setWriteSpotIdx.dstBinding = 10;
  setWriteSpotIdx.dstSet = coarseCullDescriptorSet[i];
  setWriteSpotIdx.descriptorCount = 1;
  setWriteSpotIdx.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  setWriteSpotIdx.pBufferInfo = &binfoSpotIdx;

  VkDescriptorBufferInfo binfoSpotIdxT;
  binfoSpotIdxT.buffer = _spotLightIndicesTransparentBuffer[i];
  binfoSpotIdxT.offset = 0;
  binfoSpotIdxT.range = tileXCount * tileYCount * sizeof(uint32_t) * MAX_LIGHTS_PER_TILE;
  VkWriteDescriptorSet setWriteSpotIdxT = {};
  setWriteSpotIdxT.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  setWriteSpotIdxT.dstBinding = 11;
  setWriteSpotIdxT.dstSet = coarseCullDescriptorSet[i];
  setWriteSpotIdxT.descriptorCount = 1;
  setWriteSpotIdxT.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  setWriteSpotIdxT.pBufferInfo = &binfoSpotIdxT;

  std::array<VkWriteDescriptorSet, 12> writes = {
      setWrite1, setWrite2, setWrite4,          setWriteDebug,
      setWriteDepth, setWrite6, setWrite7, setTraditionalDebug,
      setWriteSpotData, setWriteSpotXZ, setWriteSpotIdx, setWriteSpotIdxT};

  vkUpdateDescriptorSets(device.getLogicalDevice(), writes.size(),
                         writes.data(), 0, nullptr);
} // end per-frame loop

  // deferred lighting descriptor set (per-frame)

  // Allocate spot view-proj matrix buffer (static, one entry per spot light).
  // Filled once here from SpotLight::spotLightData which is populated during
  // scene loading (before LightCuller::InitRHI is first called).
  {
    constexpr VkDeviceSize vpSize = SPOT_SHADOW_MAX_COUNT * sizeof(mat4);
    VkBufferCreateInfo vpInfo{};
    vpInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    vpInfo.size = vpSize;
    vpInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    vpInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    vkCreateBuffer(device.getLogicalDevice(), &vpInfo, nullptr, &_spotViewProjBuffer);
    VkMemoryRequirements vpReqs;
    vkGetBufferMemoryRequirements(device.getLogicalDevice(), _spotViewProjBuffer, &vpReqs);
    VkMemoryAllocateInfo vpAlloc{};
    vpAlloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    vpAlloc.allocationSize = vpReqs.size;
    vpAlloc.memoryTypeIndex = device.findMemoryType(vpReqs.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    vkAllocateMemory(device.getLogicalDevice(), &vpAlloc, nullptr, &_spotViewProjMemory);
    vkBindBufferMemory(device.getLogicalDevice(), _spotViewProjBuffer, _spotViewProjMemory, 0);
    void *vpData;
    vkMapMemory(device.getLogicalDevice(), _spotViewProjMemory, 0, vpSize, 0, &vpData);
    memset(vpData, 0, vpSize);
    uint32_t toUpload = std::min((uint32_t)SpotLight::spotLightData.size(), (uint32_t)SPOT_SHADOW_MAX_COUNT);
    for (uint32_t i = 0; i < toUpload; ++i)
      memcpy((mat4 *)vpData + i, SpotLight::spotLightData[i].viewProjMatrix.value_ptr(), sizeof(mat4));
    vkUnmapMemory(device.getLogicalDevice(), _spotViewProjMemory);
  }

  for (uint32_t f = 0; f < gpuScene.framesInFlight; ++f) {
  VkDescriptorBufferInfo binfoDeferredLightData;
  binfoDeferredLightData.buffer = _pointLightCullingDataBuffer;
  binfoDeferredLightData.offset = 0;
  binfoDeferredLightData.range = gpuScene._pointLights.size() * sizeof(vec4) * 2;

  VkWriteDescriptorSet setWrite_Deferredlighting = {};
  setWrite_Deferredlighting.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  setWrite_Deferredlighting.pNext = nullptr;
  setWrite_Deferredlighting.dstBinding = 8;
  setWrite_Deferredlighting.dstSet = gpuScene.deferredLightingDescriptorSet[f];
  setWrite_Deferredlighting.descriptorCount = 1;
  setWrite_Deferredlighting.dstArrayElement = 0;
  setWrite_Deferredlighting.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  setWrite_Deferredlighting.pBufferInfo = &binfoDeferredLightData;

  VkDescriptorBufferInfo binfoDeferredLightIndices;
  binfoDeferredLightIndices.buffer = _lightIndicesBuffer[f];
  binfoDeferredLightIndices.offset = 0;
  binfoDeferredLightIndices.range =
      tileXCount * tileYCount * sizeof(uint32_t) * MAX_LIGHTS_PER_TILE;

  VkWriteDescriptorSet setWriteIndices_deferredlighting = {};
  setWriteIndices_deferredlighting.sType =
      VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  setWriteIndices_deferredlighting.pNext = nullptr;
  setWriteIndices_deferredlighting.dstBinding = 9;
  setWriteIndices_deferredlighting.dstSet =
      gpuScene.deferredLightingDescriptorSet[f];
  setWriteIndices_deferredlighting.descriptorCount = 1;
  setWriteIndices_deferredlighting.dstArrayElement = 0;
  setWriteIndices_deferredlighting.descriptorType =
      VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  setWriteIndices_deferredlighting.pBufferInfo = &binfoDeferredLightIndices;

  // Spot data + indices for the deferred lighting fullscreen pass.
  VkDescriptorBufferInfo binfoDeferredSpotData;
  binfoDeferredSpotData.buffer = _spotLightCullingDataBuffer;
  binfoDeferredSpotData.offset = 0;
  binfoDeferredSpotData.range = spotCullDataStride * spotCount;
  VkWriteDescriptorSet setWriteSpotData_deferred = {};
  setWriteSpotData_deferred.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  setWriteSpotData_deferred.dstBinding = 11;
  setWriteSpotData_deferred.dstSet = gpuScene.deferredLightingDescriptorSet[f];
  setWriteSpotData_deferred.descriptorCount = 1;
  setWriteSpotData_deferred.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  setWriteSpotData_deferred.pBufferInfo = &binfoDeferredSpotData;

  VkDescriptorBufferInfo binfoDeferredSpotIdx;
  binfoDeferredSpotIdx.buffer = _spotLightIndicesBuffer[f];
  binfoDeferredSpotIdx.offset = 0;
  binfoDeferredSpotIdx.range = tileXCount * tileYCount * sizeof(uint32_t) * MAX_LIGHTS_PER_TILE;
  VkWriteDescriptorSet setWriteSpotIdx_deferred = {};
  setWriteSpotIdx_deferred.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  setWriteSpotIdx_deferred.dstBinding = 12;
  setWriteSpotIdx_deferred.dstSet = gpuScene.deferredLightingDescriptorSet[f];
  setWriteSpotIdx_deferred.descriptorCount = 1;
  setWriteSpotIdx_deferred.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  setWriteSpotIdx_deferred.pBufferInfo = &binfoDeferredSpotIdx;

  // Binding 15: spot view-proj matrices for shadow lookup.
  VkDescriptorBufferInfo binfoSpotVP;
  binfoSpotVP.buffer = _spotViewProjBuffer;
  binfoSpotVP.offset = 0;
  binfoSpotVP.range = SPOT_SHADOW_MAX_COUNT * sizeof(mat4);
  VkWriteDescriptorSet setWriteSpotVP = {};
  setWriteSpotVP.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  setWriteSpotVP.dstBinding = 15;
  setWriteSpotVP.dstSet = gpuScene.deferredLightingDescriptorSet[f];
  setWriteSpotVP.descriptorCount = 1;
  setWriteSpotVP.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  setWriteSpotVP.pBufferInfo = &binfoSpotVP;

  std::array<VkWriteDescriptorSet, 5> writes_deferredlighting = {
      setWriteIndices_deferredlighting, setWrite_Deferredlighting,
      setWriteSpotData_deferred, setWriteSpotIdx_deferred,
      setWriteSpotVP};

  vkUpdateDescriptorSets(device.getLogicalDevice(),
                         writes_deferredlighting.size(),
                         writes_deferredlighting.data(), 0, nullptr);
  } // end deferred lighting per-frame loop

  // pipeline related
  auto computeShaderCode = readFile(
      (gpuScene.RootPath() / "shaders/CoarseCull.cs.spv").generic_string());
  VkShaderModule computeShaderModule =
      gpuScene.createShaderModule(computeShaderCode);
  VkPipelineShaderStageCreateInfo computeStageInfo{};
  computeStageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  computeStageInfo.stage = VK_SHADER_STAGE_COMPUTE_BIT;
  computeStageInfo.module = computeShaderModule;
  computeStageInfo.pName = "CoarseCull";

  auto traditionalCullingShaderCode =
      readFile((gpuScene.RootPath() / "shaders/TraditionalCull.cs.spv")
                   .generic_string());
  VkShaderModule traditionalCullingShaderModule =
      gpuScene.createShaderModule(traditionalCullingShaderCode);
  VkPipelineShaderStageCreateInfo traditionalCullingStageInfo{};
  traditionalCullingStageInfo.sType =
      VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  traditionalCullingStageInfo.stage = VK_SHADER_STAGE_COMPUTE_BIT;
  traditionalCullingStageInfo.module = traditionalCullingShaderModule;
  traditionalCullingStageInfo.pName = "TraditionalCull";

  auto clearIndicesShaderCode = readFile(
      (gpuScene.RootPath() / "shaders/ClearIndices.cs.spv").generic_string());
  VkShaderModule clearIndicesShaderModule =
      gpuScene.createShaderModule(clearIndicesShaderCode);
  VkPipelineShaderStageCreateInfo clearIndicesStageInfo{};
  clearIndicesStageInfo.sType =
      VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  clearIndicesStageInfo.stage = VK_SHADER_STAGE_COMPUTE_BIT;
  clearIndicesStageInfo.module = clearIndicesShaderModule;
  clearIndicesStageInfo.pName = "ClearLightIndices";

  auto clearDebugViewShaderCode = readFile(
      (gpuScene.RootPath() / "shaders/ClearDebugView.cs.spv").generic_string());
  VkShaderModule clearDebugViewModule =
      gpuScene.createShaderModule(clearDebugViewShaderCode);
  VkPipelineShaderStageCreateInfo clearDebugViewStageInfo{};
  clearDebugViewStageInfo.sType =
      VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  clearDebugViewStageInfo.stage = VK_SHADER_STAGE_COMPUTE_BIT;
  clearDebugViewStageInfo.module = clearDebugViewModule;
  clearDebugViewStageInfo.pName = "ClearDebugView";

  // Spot light kernel modules. Share coarseCullPipelineLayout because the
  // descriptor set layout already covers bindings 8-11.
  auto coarseCullSpotCode = readFile(
      (gpuScene.RootPath() / "shaders/CoarseCullSpot.cs.spv").generic_string());
  VkShaderModule coarseCullSpotModule = gpuScene.createShaderModule(coarseCullSpotCode);
  VkPipelineShaderStageCreateInfo coarseCullSpotStage{};
  coarseCullSpotStage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  coarseCullSpotStage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
  coarseCullSpotStage.module = coarseCullSpotModule;
  coarseCullSpotStage.pName = "CoarseCullSpot";

  auto traditionalCullSpotCode = readFile(
      (gpuScene.RootPath() / "shaders/TraditionalCullSpot.cs.spv").generic_string());
  VkShaderModule traditionalCullSpotModule = gpuScene.createShaderModule(traditionalCullSpotCode);
  VkPipelineShaderStageCreateInfo traditionalCullSpotStage{};
  traditionalCullSpotStage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  traditionalCullSpotStage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
  traditionalCullSpotStage.module = traditionalCullSpotModule;
  traditionalCullSpotStage.pName = "TraditionalCullSpot";

  auto clearIndicesSpotCode = readFile(
      (gpuScene.RootPath() / "shaders/ClearLightIndicesSpot.cs.spv").generic_string());
  VkShaderModule clearIndicesSpotModule = gpuScene.createShaderModule(clearIndicesSpotCode);
  VkPipelineShaderStageCreateInfo clearIndicesSpotStage{};
  clearIndicesSpotStage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  clearIndicesSpotStage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
  clearIndicesSpotStage.module = clearIndicesSpotModule;
  clearIndicesSpotStage.pName = "ClearLightIndicesSpot";


  VkDescriptorSetLayout coarseCullPipelineSetLayoutInfos[] = {gpuScene.globalSetLayout, coarseCullSetLayout};

  VkPipelineLayoutCreateInfo coarseCullPipelineLayoutInfo{};
  coarseCullPipelineLayoutInfo.sType =
      VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  coarseCullPipelineLayoutInfo.setLayoutCount = 2;
  coarseCullPipelineLayoutInfo.pSetLayouts = coarseCullPipelineSetLayoutInfos;
  coarseCullPipelineLayoutInfo.pushConstantRangeCount = 0;

  if (vkCreatePipelineLayout(device.getLogicalDevice(),
                             &coarseCullPipelineLayoutInfo, nullptr,
                             &coarseCullPipelineLayout) != VK_SUCCESS) {
    throw std::runtime_error("failed to create drawcluster pipeline layout!");
  }
  VkComputePipelineCreateInfo computePipelineCreateInfo{};
  computePipelineCreateInfo.sType =
      VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
  computePipelineCreateInfo.layout = coarseCullPipelineLayout;
  computePipelineCreateInfo.stage = computeStageInfo;
  computePipelineCreateInfo.basePipelineHandle = VK_NULL_HANDLE;

  VkComputePipelineCreateInfo clearDebugViewPipelineCreateInfo{};
  clearDebugViewPipelineCreateInfo.sType =
      VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
  clearDebugViewPipelineCreateInfo.layout = coarseCullPipelineLayout;
  clearDebugViewPipelineCreateInfo.stage = clearDebugViewStageInfo;
  clearDebugViewPipelineCreateInfo.basePipelineHandle = VK_NULL_HANDLE;

  VkComputePipelineCreateInfo traditionalCullPipelineCreateInfo{};
  traditionalCullPipelineCreateInfo.sType =
      VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
  traditionalCullPipelineCreateInfo.layout = coarseCullPipelineLayout;
  traditionalCullPipelineCreateInfo.stage = traditionalCullingStageInfo;
  traditionalCullPipelineCreateInfo.basePipelineHandle = VK_NULL_HANDLE;

  VkComputePipelineCreateInfo clearIndicesPipelineCreateInfo{};
  clearIndicesPipelineCreateInfo.sType =
      VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
  clearIndicesPipelineCreateInfo.layout = coarseCullPipelineLayout;
  clearIndicesPipelineCreateInfo.stage = clearIndicesStageInfo;
  clearIndicesPipelineCreateInfo.basePipelineHandle = VK_NULL_HANDLE;

  vkCreateComputePipelines(device.getLogicalDevice(), VK_NULL_HANDLE, 1,
                           &computePipelineCreateInfo, nullptr,
                           &coarseCullPipeline);
  vkCreateComputePipelines(device.getLogicalDevice(), VK_NULL_HANDLE, 1,
                           &clearDebugViewPipelineCreateInfo, nullptr,
                           &clearDebugViewPipeline);
  vkCreateComputePipelines(device.getLogicalDevice(), VK_NULL_HANDLE, 1,
                           &traditionalCullPipelineCreateInfo, nullptr,
                           &traditionalCullPipeline);
  vkCreateComputePipelines(device.getLogicalDevice(), VK_NULL_HANDLE, 1,
                           &clearIndicesPipelineCreateInfo, nullptr,
                           &clearIndicesPipeline);

  // Spot light pipelines (share coarseCullPipelineLayout).
  VkComputePipelineCreateInfo spotCoarsePCI{};
  spotCoarsePCI.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
  spotCoarsePCI.layout = coarseCullPipelineLayout;
  spotCoarsePCI.stage = coarseCullSpotStage;
  VkComputePipelineCreateInfo spotTradPCI = spotCoarsePCI;
  spotTradPCI.stage = traditionalCullSpotStage;
  VkComputePipelineCreateInfo spotClearPCI = spotCoarsePCI;
  spotClearPCI.stage = clearIndicesSpotStage;
  vkCreateComputePipelines(device.getLogicalDevice(), VK_NULL_HANDLE, 1,
                           &spotCoarsePCI, nullptr, &coarseCullSpotPipeline);
  vkCreateComputePipelines(device.getLogicalDevice(), VK_NULL_HANDLE, 1,
                           &spotTradPCI, nullptr, &traditionalCullSpotPipeline);
  vkCreateComputePipelines(device.getLogicalDevice(), VK_NULL_HANDLE, 1,
                           &spotClearPCI, nullptr, &clearIndicesSpotPipeline);
}

void LightCuller::ClusterLightForScreen(VkCommandBuffer &commandBuffer,
                                        const VulkanDevice &device,
                                        const GpuScene &gpuScene,
                                        uint32_t screen_width,
                                        uint32_t screen_heigt) {

  uint32_t currentFrame = gpuScene.currentFrame;
  VkDescriptorSet clusterLightCullSets[] = {gpuScene.globalDescriptorSets[currentFrame], coarseCullDescriptorSet[currentFrame]};
  vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                          coarseCullPipelineLayout, 0, 2,
                          clusterLightCullSets, 0, nullptr);
  vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                    clearDebugViewPipeline);
  vkCmdDispatch(commandBuffer,
                (screen_width + DEFAULT_LIGHT_CULLING_TILE_SIZE - 1) /
                    DEFAULT_LIGHT_CULLING_TILE_SIZE,
                (screen_heigt + DEFAULT_LIGHT_CULLING_TILE_SIZE - 1) /
                    DEFAULT_LIGHT_CULLING_TILE_SIZE,
                1);
  // synchronise
  VkMemoryBarrier2 memoryBarrier = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
      .pNext = nullptr,
      .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT_KHR,
      .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
      .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT_KHR,
      .dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT};

  VkDependencyInfo dependencyInfo = {
      .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
      .pNext = nullptr,
      .memoryBarrierCount = 1,
      .pMemoryBarriers = &memoryBarrier,
  };
  vkCmdPipelineBarrier2(commandBuffer, &dependencyInfo);

  vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                    coarseCullPipeline);
  // the descriptor (VkDescriptorSet 0x5b0fb50000002faf[], binding 5, index 0)
  // has VkImageView 0x41586f0000002fac[] with format of VK_FORMAT_R8_UINT which
  // is missing VK_FORMAT_FEATURE_STORAGE_IMAGE_ATOMIC_BIT in its features
  // (VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_BIT|VK_FORMAT_FEATURE_2_STORAGE_IMAGE_BIT|VK_FORMAT_FEATURE_2_COLOR_ATTACHMENT_BIT|VK_FORMAT_FEATURE_2_BLIT_SRC_BIT|VK_FORMAT_FEATURE_2_BLIT_DST_BIT|VK_FORMAT_FEATURE_2_TRANSFER_SRC_BIT|VK_FORMAT_FEATURE_2_TRANSFER_DST_BIT|VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_FILTER_MINMAX_BIT|VK_FORMAT_FEATURE_2_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR|VK_FORMAT_FEATURE_2_STORAGE_READ_WITHOUT_FORMAT_BIT|VK_FORMAT_FEATURE_2_STORAGE_WRITE_WITHOUT_FORMAT_BIT|VK_FORMAT_FEATURE_2_HOST_IMAGE_TRANSFER_BIT_EXT).
  // The Vulkan spec states: If a VkImageView is accessed using atomic
  // operations as a result of this command, then the image view's format
  // features must contain VK_FORMAT_FEATURE_STORAGE_IMAGE_ATOMIC_BIT
  vkCmdDispatch(commandBuffer, (gpuScene._pointLights.size() + 127) / 128, 1,
                1);
  // synchronise
  vkCmdPipelineBarrier2(commandBuffer, &dependencyInfo);

  vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                    clearDebugViewPipeline);
  vkCmdDispatch(commandBuffer,
                (screen_width + DEFAULT_LIGHT_CULLING_TILE_SIZE - 1) /
                    DEFAULT_LIGHT_CULLING_TILE_SIZE,
                (screen_heigt + DEFAULT_LIGHT_CULLING_TILE_SIZE - 1) /
                    DEFAULT_LIGHT_CULLING_TILE_SIZE,
                1);
  // synchronise
  vkCmdPipelineBarrier2(commandBuffer, &dependencyInfo);

  vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                    clearIndicesPipeline);
  vkCmdDispatch(commandBuffer,
                (screen_width + DEFAULT_LIGHT_CULLING_TILE_SIZE - 1) /
                    DEFAULT_LIGHT_CULLING_TILE_SIZE,
                (screen_heigt + DEFAULT_LIGHT_CULLING_TILE_SIZE - 1) /
                    DEFAULT_LIGHT_CULLING_TILE_SIZE,
                1);
  vkCmdPipelineBarrier2(commandBuffer, &dependencyInfo);

  {
    // Memory barrier between earlier compute writes (clearIndices, etc.) and
    // traditionalCull's depth sampling. The depth image's layout transition to
    // DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL is owned by GpuScene (via the
    // pre-cluster transition that runs before this function), so we don't
    // re-transition it here. Re-issuing an image-layout barrier with a stale
    // oldLayout would race with the validator's per-aspect tracking.
    VkMemoryBarrier memBarrier{};
    memBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    memBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    memBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(commandBuffer,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                         1, &memBarrier, 0, nullptr, 0, nullptr);
  }

  vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                    traditionalCullPipeline);
  vkCmdDispatch(commandBuffer,
                (screen_width + DEFAULT_LIGHT_CULLING_TILE_SIZE - 1) /
                    DEFAULT_LIGHT_CULLING_TILE_SIZE,
                (screen_heigt + DEFAULT_LIGHT_CULLING_TILE_SIZE - 1) /
                    DEFAULT_LIGHT_CULLING_TILE_SIZE,
                1);

  // ---- Spot light culling chain ----
  // Same pattern as point: clear -> coarse -> traditional, each separated by
  // a compute->compute SSBO write barrier. Spot kernels share the same
  // descriptor set / pipeline layout so we just rebind the pipeline.
  uint32_t spotCount = (uint32_t)gpuScene._spotLights.size();
  if (spotCount > 0)
  {
    vkCmdPipelineBarrier2(commandBuffer, &dependencyInfo);

    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                      clearIndicesSpotPipeline);
    vkCmdDispatch(commandBuffer,
                  (screen_width + DEFAULT_LIGHT_CULLING_TILE_SIZE - 1) /
                      DEFAULT_LIGHT_CULLING_TILE_SIZE,
                  (screen_heigt + DEFAULT_LIGHT_CULLING_TILE_SIZE - 1) /
                      DEFAULT_LIGHT_CULLING_TILE_SIZE,
                  1);
    vkCmdPipelineBarrier2(commandBuffer, &dependencyInfo);

    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                      coarseCullSpotPipeline);
    vkCmdDispatch(commandBuffer, (spotCount + 127) / 128, 1, 1);
    vkCmdPipelineBarrier2(commandBuffer, &dependencyInfo);

    // compute->compute SSBO write->read barrier mirrors the point chain.
    {
      VkMemoryBarrier memBarrier{};
      memBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
      memBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
      memBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
      vkCmdPipelineBarrier(commandBuffer,
                           VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                           VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                           1, &memBarrier, 0, nullptr, 0, nullptr);
    }

    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                      traditionalCullSpotPipeline);
    vkCmdDispatch(commandBuffer,
                  (screen_width + DEFAULT_LIGHT_CULLING_TILE_SIZE - 1) /
                      DEFAULT_LIGHT_CULLING_TILE_SIZE,
                  (screen_heigt + DEFAULT_LIGHT_CULLING_TILE_SIZE - 1) /
                      DEFAULT_LIGHT_CULLING_TILE_SIZE,
                  1);
  }
}

LightCuller::LightCuller() {}
