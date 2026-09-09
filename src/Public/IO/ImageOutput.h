#pragma once

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>

namespace dy::IO
{
enum class PixelLayout { RGB8, RGBA8, BGRA8 };

// Non-owning rows of 8-bit pixels. Padding is described by rowPitch; no color
// space conversion is performed. PPM stores RGB and deliberately omits alpha.
struct ImageView
{
    uint32_t width;
    uint32_t height;
    size_t rowPitch;
    PixelLayout layout;
    const uint8_t* pixels;
    size_t size;
};

inline void WritePpm(const std::string& path, const ImageView& image)
{
    size_t channels;
    switch (image.layout)
    {
    case PixelLayout::RGB8: channels = 3; break;
    case PixelLayout::RGBA8:
    case PixelLayout::BGRA8: channels = 4; break;
    default: throw std::invalid_argument("Unsupported pixel layout.");
    }
    const auto max = std::numeric_limits<size_t>::max();
    if (!image.pixels || !image.width || !image.height || image.width > max / channels)
        throw std::invalid_argument("Invalid image dimensions or pixels.");
    const size_t rowBytes = static_cast<size_t>(image.width) * channels;
    if (image.rowPitch < rowBytes || image.height - 1 > (max - rowBytes) / image.rowPitch
        || image.size < (image.height - 1) * image.rowPitch + rowBytes)
        throw std::invalid_argument("Image rows exceed the supplied pixel storage.");

    std::ofstream output(path, std::ios::binary);
    if (!output) throw std::runtime_error("Cannot open image file: " + path);
    output << "P6\n" << image.width << ' ' << image.height << "\n255\n";
    for (uint32_t y = 0; y < image.height; ++y)
    {
        const auto* row = image.pixels + y * image.rowPitch;
        for (uint32_t x = 0; x < image.width; ++x)
        {
            const auto* pixel = row + x * channels;
            const uint8_t rgb[] = {pixel[image.layout == PixelLayout::BGRA8 ? 2 : 0],
                pixel[1], pixel[image.layout == PixelLayout::BGRA8 ? 0 : 2]};
            output.write(reinterpret_cast<const char*>(rgb), sizeof(rgb));
        }
    }
    output.close();
    if (!output) throw std::runtime_error("Cannot write image file: " + path);
}
}
