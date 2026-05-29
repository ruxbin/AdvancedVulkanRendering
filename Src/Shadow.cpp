#include "Shadow.h"
#include "Common.h"
#include "Light.h"

void Shadow::CreateShadowSlices(const VulkanDevice &device) {
  VkImageCreateInfo imageInfo{};
  imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  imageInfo.imageType = VK_IMAGE_TYPE_2D;
  imageInfo.extent.width = _shadowResolution;
  imageInfo.extent.height = _shadowResolution;
  imageInfo.extent.depth = 1;
  imageInfo.mipLevels = 1;
  imageInfo.arrayLayers = SHADOW_CASCADE_COUNT;
  imageInfo.format = SHADOW_FORMAT; // VK_FORMAT_D32_SFLOAT_S8_UINT;
  imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
  imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  imageInfo.usage =
      VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
  imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
  imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

  if (vkCreateImage(device.getLogicalDevice(), &imageInfo, nullptr,
                    &_shadowMaps) != VK_SUCCESS) {
    throw std::runtime_error("failed to create shadowmap images");
  }
  VkMemoryRequirements memRequirements;
  vkGetImageMemoryRequirements(device.getLogicalDevice(), _shadowMaps,
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

  vkBindImageMemory(device.getLogicalDevice(), _shadowMaps, textureImageMemory,
                    0);

  for (int i = 0; i < SHADOW_CASCADE_COUNT; ++i) {
    VkImageViewCreateInfo imageviewInfo{};
    imageviewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    imageviewInfo.image = _shadowMaps;
    imageviewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;

    imageviewInfo.format = SHADOW_FORMAT;
    imageviewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    imageviewInfo.subresourceRange.baseMipLevel = 0;
    imageviewInfo.subresourceRange.levelCount =
        1; // texturedata._mipmapLevelCount;
    imageviewInfo.subresourceRange.baseArrayLayer = i;
    imageviewInfo.subresourceRange.layerCount = 1;

    // VkImageView imageView;
    if (vkCreateImageView(device.getLogicalDevice(), &imageviewInfo, nullptr,
                          &_shadowSliceViews[i]) != VK_SUCCESS) {
      throw std::runtime_error("failed to create texture image view!");
    }
  }

  VkImageViewCreateInfo imageviewInfo{};
  imageviewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  imageviewInfo.image = _shadowMaps;
  imageviewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;

  imageviewInfo.format = SHADOW_FORMAT;
  imageviewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
  imageviewInfo.subresourceRange.baseMipLevel = 0;
  imageviewInfo.subresourceRange.levelCount =
      1; // texturedata._mipmapLevelCount;
  imageviewInfo.subresourceRange.baseArrayLayer = 0;
  imageviewInfo.subresourceRange.layerCount = SHADOW_CASCADE_COUNT;
  if (vkCreateImageView(device.getLogicalDevice(), &imageviewInfo, nullptr,
                        &_shadowSliceViewFull) != VK_SUCCESS) {
    throw std::runtime_error("failed to create texture image view!");
  }
}

void Shadow::UpdateShadowMatrices(const GpuScene &gpuScene) {

  const float minDistance = 0.0001f;
  std::array<float, SHADOW_CASCADE_COUNT> cascadeSplits = {
      3.0f / gpuScene.maincamera->Far(), 10.0f / gpuScene.maincamera->Far(),
      50.0f / gpuScene.maincamera->Far()};

  const vec3 *frustumCornersWS = gpuScene.maincamera->_frustumCorners;

  for (int i = 0; i < SHADOW_CASCADE_COUNT; ++i) {
    float prevSplitDist = i == 0 ? minDistance : cascadeSplits[i - 1];

    vec3 sliceCornersWS[8];

    for (int j = 0; j < 4; ++j) {
      vec3 cornerRay = frustumCornersWS[j + 4] - frustumCornersWS[j];
      vec3 nearCornerRay = cornerRay * prevSplitDist;
      vec3 farCornerRay = cornerRay * cascadeSplits[i];
      sliceCornersWS[j + 4] = frustumCornersWS[j] + farCornerRay;
      sliceCornersWS[j] = frustumCornersWS[j] + nearCornerRay;
    }

    vec3 frustumCenter(0.0f);

    for (int j = 0; j < 8; ++j)
      frustumCenter += sliceCornersWS[j];

    frustumCenter /= 8.0f;

    // Calculate the radius of the frustum slice bounding sphere
    float sphereRadius = 0.0f;

    for (int i = 0; i < 8; ++i) {
      float dist = (sliceCornersWS[i] - frustumCenter).length();
      sphereRadius = sphereRadius > dist ? sphereRadius : dist;
    }

    // Change radius in 0.5f steps to prevent flickering
    sphereRadius = ceil(sphereRadius * 2.0f) / 2.0f;

    vec3 maxExtents(sphereRadius);
    vec3 minExtents = -maxExtents;

    vec3 cascadeExtents = maxExtents - minExtents;

    // Get position of the shadow camera
    // float3 shadowCameraPos = frustumCenter + _sunDirection * minExtents.z;
    vec3 shadowCameraPos =
        frustumCenter + gpuScene.frameConstants.sunDirection * 100.0f;

    _shadowProjectionMatrices[i] =
        orthographic(cascadeExtents.x, cascadeExtents.y, 0.f, 200.f, 0, 0);
    _shadowViewMatrices[i] =
        invLookAt(shadowCameraPos, vec3(0, 1, 0),
                  gpuScene.frameConstants.sunDirection * -1.f);

    {
      // Create the rounding matrix, by projecting the world-space origin and
      // determining the fractional offset in texel space
      mat4 shadowMatrix = transpose(_shadowProjectionMatrices[i]) *
                          transpose(_shadowViewMatrices[i]);
      ;
      vec4 shadowOrigin(0.0f, 0.0f, 0.0f, 1.0f);
      shadowOrigin = shadowMatrix * shadowOrigin;
      shadowOrigin *= (_shadowResolution / 2.0f);

      vec4 roundedOrigin = round(shadowOrigin);
      vec4 roundOffset = roundedOrigin - shadowOrigin;
      roundOffset = roundOffset * (2.0f / _shadowResolution);
      roundOffset.z = 0.0f;

      _shadowProjectionMatrices[i] =
          orthographic(cascadeExtents.x, cascadeExtents.y, 0.f, 200.f,
                       roundOffset.x, roundOffset.y);
    }

    // Per-cascade world-space frustum for GPU shadow culling.
    // Sun-view basis matches invLookAt above: _z = normalize(-sunDir),
    // _x = normalize(up × _z), _y = _z × _x. View-space OBB is
    // [-r,+r] × [-r,+r] × [0, 200]. Map each corner back to world,
    // then build 6 Planes whose normals point inward.
    {
      const vec3 sunDir = gpuScene.frameConstants.sunDirection;
      vec3 z_axis = normalize(sunDir * -1.0f);
      vec3 x_axis = normalize(vec3(0, 1, 0).cross(z_axis));
      vec3 y_axis = z_axis.cross(x_axis);

      vec3 cornersWS[8];
      for (int k = 0; k < 8; ++k) {
        float vx = (k & 1) ? sphereRadius : -sphereRadius;
        float vy = (k & 2) ? sphereRadius : -sphereRadius;
        float vz = (k & 4) ? 200.0f : 0.0f;
        cornersWS[k] = shadowCameraPos + x_axis * vx + y_axis * vy + z_axis * vz;
      }

      _cascadeFrustums[i] = {
          {cornersWS[0], cornersWS[2], cornersWS[4]}, // -X plane
          {cornersWS[1], cornersWS[5], cornersWS[3]}, // +X plane
          {cornersWS[0], cornersWS[4], cornersWS[1]}, // -Y plane
          {cornersWS[2], cornersWS[3], cornersWS[6]}, // +Y plane
          {cornersWS[0], cornersWS[1], cornersWS[2]}, // -Z (sun-near) plane
          {cornersWS[4], cornersWS[6], cornersWS[5]}, // +Z (sun-far)  plane
      };
      _cascadeSphereRadii[i] = sphereRadius;
    }
  }
}

static bool vulkanResourceCreated = false;

void Shadow::InitRHI(const VulkanDevice &device, const GpuScene &gpuScene) {
  CreateShadowSlices(device);

  {

    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.anisotropyEnable = VK_FALSE;

    samplerInfo.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
    samplerInfo.unnormalizedCoordinates = VK_FALSE;
    samplerInfo.compareEnable = VK_TRUE;
    samplerInfo.compareOp = VK_COMPARE_OP_LESS;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;

    if (vkCreateSampler(device.getLogicalDevice(), &samplerInfo, nullptr,
                        &_shadowMapSampler) != VK_SUCCESS) {
      throw std::runtime_error("failed to create texture sampler!");
    }
  }

  {
    VkDescriptorImageInfo imageinfo{};
    imageinfo.imageView = _shadowSliceViewFull;
    imageinfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkDescriptorImageInfo samplerinfo;
    samplerinfo.sampler = _shadowMapSampler;

    for (uint32_t f = 0; f < gpuScene.framesInFlight; ++f) {
    VkWriteDescriptorSet setWriteTexture = {};
    setWriteTexture.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    setWriteTexture.pNext = nullptr;
    setWriteTexture.dstBinding = 6;
    setWriteTexture.dstSet = gpuScene.deferredLightingDescriptorSet[f];
    setWriteTexture.dstArrayElement = 0;
    setWriteTexture.descriptorCount = 1;
    setWriteTexture.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    setWriteTexture.pImageInfo = &imageinfo;

    VkWriteDescriptorSet setSampler = {};
    setSampler.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    setSampler.dstBinding = 7;
    setSampler.pNext = nullptr;
    setSampler.dstSet = gpuScene.deferredLightingDescriptorSet[f];
    setSampler.dstArrayElement = 0;
    setSampler.descriptorCount = 1;
    setSampler.pImageInfo = &samplerinfo;
    setSampler.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;

    std::array<VkWriteDescriptorSet, 2> writes = {setWriteTexture, setSampler};

    vkUpdateDescriptorSets(device.getLogicalDevice(), writes.size(),
                           writes.data(), 0, nullptr);
    }
  }

  VkAttachmentDescription shadowDepthAttachment = {};
  shadowDepthAttachment.format = SHADOW_FORMAT;
  shadowDepthAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
  shadowDepthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  shadowDepthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  shadowDepthAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  shadowDepthAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  shadowDepthAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  shadowDepthAttachment.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

  VkAttachmentReference depthAttachmentRef{};
  depthAttachmentRef.attachment = 0;
  depthAttachmentRef.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

  VkSubpassDescription subpass{};
  subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  subpass.colorAttachmentCount = 0;
  subpass.pColorAttachments = nullptr;
  subpass.pDepthStencilAttachment = &depthAttachmentRef;

  VkSubpassDependency dependencies[2] = {};
  // Entry: wait for previous frame's shader read before depth write
  dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
  dependencies[0].dstSubpass = 0;
  dependencies[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
  dependencies[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
  dependencies[0].dstStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
  dependencies[0].dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
  // Exit: depth write → shader read (finalLayout transition to SHADER_READ_ONLY)
  dependencies[1].srcSubpass = 0;
  dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
  dependencies[1].srcStageMask = VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
  dependencies[1].srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
  dependencies[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
  dependencies[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

  std::array<VkAttachmentDescription, 1> attachments = {shadowDepthAttachment};
  VkRenderPassCreateInfo renderPassInfo{};
  renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
  renderPassInfo.attachmentCount = static_cast<uint32_t>(attachments.size());
  renderPassInfo.pAttachments = attachments.data();
  renderPassInfo.subpassCount = 1;
  renderPassInfo.pSubpasses = &subpass;
  renderPassInfo.dependencyCount = 2;
  renderPassInfo.pDependencies = dependencies;

  if (vkCreateRenderPass(device.getLogicalDevice(), &renderPassInfo, nullptr,
                         &_shadowPass) != VK_SUCCESS) {
    throw std::runtime_error("failed to create shadow render pass");
  }

  // Pipeline layout: Set 0 = globalSetLayout (cam UBO, vertex stage),
  //                  Set 1 = applSetLayout (materials, textures, chunkIndex)
  {
    VkDescriptorSetLayout shadowLayouts[] = {gpuScene.globalSetLayout,
                                             gpuScene.applSetLayout};
    VkPushConstantRange shadowPushConstant = {
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
        .offset = 0,
        .size = sizeof(PerObjPush)};
    VkPipelineLayoutCreateInfo shadowLayoutInfo{};
    shadowLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    shadowLayoutInfo.setLayoutCount = 2;
    shadowLayoutInfo.pSetLayouts = shadowLayouts;
    shadowLayoutInfo.pushConstantRangeCount = 1;
    shadowLayoutInfo.pPushConstantRanges = &shadowPushConstant;
    if (vkCreatePipelineLayout(device.getLogicalDevice(), &shadowLayoutInfo,
                               nullptr,
                               &_shadowPassPipelineLayout) != VK_SUCCESS) {
      throw std::runtime_error("failed to create shadow pass pipeline layout!");
    }
  }

  // pipeline

 auto vsShaderShadowCode = readFile(
      (gpuScene.RootPath() / "shaders/drawclusterShadow.vs.spv").generic_string());
  
  auto drawClusterPSShaderCodeDepthOnly =
      readFile((gpuScene.RootPath() / "shaders/drawcluster.shadow.indirect.ps.spv")
                   .generic_string());

  VkShaderModule vertShaderModule = gpuScene.createShaderModule(vsShaderShadowCode);
  VkShaderModule drawclusterPSShaderModuleDepthOnly =
      gpuScene.createShaderModule(drawClusterPSShaderCodeDepthOnly);
  // TODO: merge with GpuScene::createShaderModule
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

  VkPipelineShaderStageCreateInfo drawclusterVSShaderStageInfo{};
  drawclusterVSShaderStageInfo.sType =
      VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  drawclusterVSShaderStageInfo.stage = VK_SHADER_STAGE_VERTEX_BIT;
  drawclusterVSShaderStageInfo.module = vertShaderModule;
  drawclusterVSShaderStageInfo.pName = "RenderSceneVSShadow";

  VkPipelineShaderStageCreateInfo
      drawclusterPSShaderStageInfoAlphaMaskDepthOnly{};
  drawclusterPSShaderStageInfoAlphaMaskDepthOnly.sType =
      VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  drawclusterPSShaderStageInfoAlphaMaskDepthOnly.stage =
      VK_SHADER_STAGE_FRAGMENT_BIT;
  drawclusterPSShaderStageInfoAlphaMaskDepthOnly.module =
      drawclusterPSShaderModuleDepthOnly;
  drawclusterPSShaderStageInfoAlphaMaskDepthOnly.pName = "RenderSceneShadowDepthIndirect";
  drawclusterPSShaderStageInfoAlphaMaskDepthOnly.pSpecializationInfo =
      &specializationInfo;

  VkPipelineShaderStageCreateInfo drawclusterShaderStagesDepthOnly[] = {
      drawclusterVSShaderStageInfo};

  VkPipelineShaderStageCreateInfo drawclusterShaderStagesAlphaMaskDepthOnly[] =
      {drawclusterVSShaderStageInfo,
       drawclusterPSShaderStageInfoAlphaMaskDepthOnly};

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

  constexpr VkVertexInputAttributeDescription drawclusterInputAttributes[] = {
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

  VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
  inputAssembly.sType =
      VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
  inputAssembly.topology =
      VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; // change to strip
  inputAssembly.primitiveRestartEnable = VK_FALSE;

  VkViewport viewport{};
  viewport.x = 0.0f;
  viewport.y = 0.0f;
  viewport.width = (float)_shadowResolution;
  viewport.height = (float)_shadowResolution;
  viewport.minDepth = 0.0f;
  viewport.maxDepth = 1.0f;

  VkRect2D scissor{};
  scissor.offset = {0, 0};
  scissor.extent = {_shadowResolution, _shadowResolution};

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
  rasterizer.depthBiasEnable = VK_TRUE;
  rasterizer.depthBiasConstantFactor = 1.25f;
  rasterizer.depthBiasSlopeFactor = 1.75f;
  rasterizer.depthBiasClamp = 0.0f;

  VkPipelineMultisampleStateCreateInfo multisampling{};
  multisampling.sType =
      VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
  multisampling.sampleShadingEnable = VK_FALSE;
  multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

  VkPipelineColorBlendStateCreateInfo colorBlendingAlpha{};
  colorBlendingAlpha.sType =
      VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
  colorBlendingAlpha.logicOpEnable = VK_FALSE;
  colorBlendingAlpha.logicOp = VK_LOGIC_OP_COPY;
  colorBlendingAlpha.attachmentCount = 0;

  VkPipelineDepthStencilStateCreateInfo depthStencilState{};
  depthStencilState.sType =
      VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
  depthStencilState.depthWriteEnable = VK_TRUE;
  depthStencilState.depthTestEnable = VK_TRUE;
  depthStencilState.stencilTestEnable = VK_FALSE;
  depthStencilState.depthCompareOp = VK_COMPARE_OP_LESS;
  depthStencilState.depthBoundsTestEnable = VK_FALSE;

  VkGraphicsPipelineCreateInfo drawclusterForwardPipelineInfo{};
  drawclusterForwardPipelineInfo.sType =
      VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
  drawclusterForwardPipelineInfo.stageCount = 1;
  drawclusterForwardPipelineInfo.pStages = drawclusterShaderStagesDepthOnly;
  drawclusterForwardPipelineInfo.pVertexInputState =
      &drawclusterVertexInputInfo;
  drawclusterForwardPipelineInfo.pInputAssemblyState = &inputAssembly;
  drawclusterForwardPipelineInfo.pViewportState = &viewportState;
  drawclusterForwardPipelineInfo.pRasterizationState = &rasterizer;
  drawclusterForwardPipelineInfo.pMultisampleState = &multisampling;
  drawclusterForwardPipelineInfo.pColorBlendState = &colorBlendingAlpha;
  drawclusterForwardPipelineInfo.layout = _shadowPassPipelineLayout;
  drawclusterForwardPipelineInfo.renderPass = _shadowPass;
  drawclusterForwardPipelineInfo.subpass = 0;
  drawclusterForwardPipelineInfo.basePipelineHandle = VK_NULL_HANDLE;
  drawclusterForwardPipelineInfo.pDepthStencilState = &depthStencilState;

  if (vkCreateGraphicsPipelines(device.getLogicalDevice(), VK_NULL_HANDLE, 1,
                                &drawclusterForwardPipelineInfo, nullptr,
                                &_shadowPassPipeline) != VK_SUCCESS) {
    throw std::runtime_error("failed to create shadowpass graphics pipeline!");
  }

  VkGraphicsPipelineCreateInfo drawclusterForwardPipelineInfoAlphaMask{};
  drawclusterForwardPipelineInfoAlphaMask.sType =
      VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
  drawclusterForwardPipelineInfoAlphaMask.stageCount = 2;
  drawclusterForwardPipelineInfoAlphaMask.pStages =
      drawclusterShaderStagesAlphaMaskDepthOnly;
  drawclusterForwardPipelineInfoAlphaMask.pVertexInputState =
      &drawclusterVertexInputInfo;
  drawclusterForwardPipelineInfoAlphaMask.pInputAssemblyState = &inputAssembly;
  drawclusterForwardPipelineInfoAlphaMask.pViewportState = &viewportState;
  drawclusterForwardPipelineInfoAlphaMask.pRasterizationState = &rasterizer;
  drawclusterForwardPipelineInfoAlphaMask.pMultisampleState = &multisampling;
  drawclusterForwardPipelineInfoAlphaMask.pColorBlendState =
      &colorBlendingAlpha;
  drawclusterForwardPipelineInfoAlphaMask.layout = _shadowPassPipelineLayout;
  drawclusterForwardPipelineInfoAlphaMask.renderPass = _shadowPass;
  drawclusterForwardPipelineInfoAlphaMask.subpass = 0;
  drawclusterForwardPipelineInfoAlphaMask.basePipelineHandle = VK_NULL_HANDLE;
  drawclusterForwardPipelineInfoAlphaMask.pDepthStencilState =
      &depthStencilState;

  if (vkCreateGraphicsPipelines(device.getLogicalDevice(), VK_NULL_HANDLE, 1,
                                &drawclusterForwardPipelineInfoAlphaMask,
                                nullptr,
                                &_shadowPassPipelineAlphaMask) != VK_SUCCESS) {
    throw std::runtime_error("failed to create shadowpass graphics pipeline!");
  }

  // the framebuffer
  for (int i = 0; i < SHADOW_CASCADE_COUNT; ++i) {
    std::array<VkImageView, 1> attachments = {_shadowSliceViews[i]};

    VkFramebufferCreateInfo framebufferInfo{};
    framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    framebufferInfo.renderPass = _shadowPass;
    framebufferInfo.attachmentCount = static_cast<uint32_t>(attachments.size());
    ;
    framebufferInfo.pAttachments = attachments.data();
    framebufferInfo.width = _shadowResolution;
    framebufferInfo.height = _shadowResolution;
    framebufferInfo.layers = 1;

    if (vkCreateFramebuffer(device.getLogicalDevice(), &framebufferInfo,
                            nullptr, &_shadowFrameBuffers[i]) != VK_SUCCESS) {
      throw std::runtime_error("failed to create z framebuffer!");
    }
  }

  vkDestroyShaderModule(device.getLogicalDevice(), vertShaderModule, nullptr);
  vkDestroyShaderModule(device.getLogicalDevice(),
                        drawclusterPSShaderModuleDepthOnly, nullptr);
    vulkanResourceCreated = true;

  CreateSpotShadowResources(device, gpuScene);
}


void Shadow::RenderShadowMap(VkCommandBuffer &commandBuffer,
                             const GpuScene &gpuScene,
                             const VulkanDevice &device) {
  if (!vulkanResourceCreated) {
    InitRHI(device, gpuScene);
  }

  if (!_gpuShadowInitialized) {
    InitGPUShadowResources(device, gpuScene);
  }

  uint32_t opaqueCount = gpuScene.applMesh->_opaqueChunkCount;
  uint32_t alphaMaskedCount = gpuScene.applMesh->_alphaMaskedChunkCount;
  uint32_t cascadeMaxChunks = opaqueCount + alphaMaskedCount;

  // Dispatch ShadowCull compute shader
    uint32_t groupx = (cascadeMaxChunks + 127) / 128;
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                      _shadowCullPipeline);
    uint32_t currentFrame = gpuScene.currentFrame;

    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                            _shadowCullPipelineLayout, 0, 1,
                            &_shadowCullDescriptorSets[currentFrame], 0, nullptr);
    vkCmdDispatch(commandBuffer, groupx, 1, 1);

// Barrier: compute → indirect draw
    VkMemoryBarrier2 memBarrier = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
        .pNext = nullptr,
        .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT_KHR,
        .srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT_KHR,
        .dstStageMask = VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT_KHR |
                        VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT_KHR,
        .dstAccessMask = VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT_KHR |
                         VK_ACCESS_2_SHADER_READ_BIT_KHR};

    VkDependencyInfo depInfo = {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .pNext = nullptr,
        .memoryBarrierCount = 1,
        .pMemoryBarriers = &memBarrier,
    };
    vkCmdPipelineBarrier2(commandBuffer, &depInfo);

     VkBuffer vertexBuffers[] = {
        gpuScene.applVertexBuffer, gpuScene.applNormalBuffer,
        gpuScene.applTangentBuffer, gpuScene.applUVBuffer};
    VkDeviceSize vbOffsets[] = {0, 0, 0, 0};

    vkCmdBindVertexBuffers(commandBuffer, 0,
                           sizeof(vertexBuffers) / sizeof(vertexBuffers[0]),
                           vertexBuffers, vbOffsets);
    vkCmdBindIndexBuffer(commandBuffer, gpuScene.applIndexBuffer, 0,
                         VK_INDEX_TYPE_UINT32);

    VkDescriptorSet shadowDescriptorSets[] = {gpuScene.globalDescriptorSets[currentFrame], gpuScene.applDescriptorSets[currentFrame]};
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            _shadowPassPipelineLayout, 0, 2,
                            shadowDescriptorSets, 0, nullptr);

  for (int cascade = 0; cascade < SHADOW_CASCADE_COUNT; ++cascade) {
    // Use camera frustum for shadow culling (conservative)


    
    

    // Begin shadow render pass
    std::array<VkClearValue, 1> clearValues{};
    clearValues[0].depthStencil = {1.0f, 0};

    VkRenderPassBeginInfo passInfo{};
    passInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    passInfo.renderPass = _shadowPass;
    passInfo.framebuffer = _shadowFrameBuffers[cascade];
    passInfo.renderArea.offset = {0, 0};
    passInfo.renderArea.extent = {_shadowResolution, _shadowResolution};
    passInfo.clearValueCount = static_cast<uint32_t>(clearValues.size());
    passInfo.pClearValues = clearValues.data();

    vkCmdBeginRenderPass(commandBuffer, &passInfo, VK_SUBPASS_CONTENTS_INLINE);

   

    // Buffer layout per cascade: [opaque region: cascadeMaxChunks][alphaMask region: cascadeMaxChunks]
    VkDeviceSize stride = sizeof(VkDrawIndexedIndirectCommand);
    uint32_t cascadeBaseOpaque = cascade * cascadeMaxChunks * 2;
    uint32_t cascadeBaseAlphaMask = cascadeBaseOpaque + cascadeMaxChunks;

	
    PerObjPush perobj = {.shadowindex = (uint32_t)cascade};
    vkCmdPushConstants(commandBuffer, _shadowPassPipelineLayout,
                     VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(perobj), &perobj);
  
    // Opaque indirect draw
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      _shadowPassPipeline);
    vkCmdDrawIndexedIndirectCount(commandBuffer,
                                  _shadowDrawParamsBuffers[currentFrame],
                                  cascadeBaseOpaque * stride,
                                  _shadowWriteIndexBuffers[currentFrame],
                                  cascade * 2 * sizeof(uint32_t),
                                  opaqueCount, stride);

    // Alpha-masked indirect draw
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      _shadowPassPipelineAlphaMask);
    vkCmdDrawIndexedIndirectCount(commandBuffer,
                                  _shadowDrawParamsBuffers[currentFrame],
                                  cascadeBaseAlphaMask * stride,
                                  _shadowWriteIndexBuffers[currentFrame],
                                  (cascade * 2 + 1) * sizeof(uint32_t),
                                  alphaMaskedCount, stride);

    vkCmdEndRenderPass(commandBuffer);
  }
}

