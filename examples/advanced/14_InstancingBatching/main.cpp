#include "dyf/Platform/Window.h"
#include "dyf/RHI.h"

#include <fstream>
#include <iterator>
#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace RHI = dyf::RHI;

int main()
{
    const auto readShader = [](const char* path) {
        std::ifstream file(path, std::ios::binary);
        return std::vector<char>{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    };

#ifdef __APPLE__
    const char* vertexEntry="canvasVertex";
    const char* fragmentEntry="canvasFragment";
#else
    const char* vertexEntry="main";
    const char* fragmentEntry="main";
#endif
    const auto vertexBinary = readShader("Shaders/vertex.bin");
    const auto fragmentBinary = readShader("Shaders/fragment.bin");

    dyf::Platform::Window window(800, 600, "Advanced / Instancing and batching");
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
    pipelineDesc.raster.fillMode = RHI::FillMode::Solid;
    pipelineDesc.raster.cullMode = RHI::CullMode::None;
    pipelineDesc.raster.frontFace = RHI::FrontFace::CounterClockwise;
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
            (void)device.UpdateBuffer(*commands, instanceBuffer, 0, instances.data(), sizeof(instances));
            const RHI::ResourceBarrierDesc ready[] = {
                {vertexBuffer, nullptr, RHI::ResourceState::CopyDestination, RHI::ResourceState::VertexBuffer, {}},
                {instanceBuffer, nullptr, RHI::ResourceState::CopyDestination, RHI::ResourceState::VertexBuffer, {}}
            };
            commands->ResourceBarrier(ready, 2);
            uploaded = true;
        }
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
        commands->SetViewport({0, 0, float(extent.width), float(extent.height)});
        commands->SetScissor({0, 0, extent.width, extent.height});
        // 정점은 슬롯 0에서, 위치와 색상은 인스턴스마다 슬롯 1에서 읽는다.
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
