#include "dyf/Platform/Window.h"
#include "dyf/RHI.h"
#include "vertex.h"
#include "fragment.h"
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace RHI = dyf::RHI;
using Clock = std::chrono::steady_clock;

namespace
{
void Check(bool ok, const char* operation)
{
    if (!ok) throw std::runtime_error(operation);
}
struct Options
{
    std::string mode;
    uint32_t frames = 0;
    std::string capture;
    bool validation = false;
};
bool Parse(int argc, char** argv, Options& options, const char* modes, const char* description)
{
    for (int i = 1; i < argc; ++i)
    {
        const std::string argument = argv[i];
        if (argument == "--help")
        {
            std::printf("%s\n--mode %s --frames N (0 = unlimited) --capture image.ppm --validation\n",
                description, modes);
            return false;
        }
        if (argument == "--validation") options.validation = true;
        else if ((argument == "--mode" || argument == "--frames" || argument == "--capture") && i + 1 < argc)
        {
            const std::string value = argv[++i];
            if (argument == "--mode") options.mode = value;
            else if (argument == "--capture") options.capture = value;
            else
            {
                char* end = nullptr;
                const auto count = std::strtoull(value.c_str(), &end, 10);
                Check(!value.empty() && value[0] != '-' && end && *end == '\0' && count <= UINT32_MAX,
                    "--frames requires an unsigned 32-bit integer");
                options.frames = static_cast<uint32_t>(count);
            }
        }
        else throw std::runtime_error("Unknown or incomplete argument; use --help");
    }
    return true;
}
void Capture(RHI::IDevice& device, RHI::TextureHandle texture, const std::string& path)
{
    RHI::TextureReadback image;
    Check(device.ReadTexture(texture, image), "ReadTexture failed (this backend may not support pixel readback)");
    std::ofstream file(path, std::ios::binary);
    Check(bool(file), "Cannot open capture file");
    file << "P6\n" << image.width << ' ' << image.height << "\n255\n";
    const bool bgra = image.format == RHI::Format::B8G8R8A8_UNORM
        || image.format == RHI::Format::B8G8R8A8_UNORM_SRGB;
    for (uint32_t y = 0; y < image.height; ++y)
        for (uint32_t x = 0; x < image.width; ++x)
        {
            const auto* p = image.pixels.data() + size_t(y) * image.rowPitch + size_t(x) * 4;
            const char rgb[3] = {char(p[bgra ? 2 : 0]), char(p[1]), char(p[bgra ? 0 : 2])};
            file.write(rgb, 3);
        }
    Check(bool(file), "Capture write failed");
    std::printf("capture=%s (%ux%u)\n", path.c_str(), image.width, image.height);
}
}

