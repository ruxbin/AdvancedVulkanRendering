#pragma once
#include "vulkan/vulkan.h"
#include "Matrix.h"
#include "VulkanSetup.h"
#include "GpuScene.h"
#include <vector>
#include <array>

#define SHADOW_CASCADE_COUNT 3
#define SHADOW_FORMAT VK_FORMAT_R32_SFLOAT;


class Shadow
{
private:
	VkImage _shadowSlices;
	std::vector<mat4> _shadowMatrices;
	int32_t _shadowResolution;
	VkImage _shadowMaps;
	std::array<VkImageView,SHADOW_CASCADE_COUNT> _shadowSliceViews;
	std::array<VkFramebuffer,SHADOW_CASCADE_COUNT> _shadowFrameBuffers;
	VkRenderPass _shadowPass;
	VkPipelineLayout _shadowPassPipelineLayout;
	VkPipeline _shadowPassPipeline;
	void InitRHI(const VulkanDevice&,const GpuScene&);
public:
	Shadow(int32_t shadowResolution) : _shadowResolution(shadowResolution)
	{
		
	}

	void CreateShadowSlices(const VulkanDevice&);
	void RenderShadowMap(VkCommandBuffer&,const GpuScene&,const VulkanDevice&);
	void UpdateShadowMatrices(const GpuScene&);
};
