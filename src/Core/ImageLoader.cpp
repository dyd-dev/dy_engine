#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include "Core/ImageLoader.h"

#include <cstdint>
#include <vector>

namespace dy::Core
{
	Image LoadImageFromFile(const std::string& filepath)
	{
		int width = 0;
		int height = 0;
		int channels = 0;
		uint8_t* data = stbi_load(filepath.c_str(), &width, &height, &channels, 4);
		if (data == nullptr || width <= 0 || height <= 0)
		{
			if (data != nullptr)
			{
				stbi_image_free(data);
			}
			return {};
		}

		const size_t pixelCount = static_cast<size_t>(width) * static_cast<size_t>(height) * 4u;
		std::vector<uint8_t> pixels(data, data + pixelCount);
		stbi_image_free(data);

		return Image(static_cast<uint32_t>(width), static_cast<uint32_t>(height), std::move(pixels));
	}
}
