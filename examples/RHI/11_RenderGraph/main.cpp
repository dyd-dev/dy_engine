#include "../../HostedExample.h"
#include "dyf/RHI.h"
#include "dyf/Platform/Window.h"
#include "compute.h"
#include "vertex.h"
#include "fragment.h"
#include "display_vertex.h"
#include "display_fragment.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <stdexcept>
#include <vector>

namespace RHI = dyf::RHI;
namespace Platform = dyf::Platform;

namespace
{
void Require(bool success, const char* operation)
{
    if(!success) throw std::runtime_error(operation);
}
}

int main()
{
    try
    {
        // 1. 창은 표시 대상만 제공한다. GPU 작업은 공개 RHI로 직접 구성한다.
        DY_EXAMPLE_WINDOW(window, 960, 640, "RHI / 11 RenderGraph");
        Require(window.GetHandle() != nullptr, "Window creation failed.");
        std::unique_ptr<RHI::IDevice> device(RHI::IDevice::Create({}));
        Require(device != nullptr, "Device creation failed.");
        if(!device->Supports(RHI::Feature::Rasterization))
        {
            std::fprintf(stderr, "Unsupported: rasterization.\n");
            return 77;
        }
        if(!device->Supports(RHI::Feature::Compute))
        {
            std::fprintf(stderr, "Unsupported: Compute.\n");
            return 77;
        }

        RHI::SwapchainDesc swapchain;
        swapchain.window = window.GetHandle();
        swapchain.format = RHI::DefaultSwapchainFormat;
        swapchain.presentMode = RHI::PresentMode::Fifo;
        swapchain.minimumImageCount = 2;
        Require(device->CreateSwapchain(swapchain), "Swapchain creation failed.");

        // 셰이더 조합과 입력 형식, 고정 기능 상태를 하나의 GPU 파이프라인으로 만든다.
        auto* vertexShader = device->CreateShader({RHI::ShaderStage::Vertex,
            ShaderData::vertexEntryPoint, ShaderData::vertex, ShaderData::vertexSize});
        auto* fragmentShader = device->CreateShader({RHI::ShaderStage::Fragment,
            ShaderData::fragmentEntryPoint, ShaderData::fragment, ShaderData::fragmentSize});
        Require(vertexShader && fragmentShader, "Shader creation failed.");
        RHI::ColorAttachmentDesc colorOutput;
        colorOutput.format = RHI::Format::R8G8B8A8_UNORM;
        colorOutput.writeMask = RHI::ColorWriteMask::All;
        RHI::GraphicsPipelineDesc pipelineDesc;
        pipelineDesc.vertexShader = vertexShader;
        pipelineDesc.fragmentShader = fragmentShader;
        pipelineDesc.topology = RHI::PrimitiveTopology::TriangleList;
        pipelineDesc.raster.fillMode = RHI::FillMode::Solid;
        pipelineDesc.raster.cullMode = RHI::CullMode::None;
        pipelineDesc.raster.frontFace = RHI::FrontFace::CounterClockwise;
        pipelineDesc.colorAttachments = &colorOutput;
        pipelineDesc.colorAttachmentCount = 1;
        const RHI::VertexBufferLayout input{0, 8 * sizeof(float), RHI::VertexStepMode::Vertex};
        const std::array<RHI::VertexAttribute, 2> attributes = {{
            {0,0,RHI::Format::R32G32B32A32_FLOAT,0}, {1,0,RHI::Format::R32G32B32A32_FLOAT,16}
        }};
        pipelineDesc.vertexBuffers = &input;
        pipelineDesc.vertexBufferCount = 1;
        pipelineDesc.vertexAttributes = attributes.data();
        pipelineDesc.vertexAttributeCount = uint32_t(attributes.size());
        auto* pipeline = device->CreateGraphicsPipeline(pipelineDesc);
        Require(pipeline != nullptr, "Graphics pipeline creation failed.");
        auto* computeShader = device->CreateShader({RHI::ShaderStage::Compute,
            ShaderData::computeEntryPoint, ShaderData::compute, ShaderData::computeSize});
        Require(computeShader != nullptr, "Compute shader creation failed.");
        const RHI::ResourceBindingLayout outputBinding{0, RHI::ResourceBindingType::ReadWriteStorageBuffer,
            1, RHI::ShaderStageFlags::Compute, {}};
        RHI::ComputePipelineDesc computeDesc;
        computeDesc.computeShader = computeShader;
        computeDesc.layout = {&outputBinding,1,16,RHI::ShaderStageFlags::Compute,15};
        // Dispatch의 인자는 그룹 수다. 그룹당 64개 스레드라는 셰이더 선언과 일치시킨다.
        computeDesc.threadGroupSize[0] = 64;
        computeDesc.threadGroupSize[1] = computeDesc.threadGroupSize[2] = 1;
        auto* computePipeline = device->CreateComputePipeline(computeDesc);
        Require(computePipeline != nullptr, "Compute pipeline creation failed.");
        RHI::BufferDesc generatedDesc;
        generatedDesc.size = 3 * input.stride;
        generatedDesc.stride = input.stride;
        generatedDesc.usage = RHI::BufferUsage::Storage | RHI::BufferUsage::Vertex;
        generatedDesc.initialState = RHI::ResourceState::UnorderedAccess;
        auto* generatedVertices = device->CreateBuffer(generatedDesc);
        Require(generatedVertices != nullptr, "Generated buffer creation failed.");
        RHI::ResourceBinding output;
        output.binding = 0;
        output.buffer = generatedVertices;
        output.size = generatedDesc.size;
        auto* computeSet = device->CreateResourceSet({computePipeline,&output,1});
        Require(computeSet != nullptr, "Compute bindings failed.");
        RHI::ResourceState generatedState = RHI::ResourceState::UnorderedAccess;
        const auto started = std::chrono::steady_clock::now();

        auto* displayVertex = device->CreateShader({RHI::ShaderStage::Vertex,
            ShaderData::display_vertexEntryPoint, ShaderData::display_vertex, ShaderData::display_vertexSize});
        auto* displayFragment = device->CreateShader({RHI::ShaderStage::Fragment,
            ShaderData::display_fragmentEntryPoint, ShaderData::display_fragment, ShaderData::display_fragmentSize});
        Require(displayVertex && displayFragment, "Display shaders failed.");
        RHI::SamplerDesc sampler;
        sampler.minFilter = sampler.magFilter = RHI::SamplerFilter::Linear;
        sampler.mipFilter = RHI::SamplerFilter::Nearest;
        sampler.addressU = sampler.addressV = sampler.addressW = RHI::SamplerAddressMode::ClampToEdge;
        sampler.mipLodBias = sampler.minLod = sampler.maxLod = 0;
        const std::array<RHI::ResourceBindingLayout, 2> displayBindings = {{
            {0, RHI::ResourceBindingType::SampledTexture, 1, RHI::ShaderStageFlags::Fragment, {}},
            {1, RHI::ResourceBindingType::StaticSampler, 1, RHI::ShaderStageFlags::Fragment, sampler}
        }};
        RHI::ColorAttachmentDesc displayOutput;
        displayOutput.format = swapchain.format;
        displayOutput.writeMask = RHI::ColorWriteMask::All;
        RHI::GraphicsPipelineDesc displayDesc;
        displayDesc.vertexShader = displayVertex;
        displayDesc.fragmentShader = displayFragment;
        displayDesc.topology = RHI::PrimitiveTopology::TriangleList;
        displayDesc.raster.fillMode = RHI::FillMode::Solid;
        displayDesc.raster.cullMode = RHI::CullMode::None;
        displayDesc.raster.frontFace = RHI::FrontFace::CounterClockwise;
        displayDesc.colorAttachments = &displayOutput;
        displayDesc.colorAttachmentCount = 1;
        displayDesc.layout.bindings = displayBindings.data();
        displayDesc.layout.bindingCount = uint32_t(displayBindings.size());
        auto* displayPipeline = device->CreateGraphicsPipeline(displayDesc);
        Require(displayPipeline != nullptr, "Display pipeline creation failed.");
        RHI::TextureDesc imageDesc;
        imageDesc.width = imageDesc.height = 512;
        imageDesc.format = RHI::Format::R8G8B8A8_UNORM;
        imageDesc.usage = RHI::TextureUsage::RenderTarget | RHI::TextureUsage::ShaderResource;
        auto* offscreen = device->CreateTexture(imageDesc);
        Require(offscreen != nullptr, "Offscreen texture creation failed.");
        RHI::ResourceBinding image;
        image.binding = 0;
        image.texture = offscreen;
        auto* displaySet = device->CreateResourceSet({displayPipeline, &image, 1});
        Require(displaySet != nullptr, "Display bindings failed.");
        RHI::ResourceState offscreenState = RHI::ResourceState::Undefined;

        while(dy_examples::ExampleRunning(window))
        {
            Platform::Window::PollEvents();
            if(!dy_examples::BeginExampleFrame(window)) break;
            if(!dy_examples::ExampleRunning(window)) break;
            if(!device->BeginFrame())
            {
                Require(!device->IsLost(), "Device lost.");
                Platform::Window::WaitEvents();
                continue;
            }
            auto* target = device->GetBackBuffer();
            Require(target != nullptr, "Backbuffer unavailable.");
            const auto extent = target->GetDesc();
            auto* commands = device->AcquireCommandList();
            Require(commands != nullptr, "Command list creation failed.");

            const float parameters[4] = {std::chrono::duration<float>(std::chrono::steady_clock::now()-started).count(),0,0,0};

            // GPU Pipeline 생성은 위에서 끝났다. 아래 그래프의 노드는 셰이더 단계가 아니라 작업 패스다.
            // 경계 상태와 Read/Write는 배리어를 결정하고 SetPipeline은 패스의 Pipeline을 선택한다.
            RHI::RenderGraph graph;
            const auto vertices = graph.ImportBuffer("GeneratedVertices", generatedVertices,
                generatedState, RHI::ResourceState::VertexBuffer);
            const auto image = graph.ImportTexture("Offscreen", offscreen,
                offscreenState, RHI::ResourceState::ShaderResource);
            const auto screen = graph.ImportTexture("Backbuffer", target,
                RHI::ResourceState::Present, RHI::ResourceState::Present);

            graph.AddPass("GenerateVertices").Write(vertices,RHI::ResourceState::UnorderedAccess)
                .SetPipeline(computePipeline)
                .SetExecute([&](RHI::ICommandList* list) {
                    list->BindResourceSet(computeSet);
                    list->SetInlineConstants(0,sizeof(parameters),parameters);
                    list->Dispatch(1,1,1);
                });
            graph.AddPass("Offscreen").Read(vertices,RHI::ResourceState::VertexBuffer)
                .Write(image,RHI::ResourceState::RenderTarget)
                .SetPipeline(pipeline)
                .SetExecute([&](RHI::ICommandList* list) {
                    RHI::ColorAttachment color;
                    color.texture = offscreen;
                    color.loadOp = RHI::LoadOp::Clear;
                    color.storeOp = RHI::StoreOp::Store;
                    color.clearColor[0]=0.08f; color.clearColor[1]=0.13f; color.clearColor[2]=0.22f; color.clearColor[3]=1;
                    list->BeginRendering({&color,1,nullptr});
                    list->BindVertexBuffer(0,generatedVertices,0);
                    list->SetViewport({0,0,512,512,0,1});
                    list->SetScissor({0,0,512,512});
                    list->DrawInstanced(3,1,0,0);
                    list->EndRendering();
                });
            graph.AddPass("PresentImage").Read(image,RHI::ResourceState::ShaderResource)
                .Write(screen,RHI::ResourceState::RenderTarget)
                .SetPipeline(displayPipeline)
                .SetExecute([&](RHI::ICommandList* list) {
                    RHI::ColorAttachment color;
                    color.texture = target;
                    color.loadOp = RHI::LoadOp::Discard;
                    color.storeOp = RHI::StoreOp::Store;
                    list->BeginRendering({&color,1,nullptr});
                    list->BindResourceSet(displaySet);
                    list->SetViewport({0,0,float(extent.width),float(extent.height),0,1});
                    list->SetScissor({0,0,extent.width,extent.height});
                    list->DrawInstanced(3,1,0,0);
                    list->EndRendering();
                });
            Require(graph.Compile(), "RenderGraph compilation failed.");
            Require(graph.Execute(commands), "RenderGraph recording failed.");
            // Execute는 제출하지 않는다. 아래 Close / Submit / Present를 호출자가 수행한다.
            generatedState = RHI::ResourceState::VertexBuffer;
            offscreenState = RHI::ResourceState::ShaderResource;

            // GPU 제출과 화면 표시는 별개다. Submit은 프레임을 끝내지 않는다.
            Require(commands->Close(), "Command recording failed.");
            Require(device->Submit(&commands, 1), "Submission failed.");
            Require(device->Present(), "Presentation failed.");
            // 제출된 네이티브 명령은 GPU 완료까지 참조 자원을 보유한다.
            device->DestroyCommandList(commands);
        }

        Require(device->WaitIdle(), "GPU wait failed.");
        device->DestroyResourceSet(displaySet);
        device->DestroyTexture(offscreen);
        device->DestroyPipeline(displayPipeline);
        device->DestroyShader(displayFragment);
        device->DestroyShader(displayVertex);
        device->DestroyResourceSet(computeSet);
        device->DestroyBuffer(generatedVertices);
        device->DestroyPipeline(computePipeline);
        device->DestroyShader(computeShader);
        device->DestroyPipeline(pipeline);
        device->DestroyShader(fragmentShader);
        device->DestroyShader(vertexShader);
        return 0;
    }
    catch(const std::exception& error)
    {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
