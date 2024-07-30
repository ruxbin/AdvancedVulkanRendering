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

//PointLightData is stored in the dynmaic uniform buffer, it requires that offset should be 64 bytes aligned when binding the descriptorset
struct alignas(64) PointLightData
{
	vec4 posSqrRadius;	// Position in XYZ, radius squared in W.
	vec3 color;			// RGB color of light.
	uint32_t flags;		// Optional flags. May include `LIGHT_FOR_TRANSPARENT_FLAG`.
	PointLightData(float x, float y, float z, float radius, float r, float g, float b, uint32_t f)
	{
		posSqrRadius = vec4(x,y,z,radius);
		color = vec3(r, g, b);
		flags = f;
	}
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
	vec4    boundingSphere;     // Bounding sphere for quick visibility test.
	vec4    posAndHeight;       // Position in XYZ and height of spot in W.
	vec4    colorAndInnerAngle; // RGB color of light.
	vec4    dirAndOuterAngle;   // Direction in XYZ, cone angle in W.
	vec4	viewProjMatrix;     // View projection matrix to light space.
	uint32_t            flags;              // Optional flags. May include `LIGHT_FOR_TRANSPARENT_FLAG`.

	VkBuffer coneBuffer;
	VkBuffer coneIndexBuffer;
public:
	virtual void Draw(VkCommandBuffer&, const GpuScene&) {}
	virtual void InitRHI(const VulkanDevice&, const GpuScene&) {}
};