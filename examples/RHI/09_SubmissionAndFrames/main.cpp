#include "../../HostedExample.h"
#include "dyf/RHI.h"
#include "dyf/Platform/Window.h"
#include "vertex.h"
#include "fragment.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <stdexcept>

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
        DY_EXAMPLE_WINDOW(window, 960, 640, "RHI / 09 Submission and Frames");
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

        auto* pipeline = device->CreateGraphicsPipeline(pipelineDesc);
        Require(pipeline != nullptr, "Graphics pipeline creation failed.");

        // CPU가 기록할 슬롯마다 명령 목록과 해당 제출의 완료 토큰을 보관한다.
        // Fence는 CPU 재사용 시점을 알려준다. Present는 완료 대기 함수가 아니다.
        std::array<RHI::ICommandList*, 2> lists{};
        std::array<RHI::FenceHandle, 2> fences{};
        for(auto& list : lists)
        {
            list = device->AcquireCommandList();
            Require(list != nullptr, "Command list creation failed.");
        }
        uint64_t frame = 0;

        while(dy_examples::ExampleRunning(window))
        {
            Platform::Window::PollEvents();
            if(!dy_examples::BeginExampleFrame(window)) break;
            if(!dy_examples::ExampleRunning(window)) break;
            const uint32_t slot = uint32_t(frame % lists.size());
            if(fences[slot])
            {
                // 같은 슬롯의 명령 목록을 다시 기록하기 전에 GPU 완료를 기다린다.
                Require(device->Wait(fences[slot], UINT64_MAX), "Submission wait failed.");
                fences[slot] = {};
            }
            // 완료한 슬롯만 Reset한다. 제출 후에도 목록은 다음 재사용까지 보관한다.
            Require(device->ResetCommandList(lists[slot]), "Command list reset failed.");
            if(!device->BeginFrame())
            {
                Require(!device->IsLost(), "Device lost.");
                Platform::Window::WaitEvents();
                continue;
            }
            auto* target = device->GetBackBuffer();
            Require(target != nullptr, "Backbuffer unavailable.");
            const auto extent = target->GetDesc();
            auto* commands = lists[slot];

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
            commands->DrawInstanced(3, 1, 0, 0);
            commands->EndRendering();
            const RHI::ResourceBarrierDesc end{nullptr, target,
                RHI::ResourceState::RenderTarget, RHI::ResourceState::Present, {}};
            commands->ResourceBarrier(&end, 1);

            Require(commands->Close(), "Command recording failed.");
            RHI::SubmitDesc submission;
            submission.commandLists = &commands;
            submission.commandListCount = 1;
            Require(device->Submit(submission, fences[slot]), "Submission failed.");

            Require(device->Present(), "Presentation failed.");
            // 다음 프레임은 다른 슬롯을 사용한다. 슬롯 수와 Swapchain 이미지 번호는 별개다.
            ++frame;
        }

        Require(device->WaitIdle(), "GPU wait failed.");
        for(auto* list : lists) device->DestroyCommandList(list);
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
