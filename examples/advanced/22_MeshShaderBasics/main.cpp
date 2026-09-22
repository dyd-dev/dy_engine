#include <dyf/RHI.h>
#include <dyf/Platform/Window.h>

#include "basic_ms.h"
#include "basic_ps.h"

#include <cstring>
#include <iostream>
#include <memory>
#include <string>

using namespace dyf::RHI;

namespace
{
    struct Options
    {
        uint32_t frames = 0;
        bool validation = false;
    };

    Options ParseOptions(int argc, char** argv)
    {
        Options options;
        for(int i = 1; i < argc; ++i)
        {
            std::string arg = argv[i];
            if(arg == "--validation")
            {
                options.validation = true;
            }
            else if(arg == "--frames" && i + 1 < argc)
            {
                options.frames = static_cast<uint32_t>(std::stoul(argv[++i]));
            }
        }
        return options;
    }

    void PrintHelp()
    {
        std::cout << "Usage: AdvancedMeshShaderBasics [--help] [--validation] [--frames N]\n"
                     "  --help        Print help and exit.\n"
                     "  --validation  Enable RHI validation layer.\n"
                     "  --frames N    Render N frames and exit normally.\n"
                     "\n"
                     "This example renders geometry via Mesh Shader (DispatchMesh)\n"
                     "generating vertices and triangle indices without a Vertex Buffer.\n";
    }
}

int main(int argc, char** argv)
{
    for(int i = 1; i < argc; ++i)
    {
        if(std::strcmp(argv[i], "--help") == 0)
        {
            PrintHelp();
            return 0;
        }
    }

    Options options = ParseOptions(argc, argv);

    DeviceDesc deviceDesc{};
    deviceDesc.enableValidation = options.validation;
    deviceDesc.maxFramesInFlight = 2;

    std::unique_ptr<IDevice> device(IDevice::Create(deviceDesc));
    if(!device)
    {
        std::cerr << "Failed to create RHI device.\n";
        return 1;
    }

    std::cout << "Device Feature Check:\n";
    std::cout << "  Supports(MeshShader): " << (device->Supports(Feature::MeshShader) ? "Supported" : "Unsupported") << "\n";
    std::cout << "  Supports(TaskShader): " << (device->Supports(Feature::TaskShader) ? "Supported" : "Unsupported") << "\n";

    if(!device->Supports(Feature::MeshShader))
    {
        std::cout << "Result: Current RHI backend or GPU does not support Mesh Shader (exit code 77).\n";
        return 77;
    }

    const uint32_t width = 800, height = 600;
    dyf::Platform::Window window(width, height, "Advanced / Mesh Shader Basics");
    if(!window.GetHandle())
    {
        std::cerr << "Failed to create window.\n";
        return 1;
    }

    ResourceScope resources(*device);

    SwapchainDesc swapchain{};
    swapchain.window = window.GetHandle();
    swapchain.format = Format::B8G8R8A8_UNORM;
    swapchain.minimumImageCount = 2;
    swapchain.presentMode = PresentMode::Fifo;
    if(!device->CreateSwapchain(swapchain))
    {
        std::cerr << "Failed to create swapchain.\n";
        return 1;
    }

    auto* meshShader = resources.Keep(device->CreateShader({
        ShaderStage::Mesh,
        ShaderData::basic_msEntryPoint,
        ShaderData::basic_ms,
        ShaderData::basic_msSize
    }));
    auto* pixelShader = resources.Keep(device->CreateShader({
        ShaderStage::Fragment,
        ShaderData::basic_psEntryPoint,
        ShaderData::basic_ps,
        ShaderData::basic_psSize
    }));

    if(!meshShader || !pixelShader)
    {
        std::cerr << "Failed to create shaders.\n";
        return 1;
    }

    ColorAttachmentDesc colorAttachmentDesc{swapchain.format, {}, ColorWriteMask::All};
    MeshPipelineDesc pipelineDesc{};
    pipelineDesc.meshShader = meshShader;
    pipelineDesc.fragmentShader = pixelShader;
    pipelineDesc.raster = {FillMode::Solid, CullMode::None, FrontFace::CounterClockwise, 0, 0, 0};
    pipelineDesc.colorAttachments = &colorAttachmentDesc;
    pipelineDesc.colorAttachmentCount = 1;
    pipelineDesc.layout = {nullptr, 0, 0, ShaderStageFlags::Mesh | ShaderStageFlags::Fragment, 0};

    if(!device->Supports(pipelineDesc))
    {
        std::cerr << "Device does not support the requested MeshPipelineDesc configuration.\n";
        return 1;
    }

    auto* pipeline = resources.Keep(device->CreateMeshPipeline(pipelineDesc));
    if(!pipeline)
    {
        std::cerr << "Failed to create mesh pipeline.\n";
        return 1;
    }

    std::cout << "Mesh pipeline created successfully. Starting render loop.\n";

    const Viewport viewport{0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f};
    const Rect scissor{0, 0, width, height};
    const float clearColor[4] = {0.08f, 0.10f, 0.15f, 1.0f};

    uint32_t currentFrame = 0;
    while(true)
    {
        window.PollEvents();
        if(!window.IsRunning()) break;
        if(options.frames > 0 && currentFrame >= options.frames) break;

        if(!device->BeginFrame())
        {
            continue;
        }

        ResourceScope frameScope(*device);
        auto* commands = frameScope.Keep(device->AcquireCommandList());
        if(!commands)
        {
            std::cerr << "Failed to acquire command list.\n";
            return 1;
        }

        auto* target = device->GetBackBuffer();
        const ResourceBarrierDesc beginBarrier{nullptr, target, ResourceState::Present, ResourceState::RenderTarget, {}};
        commands->ResourceBarrier(&beginBarrier, 1);

        ColorAttachment colorAttachment{};
        colorAttachment.texture = target;
        colorAttachment.loadOp = LoadOp::Clear;
        colorAttachment.storeOp = StoreOp::Store;
        colorAttachment.clearColor[0] = clearColor[0];
        colorAttachment.clearColor[1] = clearColor[1];
        colorAttachment.clearColor[2] = clearColor[2];
        colorAttachment.clearColor[3] = clearColor[3];

        commands->BeginRendering({&colorAttachment, 1, nullptr});
        commands->BindGraphicsPipeline(pipeline);
        commands->SetViewport(viewport);
        commands->SetScissor(scissor);

        // Dispatch 1 threadgroup: mesh shader generates 3 vertices and 1 triangle without vertex buffer
        commands->DispatchMesh(1, 1, 1);

        commands->EndRendering();

        const ResourceBarrierDesc endBarrier{nullptr, target, ResourceState::RenderTarget, ResourceState::Present, {}};
        commands->ResourceBarrier(&endBarrier, 1);

        if(!commands->Close() || !device->Submit(&commands, 1))
        {
            std::cerr << "Failed to submit command list.\n";
            return 1;
        }

        if(!device->Present())
        {
            std::cerr << "Failed to present.\n";
            return 1;
        }

        ++currentFrame;
    }

    (void)device->WaitIdle();
    std::cout << "Successfully rendered " << currentFrame << " frames and exiting.\n";
    return 0;
}
