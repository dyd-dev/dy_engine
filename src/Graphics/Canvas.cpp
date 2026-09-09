#include "Graphics/Private/CanvasData.h"
#include "Graphics/Font.h"
#include "Core/Utf8.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace dy::Graphics
{
Canvas::Canvas(uint32_t width, uint32_t height) : m_impl(std::make_unique<Impl>())
{
    if(!width || !height) throw std::invalid_argument("Canvas dimensions must be positive.");
    m_impl->width = width; m_impl->height = height;
    m_impl->viewport = {0, 0, static_cast<float>(width), static_cast<float>(height)};
    ResetClipRect();
}
Canvas::~Canvas() = default;
Canvas::Canvas(Canvas&&) noexcept = default;
Canvas& Canvas::operator=(Canvas&&) noexcept = default;
void Canvas::Clear(Math::float4 color) { m_impl->clearColor = color; m_impl->vertices.clear(); m_impl->draws.clear(); }
void Canvas::SetViewport(Rectangle viewport)
{
    if(!std::isfinite(viewport.x + viewport.y + viewport.width + viewport.height) || viewport.width <= 0 || viewport.height <= 0)
        throw std::invalid_argument("Invalid canvas viewport.");
    m_impl->viewport = viewport; ResetClipRect();
}
void Canvas::SetClipRect(Rectangle clip) { m_impl->clip = clip; }
void Canvas::ResetClipRect() { m_impl->clip = {0, 0, m_impl->viewport.width, m_impl->viewport.height}; }
void Canvas::Impl::Triangle(Math::float2 a, Math::float2 b, Math::float2 c, Math::float4 color)
{
    Draw draw{static_cast<uint32_t>(vertices.size()), 3, nullptr, viewport, clip};
    for(auto point : {a, b, c}) vertices.push_back({point.x, point.y, 0, 0, color.x, color.y, color.z, color.w});
    draws.push_back(draw);
}
void Canvas::Impl::Quad(Rectangle r, Rectangle uv, Math::float4 color, const TextureAsset* image)
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
    m_impl->Triangle(p, q, r, color); m_impl->Triangle(r, q, s, color);
}
void Canvas::Rect(Rectangle r, Math::float4 color, float width)
{
    Line({r.x,r.y}, {r.x+r.width,r.y}, color,width); Line({r.x+r.width,r.y}, {r.x+r.width,r.y+r.height},color,width);
    Line({r.x+r.width,r.y+r.height}, {r.x,r.y+r.height},color,width); Line({r.x,r.y+r.height}, {r.x,r.y},color,width);
}
void Canvas::FillRect(Rectangle r, Math::float4 color) { m_impl->Quad(r, {}, color); }
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
        m_impl->Triangle(c,{c.x+radius*std::cos(a),c.y+radius*std::sin(a)},{c.x+radius*std::cos(b),c.y+radius*std::sin(b)},color); }
}
void Canvas::Image(const TextureAsset& image, Rectangle destination, Math::float4 tint) { m_impl->Quad(destination,{0,0,1,1},tint,&image); }
void Canvas::Text(Font& font, std::string_view text, Math::float2 position, Math::float4 color)
{
    const auto& atlas=font.GetAtlas(); float x=position.x, y=position.y+font.GetAscent(); uint32_t previous=0;
    for(size_t at=0;at<text.size();)
    {
        const uint32_t codepoint=Core::DecodeUtf8(text,at);
        if(codepoint=='\n') { x=position.x; y+=font.GetLineHeight(); previous=0; continue; }
        if(previous) x+=font.GetKerning(previous,codepoint);
        const auto& g=font.GetGlyph(codepoint);
        m_impl->Quad({x+g.offsetX,y+g.offsetY,static_cast<float>(g.width),static_cast<float>(g.height)},
            {g.x/static_cast<float>(atlas.width),g.y/static_cast<float>(atlas.height),g.width/static_cast<float>(atlas.width),g.height/static_cast<float>(atlas.height)},color,&atlas);
        x+=g.advance; previous=codepoint;
    }
}
}
