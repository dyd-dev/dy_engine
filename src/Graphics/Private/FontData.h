#pragma once
#include "Graphics/Font.h"
#include "Graphics/Texture.h"
#include <map>
#include <vector>
#include <stb_truetype.h>

namespace dy::Graphics
{
struct Font::Impl
{
    using Glyph = FontGlyph;
    std::vector<unsigned char> bytes;
    stbtt_fontinfo info = {};
    TextureAsset atlas;
    std::map<uint32_t, Glyph> glyphs;
    float scale = 0, lineHeight = 0, ascent = 0;
    int penX = 1, penY = 1, rowHeight = 0;
    const Glyph& GetGlyph(uint32_t codepoint);
};
}
