#pragma once
#include "Matrix.h"
#ifdef _WIN32
#define VK_USE_PLATFORM_WIN32_KHR
#endif
#ifdef __gnu_linux__
#define VK_USE_PLATFORM_XLIB_KHR
#endif
#include "vulkan/vulkan.h"

#include <vector>
#include <cstdlib>
#include <cstddef>
#include <string>
const int WINDOW_WIDTH = 1224;
const int WINDOW_HEIGHT= 691;



//appl definitions
struct AAPLBoundingBox3
{
    alignas(16) vec3 min;
    alignas(16) vec3 max;
};


struct AAPLSphere
{
    vec4 data;//xyz center, w radius
};

struct alignas(16) AAPLMeshChunk
{
    AAPLBoundingBox3 boundingBox;
    vec4 normalDistribution;
    vec4 cluterMean;

    AAPLSphere boundingSphere;

    unsigned int materialIndex;
    unsigned int indexBegin;
    unsigned int indexCount;
};

std::vector<char> readFile(const std::string &filename); 

struct PerObjPush
{
    uint32_t matindex;
};


struct uniformBufferData {
	mat4 shadowProjectionMatrix0;
    	mat4 shadowViewMatrix0;

	mat4 shadowProjectionMatrix1;
    	mat4 shadowViewMatrix1;

	mat4 shadowProjectionMatrix2;
    	mat4 shadowViewMatrix2;

    mat4 projectionMatrix;
    mat4 viewMatrix;
    mat4 invViewMatrix;
    mat4 invViewProjectionMatrix;
    };

struct FrameConstants {
    alignas(16) vec3 sunDirection;
    alignas(16) vec3 sunColor;
    float wetness;
    float emissiveScale;
    float localLightIntensity;
    float nearPlane;
    float farPlane;
    alignas(16) vec2 physicalSize;
};


struct FrameData
{
    uniformBufferData camConstants;
    FrameConstants frameConstants;
};

#define M_PI_F 3.1415926535897932f

inline bool hasStencilComponent(VkFormat format) {
    return format == VK_FORMAT_D32_SFLOAT_S8_UINT ||
        format == VK_FORMAT_D24_UNORM_S8_UINT;
}

#define LIGHT_FOR_TRANSPARENT_FLAG (0x00000001)
//PointLightData is stored in the dynmaic uniform buffer, it requires that offset should be 64 bytes aligned when binding the descriptorset
struct alignas(64) PointLightData
{
    vec4 posSqrRadius;	// Position in XYZ, radius squared in W.
    vec3 color;			// RGB color of light.
    uint32_t flags;		// Optional flags. May include `LIGHT_FOR_TRANSPARENT_FLAG`.
    PointLightData(float x, float y, float z, float radius, float r, float g, float b, uint32_t f)
    {
        posSqrRadius = vec4(x, y, z, radius);
        color = vec3(r, g, b);
        flags = f;
    }
};




#define SPOT_LIGHT_INNER_SCALE (0.8f)

struct alignas(64) SpotLightData
{
    vec4    boundingSphere;     // Bounding sphere for quick visibility test.
    vec4    posAndHeight;       // Position in XYZ and height of spot in W.
    vec4    colorAndInnerAngle; // RGB color of light.
    vec4    dirAndOuterAngle;   // Direction in XYZ, cone angle in W.
    mat4  viewProjMatrix;     // View projection matrix to light space.
    uint32_t            flags;              // Optional flags. May include `LIGHT_FOR_TRANSPARENT_FLAG`.
    SpotLightData(const vec4& bs,const vec4& ph,const vec4&ci,const vec4&da,uint32_t f) :
        boundingSphere(bs),posAndHeight(ph),colorAndInnerAngle(ci),dirAndOuterAngle(da),flags(f){}
};

