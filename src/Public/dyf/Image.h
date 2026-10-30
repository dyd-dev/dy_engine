#pragma once
#include <cstdint>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace dyf
{
    namespace Detail { struct ImageDdsData; struct ImageDdsAccess; struct ImagePixels; }
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
		// 원본 DDS에서 정확히 복원 가능한 RGBA 캐시의 용량을 반환하고 반납한다. GetPixels가 다시 복원한다.
		// 모든 복사본의 기존 픽셀 참조/읽기를 끝낸 뒤 명시적으로 호출해야 한다. 비DDS 이미지는 유지한다.
		[[nodiscard]] uint64_t ReleaseDecodedPixels() const;
		[[nodiscard]] ColorSpace GetColorSpace() const { return m_colorSpace; }
		void SetColorSpace(ColorSpace colorSpace) { m_colorSpace = colorSpace; }
		// 파일 출처 정보다. 설정하는 것만으로 파일을 읽지는 않는다.
		[[nodiscard]] const std::string& GetSourcePath() const { return m_sourcePath; }
		void SetSourcePath(std::string path);

	private:
        friend struct Detail::ImageDdsAccess;
		uint32_t m_width = 0, m_height = 0;
		std::shared_ptr<Detail::ImagePixels> m_pixels;
		ColorSpace m_colorSpace = ColorSpace::Srgb;
		std::string m_sourcePath;
        // Optional original DDS blocks; copies share them like the decoded pixels.
        std::shared_ptr<const Detail::ImageDdsData> m_dds;
	};
	// Linear HDR RGBA pixels. Copies share immutable storage and preserve values above 1.
	class HdrImage
	{
	public:
		HdrImage() = default;
		HdrImage(uint32_t width,uint32_t height,std::vector<float> rgba);
		[[nodiscard]] bool IsValid() const;
		[[nodiscard]] uint32_t GetWidth() const { return m_width; }
		[[nodiscard]] uint32_t GetHeight() const { return m_height; }
		[[nodiscard]] const std::vector<float>& GetPixels() const;
	private:
		uint32_t m_width=0,m_height=0;
		std::shared_ptr<const std::vector<float>> m_pixels;
	};
	[[nodiscard]] bool LoadHdrImage(const std::string& path,HdrImage& image);
	// Zero keeps the source size; a limit preserves HDR values while bounding both axes.
	[[nodiscard]] bool LoadHdrImage(const std::string& path,HdrImage& image,uint32_t maxDimension);

	// 실패하면 stderr로 원인을 알리고 false를 반환한다. DDS BC1/2/3/4/5/7 및
	// RGBA/BGRA8도 CPU에서 RGBA8로 해제한다. 원본 크기·알파를 유지하며 색 변환은 하지 않는다.
    // BC1/BC3 및 순서가 같은 BC5 법선맵의 완전한 원본 mip chain을 선택적 GPU 업로드용으로 공유한다.
	[[nodiscard]] bool LoadImage(const std::string& path, Image& image);
	// Reads file metadata (including DDS) without decoding pixel storage. Failure clears both dimensions.
	[[nodiscard]] bool GetImageDimensions(const std::string& path, uint32_t& width, uint32_t& height);
	// 메모리 입력은 모델에 내장된 이미지 등에서 사용한다.
	[[nodiscard]] bool LoadImage(const uint8_t* encodedBytes, size_t encodedSize, Image& image,
		uint64_t maxDecodedBytes = UINT64_MAX, bool* outLimitExceeded = nullptr);
}
