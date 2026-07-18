#pragma once

#include <cstdint>
#include <limits>

namespace dy::Backends
{
	[[nodiscard]] inline bool TryComputeVulkanDrawConstantCapacity(
		uint32_t maxDrawsPerFrame,
		uint32_t maxShadowViews,
		uint32_t& outCapacity)
	{
		outCapacity = 0u;
		if (maxDrawsPerFrame == 0u) return false;
		const uint64_t pageCount = static_cast<uint64_t>(maxShadowViews) + 1u;
		const uint64_t capacity = static_cast<uint64_t>(maxDrawsPerFrame) * pageCount;
		if (capacity > std::numeric_limits<uint32_t>::max()) return false;
		outCapacity = static_cast<uint32_t>(capacity);
		return true;
	}

	[[nodiscard]] inline bool TryGetVulkanShadowDrawSlot(
		uint32_t drawIndex,
		uint32_t maxDrawsPerFrame,
		uint32_t shadowViewIndex,
		uint32_t maxShadowViews,
		uint32_t& outSlot)
	{
		outSlot = 0u;
		if (maxDrawsPerFrame == 0u || drawIndex >= maxDrawsPerFrame || shadowViewIndex >= maxShadowViews) return false;
		const uint64_t slot =
			(static_cast<uint64_t>(shadowViewIndex) + 1u) * maxDrawsPerFrame + drawIndex;
		if (slot > std::numeric_limits<uint32_t>::max()) return false;
		outSlot = static_cast<uint32_t>(slot);
		return true;
	}

	struct VulkanDrawConstantLayout
	{
		uint64_t stride = 0u;
		uint64_t totalSize = 0u;
		uint32_t capacity = 0u;

		[[nodiscard]] static bool TryCreate(
			uint64_t elementSize,
			uint64_t alignment,
			uint32_t requestedCapacity,
			VulkanDrawConstantLayout& outLayout)
		{
			outLayout = {};
			if (elementSize == 0u || requestedCapacity == 0u) return false;
			if (alignment == 0u) alignment = 1u;

			const uint64_t remainder = elementSize % alignment;
			const uint64_t padding = remainder == 0u ? 0u : alignment - remainder;
			if (elementSize > std::numeric_limits<uint64_t>::max() - padding) return false;
			const uint64_t alignedStride = elementSize + padding;
			if (alignedStride > std::numeric_limits<uint32_t>::max()) return false;
			if (alignedStride > std::numeric_limits<uint64_t>::max() / requestedCapacity) return false;

			const uint64_t requiredSize = alignedStride * requestedCapacity;
			if (requiredSize > std::numeric_limits<uint32_t>::max()) return false;

			outLayout.stride = alignedStride;
			outLayout.totalSize = requiredSize;
			outLayout.capacity = requestedCapacity;
			return true;
		}

		[[nodiscard]] bool TryGetDynamicOffset(uint32_t drawIndex, uint32_t& outOffset) const
		{
			outOffset = 0u;
			if (drawIndex >= capacity || stride == 0u) return false;
			const uint64_t offset = stride * drawIndex;
			if (offset > std::numeric_limits<uint32_t>::max()) return false;
			outOffset = static_cast<uint32_t>(offset);
			return true;
		}
	};
}
