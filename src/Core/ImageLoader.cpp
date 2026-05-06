#define STB_IMAGE_IMPLEMENTATION
#include "Core/ImageLoader.h"
#include <stb_image.h>

namespace dy::Core
{
    Image LoadImageFromFile(const std::string& filepath)
    {
        int width = 0, height = 0, channels = 0;
        uint8_t* data = stbi_load(filepath.c_str(), &width, &height, &channels, 4);
        if(data == nullptr)
            return {};

        std::vector<uint8_t> pixels(data, data + width * height * 4);
        stbi_image_free(data);

        return Image(static_cast<uint32_t>(width), static_cast<uint32_t>(height), std::move(pixels));
    }
}
