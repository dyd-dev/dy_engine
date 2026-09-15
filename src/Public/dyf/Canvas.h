#pragma once
#include "dyf/Image.h"
#include <vector>
#include <string_view>
#include "dyf/Math/Math.h"
#include "dyf/Types.h"

namespace dyf
{
class Font;
class Renderer;

class Canvas
{
public:
    Canvas(uint32_t width, uint32_t height);
    ~Canvas();
    Canvas(Canvas&&) noexcept;
    Canvas& operator=(Canvas&&) noexcept;
    Canvas(const Canvas&) = delete;
    Canvas& operator=(const Canvas&) = delete;

    // 색상과 tint의 RGB는 화면의 sRGB 값이며 알파는 0~1의 불투명도다.
    void Clear(Math::float4 color = {0, 0, 0, 1});
    // 이후 명령의 좌표 원점과 그릴 영역을 지정한다. 기본값은 Canvas 전체 영역이다.
    bool SetViewport(Rectangle viewport);
    bool IsValid() const;
    // Viewport 내부 좌표로 잘라낼 영역을 지정한다.
    void SetClipRect(Rectangle clip);
    void ResetClipRect();
    void Point(Math::float2 position, Math::float4 color = {1, 1, 1, 1}, float size = 1);
    void Line(Math::float2 from, Math::float2 to, Math::float4 color = {1, 1, 1, 1}, float width = 1);
    void Rect(Rectangle rectangle, Math::float4 color = {1, 1, 1, 1}, float width = 1);
    void FillRect(Rectangle rectangle, Math::float4 color = {1, 1, 1, 1});
    void Circle(Math::float2 center, float radius, Math::float4 color = {1, 1, 1, 1}, float width = 1);
    void FillCircle(Math::float2 center, float radius, Math::float4 color = {1, 1, 1, 1});
    // tint는 색상과 알파에 곱하는 값이다. 기본 흰색은 원래 색을 유지한다.
    // 이미지 복사는 픽셀 저장소를 공유하며 Canvas가 그릴 때까지 수명을 유지한다.
    void Image(const dyf::Image& image, Rectangle destination, Math::float4 tint = {1, 1, 1, 1});
    bool Text(Font& font, std::string_view utf8, Math::float2 position, Math::float4 color = {1, 1, 1, 1});

private:
    // CPU 도형/이미지/글자 목록이다. GPU 업로드와 draw 명령은 Renderer가 수행한다.
    struct Vertex { float x, y, u, v, r, g, b, a; };
    struct Draw
    {
        uint32_t first = 0, count = 0;
        dyf::Image image;
        Rectangle viewport, clip;
    };
    uint32_t width = 0, height = 0;
    Math::float4 clearColor = {0, 0, 0, 1};
    Rectangle viewport, clip;
    std::vector<Vertex> vertices;
    std::vector<Draw> draws;

    void Triangle(Math::float2 a, Math::float2 b, Math::float2 c, Math::float4 color);
    void Quad(Rectangle rectangle, Rectangle uv, Math::float4 color, const dyf::Image& image = {});
    friend class Renderer;
};
}
