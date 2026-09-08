#include "VulkanRuntimePolicy.h"

#include <algorithm>
#include <cstring>
#include <limits>

namespace dy::Backends
{
	bool TryComputeVulkanDescriptorCapacity(
		const RHI::DeviceDesc& config,
		uint32_t& outFrameDescriptorCapacity)
	{
		outFrameDescriptorCapacity = 0u;
		if(config.maxFramesInFlight == 0u || config.maxDrawsPerFrame == 0u) return false;
		const uint64_t descriptorSetCount =
			static_cast<uint64_t>(config.maxDrawsPerFrame) * config.maxFramesInFlight;
		if(descriptorSetCount > std::numeric_limits<uint32_t>::max()) return false;
		outFrameDescriptorCapacity = config.maxDrawsPerFrame;
		return true;
	}

	bool TryAlignVulkanUniformStride(uint64_t size, uint64_t alignment, uint64_t& outStride)
	{
		outStride = 0u;
		if(size == 0u) return false;
		if(alignment == 0u) alignment = 1u;
		const uint64_t remainder = size % alignment;
		if(remainder == 0u)
		{
			outStride = size;
			return true;
		}
		const uint64_t padding = alignment - remainder;
		if(size > std::numeric_limits<uint64_t>::max() - padding) return false;
		outStride = size + padding;
		return true;
	}

	bool PrepareVulkanDrawConstants(
		const uint8_t* capturedConstants,
		uint32_t capturedSize,
		const RHI::ShaderLayoutDesc& layout,
		uint32_t firstIndex,
		int32_t vertexOffset,
		uint32_t firstVertex,
		uint8_t* outConstants,
		uint32_t outCapacity)
	{
		const uint32_t constantSize = layout.pushConstantRangeSize;
		const uint32_t metadataOffset = layout.drawMetadataPushConstantOffset;
		if(outConstants == nullptr || constantSize == 0u || constantSize > outCapacity) return false;
		if(metadataOffset > constantSize || constantSize - metadataOffset < 3u * sizeof(uint32_t)) return false;
		if(capturedSize > 0u && capturedConstants == nullptr) return false;
		std::memset(outConstants, 0, constantSize);
		const size_t copySize = std::min<size_t>(capturedSize, constantSize);
		if(copySize > 0u) std::memcpy(outConstants, capturedConstants, copySize);
		std::memcpy(outConstants + metadataOffset, &firstIndex, sizeof(firstIndex));
		std::memcpy(outConstants + metadataOffset + sizeof(firstIndex), &vertexOffset, sizeof(vertexOffset));
		std::memcpy(outConstants + metadataOffset + sizeof(firstIndex) + sizeof(vertexOffset), &firstVertex, sizeof(firstVertex));
		return true;
	}

	bool ValidateStorageBufferBinding(
		const VulkanCapabilities& capabilities,
		uint64_t offset,
		uint64_t range,
		uint64_t bufferSize)
	{
		if(range == 0u || range > capabilities.limits.maxStorageBufferRange) return false;
		const uint64_t alignment = capabilities.limits.minStorageBufferOffsetAlignment;
		if(alignment > 1u && offset % alignment != 0u) return false;
		if(offset > bufferSize || range > bufferSize - offset) return false;
		return true;
	}

	bool ValidateUniformBufferBinding(
		const VulkanCapabilities& capabilities,
		uint64_t offset,
		uint64_t range,
		uint64_t bufferSize)
	{
		if(range == 0u || range > capabilities.limits.maxUniformBufferRange) return false;
		const uint64_t alignment = capabilities.limits.minUniformBufferOffsetAlignment;
		if(alignment > 1u && offset % alignment != 0u) return false;
		if(offset > bufferSize || range > bufferSize - offset) return false;
		return true;
	}

	bool TryFindVulkanMemoryType(
		const VkPhysicalDeviceMemoryProperties& memoryProperties,
		uint32_t typeFilter,
		VkMemoryPropertyFlags requiredProperties,
		uint32_t& outMemoryTypeIndex)
	{
		for(uint32_t memoryTypeIndex = 0u; memoryTypeIndex < memoryProperties.memoryTypeCount; ++memoryTypeIndex)
		{
			const bool allowed = (typeFilter & (1u << memoryTypeIndex)) != 0u;
			const VkMemoryPropertyFlags available = memoryProperties.memoryTypes[memoryTypeIndex].propertyFlags;
			if(allowed && (available & requiredProperties) == requiredProperties)
			{
				outMemoryTypeIndex = memoryTypeIndex;
				return true;
			}
		}
		return false;
	}

}
