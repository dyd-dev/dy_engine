#include "vertex.h"
#include "fragment.h"
#include "Platform/Window.h"
#include <stdexcept>
#include "Graphics/Canvas.h"
#include "Graphics/Font.h"
#include "Graphics/Renderer.h"
#include <iostream>
int main()
{
    try
    {
        constexpr uint32_t width = 640, height = 480;
        dy::Platform::Window window(width, height, "Graphics / Text");
        dy::Graphics::RendererDesc desc;
        desc.canvasShaders = {
            {ShaderData::vertex, ShaderData::vertexSize, ShaderData::vertexEntryPoint},
            {ShaderData::fragment, ShaderData::fragmentSize, ShaderData::fragmentEntryPoint}, {}};

        auto renderer=dy::Graphics::Renderer::Create(window.GetHandle(),desc);
        if(!renderer) throw std::runtime_error("Renderer creation failed.");
        auto font=dy::Graphics::Font::Load(DY_EXAMPLE_FONT, 32, 1024, 1024);
        if(!font) throw std::runtime_error("Font load failed: configure DY_EXAMPLE_FONT with a TrueType font.");
        dy::Graphics::Canvas canvas(width,height);
        canvas.Clear({0.04f,0.06f,0.10f,1});
        canvas.Text(*font,"Graphics = RHI commands\nText, kerning and alpha blending",{30,70});
        canvas.Text(*font,"0123456789  AVATAR",{30,190},{0.3f,0.8f,1,1});
        canvas.SetClipRect({60,260,480,60});
        canvas.Text(*font,"Clipped text across the viewport",{30,270},{1,0.7f,0.2f,0.75f});

        while (window.IsRunning()) {
            window.PollEvents();
            if (!window.IsRunning()) break;
            (void)renderer->Render(canvas);
        }
    }
    catch(const std::exception& error) { std::cerr<<error.what()<<'\n';return 1; }
}
