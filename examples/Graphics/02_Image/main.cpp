#include "vertex.h"
#include "fragment.h"
#include "Platform/Window.h"
#include <stdexcept>
#include "Graphics/Canvas.h"
#include "Graphics/Renderer.h"
#include <iostream>
int main()
{
    try
    {
        constexpr uint32_t width = 640, height = 480;
        dy::Platform::Window window(width, height, "Graphics / Image");
        dy::Graphics::RendererDesc desc;
        desc.canvasShaders = {
            {ShaderData::vertex, ShaderData::vertexSize, ShaderData::vertexEntryPoint},
            {ShaderData::fragment, ShaderData::fragmentSize, ShaderData::fragmentEntryPoint}, {}};

        auto renderer=dy::Graphics::Renderer::Create(window.GetHandle(),desc);
        if(!renderer) throw std::runtime_error("Renderer creation failed.");
        dy::Graphics::TextureAsset image;
        if(!dy::Graphics::LoadImage(DY_EXAMPLE_IMAGE,image)) throw std::runtime_error("Image load failed.");
        dy::Graphics::Canvas canvas(width,height);
        canvas.Clear({0.04f,0.06f,0.10f,1});
        canvas.Image(image,{40,70,240,240});
        canvas.Image(image,{340,110,240,240},{1,0.5f,0.4f,0.65f});

        while (window.IsRunning()) {
            window.PollEvents();
            if (!window.IsRunning()) break;
            (void)renderer->Render(canvas);
        }
    }
    catch(const std::exception& error) { std::cerr<<error.what()<<'\n';return 1; }
}
