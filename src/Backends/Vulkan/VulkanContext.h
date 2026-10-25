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
    bool maintenance4Supported = false;
};

struct VulkanContext {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    std::shared_ptr<const VulkanPhysicalDeviceInfo> physicalInfo;
    VkDevice device = VK_NULL_HANDLE;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkQueue graphicsQueue = VK_NULL_HANDLE;
    VkQueue presentQueue = VK_NULL_HANDLE;
    uint32_t apiVersion = VK_API_VERSION_1_0;
    bool modernRendering = false;
    bool negativeViewportSupported = false;
    PFN_vkCmdBeginRendering beginRendering = nullptr;
    PFN_vkCmdEndRendering endRendering = nullptr;
    PFN_vkCmdPipelineBarrier2 pipelineBarrier2 = nullptr;
    PFN_vkCmdWriteTimestamp2 writeTimestamp2 = nullptr;
    bool debugUtilsEnabled = false;
    PFN_vkGetCalibratedTimestampsEXT getCalibratedTimestamps = nullptr;
    uint64_t cpuTimestampFrequency = 0;
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
