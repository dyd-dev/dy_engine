#pragma once
#include <memory>
#include <string_view>
#include "Graphics/Texture.h"
#include "Math/Math.h"

namespace dy::Graphics
{
class Font;
namespace Private { class CanvasRenderer; }
struct Rectangle { float x = 0, y = 0, width = 0, height = 0; };

class Canvas
{
public:
    Canvas(uint32_t width, uint32_t height);
    ~Canvas();
    Canvas(Canvas&&) noexcept;
    Canvas& operator=(Canvas&&) noexcept;
    Canvas(const Canvas&) = delete;
    Canvas& operator=(const Canvas&) = delete;

    void Clear(Math::float4 color = {0, 0, 0, 1});
    void SetViewport(Rectangle viewport);
    void SetClipRect(Rectangle clip);
    void ResetClipRect();
    void Point(Math::float2 position, Math::float4 color, float size = 1);
    void Line(Math::float2 from, Math::float2 to, Math::float4 color, float width = 1);
    void Rect(Rectangle rectangle, Math::float4 color, float width = 1);
    void FillRect(Rectangle rectangle, Math::float4 color);
    void Circle(Math::float2 center, float radius, Math::float4 color, float width = 1);
    void FillCircle(Math::float2 center, float radius, Math::float4 color);
    void Image(const TextureAsset& image, Rectangle destination, Math::float4 tint = {1, 1, 1, 1});
    void Text(Font& font, std::string_view utf8, Math::float2 position, Math::float4 color = {1, 1, 1, 1});

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
    friend class Private::CanvasRenderer;
};
}
