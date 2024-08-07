#pragma once

#include "Matrix.h"
#include "Common.h"
#include "VulkanSetup.h"
#include <vector>

class GpuScene;
class LightCuller;

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

	friend class LightCuller;
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

#   define DEFAULT_LIGHT_CULLING_TILE_SIZE  (32)
#define LIGHT_CLUSTER_DEPTH (64)

#define MAX_LIGHTS_PER_TILE                 (64)

#define MAX_LIGHTS_PER_CLUSTER              (16)

class LightCuller
{
public:
	void ClusterLightForScreen(VkCommandBuffer&, const VulkanDevice& device, const GpuScene& gpuScene, uint32_t screen_width, uint32_t screen_heigt);
	void InitRHI(const VulkanDevice& , const GpuScene& gpuScene, uint32_t screen_width, uint32_t screen_heigt);
	LightCuller();
	VkImage GetXZDebugImage(){return _xzDebugImage;}
private:

	VkBuffer            _pointLightCullingDataBuffer;

	VkBuffer            _xzRangeBuffer;
	VkImage				_xzDebugImage;
	VkImageView			_xzDebugImageView;

	VkDescriptorSetLayout coarseCullSetLayout;
	VkDescriptorPool coarseCullDescriptorPool;
	VkDescriptorSet coarseCullDescriptorSet;

	VkPipelineLayout coarseCullPipelineLayout;
	VkPipeline coarseCullPipeline;
	VkPipeline clearDebugViewPipeline;
};
