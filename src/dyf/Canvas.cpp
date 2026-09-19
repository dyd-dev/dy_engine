#include "dyf/Image.h"
#include "dyf/Canvas.h"
#include "dyf/Font.h"
#include "dyf/Utf8.h"
#include <cmath>
#include <cstdio>
#include <exception>
#include <utility>

namespace dyf
{
Canvas::Canvas(uint32_t width, uint32_t height)
{
    if(!width || !height) std::fprintf(stderr, "dyf: Canvas dimensions must be positive.\n");
    this->width = width; this->height = height;
    viewport = {0, 0, static_cast<float>(width), static_cast<float>(height)};
    ResetClipRect();
}
Canvas::~Canvas() = default;
// 이동 뒤 원본은 기존과 같이 유효하지 않은 Canvas가 된다.
Canvas::Canvas(Canvas&& other) noexcept { *this=std::move(other); }
Canvas& Canvas::operator=(Canvas&& other) noexcept
{
    if(this!=&other)
    {
        width=std::exchange(other.width,0);height=std::exchange(other.height,0);
        clearColor=other.clearColor;viewport=other.viewport;clip=other.clip;
        vertices=std::move(other.vertices);draws=std::move(other.draws);
    }
    return *this;
}
void Canvas::Clear(Math::float4 color)
{
    clearColor = color;
    vertices.clear();
    draws.clear();
    viewport = {0, 0, static_cast<float>(width), static_cast<float>(height)};
    ResetClipRect();
}
bool Canvas::IsValid() const { return width && height; }
bool Canvas::SetViewport(Rectangle viewport)
{
    if(!std::isfinite(viewport.x + viewport.y + viewport.width + viewport.height) || viewport.width <= 0 || viewport.height <= 0)
        { std::fprintf(stderr, "dyf: invalid canvas viewport.\n"); return false; }
    this->viewport = viewport; ResetClipRect();
    return true;
}
void Canvas::SetClipRect(Rectangle clip) { this->clip = clip; }
void Canvas::ResetClipRect() { clip = {0, 0, viewport.width, viewport.height}; }
void Canvas::Triangle(Math::float2 a, Math::float2 b, Math::float2 c, Math::float4 color)
{
    Draw draw{static_cast<uint32_t>(vertices.size()), 3, {}, viewport, clip};
    for(auto point : {a, b, c}) vertices.push_back({point.x, point.y, 0, 0, color.x, color.y, color.z, color.w});
    draws.push_back(draw);
}
void Canvas::Quad(Rectangle r, Rectangle uv, Math::float4 color, const dyf::Image& image)
{
    if(r.width <= 0 || r.height <= 0) return;
    Draw draw{static_cast<uint32_t>(vertices.size()), 6, image, viewport, clip};
    for(auto p : {Math::float2{0, 0}, {1, 0}, {0, 1}, {0, 1}, {1, 0}, {1, 1}})
        vertices.push_back({r.x + p.x * r.width, r.y + p.y * r.height, uv.x + p.x * uv.width, uv.y + p.y * uv.height,
            color.x, color.y, color.z, color.w});
    draws.push_back(draw);
}
void Canvas::Point(Math::float2 p, Math::float4 color, float size) { FillRect({p.x - size / 2, p.y - size / 2, size, size}, color); }
void Canvas::Line(Math::float2 a, Math::float2 b, Math::float4 color, float width)
{
    if(width <= 0) return;
    const float dx = b.x - a.x, dy = b.y - a.y, length = std::hypot(dx, dy);
    if(length <= 0) { Point(a, color, width); return; }
    const float nx = -dy / length * width / 2, ny = dx / length * width / 2;
    const Math::float2 p{a.x + nx, a.y + ny}, q{b.x + nx, b.y + ny}, r{a.x - nx, a.y - ny}, s{b.x - nx, b.y - ny};
    Triangle(p, q, r, color); Triangle(r, q, s, color);
}
void Canvas::Rect(Rectangle r, Math::float4 color, float width)
{
    Line({r.x,r.y}, {r.x+r.width,r.y}, color,width); Line({r.x+r.width,r.y}, {r.x+r.width,r.y+r.height},color,width);
    Line({r.x+r.width,r.y+r.height}, {r.x,r.y+r.height},color,width); Line({r.x,r.y+r.height}, {r.x,r.y},color,width);
}
void Canvas::FillRect(Rectangle r, Math::float4 color) { Quad(r, {}, color); }
void Canvas::Circle(Math::float2 c, float radius, Math::float4 color, float width)
{
    if(radius <= 0) return;
    for(uint32_t i=0; i<64; ++i) { const float a=i*6.283185307f/64,b=(i+1)*6.283185307f/64;
        Line({c.x+radius*std::cos(a),c.y+radius*std::sin(a)}, {c.x+radius*std::cos(b),c.y+radius*std::sin(b)},color,width); }
}
void Canvas::FillCircle(Math::float2 c, float radius, Math::float4 color)
{
    if(radius <= 0) return;
    for(uint32_t i=0; i<64; ++i) { const float a=i*6.283185307f/64,b=(i+1)*6.283185307f/64;
        Triangle(c,{c.x+radius*std::cos(a),c.y+radius*std::sin(a)},{c.x+radius*std::cos(b),c.y+radius*std::sin(b)},color); }
}
void Canvas::Image(const dyf::Image& image, Rectangle destination, Math::float4 tint)
{
    if(!image.IsValid()) {std::fprintf(stderr,"dyf: Canvas image has invalid RGBA8 pixels.\n");return;}
    Quad(destination,{0,0,1,1},tint,image);
}
bool Canvas::Text(Font& font, std::string_view text, Math::float2 position, Math::float4 color)
{
    try
    {
    for(size_t at=0; at<text.size();) { const auto codepoint=DecodeUtf8(text,at); if(codepoint!='\n') { FontGlyph glyph; if(!font.GetGlyph(codepoint,glyph)) return false; } }
    // 글리프를 모두 준비한 뒤 atlas를 공유해야 UV와 픽셀이 같은 상태를 가리킨다.
    const auto atlas=font.GetAtlas();
    float x=position.x, y=position.y+font.GetAscent(); uint32_t previous=0;
    for(size_t at=0;at<text.size();)
    {
        const uint32_t codepoint=DecodeUtf8(text,at);
        if(codepoint=='\n') { x=position.x; y+=font.GetLineHeight(); previous=0; continue; }
        if(previous) x+=font.GetKerning(previous,codepoint);
        FontGlyph g;
        if(!font.GetGlyph(codepoint,g)) return false;
        Quad({x+g.offsetX,y+g.offsetY,static_cast<float>(g.width),static_cast<float>(g.height)},
            {g.x/static_cast<float>(atlas.GetWidth()),g.y/static_cast<float>(atlas.GetHeight()),g.width/static_cast<float>(atlas.GetWidth()),g.height/static_cast<float>(atlas.GetHeight())},color,atlas);
        x+=g.advance; previous=codepoint;
    }
    return true;
    }
    catch(const std::exception& error) { std::fprintf(stderr,"dyf: text: %s\n",error.what()); return false; }
}
}
