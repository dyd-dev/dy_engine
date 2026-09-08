#include "VulkanResources.h"
#include "VulkanRuntimePolicy.h"
#include <stdexcept>

namespace dy::Backends
{

uint32_t VulkanResources::FindMemoryType(VkPhysicalDevice physicalDevice, uint32_t typeFilter, VkMemoryPropertyFlags properties) {
    VkPhysicalDeviceMemoryProperties memProps;
    vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memProps);
    uint32_t memoryTypeIndex = 0u;
    if (!TryFindVulkanMemoryType(memProps, typeFilter, properties, memoryTypeIndex)) {
        throw std::runtime_error("failed to find compatible Vulkan memory type");
    }
    return memoryTypeIndex;
}

void VulkanResources::CreateBuffer(const VulkanContext& context, VkDeviceSize size, VkBufferUsageFlags usage, 
                                 VkMemoryPropertyFlags properties, VkBuffer& buffer, VkDeviceMemory& bufferMemory) {
	buffer = VK_NULL_HANDLE;
	bufferMemory = VK_NULL_HANDLE;
    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = size;
    bufferInfo.usage = usage;

    if (vkCreateBuffer(context.device, &bufferInfo, nullptr, &buffer) != VK_SUCCESS) {
        throw std::runtime_error("failed to create buffer!");
    }

    VkBufferMemoryRequirementsInfo2 memoryRequirementsInfo{};
    memoryRequirementsInfo.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_REQUIREMENTS_INFO_2;
    memoryRequirementsInfo.buffer = buffer;
    VkMemoryRequirements2 memReqs{};
    memReqs.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2;
    vkGetBufferMemoryRequirements2(context.device, &memoryRequirementsInfo, &memReqs);

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocInfo.allocationSize = memReqs.memoryRequirements.size;
	try {
		allocInfo.memoryTypeIndex = FindMemoryType(context.physicalDevice, memReqs.memoryRequirements.memoryTypeBits, properties);
	} catch (...) {
		vkDestroyBuffer(context.device, buffer, nullptr);
		buffer = VK_NULL_HANDLE;
		throw;
	}

    if (vkAllocateMemory(context.device, &allocInfo, nullptr, &bufferMemory) != VK_SUCCESS) {
        vkDestroyBuffer(context.device, buffer, nullptr);
        buffer = VK_NULL_HANDLE;
        throw std::runtime_error("failed to allocate buffer memory!");
    }

    VkBindBufferMemoryInfo bindInfo{};
    bindInfo.sType = VK_STRUCTURE_TYPE_BIND_BUFFER_MEMORY_INFO;
    bindInfo.buffer = buffer;
    bindInfo.memory = bufferMemory;
    if (vkBindBufferMemory2(context.device, 1, &bindInfo) != VK_SUCCESS) {
        vkFreeMemory(context.device, bufferMemory, nullptr);
        vkDestroyBuffer(context.device, buffer, nullptr);
        bufferMemory = VK_NULL_HANDLE;
        buffer = VK_NULL_HANDLE;
        throw std::runtime_error("failed to bind Vulkan buffer memory");
    }
}

void VulkanResources::CopyBuffer(const VulkanContext& context, VkCommandPool commandPool, VkBuffer srcBuffer,
                               VkBuffer dstBuffer, VkDeviceSize size) {
    VkCommandBuffer commandBuffer = BeginSingleTimeCommands(context, commandPool);
    VkBufferCopy2 copyRegion{};
    copyRegion.sType = VK_STRUCTURE_TYPE_BUFFER_COPY_2;
    copyRegion.size = size;
    VkCopyBufferInfo2 copyInfo{};
    copyInfo.sType = VK_STRUCTURE_TYPE_COPY_BUFFER_INFO_2;
    copyInfo.srcBuffer = srcBuffer;
    copyInfo.dstBuffer = dstBuffer;
    copyInfo.regionCount = 1;
    copyInfo.pRegions = &copyRegion;
    vkCmdCopyBuffer2(commandBuffer, &copyInfo);
    EndSingleTimeCommands(context, commandPool, commandBuffer);
}

void VulkanResources::CreateImage(const VulkanContext& context, uint32_t width, uint32_t height, VkFormat format, 
                                VkImageTiling tiling, VkImageUsageFlags usage, VkMemoryPropertyFlags properties, 
                                VkImage& image, VkDeviceMemory& imageMemory) {
	image = VK_NULL_HANDLE;
	imageMemory = VK_NULL_HANDLE;
    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = format;
    imageInfo.extent.width = width;
    imageInfo.extent.height = height;
    imageInfo.extent.depth = 1;
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = tiling;
    imageInfo.usage = usage;

    if (vkCreateImage(context.device, &imageInfo, nullptr, &image) != VK_SUCCESS) {
        throw std::runtime_error("failed to create image!");
    }

    VkImageMemoryRequirementsInfo2 memoryRequirementsInfo{};
    memoryRequirementsInfo.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2;
    memoryRequirementsInfo.image = image;
    VkMemoryRequirements2 memReqs{};
    memReqs.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2;
    vkGetImageMemoryRequirements2(context.device, &memoryRequirementsInfo, &memReqs);

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocInfo.allocationSize = memReqs.memoryRequirements.size;
	try {
		allocInfo.memoryTypeIndex = FindMemoryType(context.physicalDevice, memReqs.memoryRequirements.memoryTypeBits, properties);
	} catch (...) {
		vkDestroyImage(context.device, image, nullptr);
		image = VK_NULL_HANDLE;
		throw;
	}

    if (vkAllocateMemory(context.device, &allocInfo, nullptr, &imageMemory) != VK_SUCCESS) {
        vkDestroyImage(context.device, image, nullptr);
        image = VK_NULL_HANDLE;
        throw std::runtime_error("failed to allocate image memory!");
    }

    VkBindImageMemoryInfo bindInfo{};
    bindInfo.sType = VK_STRUCTURE_TYPE_BIND_IMAGE_MEMORY_INFO;
    bindInfo.image = image;
    bindInfo.memory = imageMemory;
    if (vkBindImageMemory2(context.device, 1, &bindInfo) != VK_SUCCESS) {
        vkFreeMemory(context.device, imageMemory, nullptr);
        vkDestroyImage(context.device, image, nullptr);
        imageMemory = VK_NULL_HANDLE;
        image = VK_NULL_HANDLE;
        throw std::runtime_error("failed to bind Vulkan image memory");
    }
}

