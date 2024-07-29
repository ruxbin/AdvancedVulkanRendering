#include "Light.h"
#include "GpuScene.h"
#include <math.h>

static VkBuffer sphereBuffer = nullptr;
static VkBuffer sphereIndexBuffer = nullptr;

VkBuffer PointLight::pointLightDynamicUniformBuffer = nullptr;
VkPipelineLayout PointLight::drawPointLightPipelineLayout = nullptr;
VkPipeline PointLight::drawPointLightPipeline = nullptr;
VkPipeline PointLight::drawPointLightPipelineStencil = nullptr;

VkDescriptorPool PointLight::pointLightingDescriptorPool = nullptr;
VkDescriptorSetLayout PointLight::drawPointLightDescriptorSetLayout = nullptr;
VkDescriptorSet PointLight::drawPointLightDescriptorSet=nullptr;

std::vector<PointLightData> PointLight::pointLightData;

constexpr uint32_t SPHERE_SLICE = 20;
constexpr uint32_t SPHERE_SLICE_2 = SPHERE_SLICE * 2;
constexpr uint32_t SPHERE_INDEX_COUNT = SPHERE_SLICE * SPHERE_SLICE_2 * 6;
void PointLight::InitRHI(const VulkanDevice& device, const GpuScene& gpuScene)
{
	if (!sphereBuffer)
	{
        constexpr float deltaPI = M_PI_F / SPHERE_SLICE;
        std::vector<vec3> sphereVertices;
        for (int i = 0; i < SPHERE_SLICE + 1; ++i)
        {
            vec3 vertex(0, sinf(i * deltaPI), cosf(i * deltaPI));
            sphereVertices.push_back(vertex);
        }
        constexpr float delta2PI = M_PI_F * 2 / SPHERE_SLICE_2;
        for (int i = 1; i < SPHERE_SLICE_2; i++)
        {
            float angle = delta2PI * i;
            //rotate around Y
            //mat4 rotateY = rotateY(angle);
            for (int j = 0; j < SPHERE_SLICE + 1; ++j)
            {
                vec4 vertex = rotateY(angle) * vec4(sphereVertices[j], 1);
                sphereVertices.push_back(vertex.xyz());
            }
        }
	}

	if (!PointLight::drawPointLightPipelineLayout)
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
        frameDataBinding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        frameDataBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_VERTEX_BIT;

       
        VkDescriptorSetLayoutBinding pointLightDataBinding = {};
        pointLightDataBinding.binding = 7;
        pointLightDataBinding.descriptorCount = 1;
        // it's a uniform buffer binding
        pointLightDataBinding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        pointLightDataBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_VERTEX_BIT;


        VkDescriptorSetLayoutBinding bindings[] = { albedoBinding, normalBinding, emessiveBinding, f0RoughnessBinding,
            depthBinding, nearestClampSamplerBinding,frameDataBinding,pointLightDataBinding };

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
            {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 12},
            {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 3},
            {VK_DESCRIPTOR_TYPE_SAMPLER, 3},
        };

        VkDescriptorPoolCreateInfo pool_info = {};
        pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pool_info.flags = 0;
        pool_info.maxSets = 2;
        pool_info.poolSizeCount = (uint32_t)sizes.size();
        pool_info.pPoolSizes = sizes.data();

        vkCreateDescriptorPool(device.getLogicalDevice(), &pool_info, nullptr,
            &PointLight::pointLightingDescriptorPool);

        VkDescriptorSetAllocateInfo allocInfo = {};
        allocInfo.pNext = nullptr;
        allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        // using the pool we just set
        allocInfo.descriptorPool = PointLight::pointLightingDescriptorPool;
        // only 1 descriptor
        allocInfo.descriptorSetCount = 1;
        // using the global data layout
        allocInfo.pSetLayouts = &PointLight::drawPointLightDescriptorSetLayout;

        vkAllocateDescriptorSets(device.getLogicalDevice(), &allocInfo,
            &PointLight::drawPointLightDescriptorSet);


        //--------------------write the descriptset ----------
        // 
        // 
        // information about the buffer we want to point at in the descriptor
        VkDescriptorBufferInfo binfo;
        // it will be the camera buffer
        binfo.buffer = gpuScene.uniformBuffer;
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
        setWrite.dstSet = PointLight::drawPointLightDescriptorSet;

        setWrite.descriptorCount = 1;
        setWrite.dstArrayElement = 0;
        // and the type is uniform buffer
        setWrite.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        setWrite.pBufferInfo = &binfo;


        VkDescriptorBufferInfo binfo1;
        // it will be the camera buffer
        binfo1.buffer = PointLight::pointLightDynamicUniformBuffer;
        // at 0 offset
        binfo1.offset = 0;
        // of the size of a camera data struct
        binfo1.range = sizeof(FrameData);

        VkWriteDescriptorSet setWrite1 = {};
        setWrite1.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        setWrite1.pNext = nullptr;

        // we are going to write into binding number 0
        setWrite1.dstBinding = 7;
        // of the global descriptor
        setWrite1.dstSet = PointLight::drawPointLightDescriptorSet;

        setWrite1.descriptorCount = 1;
        setWrite1.dstArrayElement = 0;
        // and the type is uniform buffer
        setWrite1.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        setWrite1.pBufferInfo = &binfo1;


        VkDescriptorImageInfo samplerinfo;
        samplerinfo.sampler = gpuScene.nearestClampSampler;
        VkWriteDescriptorSet setSampler = {};
        setSampler.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        setSampler.dstBinding = 5;
        setSampler.pNext = nullptr;
        setSampler.dstSet = PointLight::drawPointLightDescriptorSet;
        setSampler.dstArrayElement = 0;
        setSampler.descriptorCount = 1;
        setSampler.pImageInfo = &samplerinfo;
        setSampler.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;



        std::array<VkDescriptorImageInfo, 4> imageinfo{};
        //imageinfo.resize(textures.size());
        for (int texturei = 0; texturei < 4; ++texturei)
        {
            imageinfo[texturei].imageView = gpuScene._gbuffersView[texturei];
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
            setWriteTexture[i].dstSet = PointLight::drawPointLightDescriptorSet;
            setWriteTexture[i].dstArrayElement = 0;

            setWriteTexture[i].descriptorCount = 1;
            setWriteTexture[i].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            setWriteTexture[i].pImageInfo = &imageinfo[i];
        }
        
        VkDescriptorImageInfo depthImageInfo{};
        depthImageInfo.imageView = device.getWindowDepthImageView();
        depthImageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkWriteDescriptorSet setWriteDepth;
        setWriteDepth.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        setWriteDepth.pNext = nullptr;
        setWriteDepth.dstBinding = 4;
        setWriteDepth.dstSet = PointLight::drawPointLightDescriptorSet;
        setWriteDepth.dstArrayElement = 0;
        setWriteDepth.descriptorCount = 1;
        setWriteDepth.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        setWriteDepth.pImageInfo = &depthImageInfo;

        std::array< VkWriteDescriptorSet, 8> writes = { setWriteTexture[0],setWriteTexture[1],setWriteTexture[2],setWriteTexture[3],setWriteDepth,setSampler,setWrite,setWrite1 };

        vkUpdateDescriptorSets(device.getLogicalDevice(), writes.size(), writes.data(), 0, nullptr);


        VkPipelineLayoutCreateInfo pointLightingPipelineLayoutInfo{};
        pointLightingPipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pointLightingPipelineLayoutInfo.setLayoutCount = 1;
        pointLightingPipelineLayoutInfo.pSetLayouts = &PointLight::drawPointLightDescriptorSetLayout;
        pointLightingPipelineLayoutInfo.pushConstantRangeCount = 0;

        if (vkCreatePipelineLayout(device.getLogicalDevice(), &pointLightingPipelineLayoutInfo,
            nullptr, &drawPointLightPipelineLayout) != VK_SUCCESS) {
            throw std::runtime_error("failed to create drawpoint light pipeline layout!");
        }


        auto deferredPointLightingVSShaderCode = readFile((gpuScene.RootPath() / "shaders/deferredPointLighting.vs.spv").generic_string());
        auto deferredPointLightingPSShaderCode = readFile((gpuScene.RootPath() / "shaders/deferredPointLighting.ps.spv").generic_string());

        VkShaderModule deferredPointLightingVSShaderModule = gpuScene.createShaderModule(deferredPointLightingVSShaderCode);
        VkShaderModule deferredPointLightingPSShaderModule = gpuScene.createShaderModule(deferredPointLightingPSShaderCode);

        VkPipelineShaderStageCreateInfo deferredLightingVSShaderStageInfo{};
        deferredLightingVSShaderStageInfo.sType =
            VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        deferredLightingVSShaderStageInfo.stage = VK_SHADER_STAGE_VERTEX_BIT;
        deferredLightingVSShaderStageInfo.module = deferredPointLightingVSShaderModule;
        deferredLightingVSShaderStageInfo.pName = "RenderSceneVS";

        VkPipelineShaderStageCreateInfo deferredLightingPSShaderStageInfo{};
        deferredLightingPSShaderStageInfo.sType =
            VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        deferredLightingPSShaderStageInfo.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        deferredLightingPSShaderStageInfo.module = deferredPointLightingPSShaderModule;
        deferredLightingPSShaderStageInfo.pName = "DeferredLighting";

        VkPipelineShaderStageCreateInfo deferredPointLightingPassStages[] = { deferredLightingVSShaderStageInfo,
                                               deferredLightingPSShaderStageInfo };

        VkPipelineShaderStageCreateInfo deferredPointLightingPassStagesStencil[] = { deferredLightingVSShaderStageInfo
                                                };

        constexpr VkVertexInputBindingDescription drawPointLightInputBindingPosition = {
            .binding = 0,
            .stride = sizeof(float) * 3,
            .inputRate = VK_VERTEX_INPUT_RATE_VERTEX };

        constexpr VkVertexInputAttributeDescription drawPointLightInputAttributes[] = {
            {
                .location = 0,
                .binding = 0,
                .format = VK_FORMAT_R32G32B32_SFLOAT,
                .offset = 0},
        };
        constexpr int inputChannelCount = sizeof(drawPointLightInputAttributes) / sizeof(drawPointLightInputAttributes[0]);

        constexpr std::array<VkVertexInputBindingDescription, inputChannelCount> drawPointLightInputs = { drawPointLightInputBindingPosition };

        VkPipelineVertexInputStateCreateInfo drawPointLightVertexInputInfo{};
        drawPointLightVertexInputInfo.sType =
            VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        drawPointLightVertexInputInfo.vertexBindingDescriptionCount = drawPointLightInputs.size();
        drawPointLightVertexInputInfo.pVertexBindingDescriptions = drawPointLightInputs.data();
        drawPointLightVertexInputInfo.vertexAttributeDescriptionCount = inputChannelCount;
        drawPointLightVertexInputInfo.pVertexAttributeDescriptions = drawPointLightInputAttributes;


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
        rasterizer.cullMode = VK_CULL_MODE_FRONT_BIT;                   //draw the backface of the sphere
        rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        rasterizer.depthBiasEnable = VK_FALSE;

        VkPipelineRasterizationStateCreateInfo rasterizer1{};
        rasterizer1.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rasterizer1.depthClampEnable = VK_FALSE;
        rasterizer1.rasterizerDiscardEnable = VK_FALSE;
        rasterizer1.polygonMode = VK_POLYGON_MODE_FILL;
        rasterizer1.lineWidth = 1.0f;
        rasterizer1.cullMode = VK_CULL_MODE_FRONT_BIT;
        rasterizer1.frontFace = VK_FRONT_FACE_CLOCKWISE;                 //stencil pass
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
        depthStencilState.front = { .failOp = VK_STENCIL_OP_KEEP ,
                                    .passOp = VK_STENCIL_OP_KEEP ,
                                    .depthFailOp = VK_STENCIL_OP_KEEP,
                                    .compareOp = VK_COMPARE_OP_NOT_EQUAL,
                                    .compareMask = 0xff,
                                    .writeMask = 0x0,
                                    .reference = 0x1
                                    };
        depthStencilState.back = {};
        depthStencilState.depthCompareOp = VK_COMPARE_OP_LESS;              //pass the depth test only when depth of the the backface of the sphere is futher away than the depth buffer
        depthStencilState.depthBoundsTestEnable = VK_FALSE;

        VkPipelineDepthStencilStateCreateInfo depthStencilState1{};
        depthStencilState1.sType =
            VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        depthStencilState1.depthWriteEnable = VK_FALSE;
        depthStencilState1.depthTestEnable = VK_TRUE;
        depthStencilState1.stencilTestEnable = VK_TRUE;
        depthStencilState1.front = { .failOp = VK_STENCIL_OP_KEEP ,
                                    .passOp = VK_STENCIL_OP_KEEP ,
                                    .depthFailOp = VK_STENCIL_OP_REPLACE,
                                    .compareOp = VK_COMPARE_OP_ALWAYS,
                                    .compareMask = 0xff,
                                    .writeMask = 0xff,
                                    .reference = 0x1
        };
        depthStencilState1.back = {};
        depthStencilState1.depthCompareOp = VK_COMPARE_OP_GREATER;
        depthStencilState1.depthBoundsTestEnable = VK_FALSE;


        VkGraphicsPipelineCreateInfo pointLightingPipelineInfo{};
        pointLightingPipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        pointLightingPipelineInfo.stageCount = 2;
        pointLightingPipelineInfo.pStages = deferredPointLightingPassStages;
        pointLightingPipelineInfo.pVertexInputState = &drawPointLightVertexInputInfo;
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

        if (vkCreateGraphicsPipelines(device.getLogicalDevice(), VK_NULL_HANDLE, 1, &pointLightingPipelineInfo,
            nullptr, &PointLight::drawPointLightPipeline) != VK_SUCCESS) {
            throw std::runtime_error("failed to create drawcluster base graphics pipeline!");
        }

        VkGraphicsPipelineCreateInfo pointLightingPipelineInfoStencil{};
        pointLightingPipelineInfoStencil.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        pointLightingPipelineInfoStencil.stageCount = 1;
        pointLightingPipelineInfoStencil.pStages = deferredPointLightingPassStagesStencil;
        pointLightingPipelineInfoStencil.pVertexInputState = &drawPointLightVertexInputInfo;
        pointLightingPipelineInfoStencil.pInputAssemblyState = &inputAssembly;
        pointLightingPipelineInfoStencil.pViewportState = &viewportState;
        pointLightingPipelineInfoStencil.pRasterizationState = &rasterizer1;
        pointLightingPipelineInfoStencil.pMultisampleState = &multisampling;
        pointLightingPipelineInfoStencil.pColorBlendState = &colorBlendingAlphaAdd;
        pointLightingPipelineInfoStencil.layout = PointLight::drawPointLightPipelineLayout;
        pointLightingPipelineInfoStencil.renderPass = gpuScene._deferredLightingPass;
        pointLightingPipelineInfoStencil.subpass = 0;
        pointLightingPipelineInfoStencil.basePipelineHandle = VK_NULL_HANDLE;
        pointLightingPipelineInfoStencil.pDepthStencilState = &depthStencilState1;
	}
}

void PointLight::CommonDrawSetup(VkCommandBuffer& commandBuffer)
{
    //only light the area where stencil not equal 1
    VkBuffer vertexBuffers[] = { sphereBuffer };
    VkDeviceSize offsets[] = { 0 };

    vkCmdBindVertexBuffers(commandBuffer, 0,
        sizeof(vertexBuffers) / sizeof(vertexBuffers[0]),
        vertexBuffers, offsets);
    vkCmdBindIndexBuffer(commandBuffer, sphereIndexBuffer, 0,
        VK_INDEX_TYPE_UINT16);
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, drawPointLightPipeline);

}

void PointLight::Draw(VkCommandBuffer& commandBuffer,const GpuScene& gpuScene)
{
	//draw the stencil buffer
	//mark the invalid area stencil 1
	//lighting
    

    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, drawPointLightPipelineLayout, 0, 1, &drawPointLightDescriptorSet, 1, &_dynamicOffset);
    vkCmdDrawIndexed(commandBuffer, SPHERE_INDEX_COUNT, 1, 0, 0, 0);
}