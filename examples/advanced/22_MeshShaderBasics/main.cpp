#include "dyf/Platform/Window.h"
#include "dyf/RHI.h"

#include <fstream>
#include <iterator>
#include <memory>
#include <vector>

using namespace dyf::RHI;

int main()
{
    const auto readShader = [](const char* path) {
        std::ifstream file(path, std::ios::binary);
        return std::vector<char>{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    };

#ifdef __APPLE__
    const char* basic_msEntry="meshMain";
    const char* basic_psEntry="fragmentMain";
#else
    const char* basic_msEntry="main";
    const char* basic_psEntry="main";
#endif
    const auto basic_msBinary = readShader("Shaders/basic_ms.bin");
    const auto basic_psBinary = readShader("Shaders/basic_ps.bin");

    dyf::Platform::Window window(800,600,"Advanced / Mesh shader basics");
    std::unique_ptr<IDevice> device(IDevice::Create({}));
    ResourceScope resources(*device);
    SwapchainDesc swapchain;
    swapchain.window=window.GetHandle();
    swapchain.format=DefaultSwapchainFormat;
    (void)device->CreateSwapchain(swapchain);

    auto* meshShader=resources.Keep(device->CreateShader({ShaderStage::Mesh,
        basic_msEntry,basic_msBinary.data(),basic_msBinary.size()}));
    auto* fragmentShader=resources.Keep(device->CreateShader({ShaderStage::Fragment,
        basic_psEntry,basic_psBinary.data(),basic_psBinary.size()}));
    const ColorAttachmentDesc output{swapchain.format,{},ColorWriteMask::All};
    MeshPipelineDesc pipelineDesc;
    pipelineDesc.meshShader=meshShader;
    pipelineDesc.fragmentShader=fragmentShader;
    pipelineDesc.raster.fillMode = FillMode::Solid;
    pipelineDesc.raster.cullMode = CullMode::None;
    pipelineDesc.raster.frontFace = FrontFace::CounterClockwise;
    pipelineDesc.colorAttachments=&output;
    pipelineDesc.colorAttachmentCount=1;
    auto* pipeline=resources.Keep(device->CreateMeshPipeline(pipelineDesc));

    while(window.IsRunning())
    {
        window.PollEvents();
        if(!window.IsRunning()) break;
        if(!device->BeginFrame()) continue;
        ResourceScope frameResources(*device);
        auto* commands=frameResources.Keep(device->AcquireCommandList());
        auto* target=device->GetBackBuffer();
        const auto extent=target->GetDesc();
        const ResourceBarrierDesc begin{nullptr,target,
            ResourceState::Present,ResourceState::RenderTarget,{}};
        commands->ResourceBarrier(&begin,1);
        ColorAttachment color;
        color.texture=target;
        color.loadOp=LoadOp::Clear;
        color.storeOp=StoreOp::Store;
        color.clearColor[0]=0.08f;
        color.clearColor[1]=0.10f;
        color.clearColor[2]=0.15f;
        color.clearColor[3]=1.0f;
        commands->BeginRendering({&color,1,nullptr});
        commands->BindGraphicsPipeline(pipeline);
        commands->SetViewport({0,0,float(extent.width),float(extent.height)});
        commands->SetScissor({0,0,extent.width,extent.height});
        // 정점 버퍼 없이 메쉬 셰이더가 정점 세 개와 삼각형 한 개를 만든다.
        commands->DispatchMesh(1,1,1);
        commands->EndRendering();
        const ResourceBarrierDesc end{nullptr,target,
            ResourceState::RenderTarget,ResourceState::Present,{}};
        commands->ResourceBarrier(&end,1);
        (void)commands->Close();
        (void)device->Submit(&commands,1);
        (void)device->Present();
    }
    (void)device->WaitIdle();
}
