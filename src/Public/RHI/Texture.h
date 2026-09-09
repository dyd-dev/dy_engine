#pragma once
#include <cstdint>
#include "Format.h"

namespace dy::RHI
{
	// Texture binding usages (Bitmask)
	enum class TextureUsage : uint32_t {
		None				= 0,
		ShaderResource		= 1 << 0,
		RenderTarget		= 1 << 1,
		DepthStencil		= 1 << 2,
		Storage				= 1 << 3,
	};

	inline constexpr TextureUsage operator|(TextureUsage left, TextureUsage right)
	{
		return static_cast<TextureUsage>(static_cast<uint32_t>(left) | static_cast<uint32_t>(right));
	}

	inline constexpr TextureUsage operator&(TextureUsage left, TextureUsage right)
	{
		return static_cast<TextureUsage>(static_cast<uint32_t>(left) & static_cast<uint32_t>(right));
	}

	inline constexpr TextureUsage& operator|=(TextureUsage& left, TextureUsage right)
	{
		left = left | right;
		return left;
	}

	inline constexpr TextureUsage& operator&=(TextureUsage& left, TextureUsage right)
	{
		left = left & right;
		return left;
	}

	struct TextureSubresourceRange
	{
		uint32_t firstMipLevel = 0;
		uint32_t mipLevelCount = 0;
		uint32_t firstArrayLayer = 0;
		uint32_t arrayLayerCount = 0;
	};

	// Descriptor for creating a texture
	struct TextureDesc {
		uint32_t width = 0;
		uint32_t height = 0;
		uint32_t depthOrArraySize = 1;
		uint32_t mipLevels = 1;
		Format format = Format::Unknown;
		TextureUsage usage = {};
	};

	class Texture
	{
	public:
		[[nodiscard]] const TextureDesc& GetDesc() const { return m_desc; }

	protected:
		virtual ~Texture() = default;
		explicit Texture(const TextureDesc& desc) : m_desc(desc) {}
		void SetDesc(const TextureDesc& desc) { m_desc = desc; }

	private:
		TextureDesc m_desc = {};
	};
}
