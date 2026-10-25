#include "dyf/RHI.h"
#include "dyf/Platform/Window.h"

#include <array>
#include <cstddef>
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
    constexpr const char* vertexEntry = "vertexMain";
    constexpr const char* fragmentEntry = "fragmentMain";
#else
    constexpr const char* vertexEntry = "main";
    constexpr const char* fragmentEntry = "main";
#endif
    const auto vertexBinary = readShader("Shaders/vertex.bin");
    const auto fragmentBinary = readShader("Shaders/fragment.bin");

    // 1. 창은 표시 대상만 제공한다. GPU 작업은 공개 RHI로 직접 구성한다.
    Platform::Window window(960, 640, "RHI / 06 Depth and Blend");
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
    struct Vertex { float x, y, z, r, g, b, a; };
    const RHI::VertexBufferLayout input{0, sizeof(Vertex), RHI::VertexStepMode::Vertex};
    const std::array<RHI::VertexAttribute, 2> attributes = {{
        {0, 0, RHI::Format::R32G32B32_FLOAT, 0},
        {1, 0, RHI::Format::R32G32B32A32_FLOAT, uint32_t(offsetof(Vertex, r))}
    }};
    pipelineDesc.vertexBuffers = &input;
    pipelineDesc.vertexBufferCount = 1;
    pipelineDesc.vertexAttributes = attributes.data();
    pipelineDesc.vertexAttributeCount = uint32_t(attributes.size());
    pipelineDesc.layout.inlineConstantSize = 16;
    pipelineDesc.layout.inlineConstantStages = RHI::ShaderStageFlags::Vertex;
    pipelineDesc.layout.inlineConstantBinding = 15;
    pipelineDesc.depthStencil.format = RHI::Format::D32_FLOAT;
    pipelineDesc.depthStencil.depthTestEnabled = true;
    pipelineDesc.depthStencil.depthWriteEnabled = true;
    pipelineDesc.depthStencil.depthCompareOp = RHI::CompareOp::Less;
    auto* depthPipeline = device->CreateGraphicsPipeline(pipelineDesc);

    // 같은 셰이더에서 깊이와 블렌딩 상태를 바꾼 별도의 파이프라인이다.
    pipelineDesc.depthStencil.depthTestEnabled = false;
    pipelineDesc.depthStencil.depthWriteEnabled = false;
    colorOutput.blend.enabled = true;
    colorOutput.blend.sourceColor = RHI::BlendFactor::SourceAlpha;
    colorOutput.blend.destinationColor = RHI::BlendFactor::OneMinusSourceAlpha;
    colorOutput.blend.colorOp = RHI::BlendOp::Add;
    colorOutput.blend.sourceAlpha = RHI::BlendFactor::One;
    colorOutput.blend.destinationAlpha = RHI::BlendFactor::OneMinusSourceAlpha;
    colorOutput.blend.alphaOp = RHI::BlendOp::Add;
    auto* blendPipeline = device->CreateGraphicsPipeline(pipelineDesc);

    const std::array<Vertex, 6> vertices = {{
        {-0.7f,-0.6f,0.2f,1,0.2f,0.15f,0.55f},{0.7f,-0.6f,0.2f,1,0.2f,0.15f,0.55f},{0,0.7f,0.2f,1,0.2f,0.15f,0.55f},
        {-0.6f,-0.7f,0.8f,0.15f,0.35f,1,1},{0.8f,-0.7f,0.8f,0.15f,0.35f,1,1},{0.1f,0.6f,0.8f,0.15f,0.35f,1,1}
    }};
    auto* vertexBuffer = device->CreateBuffer({sizeof(vertices), sizeof(Vertex), RHI::BufferUsage::Vertex,
        RHI::ResourceState::CopyDestination});

    auto* upload = device->AcquireCommandList();
    (void)device->UpdateBuffer(*upload, vertexBuffer, 0, vertices.data(), sizeof(vertices));
    const RHI::ResourceBarrierDesc ready{vertexBuffer, nullptr, RHI::ResourceState::CopyDestination,
        RHI::ResourceState::VertexBuffer, {}};
    upload->ResourceBarrier(&ready, 1);
    (void)upload->Close();
    (void)device->Submit(&upload, 1);
    device->DestroyCommandList(upload);
    // 같은 장치 큐에 뒤이어 제출하는 Draw는 이 업로드 뒤에 실행된다.
    RHI::TextureHandle depthTexture = nullptr;
    RHI::ResourceState depthState = RHI::ResourceState::Undefined;

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
        if(!depthTexture || depthTexture->GetDesc().width != extent.width || depthTexture->GetDesc().height != extent.height)
        {
            if(depthTexture) device->DestroyTexture(depthTexture);
            RHI::TextureDesc depthDesc;
            depthDesc.width = extent.width;
            depthDesc.height = extent.height;
            depthDesc.format = RHI::Format::D32_FLOAT;
            depthDesc.usage = RHI::TextureUsage::DepthStencil;
            depthTexture = device->CreateTexture(depthDesc);

            depthState = RHI::ResourceState::Undefined;
        }
        const std::array<RHI::ResourceBarrierDesc, 2> begin = {{
            {nullptr, target, RHI::ResourceState::Present, RHI::ResourceState::RenderTarget, {}},
            {nullptr, depthTexture, depthState, RHI::ResourceState::DepthWrite, {}}
        }};
        commands->ResourceBarrier(begin.data(), uint32_t(begin.size()));
        RHI::ColorAttachment color;
        color.texture = target;
        color.loadOp = RHI::LoadOp::Clear;
        color.storeOp = RHI::StoreOp::Store;
        color.clearColor[0] = 0.035f; color.clearColor[1] = 0.05f; color.clearColor[2] = 0.08f; color.clearColor[3] = 1;
        RHI::DepthStencilAttachment depth;
        depth.texture = depthTexture;
        depth.state = RHI::ResourceState::DepthWrite;
        depth.depthLoadOp = RHI::LoadOp::Clear;
        depth.depthStoreOp = RHI::StoreOp::Store;
        commands->BeginRendering({&color, 1, &depth});
        commands->SetViewport({0,0,float(extent.width),float(extent.height)});
        commands->SetScissor({0,0,extent.width,extent.height});
        commands->BindGraphicsPipeline(depthPipeline);
        commands->BindVertexBuffer(0, vertexBuffer, 0);
        const float left[4] = {-0.5f,0,0.6f,0.8f};
        commands->SetInlineConstants(0,sizeof(left),left);
        // 가까운 빨강을 먼저 그려도, 뒤의 먼 파랑이 깊이 검사에서 가려진다.
        commands->DrawInstanced(3,1,0,0);
        commands->DrawInstanced(3,1,3,0);
        commands->BindGraphicsPipeline(blendPipeline);
        commands->BindVertexBuffer(0, vertexBuffer, 0);
        const float right[4] = {0.5f,0,0.6f,0.8f};
        commands->SetInlineConstants(0,sizeof(right),right);
        // 오른쪽은 파랑 뒤에 반투명 빨강을 그려 색상 혼합을 확인한다.
        commands->DrawInstanced(3,1,3,0);
        commands->DrawInstanced(3,1,0,0);
        commands->EndRendering();
        const RHI::ResourceBarrierDesc end{nullptr, target,
            RHI::ResourceState::RenderTarget, RHI::ResourceState::Present, {}};
        commands->ResourceBarrier(&end, 1);
        depthState = RHI::ResourceState::DepthWrite;

        // GPU 제출과 화면 표시는 별개다. Submit은 프레임을 끝내지 않는다.
        (void)commands->Close();
        (void)device->Submit(&commands, 1);
        (void)device->Present();
        // 제출된 네이티브 명령은 GPU 완료까지 참조 자원을 보유한다.
        device->DestroyCommandList(commands);
    }

    (void)device->WaitIdle();
    device->DestroyTexture(depthTexture);
    device->DestroyBuffer(vertexBuffer);
    device->DestroyPipeline(blendPipeline);
    device->DestroyPipeline(depthPipeline);
    device->DestroyShader(fragmentShader);
    device->DestroyShader(vertexShader);
    return 0;
}
