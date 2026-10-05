#include "VlmRuntime.h"
#include <cstring>
#include <stdexcept>
#include <spdlog/spdlog.h>

VlmRuntime::Buffer VlmRuntime::upload(const void* data, VkDeviceSize bytes, VkBufferUsageFlags usage) {
  Buffer b;
  auto dev=_device.getLogicalDevice();
  try {
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; bi.size=bytes; bi.usage=usage;
    if(vkCreateBuffer(dev,&bi,nullptr,&b.buffer)!=VK_SUCCESS) throw std::runtime_error("VLM buffer allocation failed");
    VkMemoryRequirements req; vkGetBufferMemoryRequirements(dev,b.buffer,&req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; ai.allocationSize=req.size;
    ai.memoryTypeIndex=_device.findMemoryType(req.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if(vkAllocateMemory(dev,&ai,nullptr,&b.memory)!=VK_SUCCESS || vkBindBufferMemory(dev,b.buffer,b.memory,0)!=VK_SUCCESS) throw std::runtime_error("VLM memory allocation failed");
    void* p=nullptr;
    if(vkMapMemory(dev,b.memory,0,bytes,0,&p)!=VK_SUCCESS) throw std::runtime_error("VLM map failed");
    std::memcpy(p,data,static_cast<size_t>(bytes)); vkUnmapMemory(dev,b.memory);
    return b;
  } catch(...) { release(b); throw; }
}
void VlmRuntime::release(Buffer& b) {
  if(b.buffer) vkDestroyBuffer(_device.getLogicalDevice(),b.buffer,nullptr);
  if(b.memory) vkFreeMemory(_device.getLogicalDevice(),b.memory,nullptr);
  b={};
}
VlmRuntime::VlmRuntime(VulkanDevice& device,const SH9& physicalSky):_device(device) {
  try {
    VlmParams p{}; // probeTotal=0 is a shader guard independent of UI state.
    float zeros[8*28]{}; float sky[9][4]{};
    for(int j=0;j<9;++j) for(int c=0;c<3;++c) sky[j][c]=physicalSky.c[j][c];
    _fallbackParams=upload(&p,sizeof(p),VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
    _fallbackSh=upload(zeros,sizeof(zeros),VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    _sky=upload(sky,sizeof(sky),VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
  } catch(...) { release(_fallbackParams); release(_fallbackSh); release(_sky); throw; }
}
VlmRuntime::~VlmRuntime() { release(_params); release(_sh); release(_fallbackParams); release(_fallbackSh); release(_sky); }
bool VlmRuntime::LoadFromAsset(const VlmAssetData& asset) {
  VlmRuntimeData data;
  if(!VlmPrepareRuntimeData(asset,data)) { _active=false; spdlog::error("vlm: invalid runtime asset"); return false; }
  Buffer params, sh;
  try {
    params=upload(&data.params,sizeof(data.params),VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
    sh=upload(data.sh.data(),data.sh.size()*sizeof(float),VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    if(vkDeviceWaitIdle(_device.getLogicalDevice())!=VK_SUCCESS) throw std::runtime_error("VLM wait failed");
    release(_params); release(_sh); _params=params; _sh=sh;
    _probeCount=data.params.probeTotal; _active=true;
    spdlog::info("vlm: runtime activated ({} probes, {:.3f} MB SH)",_probeCount,data.sh.size()*sizeof(float)/1e6);
    return true;
  } catch(const std::exception& e) { release(params); release(sh); _active=false; spdlog::error("vlm: {}",e.what()); return false; }
}
bool VlmRuntime::LoadFromFile(const std::string& path,uint64_t expectedSceneHash) {
  VlmAssetData asset; std::string error;
  const auto result=VlmLoadAsset(path,asset,expectedSceneHash,error);
  if(result!=VlmLoadResult::Ok) { _active=false; spdlog::warn("vlm: {} asset disabled: {}",result==VlmLoadResult::Stale ? "stale" : "rejected",error); return false; }
  return LoadFromAsset(asset);
}
void VlmRuntime::writeSets(const std::vector<VkDescriptorSet>& sets,uint32_t first) const {
  VkDescriptorBufferInfo info[3]={{_active?_params.buffer:_fallbackParams.buffer,0,sizeof(VlmParams)},
    {_active?_sh.buffer:_fallbackSh.buffer,0,VK_WHOLE_SIZE},{_sky.buffer,0,9*16}};
  for(auto set:sets) {
    VkWriteDescriptorSet writes[3]{};
    for(uint32_t i=0;i<3;++i) {
      auto& w=writes[i]; w.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w.dstSet=set; w.dstBinding=first+i;
      w.descriptorCount=1; w.descriptorType=i==1?VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; w.pBufferInfo=&info[i];
    }
    vkUpdateDescriptorSets(_device.getLogicalDevice(),3,writes,0,nullptr);
  }
}
