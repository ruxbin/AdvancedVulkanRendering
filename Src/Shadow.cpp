#include "Shadow.h"
#include "Common.h"


void Shadow::CreateShadowSlices(const VulkanDevice& device)
{
	VkImageCreateInfo imageInfo{};
	imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	imageInfo.imageType = VK_IMAGE_TYPE_2D;
	imageInfo.extent.width = _shadowResolution;
	imageInfo.extent.height = _shadowResolution;
	imageInfo.extent.depth = 1;
	imageInfo.mipLevels = 1;
	imageInfo.arrayLayers = SHADOW_CASCADE_COUNT;
    imageInfo.format = SHADOW_FORMAT;// VK_FORMAT_D32_SFLOAT_S8_UINT;
	imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
	imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

	if(vkCreateImage(device.getLogicalDevice(),&imageInfo,nullptr,&_shadowMaps)!=VK_SUCCESS)
	{
		throw std::runtime_error("failed to create shadowmap images");
	}
	VkMemoryRequirements memRequirements;
    	vkGetImageMemoryRequirements(device.getLogicalDevice(), _shadowMaps, &memRequirements);

    	VkMemoryAllocateInfo allocInfo{};
    	allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    	allocInfo.allocationSize = memRequirements.size;
    	allocInfo.memoryTypeIndex = device.findMemoryType(memRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    	VkDeviceMemory textureImageMemory;
    	if (vkAllocateMemory(device.getLogicalDevice(), &allocInfo, nullptr, &textureImageMemory) != VK_SUCCESS) {
        throw std::runtime_error("failed to allocate image memory!");
    	}

    	vkBindImageMemory(device.getLogicalDevice(), _shadowMaps, textureImageMemory, 0);

	for(int i=0;i<SHADOW_CASCADE_COUNT;++i)
	{
		VkImageViewCreateInfo imageviewInfo{};
    		imageviewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    		imageviewInfo.image = _shadowMaps;
    		imageviewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;

    		imageviewInfo.format = SHADOW_FORMAT;
    		imageviewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    		imageviewInfo.subresourceRange.baseMipLevel = 0;
    		imageviewInfo.subresourceRange.levelCount = 1;// texturedata._mipmapLevelCount;
    		imageviewInfo.subresourceRange.baseArrayLayer = i;
    		imageviewInfo.subresourceRange.layerCount = 1;

    	//VkImageView imageView;
    		if (vkCreateImageView(device.getLogicalDevice(), &imageviewInfo, nullptr, &_shadowSliceViews[i]) != VK_SUCCESS) {
        	throw std::runtime_error("failed to create texture image view!");
    		}
	}

}

void Shadow::UpdateShadowMatrices(const GpuScene& gpuScene)
{

    const float minDistance = 0.0001f;
    std::array< float, SHADOW_CASCADE_COUNT> cascadeSplits  = { 3.0f / gpuScene.maincamera->Far(), 10.0f / gpuScene.maincamera->Far(), 50.0f / gpuScene.maincamera->Far() };

    const vec3* frustumCornersWS = gpuScene.maincamera->_frustumCorners;

    for (int i = 0; i < SHADOW_CASCADE_COUNT; ++i)
    {
        float prevSplitDist = i == 0 ? minDistance : cascadeSplits[i - 1];

        vec3 sliceCornersWS[8];

        for (int j = 0; j < 4; ++j)
        {
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

        for (int i = 0; i < 8; ++i)
        {
            float dist = (sliceCornersWS[i] - frustumCenter).length();
            sphereRadius = sphereRadius> dist? sphereRadius:dist;
        }

        // Change radius in 0.5f steps to prevent flickering
        sphereRadius = ceil(sphereRadius * 2.0f) / 2.0f;

        vec3 maxExtents(sphereRadius);
        vec3 minExtents = -maxExtents;

        vec3 cascadeExtents = maxExtents - minExtents;

        // Get position of the shadow camera
        //float3 shadowCameraPos = frustumCenter + _sunDirection * minExtents.z;
        vec3 shadowCameraPos = frustumCenter + gpuScene.frameConstants.sunDirection * 100.0f;

        _shadowProjectionMatrices[i] = orthographic(cascadeExtents.x, cascadeExtents.y, 0.f, 200.f, 0, 0);
        _shadowViewMatrices[i] = invLookAt(shadowCameraPos,vec3(0,1,0),gpuScene.frameConstants.sunDirection*-1.f);

        {
            // Create the rounding matrix, by projecting the world-space origin and determining
            // the fractional offset in texel space
            mat4 shadowMatrix = transpose(_shadowProjectionMatrices[i]) * transpose(_shadowViewMatrices[i]);;
            vec4 shadowOrigin (0.0f, 0.0f, 0.0f, 1.0f );
            shadowOrigin = shadowMatrix * shadowOrigin;
            shadowOrigin *= (_shadowResolution / 2.0f);

            vec4 roundedOrigin = round(shadowOrigin);
            vec4 roundOffset = roundedOrigin - shadowOrigin;
            roundOffset = roundOffset * (2.0f / _shadowResolution);
            roundOffset.z = 0.0f;

            _shadowProjectionMatrices[i] = orthographic(cascadeExtents.x, cascadeExtents.y, 0.f, 200.f, roundOffset.x, roundOffset.y);
        }
    }
}

void Shadow::InitRHI(const VulkanDevice& device,const GpuScene& gpuScene)
{
	CreateShadowSlices(device);
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

    	VkSubpassDependency dependency{};
    	dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    	dependency.dstSubpass = 0;
    	dependency.srcAccessMask = 0;
    	dependency.srcStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;//TODO: is dependency mask right?
    	dependency.dstStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    	dependency.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

    	std::array<VkAttachmentDescription, 1> attachments = { shadowDepthAttachment };
    	VkRenderPassCreateInfo renderPassInfo{};
    	renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    	renderPassInfo.attachmentCount = static_cast<uint32_t>(attachments.size());
    	renderPassInfo.pAttachments = attachments.data();
    	renderPassInfo.subpassCount = 1;
    	renderPassInfo.pSubpasses = &subpass;
    	renderPassInfo.dependencyCount = 1;
    	renderPassInfo.pDependencies = &dependency;
	
	if(vkCreateRenderPass(device.getLogicalDevice(),&renderPassInfo,nullptr,&_shadowPass)!=VK_SUCCESS)
	{
		throw std::runtime_error("failed to create shadow render pass");
	}
	
	//pipeline
	
	auto vsShaderCode = readFile((gpuScene.RootPath()/"shaders/drawcluster.vs.spv").generic_string());
    auto drawClusterPSShaderCodeDepthOnly = readFile((gpuScene.RootPath() / "shaders/drawcluster.depth.ps.spv").generic_string());

	VkShaderModule vertShaderModule = gpuScene.createShaderModule(vsShaderCode);
	VkShaderModule drawclusterPSShaderModuleDepthOnly = gpuScene.createShaderModule(drawClusterPSShaderCodeDepthOnly);
//TODO: merge with GpuScene::createShaderModule
    VkPipelineShaderStageCreateInfo drawclusterPSShaderStageInfoDepthOnly{};
    drawclusterPSShaderStageInfoDepthOnly.sType =
        VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    drawclusterPSShaderStageInfoDepthOnly.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    drawclusterPSShaderStageInfoDepthOnly.module = drawclusterPSShaderModuleDepthOnly;
    drawclusterPSShaderStageInfoDepthOnly.pName = "RenderSceneDepthOnly";

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
    drawclusterVSShaderStageInfo.pName = "RenderSceneVS";

    VkPipelineShaderStageCreateInfo drawclusterPSShaderStageInfoAlphaMaskDepthOnly{};
    drawclusterPSShaderStageInfoAlphaMaskDepthOnly.sType =
        VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    drawclusterPSShaderStageInfoAlphaMaskDepthOnly.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    drawclusterPSShaderStageInfoAlphaMaskDepthOnly.module = drawclusterPSShaderModuleDepthOnly;
    drawclusterPSShaderStageInfoAlphaMaskDepthOnly.pName = "RenderSceneDepthOnly";
    drawclusterPSShaderStageInfoAlphaMaskDepthOnly.pSpecializationInfo = &specializationInfo;



    VkPipelineShaderStageCreateInfo drawclusterShaderStagesDepthOnly[] = { drawclusterVSShaderStageInfo
                                               };

    VkPipelineShaderStageCreateInfo drawclusterShaderStagesAlphaMaskDepthOnly[] = { drawclusterVSShaderStageInfo,
                                              drawclusterPSShaderStageInfoAlphaMaskDepthOnly };

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

    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.sType =
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;//change to strip
    inputAssembly.primitiveRestartEnable = VK_FALSE;

    VkViewport viewport{};
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = (float)_shadowResolution;
    viewport.height = (float)_shadowResolution;
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;

    VkRect2D scissor{};
    scissor.offset = { 0, 0 };
    scissor.extent = { _shadowResolution , _shadowResolution };


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
    drawclusterForwardPipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    drawclusterForwardPipelineInfo.stageCount = 1;
    drawclusterForwardPipelineInfo.pStages = drawclusterShaderStagesDepthOnly;
    drawclusterForwardPipelineInfo.pVertexInputState = &drawclusterVertexInputInfo;
    drawclusterForwardPipelineInfo.pInputAssemblyState = &inputAssembly;
    drawclusterForwardPipelineInfo.pViewportState = &viewportState;
    drawclusterForwardPipelineInfo.pRasterizationState = &rasterizer;
    drawclusterForwardPipelineInfo.pMultisampleState = &multisampling;
    drawclusterForwardPipelineInfo.pColorBlendState = &colorBlendingAlpha;
    drawclusterForwardPipelineInfo.layout = gpuScene.drawclusterPipelineLayout;
    drawclusterForwardPipelineInfo.renderPass = _shadowPass;
    drawclusterForwardPipelineInfo.subpass = 0;
    drawclusterForwardPipelineInfo.basePipelineHandle = VK_NULL_HANDLE;
    drawclusterForwardPipelineInfo.pDepthStencilState = &depthStencilState;

    if (vkCreateGraphicsPipelines(device.getLogicalDevice(), VK_NULL_HANDLE, 1, &drawclusterForwardPipelineInfo,
        nullptr, &_shadowPassPipeline) != VK_SUCCESS) {
        throw std::runtime_error("failed to create shadowpass graphics pipeline!");
    }

    VkGraphicsPipelineCreateInfo drawclusterForwardPipelineInfoAlphaMask{};
    drawclusterForwardPipelineInfoAlphaMask.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    drawclusterForwardPipelineInfoAlphaMask.stageCount = 2;
    drawclusterForwardPipelineInfoAlphaMask.pStages = drawclusterShaderStagesAlphaMaskDepthOnly;
    drawclusterForwardPipelineInfoAlphaMask.pVertexInputState = &drawclusterVertexInputInfo;
    drawclusterForwardPipelineInfoAlphaMask.pInputAssemblyState = &inputAssembly;
    drawclusterForwardPipelineInfoAlphaMask.pViewportState = &viewportState;
    drawclusterForwardPipelineInfoAlphaMask.pRasterizationState = &rasterizer;
    drawclusterForwardPipelineInfoAlphaMask.pMultisampleState = &multisampling;
    drawclusterForwardPipelineInfoAlphaMask.pColorBlendState = &colorBlendingAlpha;
    drawclusterForwardPipelineInfoAlphaMask.layout = gpuScene.drawclusterPipelineLayout;
    drawclusterForwardPipelineInfoAlphaMask.renderPass = _shadowPass;
    drawclusterForwardPipelineInfoAlphaMask.subpass = 0;
    drawclusterForwardPipelineInfoAlphaMask.basePipelineHandle = VK_NULL_HANDLE;
    drawclusterForwardPipelineInfoAlphaMask.pDepthStencilState = &depthStencilState;

    if (vkCreateGraphicsPipelines(device.getLogicalDevice(), VK_NULL_HANDLE, 1, &drawclusterForwardPipelineInfoAlphaMask,
        nullptr, &_shadowPassPipelineAlphaMask) != VK_SUCCESS) {
        throw std::runtime_error("failed to create shadowpass graphics pipeline!");
    }

    //the framebuffer
    for (int i = 0; i < SHADOW_CASCADE_COUNT; ++i)
    {
        std::array<VkImageView, 1> attachments = {
           _shadowSliceViews[i]
        };

        VkFramebufferCreateInfo framebufferInfo{};
        framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        framebufferInfo.renderPass = _shadowPass;
        framebufferInfo.attachmentCount = static_cast<uint32_t>(attachments.size());;
        framebufferInfo.pAttachments = attachments.data();
        framebufferInfo.width = _shadowResolution;
        framebufferInfo.height = _shadowResolution;
        framebufferInfo.layers = 1;

        if (vkCreateFramebuffer(device.getLogicalDevice(), &framebufferInfo, nullptr, &_shadowFrameBuffers[i]) != VK_SUCCESS) {
            throw std::runtime_error("failed to create z framebuffer!");
        }
    }


    vkDestroyShaderModule(device.getLogicalDevice(), vertShaderModule, nullptr);
    vkDestroyShaderModule(device.getLogicalDevice(), drawclusterPSShaderModuleDepthOnly, nullptr);
}

static bool vulkanResourceCreated = false;
void Shadow::RenderShadowMap(VkCommandBuffer& commandBuffer,const GpuScene& gpuScene,const VulkanDevice& device)
{
	if(!vulkanResourceCreated)
	{
		InitRHI(device,gpuScene);
	}

	for(int i=0;i<SHADOW_CASCADE_COUNT;++i)
	{
		//update projection matrix
	void* data1;
    vkMapMemory(device.getLogicalDevice(), gpuScene.uniformBufferMemory, 0, sizeof(FrameData), 0,
        &data1);
 
    void* data = ((char*)data1) + sizeof(FrameConstants);
    memcpy(data, transpose(_shadowProjectionMatrices[i]).value_ptr(),
        (size_t)sizeof(mat4));
    memcpy(((mat4*)data) + 1, transpose(_shadowViewMatrices[i]).value_ptr(), (size_t)sizeof(mat4));

    	vkUnmapMemory(device.getLogicalDevice(), gpuScene.uniformBufferMemory);
   

        std::array<VkClearValue, 1> clearValues{};

        clearValues[0].depthStencil = { 1.0f, 0 };

	VkRenderPassBeginInfo forwardPassInfo{};
        forwardPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        forwardPassInfo.renderPass = _shadowPass;
        forwardPassInfo.framebuffer = _shadowFrameBuffers[i];
        forwardPassInfo.renderArea.offset = { 0, 0 };
        forwardPassInfo.renderArea.extent = {_shadowResolution,_shadowResolution};
        forwardPassInfo.clearValueCount = static_cast<uint32_t>(clearValues.size());
        forwardPassInfo.pClearValues = clearValues.data();

	vkCmdBeginRenderPass(commandBuffer,&forwardPassInfo,VK_SUBPASS_CONTENTS_INLINE);

    VkBuffer vertexBuffers[] = { gpuScene.applVertexBuffer, gpuScene.applNormalBuffer,
                                gpuScene.applTangentBuffer, gpuScene.applUVBuffer,
                                gpuScene.applInstanceBuffer };
    VkDeviceSize offsets[] = { 0, 0, 0, 0, 0 };

    vkCmdBindVertexBuffers(commandBuffer, 0,
        sizeof(vertexBuffers) / sizeof(vertexBuffers[0]),
        vertexBuffers, offsets);
    vkCmdBindIndexBuffer(commandBuffer, gpuScene.applIndexBuffer, 0,
        VK_INDEX_TYPE_UINT32);
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, _shadowPassPipeline);
    uint32_t dynamic_offset = sizeof(mat4)*2*i;
	vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, gpuScene.drawclusterPipelineLayout, 0, 1, &gpuScene.applDescriptorSet, 1, &dynamic_offset);


    for (int i=0;i<gpuScene.applMesh->_opaqueChunkCount;++i)
    {
	PerObjPush perobj = { .matindex = gpuScene.m_Chunks[i].materialIndex };
        
        
        {
            //if (maincamera->getFrustum().FrustumCull(gpuScene.m_Chunks[i].boundingBox))
            {
                //debug_frustum_cull[i] = true;
                //continue;
            }
        }

        //vkCmdPushConstants(commandBuffer, gpuScene.drawclusterPipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(perobj), &perobj);
        vkCmdDrawIndexed(commandBuffer, gpuScene.m_Chunks[i].indexCount, 1, gpuScene.m_Chunks[i].indexBegin, 0, 0);

    }

    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, _shadowPassPipelineAlphaMask);
    for (int i = gpuScene.applMesh->_opaqueChunkCount; i < gpuScene.applMesh->_opaqueChunkCount + gpuScene.applMesh->_alphaMaskedChunkCount ; ++i)
    {
        PerObjPush perobj = { .matindex = gpuScene.m_Chunks[i].materialIndex };


        {
            //if (maincamera->getFrustum().FrustumCull(gpuScene.m_Chunks[i].boundingBox))
            {
                //debug_frustum_cull[i] = true;
                //continue;
            }
        }

        vkCmdPushConstants(commandBuffer, gpuScene.drawclusterPipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(perobj), &perobj);
        vkCmdDrawIndexed(commandBuffer, gpuScene.m_Chunks[i].indexCount, 1, gpuScene.m_Chunks[i].indexBegin, 0, 0);
    }

    vkCmdEndRenderPass(commandBuffer);

	}
 
}