void VulkanResources::TransitionImageLayout(const VulkanContext& context, VkCommandPool commandPool, VkImage image,
                                          VkImageLayout oldLayout, VkImageLayout newLayout) {
    VkCommandBuffer commandBuffer = BeginSingleTimeCommands(context, commandPool);

    VkImageMemoryBarrier2 barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = 1;

    VkPipelineStageFlags2 sourceStage;
    VkPipelineStageFlags2 destinationStage;

    if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED && newLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
        barrier.srcAccessMask = VK_ACCESS_2_NONE;
        barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        sourceStage = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
        destinationStage = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    } else if (oldLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL && newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
        barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
        sourceStage = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        destinationStage = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    } else {
        throw std::invalid_argument("unsupported layout transition!");
    }

    barrier.srcStageMask = sourceStage;
    barrier.dstStageMask = destinationStage;
    VkDependencyInfo dependencyInfo{};
    dependencyInfo.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependencyInfo.imageMemoryBarrierCount = 1;
    dependencyInfo.pImageMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(commandBuffer, &dependencyInfo);
    EndSingleTimeCommands(context, commandPool, commandBuffer);
}

void VulkanResources::CopyBufferToImage(const VulkanContext& context, VkCommandPool commandPool, VkBuffer buffer, 
                                      VkImage image, uint32_t width, uint32_t height) {
    VkCommandBuffer commandBuffer = BeginSingleTimeCommands(context, commandPool);
    VkBufferImageCopy2 region{};
    region.sType = VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2;
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = { width, height, 1 };
    VkCopyBufferToImageInfo2 copyInfo{};
    copyInfo.sType = VK_STRUCTURE_TYPE_COPY_BUFFER_TO_IMAGE_INFO_2;
    copyInfo.srcBuffer = buffer;
    copyInfo.dstImage = image;
    copyInfo.dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    copyInfo.regionCount = 1;
    copyInfo.pRegions = &region;
    vkCmdCopyBufferToImage2(commandBuffer, &copyInfo);
    EndSingleTimeCommands(context, commandPool, commandBuffer);
}

VkImageView VulkanResources::CreateImageView(VkDevice device, VkImage image, VkFormat format, VkImageAspectFlags aspectMask) {
    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = format;
    viewInfo.subresourceRange.aspectMask = aspectMask;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.layerCount = 1;
    VkImageView view;
    if (vkCreateImageView(device, &viewInfo, nullptr, &view) != VK_SUCCESS) {
        throw std::runtime_error("failed to create image view!");
    }
    return view;
}

VkCommandBuffer VulkanResources::BeginSingleTimeCommands(const VulkanContext& context, VkCommandPool commandPool) {
    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool = commandPool;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = 1;
	VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
	if(vkAllocateCommandBuffers(context.device, &allocInfo, &commandBuffer) != VK_SUCCESS) {
		throw std::runtime_error("failed to allocate Vulkan one-time command buffer");
	}
    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	if(vkBeginCommandBuffer(commandBuffer, &beginInfo) != VK_SUCCESS) {
		vkFreeCommandBuffers(context.device, commandPool, 1, &commandBuffer);
		throw std::runtime_error("failed to begin Vulkan one-time command buffer");
	}
    return commandBuffer;
}

void VulkanResources::EndSingleTimeCommands(const VulkanContext& context, VkCommandPool commandPool, VkCommandBuffer commandBuffer) {
	if(commandBuffer == VK_NULL_HANDLE) throw std::runtime_error("invalid Vulkan one-time command buffer");
	if(vkEndCommandBuffer(commandBuffer) != VK_SUCCESS) {
		vkFreeCommandBuffers(context.device, commandPool, 1, &commandBuffer);
		throw std::runtime_error("failed to end Vulkan one-time command buffer");
	}
    VkCommandBufferSubmitInfo commandBufferInfo{};
    commandBufferInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
    commandBufferInfo.commandBuffer = commandBuffer;
    VkSubmitInfo2 submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
    submitInfo.commandBufferInfoCount = 1;
    submitInfo.pCommandBufferInfos = &commandBufferInfo;
	const VkResult submitResult = vkQueueSubmit2(context.graphicsQueue, 1, &submitInfo, VK_NULL_HANDLE);
	if (submitResult != VK_SUCCESS) {
        vkFreeCommandBuffers(context.device, commandPool, 1, &commandBuffer);
		throw std::runtime_error("failed to submit Vulkan one-time command buffer");
    }
	const VkResult waitResult = vkQueueWaitIdle(context.graphicsQueue);
	if(waitResult != VK_SUCCESS) throw std::runtime_error("failed to wait for Vulkan one-time command buffer");
    vkFreeCommandBuffers(context.device, commandPool, 1, &commandBuffer);
}

}
