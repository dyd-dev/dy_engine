#include "Core/Utf8.h"
#include "Graphics/Font.h"
#include "Graphics/Mesh.h"
#include "Graphics/Shaders.h"
#include "Graphics/Texture.h"
#include "IO/ImageOutput.h"
#include <array>
#include <cmath>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>

namespace
{
void Require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

template<class Function>
void Reject(Function function)
{
    try { function(); }
    catch (const std::invalid_argument&) { return; }
    throw std::runtime_error("Expected invalid input to be rejected.");
}

std::string Read(const char* path)
{
    std::ifstream input(path, std::ios::binary);
    Require(static_cast<bool>(input), "Cannot read test image.");
    return {std::istreambuf_iterator<char>(input), {}};
}

void CheckImageOutput(const char* path)
{
    // Two BGRA rows with non-pixel padding. The final row need not include padding.
    const std::array<uint8_t, 20> bgra{3, 2, 1, 200, 6, 5, 4, 0, 99, 99, 99, 99,
        9, 8, 7, 255, 12, 11, 10, 25};
    dy::IO::ImageView view{2, 2, 12, dy::IO::PixelLayout::BGRA8, bgra.data(), bgra.size()};
    dy::IO::WritePpm(path, view);
    std::string expected = "P6\n2 2\n255\n";
    for (char value = 1; value <= 12; ++value) expected.push_back(value);
    Require(Read(path) == expected, "PPM must respect BGRA channels, padding and alpha removal.");

    view.size = 19;
    Reject([&] { dy::IO::WritePpm(path, view); });
    Require(Read(path) == expected, "Invalid input must not truncate the output file.");
    view.size = bgra.size();
    view.rowPitch = 7;
    Reject([&] { dy::IO::WritePpm(path, view); });
    view.rowPitch = 12;
    view.layout = static_cast<dy::IO::PixelLayout>(99);
    Reject([&] { dy::IO::WritePpm(path, view); });

    const std::array<uint8_t, 4> rgba{20, 30, 40, 0};
    dy::IO::WritePpm(path, {1, 1, 4, dy::IO::PixelLayout::RGBA8, rgba.data(), rgba.size()});
    Require(Read(path) == std::string("P6\n1 1\n255\n") + char(20) + char(30) + char(40), "RGBA output changed channels.");
    dy::IO::WritePpm(path, {1, 1, 3, dy::IO::PixelLayout::RGB8, rgba.data(), 3});
    Require(Read(path) == std::string("P6\n1 1\n255\n") + char(20) + char(30) + char(40), "RGB output changed channels.");
}

void CheckUtf8()
{
    const std::string text = u8"A가😀";
    size_t at = 0;
    for (uint32_t expected : {0x41u, 0xAC00u, 0x1F600u})
        Require(dy::Core::DecodeUtf8(text, at) == expected, "UTF-8 codepoint decoding failed.");
    Require(at == text.size(), "UTF-8 decoder did not consume exactly the input.");
    for (const std::string malformed : {"\xC0\xAF", "\xED\xA0\x80", "\xF4\x90\x80\x80", "\xE2\x82"})
    {
        at = 0;
        Require(dy::Core::DecodeUtf8(malformed, at) == 0xFFFD && at > 0 && at <= malformed.size(),
            "Invalid UTF-8 must advance without returning a non-scalar value.");
    }
}

void CheckMesh()
{
    const auto mesh = dy::Graphics::CreateCubeMesh(2);
    Require(mesh.vertices.size() == 24 && mesh.indices.size() == 36, "Cube topology changed.");
    for (auto index : mesh.indices) Require(index < mesh.vertices.size(), "Cube index out of bounds.");
    for (const auto& vertex : mesh.vertices)
        Require(std::abs(vertex.position.x) == 1 && std::abs(vertex.position.y) == 1
            && std::abs(vertex.position.z) == 1, "Cube size must be supplied by the caller.");
}

void CheckFont(const char* path)
{
    auto font = dy::Graphics::Font::Load(path, 32, 256, 128);
    Require(font != nullptr, "Font load failed.");
    Require(font->GetAtlas().width == 256 && font->GetAtlas().height == 128,
        "Font atlas dimensions must be supplied by the caller.");
    const auto& a = font->GetGlyph('A');
    Require(a.width > 0 && a.height > 0 && a.advance > 0, "Glyph metrics are empty.");
    Require(&a == &font->GetGlyph('A'), "Repeated glyph requests must reuse the atlas entry.");
    const auto& unicode = font->GetGlyph(0xAC00);
    Require(unicode.x + unicode.width < 256 && unicode.y + unicode.height < 128, "Glyph escaped the atlas.");
    Require(std::isfinite(font->GetKerning('A', 'V')) && font->GetLineHeight() > 0, "Invalid font metrics.");
    Require(dy::Graphics::Font::Load(path, 0, 256, 128) == nullptr, "Zero font size accepted.");
    Require(dy::Graphics::Font::Load(path, 32, 0, 128) == nullptr, "Zero atlas width accepted.");

    auto narrow = dy::Graphics::Font::Load(path, 32, 3, 128);
    Require(narrow != nullptr, "Valid narrow atlas failed to load.");
    bool full = false;
    try { (void)narrow->GetGlyph('W'); }
    catch (const std::runtime_error&) { full = true; }
    Require(full, "Glyph wider than the atlas must be rejected before writing pixels.");
}
}

int main(int argc, char** argv)
{
    try
    {
        Require(argc == 2 || argc == 3, "Usage: dy_asset_tests output.ppm [font.ttf]");
        CheckImageOutput(argv[1]);
        CheckUtf8();
        CheckMesh();
        for (const auto& shaders : {dy::Graphics::GetCanvasShaderAssets(),
            dy::Graphics::GetMeshShaderAssets(false), dy::Graphics::GetMeshShaderAssets(true)})
#if defined(DY_TEST_NATIVE_SHADERS)
            Require(shaders.vertex.binary && shaders.vertex.binarySize && shaders.vertex.entryPoint
                && shaders.fragment.binary && shaders.fragment.binarySize && shaders.fragment.entryPoint,
                "Public shader assets are incomplete.");
#else
            Require(!shaders.vertex.binary && !shaders.fragment.binary,
                "Null must not select a native shader language implicitly.");
#endif
        if (argc == 3) CheckFont(argv[2]);
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
