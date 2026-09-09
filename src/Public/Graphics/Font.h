#pragma once
#include <memory>
#include <cstdint>
#include <string>

namespace dy::Graphics
{
struct TextureAsset;

struct FontGlyph
{
    int x = 0, y = 0, width = 0, height = 0, offsetX = 0, offsetY = 0;
    float advance = 0;
};

class Font
{
public:
    static std::unique_ptr<Font> Load(const std::string& path, float pixelHeight,
        uint32_t atlasWidth, uint32_t atlasHeight);
    ~Font();
    Font(const Font&) = delete;
    Font& operator=(const Font&) = delete;
    // CPU glyph metrics and rasterization, shared by Canvas and direct RHI users.
    // GetGlyph may add pixels to the atlas; upload after preparing the text.
    const FontGlyph& GetGlyph(uint32_t codepoint);
    float GetKerning(uint32_t left, uint32_t right) const;
    float GetAscent() const;
    float GetLineHeight() const;
    const TextureAsset& GetAtlas() const;
private:
    struct Impl;
    explicit Font(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> m_impl;
};
}
