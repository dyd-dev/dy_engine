#include "Viewer.h"
#include "dyf/Extends/ImGui/ImGui.h"
#include "dyf/Platform/Window.h"
#include "dyf/RHI.h"
#include <imgui.h>
#include <GLFW/glfw3.h>
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
        if(argc==3&&std::wstring(argv[1])==L"--session-helper")return viewer::RunAutoSessionHelper(fs::path(argv[2]));
        dyf::Diagnostics::SetLiveCollectorEnabled(false);
        int maxFrames = 0;
        int hideTracyAfter=0,hiddenAtFrame=0;
        uint64_t hiddenAtNs=0;
        std::optional<int64_t> collectorLimit;
        int width=1540,height=960,tracyVisible=-1;
        uint32_t attachPid=0;
        uint16_t tracyPort=8086;
        std::string cliTarget,targetArguments,targetDirectory;
        fs::path capture,report,tracySave,tracyOpen;
        bool explicitTarget=false,autoStart=true;
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
                attachPid=pid;
                explicitTarget=true;
            }
            else if(option == L"--frames"||option==L"--tracy-hide-after")
            {
                auto& number=option==L"--frames" ? maxFrames : hideTracyAfter;
                const auto utf8 = viewer::ToUtf8(value);
                const auto result = std::from_chars(utf8.data(), utf8.data() + utf8.size(), number);
                if(result.ec != std::errc{} || result.ptr != utf8.data() + utf8.size() || number < 1)
                    throw std::invalid_argument("Frame count requires a positive integer.");
            }
            else if(option == L"--capture") capture = fs::path(value);
            else if(option == L"--report") report = fs::path(value);
            else if(option == L"--tracy-save")tracySave=fs::path(value);
            else if(option == L"--tracy-open")tracyOpen=fs::path(value);
            else if(option==L"--tracy")
            {
                if(value!=L"0"&&value!=L"1")throw std::invalid_argument("--tracy requires 0 or 1.");tracyVisible=value==L"1";
            }
            else if(option==L"--tracy-port"||option==L"--width"||option==L"--height")
            {
                const auto utf8=viewer::ToUtf8(value);int number=0;
                const auto parsed=std::from_chars(utf8.data(),utf8.data()+utf8.size(),number);
                if(parsed.ec!=std::errc{}||parsed.ptr!=utf8.data()+utf8.size()||number<1||number>(option==L"--tracy-port" ? 65535 : 16384))
                    throw std::invalid_argument("Port or window size is outside its valid range.");
                if(option==L"--tracy-port")tracyPort=uint16_t(number);else if(option==L"--width")width=number;else height=number;
            }
            else if(option == L"--auto-session")
            {
                if(value!=L"0"&&value!=L"1")throw std::invalid_argument("--auto-session requires 0 or 1.");autoStart=value==L"1";
            }
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
        dyf::Platform::Window window(width, height, "dy_engine / Execution Visualizer");
        if(!window.GetHandle()) throw std::runtime_error("Window creation failed.");
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
        std::string fontError;
        if(!viewer::EmbeddedTracy::PrepareFonts(fontError))throw std::runtime_error(fontError);
        viewer::Style();
        // Session and textures must die while Gui and the RHI device are still alive.
        viewer::Viewer app;
        app.SetOwnerWindow(window.GetHandle());app.InitializeEmbedded(*gui,*device);app.AttachPid(attachPid);app.SetTracyPort(tracyPort);
        const auto tracyOverride=tracyVisible>=0 ? std::optional<bool>(tracyVisible!=0) : !tracyOpen.empty() ? std::optional<bool>(false) : std::nullopt;
        app.InitializeAutomatic(autoStart&&!explicitTarget&&cliTarget.empty()&&tracyOpen.empty(),tracyOverride);
        if(!tracyOpen.empty()&&!app.OpenTracy(tracyOpen))throw std::runtime_error("Tracy capture could not be opened.");
        if(!cliTarget.empty())
        {
            app.SetLaunchExecutable(cliTarget);
            app.launch.useRaw=true; app.launch.rawArguments=targetArguments;
            if(!targetDirectory.empty()) app.launch.directory=targetDirectory;
            app.Launch();
        }
        auto nextFrame = Clock::now();
        int rendered = 0;
        bool closeRequested=false,captureSaved=false;
        while(true)
        {
            window.PollEvents();
            if(hideTracyAfter&&rendered>=hideTracyAfter)
            {app.ShowTracy(false,false);hiddenAtFrame=rendered;hiddenAtNs=dyf::Diagnostics::LiveNowNs();hideTracyAfter=0;}
            if(!window.IsRunning()) {glfwSetWindowShouldClose(window.GetGlfwHandle(),GLFW_FALSE);closeRequested=true;}
            if(maxFrames&&rendered>=maxFrames)
            {
                if(!tracySave.empty()&&!captureSaved)
                {
                    collectorLimit=app.TracyMemoryLimit();
                    if(!app.SaveTracy(tracySave))
                    {app.message="Tracy 저장 실패 · 기록을 보존했습니다. 분석 도구에서 다시 저장하세요.";maxFrames=0;closeRequested=false;}
                    else captureSaved=true;
                }
                if(maxFrames)closeRequested=true;
            }
            app.Tick();
            if(closeRequested&&!collectorLimit)collectorLimit=app.TracyMemoryLimit();
            if(closeRequested&&app.RequestClose())break;
            const auto now = Clock::now();
            if(now < nextFrame) { window.WaitEvents(std::min(.005, std::chrono::duration<double>(nextFrame - now).count())); continue; }
            const auto extent = window.GetFramebufferSize();
            if(window.IsMinimized() || extent.width <= 0 || extent.height <= 0) { window.WaitEvents(.05); continue; }
            nextFrame = now + std::chrono::microseconds(33334);
            if(!gui->BeginFrame()) throw std::runtime_error("Gui BeginFrame failed.");
            app.Draw();
            if(closeRequested&&app.CloseCancelled()) {closeRequested=false;maxFrames=0;collectorLimit.reset();}
            gui->EndFrame();
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
        if(!report.empty())
        {
            std::ofstream metadata(report,std::ios::app);
            metadata<<"native_tracy_hidden_at_frame="<<hiddenAtFrame<<"\nnative_tracy_hidden_at_ns="<<hiddenAtNs
                <<"\nnative_tracy_memory_limit_bytes_before_stop=";
            if(collectorLimit)metadata<<*collectorLimit;else metadata<<"--";
            metadata<<'\n';if(!metadata)throw std::runtime_error("Native Tracy report metadata could not be written.");
        }
        std::printf("PASS viewer native render loop: %d frames\n", rendered);
    }
    catch(const std::exception& exception) { std::fprintf(stderr, "Execution Visualizer: %s\n", exception.what()); return 1; }
    return 0;
}
