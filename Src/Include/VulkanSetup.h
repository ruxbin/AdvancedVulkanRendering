#pragma once
#define VK_USE_PLATFORM_WIN32_KHR
#include<vector>
#include<string_view>
#include<set>
#include<optional>
#include<limits>
#include<algorithm>
#include "vulkan/vulkan.h"
#include "vulkan/vulkan_win32.h"
#include "Common.h"

struct SDL_Window;

class VulkanDevice
{
    public:
        VulkanDevice(SDL_Window*);
        VulkanDevice(const VulkanDevice&)=delete;
        VulkanDevice() = delete;
        VkPhysicalDevice getPhysicalDevice() const
        {
            return physicalDevice;
        }
        VkDevice getLogicalDevice() const
        {
            return device;
        }
    private:
        constexpr std::vector<std::string_view> getRequiredExtensions();
        VkInstance vkInstance;
        VkDevice device;
        VkQueue graphicsQueue;
        VkQueue presentQueue;
        VkSurfaceKHR win32Surface;
        VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;

        VkSwapchainKHR swapChain;
        std::vector<VkImage> swapChainImages;
        VkFormat swapChainImageFormat;
        VkExtent2D swapChainExtent;
        std::vector<VkImageView> swapChainImageViews;
        std::vector<VkFramebuffer> swapChainFramebuffers;

        VkImage depthImage;
        VkDeviceMemory depthImageMemory;
        VkImageView depthImageView;

        VkCommandPool commandPool;
        VkCommandBuffer commandBuffer;


        constexpr static const char* const instaceExtensionNames[] = { "VK_KHR_surface","VK_KHR_win32_surface" };
        constexpr static const char* const deviceExtensionNames[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
        constexpr static const char* const validationLayers[] = { "VK_LAYER_RENDERDOC_Capture","VK_LAYER_KHRONOS_synchronization2","VK_LAYER_KHRONOS_validation" };

        bool checkValidationLayerSupport();
        void pickPhysicalDevice();
        void createLogicalDevice();

        struct SwapChainSupportDetails {
            VkSurfaceCapabilitiesKHR capabilities;
            std::vector<VkSurfaceFormatKHR> formats;
            std::vector<VkPresentModeKHR> presentModes;
        };

        struct QueueFamilyIndices {
            std::optional<uint32_t> graphicsFamily;
            std::optional<uint32_t> presentFamily;

            bool isComplete() {
                return graphicsFamily.has_value() && presentFamily.has_value();
            }
        };

        SwapChainSupportDetails querySwapChainSupport(VkPhysicalDevice device) {
            SwapChainSupportDetails details;

            vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device, win32Surface, &details.capabilities);

            uint32_t formatCount;
            vkGetPhysicalDeviceSurfaceFormatsKHR(device, win32Surface, &formatCount, nullptr);

            if (formatCount != 0) {
                details.formats.resize(formatCount);
                vkGetPhysicalDeviceSurfaceFormatsKHR(device, win32Surface, &formatCount, details.formats.data());
            }

            uint32_t presentModeCount;
            vkGetPhysicalDeviceSurfacePresentModesKHR(device, win32Surface, &presentModeCount, nullptr);

            if (presentModeCount != 0) {
                details.presentModes.resize(presentModeCount);
                vkGetPhysicalDeviceSurfacePresentModesKHR(device, win32Surface, &presentModeCount, details.presentModes.data());
            }

            return details;
        }

        bool isDeviceSuitable(VkPhysicalDevice device) {
            QueueFamilyIndices indices = findQueueFamilies(device);

            bool extensionsSupported = checkDeviceExtensionSupport(device);

            bool swapChainAdequate = false;
            if (extensionsSupported) {
                SwapChainSupportDetails swapChainSupport = querySwapChainSupport(device);
                swapChainAdequate = !swapChainSupport.formats.empty() && !swapChainSupport.presentModes.empty();
            }

            return indices.isComplete() && extensionsSupported && swapChainAdequate;
        }

        bool checkDeviceExtensionSupport(VkPhysicalDevice device) {
            uint32_t extensionCount;
            vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, nullptr);

