#pragma once
#include "VulkanContext.h"
#include <vulkan/vulkan.h>

class VulkanResources {
public:
    static uint32_t FindMemoryType(VkPhysicalDevice physicalDevice, uint32_t typeFilter, VkMemoryPropertyFlags properties);
    
    static void CreateBuffer(const VulkanContext& context, VkDeviceSize size, VkBufferUsageFlags usage, 
                           VkMemoryPropertyFlags properties, VkBuffer& buffer, VkDeviceMemory& bufferMemory);
    
    static void CopyBuffer(const VulkanContext& context, VkCommandPool commandPool, VkBuffer srcBuffer, 
                         VkBuffer dstBuffer, VkDeviceSize size);
    
    static void CreateImage(const VulkanContext& context, uint32_t width, uint32_t height, VkFormat format, 
                          VkImageTiling tiling, VkImageUsageFlags usage, VkMemoryPropertyFlags properties, 
                          VkImage& image, VkDeviceMemory& imageMemory);
    
    static void TransitionImageLayout(const VulkanContext& context, VkCommandPool commandPool, VkImage image, 
                                    VkFormat format, VkImageLayout oldLayout, VkImageLayout newLayout);
    
    static void CopyBufferToImage(const VulkanContext& context, VkCommandPool commandPool, VkBuffer buffer, 
                                VkImage image, uint32_t width, uint32_t height);
    
    static VkImageView CreateImageView(VkDevice device, VkImage image, VkFormat format);

    static VkCommandBuffer BeginSingleTimeCommands(const VulkanContext& context, VkCommandPool commandPool);
    static void EndSingleTimeCommands(const VulkanContext& context, VkCommandPool commandPool, VkCommandBuffer commandBuffer);
};
