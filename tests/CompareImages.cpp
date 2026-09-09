#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

struct Image
{
    uint32_t width = 0, height = 0;
    std::vector<uint8_t> pixels;
};

Image Load(const char* path)
{
    std::ifstream input(path, std::ios::binary);
    Image image;
    std::string magic;
    unsigned maximum = 0;
    if (!(input >> magic >> image.width >> image.height >> maximum) || magic != "P6" || maximum != 255
        || !image.width || !image.height || image.width > 16384 || image.height > 16384)
        throw std::runtime_error(std::string("Invalid PPM: ") + path);
    if (input.get() != '\n') throw std::runtime_error("Invalid PPM header separator.");
    image.pixels.resize(static_cast<size_t>(image.width) * image.height * 3);
    if (!input.read(reinterpret_cast<char*>(image.pixels.data()), image.pixels.size()))
        throw std::runtime_error(std::string("Truncated PPM: ") + path);
    return image;
}

int main(int argc, char** argv)
{
    try {
        if (argc != 3) throw std::runtime_error("Usage: dy_compare_images high.ppm low.ppm");
        const auto high = Load(argv[1]), low = Load(argv[2]);
        for (const auto& capture : {std::pair{argv[1], &high}, std::pair{argv[2], &low}}) {
            const auto& image = *capture.second;
            const std::string png = std::string(capture.first) + ".png";
            if (!stbi_write_png(png.c_str(), image.width, image.height, 3, image.pixels.data(), image.width * 3))
                throw std::runtime_error("Cannot write diagnostic PNG: " + png);
        }
        if (high.width != low.width || high.height != low.height) throw std::runtime_error("Capture dimensions differ.");
        uint32_t maximumDifference = 0;
        size_t differentPixels = 0, nonBackgroundPixels = 0;
        uint64_t totalDifference = 0;
        for (size_t i = 0; i < high.pixels.size(); i += 3) {
            bool different = false, nonBackground = false;
            for (size_t c = 0; c < 3; ++c) {
                const auto difference = static_cast<uint32_t>(std::abs(int(high.pixels[i+c]) - int(low.pixels[i+c])));
                maximumDifference = std::max(maximumDifference, difference);
                totalDifference += difference;
                different |= difference > 1;
                nonBackground |= std::abs(int(high.pixels[i+c]) - int(high.pixels[c])) > 4;
            }
            differentPixels += different;
            nonBackgroundPixels += nonBackground;
        }
        std::cout << high.width << 'x' << high.height << " max-channel-difference=" << maximumDifference
            << " mismatched-pixels=" << differentPixels << " non-background-pixels=" << nonBackgroundPixels
            << " mean-channel-difference=" << double(totalDifference) / high.pixels.size() << '\n';
        if (nonBackgroundPixels < 100) throw std::runtime_error("Both images could be empty; no substantial drawing detected.");
        if (differentPixels) throw std::runtime_error("High/Low output differs by more than one 8-bit unit.");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
