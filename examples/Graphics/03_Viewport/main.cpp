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
        dy::Platform::Window window(width, height, "Graphics / Viewport and clip");
        dy::Graphics::RendererDesc desc;
        desc.canvasShaders = {
            {ShaderData::vertex, ShaderData::vertexSize, ShaderData::vertexEntryPoint},
            {ShaderData::fragment, ShaderData::fragmentSize, ShaderData::fragmentEntryPoint}, {}};

        auto renderer=dy::Graphics::Renderer::Create(window.GetHandle(),desc);
        if(!renderer) throw std::runtime_error("Renderer creation failed.");
        dy::Graphics::Canvas canvas(width,height);
        canvas.Clear({0.04f,0.06f,0.10f,1});
        canvas.SetViewport({0,0,320,480});
        canvas.FillRect({20,20,280,440},{0.1f,0.2f,0.4f,1});
        canvas.SetClipRect({60,100,200,240});
        canvas.FillCircle({160,240},150,{1,0.4f,0.2f,1});
        canvas.SetViewport({320,0,320,480});
        canvas.FillRect({20,20,280,440},{0.15f,0.35f,0.2f,1});
        canvas.Line({0,0},{320,480},{1,1,1,1},12);

        while (window.IsRunning()) {
            window.PollEvents();
            if (!window.IsRunning()) break;
            (void)renderer->Render(canvas);
        }
    }
    catch(const std::exception& error) { std::cerr<<error.what()<<'\n';return 1; }
}
