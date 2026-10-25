#define STB_TRUETYPE_IMPLEMENTATION
#include "dyf/Font.h"
#include "dyf/Image.h"
#include <map>
#include <vector>
#include <stb_truetype.h>

namespace dyf
{
// stbtt_fontinfo는 bytes 안의 폰트 자료를 참조하므로 둘은 같은 Impl 수명으로 유지한다.
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
// 완성된 Impl의 단독 소유권을 Font로 넘긴다.
Font::Font(std::unique_ptr<Impl> impl) : m_impl(std::move(impl)) {}
// Impl과 그 안의 폰트 바이트·글리프·이미지 참조를 함께 정리한다.
Font::~Font() = default;
// 파일과 atlas 설정을 검사하고 픽셀 크기에 맞는 배율·줄 높이·상단 높이를 계산한다. 예외는 진단과 nullptr로 바꾼다.
std::unique_ptr<Font> Font::Load(const std::string& path, float pixelHeight,
    uint32_t atlasWidth, uint32_t atlasHeight)
{
    try
    {
    // 글리프 사이의 투명 여백으로 인접 글리프가 섞이는 것을 막는다.
    if (!std::isfinite(pixelHeight) || pixelHeight <= 0 || atlasWidth < 3 || atlasHeight < 3 // 글꼴 크기·atlas 여백·정수 범위·RGBA 용량 조건을 모두 만족해야 로드한다.
        || pixelHeight > static_cast<float>(atlasHeight - 2)
        || atlasWidth > static_cast<uint32_t>(std::numeric_limits<int>::max())
        || atlasHeight > static_cast<uint32_t>(std::numeric_limits<int>::max())
        || static_cast<uint64_t>(atlasWidth) * atlasHeight > std::numeric_limits<size_t>::max() / 4)
        throw std::runtime_error("Invalid font data or atlas settings.");
    std::ifstream file(path, std::ios::binary);
    if(!file) throw std::runtime_error("Cannot open font file."); // 폰트 파일을 열 수 없으면 실패 예외로 빠진다.
    auto impl = std::make_unique<Impl>();
    impl->bytes.assign(std::istreambuf_iterator<char>(file), {});
    if(impl->bytes.size() < 12) throw std::runtime_error("Invalid font data or atlas settings."); // 최소 헤더 크기보다 작으면 stb 초기화를 진행하지 않는다.
    const int offset = stbtt_GetFontOffsetForIndex(impl->bytes.data(), 0);
    if(offset < 0 || !stbtt_InitFont(&impl->info, impl->bytes.data(), offset)) throw std::runtime_error("Invalid font data or atlas settings."); // 첫 폰트의 위치나 초기화 결과가 잘못되면 로드를 실패시킨다.
    impl->scale = stbtt_ScaleForPixelHeight(&impl->info, pixelHeight);
    int ascent, descent, gap;
    stbtt_GetFontVMetrics(&impl->info, &ascent, &descent, &gap);
    impl->ascent = ascent * impl->scale;
    impl->lineHeight = (ascent - descent + gap) * impl->scale;
    impl->atlasWidth = atlasWidth;
    impl->atlasHeight = atlasHeight;
    // 투명 RGBA 배열만 먼저 확보한다. 개별 글리프는 요청받을 때 그린다.
    impl->atlasPixels.resize(static_cast<size_t>(atlasWidth) * atlasHeight * 4, 0);
    return std::unique_ptr<Font>(new Font(std::move(impl)));
    }
    catch(const std::exception& error) { std::fprintf(stderr,"dyf: font %s: %s\n",path.c_str(),error.what()); return nullptr; }
}

// 범위 밖 코드포인트를 대체 문자로 바꾼 뒤 캐시를 찾는다. 없으면 래스터화한 글리프를 여백 한 픽셀을 두고 행 단위로 배치한다.
const FontGlyph& Font::Impl::GetGlyph(uint32_t codepoint)
{
    if (codepoint > 0x10FFFF || (codepoint >= 0xD800 && codepoint <= 0xDFFF)) // 유니코드 범위 밖이나 서로게이트 값은 대체 문자로 바꾼다.
        codepoint = 0xFFFD;
    const auto found = glyphs.find(codepoint);
    if(found != glyphs.end()) return found->second; // 이미 만든 문자는 같은 atlas 위치와 크기를 재사용한다.
    FontGlyph glyph;
    int advance;
    stbtt_GetCodepointHMetrics(&info, static_cast<int>(codepoint), &advance, nullptr);
    glyph.advance = advance * scale;
    unsigned char* pixels = stbtt_GetCodepointBitmap(&info, scale, scale, static_cast<int>(codepoint),
        &glyph.width, &glyph.height, &glyph.offsetX, &glyph.offsetY);
    if (glyph.width > 0 && glyph.height > 0 && !pixels) // 실제 그림이 필요한데 비트맵이 없으면 래스터화 실패다.
        throw std::runtime_error("Font glyph rasterization failed.");
    if (!glyph.width || !glyph.height) // 공백처럼 그림이 없으면 전진 폭만 캐시에 저장한다.
    {
        stbtt_FreeBitmap(pixels, nullptr);
        return glyphs.emplace(codepoint, glyph).first->second;
    }
    if (glyph.width > static_cast<int>(atlasWidth) - 2 || glyph.height > static_cast<int>(atlasHeight) - 2) // 한 글자가 atlas 내부보다 크면 비트맵을 놓고 실패한다.
    {
        stbtt_FreeBitmap(pixels, nullptr);
        throw std::runtime_error("Glyph exceeds the font atlas dimensions.");
    }
    if (glyph.width > static_cast<int>(atlasWidth) - penX - 1) // 현재 행의 가로 공간이 부족하면 다음 행으로 이동한다.
    {
        if (rowHeight + 1 >= static_cast<int>(atlasHeight) - penY) // 다음 행을 시작할 세로 공간도 없으면 atlas가 가득 찬 것이다.
        {
            stbtt_FreeBitmap(pixels, nullptr);
            throw std::runtime_error("Font glyph atlas is full.");
        }
        penX = 1; penY += rowHeight + 1; rowHeight = 0;
    }
    if(glyph.height > static_cast<int>(atlasHeight) - penY - 1) // 현재 배치 위치에서 아래쪽 여백까지 확보하지 못하면 실패한다.
    {
        stbtt_FreeBitmap(pixels, nullptr);
        throw std::runtime_error("Font glyph atlas is full.");
    }
    // atlas의 RGB는 255의 흰색으로 두고 알파에는 stb가 만든 글리프 픽셀값을 넣는다. Canvas의 tint가 글자색을 정하면서 알파에 따라 글자 가장자리와 배경이 합성된다.
    glyph.x = penX; glyph.y = penY;
    for(int y = 0; y < glyph.height; ++y)
        for(int x = 0; x < glyph.width; ++x)
        {
            const auto dst = ((static_cast<size_t>(glyph.y) + y) * atlasWidth + glyph.x + x) * 4;
            atlasPixels[dst] = atlasPixels[dst + 1] = atlasPixels[dst + 2] = 255;
            atlasPixels[dst + 3] = pixels[y * glyph.width + x];
        }
    if(glyph.width > 0 && glyph.height > 0) atlasDirty = true; // 픽셀이 추가되었으므로 다음 GetAtlas에서 이미지 사본을 갱신한다.
    stbtt_FreeBitmap(pixels, nullptr);
    penX += glyph.width + 1;
    rowHeight = std::max(rowHeight, glyph.height);
    return glyphs.emplace(codepoint, glyph).first->second;
}

// 내부 글리프를 호출자에게 복사한다. 공간 부족·래스터화 예외는 false로 전달한다.
bool Font::GetGlyph(uint32_t codepoint, FontGlyph& glyph)
{
    try { glyph = m_impl->GetGlyph(codepoint); return true; }
    catch(const std::exception& error) { std::fprintf(stderr,"dyf: glyph: %s\n",error.what()); return false; }
}
// 두 코드포인트의 폰트 단위 간격을 현재 픽셀 배율로 바꿔 반환한다.
float Font::GetKerning(uint32_t left, uint32_t right) const
{
    if (left > 0x10FFFF || right > 0x10FFFF) return 0; // 범위 밖 코드포인트의 커닝은 0으로 처리한다.
    return stbtt_GetCodepointKernAdvance(&m_impl->info, static_cast<int>(left), static_cast<int>(right)) * m_impl->scale;
}
// 기준선 위쪽 높이와 줄 간격은 Load에서 계산해 둔 픽셀 단위 값이다.
float Font::GetAscent() const { return m_impl->ascent; }
float Font::GetLineHeight() const { return m_impl->lineHeight; }
// 필요한 글리프를 모두 준비한 뒤 변경된 atlas만 불변 Image로 한 번 복사하고 dirty를 끄며, 이전 Image 사본의 픽셀은 유지한다.
const Image& Font::GetAtlas() const
{
    if(m_impl->atlasDirty)
    {
        m_impl->atlas = Image(m_impl->atlasWidth, m_impl->atlasHeight, m_impl->atlasPixels, ColorSpace::Linear);
        m_impl->atlasDirty = false;
    }
    return m_impl->atlas;
}
}
