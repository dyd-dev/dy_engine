#include "dyf/Image.h"

#include <limits>
#include <cstdio>
#include <stdexcept>
#include <memory>
#include <utility>

// 파일과 모델 내장 이미지의 디코딩은 이 CPU 이미지 로더에서 담당한다.
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

namespace dyf
{
namespace
{
    using DecodedPixels = std::unique_ptr<stbi_uc, decltype(&stbi_image_free)>;

    bool DecodedByteSize(int width, int height, uint64_t limit, size_t& bytes)
    {
        if(width <= 0 || height <= 0) return false;
        const uint64_t pixels = static_cast<uint64_t>(width) * static_cast<uint64_t>(height);
        if(pixels > std::numeric_limits<uint64_t>::max() / 4u) return false;
        const uint64_t size = pixels * 4u;
        if(size > limit || size > std::numeric_limits<size_t>::max()) return false;
        bytes = static_cast<size_t>(size);
        return true;
    }
}

static bool DecodeImage(const std::string& path, Image& result)
{
    if(path.empty()) return false;
    int width = 0, height = 0, channels = 0;
    DecodedPixels pixels(stbi_load(path.c_str(), &width, &height, &channels, 4), stbi_image_free);
    size_t bytes = 0;
    if(!pixels || !DecodedByteSize(width, height, UINT64_MAX, bytes)) return false;

    Image image(static_cast<uint32_t>(width), static_cast<uint32_t>(height),
        std::vector<uint8_t>(pixels.get(), pixels.get() + bytes));
    image.SetSourcePath(path);
    result = std::move(image);
    return true;
}

static bool DecodeImage(const uint8_t* encodedBytes, size_t encodedSize, Image& result,
    uint64_t maxDecodedBytes, bool* outLimitExceeded)
{
    if(outLimitExceeded) *outLimitExceeded = false;
    if(!encodedBytes || !encodedSize || encodedSize > static_cast<size_t>(std::numeric_limits<int>::max()))
        return false;

    int width = 0, height = 0, channels = 0;
    const auto size = static_cast<int>(encodedSize);
    if(!stbi_info_from_memory(encodedBytes, size, &width, &height, &channels)) return false;
    size_t bytes = 0;
    if(!DecodedByteSize(width, height, maxDecodedBytes, bytes))
    {
        if(outLimitExceeded && width > 0 && height > 0) *outLimitExceeded = true;
        return false;
    }
    DecodedPixels pixels(stbi_load_from_memory(encodedBytes, size, &width, &height, &channels, 4), stbi_image_free);
    if(!pixels) return false;
    if(!DecodedByteSize(width, height, maxDecodedBytes, bytes))
    {
        if(outLimitExceeded && width > 0 && height > 0) *outLimitExceeded = true;
        return false;
    }

    Image image(static_cast<uint32_t>(width), static_cast<uint32_t>(height),
        std::vector<uint8_t>(pixels.get(), pixels.get() + bytes));
    result = std::move(image);
    return true;
}
bool LoadImage(const std::string& path, Image& result)
{
    try
    {
        if(DecodeImage(path,result)) return true;
        std::fprintf(stderr,"dyf: image load failed: %s\n",path.c_str());
    }
    catch(const std::exception& error) { std::fprintf(stderr,"dyf: image load: %s\n",error.what()); }
    return false;
}
bool LoadImage(const uint8_t* bytes,size_t size,Image& result,uint64_t limit,bool* exceeded)
{
    try
    {
        if(DecodeImage(bytes,size,result,limit,exceeded)) return true;
        std::fprintf(stderr,"dyf: image decode failed or decoded byte limit exceeded.\n");
    }
    catch(const std::exception& error) { std::fprintf(stderr,"dyf: image decode: %s\n",error.what()); }
    return false;
}

}

namespace dyf
{
Image::Image(uint32_t width, uint32_t height, std::vector<uint8_t> rgba8, ColorSpace colorSpace)
    : m_width(width), m_height(height),
      m_pixels(std::make_shared<const std::vector<uint8_t>>(std::move(rgba8))), m_colorSpace(colorSpace)
{
}

bool Image::IsValid() const
{
    return m_width && m_height && m_pixels && m_pixels->size() % 4 == 0 &&
        static_cast<uint64_t>(m_width) * m_height == m_pixels->size() / 4;
}

const std::vector<uint8_t>& Image::GetPixels() const
{
    static const std::vector<uint8_t> empty;
    return m_pixels ? *m_pixels : empty;
}

void Image::SetSourcePath(std::string path)
{
    m_sourcePath = std::move(path);
}
}