            std::vector<VkExtensionProperties> availableExtensions(extensionCount);
            vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, availableExtensions.data());

            int requireExtensionCount = sizeof(deviceExtensionNames) / sizeof(deviceExtensionNames[0]);

            for (const auto& extension : availableExtensions) {
                //requiredExtensions.erase(extension.extensionName);
                for (auto& i : deviceExtensionNames)
                {
                    if (strcmp(i, extension.extensionName) == 0)
                    {
                        --requireExtensionCount;
                        if (requireExtensionCount == 0)
                            return true;
                    }
                }
            }

            return false;
        }

        QueueFamilyIndices findQueueFamilies(VkPhysicalDevice device) {
            QueueFamilyIndices indices;

            uint32_t queueFamilyCount = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, nullptr);

            std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
            vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, queueFamilies.data());

            int i = 0;
            for (const auto& queueFamily : queueFamilies) {
                if (queueFamily.queueFlags & VK_QUEUE_GRAPHICS_BIT) {
                    indices.graphicsFamily = i;
                }

                VkBool32 presentSupport = false;
                vkGetPhysicalDeviceSurfaceSupportKHR(device, i, win32Surface, &presentSupport);

                if (presentSupport) {
                    indices.presentFamily = i;
                }

                if (indices.isComplete()) {
                    break;
                }

                i++;
            }

            return indices;
        }

        void createSwapChain();
        void createImageViews();
        void createCommandPool();

        VkSurfaceFormatKHR chooseSwapSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& availableFormats);

        VkPresentModeKHR chooseSwapPresentMode(const std::vector<VkPresentModeKHR>& availablePresentModes) {
            for (const auto& availablePresentMode : availablePresentModes) {
                if (availablePresentMode == VK_PRESENT_MODE_FIFO_KHR) {
                    return availablePresentMode;
                }
            }

            return VK_PRESENT_MODE_FIFO_KHR;
        }

        VkExtent2D chooseSwapExtent(const VkSurfaceCapabilitiesKHR& capabilities) {
            //constexpr uint32_t maxuint = std::numeric_limits<uint32_t>::max();//TODO will error
            constexpr uint32_t maxuint = 0xffffffff;
            if (capabilities.currentExtent.width != maxuint) {
                return capabilities.currentExtent;
            }
            else {
                //int width, height;
                //glfwGetFramebufferSize(window, &width, &height);
                int width = WINDOW_WIDTH;
                int height = WINDOW_HEIGHT;
                VkExtent2D actualExtent = {
                    static_cast<uint32_t>(width),
                    static_cast<uint32_t>(height)
                };

                actualExtent.width = std::clamp(actualExtent.width, capabilities.minImageExtent.width, capabilities.maxImageExtent.width);
                actualExtent.height = std::clamp(actualExtent.height, capabilities.minImageExtent.height, capabilities.maxImageExtent.height);

                return actualExtent;
            }
        }

        VkFormat findSupportedFormat(const std::vector<VkFormat>& candidates, VkImageTiling tiling, VkFormatFeatureFlags features) {
            for (VkFormat format : candidates) {
                VkFormatProperties props;
                vkGetPhysicalDeviceFormatProperties(physicalDevice, format, &props);
                if (tiling == VK_IMAGE_TILING_LINEAR && (props.linearTilingFeatures & features) == features) {
                    return format;
                }
                else if (tiling == VK_IMAGE_TILING_OPTIMAL && (props.optimalTilingFeatures & features) == features) {
                    return format;
                }
            }
            throw std::runtime_error("failed to find supported format!");
        }

        VkFormat findDepthFormat() {
            return findSupportedFormat(
                { VK_FORMAT_D32_SFLOAT, VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_D24_UNORM_S8_UINT },
                VK_IMAGE_TILING_OPTIMAL,
                VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT
            );
        }

        void CreateDepthResource() {
            VkFormat depthFormat = findDepthFormat();
            createImage(swapChainExtent.width, swapChainExtent.height, depthFormat,
                VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, depthImage, depthImageMemory);
            //depthImageView = createImageView(depthImage, depthFormat);
            VkImageViewCreateInfo createInfo{};
            createInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            createInfo.image = depthImage;
            createInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
            createInfo.format = depthFormat;

            createInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
            createInfo.subresourceRange.baseMipLevel = 0;
            createInfo.subresourceRange.levelCount = 1;
            createInfo.subresourceRange.baseArrayLayer = 0;
            createInfo.subresourceRange.layerCount = 1;

            if (vkCreateImageView(device, &createInfo, nullptr, &depthImageView) != VK_SUCCESS) {
                throw std::runtime_error("failed to create depthimage views!");
            }

            transitionImageLayout(depthImage, depthFormat, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
        }

        void createImage(uint32_t width, uint32_t height, VkFormat format, VkImageTiling tiling, VkImageUsageFlags usage, VkMemoryPropertyFlags properties, VkImage& image, VkDeviceMemory& imageMemory) {
            VkImageCreateInfo imageInfo{};
            imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
            imageInfo.imageType = VK_IMAGE_TYPE_2D;
            imageInfo.extent.width = width;
            imageInfo.extent.height = height;
            imageInfo.extent.depth = 1;
            imageInfo.mipLevels = 1;
            imageInfo.arrayLayers = 1;
            imageInfo.format = format;
            imageInfo.tiling = tiling;
            imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            imageInfo.usage = usage;
            imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
            imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

            if (vkCreateImage(device, &imageInfo, nullptr, &image) != VK_SUCCESS) {
                throw std::runtime_error("failed to create image!");
            }

            VkMemoryRequirements memRequirements;
            vkGetImageMemoryRequirements(device, image, &memRequirements);

            VkMemoryAllocateInfo allocInfo{};
            allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            allocInfo.allocationSize = memRequirements.size;
            allocInfo.memoryTypeIndex = findMemoryType(memRequirements.memoryTypeBits, properties);

            if (vkAllocateMemory(device, &allocInfo, nullptr, &imageMemory) != VK_SUCCESS) {
                throw std::runtime_error("failed to allocate image memory!");
            }

            vkBindImageMemory(device, image, imageMemory, 0);
        }

        uint32_t findMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties) {
            VkPhysicalDeviceMemoryProperties memProperties;
            vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memProperties);
            for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++) {
                if ((typeFilter & (1 << i)) && (memProperties.memoryTypes[i].propertyFlags & properties) == properties) {
                    return i;
                }
            }

            throw std::runtime_error("failed to find suitable memory type!");
        }

        void transitionImageLayout(VkImage image, VkFormat format, VkImageLayout oldLayout, VkImageLayout newLayout);

        VkCommandBuffer beginSingleTimeCommands();
        void endSingleTimeCommands(VkCommandBuffer commandBuffer);

};