#include "dyf/RHI.h"
#include "dyf/Platform/Window.h"
#include "scene_vertex.h"
#include "scene_fragment.h"
#include "display_vertex.h"
#include "display_fragment.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <memory>
#include <stdexcept>
#include <thread>

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
        Platform::Window window(960, 640, "RHI / 10 Parallel Recording");
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

        // 1. Pipeline과 Shader는 메인 스레드에서 만들고 기록 중에는 공유해서 읽는다.
        auto* sceneVertex = device->CreateShader({RHI::ShaderStage::Vertex,
            ShaderData::scene_vertexEntryPoint, ShaderData::scene_vertex, ShaderData::scene_vertexSize});
        auto* sceneFragment = device->CreateShader({RHI::ShaderStage::Fragment,
            ShaderData::scene_fragmentEntryPoint, ShaderData::scene_fragment, ShaderData::scene_fragmentSize});
        auto* displayVertex = device->CreateShader({RHI::ShaderStage::Vertex,
            ShaderData::display_vertexEntryPoint, ShaderData::display_vertex, ShaderData::display_vertexSize});
        auto* displayFragment = device->CreateShader({RHI::ShaderStage::Fragment,
            ShaderData::display_fragmentEntryPoint, ShaderData::display_fragment, ShaderData::display_fragmentSize});
        Require(sceneVertex && sceneFragment && displayVertex && displayFragment, "Shader creation failed.");

        RHI::ColorAttachmentDesc sceneOutput;
        sceneOutput.format = RHI::Format::R8G8B8A8_UNORM;
        sceneOutput.writeMask = RHI::ColorWriteMask::All;
        RHI::GraphicsPipelineDesc sceneDesc;
        sceneDesc.vertexShader = sceneVertex;
        sceneDesc.fragmentShader = sceneFragment;
        sceneDesc.topology = RHI::PrimitiveTopology::TriangleList;
        sceneDesc.raster.fillMode = RHI::FillMode::Solid;
        sceneDesc.raster.cullMode = RHI::CullMode::None;
        sceneDesc.raster.frontFace = RHI::FrontFace::CounterClockwise;
        sceneDesc.colorAttachments = &sceneOutput;
        sceneDesc.colorAttachmentCount = 1;
        sceneDesc.layout = {nullptr, 0, 16, RHI::ShaderStageFlags::Vertex, 15};
        auto* scenePipeline = device->CreateGraphicsPipeline(sceneDesc);
        Require(scenePipeline != nullptr, "Scene pipeline creation failed.");

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
        displayDesc.layout = {displayBindings.data(), uint32_t(displayBindings.size()),
            16, RHI::ShaderStageFlags::Vertex, 15};
        auto* displayPipeline = device->CreateGraphicsPipeline(displayDesc);
        Require(displayPipeline != nullptr, "Display pipeline creation failed.");

        // 2. 프레임 슬롯마다 출력 텍스처 2개, 작업 목록 2개, 합성 목록 1개를 보관한다.
        // 동시에 진행하는 프레임이 같은 슬롯을 덮어쓰지 않도록 완료 Fence를 연결한다.
        constexpr uint32_t workerCount = 2;
        struct FrameSlot
        {
            std::array<RHI::TextureHandle, workerCount> images{};
            std::array<RHI::ResourceSetHandle, workerCount> displaySets{};
            std::array<RHI::ICommandList*, workerCount + 1> lists{};
            RHI::FenceHandle completion;
            RHI::ResourceState imageState = RHI::ResourceState::Undefined;
        };
        std::array<FrameSlot, 2> frames{};
        RHI::TextureDesc imageDesc;
        imageDesc.width = imageDesc.height = 512;
        imageDesc.format = sceneOutput.format;
        imageDesc.usage = RHI::TextureUsage::RenderTarget | RHI::TextureUsage::ShaderResource;
        for(auto& frame : frames)
        {
            for(uint32_t i = 0; i < workerCount; ++i)
            {
                frame.images[i] = device->CreateTexture(imageDesc);
                Require(frame.images[i] != nullptr, "Offscreen texture creation failed.");
                RHI::ResourceBinding image;
                image.binding = 0;
                image.texture = frame.images[i];
                frame.displaySets[i] = device->CreateResourceSet({displayPipeline, &image, 1});
                Require(frame.displaySets[i] != nullptr, "Display bindings failed.");
            }
            for(auto& list : frame.lists)
            {
                list = device->AcquireCommandList();
                Require(list != nullptr, "Command list creation failed.");
            }
        }
        const auto started = std::chrono::steady_clock::now();
        uint64_t frameIndex = 0;

        while(window.IsRunning())
        {
            Platform::Window::PollEvents();
            if(!window.IsRunning()) break;
            auto& frame = frames[frameIndex % frames.size()];

            // Fence: GPU가 이전 제출을 끝낸 후 같은 슬롯의 목록과 텍스처를 재사용한다.
            if(frame.completion)
            {
                Require(device->Wait(frame.completion, UINT64_MAX), "Frame slot wait failed.");
                frame.completion = {};
            }
            for(auto* list : frame.lists)
                Require(device->ResetCommandList(list), "Command list reset failed.");
            if(!device->BeginFrame())
            {
                Require(!device->IsLost(), "Device lost.");
                Platform::Window::WaitEvents();
                continue;
            }
            auto* target = device->GetBackBuffer();
            Require(target != nullptr, "Backbuffer unavailable.");
            const auto extent = target->GetDesc();
            const float elapsed = std::chrono::duration<float>(
                std::chrono::steady_clock::now() - started).count();

            // 3. 각 스레드는 자신의 CommandList와 출력 텍스처만 기록한다.
            // 공유 Pipeline과 자원 수명, 프레임/스왑체인은 작업 중에 변경하지 않는다.
            std::array<std::exception_ptr, workerCount> errors{};
            {
                // 이 블록을 벗어날 때 모든 작업 스레드가 합류한다. 생성 도중 예외가 나도 합류한다.
                std::array<std::jthread, workerCount> workers;
                for(uint32_t i = 0; i < workerCount; ++i)
                {
                    workers[i] = std::jthread([&, i, elapsed] {
                        try
                        {
                            auto* commands = frame.lists[i];
                            auto* image = frame.images[i];
                            const RHI::ResourceBarrierDesc writable{nullptr, image,
                                frame.imageState, RHI::ResourceState::RenderTarget, {}};
                            commands->ResourceBarrier(&writable, 1);
                            RHI::ColorAttachment color;
                            color.texture = image;
                            color.loadOp = RHI::LoadOp::Clear;
                            color.storeOp = RHI::StoreOp::Store;
                            color.clearColor[0] = i == 0 ? 0.12f : 0.035f;
                            color.clearColor[1] = 0.06f;
                            color.clearColor[2] = i == 0 ? 0.035f : 0.16f;
                            color.clearColor[3] = 1;
                            commands->BeginRendering({&color, 1, nullptr});
                            commands->BindGraphicsPipeline(scenePipeline);
                            commands->SetViewport({0, 0, 512, 512, 0, 1});
                            commands->SetScissor({0, 0, 512, 512});
                            const float parameters[4] = {elapsed * 0.4f + float(i) * 1.2f, 0, 0, 0};
                            commands->SetInlineConstants(0, sizeof(parameters), parameters);
                            commands->DrawInstanced(3, 1, 0, 0);
                            commands->EndRendering();

                            // GPU Barrier: 텍스처의 Draw 쓰기를 뒤따르는 샘플링 읽기와 연결한다.
                            const RHI::ResourceBarrierDesc readable{nullptr, image,
                                RHI::ResourceState::RenderTarget, RHI::ResourceState::ShaderResource, {}};
                            commands->ResourceBarrier(&readable, 1);
                            Require(commands->Close(), "Worker command recording failed.");
                        }
                        catch(...)
                        {
                            // 예외는 각 스레드의 슬롯에 저장하고 모두 합류한 뒤 메인에서 처리한다.
                            errors[i] = std::current_exception();
                        }
                    });
                }
            }
            // CPU 합류는 기록 완료를 보장한다. GPU 의존성은 위 Barrier와 아래 제출 순서로 표현한다.
            for(const auto& error : errors)
                if(error) std::rethrow_exception(error);

            // 4. 두 결과를 읽는 합성 패스는 메인 스레드의 별도 목록에 기록한다.
            auto* composite = frame.lists[workerCount];
            const RHI::ResourceBarrierDesc begin{nullptr, target,
                RHI::ResourceState::Present, RHI::ResourceState::RenderTarget, {}};
            composite->ResourceBarrier(&begin, 1);
            RHI::ColorAttachment color;
            color.texture = target;
            color.loadOp = RHI::LoadOp::Clear;
            color.storeOp = RHI::StoreOp::Store;
            color.clearColor[3] = 1;
            composite->BeginRendering({&color, 1, nullptr});
            composite->BindGraphicsPipeline(displayPipeline);
            composite->SetViewport({0, 0, float(extent.width), float(extent.height), 0, 1});
            composite->SetScissor({0, 0, extent.width, extent.height});
            for(uint32_t i = 0; i < workerCount; ++i)
            {
                composite->BindResourceSet(frame.displaySets[i]);
                const float transform[4] = {i == 0 ? -0.5f : 0.5f, 0, 0.5f, 1};
                composite->SetInlineConstants(0, sizeof(transform), transform);
                composite->DrawInstanced(6, 1, 0, 0);
            }
            composite->EndRendering();
            const RHI::ResourceBarrierDesc end{nullptr, target,
                RHI::ResourceState::RenderTarget, RHI::ResourceState::Present, {}};
            composite->ResourceBarrier(&end, 1);
            Require(composite->Close(), "Composite command recording failed.");

            // 5. 공통 RHI 명령을 네이티브 명령으로 변환하는 작업도 장치의 작업자 2개로 기록한다.
            // PrepareCommandLists는 모든 네이티브 기록이 끝난 뒤 반환한다.
            Require(device->PrepareCommandLists(frame.lists.data(), uint32_t(frame.lists.size()), workerCount),
                "Parallel native recording failed.");
            RHI::SubmitDesc submission;
            // CPU 스레드가 끝난 순서와 무관하게 worker 0, worker 1, composite 순서로 제출한다.
            submission.commandLists = frame.lists.data();
            submission.commandListCount = uint32_t(frame.lists.size());
            Require(device->Submit(submission, frame.completion), "Submission failed.");
            frame.imageState = RHI::ResourceState::ShaderResource;
            Require(device->Present(), "Presentation failed.");
            ++frameIndex;
        }

        Require(device->WaitIdle(), "GPU wait failed.");
        for(auto& frame : frames)
        {
            for(auto* list : frame.lists) device->DestroyCommandList(list);
            for(auto* set : frame.displaySets) device->DestroyResourceSet(set);
            for(auto* image : frame.images) device->DestroyTexture(image);
        }
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
