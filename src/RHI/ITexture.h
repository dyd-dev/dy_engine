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
	DY_RHI_ENABLE_ENUM_FLAGS(TextureUsage)

	// Descriptor for creating a texture
	struct TextureDesc {
		uint32_t width = 0;
		uint32_t height = 0;
		uint32_t depthOrArraySize = 1;
		uint32_t mipLevels = 1;
		Format format = Format::Unknown;
		TextureUsage usage = {};
	};

	class ITexture
	{
	public:
		virtual ~ITexture() = default;

		[[nodiscard]] const TextureDesc& GetDesc() const { return m_desc; }
		[[nodiscard]] uint32_t GetWidth() const { return m_desc.width; }
		[[nodiscard]] uint32_t GetHeight() const { return m_desc.height; }
		[[nodiscard]] uint32_t GetDepthOrArraySize() const { return m_desc.depthOrArraySize; }
		[[nodiscard]] uint32_t GetMipLevels() const { return m_desc.mipLevels; }
		[[nodiscard]] Format GetFormat() const { return m_desc.format; }
		[[nodiscard]] TextureUsage GetUsage() const { return m_desc.usage; }

	protected:
		explicit ITexture(const TextureDesc& desc) : m_desc(desc) {}
		void SetDesc(const TextureDesc& desc) { m_desc = desc; }

	private:
		TextureDesc m_desc = {};
	};
}
