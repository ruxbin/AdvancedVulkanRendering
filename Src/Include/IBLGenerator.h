#pragma once

// 与 Common.h/VulkanSetup.h 一致:vulkan.h 必须在平台宏定义后包含,
// 否则 VulkanSetup.h 直接包含 vulkan_win32.h 时缺少 windows.h 类型。
#ifdef __ANDROID__
#define VK_USE_PLATFORM_ANDROID_KHR
#elif defined(_WIN32)
#define VK_USE_PLATFORM_WIN32_KHR
#elif defined(__gnu_linux__)
#define VK_USE_PLATFORM_WAYLAND_KHR
#endif
#include <vulkan/vulkan.h>
#include <filesystem>

class VulkanDevice;

// IBLGenerator 的输出资源(所有权归调用方,负责最终销毁)。
struct IBLResources {
  VkImage envCube = VK_NULL_HANDLE;        // 256x256x6, 9 mips, R16G16B16A16_SFLOAT
  VkDeviceMemory envCubeMemory = VK_NULL_HANDLE;
  VkImageView envCubeView = VK_NULL_HANDLE; // VK_IMAGE_VIEW_TYPE_CUBE, 全 mip
  VkImage dfgLut = VK_NULL_HANDLE;          // 256x256 RG16F
  VkDeviceMemory dfgLutMemory = VK_NULL_HANDLE;
  VkImageView dfgLutView = VK_NULL_HANDLE;
  VkSampler sampler = VK_NULL_HANDLE;       // 三线性 + clamp,生成与光照共用
};

// 一次性 compute 链:equirect→cube mip0(Task 3)→ GGX 预过滤 mip1-8(Task 4)
// → DFG LUT(Task 5)。在单个 one-shot command buffer 内完成全部 dispatch 与
// layout 转换,返回时所有输出已处 SHADER_READ_ONLY_OPTIMAL。
class IBLGenerator {
public:
  static constexpr uint32_t kEnvMapSize = 256;
  static constexpr uint32_t kEnvMipCount = 9; // 256..1,lod = roughness * 8

  IBLResources generate(const VulkanDevice& device, VkImageView equirectView,
                        const std::filesystem::path& rootPath);
};
