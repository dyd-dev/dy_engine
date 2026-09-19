#pragma once

#include <cstdint>
#include <limits>

#include "ResourceHandles.h"
#include "Texture.h"

namespace dyf::RHI
{
	enum class ShaderStageFlags : uint32_t
	{
		None = 0,
		Vertex = 1u << 0u,
		Fragment = 1u << 1u,
        Compute = 1u << 2u
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
        // 1보다 크면 세 필터를 모두 Linear로 지정한다. 지원 한도는 장치에서 조회한다.
		uint32_t maxAnisotropy = 1;
        // 0이 아닌 LOD bias의 지원 여부는 Supports(SamplerDesc)로 확인한다.
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
        // 시작 번호는 전체 레이아웃에서 유일해야 한다. 배열은 count개의 연속 번호를 사용한다.
        // 같은 종류의 슬롯 범위는 단계가 달라도 겹칠 수 없다:
        // 텍스처/읽기 전용 저장 버퍼, 저장 텍스처/읽기쓰기 버퍼, 상수 버퍼, 샘플러.
        // 인라인 상수의 번호는 상수 버퍼 슬롯과 공유한다.
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
