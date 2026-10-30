#pragma once
#include <memory>
#include <cstdint>
#include <string>

namespace dyf
{
class Image;

struct FontGlyph
{
    int x = 0, y = 0, width = 0, height = 0, offsetX = 0, offsetY = 0;
    float advance = 0;
};

class Font
{
public:
    static std::unique_ptr<Font> Load(const std::string& path, float pixelHeight = 16,
        uint32_t atlasWidth = 1024, uint32_t atlasHeight = 1024);
    ~Font();
    Font(const Font&) = delete;
    Font& operator=(const Font&) = delete;
    // CPU 글리프 계산과 이미지 생성만 담당하며 Canvas와 RHI 사용자 모두 사용할 수 있다.
    // GetGlyph가 아틀라스 픽셀을 추가하므로 필요한 글자를 준비한 다음 RHI로 업로드한다.
    [[nodiscard]] bool GetGlyph(uint32_t codepoint, FontGlyph& glyph);
    float GetKerning(uint32_t left, uint32_t right) const;
    float GetAscent() const;
    float GetLineHeight() const;
    const Image& GetAtlas() const;
private:
    // CPU 글꼴 파서의 상태다. GPU 자원 생성이나 명령 실행은 포함하지 않는다.
    struct Impl;
    explicit Font(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> m_impl;
};
}
