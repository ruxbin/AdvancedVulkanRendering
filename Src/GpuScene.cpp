
#include "GpuScene.h"
#include "ObjLoader.h"
#include "VulkanSetup.h"
#include <fstream>
#include <vector>
#include <array>
static std::vector<char> readFile(const std::string &filename) {
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

VkShaderModule GpuScene::createShaderModule(const std::vector<char> &code) {
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

void GpuScene::createGraphicsPipeline(VkRenderPass renderPass) {
  // TODO: shader management -- hot reload
  auto vertShaderCode = readFile("shaders/vert.spv");
  auto fragShaderCode = readFile("shaders/frag.spv");

  auto evertShaderCode = readFile("shaders/edward.vert.spv");
  auto efragShaderCode = readFile("shaders/edward.frag.spv");

  VkShaderModule vertShaderModule = createShaderModule(vertShaderCode);
  VkShaderModule fragShaderModule = createShaderModule(fragShaderCode);

  VkShaderModule evertShaderModule = createShaderModule(evertShaderCode);
  VkShaderModule efragShaderModule = createShaderModule(efragShaderCode);

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
  evertShaderStageInfo.pName = "main";

  VkPipelineShaderStageCreateInfo efragShaderStageInfo{};
  efragShaderStageInfo.sType =
      VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  efragShaderStageInfo.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  efragShaderStageInfo.module = efragShaderModule;
  efragShaderStageInfo.pName = "main";

  VkPipelineShaderStageCreateInfo shaderStages[] = {vertShaderStageInfo,
                                                    fragShaderStageInfo};
  VkPipelineShaderStageCreateInfo eshaderStages[] = {evertShaderStageInfo,
                                                     efragShaderStageInfo};

  VkPipelineVertexInputStateCreateInfo vertexInputInfo{};
  vertexInputInfo.sType =
      VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
  vertexInputInfo.vertexBindingDescriptionCount = 0;
  vertexInputInfo.vertexAttributeDescriptionCount = 0;

  VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
  inputAssembly.sType =
      VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
  inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
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

  VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
  pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  pipelineLayoutInfo.setLayoutCount = 0;
  pipelineLayoutInfo.pushConstantRangeCount = 0;

  if (vkCreatePipelineLayout(device.getLogicalDevice(), &pipelineLayoutInfo,
                             nullptr, &pipelineLayout) != VK_SUCCESS) {
    throw std::runtime_error("failed to create pipeline layout!");
  }

  VkPushConstantRange epushconstantRange = {.stageFlags =
                                                VK_SHADER_STAGE_VERTEX_BIT,
                                            .offset = 0,
                                            .size = sizeof(mat4)};
  VkPipelineLayoutCreateInfo epipelineLayoutInfo{};
  epipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  epipelineLayoutInfo.setLayoutCount = 1;
  epipelineLayoutInfo.pSetLayouts = &globalSetLayout;
  epipelineLayoutInfo.pushConstantRangeCount = 1;
  epipelineLayoutInfo.pPushConstantRanges = &epushconstantRange;

  if (vkCreatePipelineLayout(device.getLogicalDevice(), &epipelineLayoutInfo,
                             nullptr, &epipelineLayout) != VK_SUCCESS) {
    throw std::runtime_error("failed to create pipeline layout!");
  }

  VkPipelineDepthStencilStateCreateInfo depthStencilState1{};
  depthStencilState1.sType =
      VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
  depthStencilState1.depthWriteEnable = VK_FALSE;
  depthStencilState1.depthTestEnable = VK_FALSE;
  depthStencilState1.stencilTestEnable = VK_FALSE;

  VkGraphicsPipelineCreateInfo pipelineInfo{};
  pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
  pipelineInfo.stageCount = 2;
  pipelineInfo.pStages = shaderStages;
  pipelineInfo.pVertexInputState = &vertexInputInfo;
  pipelineInfo.pInputAssemblyState = &inputAssembly;
  pipelineInfo.pViewportState = &viewportState;
  pipelineInfo.pRasterizationState = &rasterizer;
  pipelineInfo.pMultisampleState = &multisampling;
  pipelineInfo.pColorBlendState = &colorBlending;
  pipelineInfo.layout = pipelineLayout;
  pipelineInfo.renderPass = renderPass;
  pipelineInfo.subpass = 0;
  pipelineInfo.basePipelineHandle = VK_NULL_HANDLE;
  // The Vulkan spec states: If renderPass is not VK_NULL_HANDLE, the pipeline
  // is being created with fragment shader state, and subpass uses a
  // depth/stencil attachment, pDepthStencilState must be a valid pointer to a
  // valid VkPipelineDepthStencilStateCreateInfo structure
  pipelineInfo.pDepthStencilState = &depthStencilState1;

  if (vkCreateGraphicsPipelines(device.getLogicalDevice(), VK_NULL_HANDLE, 1, &pipelineInfo,
                                nullptr, &graphicsPipeline) != VK_SUCCESS) {
    throw std::runtime_error("failed to create graphics pipeline!");
  }

  VkVertexInputBindingDescription edwardInputBinding = {
      .binding = 0,
      .stride = sizeof(float) * 3 * 2 + sizeof(float) * 2,
      .inputRate = VK_VERTEX_INPUT_RATE_VERTEX};

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
       .offset = sizeof(float) * 3 * 2}};

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
  depthStencilState.depthCompareOp = VK_COMPARE_OP_LESS;
  depthStencilState.depthBoundsTestEnable = VK_FALSE;

  VkGraphicsPipelineCreateInfo edwardpipelineInfo{};
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
  }

  vkDestroyShaderModule(device.getLogicalDevice(), fragShaderModule, nullptr);
  vkDestroyShaderModule(device.getLogicalDevice(), vertShaderModule, nullptr);

  vkDestroyShaderModule(device.getLogicalDevice(), evertShaderModule, nullptr);
  vkDestroyShaderModule(device.getLogicalDevice(), efragShaderModule, nullptr);
}

