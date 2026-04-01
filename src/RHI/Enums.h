#pragma once
#include <cstdint>

// Helper macro to enable bitwise operations for strongly typed enums
#define DY_ENUM_FLAGS(T) \
	inline T operator|(T a, T b) { return static_cast<T>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b)); } \
	inline T operator&(T a, T b) { return static_cast<T>(static_cast<uint32_t>(a) & static_cast<uint32_t>(b)); }

namespace dy::RHI
{
	// Pixel formats for textures and render targets
	enum class Format : uint32_t {
		Unknown = 0,
		R8G8B8A8_UNORM,
		R16G16B16A16_FLOAT,
		R32G32B32A32_FLOAT,
		D32_FLOAT,
		D24_UNORM_S8_UINT
	};

	// Buffer binding usages (Bitmask)
	enum class BufferUsage : uint32_t {
		None		= 0,
		Vertex		= 1 << 0,
		Index		= 1 << 1,
		Constant	= 1 << 2,
		Storage		= 1 << 3,
		Indirect	= 1 << 4
	};

	// Texture binding usages (Bitmask)
	enum class TextureUsage : uint32_t {
		None				= 0,
		ShaderResource		= 1 << 0,
		RenderTarget		= 1 << 1,
		DepthStencil		= 1 << 2,
		UnorderedAccess		= 1 << 3,
	};

	// Resource barrier states for synchronization
	enum class ResourceState : uint32_t {
		Common,
		RenderTarget,
		DepthWrite,
		DepthRead,
		PixelShaderResource,
		ComputeWrite,
		IndirectArgument,
		Present
	};

	DY_ENUM_FLAGS(BufferUsage)
	DY_ENUM_FLAGS(TextureUsage)
}