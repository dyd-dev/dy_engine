#pragma once
#include <cstdint>
#include "Enums.h"

namespace dy::RHI
{
	// Descriptor for creating a texture
	struct TextureDesc {
		uint32_t width;
		uint32_t height;
		uint32_t depthOrArraySize = 1;
		uint32_t mipLevels = 1;
		Format format;
		TextureUsage usage;
	};

	class ITexture
	{
	public:
		virtual ~ITexture() = default;

		virtual uint32_t GetWidth() const = 0;
		virtual uint32_t GetHeight() const = 0;
		virtual Format GetFormat() const = 0;
	};
}