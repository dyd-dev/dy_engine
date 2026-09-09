#pragma once
/* ImageFile
 *
 * 경로 기반 텍스처 모델의 디코드 헬퍼. Scene 은 TextureAsset{sourcePath} 만 보관하고,
 * 실제 픽셀 디코드는 업로드 시점(렌더러)에서 이 헬퍼로 수행한다. (Core::Image 폐기 대체물)
 * RGBA8 로 강제 디코드한다.
 */
#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

namespace dy::Graphics
{
	class ImageFile
	{
	public:
		[[nodiscard]] bool IsValid() const { return !m_pixels.empty() && m_width > 0u && m_height > 0u; }
		[[nodiscard]] uint32_t GetWidth() const { return m_width; }
		[[nodiscard]] uint32_t GetHeight() const { return m_height; }
		[[nodiscard]] uint32_t GetRowPitch() const { return m_width * 4u; }
		[[nodiscard]] const std::vector<uint8_t>& GetPixels() const { return m_pixels; }

		friend ImageFile LoadImageFile(const std::string& path);
		friend ImageFile LoadImageMemory(
			const uint8_t* encodedBytes,
			size_t encodedSize,
			uint64_t maxDecodedBytes,
			bool* outLimitExceeded);

	private:
		uint32_t m_width = 0u;
		uint32_t m_height = 0u;
		std::vector<uint8_t> m_pixels; // RGBA8, row-major, tightly packed
	};

	// 파일 경로에서 RGBA8 이미지를 디코드한다. 실패 시 IsValid()==false 인 ImageFile 반환.
	[[nodiscard]] ImageFile LoadImageFile(const std::string& path);
	[[nodiscard]] ImageFile LoadImageMemory(
		const uint8_t* encodedBytes,
		size_t encodedSize,
		uint64_t maxDecodedBytes,
		bool* outLimitExceeded = nullptr);
}
