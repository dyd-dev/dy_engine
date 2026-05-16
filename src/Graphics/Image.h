#pragma once
#include <cstdint>
#include <vector>

namespace dy::Core
{
	class Image
	{
	public:
		Image() = default;

		Image(uint32_t width, uint32_t height, std::vector<uint8_t> pixels)
			: m_width(width), m_height(height), m_pixels(std::move(pixels))
		{
		}

		[[nodiscard]] bool IsValid() const { return m_width > 0u && m_height > 0u && !m_pixels.empty(); }
		[[nodiscard]] uint32_t GetWidth() const { return m_width; }
		[[nodiscard]] uint32_t GetHeight() const { return m_height; }
		[[nodiscard]] uint32_t GetRowPitch() const { return m_width * 4u; }
		[[nodiscard]] const std::vector<uint8_t>& GetPixels() const { return m_pixels; }

	private:
		uint32_t m_width = 0;
		uint32_t m_height = 0;
		std::vector<uint8_t> m_pixels;
	};
}
