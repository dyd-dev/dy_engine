#include "dyf/Platform/Window.h"
#include "dyf/RHI.h"

#include <fstream>
#include <iterator>
#include <chrono>
#include <cstdint>
#include <memory>
#include <vector>

using namespace dyf::RHI;

int main()
{
    // Vulkan: glslc -fshader-stage=comp Shaders/generate.glsl -o Shaders/generate.bin
    // D3D12: dxc -T cs_6_0 -E main Shaders/generate.hlsl -Fo Shaders/generate.bin
    // Metal: xcrun -sdk macosx metal -c Shaders/program.metal -o Shaders/generate.air
    //        xcrun -sdk macosx metallib Shaders/generate.air -o Shaders/generate.bin
    std::ifstream generateFile("Shaders/generate.bin",std::ios::binary);
    const std::vector<char> generateBinary{
        std::istreambuf_iterator<char>(generateFile),std::istreambuf_iterator<char>()};
#ifdef DY_EXAMPLE_METAL
    const char* generateEntry="generateArguments";
#else
    const char* generateEntry="main";
#endif
    // Vulkan: glslc -fshader-stage=vert Shaders/vertex.glsl -o Shaders/vertex.bin
    // D3D12: dxc -T vs_6_0 -E main Shaders/vertex.hlsl -Fo Shaders/vertex.bin
    // Metal: xcrun -sdk macosx metal -c Shaders/program.metal -o Shaders/vertex.air
    //        xcrun -sdk macosx metallib Shaders/vertex.air -o Shaders/vertex.bin
    std::ifstream vertexFile("Shaders/vertex.bin",std::ios::binary);
    const std::vector<char> vertexBinary{
        std::istreambuf_iterator<char>(vertexFile),std::istreambuf_iterator<char>()};
#ifdef DY_EXAMPLE_METAL
    const char* vertexEntry="vertexMain";
#else
    const char* vertexEntry="main";
#endif
    // Vulkan: glslc -fshader-stage=frag Shaders/fragment.glsl -o Shaders/fragment.bin
    // D3D12: dxc -T ps_6_0 -E main Shaders/fragment.hlsl -Fo Shaders/fragment.bin
    // Metal: xcrun -sdk macosx metal -c Shaders/program.metal -o Shaders/fragment.air
    //        xcrun -sdk macosx metallib Shaders/fragment.air -o Shaders/fragment.bin
    std::ifstream fragmentFile("Shaders/fragment.bin",std::ios::binary);
    const std::vector<char> fragmentBinary{
        std::istreambuf_iterator<char>(fragmentFile),std::istreambuf_iterator<char>()};
#ifdef DY_EXAMPLE_METAL
    const char* fragmentEntry="fragmentMain";
#else
    const char* fragmentEntry="main";
#endif

    dyf::Platform::Window window(1000, 650, "Advanced / GPU-generated indirect drawing");
    
    std::unique_ptr<IDevice> owner(IDevice::Create({}));
    
    auto& device = *owner;
    dyf::RHI::ResourceScope resources(device);
    SwapchainDesc swapchain;
    swapchain.window = window.GetHandle();
    swapchain.format = DefaultSwapchainFormat;
    (void)device.CreateSwapchain(swapchain);

    auto* cs = resources.Keep(device.CreateShader({ShaderStage::Compute,
        generateEntry, generateBinary.data(), generateBinary.size()}));
    auto* vs = resources.Keep(device.CreateShader({ShaderStage::Vertex,
        vertexEntry, vertexBinary.data(), vertexBinary.size()}));
    auto* fs = resources.Keep(device.CreateShader({ShaderStage::Fragment,
        fragmentEntry, fragmentBinary.data(), fragmentBinary.size()}));

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

    const auto start = std::chrono::steady_clock::now();
    uint32_t frame = 0;
    while(window.IsRunning())
    {
        window.PollEvents();
        if(!window.IsRunning()) break;
        if(!device.BeginFrame())
        {
            
            continue;
        }
        dyf::RHI::ResourceScope frameResources(device);
        auto* commands = frameResources.Keep(device.AcquireCommandList());
        if(frame == 0)
        {
            const DispatchIndirectArguments dispatch{1, 1, 1};
            (void)device.UpdateBuffer(*commands, dispatchArguments, 0, &dispatch, sizeof(dispatch));
            (void)device.UpdateBuffer(*commands, indexBuffer, 0, indices, sizeof(indices));
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
        (void)commands->Close();
        (void)device.Submit(&commands, 1);
        (void)device.Present();
        ++frame;
    }
    (void)device.WaitIdle();
    return 0;
}
