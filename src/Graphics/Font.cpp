#define STB_TRUETYPE_IMPLEMENTATION
#include "Graphics/Private/FontData.h"
#include <cmath>
#include <fstream>
#include <iterator>
#include <algorithm>
#include <limits>
#include <stdexcept>

namespace dy::Graphics
{
Font::Font(std::unique_ptr<Impl> impl) : m_impl(std::move(impl)) {}
Font::~Font() = default;
std::unique_ptr<Font> Font::Load(const std::string& path, float pixelHeight,
    uint32_t atlasWidth, uint32_t atlasHeight)
{
    // A transparent border around glyphs prevents filtering into adjacent glyphs.
    if (!std::isfinite(pixelHeight) || pixelHeight <= 0 || atlasWidth < 3 || atlasHeight < 3
        || pixelHeight > static_cast<float>(atlasHeight - 2)
        || atlasWidth > static_cast<uint32_t>(std::numeric_limits<int>::max())
        || atlasHeight > static_cast<uint32_t>(std::numeric_limits<int>::max())
        || static_cast<uint64_t>(atlasWidth) * atlasHeight > std::numeric_limits<size_t>::max() / 4)
        return nullptr;
    std::ifstream file(path, std::ios::binary);
    if(!file) return nullptr;
    auto impl = std::make_unique<Impl>();
    impl->bytes.assign(std::istreambuf_iterator<char>(file), {});
    if(impl->bytes.size() < 12) return nullptr;
    const int offset = stbtt_GetFontOffsetForIndex(impl->bytes.data(), 0);
    if(offset < 0 || !stbtt_InitFont(&impl->info, impl->bytes.data(), offset)) return nullptr;
    impl->scale = stbtt_ScaleForPixelHeight(&impl->info, pixelHeight);
    int ascent, descent, gap;
    stbtt_GetFontVMetrics(&impl->info, &ascent, &descent, &gap);
    impl->ascent = ascent * impl->scale;
    impl->lineHeight = (ascent - descent + gap) * impl->scale;
    impl->atlas.width = atlasWidth;
    impl->atlas.height = atlasHeight;
    impl->atlas.rgba8.resize(static_cast<size_t>(atlasWidth) * atlasHeight * 4, 0);
    return std::unique_ptr<Font>(new Font(std::move(impl)));
}
const Font::Impl::Glyph& Font::Impl::GetGlyph(uint32_t codepoint)
{
    if (codepoint > 0x10FFFF || (codepoint >= 0xD800 && codepoint <= 0xDFFF))
        codepoint = 0xFFFD;
    const auto found = glyphs.find(codepoint);
    if(found != glyphs.end()) return found->second;
    Glyph glyph;
    int advance, bearing;
    stbtt_GetCodepointHMetrics(&info, static_cast<int>(codepoint), &advance, &bearing);
    glyph.advance = advance * scale;
    unsigned char* pixels = stbtt_GetCodepointBitmap(&info, scale, scale, static_cast<int>(codepoint),
        &glyph.width, &glyph.height, &glyph.offsetX, &glyph.offsetY);
    if (glyph.width > 0 && glyph.height > 0 && !pixels)
        throw std::runtime_error("Font glyph rasterization failed.");
    if (glyph.width > static_cast<int>(atlas.width) - 2 || glyph.height > static_cast<int>(atlas.height) - 2)
    {
        stbtt_FreeBitmap(pixels, nullptr);
        throw std::runtime_error("Glyph exceeds the font atlas dimensions.");
    }
    if (glyph.width >= static_cast<int>(atlas.width) - penX - 1)
    {
        if (rowHeight + 1 >= static_cast<int>(atlas.height) - penY)
        {
            stbtt_FreeBitmap(pixels, nullptr);
            throw std::runtime_error("Font glyph atlas is full.");
        }
        penX = 1; penY += rowHeight + 1; rowHeight = 0;
    }
    if(glyph.height >= static_cast<int>(atlas.height) - penY - 1)
    {
        stbtt_FreeBitmap(pixels, nullptr);
        throw std::runtime_error("Font glyph atlas is full.");
    }
    glyph.x = penX; glyph.y = penY;
    for(int y = 0; y < glyph.height; ++y)
        for(int x = 0; x < glyph.width; ++x)
        {
            const auto dst = ((static_cast<size_t>(glyph.y) + y) * atlas.width + glyph.x + x) * 4;
            atlas.rgba8[dst] = atlas.rgba8[dst + 1] = atlas.rgba8[dst + 2] = 255;
            atlas.rgba8[dst + 3] = pixels[y * glyph.width + x];
        }
    stbtt_FreeBitmap(pixels, nullptr);
    penX += glyph.width + 1;
    rowHeight = std::max(rowHeight, glyph.height);
    return glyphs.emplace(codepoint, glyph).first->second;
}

const FontGlyph& Font::GetGlyph(uint32_t codepoint) { return m_impl->GetGlyph(codepoint); }
float Font::GetKerning(uint32_t left, uint32_t right) const
{
    if (left > 0x10FFFF || right > 0x10FFFF) return 0;
    return stbtt_GetCodepointKernAdvance(&m_impl->info, static_cast<int>(left), static_cast<int>(right)) * m_impl->scale;
}
float Font::GetAscent() const { return m_impl->ascent; }
float Font::GetLineHeight() const { return m_impl->lineHeight; }
const TextureAsset& Font::GetAtlas() const { return m_impl->atlas; }
}
