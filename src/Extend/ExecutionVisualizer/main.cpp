#include "Viewer.h"
#include "dyf/Extends/ImGui/ImGui.h"
#include "dyf/Platform/Window.h"
#include "dyf/RHI.h"
#include <imgui.h>
#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>

using Clock = std::chrono::steady_clock;
namespace RHI = dyf::RHI;
namespace fs = std::filesystem;

static bool Capture(RHI::IDevice& device, RHI::TextureHandle target, const fs::path& path)
{
    RHI::TextureReadback pixels;
    if(!device.ReadTexture(target, pixels)) return false;
    std::ofstream output(path, std::ios::binary);
    output << "P6\n" << pixels.width << ' ' << pixels.height << "\n255\n";
    const bool bgra = pixels.format == RHI::Format::B8G8R8A8_UNORM || pixels.format == RHI::Format::B8G8R8A8_UNORM_SRGB;
    for(uint32_t y = 0; y < pixels.height; ++y) for(uint32_t x = 0; x < pixels.width; ++x)
    {
        const auto* p = pixels.pixels.data() + y * pixels.rowPitch + x * 4;
        const char rgb[] = {char(p[bgra ? 2 : 0]), char(p[1]), char(p[bgra ? 0 : 2])};
        output.write(rgb, 3);
    }
    return output.good();
}

