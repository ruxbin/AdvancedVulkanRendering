// Runs a small, real Vulkan compute kernel. No renderer/window/assets required.
#include <vulkan/vulkan.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>
#include "VlmEnvironment.h"
#include "SphericalHarmonics.h"

static void check(VkResult r) { if (r != VK_SUCCESS) throw std::runtime_error("Vulkan call failed: " + std::to_string(r)); }
int main(int argc, char** argv) {
  if (argc != 2 && argc != 3) return 2;
  const bool sampling = argc == 3;
  const bool environment = sampling && std::strcmp(argv[2],"environment")==0;
  try {
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO}; app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; ici.pApplicationInfo = &app;
    VkInstance instance; check(vkCreateInstance(&ici, nullptr, &instance));
    uint32_t n = 0; check(vkEnumeratePhysicalDevices(instance, &n, nullptr));
    if (!n) return 77;
    std::vector<VkPhysicalDevice> physical(n); check(vkEnumeratePhysicalDevices(instance, &n, physical.data()));
    VkPhysicalDevice gpu = physical[0];
    vkGetPhysicalDeviceQueueFamilyProperties(gpu, &n, nullptr);
    std::vector<VkQueueFamilyProperties> families(n); vkGetPhysicalDeviceQueueFamilyProperties(gpu, &n, families.data());
    uint32_t family = 0; while (family < n && !(families[family].queueFlags & VK_QUEUE_COMPUTE_BIT)) ++family;
    if (family == n) return 77;
    float priority = 1;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO}; qci.queueFamilyIndex = family; qci.queueCount = 1; qci.pQueuePriorities = &priority;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO}; dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qci;
    VkDevice device; check(vkCreateDevice(gpu, &dci, nullptr, &device));
    VkQueue queue; vkGetDeviceQueue(device, family, 0, &queue);
    constexpr size_t count = 64, outputBytes = count * 4 * sizeof(float);
    constexpr size_t fixtureStride = 16384, fixtureBytes = fixtureStride, bytes = outputBytes + 2*fixtureStride;
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; bci.size = bytes; bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    VkBuffer buffer; check(vkCreateBuffer(device, &bci, nullptr, &buffer));
    VkMemoryRequirements req; vkGetBufferMemoryRequirements(device, buffer, &req);
    VkPhysicalDeviceMemoryProperties memory; vkGetPhysicalDeviceMemoryProperties(gpu, &memory);
    uint32_t type = 0;
    const auto flags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    while (type < memory.memoryTypeCount && (!(req.memoryTypeBits & (1u << type)) || (memory.memoryTypes[type].propertyFlags & flags) != flags)) ++type;
    if (type == memory.memoryTypeCount) return 77;
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; mai.allocationSize = req.size; mai.memoryTypeIndex = type;
    VkDeviceMemory allocation; check(vkAllocateMemory(device, &mai, nullptr, &allocation)); check(vkBindBufferMemory(device, buffer, allocation, 0));
    float* fixture;
    check(vkMapMemory(device,allocation,0,bytes,0,reinterpret_cast<void**>(&fixture)));
    std::memset(fixture,0,bytes);
    for(unsigned z=0;z<3;++z) for(unsigned y=0;y<3;++y) for(unsigned x=0;x<3;++x) {
      unsigned index=(z*3+y)*3+x, base=outputBytes/sizeof(float)+index*28;
      for(unsigned c=0;c<3;++c) fixture[base+c]=float((1+x+2*y+4*z)*(c+1));
      fixture[base+27]=index ? 1.f : 0.f;
    }
    if(environment) {
      std::vector<float> image(64*32*4,1);
      for(int c=0;c<3;++c) image[(8*64+10)*4+c]=2048;
      VlmEnvironmentDistribution distribution;
      if(!VlmBuildEnvironmentDistribution(image.data(),64,32,distribution)) throw std::runtime_error("invalid test sky");
      std::memcpy(fixture+outputBytes/4,distribution.cdf.data(),distribution.cdf.size()*sizeof(float));
      const SH9 referenceSky=ComputeSH9FromEquirect(image.data(),64,32);
      for(int j=0;j<9;++j) for(int c=0;c<3;++c)
        fixture[(outputBytes+fixtureStride)/4+j*4+c]=referenceSky.c[j][c];
    }
    vkUnmapMemory(device,allocation);
    VkDescriptorSetLayoutBinding bindings[3]{};
    for(unsigned i=0;i<3;++i) bindings[i]={i,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};
    VkDescriptorSetLayoutCreateInfo slci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; slci.bindingCount=3; slci.pBindings=bindings;
    VkDescriptorSetLayout layout; check(vkCreateDescriptorSetLayout(device,&slci,nullptr,&layout));
    VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,3};
    VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO}; pci.maxSets=1; pci.poolSizeCount=1; pci.pPoolSizes=&size;
    VkDescriptorPool pool; check(vkCreateDescriptorPool(device,&pci,nullptr,&pool));
    VkDescriptorSetAllocateInfo sai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO}; sai.descriptorPool=pool; sai.descriptorSetCount=1; sai.pSetLayouts=&layout;
    VkDescriptorSet set; check(vkAllocateDescriptorSets(device,&sai,&set));
    VkDescriptorBufferInfo infos[3]={{buffer,0,outputBytes},{buffer,outputBytes,fixtureBytes},{buffer,outputBytes+fixtureStride,fixtureBytes}};
    VkWriteDescriptorSet writes[3]{};
    for(unsigned i=0;i<3;++i) { auto& write=writes[i]; write.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; write.dstSet=set; write.dstBinding=i; write.descriptorCount=1; write.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; write.pBufferInfo=&infos[i]; }
    vkUpdateDescriptorSets(device,3,writes,0,nullptr);
    std::ifstream file(argv[1],std::ios::binary | std::ios::ate);
    if (!file || file.tellg() <= 0 || size_t(file.tellg()) % 4) throw std::runtime_error("invalid SPIR-V file");
    std::vector<uint32_t> spv(size_t(file.tellg())/4); file.seekg(0); file.read(reinterpret_cast<char*>(spv.data()),spv.size()*4);
    VkShaderModuleCreateInfo smci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; smci.codeSize=spv.size()*4; smci.pCode=spv.data();
    VkShaderModule shader; check(vkCreateShaderModule(device,&smci,nullptr,&shader));
    VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; plci.setLayoutCount=1; plci.pSetLayouts=&layout;
    VkPipelineLayout pipelineLayout; check(vkCreatePipelineLayout(device,&plci,nullptr,&pipelineLayout));
    VkComputePipelineCreateInfo cpci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO}; cpci.layout=pipelineLayout;
    cpci.stage.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO; cpci.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT; cpci.stage.module=shader; cpci.stage.pName="main";
    VkPipeline pipeline; check(vkCreateComputePipelines(device,VK_NULL_HANDLE,1,&cpci,nullptr,&pipeline));
    VkCommandPoolCreateInfo cp{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; cp.queueFamilyIndex=family;
    VkCommandPool commandPool; check(vkCreateCommandPool(device,&cp,nullptr,&commandPool));
    VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO}; ca.commandPool=commandPool; ca.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY; ca.commandBufferCount=1;
    VkCommandBuffer cmd; check(vkAllocateCommandBuffers(device,&ca,&cmd));
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; check(vkBeginCommandBuffer(cmd,&begin));
    vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline);
    vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,pipelineLayout,0,1,&set,0,nullptr);
    vkCmdDispatch(cmd,count,1,1);
    VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER}; barrier.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT; barrier.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&barrier,0,nullptr,0,nullptr);
    check(vkEndCommandBuffer(cmd));
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO}; submit.commandBufferCount=1; submit.pCommandBuffers=&cmd;
    check(vkQueueSubmit(queue,1,&submit,VK_NULL_HANDLE)); check(vkQueueWaitIdle(queue));
    float* result; check(vkMapMemory(device,allocation,0,bytes,0,reinterpret_cast<void**>(&result)));
    double sampled[2]{}, reference[2]{}; bool finite=true;
    bool individualPass=true;
    if(sampling && !environment) for(size_t i=0;i<count;++i) for(int c=0;c<2;++c) {
      if(!std::isfinite(result[4*i+2*c]) || std::abs(result[4*i+2*c]-result[4*i+2*c+1])>0.0001f) {
        std::printf("Sampler lane %zu component %d actual=%f expected=%f\n",i,c,result[4*i+2*c],result[4*i+2*c+1]); individualPass=false;
      }
    }
    for (size_t i=0;i<count;++i) for(int c=0;c<2;++c) { sampled[c]+=result[4*i+2*c]; reference[c]+=result[4*i+2*c+1]; finite &= std::isfinite(result[4*i+2*c]) && std::isfinite(result[4*i+2*c+1]); }
    vkUnmapMemory(device,allocation);
    bool pass=finite && individualPass;
    for(int c=0;c<2;++c) {
      double error=std::abs(sampled[c]-reference[c])/std::fmax(0.01,std::abs(reference[c]));
      std::printf("BRDF case %d sampled=%.6f reference=%.6f relativeError=%.6f\n",c,sampled[c]/count,reference[c]/count,error);
      pass &= error < (environment ? 0.01 : 0.02);
    }
    vkDestroyCommandPool(device,commandPool,nullptr); vkDestroyPipeline(device,pipeline,nullptr); vkDestroyPipelineLayout(device,pipelineLayout,nullptr); vkDestroyShaderModule(device,shader,nullptr);
    vkDestroyDescriptorPool(device,pool,nullptr); vkDestroyDescriptorSetLayout(device,layout,nullptr); vkDestroyBuffer(device,buffer,nullptr); vkFreeMemory(device,allocation,nullptr);
    vkDestroyDevice(device,nullptr); vkDestroyInstance(instance,nullptr);
    return pass ? 0 : 1;
  } catch(const std::exception& e) { std::fprintf(stderr,"%s\n",e.what()); return 1; }
}