void Shadow::InitGPUShadowResources(const VulkanDevice &device,
                                     const GpuScene &gpuScene) {
  uint32_t opaqueCount = gpuScene.applMesh->_opaqueChunkCount;
  uint32_t alphaMaskedCount = gpuScene.applMesh->_alphaMaskedChunkCount;
  uint32_t cascadeMaxChunks = opaqueCount + alphaMaskedCount;
  uint32_t totalSlots = SHADOW_CASCADE_COUNT * gpuScene.applMesh->_chunkCount * 2;

  uint32_t framesInFlight = gpuScene.framesInFlight;

  auto createBuf = [&](VkDeviceSize size, VkBufferUsageFlags usage,
                       VkMemoryPropertyFlags props, VkBuffer &buf,
                       VkDeviceMemory &mem) {
    VkBufferCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.size = size;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    vkCreateBuffer(device.getLogicalDevice(), &info, nullptr, &buf);
    VkMemoryRequirements reqs;
    vkGetBufferMemoryRequirements(device.getLogicalDevice(), buf, &reqs);
    VkMemoryAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize = reqs.size;
    alloc.memoryTypeIndex = device.findMemoryType(reqs.memoryTypeBits, props);
    vkAllocateMemory(device.getLogicalDevice(), &alloc, nullptr, &mem);
    vkBindBufferMemory(device.getLogicalDevice(), buf, mem, 0);
  };

  _shadowDrawParamsBuffers.resize(framesInFlight);
  _shadowDrawParamsMemories.resize(framesInFlight);
  _shadowWriteIndexBuffers.resize(framesInFlight);
  _shadowWriteIndexMemories.resize(framesInFlight);
  _shadowCullParamsBuffers.resize(framesInFlight);
  _shadowCullParamsMemories.resize(framesInFlight);

  for (uint32_t f = 0; f < framesInFlight; ++f) {
    createBuf(totalSlots * sizeof(VkDrawIndexedIndirectCommand),
              VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
              VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
              _shadowDrawParamsBuffers[f], _shadowDrawParamsMemories[f]);

    createBuf(SHADOW_CASCADE_COUNT * 2 * sizeof(uint32_t),
              VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
              VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
              _shadowWriteIndexBuffers[f], _shadowWriteIndexMemories[f]);

    createBuf(sizeof(ShadowCullParams),
              VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
              VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
              _shadowCullParamsBuffers[f], _shadowCullParamsMemories[f]);
  }

  // Descriptor set layout (matches shadowcull.hlsl)
  VkDescriptorSetLayoutBinding layoutBindings[] = {
      {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
      {1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
      {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
      {3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
      {4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
      //{5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
  };

  VkDescriptorSetLayoutCreateInfo layoutInfo{};
  layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  layoutInfo.bindingCount = 5;
  layoutInfo.pBindings = layoutBindings;
  vkCreateDescriptorSetLayout(device.getLogicalDevice(), &layoutInfo, nullptr,
                              &_shadowCullSetLayout);

  std::vector<VkDescriptorPoolSize> poolSizes = {
      {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 10 * framesInFlight},
      {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 10 * framesInFlight}};
  VkDescriptorPoolCreateInfo poolInfo{};
  poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  poolInfo.maxSets = 10 * framesInFlight;
  poolInfo.poolSizeCount = poolSizes.size();
  poolInfo.pPoolSizes = poolSizes.data();
  vkCreateDescriptorPool(device.getLogicalDevice(), &poolInfo, nullptr,
                         &_shadowCullDescriptorPool);

  _shadowCullDescriptorSets.resize(framesInFlight);
  std::vector<VkDescriptorSetLayout> layouts(framesInFlight, _shadowCullSetLayout);

  VkDescriptorSetAllocateInfo dsAlloc{};
  dsAlloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  dsAlloc.descriptorPool = _shadowCullDescriptorPool;
  dsAlloc.descriptorSetCount = framesInFlight;
  dsAlloc.pSetLayouts = layouts.data();
  vkAllocateDescriptorSets(device.getLogicalDevice(), &dsAlloc,
                           _shadowCullDescriptorSets.data());

  // Write descriptors for each frame
  for (uint32_t f = 0; f < framesInFlight; ++f) {
    VkDescriptorBufferInfo drawParamsInfo = {_shadowDrawParamsBuffers[f], 0, totalSlots * sizeof(VkDrawIndexedIndirectCommand)};
    VkDescriptorBufferInfo cullParamsInfo = {_shadowCullParamsBuffers[f], 0, sizeof(ShadowCullParams)};
    VkDescriptorBufferInfo meshChunksInfo = {gpuScene.meshChunksBuffer, 0, sizeof(AAPLMeshChunk) * gpuScene.applMesh->_chunkCount};
    VkDescriptorBufferInfo writeIdxInfo = {_shadowWriteIndexBuffers[f], 0, SHADOW_CASCADE_COUNT * 2 * sizeof(uint32_t)};
    VkDescriptorBufferInfo chunkIdxInfo = {gpuScene.chunkIndicesBuffers[f], 0, totalSlots * sizeof(uint32_t)};

    VkWriteDescriptorSet dsWrites[5] = {};
    constexpr int writecount = sizeof(dsWrites) / sizeof(dsWrites[0]);
    std::vector<VkDescriptorBufferInfo> bufInfos;
    bufInfos.push_back(drawParamsInfo);
    bufInfos.push_back(cullParamsInfo);
    bufInfos.push_back(meshChunksInfo);
    bufInfos.push_back(writeIdxInfo);
    bufInfos.push_back(chunkIdxInfo);
    VkDescriptorType types[] = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                                VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                VK_DESCRIPTOR_TYPE_STORAGE_BUFFER};
    for (int b = 0; b < writecount; ++b) {
      dsWrites[b].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      dsWrites[b].dstSet = _shadowCullDescriptorSets[f];
      dsWrites[b].dstBinding = b;
      dsWrites[b].descriptorCount = 1;
      dsWrites[b].descriptorType = types[b];
      dsWrites[b].pBufferInfo = &bufInfos[b];
    }

    vkUpdateDescriptorSets(device.getLogicalDevice(), writecount, dsWrites, 0, nullptr);
  }

  // Compute pipeline
  VkPipelineLayoutCreateInfo pipeLayoutInfo{};
  pipeLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  pipeLayoutInfo.setLayoutCount = 1;
  pipeLayoutInfo.pSetLayouts = &_shadowCullSetLayout;
  vkCreatePipelineLayout(device.getLogicalDevice(), &pipeLayoutInfo, nullptr,
                         &_shadowCullPipelineLayout);

  auto shaderCode = readFile(
      (gpuScene._rootPath / "shaders/shadowcull.cs.spv").generic_string());
  VkShaderModule shaderModule = gpuScene.createShaderModule(shaderCode);

  VkPipelineShaderStageCreateInfo stageInfo{};
  stageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stageInfo.stage = VK_SHADER_STAGE_COMPUTE_BIT;
  stageInfo.module = shaderModule;
  stageInfo.pName = "ShadowCull";

  VkComputePipelineCreateInfo compInfo{};
  compInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
  compInfo.layout = _shadowCullPipelineLayout;
  compInfo.stage = stageInfo;
  vkCreateComputePipelines(device.getLogicalDevice(), VK_NULL_HANDLE, 1,
                           &compInfo, nullptr, &_shadowCullPipeline);

  vkDestroyShaderModule(device.getLogicalDevice(), shaderModule, nullptr);

  _gpuShadowInitialized = true;
  spdlog::info("GPU-driven shadow resources initialized: {} total slots", totalSlots);
}

void Shadow::CreateSpotShadowResources(const VulkanDevice &device,
                                        const GpuScene &gpuScene) {
  // --- 1. Texture array: 256×256 × SPOT_SHADOW_MAX_COUNT ---
  VkImageCreateInfo imageInfo{};
  imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  imageInfo.imageType = VK_IMAGE_TYPE_2D;
  imageInfo.extent = {SPOT_SHADOW_RESOLUTION, SPOT_SHADOW_RESOLUTION, 1};
  imageInfo.mipLevels = 1;
  imageInfo.arrayLayers = SPOT_SHADOW_MAX_COUNT;
  imageInfo.format = SHADOW_FORMAT;
  imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
  imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  imageInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
  imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
  imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  if (vkCreateImage(device.getLogicalDevice(), &imageInfo, nullptr, &_spotShadowMaps) != VK_SUCCESS)
    throw std::runtime_error("failed to create spot shadow map image");

  VkMemoryRequirements memReq;
  vkGetImageMemoryRequirements(device.getLogicalDevice(), _spotShadowMaps, &memReq);
  VkMemoryAllocateInfo allocInfo{};
  allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  allocInfo.allocationSize = memReq.size;
  allocInfo.memoryTypeIndex = device.findMemoryType(memReq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  if (vkAllocateMemory(device.getLogicalDevice(), &allocInfo, nullptr, &_spotShadowMapsMemory) != VK_SUCCESS)
    throw std::runtime_error("failed to allocate spot shadow map memory");
  vkBindImageMemory(device.getLogicalDevice(), _spotShadowMaps, _spotShadowMapsMemory, 0);

  // --- 2. Per-slice views (for framebuffers, one per spot light) ---
  for (int i = 0; i < SPOT_SHADOW_MAX_COUNT; ++i) {
    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = _spotShadowMaps;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = SHADOW_FORMAT;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.baseArrayLayer = i;
    viewInfo.subresourceRange.layerCount = 1;
    if (vkCreateImageView(device.getLogicalDevice(), &viewInfo, nullptr, &_spotShadowSliceViews[i]) != VK_SUCCESS)
      throw std::runtime_error("failed to create spot shadow slice view");
  }

  // --- 3. Full array view (bound as Texture2DArray in deferred lighting) ---
  VkImageViewCreateInfo arrayViewInfo{};
  arrayViewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  arrayViewInfo.image = _spotShadowMaps;
  arrayViewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
  arrayViewInfo.format = SHADOW_FORMAT;
  arrayViewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
  arrayViewInfo.subresourceRange.baseMipLevel = 0;
  arrayViewInfo.subresourceRange.levelCount = 1;
  arrayViewInfo.subresourceRange.baseArrayLayer = 0;
  arrayViewInfo.subresourceRange.layerCount = SPOT_SHADOW_MAX_COUNT;
  if (vkCreateImageView(device.getLogicalDevice(), &arrayViewInfo, nullptr, &_spotShadowArrayView) != VK_SUCCESS)
    throw std::runtime_error("failed to create spot shadow array view");

  // --- 4. Framebuffers (one per slice, reusing the CSM _shadowPass) ---
  for (int i = 0; i < SPOT_SHADOW_MAX_COUNT; ++i) {
    VkFramebufferCreateInfo fbInfo{};
    fbInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fbInfo.renderPass = _shadowPass;
    fbInfo.attachmentCount = 1;
    fbInfo.pAttachments = &_spotShadowSliceViews[i];
    fbInfo.width = SPOT_SHADOW_RESOLUTION;
    fbInfo.height = SPOT_SHADOW_RESOLUTION;
    fbInfo.layers = 1;
    if (vkCreateFramebuffer(device.getLogicalDevice(), &fbInfo, nullptr, &_spotShadowFrameBuffers[i]) != VK_SUCCESS)
      throw std::runtime_error("failed to create spot shadow framebuffer");
  }

  // --- 5. Pipeline layout: globalSetLayout + applSetLayout, push constant = mat4 ---
  VkDescriptorSetLayout setLayouts[] = {gpuScene.globalSetLayout, gpuScene.applSetLayout};
  VkPushConstantRange pcRange = {
      .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
      .offset = 0,
      .size = sizeof(mat4)};
  VkPipelineLayoutCreateInfo layoutInfo{};
  layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  layoutInfo.setLayoutCount = 2;
  layoutInfo.pSetLayouts = setLayouts;
  layoutInfo.pushConstantRangeCount = 1;
  layoutInfo.pPushConstantRanges = &pcRange;
  if (vkCreatePipelineLayout(device.getLogicalDevice(), &layoutInfo, nullptr, &_spotShadowPipelineLayout) != VK_SUCCESS)
    throw std::runtime_error("failed to create spot shadow pipeline layout");

  // --- 6. Load shaders ---
  auto vsCode = readFile((gpuScene.RootPath() / "shaders/drawclusterShadowSpot.vs.spv").generic_string());
  auto psCode = readFile((gpuScene.RootPath() / "shaders/drawcluster.shadow.indirect.ps.spv").generic_string());
  VkShaderModule vsModule = gpuScene.createShaderModule(vsCode);
  VkShaderModule psModule = gpuScene.createShaderModule(psCode);

  VkSpecializationMapEntry specEntry = {0, 0, sizeof(VkBool32)};
  VkBool32 alphaMaskTrue = VK_TRUE;
  VkSpecializationInfo specInfo = {1, &specEntry, sizeof(VkBool32), &alphaMaskTrue};

  VkPipelineShaderStageCreateInfo vsStage{};
  vsStage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  vsStage.stage = VK_SHADER_STAGE_VERTEX_BIT;
  vsStage.module = vsModule;
  vsStage.pName = "RenderSceneVSShadowSpot";

  VkPipelineShaderStageCreateInfo psStage{};
  psStage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  psStage.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  psStage.module = psModule;
  psStage.pName = "RenderSceneShadowDepthIndirect";
  psStage.pSpecializationInfo = &specInfo;

  VkPipelineShaderStageCreateInfo stagesOpaque[] = {vsStage};
  VkPipelineShaderStageCreateInfo stagesAlpha[] = {vsStage, psStage};

  // --- 7. Vertex input state (identical to CSM) ---
  constexpr VkVertexInputBindingDescription bindPos  = {0, sizeof(float)*3, VK_VERTEX_INPUT_RATE_VERTEX};
  constexpr VkVertexInputBindingDescription bindNorm = {1, sizeof(float)*3, VK_VERTEX_INPUT_RATE_VERTEX};
  constexpr VkVertexInputBindingDescription bindTan  = {2, sizeof(float)*3, VK_VERTEX_INPUT_RATE_VERTEX};
  constexpr VkVertexInputBindingDescription bindUV   = {3, sizeof(float)*2, VK_VERTEX_INPUT_RATE_VERTEX};
  constexpr VkVertexInputAttributeDescription attrs[] = {
      {0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0},
      {1, 1, VK_FORMAT_R32G32B32_SFLOAT, 0},
      {2, 2, VK_FORMAT_R32G32B32_SFLOAT, 0},
      {3, 3, VK_FORMAT_R32G32_SFLOAT, 0}};
  constexpr std::array<VkVertexInputBindingDescription, 4> bindings = {bindPos, bindNorm, bindTan, bindUV};

  VkPipelineVertexInputStateCreateInfo vertexInputInfo{};
  vertexInputInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
  vertexInputInfo.vertexBindingDescriptionCount = 4;
  vertexInputInfo.pVertexBindingDescriptions = bindings.data();
  vertexInputInfo.vertexAttributeDescriptionCount = 4;
  vertexInputInfo.pVertexAttributeDescriptions = attrs;

  VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
  inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
  inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

  VkViewport vp{0, 0, (float)SPOT_SHADOW_RESOLUTION, (float)SPOT_SHADOW_RESOLUTION, 0.0f, 1.0f};
  VkRect2D sc{{0, 0}, {SPOT_SHADOW_RESOLUTION, SPOT_SHADOW_RESOLUTION}};
  VkPipelineViewportStateCreateInfo viewportState{};
  viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
  viewportState.viewportCount = 1;
  viewportState.pViewports = &vp;
  viewportState.scissorCount = 1;
  viewportState.pScissors = &sc;

  VkPipelineRasterizationStateCreateInfo rasterizer{};
  rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
  rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
  rasterizer.lineWidth = 1.0f;
  rasterizer.cullMode = VK_CULL_MODE_BACK_BIT;
  rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
  rasterizer.depthBiasEnable = VK_TRUE;
  rasterizer.depthBiasConstantFactor = 1.25f;
  rasterizer.depthBiasSlopeFactor = 1.75f;

  VkPipelineMultisampleStateCreateInfo multisampling{};
  multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
  multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

  VkPipelineColorBlendStateCreateInfo colorBlend{};
  colorBlend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
  colorBlend.attachmentCount = 0;

  VkPipelineDepthStencilStateCreateInfo depthStencil{};
  depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
  depthStencil.depthWriteEnable = VK_TRUE;
  depthStencil.depthTestEnable = VK_TRUE;
  depthStencil.depthCompareOp = VK_COMPARE_OP_LESS;

  VkGraphicsPipelineCreateInfo pipeInfo{};
  pipeInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
  pipeInfo.pVertexInputState = &vertexInputInfo;
  pipeInfo.pInputAssemblyState = &inputAssembly;
  pipeInfo.pViewportState = &viewportState;
  pipeInfo.pRasterizationState = &rasterizer;
  pipeInfo.pMultisampleState = &multisampling;
  pipeInfo.pColorBlendState = &colorBlend;
  pipeInfo.pDepthStencilState = &depthStencil;
  pipeInfo.layout = _spotShadowPipelineLayout;
  pipeInfo.renderPass = _shadowPass;
  pipeInfo.subpass = 0;

  // Opaque: VS only
  pipeInfo.stageCount = 1;
  pipeInfo.pStages = stagesOpaque;
  if (vkCreateGraphicsPipelines(device.getLogicalDevice(), VK_NULL_HANDLE, 1, &pipeInfo, nullptr, &_spotShadowOpaquePipeline) != VK_SUCCESS)
    throw std::runtime_error("failed to create spot shadow opaque pipeline");

  // Alpha-masked: VS + PS
  pipeInfo.stageCount = 2;
  pipeInfo.pStages = stagesAlpha;
  if (vkCreateGraphicsPipelines(device.getLogicalDevice(), VK_NULL_HANDLE, 1, &pipeInfo, nullptr, &_spotShadowAlphaMaskPipeline) != VK_SUCCESS)
    throw std::runtime_error("failed to create spot shadow alpha-mask pipeline");

  vkDestroyShaderModule(device.getLogicalDevice(), vsModule, nullptr);
  vkDestroyShaderModule(device.getLogicalDevice(), psModule, nullptr);

  // --- 8. Write descriptor bindings 13 (spot shadow texture) and 14 (comparison sampler) ---
  for (uint32_t f = 0; f < gpuScene.framesInFlight; ++f) {
    VkDescriptorImageInfo imgInfo{};
    imgInfo.imageView = _spotShadowArrayView;
    imgInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkWriteDescriptorSet wTex{};
    wTex.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    wTex.dstSet = gpuScene.deferredLightingDescriptorSet[f];
    wTex.dstBinding = 13;
    wTex.descriptorCount = 1;
    wTex.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    wTex.pImageInfo = &imgInfo;

    VkDescriptorImageInfo sampInfo{};
    sampInfo.sampler = _shadowMapSampler;

    VkWriteDescriptorSet wSamp{};
    wSamp.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    wSamp.dstSet = gpuScene.deferredLightingDescriptorSet[f];
    wSamp.dstBinding = 14;
    wSamp.descriptorCount = 1;
    wSamp.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    wSamp.pImageInfo = &sampInfo;

    std::array<VkWriteDescriptorSet, 2> writes = {wTex, wSamp};
    vkUpdateDescriptorSets(device.getLogicalDevice(), (uint32_t)writes.size(), writes.data(), 0, nullptr);
  }
}

void Shadow::RenderSpotShadowMaps(VkCommandBuffer &commandBuffer,
                                   const GpuScene &gpuScene,
                                   const VulkanDevice &device) {
  if (!vulkanResourceCreated)
    InitRHI(device, gpuScene);
  if (_spotShadowsRendered)
    return;

  const auto &spotData = SpotLight::spotLightData;
  if (spotData.empty()) {
    _spotShadowsRendered = true;
    return;
  }

  const uint32_t spotCount = std::min((uint32_t)spotData.size(), (uint32_t)SPOT_SHADOW_MAX_COUNT);
  const uint32_t opaqueCount = (uint32_t)gpuScene.applMesh->_opaqueChunkCount;
  const uint32_t alphaMaskedCount = (uint32_t)gpuScene.applMesh->_alphaMaskedChunkCount;
  const AAPLMeshChunk *chunks = gpuScene.m_Chunks;

  VkBuffer vertexBuffers[] = {gpuScene.applVertexBuffer, gpuScene.applNormalBuffer,
                               gpuScene.applTangentBuffer, gpuScene.applUVBuffer};
  VkDeviceSize vbOffsets[] = {0, 0, 0, 0};
  vkCmdBindVertexBuffers(commandBuffer, 0, 4, vertexBuffers, vbOffsets);
  vkCmdBindIndexBuffer(commandBuffer, gpuScene.applIndexBuffer, 0, VK_INDEX_TYPE_UINT32);

  uint32_t cf = gpuScene.currentFrame;
  VkDescriptorSet descSets[] = {gpuScene.globalDescriptorSets[cf], gpuScene.applDescriptorSets[cf]};
  vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                           _spotShadowPipelineLayout, 0, 2, descSets, 0, nullptr);

  for (uint32_t lightIdx = 0; lightIdx < spotCount; ++lightIdx) {
    VkClearValue clearVal{};
    clearVal.depthStencil = {1.0f, 0};

    VkRenderPassBeginInfo passInfo{};
    passInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    passInfo.renderPass = _shadowPass;
    passInfo.framebuffer = _spotShadowFrameBuffers[lightIdx];
    passInfo.renderArea = {{0, 0}, {SPOT_SHADOW_RESOLUTION, SPOT_SHADOW_RESOLUTION}};
    passInfo.clearValueCount = 1;
    passInfo.pClearValues = &clearVal;
    vkCmdBeginRenderPass(commandBuffer, &passInfo, VK_SUBPASS_CONTENTS_INLINE);

    mat4 vp = spotData[lightIdx].viewProjMatrix;
    vkCmdPushConstants(commandBuffer, _spotShadowPipelineLayout,
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(mat4), vp.value_ptr());

    // Opaque geometry
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, _spotShadowOpaquePipeline);
    for (uint32_t c = 0; c < opaqueCount; ++c)
      vkCmdDrawIndexed(commandBuffer, chunks[c].indexCount, 1, chunks[c].indexBegin, 0, c);

    // Alpha-masked geometry
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, _spotShadowAlphaMaskPipeline);
    for (uint32_t c = 0; c < alphaMaskedCount; ++c) {
      uint32_t ci = opaqueCount + c;
      vkCmdDrawIndexed(commandBuffer, chunks[ci].indexCount, 1, chunks[ci].indexBegin, 0, ci);
    }

    vkCmdEndRenderPass(commandBuffer);
  }

  // Transition any unused slices (spotCount..SPOT_SHADOW_MAX_COUNT-1) from
  // UNDEFINED to SHADER_READ_ONLY_OPTIMAL so the descriptor is consistent.
  // Rendered slices were already transitioned by the render pass finalLayout.
  if (spotCount < SPOT_SHADOW_MAX_COUNT) {
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = _spotShadowMaps;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.baseArrayLayer = spotCount;
    barrier.subresourceRange.layerCount = SPOT_SHADOW_MAX_COUNT - spotCount;
    vkCmdPipelineBarrier(commandBuffer,
                         VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &barrier);
  }

  _spotShadowsRendered = true;
  spdlog::info("Spot shadow maps rendered for {} lights", spotCount);
}
