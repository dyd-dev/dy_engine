#include "dyf/Platform/Window.h"
#include "dyf/RHI.h"
#include <cstdio>
#include <string>
#include <utility>

#include <fstream>
#include <iterator>
#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace RHI = dyf::RHI;

int main()
{
    // 예제 폴더에서 실행한다. 컴파일러가 포함된 엔진은 GLSL을 컴파일하고, 그 외에는 .bin을 읽는다.
    const auto loadShader = [](dyf::RHI::ShaderStage stage, const char* sourcePath, const char* binaryPath)
    {
        bool compile = dyf::RHI::IsShaderCompilerAvailable();
        const char* path = compile ? sourcePath : binaryPath;
        std::ifstream file(path, std::ios::binary);
        if(!file && compile)
        {
            compile = false;
            path = binaryPath;
            file.open(path, std::ios::binary);
        }
        if(!file) { std::fprintf(stderr, "Cannot open shader: %s\n", path); return std::vector<uint8_t>{}; }
        const std::string data{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
        if(!compile) return std::vector<uint8_t>(data.begin(), data.end());
        auto result = dyf::RHI::CompileShader({stage, data, "main", sourcePath});
        if(!result.diagnostics.empty()) std::fprintf(stderr, "%s\n", result.diagnostics.c_str());
        return std::move(result.binary);
    };

    const auto vertexBinary = loadShader(dyf::RHI::ShaderStage::Vertex, "Shaders/canvas_vs.glsl", "Shaders/vertex.bin");
#ifdef DY_EXAMPLE_METAL
    const char* vertexEntry="canvasVertex";
#else
    const char* vertexEntry="main";
#endif
    const auto fragmentBinary = loadShader(dyf::RHI::ShaderStage::Fragment, "Shaders/canvas_ps.glsl", "Shaders/fragment.bin");
#ifdef DY_EXAMPLE_METAL
    const char* fragmentEntry="canvasFragment";
#else
    const char* fragmentEntry="main";
#endif
    if(vertexBinary.empty() || fragmentBinary.empty()) return 1;

    dyf::Platform::Window window(800, 600, "Advanced / Resource reuse");
    std::unique_ptr<RHI::IDevice> owner(RHI::IDevice::Create({}));
    auto& device = *owner;
    RHI::ResourceScope resources(device);
    RHI::SwapchainDesc swapchain;
    swapchain.window = window.GetHandle();
    swapchain.format = RHI::DefaultSwapchainFormat;
    (void)device.CreateSwapchain(swapchain);

    struct Vertex { float x, y; };
    struct Instance { float x, y, scaleX, scaleY, r, g, b, a; };
    const std::array<Vertex, 6> vertices{{
        {-0.5f, -0.5f}, {0.5f, -0.5f}, {0.5f, 0.5f},
        {-0.5f, -0.5f}, {0.5f, 0.5f}, {-0.5f, 0.5f}
    }};
    constexpr uint32_t side = 16, objectCount = side * side;
    std::array<Instance, objectCount> instances{};
    for(uint32_t i = 0; i < objectCount; ++i)
    {
        const uint32_t x = i % side, y = i / side;
        instances[i] = {
            -0.9f + (x + 0.5f) * 1.8f / side, -0.9f + (y + 0.5f) * 1.8f / side,
            1.55f / side, 1.55f / side,
            0.2f + 0.7f * x / (side - 1), 0.2f + 0.7f * y / (side - 1), 0.8f, 1.0f
        };
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

    auto* vertexShader = resources.Keep(device.CreateShader({RHI::ShaderStage::Vertex,
        vertexEntry, vertexBinary.data(), vertexBinary.size()}));
    auto* fragmentShader = resources.Keep(device.CreateShader({RHI::ShaderStage::Fragment,
        fragmentEntry, fragmentBinary.data(), fragmentBinary.size()}));
    const RHI::VertexBufferLayout inputs[] = {
        {0, sizeof(Vertex), RHI::VertexStepMode::Vertex},
        {1, sizeof(Instance), RHI::VertexStepMode::Instance}
    };
    const RHI::VertexAttribute attributes[] = {
        {0, 0, RHI::Format::R32G32_FLOAT, 0},
        {1, 1, RHI::Format::R32G32B32A32_FLOAT, 0},
        {2, 1, RHI::Format::R32G32B32A32_FLOAT, 16}
    };
    const RHI::ColorAttachmentDesc output{swapchain.format, {}, RHI::ColorWriteMask::All};
    RHI::GraphicsPipelineDesc pipelineDesc;
    pipelineDesc.vertexShader = vertexShader;
    pipelineDesc.fragmentShader = fragmentShader;
    pipelineDesc.topology = RHI::PrimitiveTopology::TriangleList;
    pipelineDesc.vertexBuffers = inputs;
    pipelineDesc.vertexBufferCount = 2;
    pipelineDesc.vertexAttributes = attributes;
    pipelineDesc.vertexAttributeCount = 3;
    pipelineDesc.raster = {RHI::FillMode::Solid, RHI::CullMode::None, RHI::FrontFace::CounterClockwise, 0, 0, 0};
    pipelineDesc.colorAttachments = &output;
    pipelineDesc.colorAttachmentCount = 1;
    auto* pipeline = resources.Keep(device.CreateGraphicsPipeline(pipelineDesc));

    bool uploaded = false;
    while(window.IsRunning())
    {
        window.PollEvents();
        if(!window.IsRunning()) break;
        if(!device.BeginFrame()) continue;
        RHI::ResourceScope frameResources(device);
        auto* commands = frameResources.Keep(device.AcquireCommandList());
        if(!uploaded)
        {
            (void)device.UpdateBuffer(*commands, vertexBuffer, 0, vertices.data(), sizeof(vertices));
            const RHI::ResourceBarrierDesc ready[] = {
                {vertexBuffer, nullptr, RHI::ResourceState::CopyDestination, RHI::ResourceState::VertexBuffer, {}}
            };
            commands->ResourceBarrier(ready, 1);
        }
        if(uploaded)
        {
            const RHI::ResourceBarrierDesc writable{instanceBuffer, nullptr,
                RHI::ResourceState::VertexBuffer, RHI::ResourceState::CopyDestination, {}};
            commands->ResourceBarrier(&writable, 1);
        }
        (void)device.UpdateBuffer(*commands, instanceBuffer, 0, instances.data(), sizeof(instances));
        const RHI::ResourceBarrierDesc readable{instanceBuffer, nullptr,
            RHI::ResourceState::CopyDestination, RHI::ResourceState::VertexBuffer, {}};
        commands->ResourceBarrier(&readable, 1);
        uploaded = true;
        auto* target = device.GetBackBuffer();
        const auto extent = target->GetDesc();
        const RHI::ResourceBarrierDesc begin{nullptr, target,
            RHI::ResourceState::Present, RHI::ResourceState::RenderTarget, {}};
        commands->ResourceBarrier(&begin, 1);
        RHI::ColorAttachment color;
        color.texture = target;
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
        commands->DrawInstanced(uint32_t(vertices.size()), objectCount, 0, 0);
        commands->EndRendering();
        const RHI::ResourceBarrierDesc end{nullptr, target,
            RHI::ResourceState::RenderTarget, RHI::ResourceState::Present, {}};
        commands->ResourceBarrier(&end, 1);
        (void)commands->Close();
        (void)device.Submit(&commands, 1);
        (void)device.Present();
    }
    (void)device.WaitIdle();
}
