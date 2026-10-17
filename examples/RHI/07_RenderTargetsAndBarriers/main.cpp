#include "dyf/RHI.h"
#include "dyf/Platform/Window.h"
#include "scene_vertex.h"
#include "scene_fragment.h"
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
        Platform::Window window(960, 640, "RHI / 07 Render Targets and Barriers");
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
        auto* sceneVertex = device->CreateShader({RHI::ShaderStage::Vertex,
            ShaderData::scene_vertexEntryPoint, ShaderData::scene_vertex, ShaderData::scene_vertexSize});
        auto* sceneFragment = device->CreateShader({RHI::ShaderStage::Fragment,
            ShaderData::scene_fragmentEntryPoint, ShaderData::scene_fragment, ShaderData::scene_fragmentSize});
        Require(sceneVertex && sceneFragment, "Shader creation failed.");
        RHI::ColorAttachmentDesc colorOutput;
        colorOutput.format = swapchain.format;
        colorOutput.writeMask = RHI::ColorWriteMask::All;
        RHI::GraphicsPipelineDesc pipelineDesc;
        pipelineDesc.vertexShader = sceneVertex;
        pipelineDesc.fragmentShader = sceneFragment;
        pipelineDesc.topology = RHI::PrimitiveTopology::TriangleList;
        pipelineDesc.raster.fillMode = RHI::FillMode::Solid;
        pipelineDesc.raster.cullMode = RHI::CullMode::None;
        pipelineDesc.raster.frontFace = RHI::FrontFace::CounterClockwise;
        pipelineDesc.colorAttachments = &colorOutput;
        pipelineDesc.colorAttachmentCount = 1;
        colorOutput.format = RHI::Format::R8G8B8A8_UNORM;
        auto* scenePipeline = device->CreateGraphicsPipeline(pipelineDesc);
        Require(scenePipeline != nullptr, "Scene pipeline creation failed.");
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

            // 1차 패스는 텍스처를 출력으로, 2차 패스는 같은 텍스처를 입력으로 사용한다.
            const std::array<RHI::ResourceBarrierDesc, 2> begin = {{
                {nullptr, offscreen, offscreenState, RHI::ResourceState::RenderTarget, {}},
                {nullptr, target, RHI::ResourceState::Present, RHI::ResourceState::RenderTarget, {}}
            }};
            commands->ResourceBarrier(begin.data(), uint32_t(begin.size()));
            RHI::ColorAttachment sceneColor;
            sceneColor.texture = offscreen;
            sceneColor.loadOp = RHI::LoadOp::Clear;
            sceneColor.storeOp = RHI::StoreOp::Store;
            sceneColor.clearColor[0] = 0.08f; sceneColor.clearColor[1] = 0.13f; sceneColor.clearColor[2] = 0.22f; sceneColor.clearColor[3] = 1;
            commands->BeginRendering({&sceneColor, 1, nullptr});
            commands->BindGraphicsPipeline(scenePipeline);
            commands->SetViewport({0,0,512,512,0,1});
            commands->SetScissor({0,0,512,512});
            commands->DrawInstanced(3,1,0,0);
            commands->EndRendering();
            const RHI::ResourceBarrierDesc sampled{nullptr, offscreen,
                RHI::ResourceState::RenderTarget, RHI::ResourceState::ShaderResource, {}};
            commands->ResourceBarrier(&sampled,1);
            RHI::ColorAttachment screenColor;
            screenColor.texture = target;
            screenColor.loadOp = RHI::LoadOp::Discard;
            screenColor.storeOp = RHI::StoreOp::Store;
            commands->BeginRendering({&screenColor,1,nullptr});
            commands->BindGraphicsPipeline(displayPipeline);
            commands->BindResourceSet(displaySet);
            commands->SetViewport({0,0,float(extent.width),float(extent.height),0,1});
            commands->SetScissor({0,0,extent.width,extent.height});
            // fullscreen triangle은 VB 없이 정점 번호로 생성한다.
            commands->DrawInstanced(3,1,0,0);
            commands->EndRendering();
            const RHI::ResourceBarrierDesc end{nullptr, target,
                RHI::ResourceState::RenderTarget, RHI::ResourceState::Present, {}};
            commands->ResourceBarrier(&end, 1);
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
        device->DestroyPipeline(scenePipeline);
        device->DestroyShader(displayFragment);
        device->DestroyShader(displayVertex);
        device->DestroyShader(sceneFragment);
        device->DestroyShader(sceneVertex);
        return 0;
    }
    catch(const std::exception& error)
    {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
