#pragma once
#include <cstdint>

#define DY_RHI_ENABLE_ENUM_FLAGS(T) \
	inline constexpr T operator|(T a, T b) { return static_cast<T>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b)); } \
	inline constexpr T operator&(T a, T b) { return static_cast<T>(static_cast<uint32_t>(a) & static_cast<uint32_t>(b)); } \
	inline T& operator|=(T& a, T b) { a = a | b; return a; } \
	inline T& operator&=(T& a, T b) { a = a & b; return a; }

namespace dy::RHI
{
	// Texture, attachment, index, vertex attribute에 쓰는 데이터 포맷
	enum class Format : uint32_t {
		Unknown = 0,
		R8G8B8A8_UNORM,
		B8G8R8A8_UNORM,
		R8G8B8A8_UNORM_SRGB,   // 하드웨어가 선형→sRGB 인코딩(셰이더에서 수동 감마 금지)
		B8G8R8A8_UNORM_SRGB,
		R16G16B16A16_FLOAT,
		R32_FLOAT,
		R32G32_FLOAT,
		R32G32B32_FLOAT,
		R32G32B32A32_FLOAT,
		D32_FLOAT,
		D24_UNORM_S8_UINT,
		R32_UINT,
		R16_UINT
	};

	// sRGB(하드웨어 감마) 포맷인지. 셰이더 수동 감마 vs 하드웨어 감마 분기에 사용.
	[[nodiscard]] inline constexpr bool IsSrgbFormat(Format format)
	{
		return format == Format::R8G8B8A8_UNORM_SRGB || format == Format::B8G8R8A8_UNORM_SRGB;
	}

	using DescriptorIndex = uint32_t;
	static const DescriptorIndex INVALID_DESCRIPTOR_INDEX = static_cast<DescriptorIndex>(-1);

	using BindingIndex = uint32_t;
	static const BindingIndex INVALID_BINDING_INDEX = static_cast<BindingIndex>(-1);
}
