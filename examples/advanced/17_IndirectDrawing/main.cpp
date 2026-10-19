#include "../../HostedExample.h"
#include "dyf/RHI/IDevice.h"
#include "dyf/Platform/Window.h"
#include "dyf/RHI.h"
#include "generate.h"
#include "vertex.h"
#include "fragment.h"

#include <charconv>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string_view>

using namespace dyf::RHI;

int main(int argc, char** argv) try
{
    const auto check = [](bool ok, const char* message) {
        if(!ok) throw std::runtime_error(message);
    };
    uint32_t frameLimit = 0;
    bool validation = false;
    for(int i = 1; i < argc; ++i)
    {
        const std::string_view argument(argv[i]);
        if(argument == "--help")
        {
            std::cout << "AdvancedIndirectDrawing [--frames N] [--validation]\n"
                "GPU compute generates both draw argument structures; no CPU readback.\n"
                "Left cyan: DrawIndirect. Right orange: DrawIndexedIndirect.\n"
                "DispatchIndirect launches the generator from an uploaded {1,1,1} argument.\n"
                "Instance counts vary between 0 and 64. Escape closes the window.\n"
                "Exit codes: success 0, error 1, unsupported device 77.\n";
            return 0;
        }
        if(argument == "--validation") validation = true;
        else if(argument == "--frames" && i + 1 < argc)
        {
            const std::string_view value(argv[++i]);
            const auto result = std::from_chars(value.data(), value.data() + value.size(), frameLimit);
            check(result.ec == std::errc{} && result.ptr == value.data() + value.size(),
                "--frames requires an unsigned 32-bit integer (0 = unlimited).");
        }
        else throw std::runtime_error("Unknown or incomplete argument; use --help.");
    }

    DY_EXAMPLE_WINDOW(window, 1000, 650, "Advanced / GPU-generated indirect drawing");
    check(window.GetHandle() != nullptr, "Window creation failed.");
    DeviceDesc deviceDesc;
    deviceDesc.enableValidation = validation;
    std::unique_ptr<IDevice> owner(IDevice::Create(deviceDesc));
    check(bool(owner), "Device creation failed.");
    auto& device = *owner;
    if(!device.Supports(Feature::Rasterization) || !device.Supports(Feature::Compute) ||
        !device.Supports(Feature::IndirectDraw) || !device.Supports(Feature::IndirectDispatch))
    {
        std::cout << "UNSUPPORTED: requires rasterization, compute, indirect drawing and indirect dispatch.\n";
        return 77;
    }
    dyf::RHI::ResourceScope resources(device);
    SwapchainDesc swapchain;
    swapchain.window = window.GetHandle();
    swapchain.format = DefaultSwapchainFormat;
    check(device.CreateSwapchain(swapchain), "Swapchain creation failed.");

    auto* cs = resources.Keep(device.CreateShader({ShaderStage::Compute,
        ShaderData::generateEntryPoint, ShaderData::generate, ShaderData::generateSize}));
    auto* vs = resources.Keep(device.CreateShader({ShaderStage::Vertex,
        ShaderData::vertexEntryPoint, ShaderData::vertex, ShaderData::vertexSize}));
    auto* fs = resources.Keep(device.CreateShader({ShaderStage::Fragment,
        ShaderData::fragmentEntryPoint, ShaderData::fragment, ShaderData::fragmentSize}));

    // Compute writes all 9 words: DrawIndirectArguments, then DrawIndexedIndirectArguments.
    constexpr uint32_t indexedOffset = sizeof(DrawIndirectArguments);
    BufferDesc argumentDesc;
    argumentDesc.size = indexedOffset + sizeof(DrawIndexedIndirectArguments);
    argumentDesc.stride = sizeof(uint32_t);
    argumentDesc.usage = BufferUsage::Storage | BufferUsage::Indirect;
    auto* drawArguments = resources.Keep(device.CreateBuffer(argumentDesc));
    BufferDesc dispatchDesc;
    dispatchDesc.size = sizeof(DispatchIndirectArguments);
    dispatchDesc.usage = BufferUsage::Indirect;
    dispatchDesc.initialState = ResourceState::CopyDestination;
    auto* dispatchArguments = resources.Keep(device.CreateBuffer(dispatchDesc));
    const uint32_t indices[] = {0, 1, 2};
    BufferDesc indexDesc;
    indexDesc.size = sizeof(indices);
    indexDesc.usage = BufferUsage::Index;
    indexDesc.initialState = ResourceState::CopyDestination;
    auto* indexBuffer = resources.Keep(device.CreateBuffer(indexDesc));

    const ResourceBindingLayout writable{0, ResourceBindingType::ReadWriteStorageBuffer,
        1, ShaderStageFlags::Compute, {}};
    ComputePipelineDesc computeDesc;
    computeDesc.computeShader = cs;
    computeDesc.layout = {&writable, 1, 16, ShaderStageFlags::Compute, 15};
    auto* compute = resources.Keep(device.CreateComputePipeline(computeDesc));
    const ResourceBinding output{0, 0, drawArguments, nullptr, 0, argumentDesc.size, {}};
    auto* computeSet = resources.Keep(device.CreateResourceSet({compute, &output, 1}));

    const ColorAttachmentDesc format{swapchain.format, {}, ColorWriteMask::All};
    GraphicsPipelineDesc graphicsDesc;
    graphicsDesc.vertexShader = vs;
    graphicsDesc.fragmentShader = fs;
    graphicsDesc.topology = PrimitiveTopology::TriangleList;
    graphicsDesc.raster = {FillMode::Solid, CullMode::None, FrontFace::CounterClockwise, 0, 0, 0};
    graphicsDesc.colorAttachments = &format;
    graphicsDesc.colorAttachmentCount = 1;
    graphicsDesc.layout = {nullptr, 0, 16, ShaderStageFlags::Vertex, 15};
    auto* graphics = resources.Keep(device.CreateGraphicsPipeline(graphicsDesc));

    std::cout << "Left cyan: DrawIndirect | right orange: DrawIndexedIndirect\n"
        "GPU-generated instance counts: 0..64; CPU does not read the argument buffer.\n";
    const auto start = std::chrono::steady_clock::now();
    uint32_t frame = 0;
    while(dy_examples::ExampleRunning(window) && (!frameLimit || frame < frameLimit))
    {
        window.PollEvents();
        if(!dy_examples::BeginExampleFrame(window)) break;
        if(!dy_examples::ExampleRunning(window) || dy_examples::ExampleInput(window).WasPressed(dyf::Platform::Key::Escape)) break;
        if(!device.BeginFrame())
        {
            check(!device.IsLost(), "Device lost during BeginFrame.");
            window.WaitEvents(0.001);
            continue;
        }
        dyf::RHI::ResourceScope frameResources(device);
        auto* commands = frameResources.Keep(device.AcquireCommandList());
        if(frame == 0)
        {
            const DispatchIndirectArguments dispatch{1, 1, 1};
            check(device.UpdateBuffer(*commands, dispatchArguments, 0, &dispatch, sizeof(dispatch)),
                "Dispatch argument upload failed.");
            check(device.UpdateBuffer(*commands, indexBuffer, 0, indices, sizeof(indices)),
                "Index upload failed.");
            const ResourceBarrierDesc ready[] = {
                {dispatchArguments, nullptr, ResourceState::CopyDestination, ResourceState::IndirectArgument, {}},
                {indexBuffer, nullptr, ResourceState::CopyDestination, ResourceState::IndexBuffer, {}}
            };
            commands->ResourceBarrier(ready, 2);
        }

        const ResourceBarrierDesc prepare{drawArguments, nullptr,
            frame ? ResourceState::IndirectArgument : ResourceState::Undefined,
            ResourceState::UnorderedAccess, {}};
        commands->ResourceBarrier(&prepare, 1);
        commands->BindComputePipeline(compute);
        commands->BindResourceSet(computeSet);
        const float time = std::chrono::duration<float>(std::chrono::steady_clock::now() - start).count();
        const float computeConstants[4] = {time, 0, 0, 0};
        commands->SetInlineConstants(0, sizeof(computeConstants), computeConstants);
        commands->DispatchIndirect(dispatchArguments);
        const ResourceBarrierDesc generated{drawArguments, nullptr,
            ResourceState::UnorderedAccess, ResourceState::IndirectArgument, {}};
        commands->ResourceBarrier(&generated, 1);

        auto* target = device.GetBackBuffer();
        check(target != nullptr, "Backbuffer unavailable.");
        const auto& extent = target->GetDesc();
        const ResourceBarrierDesc begin{nullptr, target,
            ResourceState::Present, ResourceState::RenderTarget, {}};
        commands->ResourceBarrier(&begin, 1);
        ColorAttachment color;
        color.texture = target;
        color.loadOp = LoadOp::Clear;
        color.storeOp = StoreOp::Store;
        color.clearColor[0] = 0.018f;
        color.clearColor[1] = 0.025f;
        color.clearColor[2] = 0.045f;
        color.clearColor[3] = 1.0f;
        commands->BeginRendering({&color, 1, nullptr});
        commands->BindGraphicsPipeline(graphics);
        commands->SetViewport({0, 0, float(extent.width), float(extent.height), 0, 1});
        commands->SetScissor({0, 0, extent.width, extent.height});
        // Keep the grids and triangles proportional while the tiled window resizes.
        const float aspect = float(extent.width) / float(extent.height);
        float display[4] = {1.0f / (aspect > 1.5f ? aspect : 1.5f),
            aspect < 1.5f ? aspect / 1.5f : 1.0f, 0, 0};
        commands->SetInlineConstants(0, sizeof(display), display);
        commands->DrawIndirect(drawArguments);
        commands->BindIndexBuffer(indexBuffer, Format::R32_UINT, 0);
        display[2] = 1;
        commands->SetInlineConstants(0, sizeof(display), display);
        commands->DrawIndexedIndirect(drawArguments, indexedOffset);
        commands->EndRendering();
        const ResourceBarrierDesc end{nullptr, target,
            ResourceState::RenderTarget, ResourceState::Present, {}};
        commands->ResourceBarrier(&end, 1);
        check(commands->Close(), "Indirect command recording failed.");
        check(device.Submit(&commands, 1), "Indirect submission failed.");
        check(device.Present(), "Present failed.");
        ++frame;
    }
    check(device.WaitIdle(), "WaitIdle failed.");
    return 0;
}
catch(const std::exception& error)
{
    std::cerr << error.what() << '\n';
    return 1;
}
