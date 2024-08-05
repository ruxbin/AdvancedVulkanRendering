#pragma once

#include "Matrix.h"
#include "Common.h"
#include "VulkanSetup.h"
#include <vector>

class GpuScene;

class Light
{
public:
	virtual void Draw(VkCommandBuffer& ,const GpuScene&)=0;
	//virtual void InitRHI(const VulkanDevice&, const GpuScene&)=0;
};


class PointLight : public Light
{
private:
	uint32_t _dynamicOffset = 0;
	const PointLightData* _pointLightData = nullptr;
public:
	virtual void Draw(VkCommandBuffer& ,const GpuScene&);
	static void InitRHI(const VulkanDevice&, const GpuScene&);
	PointLight(uint32_t dynamic_offset, const PointLightData* pld) : _dynamicOffset(dynamic_offset),_pointLightData(pld)
	{

	}
	const PointLightData* getPointLightData()const { return _pointLightData; }

	static VkBuffer pointLightDynamicUniformBuffer;

	static VkPipelineLayout drawPointLightPipelineLayout;
	static VkPipeline drawPointLightPipeline;
	static VkPipeline drawPointLightPipelineStencil;

	static VkDescriptorPool pointLightingDescriptorPool;
	static VkDescriptorSetLayout drawPointLightDescriptorSetLayout;
	static VkDescriptorSet drawPointLightDescriptorSet;
	static std::vector<PointLightData> pointLightData;


	static void CommonDrawSetup(VkCommandBuffer&);
};



class SpotLight : public Light
{
private:
	//VkBuffer coneBuffer;
	//VkBuffer coneIndexBuffer;
	const SpotLightData* _spotLightData = nullptr;
	uint32_t _dynamicOffset = 0;
	

public:
	virtual void Draw(VkCommandBuffer&, const GpuScene&) {}
	virtual void InitRHI(const VulkanDevice&, const GpuScene&) {}

	SpotLight(uint32_t dynamic_offset, const SpotLightData* sld) : _dynamicOffset(dynamic_offset), _spotLightData(sld) {}

	static std::vector<SpotLightData> spotLightData;
};