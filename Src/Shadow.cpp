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
	imageInfo.format = SHADOW_FORMAT;
	imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;

	imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    	imageInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
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
    		imageviewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
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
	shadowDepthAttachment.initialLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
	shadowDepthAttachment.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

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
	
	if(vkCreateRenderPass(device.getLogicalDevice(),&renderPassInfo,nullptr,&_shadowPass)!=VK_SUCCESS)
	{
		throw std::runtime_error("failed to create shadow render pass");
	}
	
	//pipeline
	/*
	auto vsShaderCode = readFile((gpuScene.RootPath()/"shaders/drawcluster.vs.spv").generic_string());
	auto psShaderCode = readFile((gpuScene.RootPath()/"shaders/drawcluster.forward.ps.spv").generic_string());

	VkShaderModule vertShaderModule = gpuScene.createShaderModule(vsShaderCode);
	VkShaderModule fragShaderModule = gpuScene.createShaderModule(psShaderCode);
//TODO: merge with GpuScene::createShaderModule
	VkPipelineShaderStageCreateInfo drawclusterVSShaderStageInfo{};
    	drawclusterVSShaderStageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    	drawclusterVSShaderStageInfo.stage = VK_SHADER_STAGE_VERTEX_BIT;
    	drawclusterVSShaderStageInfo.module = vertShaderModule;
    	drawclusterVSShaderStageInfo.pName = "RenderSceneVS";

	VkPipelineShaderStageCreateInfo drawclusterForwardPSShaderStageInfo{};
    drawclusterForwardPSShaderStageInfo.sType =
        VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    drawclusterForwardPSShaderStageInfo.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    drawclusterForwardPSShaderStageInfo.module = drawclusterForwardPSShaderModule;//TODO: 这几个module应该可以合并，在dxc中添加适当的参数？
    drawclusterForwardPSShaderStageInfo.pName = "RenderSceneForwardPS";

    VkPipelineShaderStageCreateInfo drawclusterForwardStages[] = { drawclusterVSShaderStageInfo,
                                                      drawclusterForwardPSShaderStageInfo };

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
    drawclusterForwardPipelineInfo.layout = gpuScene.getDrawclusterPipelineLayout();
    drawclusterForwardPipelineInfo.renderPass = _forwardLightingPass;
    drawclusterForwardPipelineInfo.subpass = 0;
    drawclusterForwardPipelineInfo.basePipelineHandle = VK_NULL_HANDLE;
    drawclusterForwardPipelineInfo.pDepthStencilState = &depthStencilState;
*/

	
}

static bool vulkanResourceCreated = false;
void Shadow::RenderShadowMap(VkCommandBuffer& commandBuffer,const GpuScene& gpuScene,const VulkanDevice& device)
{
	if(!vulkanResourceCreated)
	{
		InitRHI(gpuScene);
	}

	for(int i=0;i<SHADOW_CASCADE_COUNT;++i)
	{
		//update projection matrix
		//TODO: dynamic offset
	void* data1;
    vkMapMemory(device.getLogicalDevice(), gpuScene.uniformBufferMemory, 0, sizeof(FrameData), 0,
        &data1);
 
    void* data = ((char*)data1) + sizeof(FrameConstants);
    memcpy(data, transpose(maincamera->getProjectMatrix()).value_ptr(),
        (size_t)sizeof(mat4));
    memcpy(((mat4*)data) + 1, transpose(maincamera->getObjectToCamera()).value_ptr(), (size_t)sizeof(mat4));

    	vkUnmapMemory(device.getLogicalDevice(),uniformBufferMemory);
   

	VkRenderPassBeginInfo forwardPassInfo{};
        forwardPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        forwardPassInfo.renderPass = _shadowPass;
        forwardPassInfo.framebuffer = _shadowFrameBuffers[i];
        forwardPassInfo.renderArea.offset = { 0, 0 };
        forwardPassInfo.renderArea.extent = {_shadowResolution,_shadowResolution};
        forwardPassInfo.clearValueCount = 0;//static_cast<uint32_t>(clearValues.size());
        //forwardPassInfo.pClearValues = clearValues.data();

	vkCmdBeginRenderPass(commandBuffer,&forwardPassInfo,VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,gpuScene.drawclusterForwardPipeline);
	vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, gpuScene.drawclusterPipelineLayout, 0, 1, &gpuScene.applDescriptorSet, 0, nullptr);


    for (int i=0;i<gpuScene.applMesh->_opaqueChunkCount;++i)
    {
	PerObjPush perobj = { .matindex = gpuScene.m_Chunks[i].materialIndex };
        
        
        {
            if (maincamera->getFrustum().FrustumCull(gpuScene.m_Chunks[i].boundingBox))
            {
                //debug_frustum_cull[i] = true;
                continue;
            }
        }

        vkCmdPushConstants(commandBuffer, gpuScene.drawclusterPipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(perobj), &perobj);
        vkCmdDrawIndexed(commandBuffer, gpuScene.m_Chunks[i].indexCount, 1, gpuScene.m_Chunks[i].indexBegin, 0, 0);

    }

	}
 
}
