#pragma once
#include "Graphics/Canvas.h"
#include "Graphics/ShaderLayout.h"
#include <vector>

namespace dy::Graphics
{
struct Canvas::Impl
{
    using Vertex = ShaderLayout::CanvasVertex;
    struct Draw
    {
        uint32_t first = 0, count = 0;
        const TextureAsset* image = nullptr;
        Rectangle viewport, clip;
    };
    uint32_t width = 0, height = 0;
    Math::float4 clearColor = {0, 0, 0, 1};
    Rectangle viewport, clip;
    std::vector<Vertex> vertices;
    std::vector<Draw> draws;

    void Triangle(Math::float2 a, Math::float2 b, Math::float2 c, Math::float4 color);
    void Quad(Rectangle rectangle, Rectangle uv, Math::float4 color, const TextureAsset* image = nullptr);
};
}
