#pragma once

#include "RHI/UploadPool.h"
#include "VulkanContext.h"

#include <memory>

namespace dyf::Backends
{
    struct VulkanUploadPage
    {
        uint64_t capacity = 0;
        uint8_t* mapped = nullptr;
        VkDevice device = VK_NULL_HANDLE;
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;

        ~VulkanUploadPage()
        {
            if (mapped) vkUnmapMemory(device, memory);
            if (buffer) vkDestroyBuffer(device, buffer, nullptr);
            if (memory) vkFreeMemory(device, memory, nullptr);
        }
    };

    using VulkanUploadPool = RHI::Detail::UploadPagePool<VulkanUploadPage>;
    using VulkanUploadArena = RHI::Detail::UploadArena<VulkanUploadPage>;

    // The owner must release its commands and pool before destroying VkDevice.
    std::shared_ptr<VulkanUploadPool> CreateVulkanUploadPool(const VulkanContext& context);
}
