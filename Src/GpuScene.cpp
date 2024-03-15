
#include "VulkanSetup.h"

#include "GpuScene.h"
#include <vector>

void GpuScene::init_descriptors()
    {

        //information about the binding.
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

        //we are going to have 1 binding
        setinfo.bindingCount = 1;
        //no flags
        setinfo.flags = 0;
        //point to the camera buffer binding
        setinfo.pBindings = &camBufferBinding;

        vkCreateDescriptorSetLayout(device.getLogicalDevice(), &setinfo, nullptr, &globalSetLayout);

        // other code ....
        //create a descriptor pool that will hold 10 uniform buffers
        std::vector<VkDescriptorPoolSize> sizes =
        {
            { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 10 }
        };

        VkDescriptorPoolCreateInfo pool_info = {};
        pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pool_info.flags = 0;
        pool_info.maxSets = 10;
        pool_info.poolSizeCount = (uint32_t)sizes.size();
        pool_info.pPoolSizes = sizes.data();

        vkCreateDescriptorPool(device.getLogicalDevice(), &pool_info, nullptr, &descriptorPool);

        VkDescriptorSetAllocateInfo allocInfo ={};
		allocInfo.pNext = nullptr;
		allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
		//using the pool we just set
		allocInfo.descriptorPool = descriptorPool;
		//only 1 descriptor
		allocInfo.descriptorSetCount = 1;
		//using the global data layout
		allocInfo.pSetLayouts = &globalSetLayout;

		vkAllocateDescriptorSets(device.getLogicalDevice(), &allocInfo, &globalDescriptor);

        //information about the buffer we want to point at in the descriptor
		VkDescriptorBufferInfo binfo;
		//it will be the camera buffer
		binfo.buffer = uniformBuffer;
		//at 0 offset
		binfo.offset = 0;
		//of the size of a camera data struct
		binfo.range = sizeof(uniformBufferData);

		VkWriteDescriptorSet setWrite = {};
		setWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		setWrite.pNext = nullptr;

		//we are going to write into binding number 0
		setWrite.dstBinding = 0;
		//of the global descriptor
		setWrite.dstSet = globalDescriptor;

		setWrite.descriptorCount = 1;
		//and the type is uniform buffer
		setWrite.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
		setWrite.pBufferInfo = &binfo;


		vkUpdateDescriptorSets(device.getLogicalDevice(), 1, &setWrite, 0, nullptr);
        
    }

GpuScene::GpuScene(std::string_view& filepath, const VulkanDevice& deviceref) : device(deviceref)
{

}

    void GpuScene::createVertexBuffer() {
        VkBufferCreateInfo bufferInfo{};
        bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.size = getVertexSize();
        bufferInfo.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        bufferInfo.flags = 0;
        if (vkCreateBuffer(device, &bufferInfo, nullptr, &vertexBuffer) != VK_SUCCESS) {
            throw std::runtime_error("failed to create vertex buffer!");
        }
        VkMemoryRequirements memRequirements;
        vkGetBufferMemoryRequirements(device, vertexBuffer, &memRequirements);

        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = memRequirements.size;
        allocInfo.memoryTypeIndex = findMemoryType(memRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

        if (vkAllocateMemory(device, &allocInfo, nullptr, &vertexBufferMemory) != VK_SUCCESS) {
            throw std::runtime_error("failed to allocate vertex buffer memory!");
        }
        vkBindBufferMemory(device, vertexBuffer, vertexBufferMemory, 0);
        void* data;
        vkMapMemory(device, vertexBufferMemory, 0, bufferInfo.size, 0, &data);
        memcpy(data, getRawVertexData(), (size_t) bufferInfo.size);
        vkUnmapMemory(device, vertexBufferMemory);
    }