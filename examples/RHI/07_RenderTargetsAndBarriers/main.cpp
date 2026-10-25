#include "dyf/RHI.h"
#include "dyf/Platform/Window.h"

#include <array>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <memory>
#include <vector>

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
    constexpr const char* scene_vertexEntry = "vertexMain";
    constexpr const char* scene_fragmentEntry = "fragmentMain";
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

    // 1. 창은 표시 대상만 제공한다. GPU 작업은 공개 RHI로 직접 구성한다.
    Platform::Window window(960, 640, "RHI / 07 Render Targets and Barriers");
    std::unique_ptr<RHI::IDevice> device(RHI::IDevice::Create({}));

    RHI::SwapchainDesc swapchain;
    swapchain.window = window.GetHandle();
    swapchain.format = RHI::DefaultSwapchainFormat;
    swapchain.presentMode = RHI::PresentMode::Fifo;
    swapchain.minimumImageCount = 2;
    (void)device->CreateSwapchain(swapchain);

    // 셰이더 조합과 입력 형식, 고정 기능 상태를 하나의 GPU 파이프라인으로 만든다.
    auto* sceneVertex = device->CreateShader({RHI::ShaderStage::Vertex,
        scene_vertexEntry, scene_vertexBinary.data(), scene_vertexBinary.size()});
    auto* sceneFragment = device->CreateShader({RHI::ShaderStage::Fragment,
        scene_fragmentEntry, scene_fragmentBinary.data(), scene_fragmentBinary.size()});

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

    auto* displayVertex = device->CreateShader({RHI::ShaderStage::Vertex,
        display_vertexEntry, display_vertexBinary.data(), display_vertexBinary.size()});
    auto* displayFragment = device->CreateShader({RHI::ShaderStage::Fragment,
        display_fragmentEntry, display_fragmentBinary.data(), display_fragmentBinary.size()});

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

    RHI::TextureDesc imageDesc;
    imageDesc.width = imageDesc.height = 512;
    imageDesc.format = RHI::Format::R8G8B8A8_UNORM;
    imageDesc.usage = RHI::TextureUsage::RenderTarget | RHI::TextureUsage::ShaderResource;
    auto* offscreen = device->CreateTexture(imageDesc);

    RHI::ResourceBinding image;
    image.binding = 0;
    image.texture = offscreen;
    auto* displaySet = device->CreateResourceSet({displayPipeline, &image, 1});

    RHI::ResourceState offscreenState = RHI::ResourceState::Undefined;

    while(window.IsRunning())
    {
        Platform::Window::PollEvents();
        if(!window.IsRunning()) break;
        if(!device->BeginFrame())
        {
            Platform::Window::WaitEvents();
            continue;
        }
        auto* target = device->GetBackBuffer();
        const auto extent = target->GetDesc();
        auto* commands = device->AcquireCommandList();
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
        (void)commands->Close();
        (void)device->Submit(&commands, 1);
        (void)device->Present();
        // 제출된 네이티브 명령은 GPU 완료까지 참조 자원을 보유한다.
        device->DestroyCommandList(commands);
    }

    (void)device->WaitIdle();
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
