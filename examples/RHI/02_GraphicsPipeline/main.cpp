#include "dyf/RHI.h"
#include "dyf/Platform/Window.h"

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
    constexpr const char* vertexEntry = "vertexMain";
    constexpr const char* fragmentEntry = "fragmentMain";
#else
    constexpr const char* vertexEntry = "main";
    constexpr const char* fragmentEntry = "main";
#endif
    const auto vertexBinary = readShader("Shaders/vertex.bin");
    const auto fragmentBinary = readShader("Shaders/fragment.bin");

    // 1. 창은 표시 대상만 제공한다. GPU 작업은 공개 RHI로 직접 구성한다.
    Platform::Window window(960, 640, "RHI / 02 Graphics Pipeline");
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
    auto* pipeline = device->CreateGraphicsPipeline(pipelineDesc);

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
        // 파이프라인은 셰이더와 상태의 묶음이고 Draw가 실제 실행을 기록한다.
        commands->BindGraphicsPipeline(pipeline);
        commands->DrawInstanced(3, 1, 0, 0);
        commands->EndRendering();
        const RHI::ResourceBarrierDesc end{nullptr, target,
            RHI::ResourceState::RenderTarget, RHI::ResourceState::Present, {}};
        commands->ResourceBarrier(&end, 1);

        // GPU 제출과 화면 표시는 별개다. Submit은 프레임을 끝내지 않는다.
        (void)commands->Close();
        (void)device->Submit(&commands, 1);
        (void)device->Present();
        // 제출된 네이티브 명령은 GPU 완료까지 참조 자원을 보유한다.
        device->DestroyCommandList(commands);
    }

    (void)device->WaitIdle();
    device->DestroyPipeline(pipeline);
    device->DestroyShader(fragmentShader);
    device->DestroyShader(vertexShader);
    return 0;
}
