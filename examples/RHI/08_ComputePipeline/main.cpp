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
    const auto readShader = [](const char* path) {
        std::ifstream file(path, std::ios::binary);
        return std::vector<char>{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    };

#ifdef __APPLE__
    constexpr const char* computeEntry = "computeMain";
    constexpr const char* vertexEntry = "vertexMain";
    constexpr const char* fragmentEntry = "fragmentMain";
#else
    constexpr const char* computeEntry = "main";
    constexpr const char* vertexEntry = "main";
    constexpr const char* fragmentEntry = "main";
#endif
    const auto computeBinary = readShader("Shaders/compute.bin");
    const auto vertexBinary = readShader("Shaders/vertex.bin");
    const auto fragmentBinary = readShader("Shaders/fragment.bin");

    // 1. 창은 표시 대상만 제공한다. GPU 작업은 공개 RHI로 직접 구성한다.
    Platform::Window window(960, 640, "RHI / 08 Compute Pipeline");
    std::unique_ptr<RHI::IDevice> device(RHI::IDevice::Create({}));

    RHI::SwapchainDesc swapchain;
    swapchain.window = window.GetHandle();
    swapchain.format = RHI::DefaultSwapchainFormat;
    (void)device->CreateSwapchain(swapchain);

    // 셰이더 조합과 입력 형식, 고정 기능 상태를 하나의 GPU 파이프라인으로 만든다.
    auto* vertexShader = device->CreateShader({RHI::ShaderStage::Vertex,
        vertexEntry, vertexBinary.data(), vertexBinary.size()});
    auto* fragmentShader = device->CreateShader({RHI::ShaderStage::Fragment,
        fragmentEntry, fragmentBinary.data(), fragmentBinary.size()});

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
        // Compute는 BeginRendering 바깥에서 기록한다.
        const RHI::ResourceBarrierDesc writable{generatedVertices,nullptr,
            generatedState,RHI::ResourceState::UnorderedAccess,{}};
        commands->ResourceBarrier(&writable,1);
        commands->BindComputePipeline(computePipeline);
        commands->BindResourceSet(computeSet);
        const float parameters[4] = {std::chrono::duration<float>(std::chrono::steady_clock::now()-started).count(),0,0,0};
        commands->SetInlineConstants(0,sizeof(parameters),parameters);
        commands->Dispatch(1,1,1);
        const RHI::ResourceBarrierDesc vertexReady{generatedVertices,nullptr,
            RHI::ResourceState::UnorderedAccess,RHI::ResourceState::VertexBuffer,{}};
        commands->ResourceBarrier(&vertexReady,1);
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
        commands->SetViewport({0, 0, float(extent.width), float(extent.height)});
        commands->SetScissor({0, 0, extent.width, extent.height});
        commands->BindGraphicsPipeline(pipeline);
        commands->BindVertexBuffer(0,generatedVertices,0);
        commands->DrawInstanced(3,1,0,0);
        commands->EndRendering();
        const RHI::ResourceBarrierDesc end{nullptr, target,
            RHI::ResourceState::RenderTarget, RHI::ResourceState::Present, {}};
        commands->ResourceBarrier(&end, 1);
        generatedState = RHI::ResourceState::VertexBuffer;

        // GPU 제출과 화면 표시는 별개다. Submit은 프레임을 끝내지 않는다.
        (void)commands->Close();
        (void)device->Submit(&commands, 1);
        (void)device->Present();
        // 제출된 네이티브 명령은 GPU 완료까지 참조 자원을 보유한다.
        device->DestroyCommandList(commands);
    }

    (void)device->WaitIdle();
    device->DestroyResourceSet(computeSet);
    device->DestroyBuffer(generatedVertices);
    device->DestroyPipeline(computePipeline);
    device->DestroyShader(computeShader);
    device->DestroyPipeline(pipeline);
    device->DestroyShader(fragmentShader);
    device->DestroyShader(vertexShader);
    return 0;
}
