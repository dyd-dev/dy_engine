#pragma once

#include <cstdint>

#include <vulkan/vulkan.h>

#include "RHI/IDevice.h"
#include "RHI/IPipelineState.h"
#include "Graphics/RendererShaderLayout.h"

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
		bool supportsBindlessDynamicIndexing = false;
		bool validationEnabled = false;
		bool supportsSkinningStorageBindings = false;
		bool supportsComputeSkinning = false;
	};

	struct VulkanSubmissionDecision
	{
		bool resetFence = false;
		bool submit = false;
		bool present = false;
	};

	enum class VulkanQueueFailureAction : uint8_t
	{
		None,
		RecreateSignaledFence,
		MarkDeviceLost
	};

	[[nodiscard]] bool ValidateVulkanDeviceConfig(
		const RHI::DeviceDesc& config,
		uint32_t& outDescriptorSetCount);
	[[nodiscard]] bool TryComputeVulkanDescriptorPageCapacity(
		const RHI::DeviceDesc& config,
		uint32_t maxPagesPerFrame,
		uint32_t& outFrameDescriptorCapacity,
		uint32_t& outDescriptorSetCount);
	[[nodiscard]] VulkanCapabilities BuildVulkanCapabilities(
		const VulkanDeviceLimits& limits,
		bool supportsBindlessDynamicIndexing,
		bool validationEnabled,
		VkQueueFlags graphicsQueueFlags = VK_QUEUE_GRAPHICS_BIT);
	[[nodiscard]] uint32_t RequiredVulkanStorageBufferCount(RHI::GraphicsResourceProfile profile);
	[[nodiscard]] bool SupportsVulkanResourceProfile(
		const VulkanCapabilities& capabilities,
		RHI::GraphicsResourceProfile profile);
	[[nodiscard]] bool TryAlignVulkanUniformStride(
		uint64_t size,
		uint64_t alignment,
		uint64_t& outStride);
	[[nodiscard]] bool PrepareVulkanDrawConstants(
		const uint8_t* capturedConstants,
		uint32_t capturedSize,
		uint32_t firstIndex,
		int32_t vertexOffset,
		uint32_t firstVertex,
		Graphics::RendererShaderLayout::DrawConstants& outConstants);
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
	[[nodiscard]] VulkanSubmissionDecision EvaluateVulkanSubmissionPreparation(
		bool frameReady,
		bool commandListValid,
		bool descriptorsUpdated,
		bool commandRecorded);
	[[nodiscard]] VulkanQueueFailureAction EvaluateVulkanQueueFailure(VkResult result);
}
