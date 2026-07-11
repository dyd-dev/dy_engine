#include "VulkanRuntimePolicy.h"

#include <algorithm>
#include <cstring>
#include <limits>

namespace dy::Backends
{
	bool ValidateVulkanDeviceConfig(const RHI::DeviceDesc& config, uint32_t& outDescriptorSetCount)
	{
		outDescriptorSetCount = 0u;
		if(config.maxFramesInFlight == 0u || config.maxDrawsPerFrame == 0u) return false;
		const uint64_t descriptorSetCount = static_cast<uint64_t>(config.maxFramesInFlight)
			* static_cast<uint64_t>(config.maxDrawsPerFrame);
		if(descriptorSetCount > std::numeric_limits<uint32_t>::max()) return false;
		outDescriptorSetCount = static_cast<uint32_t>(descriptorSetCount);
		return true;
	}

	bool TryComputeVulkanDescriptorPageCapacity(
		const RHI::DeviceDesc& config,
		uint32_t maxPagesPerFrame,
		uint32_t& outFrameDescriptorCapacity,
		uint32_t& outDescriptorSetCount)
	{
		outFrameDescriptorCapacity = 0u;
		outDescriptorSetCount = 0u;
		uint32_t baseDescriptorSetCount = 0u;
		if(maxPagesPerFrame == 0u || !ValidateVulkanDeviceConfig(config, baseDescriptorSetCount)) return false;
		const uint64_t frameCapacity = static_cast<uint64_t>(config.maxDrawsPerFrame) * maxPagesPerFrame;
		const uint64_t descriptorSetCount = frameCapacity * config.maxFramesInFlight;
		if(frameCapacity > std::numeric_limits<uint32_t>::max()
			|| descriptorSetCount > std::numeric_limits<uint32_t>::max()) return false;
		outFrameDescriptorCapacity = static_cast<uint32_t>(frameCapacity);
		outDescriptorSetCount = static_cast<uint32_t>(descriptorSetCount);
		return true;
	}

	VulkanCapabilities BuildVulkanCapabilities(
		const VulkanDeviceLimits& limits,
		bool supportsBindlessDynamicIndexing,
		bool validationEnabled,
		VkQueueFlags graphicsQueueFlags)
	{
		VulkanCapabilities capabilities;
		capabilities.limits = limits;
		capabilities.supportsBindlessDynamicIndexing = supportsBindlessDynamicIndexing;
		capabilities.validationEnabled = validationEnabled;
		capabilities.supportsSkinningStorageBindings =
			limits.maxPerStageDescriptorStorageBuffers >= 4u
			&& limits.maxDescriptorSetStorageBuffers >= 4u
			&& limits.maxStorageBufferRange > 0u;
		capabilities.supportsComputeSkinning =
			capabilities.supportsSkinningStorageBindings
			&& limits.maxPushConstantsSize >= 8u
			&& (graphicsQueueFlags & VK_QUEUE_COMPUTE_BIT) != 0u;
		return capabilities;
	}

	uint32_t RequiredVulkanStorageBufferCount(RHI::GraphicsResourceProfile profile)
	{
		switch(profile)
		{
		case RHI::GraphicsResourceProfile::PerDrawSkin:
			return 4u;
		case RHI::GraphicsResourceProfile::Batched:
		case RHI::GraphicsResourceProfile::Bindless:
			return 3u;
		}
		return std::numeric_limits<uint32_t>::max();
	}

	bool SupportsVulkanResourceProfile(
		const VulkanCapabilities& capabilities,
		RHI::GraphicsResourceProfile profile)
	{
		const uint32_t requiredStorageBuffers = RequiredVulkanStorageBufferCount(profile);
		const bool supportsStorageBuffers =
			capabilities.limits.maxPerStageDescriptorStorageBuffers >= requiredStorageBuffers
			&& capabilities.limits.maxDescriptorSetStorageBuffers >= requiredStorageBuffers
			&& capabilities.limits.maxStorageBufferRange > 0u;
		if(!supportsStorageBuffers) return false;
		return profile != RHI::GraphicsResourceProfile::Bindless
			|| capabilities.supportsBindlessDynamicIndexing;
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
		uint32_t firstIndex,
		int32_t vertexOffset,
		uint32_t firstVertex,
		Graphics::RendererShaderLayout::DrawConstants& outConstants)
	{
		outConstants = {};
		if(capturedSize > 0u && capturedConstants == nullptr) return false;
		const size_t copySize = std::min<size_t>(capturedSize, sizeof(outConstants));
		if(copySize > 0u) std::memcpy(&outConstants, capturedConstants, copySize);
		outConstants.firstIndex = firstIndex;
		outConstants.vertexOffset = vertexOffset;
		outConstants.firstVertex = firstVertex;
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

	VulkanSubmissionDecision EvaluateVulkanSubmissionPreparation(
		bool frameReady,
		bool commandListValid,
		bool descriptorsUpdated,
		bool commandRecorded)
	{
		const bool ready = frameReady && commandListValid && descriptorsUpdated && commandRecorded;
		return VulkanSubmissionDecision{ ready, ready, ready };
	}

	VulkanQueueFailureAction EvaluateVulkanQueueFailure(VkResult result)
	{
		if(result == VK_SUCCESS) return VulkanQueueFailureAction::None;
		if(result == VK_ERROR_DEVICE_LOST) return VulkanQueueFailureAction::MarkDeviceLost;
		return VulkanQueueFailureAction::RecreateSignaledFence;
	}
}
