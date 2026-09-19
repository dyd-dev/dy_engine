#pragma once
#include <cstdint>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace dyf
{
	enum class ColorSpace : uint8_t { Srgb, Linear };

	// CPU의 RGBA8 이미지다. 복사하면 읽기 전용 픽셀 저장소를 공유하며 GPU 자원을 만들지 않는다.
	class Image
	{
	public:
		Image() = default;
		Image(uint32_t width, uint32_t height, std::vector<uint8_t> rgba8,
			ColorSpace colorSpace = ColorSpace::Srgb);

		[[nodiscard]] bool IsValid() const;
		[[nodiscard]] uint32_t GetWidth() const { return m_width; }
		[[nodiscard]] uint32_t GetHeight() const { return m_height; }
		// 픽셀을 바꾸려면 새 Image를 대입한다. 이미 Canvas나 Material에 전달한 이미지에는 영향이 없다.
		[[nodiscard]] const std::vector<uint8_t>& GetPixels() const;
		[[nodiscard]] ColorSpace GetColorSpace() const { return m_colorSpace; }
		void SetColorSpace(ColorSpace colorSpace) { m_colorSpace = colorSpace; }
		// 파일 출처 정보다. 설정하는 것만으로 파일을 읽지는 않는다.
		[[nodiscard]] const std::string& GetSourcePath() const { return m_sourcePath; }
		void SetSourcePath(std::string path);

	private:
		uint32_t m_width = 0, m_height = 0;
		std::shared_ptr<const std::vector<uint8_t>> m_pixels;
		ColorSpace m_colorSpace = ColorSpace::Srgb;
		std::string m_sourcePath;
	};

	// 실패하면 stderr로 원인을 알리고 false를 반환한다.
	[[nodiscard]] bool LoadImage(const std::string& path, Image& image);
	// 메모리 입력은 모델에 내장된 이미지 등에서 사용한다.
	[[nodiscard]] bool LoadImage(const uint8_t* encodedBytes, size_t encodedSize, Image& image,
		uint64_t maxDecodedBytes = UINT64_MAX, bool* outLimitExceeded = nullptr);
}
