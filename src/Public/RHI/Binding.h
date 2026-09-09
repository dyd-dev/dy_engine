#pragma once

#include <cstdint>
#include <limits>

#include "ResourceHandles.h"
#include "Texture.h"

namespace dy::RHI
{
	enum class ShaderStageFlags : uint32_t
	{
		None = 0,
		Vertex = 1u << 0u,
		Fragment = 1u << 1u
	};

	inline constexpr ShaderStageFlags operator|(ShaderStageFlags left, ShaderStageFlags right)
	{
		return static_cast<ShaderStageFlags>(static_cast<uint32_t>(left) | static_cast<uint32_t>(right));
	}

	inline constexpr ShaderStageFlags operator&(ShaderStageFlags left, ShaderStageFlags right)
	{
		return static_cast<ShaderStageFlags>(static_cast<uint32_t>(left) & static_cast<uint32_t>(right));
	}

	enum class SamplerFilter : uint8_t
	{
		Undefined,
		Nearest,
		Linear
	};

	enum class SamplerAddressMode : uint8_t
	{
		Undefined,
		Repeat,
		MirroredRepeat,
		ClampToEdge,
		ClampToBorder
	};

	enum class SamplerBorderColor : uint8_t
	{
		Undefined,
		TransparentBlack,
		OpaqueBlack,
		OpaqueWhite
	};

	struct SamplerDesc
	{
		SamplerFilter minFilter = SamplerFilter::Undefined;
		SamplerFilter magFilter = SamplerFilter::Undefined;
		SamplerFilter mipFilter = SamplerFilter::Undefined;
		SamplerAddressMode addressU = SamplerAddressMode::Undefined;
		SamplerAddressMode addressV = SamplerAddressMode::Undefined;
		SamplerAddressMode addressW = SamplerAddressMode::Undefined;
		SamplerBorderColor borderColor = SamplerBorderColor::Undefined;
		uint32_t maxAnisotropy = 1;
		float mipLodBias = std::numeric_limits<float>::quiet_NaN();
		float minLod = std::numeric_limits<float>::quiet_NaN();
		float maxLod = std::numeric_limits<float>::quiet_NaN();
	};

	enum class ResourceBindingType : uint8_t
	{
		Undefined,
		ConstantBuffer,
		ReadOnlyStorageBuffer,
		ReadWriteStorageBuffer,
		SampledTexture,
		StorageTexture,
		StaticSampler
	};

	struct ResourceBindingLayout
	{
		uint32_t binding = 0;
		ResourceBindingType type = ResourceBindingType::Undefined;
		uint32_t count = 1;
		ShaderStageFlags stages = ShaderStageFlags::None;
		SamplerDesc staticSampler = {};
	};

	struct ResourceBinding
	{
		uint32_t binding = 0;
		uint32_t arrayElement = 0;
		BufferHandle buffer = nullptr;
		TextureHandle texture = nullptr;
		uint32_t offset = 0;
		uint32_t size = 0;
		TextureSubresourceRange subresources = {};
	};
}
