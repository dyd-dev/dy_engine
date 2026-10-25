#pragma once
#include "dyf/Image.h"
#include "dyf/RHI/Format.h"
#include <utility>

namespace dyf
{
// Zero keeps the source size. Samples linear float pixels without clamping HDR values.
[[nodiscard]] bool LoadHdrImage(const std::string& path,HdrImage& image,uint32_t maxDimension);
// Reports the bounded output size, selecting an authored DDS mip when available.
[[nodiscard]] bool GetImageDimensions(const std::string& path, uint32_t& width, uint32_t& height,
    uint32_t maxDimension);
// Zero preserves the original dimensions. Nearest sampling preserves channel values
// without mixing sRGB color, alpha or linear data. Non-DDS temporary full decoding
// still obeys maxDecodedBytes; DDS can start from an existing smaller mip.
[[nodiscard]] bool LoadImage(const uint8_t* encodedBytes, size_t encodedSize, Image& image,
    uint64_t maxDecodedBytes, uint32_t maxDimension, bool* outLimitExceeded);
}

namespace dyf::Detail
{
struct ImageDdsData
{
    RHI::Format format = RHI::Format::Unknown;
    std::vector<std::vector<uint8_t>> mips;
};

struct ImageDdsAccess
{
    static const ImageDdsData* Get(const Image& image) { return image.m_dds.get(); }
    static void Set(Image& image, std::shared_ptr<const ImageDdsData> data)
    { image.m_dds = std::move(data); }
};
}