int wmain(int argc, wchar_t** argv)
{
    try
    {
        dyf::Diagnostics::SetLiveCollectorEnabled(false);
        viewer::Viewer app;
        int maxFrames = 0;
        std::string cliTarget,targetArguments,targetDirectory;
        fs::path capture,report;
        for(int i = 1; i < argc; ++i)
        {
            const std::wstring option(argv[i]);
            if(i + 1 >= argc) throw std::invalid_argument("Each option requires a value.");
            const std::wstring value(argv[++i]);
            if(option == L"--attach-pid")
            {
                uint32_t pid = 0;
                const auto utf8 = viewer::ToUtf8(value);
                const auto result = std::from_chars(utf8.data(), utf8.data() + utf8.size(), pid);
                if(result.ec != std::errc{} || result.ptr != utf8.data() + utf8.size() || !pid)
                    throw std::invalid_argument("--attach-pid requires a positive process ID.");
                app.AttachPid(pid);
            }
            else if(option == L"--frames")
            {
                const auto utf8 = viewer::ToUtf8(value);
                const auto result = std::from_chars(utf8.data(), utf8.data() + utf8.size(), maxFrames);
                if(result.ec != std::errc{} || result.ptr != utf8.data() + utf8.size() || maxFrames < 1)
                    throw std::invalid_argument("--frames requires a positive integer.");
            }
            else if(option == L"--capture") capture = fs::path(value);
            else if(option == L"--report") report = fs::path(value);
            else
            {
                const auto utf8 = viewer::ToUtf8(value);
                auto* destination = option == L"--target" ? &cliTarget : option == L"--arguments" ? &targetArguments
                    : option == L"--working-directory" ? &targetDirectory : nullptr;
                if(!destination) throw std::invalid_argument("Unknown command-line option.");
                *destination=utf8;
            }
        }
        if(!capture.empty() && !maxFrames) maxFrames = 90;
        dyf::Platform::Window window(1540, 960, "dy_engine / Execution Visualizer");
        if(!window.GetHandle()) throw std::runtime_error("Window creation failed.");
        app.SetOwnerWindow(window.GetHandle());
        RHI::DeviceDesc desc;
        std::unique_ptr<RHI::IDevice> device(RHI::IDevice::Create(desc));
        if(!device) throw std::runtime_error("GUI device creation failed.");
        RHI::SwapchainDesc swapchain;
        swapchain.window = window.GetHandle(); swapchain.format = RHI::Format::B8G8R8A8_UNORM;
        swapchain.presentMode = RHI::PresentMode::Immediate; swapchain.allowReadback = !capture.empty();
        if(!device->CreateSwapchain(swapchain)) throw std::runtime_error("GUI swapchain creation failed.");
        auto gui = dyf::Gui::Create(window, *device);
        if(!gui) throw std::runtime_error("Gui creation failed.");
        ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_DockingEnable | ImGuiConfigFlags_NavEnableKeyboard;
        ImGui::GetIO().IniFilename = nullptr;
        ImFontConfig font; font.OversampleH = font.OversampleV = 1;
        static ImVector<ImWchar> fontRanges;
        ImFontGlyphRangesBuilder glyphs;
        glyphs.AddRanges(ImGui::GetIO().Fonts->GetGlyphRangesKorean());
        glyphs.AddChar(0x2192); // The UI's right arrow is outside the Korean ranges.
        glyphs.BuildRanges(&fontRanges);
        if(!ImGui::GetIO().Fonts->AddFontFromFileTTF("C:/Windows/Fonts/malgun.ttf", 22, &font,
            fontRanges.Data)) throw std::runtime_error("Korean font could not be loaded.");
        viewer::Style();
        if(!cliTarget.empty())
        {
            app.SetLaunchExecutable(cliTarget);
            app.launch.useRaw=true; app.launch.rawArguments=targetArguments;
            if(!targetDirectory.empty()) app.launch.directory=targetDirectory;
            app.Launch();
        }
        auto nextFrame = Clock::now();
        int rendered = 0;
        while(window.IsRunning() && (!maxFrames || rendered < maxFrames))
        {
            window.PollEvents();
            app.Tick();
            const auto now = Clock::now();
            if(now < nextFrame) { window.WaitEvents(std::min(.005, std::chrono::duration<double>(nextFrame - now).count())); continue; }
            const auto extent = window.GetFramebufferSize();
            if(window.IsMinimized() || extent.width <= 0 || extent.height <= 0) { window.WaitEvents(.05); continue; }
            nextFrame = now + std::chrono::microseconds(33334);
            if(!gui->BeginFrame()) throw std::runtime_error("Gui BeginFrame failed.");
            app.Draw(); gui->EndFrame();
            if(!device->BeginFrame())
            {
                if(device->IsLost()) throw std::runtime_error("GUI device was lost.");
                continue;
            }
            RHI::ResourceScope resources(*device);
            auto* commands = resources.Keep(device->AcquireCommandList());
            auto* target = device->GetBackBuffer();
            if(!target) throw std::runtime_error("GUI backbuffer is unavailable.");
            const RHI::ResourceBarrierDesc begin{nullptr, target, RHI::ResourceState::Present, RHI::ResourceState::RenderTarget, {}};
            commands->ResourceBarrier(&begin, 1);
            RHI::ColorAttachment color;
            color.texture = target; color.loadOp = RHI::LoadOp::Clear; color.storeOp = RHI::StoreOp::Store; color.clearColor[3] = 1;
            commands->BeginRendering({&color, 1, nullptr}); commands->EndRendering();
            const RHI::ResourceBarrierDesc end{nullptr, target, RHI::ResourceState::RenderTarget, RHI::ResourceState::Present, {}};
            commands->ResourceBarrier(&end, 1);
            if(!gui->Record(*commands, target) || !commands->Close() || !device->Submit(&commands, 1))
                throw std::runtime_error("GUI record or submit failed.");
            if(!capture.empty() && rendered == maxFrames - 1 && !Capture(*device, target, capture))
                throw std::runtime_error("GUI readback capture failed.");
            if(!device->Present()) throw std::runtime_error("GUI Present failed.");
            app.Presented(dyf::Diagnostics::LiveNowNs());
            ++rendered;
        }
        if(!device->WaitIdle()) throw std::runtime_error("GUI WaitIdle failed.");
        if(!report.empty() && !app.WriteReport(report,uint64_t(rendered))) throw std::runtime_error("GUI report could not be written.");
        std::printf("PASS viewer native render loop: %d frames\n", rendered);
    }
    catch(const std::exception& exception) { std::fprintf(stderr, "Execution Visualizer: %s\n", exception.what()); return 1; }
    return 0;
}
