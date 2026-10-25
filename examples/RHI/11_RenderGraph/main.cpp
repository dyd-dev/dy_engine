#include "dyf/RHI.h"
#include "dyf/Platform/Window.h"

#include <array>
#include <chrono>
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
    // Vulkan: glslc -fshader-stage=comp Shaders/compute.glsl -o Shaders/compute.bin
    // D3D12: dxc -T cs_6_0 -E main Shaders/compute.hlsl -Fo Shaders/compute.bin
    // Vulkan: glslc -fshader-stage=vert Shaders/vertex.glsl -o Shaders/vertex.bin
    // D3D12: dxc -T vs_6_0 -E main Shaders/vertex.hlsl -Fo Shaders/vertex.bin
    // Vulkan: glslc -fshader-stage=frag Shaders/fragment.glsl -o Shaders/fragment.bin
    // D3D12: dxc -T ps_6_0 -E main Shaders/fragment.hlsl -Fo Shaders/fragment.bin
    // Vulkan: glslc -fshader-stage=vert Shaders/display_vertex.glsl -o Shaders/display_vertex.bin
    // D3D12: dxc -T vs_6_0 -E main Shaders/display_vertex.hlsl -Fo Shaders/display_vertex.bin
    // Vulkan: glslc -fshader-stage=frag Shaders/display_fragment.glsl -o Shaders/display_fragment.bin
    // D3D12: dxc -T ps_6_0 -E main Shaders/display_fragment.hlsl -Fo Shaders/display_fragment.bin
    // Metal: 대상 SDK에 맞춰 macosx를 변경한다.
    // xcrun -sdk macosx metal -c Shaders/program.metal -o Shaders/program.air
    // 각 셰이더가 같은 라이브러리에서 자신의 진입점을 선택한다.
    // xcrun -sdk macosx metallib Shaders/program.air -o Shaders/compute.bin
    // xcrun -sdk macosx metallib Shaders/program.air -o Shaders/vertex.bin
    // xcrun -sdk macosx metallib Shaders/program.air -o Shaders/fragment.bin
    // xcrun -sdk macosx metallib Shaders/program.air -o Shaders/display_vertex.bin
    // xcrun -sdk macosx metallib Shaders/program.air -o Shaders/display_fragment.bin
#ifdef DY_EXAMPLE_METAL
    constexpr const char* computeEntry = "computeMain";
    constexpr const char* vertexEntry = "vertexMain";
    constexpr const char* fragmentEntry = "fragmentMain";
    constexpr const char* display_vertexEntry = "displayVertex";
    constexpr const char* display_fragmentEntry = "displayFragment";
#else
    constexpr const char* computeEntry = "main";
    constexpr const char* vertexEntry = "main";
    constexpr const char* fragmentEntry = "main";
    constexpr const char* display_vertexEntry = "main";
    constexpr const char* display_fragmentEntry = "main";
#endif
    std::ifstream computeFile("Shaders/compute.bin", std::ios::binary);
    const std::vector<char> computeBinary{
        std::istreambuf_iterator<char>(computeFile), std::istreambuf_iterator<char>()};
    std::ifstream vertexFile("Shaders/vertex.bin", std::ios::binary);
    const std::vector<char> vertexBinary{
        std::istreambuf_iterator<char>(vertexFile), std::istreambuf_iterator<char>()};
    std::ifstream fragmentFile("Shaders/fragment.bin", std::ios::binary);
    const std::vector<char> fragmentBinary{
        std::istreambuf_iterator<char>(fragmentFile), std::istreambuf_iterator<char>()};
    std::ifstream display_vertexFile("Shaders/display_vertex.bin", std::ios::binary);
    const std::vector<char> display_vertexBinary{
        std::istreambuf_iterator<char>(display_vertexFile), std::istreambuf_iterator<char>()};
    std::ifstream display_fragmentFile("Shaders/display_fragment.bin", std::ios::binary);
    const std::vector<char> display_fragmentBinary{
        std::istreambuf_iterator<char>(display_fragmentFile), std::istreambuf_iterator<char>()};

    // 1. 창은 표시 대상만 제공한다. GPU 작업은 공개 RHI로 직접 구성한다.
    Platform::Window window(960, 640, "RHI / 11 RenderGraph");
    std::unique_ptr<RHI::IDevice> device(RHI::IDevice::Create({}));

    RHI::SwapchainDesc swapchain;
    swapchain.window = window.GetHandle();
    swapchain.format = RHI::DefaultSwapchainFormat;
    swapchain.presentMode = RHI::PresentMode::Fifo;
    swapchain.minimumImageCount = 2;
    (void)device->CreateSwapchain(swapchain);

    // 셰이더 조합과 입력 형식, 고정 기능 상태를 하나의 GPU 파이프라인으로 만든다.
    auto* vertexShader = device->CreateShader({RHI::ShaderStage::Vertex,
        vertexEntry, vertexBinary.data(), vertexBinary.size()});
    auto* fragmentShader = device->CreateShader({RHI::ShaderStage::Fragment,
        fragmentEntry, fragmentBinary.data(), fragmentBinary.size()});

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

    auto* computeShader = device->CreateShader({RHI::ShaderStage::Compute,
        computeEntry, computeBinary.data(), computeBinary.size()});

    const RHI::ResourceBindingLayout outputBinding{0, RHI::ResourceBindingType::ReadWriteStorageBuffer,
        1, RHI::ShaderStageFlags::Compute, {}};
    RHI::ComputePipelineDesc computeDesc;
    computeDesc.computeShader = computeShader;
    computeDesc.layout = {&outputBinding,1,16,RHI::ShaderStageFlags::Compute,15};
    // 그룹 하나의 스레드 3개가 정점 하나씩 생성한다.
    computeDesc.threadGroupSize[0] = 3;
    computeDesc.threadGroupSize[1] = computeDesc.threadGroupSize[2] = 1;
    auto* computePipeline = device->CreateComputePipeline(computeDesc);

    RHI::BufferDesc generatedDesc;
    generatedDesc.size = 3 * input.stride;
    generatedDesc.stride = input.stride;
    generatedDesc.usage = RHI::BufferUsage::Storage | RHI::BufferUsage::Vertex;
    generatedDesc.initialState = RHI::ResourceState::UnorderedAccess;
    auto* generatedVertices = device->CreateBuffer(generatedDesc);

    RHI::ResourceBinding output;
    output.binding = 0;
    output.buffer = generatedVertices;
    output.size = generatedDesc.size;
    auto* computeSet = device->CreateResourceSet({computePipeline,&output,1});

    RHI::ResourceState generatedState = RHI::ResourceState::UnorderedAccess;
    const auto started = std::chrono::steady_clock::now();

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
        (void)graph.Compile();
        (void)graph.Execute(commands);
        // Execute는 제출하지 않는다. 아래 Close / Submit / Present를 호출자가 수행한다.
        generatedState = RHI::ResourceState::VertexBuffer;
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
