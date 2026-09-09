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
        dy::Platform::Window window(width, height, "Graphics / Primitives");
        dy::Graphics::RendererDesc desc;
        desc.canvasShaders = {
            {ShaderData::vertex, ShaderData::vertexSize, ShaderData::vertexEntryPoint},
            {ShaderData::fragment, ShaderData::fragmentSize, ShaderData::fragmentEntryPoint}, {}};

        auto renderer=dy::Graphics::Renderer::Create(window.GetHandle(),desc);
        if(!renderer) throw std::runtime_error("Renderer creation failed.");
        dy::Graphics::Canvas canvas(width,height);
        canvas.Clear({0.04f,0.06f,0.10f,1});
        canvas.Point({70,70},{1,0.7f,0.1f,1},10);
        canvas.Line({100,70},{260,130},{0.1f,0.8f,1,1},6);
        canvas.Rect({320,45,200,100},{1,0.3f,0.4f,1},5);
        canvas.FillRect({45,210,180,120},{0.2f,0.8f,0.4f,1});
        canvas.Circle({330,275},60,{0.8f,0.5f,1,1},4);
        canvas.FillCircle({510,300},70,{1,0.65f,0.15f,1});

        while (window.IsRunning()) {
            window.PollEvents();
            if (!window.IsRunning()) break;
            (void)renderer->Render(canvas);
        }
    }
    catch(const std::exception& error) { std::cerr<<error.what()<<'\n';return 1; }
}
