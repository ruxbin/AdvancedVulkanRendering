#include "GpuScene.h"
#include "SceneClickTracker.h"
#include "AssetLoader.h"
#include "imgui_impl_vulkan.h"
#include <SDL.h>
#include <array>
#include <cstring>

namespace {
void pickCheck(VkResult result) {
  if (result != VK_SUCCESS) throw std::runtime_error("Vulkan picking resource/command failed: " + std::to_string(result));
}
}

void GpuScene::RequestScenePick(float mx, float my) {
  if (!_selection.ready || _pickFailed || useRayTracing || _decalEditMode || !_sdlWindow) return;
  int w, h; SDL_GetWindowSize(_sdlWindow, &w, &h);
  const auto extent = device.getSwapChainExtent();
  uint32_t x, y;
  if (scene_selection::ScreenPixel(mx,my,w,h,extent.width,extent.height,x,y))
    _selection.Request(x,y);
}

void GpuScene::createPicking() {
  const auto dev = device.getLogicalDevice();
  const auto extent = device.getSwapChainExtent();
  VkFormatProperties props{};
  vkGetPhysicalDeviceFormatProperties(device.getPhysicalDevice(), VK_FORMAT_R32_UINT, &props);
  const auto required = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT;
  if ((props.optimalTilingFeatures & required) != required)
    throw std::runtime_error("R32_UINT picking attachment unsupported");
  const auto depthFormat = device.getWindowDepthFormat();
  VkAttachmentDescription attachments[2]{};
  attachments[0].format = VK_FORMAT_R32_UINT;
  attachments[0].samples = VK_SAMPLE_COUNT_1_BIT;
  attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  attachments[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  attachments[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  attachments[0].finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  attachments[1] = attachments[0];
  attachments[1].format = depthFormat;
  attachments[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  attachments[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
  VkAttachmentReference color{0,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
  VkAttachmentReference depth{1,VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
  VkSubpassDescription sub{};
  sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  sub.colorAttachmentCount=1; sub.pColorAttachments=&color; sub.pDepthStencilAttachment=&depth;
  VkSubpassDependency deps[2]{};
  deps[0].srcSubpass=VK_SUBPASS_EXTERNAL; deps[0].dstSubpass=0;
  deps[0].srcStageMask=VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
  deps[0].dstStageMask=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
  deps[0].dstAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
  deps[1].srcSubpass=0; deps[1].dstSubpass=VK_SUBPASS_EXTERNAL;
  deps[1].srcStageMask=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  deps[1].dstStageMask=VK_PIPELINE_STAGE_TRANSFER_BIT;
  deps[1].srcAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  deps[1].dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
  VkRenderPassCreateInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
  rp.attachmentCount=2; rp.pAttachments=attachments; rp.subpassCount=1; rp.pSubpasses=&sub;
  rp.dependencyCount=2; rp.pDependencies=deps;
  pickCheck(vkCreateRenderPass(dev,&rp,nullptr,&_pickPass));

  _pickSlots.resize(framesInFlight);
  auto image = [&](VkFormat format, VkImageUsageFlags usage, VkImageAspectFlags aspect,
                   VkImage& img, VkDeviceMemory& memory, VkImageView& view) {
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType=VK_IMAGE_TYPE_2D; ci.format=format; ci.extent={extent.width,extent.height,1};
    ci.mipLevels=1; ci.arrayLayers=1; ci.samples=VK_SAMPLE_COUNT_1_BIT;
    ci.tiling=VK_IMAGE_TILING_OPTIMAL; ci.usage=usage;
    pickCheck(vkCreateImage(dev,&ci,nullptr,&img));
    VkMemoryRequirements req{}; vkGetImageMemoryRequirements(dev,img,&req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize=req.size;
    ai.memoryTypeIndex=device.findMemoryType(req.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    pickCheck(vkAllocateMemory(dev,&ai,nullptr,&memory));
    pickCheck(vkBindImageMemory(dev,img,memory,0));
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image=img; vi.viewType=VK_IMAGE_VIEW_TYPE_2D; vi.format=format;
    vi.subresourceRange={aspect,0,1,0,1};
    pickCheck(vkCreateImageView(dev,&vi,nullptr,&view));
  };
  for (auto& s : _pickSlots) {
    image(VK_FORMAT_R32_UINT,VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
          VK_IMAGE_ASPECT_COLOR_BIT,s.id,s.idMemory,s.idView);
    image(depthFormat,VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,VK_IMAGE_ASPECT_DEPTH_BIT,
          s.depth,s.depthMemory,s.depthView);
    VkImageView views[]={s.idView,s.depthView};
    VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fi.renderPass=_pickPass; fi.attachmentCount=2; fi.pAttachments=views;
    fi.width=extent.width; fi.height=extent.height; fi.layers=1;
    pickCheck(vkCreateFramebuffer(dev,&fi,nullptr,&s.framebuffer));
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size=4; bi.usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    pickCheck(vkCreateBuffer(dev,&bi,nullptr,&s.readback));
    VkMemoryRequirements req{}; vkGetBufferMemoryRequirements(dev,s.readback,&req);
    VkPhysicalDeviceMemoryProperties mp{};
    vkGetPhysicalDeviceMemoryProperties(device.getPhysicalDevice(),&mp);
    uint32_t type=UINT32_MAX;
    for (uint32_t i=0;i<mp.memoryTypeCount;++i)
      if ((req.memoryTypeBits & (1u<<i)) && (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) {
        type=i;
        if (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) break;
      }
    if (type==UINT32_MAX) throw std::runtime_error("No host-visible picking memory");
    s.coherent=(mp.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)!=0;
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize=req.size; ai.memoryTypeIndex=type;
    pickCheck(vkAllocateMemory(dev,&ai,nullptr,&s.readbackMemory));
    pickCheck(vkBindBufferMemory(dev,s.readback,s.readbackMemory,0));
  }

  VkVertexInputBindingDescription bindings[]={{0,12,VK_VERTEX_INPUT_RATE_VERTEX},{1,12,VK_VERTEX_INPUT_RATE_VERTEX},
                                             {2,12,VK_VERTEX_INPUT_RATE_VERTEX},{3,8,VK_VERTEX_INPUT_RATE_VERTEX}};
  VkVertexInputAttributeDescription attrs[]={{0,0,VK_FORMAT_R32G32B32_SFLOAT,0},{1,1,VK_FORMAT_R32G32B32_SFLOAT,0},
                                           {2,2,VK_FORMAT_R32G32B32_SFLOAT,0},{3,3,VK_FORMAT_R32G32_SFLOAT,0}};
  VkPipelineVertexInputStateCreateInfo vertex{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  vertex.vertexBindingDescriptionCount=4; vertex.pVertexBindingDescriptions=bindings;
  vertex.vertexAttributeDescriptionCount=4; vertex.pVertexAttributeDescriptions=attrs;
  VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
  assembly.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
  vp.viewportCount=1; vp.scissorCount=1;
  VkPipelineRasterizationStateCreateInfo rast{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
  rast.polygonMode=VK_POLYGON_MODE_FILL; rast.lineWidth=1; rast.frontFace=VK_FRONT_FACE_CLOCKWISE;
  // All three current Vulkan main-scene PSOs use this cull state.
  rast.cullMode=VK_CULL_MODE_BACK_BIT;
  VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
  ms.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT;
  VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
  ds.depthTestEnable=VK_TRUE; ds.depthWriteEnable=VK_TRUE; ds.depthCompareOp=VK_COMPARE_OP_GREATER;
  VkPipelineColorBlendAttachmentState blend{}; blend.colorWriteMask=VK_COLOR_COMPONENT_R_BIT;
  VkPipelineColorBlendStateCreateInfo bs{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
  bs.attachmentCount=1; bs.pAttachments=&blend;
  VkDynamicState dynamic[]={VK_DYNAMIC_STATE_VIEWPORT,VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo dyn{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
  dyn.dynamicStateCount=2; dyn.pDynamicStates=dynamic;
  const char* files[]={"picking.opaque.ps.spv","picking.mask.ps.spv","picking.transparent.ps.spv"};
  const char* entries[]={"PickOpaque","PickAlphaMask","PickTransparent"};
  VkShaderModule vs=VK_NULL_HANDLE, ps=VK_NULL_HANDLE;
  try {
    vs=createShaderModule(AssetLoader::readFileAsset((_rootPath/"shaders/picking.vs.spv").string()));
    for (int i=0;i<3;++i) {
      ps=createShaderModule(AssetLoader::readFileAsset((_rootPath/"shaders"/files[i]).string()));
      VkPipelineShaderStageCreateInfo stages[2]{};
      stages[0]={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
      stages[0].stage=VK_SHADER_STAGE_VERTEX_BIT; stages[0].module=vs; stages[0].pName="RenderScenePickingVS";
      stages[1]={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
      stages[1].stage=VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module=ps; stages[1].pName=entries[i];
      VkGraphicsPipelineCreateInfo pi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
      pi.stageCount=2; pi.pStages=stages; pi.pVertexInputState=&vertex; pi.pInputAssemblyState=&assembly;
      pi.pViewportState=&vp; pi.pRasterizationState=&rast; pi.pMultisampleState=&ms;
      pi.pDepthStencilState=&ds; pi.pColorBlendState=&bs; pi.pDynamicState=&dyn;
      pi.layout=drawclusterBasePipelineLayout; pi.renderPass=_pickPass;
      pickCheck(vkCreateGraphicsPipelines(dev,VK_NULL_HANDLE,1,&pi,nullptr,&_pickPipelines[i]));
      vkDestroyShaderModule(dev,ps,nullptr); ps=VK_NULL_HANDLE;
    }
  } catch (...) {
    if(ps) vkDestroyShaderModule(dev,ps,nullptr);
    if(vs) vkDestroyShaderModule(dev,vs,nullptr);
    throw;
  }
  vkDestroyShaderModule(dev,vs,nullptr);
}

void GpuScene::recordPicking(VkCommandBuffer cmd) {
  if (!_selection.HasPending() || !_selection.ready || _pickFailed) return;
  if (!_pickPass) {
    try { createPicking(); }
    catch (const std::exception& e) {
      destroyPicking(); _pickFailed=true; _selection.BackendError(e.what());
      spdlog::error("Picking disabled: {}",e.what()); return;
    }
  }
  auto& s=_pickSlots[_syncSlot];
  s.request=_selection.TakePending();
  const auto extent=device.getSwapChainExtent();
  const auto request=*s.request;
  VkClearValue clear[2]{}; clear[0].color.uint32[0]=scene_selection::InvalidId;
  clear[1].depthStencil={0,0};
  VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
  begin.renderPass=_pickPass; begin.framebuffer=s.framebuffer;
  begin.renderArea={{int32_t(request.x),int32_t(request.y)},{1,1}};
  begin.clearValueCount=2; begin.pClearValues=clear;
  vkCmdBeginRenderPass(cmd,&begin,VK_SUBPASS_CONTENTS_INLINE);
  VkViewport viewport{0,0,float(extent.width),float(extent.height),0,1};
  vkCmdSetViewport(cmd,0,1,&viewport); vkCmdSetScissor(cmd,0,1,&begin.renderArea);
  VkBuffer vertex[]={applVertexBuffer,applNormalBuffer,applTangentBuffer,applUVBuffer};
  VkDeviceSize offsets[]={0,0,0,0};
  vkCmdBindVertexBuffers(cmd,0,4,vertex,offsets);
  vkCmdBindIndexBuffer(cmd,applIndexBuffer,0,VK_INDEX_TYPE_UINT32);
  VkDescriptorSet sets[]={globalDescriptorSets[currentFrame],applDescriptorSets[currentFrame]};
  vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,drawclusterBasePipelineLayout,0,2,sets,0,nullptr);
  uint32_t counts[]={uint32_t(applMesh->_opaqueChunkCount),uint32_t(applMesh->_alphaMaskedChunkCount),
                     uint32_t(applMesh->_transparentChunkCount)};
  VkDeviceSize offset=0;
  for(int i=0;i<3;++i) {
    if(counts[i] && (i!=2 || _selection.pickTransparent)) {
      vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,_pickPipelines[i]);
      vkCmdDrawIndexedIndirectCount(cmd,drawParamsBuffers[currentFrame],offset,writeIndexBuffers[currentFrame],i*4,
                                    counts[i],sizeof(VkDrawIndexedIndirectCommand));
    }
    offset+=VkDeviceSize(counts[i])*sizeof(VkDrawIndexedIndirectCommand);
  }
  vkCmdEndRenderPass(cmd);
  VkBufferImageCopy copy{}; copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};
  copy.imageOffset={int32_t(request.x),int32_t(request.y),0}; copy.imageExtent={1,1,1};
  vkCmdCopyImageToBuffer(cmd,s.id,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,s.readback,1,&copy);
  VkBufferMemoryBarrier barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
  barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT; barrier.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
  barrier.srcQueueFamilyIndex=barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
  barrier.buffer=s.readback; barrier.size=VK_WHOLE_SIZE;
  vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,0,nullptr,1,&barrier,0,nullptr);
  VkRect2D full{{0,0},extent}; vkCmdSetScissor(cmd,0,1,&full);
}

void GpuScene::consumePicking(uint32_t slot) {
  if (slot>=_pickSlots.size() || !_pickSlots[slot].request) return;
  auto& s=_pickSlots[slot];
  void* data=nullptr;
  VkResult result=vkMapMemory(device.getLogicalDevice(),s.readbackMemory,0,VK_WHOLE_SIZE,0,&data);
  if(result==VK_SUCCESS) {
    if(!s.coherent) {
      VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
      range.memory=s.readbackMemory; range.size=VK_WHOLE_SIZE;
      result=vkInvalidateMappedMemoryRanges(device.getLogicalDevice(),1,&range);
    }
    if(result==VK_SUCCESS) {
      uint32_t id; std::memcpy(&id,data,4); _selection.Complete(*s.request,id);

    }
    vkUnmapMemory(device.getLogicalDevice(),s.readbackMemory);
  }
  if(result!=VK_SUCCESS) _selection.BackendError("Picking readback failed");
  s.request.reset();
}

void GpuScene::destroyPicking() {
  _selection.Cancel();
  const auto dev=device.getLogicalDevice();
  for(auto& s:_pickSlots) {
    if(s.framebuffer) vkDestroyFramebuffer(dev,s.framebuffer,nullptr);
    if(s.idView) vkDestroyImageView(dev,s.idView,nullptr);
    if(s.depthView) vkDestroyImageView(dev,s.depthView,nullptr);
    if(s.id) vkDestroyImage(dev,s.id,nullptr);
    if(s.depth) vkDestroyImage(dev,s.depth,nullptr);
    if(s.idMemory) vkFreeMemory(dev,s.idMemory,nullptr);
    if(s.depthMemory) vkFreeMemory(dev,s.depthMemory,nullptr);
    if(s.readback) vkDestroyBuffer(dev,s.readback,nullptr);
    if(s.readbackMemory) vkFreeMemory(dev,s.readbackMemory,nullptr);
  }
  _pickSlots.clear();
  for(auto& p:_pickPipelines) { if(p) vkDestroyPipeline(dev,p,nullptr); p=VK_NULL_HANDLE; }
  if(_pickPass) vkDestroyRenderPass(dev,_pickPass,nullptr);
  _pickPass=VK_NULL_HANDLE;
}

// The scene UI must be drawn after resolve, to the swapchain only, never history.
void GpuScene::createSelectionUiPass() {
  const auto dev=device.getLogicalDevice();
  VkAttachmentDescription color{};
  color.format=device.getSwapChainImageFormat(); color.samples=VK_SAMPLE_COUNT_1_BIT;
  color.loadOp=VK_ATTACHMENT_LOAD_OP_LOAD; color.storeOp=VK_ATTACHMENT_STORE_OP_STORE;
  color.stencilLoadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE; color.stencilStoreOp=VK_ATTACHMENT_STORE_OP_DONT_CARE;
  color.initialLayout=color.finalLayout=VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
  VkAttachmentReference ref{0,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
  VkSubpassDescription sub{}; sub.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS;
  sub.colorAttachmentCount=1; sub.pColorAttachments=&ref;
  VkSubpassDependency dep{}; dep.srcSubpass=VK_SUBPASS_EXTERNAL; dep.dstSubpass=0;
  dep.srcStageMask=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
  dep.srcAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
  dep.dstStageMask=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  dep.dstAccessMask=VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  VkRenderPassCreateInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
  rp.attachmentCount=1; rp.pAttachments=&color; rp.subpassCount=1; rp.pSubpasses=&sub;
  rp.dependencyCount=1; rp.pDependencies=&dep;
  pickCheck(vkCreateRenderPass(dev,&rp,nullptr,&_selectionUiPass));
  auto extent=device.getSwapChainExtent();
  _selectionUiFrames.resize(device.getSwapChainImageCount());
  for(uint32_t i=0;i<_selectionUiFrames.size();++i) {
    auto view=device.getSwapChainImageView(i);
    VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fi.renderPass=_selectionUiPass; fi.attachmentCount=1; fi.pAttachments=&view;
    fi.width=extent.width; fi.height=extent.height; fi.layers=1;
    pickCheck(vkCreateFramebuffer(dev,&fi,nullptr,&_selectionUiFrames[i]));
  }
  if(_imguiInitialized) {
    ImGui_ImplVulkan_PipelineInfo info{}; info.RenderPass=_selectionUiPass; info.MSAASamples=VK_SAMPLE_COUNT_1_BIT;
    ImGui_ImplVulkan_CreateMainPipeline(&info);
  }
}
void GpuScene::destroySelectionUiPass() {
  for(auto f:_selectionUiFrames) vkDestroyFramebuffer(device.getLogicalDevice(),f,nullptr);
  _selectionUiFrames.clear();
  if(_selectionUiPass) vkDestroyRenderPass(device.getLogicalDevice(),_selectionUiPass,nullptr);
  _selectionUiPass=VK_NULL_HANDLE;
}
void GpuScene::recordSelectionUi(VkCommandBuffer cmd,uint32_t index) {
  if(!_imguiInitialized) return;
  VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
  begin.renderPass=_selectionUiPass; begin.framebuffer=_selectionUiFrames[index];
  begin.renderArea.extent=device.getSwapChainExtent();
  vkCmdBeginRenderPass(cmd,&begin,VK_SUBPASS_CONTENTS_INLINE);
  renderImGuiOverlay(cmd,index);
  vkCmdEndRenderPass(cmd);
}
