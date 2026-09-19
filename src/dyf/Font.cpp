#define STB_TRUETYPE_IMPLEMENTATION
#include "dyf/Font.h"
#include "dyf/Image.h"
#include <map>
#include <vector>
#include <stb_truetype.h>

namespace dyf
{
struct Font::Impl
{
    std::vector<unsigned char> bytes;
    stbtt_fontinfo info = {};
    Image atlas;
    // 글리프를 추가할 CPU 저장소와 이미 전달한 읽기 전용 이미지의 수명을 분리한다.
    std::vector<uint8_t> atlasPixels;
    uint32_t atlasWidth = 0, atlasHeight = 0;
    bool atlasDirty = true;
    std::map<uint32_t, FontGlyph> glyphs;
    float scale = 0, lineHeight = 0, ascent = 0;
    int penX = 1, penY = 1, rowHeight = 0;
    const FontGlyph& GetGlyph(uint32_t codepoint);
};
}
#include <cmath>
#include <fstream>
#include <iterator>
#include <algorithm>
#include <limits>
#include <stdexcept>
#include <cstdio>

namespace dyf
{
Font::Font(std::unique_ptr<Impl> impl) : m_impl(std::move(impl)) {}
Font::~Font() = default;
std::unique_ptr<Font> Font::Load(const std::string& path, float pixelHeight,
    uint32_t atlasWidth, uint32_t atlasHeight)
{
    try
    {
    // 글리프 사이의 투명 여백으로 인접 글리프가 섞이는 것을 막는다.
    if (!std::isfinite(pixelHeight) || pixelHeight <= 0 || atlasWidth < 3 || atlasHeight < 3
        || pixelHeight > static_cast<float>(atlasHeight - 2)
        || atlasWidth > static_cast<uint32_t>(std::numeric_limits<int>::max())
        || atlasHeight > static_cast<uint32_t>(std::numeric_limits<int>::max())
        || static_cast<uint64_t>(atlasWidth) * atlasHeight > std::numeric_limits<size_t>::max() / 4)
        throw std::runtime_error("Invalid font data or atlas settings.");
    std::ifstream file(path, std::ios::binary);
    if(!file) throw std::runtime_error("Cannot open font file.");
    auto impl = std::make_unique<Impl>();
    impl->bytes.assign(std::istreambuf_iterator<char>(file), {});
    if(impl->bytes.size() < 12) throw std::runtime_error("Invalid font data or atlas settings.");
    const int offset = stbtt_GetFontOffsetForIndex(impl->bytes.data(), 0);
    if(offset < 0 || !stbtt_InitFont(&impl->info, impl->bytes.data(), offset)) throw std::runtime_error("Invalid font data or atlas settings.");
    impl->scale = stbtt_ScaleForPixelHeight(&impl->info, pixelHeight);
    int ascent, descent, gap;
    stbtt_GetFontVMetrics(&impl->info, &ascent, &descent, &gap);
    impl->ascent = ascent * impl->scale;
    impl->lineHeight = (ascent - descent + gap) * impl->scale;
    impl->atlasWidth = atlasWidth;
    impl->atlasHeight = atlasHeight;
    impl->atlasPixels.resize(static_cast<size_t>(atlasWidth) * atlasHeight * 4, 0);
    return std::unique_ptr<Font>(new Font(std::move(impl)));
    }
    catch(const std::exception& error) { std::fprintf(stderr,"dyf: font %s: %s\n",path.c_str(),error.what()); return nullptr; }
}

const FontGlyph& Font::Impl::GetGlyph(uint32_t codepoint)
{
    if (codepoint > 0x10FFFF || (codepoint >= 0xD800 && codepoint <= 0xDFFF))
        codepoint = 0xFFFD;
    const auto found = glyphs.find(codepoint);
    if(found != glyphs.end()) return found->second;
    FontGlyph glyph;
    int advance, bearing;
    stbtt_GetCodepointHMetrics(&info, static_cast<int>(codepoint), &advance, &bearing);
    glyph.advance = advance * scale;
    unsigned char* pixels = stbtt_GetCodepointBitmap(&info, scale, scale, static_cast<int>(codepoint),
        &glyph.width, &glyph.height, &glyph.offsetX, &glyph.offsetY);
    if (glyph.width > 0 && glyph.height > 0 && !pixels)
        throw std::runtime_error("Font glyph rasterization failed.");
    if (glyph.width > static_cast<int>(atlasWidth) - 2 || glyph.height > static_cast<int>(atlasHeight) - 2)
    {
        stbtt_FreeBitmap(pixels, nullptr);
        throw std::runtime_error("Glyph exceeds the font atlas dimensions.");
    }
    if (glyph.width >= static_cast<int>(atlasWidth) - penX - 1)
    {
        if (rowHeight + 1 >= static_cast<int>(atlasHeight) - penY)
        {
            stbtt_FreeBitmap(pixels, nullptr);
            throw std::runtime_error("Font glyph atlas is full.");
        }
        penX = 1; penY += rowHeight + 1; rowHeight = 0;
    }
    if(glyph.height >= static_cast<int>(atlasHeight) - penY - 1)
    {
        stbtt_FreeBitmap(pixels, nullptr);
        throw std::runtime_error("Font glyph atlas is full.");
    }
    glyph.x = penX; glyph.y = penY;
    for(int y = 0; y < glyph.height; ++y)
        for(int x = 0; x < glyph.width; ++x)
        {
            const auto dst = ((static_cast<size_t>(glyph.y) + y) * atlasWidth + glyph.x + x) * 4;
            atlasPixels[dst] = atlasPixels[dst + 1] = atlasPixels[dst + 2] = 255;
            atlasPixels[dst + 3] = pixels[y * glyph.width + x];
        }
    if(glyph.width > 0 && glyph.height > 0) atlasDirty = true;
    stbtt_FreeBitmap(pixels, nullptr);
    penX += glyph.width + 1;
    rowHeight = std::max(rowHeight, glyph.height);
    return glyphs.emplace(codepoint, glyph).first->second;
}

bool Font::GetGlyph(uint32_t codepoint, FontGlyph& glyph)
{
    try { glyph = m_impl->GetGlyph(codepoint); return true; }
    catch(const std::exception& error) { std::fprintf(stderr,"dyf: glyph: %s\n",error.what()); return false; }
}
float Font::GetKerning(uint32_t left, uint32_t right) const
{
    if (left > 0x10FFFF || right > 0x10FFFF) return 0;
    return stbtt_GetCodepointKernAdvance(&m_impl->info, static_cast<int>(left), static_cast<int>(right)) * m_impl->scale;
}
float Font::GetAscent() const { return m_impl->ascent; }
float Font::GetLineHeight() const { return m_impl->lineHeight; }
const Image& Font::GetAtlas() const
{
    // 필요한 글리프를 모두 만든 다음 한 번만 픽셀을 복사한다. 이전 Image 사본은 그대로 유지된다.
    if(m_impl->atlasDirty)
    {
        m_impl->atlas = Image(m_impl->atlasWidth, m_impl->atlasHeight, m_impl->atlasPixels, ColorSpace::Linear);
        m_impl->atlasDirty = false;
    }
    return m_impl->atlas;
}
}
