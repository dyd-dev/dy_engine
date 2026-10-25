#include "dyf/RHI.h"
#include "dyf/Platform/Window.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <memory>
#include <vector>
#include <thread>

namespace RHI = dyf::RHI;
namespace Platform = dyf::Platform;

int main()
{
    // 해당 예제 폴더에서 실행한다. 연결한 백엔드에 맞는 명령으로 .bin을 준비한다.
    // Vulkan: glslc -fshader-stage=vert Shaders/scene_vertex.glsl -o Shaders/scene_vertex.bin
    // D3D12: dxc -T vs_6_0 -E main Shaders/scene_vertex.hlsl -Fo Shaders/scene_vertex.bin
    // Vulkan: glslc -fshader-stage=frag Shaders/scene_fragment.glsl -o Shaders/scene_fragment.bin
    // D3D12: dxc -T ps_6_0 -E main Shaders/scene_fragment.hlsl -Fo Shaders/scene_fragment.bin
    // Vulkan: glslc -fshader-stage=vert Shaders/display_vertex.glsl -o Shaders/display_vertex.bin
    // D3D12: dxc -T vs_6_0 -E main Shaders/display_vertex.hlsl -Fo Shaders/display_vertex.bin
    // Vulkan: glslc -fshader-stage=frag Shaders/display_fragment.glsl -o Shaders/display_fragment.bin
    // D3D12: dxc -T ps_6_0 -E main Shaders/display_fragment.hlsl -Fo Shaders/display_fragment.bin
    // Metal: 대상 SDK에 맞춰 macosx를 변경한다.
    // xcrun -sdk macosx metal -c Shaders/program.metal -o Shaders/program.air
    // 각 셰이더가 같은 라이브러리에서 자신의 진입점을 선택한다.
    // xcrun -sdk macosx metallib Shaders/program.air -o Shaders/scene_vertex.bin
    // xcrun -sdk macosx metallib Shaders/program.air -o Shaders/scene_fragment.bin
    // xcrun -sdk macosx metallib Shaders/program.air -o Shaders/display_vertex.bin
    // xcrun -sdk macosx metallib Shaders/program.air -o Shaders/display_fragment.bin
#ifdef DY_EXAMPLE_METAL
    constexpr const char* scene_vertexEntry = "sceneVertex";
    constexpr const char* scene_fragmentEntry = "sceneFragment";
    constexpr const char* display_vertexEntry = "displayVertex";
    constexpr const char* display_fragmentEntry = "displayFragment";
#else
    constexpr const char* scene_vertexEntry = "main";
    constexpr const char* scene_fragmentEntry = "main";
    constexpr const char* display_vertexEntry = "main";
    constexpr const char* display_fragmentEntry = "main";
#endif
    std::ifstream scene_vertexFile("Shaders/scene_vertex.bin", std::ios::binary);
    const std::vector<char> scene_vertexBinary{
        std::istreambuf_iterator<char>(scene_vertexFile), std::istreambuf_iterator<char>()};
    std::ifstream scene_fragmentFile("Shaders/scene_fragment.bin", std::ios::binary);
    const std::vector<char> scene_fragmentBinary{
        std::istreambuf_iterator<char>(scene_fragmentFile), std::istreambuf_iterator<char>()};
    std::ifstream display_vertexFile("Shaders/display_vertex.bin", std::ios::binary);
    const std::vector<char> display_vertexBinary{
        std::istreambuf_iterator<char>(display_vertexFile), std::istreambuf_iterator<char>()};
    std::ifstream display_fragmentFile("Shaders/display_fragment.bin", std::ios::binary);
    const std::vector<char> display_fragmentBinary{
        std::istreambuf_iterator<char>(display_fragmentFile), std::istreambuf_iterator<char>()};

    Platform::Window window(960, 640, "RHI / 10 Parallel Recording");
    std::unique_ptr<RHI::IDevice> device(RHI::IDevice::Create({}));

    RHI::SwapchainDesc swapchain;
    swapchain.window = window.GetHandle();
    swapchain.format = RHI::DefaultSwapchainFormat;
    swapchain.presentMode = RHI::PresentMode::Fifo;
    swapchain.minimumImageCount = 2;
    (void)device->CreateSwapchain(swapchain);

    // 1. Pipeline과 Shader는 메인 스레드에서 만들고 기록 중에는 공유해서 읽는다.
    auto* sceneVertex = device->CreateShader({RHI::ShaderStage::Vertex,
        scene_vertexEntry, scene_vertexBinary.data(), scene_vertexBinary.size()});
    auto* sceneFragment = device->CreateShader({RHI::ShaderStage::Fragment,
        scene_fragmentEntry, scene_fragmentBinary.data(), scene_fragmentBinary.size()});
    auto* displayVertex = device->CreateShader({RHI::ShaderStage::Vertex,
        display_vertexEntry, display_vertexBinary.data(), display_vertexBinary.size()});
    auto* displayFragment = device->CreateShader({RHI::ShaderStage::Fragment,
        display_fragmentEntry, display_fragmentBinary.data(), display_fragmentBinary.size()});

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

            RHI::ResourceBinding image;
            image.binding = 0;
            image.texture = frame.images[i];
            frame.displaySets[i] = device->CreateResourceSet({displayPipeline, &image, 1});
        }
        for(auto& list : frame.lists)
        {
            list = device->AcquireCommandList();
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
            (void)device->Wait(frame.completion, UINT64_MAX);
            frame.completion = {};
        }
        for(auto* list : frame.lists)
            (void)device->ResetCommandList(list);
        if(!device->BeginFrame())
        {
            Platform::Window::WaitEvents();
            continue;
        }
        auto* target = device->GetBackBuffer();
        const auto extent = target->GetDesc();
        const float elapsed = std::chrono::duration<float>(
            std::chrono::steady_clock::now() - started).count();

        // 3. 각 스레드는 자신의 CommandList와 출력 텍스처만 기록한다.
        // 공유 Pipeline과 자원 수명, 프레임/스왑체인은 작업 중에 변경하지 않는다.
        {
            std::array<std::thread, workerCount> workers;
            for(uint32_t i = 0; i < workerCount; ++i)
            {
                workers[i] = std::thread([&, i, elapsed] {
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
                    (void)commands->Close();
                });
            }
            for(auto& worker : workers) worker.join();
        }
        // CPU 합류는 기록 완료를 보장한다. GPU 의존성은 위 Barrier와 아래 제출 순서로 표현한다.

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
        (void)composite->Close();

        // 5. 공통 RHI 명령을 네이티브 명령으로 변환하는 작업도 장치의 작업자 2개로 기록한다.
        // PrepareCommandLists는 모든 네이티브 기록이 끝난 뒤 반환한다.
        (void)device->PrepareCommandLists(frame.lists.data(), uint32_t(frame.lists.size()), workerCount);
        RHI::SubmitDesc submission;
        // CPU 스레드가 끝난 순서와 무관하게 worker 0, worker 1, composite 순서로 제출한다.
        submission.commandLists = frame.lists.data();
        submission.commandListCount = uint32_t(frame.lists.size());
        (void)device->Submit(submission, frame.completion);
        frame.imageState = RHI::ResourceState::ShaderResource;
        (void)device->Present();
        ++frameIndex;
    }

    (void)device->WaitIdle();
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
