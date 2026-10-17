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
        Platform::Window window(960, 640, "RHI / 05 Textures and Samplers");
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
        swapchain.format = RHI::Format::B8G8R8A8_UNORM;
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
        struct Vertex { float x, y, u, v; };
        const RHI::VertexBufferLayout input{0, sizeof(Vertex), RHI::VertexStepMode::Vertex};
        const std::array<RHI::VertexAttribute, 2> attributes = {{
            {0, 0, RHI::Format::R32G32_FLOAT, 0}, {1, 0, RHI::Format::R32G32_FLOAT, 8}
        }};
        pipelineDesc.vertexBuffers = &input;
        pipelineDesc.vertexBufferCount = 1;
        pipelineDesc.vertexAttributes = attributes.data();
        pipelineDesc.vertexAttributeCount = uint32_t(attributes.size());
        pipelineDesc.layout.inlineConstantSize = 16;
        pipelineDesc.layout.inlineConstantStages = RHI::ShaderStageFlags::Vertex;
        pipelineDesc.layout.inlineConstantBinding = 15;
        std::array<RHI::ResourceBindingLayout, 2> bindings = {{
            {0, RHI::ResourceBindingType::SampledTexture, 1, RHI::ShaderStageFlags::Fragment, {}},
            {1, RHI::ResourceBindingType::StaticSampler, 1, RHI::ShaderStageFlags::Fragment, {}}
        }};
        pipelineDesc.layout.bindings = bindings.data();
        pipelineDesc.layout.bindingCount = uint32_t(bindings.size());
        std::array<RHI::PipelineHandle, 2> pipelines{};
        for(uint32_t i = 0; i < pipelines.size(); ++i)
        {
            RHI::SamplerDesc sampler;
            sampler.minFilter = sampler.magFilter = i == 0 ? RHI::SamplerFilter::Nearest : RHI::SamplerFilter::Linear;
            sampler.mipFilter = RHI::SamplerFilter::Nearest;
            sampler.addressU = sampler.addressV = sampler.addressW = RHI::SamplerAddressMode::ClampToEdge;
            sampler.mipLodBias = sampler.minLod = sampler.maxLod = 0;
            bindings[1].staticSampler = sampler;
            pipelines[i] = device->CreateGraphicsPipeline(pipelineDesc);
            Require(pipelines[i] != nullptr, "Textured pipeline creation failed.");
        }
        const std::array<Vertex, 4> vertices = {{{-0.65f,-0.65f,0,1},{0.65f,-0.65f,1,1},
            {0.65f,0.65f,1,0},{-0.65f,0.65f,0,0}}};
        const std::array<uint32_t, 6> indices = {{0,1,2,2,3,0}};
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

        // 외부 이미지 대신 화소와 rowPitch / slicePitch의 관계를 직접 보여준다.
        constexpr uint32_t textureWidth = 8, textureHeight = 8, rowPitch = textureWidth * 4;
        std::array<uint8_t, rowPitch * textureHeight> pixels{};
        for(uint32_t y = 0; y < textureHeight; ++y)
            for(uint32_t x = 0; x < textureWidth; ++x)
            {
                const size_t pixel = y * rowPitch + x * 4;
                const bool bright = ((x + y) & 1u) != 0;
                pixels[pixel] = bright ? 245 : 25;
                pixels[pixel + 1] = bright ? 190 : 75;
                pixels[pixel + 2] = bright ? 45 : 210;
                pixels[pixel + 3] = 255;
            }
        RHI::TextureDesc textureDesc;
        textureDesc.width = textureWidth;
        textureDesc.height = textureHeight;
        textureDesc.format = RHI::Format::R8G8B8A8_UNORM;
        textureDesc.usage = RHI::TextureUsage::ShaderResource;
        auto* texture = device->CreateTexture(textureDesc);
        Require(texture != nullptr, "Texture creation failed.");
        const RHI::ResourceBarrierDesc writable{nullptr, texture,
            RHI::ResourceState::Undefined, RHI::ResourceState::CopyDestination, {}};
        upload->ResourceBarrier(&writable, 1);
        Require(device->UpdateTexture(*upload, texture, 0, 0, pixels.data(),
            uint32_t(pixels.size()), rowPitch, rowPitch * textureHeight), "Texture upload failed.");
        const std::array<RHI::ResourceBarrierDesc, 3> ready = {{
            {vertexBuffer, nullptr, RHI::ResourceState::CopyDestination, RHI::ResourceState::VertexBuffer, {}},
            {indexBuffer, nullptr, RHI::ResourceState::CopyDestination, RHI::ResourceState::IndexBuffer, {}},
            {nullptr, texture, RHI::ResourceState::CopyDestination, RHI::ResourceState::ShaderResource, {}}
        }};
        upload->ResourceBarrier(ready.data(), uint32_t(ready.size()));
        Require(upload->Close(), "Upload recording failed.");
        Require(device->Submit(&upload, 1), "Upload submission failed.");
        device->DestroyCommandList(upload);
        // 같은 장치 큐에 뒤이어 제출하는 Draw는 이 업로드 뒤에 실행된다.

        RHI::ResourceBinding image;
        image.binding = 0;
        image.texture = texture;
        std::array<RHI::ResourceSetHandle, 2> sets{};
        for(uint32_t i = 0; i < sets.size(); ++i)
        {
            sets[i] = device->CreateResourceSet({pipelines[i], &image, 1});
            Require(sets[i] != nullptr, "Texture resource set creation failed.");
        }

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
            for(uint32_t i = 0; i < pipelines.size(); ++i)
            {
                commands->BindGraphicsPipeline(pipelines[i]);
                // ResourceSet은 현재 파이프라인용이다. 파이프라인 변경 뒤 다시 연결한다.
                commands->BindResourceSet(sets[i]);
                commands->BindVertexBuffer(0, vertexBuffer, 0);
                commands->BindIndexBuffer(indexBuffer, RHI::Format::R32_UINT, 0);
                const float transform[4] = {i == 0 ? -0.5f : 0.5f, 0, 0.65f, 0.85f};
                commands->SetInlineConstants(0, sizeof(transform), transform);
                commands->DrawIndexedInstanced(6, 1, 0, 0, 0);
            }
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
        for(auto* set : sets) device->DestroyResourceSet(set);
        for(auto* pipeline : pipelines) device->DestroyPipeline(pipeline);
        device->DestroyTexture(texture);
        device->DestroyBuffer(indexBuffer);
        device->DestroyBuffer(vertexBuffer);
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