void GpuScene::init_descriptors() {

  // information about the binding.
  VkDescriptorSetLayoutBinding camBufferBinding = {};
  camBufferBinding.binding = 0;
  camBufferBinding.descriptorCount = 1;
  // it's a uniform buffer binding
  camBufferBinding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;

  // we use it from the vertex shader
  camBufferBinding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;

  VkDescriptorSetLayoutCreateInfo setinfo = {};
  setinfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  setinfo.pNext = nullptr;

  // we are going to have 1 binding
  setinfo.bindingCount = 1;
  // no flags
  setinfo.flags = 0;
  // point to the camera buffer binding
  setinfo.pBindings = &camBufferBinding;

  vkCreateDescriptorSetLayout(device.getLogicalDevice(), &setinfo, nullptr,
                              &globalSetLayout);

  // other code ....
  // create a descriptor pool that will hold 10 uniform buffers
  std::vector<VkDescriptorPoolSize> sizes = {
      {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 10}};

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
  binfo.range = sizeof(uniformBufferData);

  VkWriteDescriptorSet setWrite = {};
  setWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  setWrite.pNext = nullptr;

  // we are going to write into binding number 0
  setWrite.dstBinding = 0;
  // of the global descriptor
  setWrite.dstSet = globalDescriptor;

  setWrite.descriptorCount = 1;
  // and the type is uniform buffer
  setWrite.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
  setWrite.pBufferInfo = &binfo;

  vkUpdateDescriptorSets(device.getLogicalDevice(), 1, &setWrite, 0, nullptr);
}

GpuScene::GpuScene(std::string_view &filepath, const VulkanDevice &deviceref)
    : device(deviceref), modelScale(1.f) {
  LoadObj(filepath.data());
  createSyncObjects();
  createVertexBuffer();
  createIndexBuffer();
  createUniformBuffer();
  init_descriptors();
  createCommandBuffer(deviceref.getCommandPool());
  createGraphicsPipeline(deviceref.getMainRenderPass());

  maincamera = new Camera(60 * 3.1414926f / 180.f, 0.1, 100, vec3(0, 0, -2),
                          deviceref.getSwapChainExtent().width /
                              float(deviceref.getSwapChainExtent().height));
}

void GpuScene::recordCommandBuffer(int imageIndex){

	VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;

        if (vkBeginCommandBuffer(commandBuffer, &beginInfo) != VK_SUCCESS) {
            throw std::runtime_error("failed to begin recording command buffer!");
        }

VkRenderPassBeginInfo renderPassInfo{};
        renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        renderPassInfo.renderPass = device.getMainRenderPass();
        renderPassInfo.framebuffer = device.getSwapChainFrameBuffer(imageIndex);
        renderPassInfo.renderArea.offset = {0, 0};
        renderPassInfo.renderArea.extent = device.getSwapChainExtent();

        std::array<VkClearValue, 2> clearValues{};
        clearValues[0].color = {{0.0f, 0.0f, 0.0f, 1.0f}};
        clearValues[1].depthStencil = {1.0f, 0};
        renderPassInfo.clearValueCount = static_cast<uint32_t>(clearValues.size());
        renderPassInfo.pClearValues = clearValues.data();

        vkCmdBeginRenderPass(commandBuffer, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);

            vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, graphicsPipeline);

            vkCmdDraw(commandBuffer, 3, 1, 0, 0);

            vkCmdBindPipeline(commandBuffer,VK_PIPELINE_BIND_POINT_GRAPHICS, egraphicsPipeline);
            VkBuffer vertexBuffers[] = {vertexBuffer};
            VkDeviceSize offsets[] = {0};

            vkCmdBindVertexBuffers(commandBuffer, 0, 1, vertexBuffers, offsets);
            vkCmdBindIndexBuffer(commandBuffer,indexBuffer,0,VK_INDEX_TYPE_UINT16);
            //if the descriptor set data isn't change we can omit this?
            vkCmdBindDescriptorSets(commandBuffer,VK_PIPELINE_BIND_POINT_GRAPHICS,epipelineLayout,0,1,&globalDescriptor,0,nullptr);
            //if the constant isn't changed we can omit this?
            mat4 scaleM = scale(modelScale);
            mat4 withScale = transpose(maincamera->getObjectToCamera()) * scaleM;
            vkCmdPushConstants(commandBuffer,epipelineLayout,VK_SHADER_STAGE_VERTEX_BIT,0,sizeof(mat4),withScale.value_ptr());
            vkCmdDrawIndexed(commandBuffer,getIndexSize()/sizeof(unsigned short),1,0,0,0);

        vkCmdEndRenderPass(commandBuffer);

        if (vkEndCommandBuffer(commandBuffer) != VK_SUCCESS) {
            throw std::runtime_error("failed to record command buffer!");
        }

}

