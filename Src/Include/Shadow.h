#pragma once
#include "Matrix.h"
#include "VulkanSetup.h"
#include "GpuScene.h"
#include <vector>
#include <array>

#define SHADOW_CASCADE_COUNT 3
//VK_FORMAT_R32_SFLOAT won't work
#define SHADOW_FORMAT VK_FORMAT_D32_SFLOAT_S8_UINT

class GpuScene;

class Shadow
{
private:
	VkImage _shadowSlices;
	std::array<mat4, SHADOW_CASCADE_COUNT> _shadowViewMatrices;
	std::array<mat4, SHADOW_CASCADE_COUNT> _shadowProjectionMatrices;
	uint32_t _shadowResolution;
	VkImage _shadowMaps;
	std::array<VkImageView,SHADOW_CASCADE_COUNT> _shadowSliceViews;
	std::array<VkFramebuffer,SHADOW_CASCADE_COUNT> _shadowFrameBuffers;
	VkImageView _shadowSliceViewFull;
	VkRenderPass _shadowPass;
	VkPipelineLayout _shadowPassPipelineLayout;
	VkPipeline _shadowPassPipeline;
	VkPipeline _shadowPassPipelineAlphaMask;
	VkSampler _shadowMapSampler;
	void InitRHI(const VulkanDevice&,const GpuScene&);
public:
	Shadow(uint32_t shadowResolution) : _shadowResolution(shadowResolution)
	{
		
	}

	void CreateShadowSlices(const VulkanDevice&);
	void RenderShadowMap(VkCommandBuffer&,const GpuScene&,const VulkanDevice&);
	void UpdateShadowMatrices(const GpuScene&);
	friend class GpuScene;
};
