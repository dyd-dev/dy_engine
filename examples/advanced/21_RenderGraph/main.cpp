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
    pipelineDesc.raster = {RHI::FillMode::Solid, RHI::CullMode::None, RHI::FrontFace::CounterClockwise, 0, 0, 0};
    pipelineDesc.colorAttachments = &output;
    pipelineDesc.colorAttachmentCount = 1;
    pipelineDesc.layout = {nullptr, 0, sizeof(Constants), RHI::ShaderStageFlags::Vertex, 15};
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
                    list->SetViewport({0,0,float(extent.width),float(extent.height),0,1});
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
