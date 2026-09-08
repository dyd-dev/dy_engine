#pragma once

#include <cstdint>

#include <vulkan/vulkan.h>

#include "RHI/IDevice.h"

namespace dy::Backends
{
	struct VulkanDeviceLimits
	{
		uint32_t maxPushConstantsSize = 0u;
		uint32_t maxPerStageDescriptorStorageBuffers = 0u;
		uint32_t maxDescriptorSetStorageBuffers = 0u;
		uint64_t maxStorageBufferRange = 0u;
		uint64_t maxUniformBufferRange = 0u;
		uint64_t minUniformBufferOffsetAlignment = 1u;
		uint64_t minStorageBufferOffsetAlignment = 1u;
	};

	struct VulkanCapabilities
	{
		VulkanDeviceLimits limits;
		bool supportsComputeSkinning = false;
	};
	[[nodiscard]] bool TryComputeVulkanDescriptorCapacity(
		const RHI::DeviceDesc& config,
		uint32_t& outFrameDescriptorCapacity);
	[[nodiscard]] bool TryAlignVulkanUniformStride(
		uint64_t size,
		uint64_t alignment,
		uint64_t& outStride);
	[[nodiscard]] bool PrepareVulkanDrawConstants(
		const uint8_t* capturedConstants,
		uint32_t capturedSize,
		const RHI::ShaderLayoutDesc& layout,
		uint32_t firstIndex,
		int32_t vertexOffset,
		uint32_t firstVertex,
		uint8_t* outConstants,
		uint32_t outCapacity);
	[[nodiscard]] bool ValidateStorageBufferBinding(
		const VulkanCapabilities& capabilities,
		uint64_t offset,
		uint64_t range,
		uint64_t bufferSize);
	[[nodiscard]] bool ValidateUniformBufferBinding(
		const VulkanCapabilities& capabilities,
		uint64_t offset,
		uint64_t range,
		uint64_t bufferSize);
	[[nodiscard]] bool TryFindVulkanMemoryType(
		const VkPhysicalDeviceMemoryProperties& memoryProperties,
		uint32_t typeFilter,
		VkMemoryPropertyFlags requiredProperties,
		uint32_t& outMemoryTypeIndex);
}
