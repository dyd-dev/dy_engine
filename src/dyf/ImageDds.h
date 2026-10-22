#pragma once
#include "dyf/Image.h"
#include "dyf/RHI/Format.h"
#include <utility>

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