void GpuScene::createVertexBuffer() {
  VkBufferCreateInfo bufferInfo{};
  bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  bufferInfo.size = getVertexSize();
  bufferInfo.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
  bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  bufferInfo.flags = 0;
  if (vkCreateBuffer(device.getLogicalDevice(), &bufferInfo, nullptr,
                     &vertexBuffer) != VK_SUCCESS) {
    throw std::runtime_error("failed to create vertex buffer!");
  }
  VkMemoryRequirements memRequirements;
  vkGetBufferMemoryRequirements(device.getLogicalDevice(), vertexBuffer,
                                &memRequirements);

  VkMemoryAllocateInfo allocInfo{};
  allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  allocInfo.allocationSize = memRequirements.size;
  allocInfo.memoryTypeIndex = findMemoryType(
      memRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

  if (vkAllocateMemory(device.getLogicalDevice(), &allocInfo, nullptr,
                       &vertexBufferMemory) != VK_SUCCESS) {
    throw std::runtime_error("failed to allocate vertex buffer memory!");
  }
  vkBindBufferMemory(device.getLogicalDevice(), vertexBuffer,
                     vertexBufferMemory, 0);
  void *data;
  vkMapMemory(device.getLogicalDevice(), vertexBufferMemory, 0, bufferInfo.size,
              0, &data);
  memcpy(data, getRawVertexData(), (size_t)bufferInfo.size);
  vkUnmapMemory(device.getLogicalDevice(), vertexBufferMemory);
}

void GpuScene::createIndexBuffer() {
  VkBufferCreateInfo bufferInfo{};
  bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  bufferInfo.size = getIndexSize();
  bufferInfo.usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
  bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  bufferInfo.flags = 0;
  if (vkCreateBuffer(device.getLogicalDevice(), &bufferInfo, nullptr, &indexBuffer) !=
      VK_SUCCESS) {
    throw std::runtime_error("failed to create index buffer!");
  }
  VkMemoryRequirements memRequirements;
  vkGetBufferMemoryRequirements(device.getLogicalDevice(), indexBuffer, &memRequirements);

  VkMemoryAllocateInfo allocInfo{};
  allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  allocInfo.allocationSize = memRequirements.size;
  allocInfo.memoryTypeIndex = findMemoryType(
      memRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

  if (vkAllocateMemory(device.getLogicalDevice(), &allocInfo, nullptr, &indexBufferMemory) !=
      VK_SUCCESS) {
    throw std::runtime_error("failed to allocate vertex buffer memory!");
  }
  vkBindBufferMemory(device.getLogicalDevice(), indexBuffer, indexBufferMemory, 0);
  void *data;
  vkMapMemory(device.getLogicalDevice(), indexBufferMemory, 0, bufferInfo.size, 0, &data);
  memcpy(data, getRawIndexData(), (size_t)bufferInfo.size);
  vkUnmapMemory(device.getLogicalDevice(), indexBufferMemory);
}

void GpuScene::Draw() {
  // begin command buffer record
  // bind graphics pipeline
  // update uniform buffer
  // draw mesh
  // submit commandbuffer

  vkWaitForFences(device.getLogicalDevice(), 1, &inFlightFence, VK_TRUE, UINT64_MAX);
  vkResetFences(device.getLogicalDevice(), 1, &inFlightFence);

  uint32_t imageIndex;
  vkAcquireNextImageKHR(device.getLogicalDevice(), device.getSwapChain(), UINT64_MAX, imageAvailableSemaphore,
                        VK_NULL_HANDLE, &imageIndex);

  void *data;
  vkMapMemory(device.getLogicalDevice(), uniformBufferMemory, 0, sizeof(uniformBufferData), 0,
              &data);
  memcpy(data, transpose(maincamera->getProjectMatrix()).value_ptr(),
         (size_t)sizeof(uniformBufferData));
  vkUnmapMemory(device.getLogicalDevice(), uniformBufferMemory);
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

  if (vkQueueSubmit(device.getGraphicsQueue(), 1, &submitInfo, inFlightFence) !=
      VK_SUCCESS) {
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

