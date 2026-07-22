#pragma once

#include <cstdint>

#include "Format.h"

namespace dy::RHI
{
	enum class ShaderStageFlags : uint32_t
	{
		None = 0,
		Vertex = 1u << 0,
		Fragment = 1u << 1,
		AllGraphics = (1u << 0) | (1u << 1)
	};
	DY_RHI_ENABLE_ENUM_FLAGS(ShaderStageFlags)

	enum class ResourceType : uint32_t
	{
		ConstantBuffer,
		StorageBuffer,
		TextureSampler
	};

	struct ResourceBindingDesc
	{
		uint32_t set = 0;
		uint32_t binding = 0;
		ResourceType type = ResourceType::ConstantBuffer;
		uint32_t arrayCount = 1;
		ShaderStageFlags stages = ShaderStageFlags::AllGraphics;
	};

	struct InlineConstantRangeDesc
	{
		uint32_t binding = 0;
		uint32_t offset = 0;
		uint32_t size = 0;
		ShaderStageFlags stages = ShaderStageFlags::AllGraphics;
	};

	struct PipelineLayoutDesc
	{
		const ResourceBindingDesc* resourceBindings = nullptr;
		uint32_t resourceBindingCount = 0;
		const InlineConstantRangeDesc* inlineConstantRanges = nullptr;
		uint32_t inlineConstantRangeCount = 0;
	};
}
