#pragma once

#include <cstdint>
#include <vector>
#include "RHI/Format.h"

namespace dy::RHI
{
    struct TextureReadback
    {
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t rowPitch = 0;
        Format format = Format::Unknown;
        std::vector<uint8_t> pixels;
    };

    inline bool IsReadbackFormat(Format format)
    {
        return format == Format::R8G8B8A8_UNORM || format == Format::R8G8B8A8_UNORM_SRGB
            || format == Format::B8G8R8A8_UNORM || format == Format::B8G8R8A8_UNORM_SRGB;
    }
}
