#include "Graphics/ImageFile.h"

#include <cstring>
#include <limits>

// 이미지 디코드는 Graphics 레이어의 관심사다. stb 구현을 여기(라이브러리)에서 단일
// 제공하여 백엔드(Null/Vulkan/...)에 의존하지 않는다. STB_IMAGE_IMPLEMENTATION 은
// 이 TU 한 곳에서만 정의한다(다른 곳에서 정의하면 중복 심볼).
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO_DEPRECATED
#include "stb_image.h"

namespace dy::Graphics
{
	namespace
	{
		[[nodiscard]] bool ComputeDecodedByteSize(
			int width,
			int height,
			uint64_t maxDecodedBytes,
			size_t& outBytes)
		{
			if(width <= 0 || height <= 0) return false;
			const uint64_t width64 = static_cast<uint64_t>(width);
			const uint64_t height64 = static_cast<uint64_t>(height);
			if(width64 > std::numeric_limits<uint64_t>::max() / height64) return false;
			const uint64_t pixels = width64 * height64;
			if(pixels > std::numeric_limits<uint64_t>::max() / 4u) return false;
			const uint64_t bytes = pixels * 4u;
			if(bytes > maxDecodedBytes || bytes > std::numeric_limits<size_t>::max()) return false;
			outBytes = static_cast<size_t>(bytes);
			return true;
		}
	}

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

		size_t decodedBytes = 0u;
		if(!ComputeDecodedByteSize(width, height, std::numeric_limits<uint64_t>::max(), decodedBytes))
		{
			stbi_image_free(data);
			return image;
		}
		image.m_width = static_cast<uint32_t>(width);
		image.m_height = static_cast<uint32_t>(height);
		image.m_pixels.resize(decodedBytes);
		std::memcpy(image.m_pixels.data(), data, image.m_pixels.size());
		stbi_image_free(data);
		return image;
	}

	ImageFile LoadImageMemory(
		const uint8_t* encodedBytes,
		size_t encodedSize,
		uint64_t maxDecodedBytes,
		bool* outLimitExceeded)
	{
		ImageFile image;
		if(outLimitExceeded != nullptr) *outLimitExceeded = false;
		if(encodedBytes == nullptr || encodedSize == 0u
			|| encodedSize > static_cast<size_t>(std::numeric_limits<int>::max())) return image;

		int width = 0;
		int height = 0;
		int channels = 0;
		if(stbi_info_from_memory(
			reinterpret_cast<const stbi_uc*>(encodedBytes),
			static_cast<int>(encodedSize),
			&width,
			&height,
			&channels) == 0) return image;
		size_t decodedBytes = 0u;
		if(!ComputeDecodedByteSize(width, height, maxDecodedBytes, decodedBytes))
		{
			if(outLimitExceeded != nullptr && width > 0 && height > 0)
				*outLimitExceeded = true;
			return image;
		}

		unsigned char* data = stbi_load_from_memory(
			reinterpret_cast<const stbi_uc*>(encodedBytes),
			static_cast<int>(encodedSize),
			&width,
			&height,
			&channels,
			4);
		if(data == nullptr) return image;
		image.m_width = static_cast<uint32_t>(width);
		image.m_height = static_cast<uint32_t>(height);
		image.m_pixels.resize(decodedBytes);
		std::memcpy(image.m_pixels.data(), data, decodedBytes);
		stbi_image_free(data);
		return image;
	}
}