int main(int argc, char** argv)
{
    try
    {
        Options options;
        options.mode = "graph";
        if (!Parse(argc, argv, options, "manual|graph",
            "Identical two-pass picture: explicit barriers versus compiled RenderGraph.")) return 0;
        Check(options.mode == "manual" || options.mode == "graph", "Invalid --mode");
        dyf::Platform::Window window(800, 600, "Advanced / RenderGraph");
        Check(window.GetHandle() != nullptr, "Window creation failed");
        RHI::DeviceDesc deviceDesc;
        deviceDesc.enableValidation = options.validation;
        std::unique_ptr<RHI::IDevice> deviceOwner(RHI::IDevice::Create(deviceDesc));
        Check(bool(deviceOwner), "Device creation failed");
        auto& device = *deviceOwner;
        RHI::ResourceScope resources(device);
        RHI::SwapchainDesc swapchain;
        swapchain.window = window.GetHandle();
        swapchain.format = RHI::Format::B8G8R8A8_UNORM;
        swapchain.minimumImageCount = 2;
        swapchain.presentMode = RHI::PresentMode::Fifo;
        swapchain.allowReadback = !options.capture.empty();
        if (ShaderData::vertexSize == 0) { swapchain.initialWidth = 800; swapchain.initialHeight = 600; }
        Check(device.CreateSwapchain(swapchain), "Swapchain creation failed");

        struct Vertex { float x, y; };
        struct Constants { float scaleX, scaleY, x, y, r, g, b, a; };
        const std::array<Vertex, 6> vertices{{{-0.5f,-0.5f},{0.5f,-0.5f},{0.5f,0.5f},
            {-0.5f,-0.5f},{0.5f,0.5f},{-0.5f,0.5f}}};
        const std::array<Constants, 2> constants{{
            {1.1f, 1.1f, -0.2f, -0.12f, 0.12f, 0.42f, 0.86f, 1.0f},
            {0.9f, 0.8f, 0.28f, 0.22f, 0.95f, 0.48f, 0.12f, 1.0f}}};
        RHI::BufferDesc bufferDesc;
        bufferDesc.size = sizeof(vertices);
        bufferDesc.usage = RHI::BufferUsage::Vertex;
        bufferDesc.initialState = RHI::ResourceState::CopyDestination;
        auto* vertexBuffer = resources.Keep(device.CreateBuffer(bufferDesc));
        auto* vs = resources.Keep(device.CreateShader({RHI::ShaderStage::Vertex,
            ShaderData::vertexEntryPoint, ShaderData::vertex, ShaderData::vertexSize}));
        auto* fs = resources.Keep(device.CreateShader({RHI::ShaderStage::Fragment,
            ShaderData::fragmentEntryPoint, ShaderData::fragment, ShaderData::fragmentSize}));
        const RHI::VertexBufferLayout input{0, sizeof(Vertex), RHI::VertexStepMode::Vertex};
        const RHI::VertexAttribute attribute{0, 0, RHI::Format::R32G32_FLOAT, 0};
        const RHI::ColorAttachmentDesc output{swapchain.format, {}, RHI::ColorWriteMask::All};
        RHI::GraphicsPipelineDesc pipelineDesc;
        pipelineDesc.vertexShader = vs;
        pipelineDesc.fragmentShader = fs;
        pipelineDesc.topology = RHI::PrimitiveTopology::TriangleList;
        pipelineDesc.vertexBuffers = &input;
        pipelineDesc.vertexBufferCount = 1;
        pipelineDesc.vertexAttributes = &attribute;
        pipelineDesc.vertexAttributeCount = 1;
        pipelineDesc.raster = {RHI::FillMode::Solid, RHI::CullMode::None, RHI::FrontFace::CounterClockwise, 0, 0, 0};
        pipelineDesc.colorAttachments = &output;
        pipelineDesc.colorAttachmentCount = 1;
        pipelineDesc.layout = {nullptr, 0, sizeof(Constants), RHI::ShaderStageFlags::Vertex, 15};
        auto* pipeline = resources.Keep(device.CreateGraphicsPipeline(pipelineDesc));

        uint32_t frame = 0, callbacks = 0;
        double compileMilliseconds = 0.0, recordMilliseconds = 0.0;
        bool captured = false;
        while (window.IsRunning() && (options.frames == 0 || frame < options.frames))
        {
            window.PollEvents();
            if (!window.IsRunning()) break;
            if (!device.BeginFrame())
            {
                Check(!device.IsLost(), "Device lost during BeginFrame");
                continue;
            }
            RHI::ResourceScope frameResources(device);
            auto* commands = frameResources.Keep(device.AcquireCommandList());
            if (frame == 0)
            {
                Check(device.UpdateBuffer(*commands, vertexBuffer, 0, vertices.data(), sizeof(vertices)), "Vertex upload failed");
                const RHI::ResourceBarrierDesc uploaded{vertexBuffer, nullptr,
                    RHI::ResourceState::CopyDestination, RHI::ResourceState::VertexBuffer, {}};
                commands->ResourceBarrier(&uploaded, 1);
            }
            auto* backBuffer = device.GetBackBuffer();
            const auto extent = backBuffer->GetDesc();
            // 두 경로가 정확히 같은 패스 본문을 사용한다. callback 안에서는 상태 전환을 하지 않는다.
            const auto drawPass = [&](RHI::ICommandList* list, uint32_t pass)
            {
                RHI::ColorAttachment color;
                color.texture = backBuffer;
                color.loadOp = pass == 0 ? RHI::LoadOp::Clear : RHI::LoadOp::Load;
                color.storeOp = RHI::StoreOp::Store;
                color.clearColor[0] = 0.025f;
                color.clearColor[1] = 0.035f;
                color.clearColor[2] = 0.055f;
                color.clearColor[3] = 1.0f;
                list->BeginRendering({&color, 1, nullptr});
                list->BindGraphicsPipeline(pipeline);
                list->BindVertexBuffer(0, vertexBuffer, 0);
                list->SetViewport({0, 0, float(extent.width), float(extent.height), 0, 1});
                list->SetScissor({0, 0, extent.width, extent.height});
                list->SetInlineConstants(0, sizeof(Constants), &constants[pass]);
                list->DrawInstanced(uint32_t(vertices.size()), 1, 0, 0);
                list->EndRendering();
                ++callbacks;
            };
            const auto recordStart = Clock::now();
            if (options.mode == "manual")
            {
                const RHI::ResourceBarrierDesc begin{nullptr, backBuffer,
                    RHI::ResourceState::Present, RHI::ResourceState::RenderTarget, {}};
                commands->ResourceBarrier(&begin, 1);
                drawPass(commands, 0);
                // 상태가 같아도 앞선 color write와 뒤의 load/write 사이에는 의존성이 필요하다.
                const RHI::ResourceBarrierDesc between{nullptr, backBuffer,
                    RHI::ResourceState::RenderTarget, RHI::ResourceState::RenderTarget, {}};
                commands->ResourceBarrier(&between, 1);
                drawPass(commands, 1);
                const RHI::ResourceBarrierDesc end{nullptr, backBuffer,
                    RHI::ResourceState::RenderTarget, RHI::ResourceState::Present, {}};
                commands->ResourceBarrier(&end, 1);
            }
            else
            {
                // swapchain handle은 프레임마다 달라질 수 있으므로 현재 자원으로 새 계획을 만든다.
                RHI::RenderGraph graph;
                const auto target = graph.ImportTexture("backbuffer", backBuffer,
                    RHI::ResourceState::Present, RHI::ResourceState::Present);
                const auto geometry = graph.ImportBuffer("quad", vertexBuffer,
                    RHI::ResourceState::VertexBuffer, RHI::ResourceState::VertexBuffer);
                Check(target.IsValid() && geometry.IsValid(), "Graph import failed");
                graph.AddPass("clear-and-blue")
                    .Read(geometry, RHI::ResourceState::VertexBuffer)
                    .Write(target, RHI::ResourceState::RenderTarget)
                    .SetExecute([&](RHI::ICommandList* list) { drawPass(list, 0); });
                graph.AddPass("load-and-orange")
                    .Read(geometry, RHI::ResourceState::VertexBuffer)
                    .Write(target, RHI::ResourceState::RenderTarget)
                    .SetExecute([&](RHI::ICommandList* list) { drawPass(list, 1); });
                const auto compileStart = Clock::now();
                Check(graph.Compile(), "RenderGraph Compile failed");
                compileMilliseconds += std::chrono::duration<double, std::milli>(Clock::now() - compileStart).count();
                if (frame == 0)
                {
                    std::printf("compiled order:");
                    for (const auto& name : graph.GetExecutionOrderNames()) std::printf(" %s", name.c_str());
                    std::puts("");
                }
                const RHI::RenderGraph& compiled = graph;
                Check(compiled.Execute(commands), "RenderGraph Execute failed");
                // Execute는 명령 기록이다. 아래 Close/Submit/Present가 실제 GPU 실행과 표시를 담당한다.
            }
            Check(commands->Close(), "Command recording failed");
            recordMilliseconds += std::chrono::duration<double, std::milli>(Clock::now() - recordStart).count();
            Check(device.Submit(&commands, 1), "Submission failed");
            if (!captured && !options.capture.empty() && (options.frames == 0 || frame + 1 == options.frames))
            {
                Capture(device, backBuffer, options.capture);
                captured = true;
            }
            Check(device.Present(), "Present failed");
            ++frame;
        }
        Check(device.WaitIdle(), "WaitIdle failed");
        std::printf("mode=%s frames=%u pass_callbacks=%u cpu_record_ms=%.3f compile_ms=%.3f\n",
            options.mode.c_str(), frame, callbacks, recordMilliseconds, compileMilliseconds);
        std::puts("Graph creates Present->RenderTarget, same-state write dependency, and RenderTarget->Present barriers. CPU recording includes graph setup/compile; GPU execution is not timed.");
        return 0;
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
