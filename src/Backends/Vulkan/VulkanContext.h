#pragma once
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define VK_USE_PLATFORM_WIN32_KHR
#endif
#include <vulkan/vulkan.h>
#include <memory>

namespace dyf::Backends
{

struct VulkanPhysicalDeviceInfo {
    VkPhysicalDeviceProperties properties = {};
    VkPhysicalDeviceFeatures features = {};
    VkQueueFamilyProperties graphicsQueue = {};
};

struct VulkanContext {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    std::shared_ptr<const VulkanPhysicalDeviceInfo> physicalInfo;
    VkDevice device = VK_NULL_HANDLE;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkQueue graphicsQueue = VK_NULL_HANDLE;
    VkQueue presentQueue = VK_NULL_HANDLE;
    bool debugUtilsEnabled = false;
    PFN_vkCmdBeginDebugUtilsLabelEXT beginDebugLabel = nullptr;
    PFN_vkCmdEndDebugUtilsLabelEXT endDebugLabel = nullptr;
    PFN_vkCmdInsertDebugUtilsLabelEXT insertDebugLabel = nullptr;

    struct QueueFamilyIndices {
        uint32_t graphicsFamily = UINT32_MAX;
        uint32_t presentFamily = UINT32_MAX;
        bool IsComplete() const { return graphicsFamily != UINT32_MAX && presentFamily != UINT32_MAX; }
    } queueIndices;

    bool meshShaderSupported = false;
    PFN_vkCmdDrawMeshTasksEXT vkCmdDrawMeshTasksEXT = nullptr;
};

}
