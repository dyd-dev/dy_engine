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
        Platform::Window window(960, 640, "RHI / 04 Constants and Bindings");
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
        const RHI::VertexBufferLayout vertexInput{0, sizeof(Vertex), RHI::VertexStepMode::Vertex};
        const std::array<RHI::VertexAttribute, 2> attributes = {{
            {0, 0, RHI::Format::R32G32_FLOAT, uint32_t(offsetof(Vertex, x))},
            {1, 0, RHI::Format::R32G32B32A32_FLOAT, uint32_t(offsetof(Vertex, r))}
        }};
        pipelineDesc.vertexBuffers = &vertexInput;
        pipelineDesc.vertexBufferCount = 1;
        pipelineDesc.vertexAttributes = attributes.data();
        pipelineDesc.vertexAttributeCount = uint32_t(attributes.size());
        // 작은 Draw별 값은 인라인 상수, 여러 Draw가 공유하는 값은 상수 버퍼로 전달한다.
        RHI::ResourceBindingLayout globalsLayout;
        globalsLayout.binding = 0;
        globalsLayout.type = RHI::ResourceBindingType::ConstantBuffer;
        globalsLayout.count = 1;
        globalsLayout.stages = RHI::ShaderStageFlags::Fragment;
        pipelineDesc.layout.bindings = &globalsLayout;
        pipelineDesc.layout.bindingCount = 1;
        pipelineDesc.layout.inlineConstantSize = 16;
        pipelineDesc.layout.inlineConstantStages = RHI::ShaderStageFlags::Vertex;
        pipelineDesc.layout.inlineConstantBinding = 15;
        auto* pipeline = device->CreateGraphicsPipeline(pipelineDesc);
        Require(pipeline != nullptr, "Graphics pipeline creation failed.");
        const std::array<Vertex, 4> vertices = {{
            {-0.65f, -0.65f, 1, 0.25f, 0.15f, 1},
            { 0.65f, -0.65f, 0.15f, 1, 0.3f, 1},
            { 0.65f,  0.65f, 0.15f, 0.4f, 1, 1},
            {-0.65f,  0.65f, 1, 0.8f, 0.15f, 1}
        }};
        const std::array<uint32_t, 6> indices = {{0, 1, 2, 2, 3, 0}};
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

        const std::array<float, 4> leftTint = {{1, 0.4f, 0.4f, 1}};
        const std::array<float, 4> rightTint = {{0.4f, 0.8f, 1, 1}};
        const uint64_t alignment = device->GetLimit(RHI::Limit::UniformBufferOffsetAlignment);
        Require(alignment != 0 && sizeof(leftTint) <= device->GetLimit(RHI::Limit::UniformBufferBytes),
            "Uniform buffer limits cannot represent this example.");
        const uint64_t slotStride = ((sizeof(leftTint) + alignment - 1) / alignment) * alignment;
        Require(slotStride <= UINT32_MAX - sizeof(rightTint), "Uniform buffer size exceeds uint32_t.");
        std::vector<uint8_t> globals(size_t(slotStride + sizeof(rightTint)), 0);
        std::memcpy(globals.data(), leftTint.data(), sizeof(leftTint));
        std::memcpy(globals.data() + slotStride, rightTint.data(), sizeof(rightTint));
        RHI::BufferDesc uniformDesc;
        uniformDesc.size = uint32_t(globals.size());
        uniformDesc.usage = RHI::BufferUsage::Constant;
        uniformDesc.initialState = RHI::ResourceState::CopyDestination;
        auto* uniformBuffer = device->CreateBuffer(uniformDesc);
        Require(uniformBuffer != nullptr, "Uniform buffer creation failed.");
        Require(device->UpdateBuffer(*upload, uniformBuffer, 0, globals.data(), uint32_t(globals.size())), "Uniform upload failed.");
        const std::array<RHI::ResourceBarrierDesc, 3> ready = {{
            {vertexBuffer, nullptr, RHI::ResourceState::CopyDestination, RHI::ResourceState::VertexBuffer, {}},
            {indexBuffer, nullptr, RHI::ResourceState::CopyDestination, RHI::ResourceState::IndexBuffer, {}},
            {uniformBuffer, nullptr, RHI::ResourceState::CopyDestination, RHI::ResourceState::ConstantBuffer, {}}
        }};
        upload->ResourceBarrier(ready.data(), uint32_t(ready.size()));
        Require(upload->Close(), "Upload recording failed.");
        Require(device->Submit(&upload, 1), "Upload submission failed.");
        device->DestroyCommandList(upload);
        // 같은 장치 큐에 뒤이어 제출하는 Draw는 이 업로드 뒤에 실행된다.

        RHI::ResourceBinding binding;
        binding.binding = 0;
        binding.buffer = uniformBuffer;
        binding.size = sizeof(leftTint);
        auto* leftSet = device->CreateResourceSet({pipeline, &binding, 1});
        binding.offset = uint32_t(slotStride);
        auto* rightSet = device->CreateResourceSet({pipeline, &binding, 1});
        Require(leftSet && rightSet, "Resource set creation failed.");

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
            commands->BindIndexBuffer(indexBuffer, RHI::Format::R32_UINT, 0);
            const float leftTransform[4] = {-0.5f, 0, 0.65f, 0.85f};
            commands->BindResourceSet(leftSet);
            commands->SetInlineConstants(0, sizeof(leftTransform), leftTransform);
            commands->DrawIndexedInstanced(6, 1, 0, 0, 0);
            const float rightTransform[4] = {0.5f, 0, 0.65f, 0.85f};
            commands->BindResourceSet(rightSet);
            commands->SetInlineConstants(0, sizeof(rightTransform), rightTransform);
            commands->DrawIndexedInstanced(6, 1, 0, 0, 0);
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
        device->DestroyResourceSet(rightSet);
        device->DestroyResourceSet(leftSet);
        device->DestroyBuffer(uniformBuffer);
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
