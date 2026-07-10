#include "Graphics/ImageFile.h"

#include <cstring>

// 이미지 디코드는 Graphics 레이어의 관심사다. stb 구현을 여기(라이브러리)에서 단일
// 제공하여 백엔드(Null/Vulkan/...)에 의존하지 않는다. STB_IMAGE_IMPLEMENTATION 은
// 이 TU 한 곳에서만 정의한다(다른 곳에서 정의하면 중복 심볼).
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO_DEPRECATED
#include "stb_image.h"

namespace dy::Graphics
{
	ImageFile LoadImageFile(const std::string& path)
	{
		ImageFile image;
		if (path.empty()) return image;

		int width = 0;
		int height = 0;
		int channels = 0;
		unsigned char* data = stbi_load(path.c_str(), &width, &height, &channels, 4 /* force RGBA8 */);
		if (data == nullptr || width <= 0 || height <= 0)
		{
			if (data != nullptr) stbi_image_free(data);
			return image;
		}

		image.m_width = static_cast<uint32_t>(width);
		image.m_height = static_cast<uint32_t>(height);
		image.m_pixels.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4u);
		std::memcpy(image.m_pixels.data(), data, image.m_pixels.size());
		stbi_image_free(data);
		return image;
	}
}
