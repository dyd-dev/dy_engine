#include <dyf.h>
#include <cstdio>
#include <exception>

int main()
{
    try
    {
        constexpr uint32_t width = 640, height = 480;
        dyf::Platform::Window window(width, height, "dy_engine / Canvas");
        if (!window.GetHandle()) return 1;

        // 창은 렌더러보다 오래 살아 있어야 한다.
        auto renderer = dyf::Renderer::Create(window.GetHandle());
        if (!renderer) return 1;

        dyf::Canvas canvas(width, height);
        if (!canvas.IsValid()) return 1;
        canvas.Clear({0.04f, 0.06f, 0.10f, 1.0f});
        canvas.FillRect({80, 100, 220, 140}, {0.10f, 0.65f, 0.95f, 1.0f});
        canvas.FillCircle({430, 230}, 70.0f, {1.0f, 0.65f, 0.15f, 1.0f});
        canvas.Line({80, 340}, {540, 340}, {1, 1, 1, 1}, 3.0f);

        while (window.IsRunning())
        {
            window.PollEvents();
            if (!window.IsRunning()) break;
            // 같은 CPU 명령 목록을 여러 프레임에서 다시 그릴 수 있다.
            if (!renderer->Render(canvas)) return 1;
        }
        return 0;
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}