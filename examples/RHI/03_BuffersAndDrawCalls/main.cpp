#include "dyf/RHI.h"
#include "dyf/Platform/Window.h"
#include "vertex.h"
#include "fragment.h"

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
        Platform::Window window(960, 640, "RHI / 03 Buffers and Draw Calls");
        Require(window.GetHandle() != nullptr, "Window creation failed.");
        std::unique_ptr<RHI::IDevice> device(RHI::IDevice::Create({}));
        Require(device != nullptr, "Device creation failed.");
        if(!device->Supports(RHI::Feature::Rasterization))
        {
            std::fprintf(stderr, "Unsupported: rasterization.\n");
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
        colorOutput.format = swapchain.format;
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
        struct Vertex { float x, y, r, g, b, a; };
        struct Instance { float x, y, scaleX, scaleY; };
        const std::array<RHI::VertexBufferLayout, 2> inputs = {{
            {0, sizeof(Vertex), RHI::VertexStepMode::Vertex},
            {1, sizeof(Instance), RHI::VertexStepMode::Instance}
        }};
        const std::array<RHI::VertexAttribute, 3> attributes = {{
            {0, 0, RHI::Format::R32G32_FLOAT, uint32_t(offsetof(Vertex, x))},
            {1, 0, RHI::Format::R32G32B32A32_FLOAT, uint32_t(offsetof(Vertex, r))},
            {2, 1, RHI::Format::R32G32B32A32_FLOAT, 0}
        }};
        pipelineDesc.vertexBuffers = inputs.data();
        pipelineDesc.vertexBufferCount = uint32_t(inputs.size());
        pipelineDesc.vertexAttributes = attributes.data();
        pipelineDesc.vertexAttributeCount = uint32_t(attributes.size());
        auto* pipeline = device->CreateGraphicsPipeline(pipelineDesc);
        Require(pipeline != nullptr, "Graphics pipeline creation failed.");
        const std::array<Vertex, 7> vertices = {{
            {-0.4f,-0.4f,1,0.3f,0.2f,1},{0.4f,-0.4f,0.2f,1,0.3f,1},
            {0.4f,0.4f,0.2f,0.4f,1,1},{-0.4f,0.4f,1,0.8f,0.2f,1},
            {-0.4f,-0.4f,1,0.3f,0.2f,1},{0.4f,-0.4f,0.2f,1,0.3f,1},
            {0,0.4f,0.2f,0.4f,1,1}
        }};
        const std::array<uint32_t, 6> indices = {{0,1,2,2,3,0}};
        const std::array<Instance, 5> instances = {{
            {-0.5f,0.4f,0.8f,0.8f}, {0.5f,0.4f,0.8f,0.8f},
            {-0.55f,-0.45f,0.5f,0.5f}, {0,-0.45f,0.5f,0.5f}, {0.55f,-0.45f,0.5f,0.5f}
        }};
        RHI::BufferDesc vertexDesc;
        vertexDesc.size = sizeof(vertices);
        vertexDesc.stride = sizeof(Vertex);
        vertexDesc.usage = RHI::BufferUsage::Vertex;
        vertexDesc.initialState = RHI::ResourceState::CopyDestination;
        auto* vertexBuffer = device->CreateBuffer(vertexDesc);
        RHI::BufferDesc indexDesc;
        indexDesc.size = sizeof(indices);
        indexDesc.stride = sizeof(uint32_t);
        indexDesc.usage = RHI::BufferUsage::Index;
        indexDesc.initialState = RHI::ResourceState::CopyDestination;
        auto* indexBuffer = device->CreateBuffer(indexDesc);
        Require(vertexBuffer && indexBuffer, "Buffer creation failed.");
        auto* upload = device->AcquireCommandList();
        Require(upload != nullptr, "Upload command list creation failed.");
        Require(device->UpdateBuffer(*upload, vertexBuffer, 0, vertices.data(), sizeof(vertices)), "Vertex upload failed.");
        Require(device->UpdateBuffer(*upload, indexBuffer, 0, indices.data(), sizeof(indices)), "Index upload failed.");
        RHI::BufferDesc instanceDesc;
        instanceDesc.size = sizeof(instances);
        instanceDesc.stride = sizeof(Instance);
        instanceDesc.usage = RHI::BufferUsage::Vertex;
        instanceDesc.initialState = RHI::ResourceState::CopyDestination;
        auto* instanceBuffer = device->CreateBuffer(instanceDesc);
        Require(instanceBuffer != nullptr, "Instance buffer creation failed.");
        Require(device->UpdateBuffer(*upload, instanceBuffer, 0, instances.data(), sizeof(instances)), "Instance upload failed.");
        const std::array<RHI::ResourceBarrierDesc, 3> ready = {{
            {vertexBuffer, nullptr, RHI::ResourceState::CopyDestination, RHI::ResourceState::VertexBuffer, {}},
            {indexBuffer, nullptr, RHI::ResourceState::CopyDestination, RHI::ResourceState::IndexBuffer, {}},
            {instanceBuffer, nullptr, RHI::ResourceState::CopyDestination, RHI::ResourceState::VertexBuffer, {}}
        }};
        upload->ResourceBarrier(ready.data(), uint32_t(ready.size()));
        Require(upload->Close(), "Upload recording failed.");
        Require(device->Submit(&upload, 1), "Upload submission failed.");
        device->DestroyCommandList(upload);
        // 같은 장치 큐에 뒤이어 제출하는 Draw는 이 업로드 뒤에 실행된다.

        while(window.IsRunning())
        {
            Platform::Window::PollEvents();
            if(!window.IsRunning()) break;
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

            const RHI::ResourceBarrierDesc begin{nullptr, target,
                RHI::ResourceState::Present, RHI::ResourceState::RenderTarget, {}};
            commands->ResourceBarrier(&begin, 1);
            RHI::ColorAttachment color;
            color.texture = target;
            color.loadOp = RHI::LoadOp::Clear;
            color.storeOp = RHI::StoreOp::Store;
            color.clearColor[0] = 0.035f;
            color.clearColor[1] = 0.05f;
            color.clearColor[2] = 0.08f;
            color.clearColor[3] = 1;
            commands->BeginRendering({&color, 1, nullptr});
            commands->SetViewport({0, 0, float(extent.width), float(extent.height), 0, 1});
            commands->SetScissor({0, 0, extent.width, extent.height});
            commands->BindGraphicsPipeline(pipeline);
            commands->BindVertexBuffer(0, vertexBuffer, 0);
            commands->BindVertexBuffer(1, instanceBuffer, 0);
            commands->BindIndexBuffer(indexBuffer, RHI::Format::R32_UINT, 0);
            // 위 왼쪽: 4개 정점을 인덱스 6개로 두 삼각형에 재사용한다.
            commands->DrawIndexedInstanced(6, 1, 0, 0, 0);
            // 위 오른쪽: VB의 정점 4번부터 3개를 순서대로 사용한다.
            commands->DrawInstanced(3, 1, 4, 1);
            // 아래: 같은 사각형을 3개 인스턴스의 transform으로 실행한다.
            commands->DrawIndexedInstanced(6, 3, 0, 0, 2);
            commands->EndRendering();
            const RHI::ResourceBarrierDesc end{nullptr, target,
                RHI::ResourceState::RenderTarget, RHI::ResourceState::Present, {}};
            commands->ResourceBarrier(&end, 1);

            // GPU 제출과 화면 표시는 별개다. Submit은 프레임을 끝내지 않는다.
            Require(commands->Close(), "Command recording failed.");
            Require(device->Submit(&commands, 1), "Submission failed.");
            Require(device->Present(), "Presentation failed.");
            // 제출된 네이티브 명령은 GPU 완료까지 참조 자원을 보유한다.
            device->DestroyCommandList(commands);
        }

        Require(device->WaitIdle(), "GPU wait failed.");
        device->DestroyBuffer(instanceBuffer);
        device->DestroyBuffer(indexBuffer);
        device->DestroyBuffer(vertexBuffer);
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
