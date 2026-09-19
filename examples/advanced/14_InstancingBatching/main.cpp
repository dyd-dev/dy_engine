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
        options.mode = "instanced";
        if (!Parse(argc, argv, options, "draws|instanced",
            "Identical picture: 256 individual draws versus one instanced draw.")) return 0;
        Check(options.mode == "draws" || options.mode == "instanced", "Invalid --mode");
        dyf::Platform::Window window(800, 600, "Advanced / Instancing and batching");
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

        // 정점 버퍼와 인스턴스 버퍼는 서로 다른 입력 슬롯이다.
        struct Vertex { float x, y; };
        struct Instance { float x, y, scaleX, scaleY, r, g, b, a; };
        const std::array<Vertex, 6> vertices{{{-0.5f,-0.5f},{0.5f,-0.5f},{0.5f,0.5f},
            {-0.5f,-0.5f},{0.5f,0.5f},{-0.5f,0.5f}}};
        constexpr uint32_t side = 16, objectCount = side * side;
        std::array<Instance, objectCount> instances{};
        for (uint32_t i = 0; i < objectCount; ++i)
        {
            const uint32_t x = i % side, y = i / side;
            instances[i] = {-0.9f + (x + 0.5f) * 1.8f / side, -0.9f + (y + 0.5f) * 1.8f / side,
                1.55f / side, 1.55f / side, 0.2f + 0.7f * x / (side - 1),
                0.2f + 0.7f * y / (side - 1), 0.8f, 1.0f};
        }
        RHI::BufferDesc vertexDesc;
        vertexDesc.size = sizeof(vertices);
        vertexDesc.usage = RHI::BufferUsage::Vertex;
        vertexDesc.initialState = RHI::ResourceState::CopyDestination;
        auto* vertexBuffer = resources.Keep(device.CreateBuffer(vertexDesc));
        RHI::BufferDesc instanceDesc;
        instanceDesc.size = sizeof(instances);
        instanceDesc.usage = RHI::BufferUsage::Vertex;
        instanceDesc.initialState = RHI::ResourceState::CopyDestination;
        auto* instanceBuffer = resources.Keep(device.CreateBuffer(instanceDesc));

        auto* vs = resources.Keep(device.CreateShader({RHI::ShaderStage::Vertex,
            ShaderData::vertexEntryPoint, ShaderData::vertex, ShaderData::vertexSize}));
        auto* fs = resources.Keep(device.CreateShader({RHI::ShaderStage::Fragment,
            ShaderData::fragmentEntryPoint, ShaderData::fragment, ShaderData::fragmentSize}));
        const std::array<RHI::VertexBufferLayout, 2> input{{
            {0, sizeof(Vertex), RHI::VertexStepMode::Vertex},
            {1, sizeof(Instance), RHI::VertexStepMode::Instance}}};
        const std::array<RHI::VertexAttribute, 3> attributes{{
            {0, 0, RHI::Format::R32G32_FLOAT, 0},
            {1, 1, RHI::Format::R32G32B32A32_FLOAT, 0},
            {2, 1, RHI::Format::R32G32B32A32_FLOAT, 16}}};
        RHI::ColorAttachmentDesc output{swapchain.format, {}, RHI::ColorWriteMask::All};
        RHI::GraphicsPipelineDesc pipelineDesc;
        pipelineDesc.vertexShader = vs;
        pipelineDesc.fragmentShader = fs;
        pipelineDesc.topology = RHI::PrimitiveTopology::TriangleList;
        pipelineDesc.vertexBuffers = input.data();
        pipelineDesc.vertexBufferCount = uint32_t(input.size());
        pipelineDesc.vertexAttributes = attributes.data();
        pipelineDesc.vertexAttributeCount = uint32_t(attributes.size());
        pipelineDesc.raster = {RHI::FillMode::Solid, RHI::CullMode::None, RHI::FrontFace::CounterClockwise, 0, 0, 0};
        pipelineDesc.colorAttachments = &output;
        pipelineDesc.colorAttachmentCount = 1;
        auto* pipeline = resources.Keep(device.CreateGraphicsPipeline(pipelineDesc));

        uint32_t frame = 0;
        uint64_t drawCalls = 0;
        double recordMilliseconds = 0.0;
        bool captured = false;
        while (window.IsRunning() && (options.frames == 0 || frame < options.frames))
        {
            window.PollEvents();
            if (!window.IsRunning()) break;
            if (!device.BeginFrame())
            {
                Check(!device.IsLost(), "Device lost during BeginFrame");
                continue; // 최소화/resize로 준비되지 않은 프레임은 세지 않는다.
            }
            // 제출된 명령이 참조 자원을 GPU 완료까지 유지하므로 이 scope는 프레임 끝에 해제할 수 있다.
            RHI::ResourceScope frameResources(device);
            auto* commands = frameResources.Keep(device.AcquireCommandList());
            const auto recordStart = Clock::now();
            if (frame == 0)
            {
                Check(device.UpdateBuffer(*commands, vertexBuffer, 0, vertices.data(), sizeof(vertices)), "Vertex upload failed");
                const RHI::ResourceBarrierDesc uploaded{vertexBuffer, nullptr,
                    RHI::ResourceState::CopyDestination, RHI::ResourceState::VertexBuffer, {}};
                commands->ResourceBarrier(&uploaded, 1);
            }
            if (frame == 0)
            {
                Check(device.UpdateBuffer(*commands, instanceBuffer, 0, instances.data(), sizeof(instances)), "Instance upload failed");
                const RHI::ResourceBarrierDesc uploaded{instanceBuffer, nullptr,
                    RHI::ResourceState::CopyDestination, RHI::ResourceState::VertexBuffer, {}};
                commands->ResourceBarrier(&uploaded, 1);
            }

            auto* backBuffer = device.GetBackBuffer(); // resize 이후의 실제 handle/크기를 매 프레임 사용한다.
            const auto& extent = backBuffer->GetDesc();
            const RHI::ResourceBarrierDesc begin{nullptr, backBuffer,
                RHI::ResourceState::Present, RHI::ResourceState::RenderTarget, {}};
            commands->ResourceBarrier(&begin, 1);
            RHI::ColorAttachment color;
            color.texture = backBuffer;
            color.loadOp = RHI::LoadOp::Clear;
            color.storeOp = RHI::StoreOp::Store;
            color.clearColor[0] = 0.025f;
            color.clearColor[1] = 0.035f;
            color.clearColor[2] = 0.055f;
            color.clearColor[3] = 1.0f;
            commands->BeginRendering({&color, 1, nullptr});
            commands->BindGraphicsPipeline(pipeline);
            commands->BindVertexBuffer(0, vertexBuffer, 0);
            commands->BindVertexBuffer(1, instanceBuffer, 0);
            commands->SetViewport({0, 0, float(extent.width), float(extent.height), 0, 1});
            commands->SetScissor({0, 0, extent.width, extent.height});
            if (options.mode == "instanced")
            {
                commands->DrawInstanced(uint32_t(vertices.size()), objectCount, 0, 0);
                ++drawCalls;
            }
            else
            {
                // firstInstance로 같은 인스턴스 속성의 i번째 원소를 선택한다.
                for (uint32_t i = 0; i < objectCount; ++i)
                    commands->DrawInstanced(uint32_t(vertices.size()), 1, 0, i);
                drawCalls += objectCount;
            }
            commands->EndRendering();
            const RHI::ResourceBarrierDesc end{nullptr, backBuffer,
                RHI::ResourceState::RenderTarget, RHI::ResourceState::Present, {}};
            commands->ResourceBarrier(&end, 1);
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
        std::printf("mode=%s frames=%u objects=%u draw_calls=%llu cpu_record_ms=%.3f avg_record_us=%.3f\n",
            options.mode.c_str(), frame, objectCount, static_cast<unsigned long long>(drawCalls),
            recordMilliseconds, frame ? recordMilliseconds * 1000.0 / frame : 0.0);
        std::puts("Both modes use the same instance-rate attributes and pixels; only draw grouping differs. CPU recording time excludes GPU execution, present and capture.");
        return 0;
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
