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

    dyf::Platform::Window window(800,600,"Advanced / Render graph");
    std::unique_ptr<RHI::IDevice> owner(RHI::IDevice::Create({}));
    auto& device=*owner;
    RHI::ResourceScope resources(device);
    RHI::SwapchainDesc swapchain;
    swapchain.window=window.GetHandle();
    swapchain.format=RHI::DefaultSwapchainFormat;
    (void)device.CreateSwapchain(swapchain);

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
        vertexEntry, vertexBinary.data(), vertexBinary.size()}));
    auto* fs = resources.Keep(device.CreateShader({RHI::ShaderStage::Fragment,
        fragmentEntry, fragmentBinary.data(), fragmentBinary.size()}));
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
    pipelineDesc.raster.fillMode = RHI::FillMode::Solid;
    pipelineDesc.raster.cullMode = RHI::CullMode::None;
    pipelineDesc.raster.frontFace = RHI::FrontFace::CounterClockwise;
    pipelineDesc.colorAttachments = &output;
    pipelineDesc.colorAttachmentCount = 1;
    pipelineDesc.layout.inlineConstantSize = sizeof(Constants);
    pipelineDesc.layout.inlineConstantStages = RHI::ShaderStageFlags::Vertex;
    pipelineDesc.layout.inlineConstantBinding = 15;
    auto* pipeline = resources.Keep(device.CreateGraphicsPipeline(pipelineDesc));

    bool uploaded=false;
    while(window.IsRunning())
    {
        window.PollEvents();
        if(!window.IsRunning()) break;
        if(!device.BeginFrame()) continue;
        RHI::ResourceScope frameResources(device);
        auto* commands=frameResources.Keep(device.AcquireCommandList());
        if(!uploaded)
        {
            (void)device.UpdateBuffer(*commands,vertexBuffer,0,vertices.data(),sizeof(vertices));
            const RHI::ResourceBarrierDesc ready{vertexBuffer,nullptr,
                RHI::ResourceState::CopyDestination,RHI::ResourceState::VertexBuffer,{}};
            commands->ResourceBarrier(&ready,1);
            uploaded=true;
        }
        auto* backBuffer=device.GetBackBuffer();
        const auto extent=backBuffer->GetDesc();
        RHI::RenderGraph graph;
        const auto target=graph.ImportTexture("backbuffer",backBuffer,
            RHI::ResourceState::Present,RHI::ResourceState::Present);
        const auto geometry=graph.ImportBuffer("quad",vertexBuffer,
            RHI::ResourceState::VertexBuffer,RHI::ResourceState::VertexBuffer);
        // 같은 출력에 쓰는 두 패스를 선언하면 그래프가 순서와 배리어를 만든다.
        for(uint32_t pass=0;pass<2;++pass)
        {
            graph.AddPass(pass==0?"clear-and-blue":"load-and-orange")
                .Read(geometry,RHI::ResourceState::VertexBuffer)
                .Write(target,RHI::ResourceState::RenderTarget)
                .SetExecute([&,pass](RHI::ICommandList* list)
                {
                    RHI::ColorAttachment color;
                    color.texture=backBuffer;
                    color.loadOp=pass==0?RHI::LoadOp::Clear:RHI::LoadOp::Load;
                    color.storeOp=RHI::StoreOp::Store;
                    color.clearColor[0] = 0.025f;
                    color.clearColor[1] = 0.035f;
                    color.clearColor[2] = 0.055f;
                    color.clearColor[3] = 1.0f;
                    list->BeginRendering({&color,1,nullptr});
                    list->BindGraphicsPipeline(pipeline);
                    list->BindVertexBuffer(0,vertexBuffer,0);
                    list->SetViewport({0,0,float(extent.width),float(extent.height)});
                    list->SetScissor({0,0,extent.width,extent.height});
                    list->SetInlineConstants(0,sizeof(Constants),&constants[pass]);
                    list->DrawInstanced(uint32_t(vertices.size()),1,0,0);
                    list->EndRendering();
                });
        }
        (void)graph.Compile();
        (void)graph.Execute(commands);
        (void)commands->Close();
        (void)device.Submit(&commands,1);
        (void)device.Present();
    }
    (void)device.WaitIdle();
}
