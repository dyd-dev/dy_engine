#include "dyf/RHI.h"
#include "dyf/Platform/Window.h"

#include <vector>
#include <iterator>
#include <fstream>
#include <cstdint>
#include <memory>

using namespace dyf::RHI;

int main() {
    dyf::Platform::Window window(800, 600, "Advanced / 05_ResourceBarriers");

    DeviceDesc deviceDesc;
    deviceDesc.maxFramesInFlight = 2;

    std::unique_ptr<IDevice> owner(IDevice::Create(deviceDesc));

    auto& device = *owner;

    dyf::RHI::ResourceScope resources(device);

    SwapchainDesc swapchain;
    swapchain.window = window.GetHandle();
    swapchain.format = DefaultSwapchainFormat;
    swapchain.minimumImageCount = 2;

    (void)device.CreateSwapchain(swapchain);

    // 예제 폴더에서 연결한 엔진의 백엔드에 맞는 명령으로 셰이더를 컴파일한다.
    // Metal의 macosx SDK는 대상에 따라 iphoneos 또는 iphonesimulator로 바꾼다.
    // Vulkan: glslc -fshader-stage=vert Shaders/vertex.glsl -o Shaders/vertex.bin
    // D3D12: dxc -T vs_6_0 -E main Shaders/vertex.hlsl -Fo Shaders/vertex.bin
    // Metal: xcrun -sdk macosx metal -c Shaders/program.metal -o Shaders/vertex.air
    //        xcrun -sdk macosx metallib Shaders/vertex.air -o Shaders/vertex.bin
    std::ifstream vertexFile("Shaders/vertex.bin", std::ios::binary);
    const std::vector<char> vertexBinary{
        std::istreambuf_iterator<char>(vertexFile), std::istreambuf_iterator<char>()};
    // Vulkan: glslc -fshader-stage=frag Shaders/fragment.glsl -o Shaders/fragment.bin
    // D3D12: dxc -T ps_6_0 -E main Shaders/fragment.hlsl -Fo Shaders/fragment.bin
    // Metal: xcrun -sdk macosx metal -c Shaders/program.metal -o Shaders/fragment.air
    //        xcrun -sdk macosx metallib Shaders/fragment.air -o Shaders/fragment.bin
    std::ifstream fragmentFile("Shaders/fragment.bin", std::ios::binary);
    const std::vector<char> fragmentBinary{
        std::istreambuf_iterator<char>(fragmentFile), std::istreambuf_iterator<char>()};
    // Vulkan: glslc -fshader-stage=frag Shaders/producer.glsl -o Shaders/producer.bin
    // D3D12: dxc -T ps_6_0 -E main Shaders/producer.hlsl -Fo Shaders/producer.bin
    // Metal: xcrun -sdk macosx metal -c Shaders/program.metal -o Shaders/producer.air
    //        xcrun -sdk macosx metallib Shaders/producer.air -o Shaders/producer.bin
    std::ifstream producerFile("Shaders/producer.bin", std::ios::binary);
    const std::vector<char> producerBinary{
        std::istreambuf_iterator<char>(producerFile), std::istreambuf_iterator<char>()};
#ifdef DY_EXAMPLE_METAL
    constexpr const char* vertexEntryPoint = "vertexMain";
    constexpr const char* fragmentEntryPoint = "fragmentMain";
    constexpr const char* producerEntryPoint = "producerMain";
#else
    constexpr const char* vertexEntryPoint = "main";
    constexpr const char* fragmentEntryPoint = "main";
    constexpr const char* producerEntryPoint = "main";
#endif

    auto* vertex = resources.Keep(device.CreateShader({
        ShaderStage::Vertex,
        vertexEntryPoint,
        vertexBinary.data(),
        vertexBinary.size()
    }));
    auto* fragment = resources.Keep(device.CreateShader({
        ShaderStage::Fragment,
        fragmentEntryPoint,
        fragmentBinary.data(),
        fragmentBinary.size()
    }));

    ColorAttachmentDesc colorFormat{swapchain.format, {}, ColorWriteMask::All};

    GraphicsPipelineDesc desc;
    desc.vertexShader = vertex;
    desc.fragmentShader = fragment;
    desc.topology = PrimitiveTopology::TriangleList;
    desc.raster = {FillMode::Solid, CullMode::None, FrontFace::CounterClockwise, 0, 0, 0};
    desc.colorAttachments = &colorFormat;
    desc.colorAttachmentCount = 1;
    desc.layout = {nullptr, 0, 16, ShaderStageFlags::Vertex | ShaderStageFlags::Fragment, 15};

    auto* producerShader = resources.Keep(device.CreateShader({
        ShaderStage::Fragment,
        producerEntryPoint,
        producerBinary.data(),
        producerBinary.size()
    }));

    GraphicsPipelineDesc producerDesc = desc;
    producerDesc.fragmentShader = producerShader;
    ColorAttachmentDesc intermediateFormat{Format::R8G8B8A8_UNORM, {}, ColorWriteMask::All};
    producerDesc.colorAttachments = &intermediateFormat;

    auto* producer = resources.Keep(device.CreateGraphicsPipeline(producerDesc));

    SamplerDesc sampler;
    sampler.minFilter = sampler.magFilter = sampler.mipFilter = SamplerFilter::Nearest;
    sampler.addressU = sampler.addressV = sampler.addressW = SamplerAddressMode::ClampToEdge;
    sampler.mipLodBias = sampler.minLod = sampler.maxLod = 0;

    const ResourceBindingLayout layout[] = {
        {0, ResourceBindingType::SampledTexture, 1, ShaderStageFlags::Fragment, {}},
        {1, ResourceBindingType::StaticSampler, 1, ShaderStageFlags::Fragment, sampler}
    };
    desc.layout.bindings = layout;
    desc.layout.bindingCount = 2;

    auto* display = resources.Keep(device.CreateGraphicsPipeline(desc));

    TextureHandle intermediate = nullptr;
    ResourceSetHandle bindings = nullptr;
    ResourceState intermediateState = ResourceState::Undefined;

    while (window.IsRunning()) {
        window.PollEvents();
        if (!window.IsRunning()) {
            break;
        }

        if (!device.BeginFrame()) continue;

        auto* target = device.GetBackBuffer();
        const auto& extent = target->GetDesc();

        if (!intermediate || intermediate->GetDesc().width != extent.width || intermediate->GetDesc().height != extent.height) {
            if (bindings) {
                device.DestroyResourceSet(bindings);
            }
            if (intermediate) {
                device.DestroyTexture(intermediate);
            }

            TextureDesc texture;
            texture.width = extent.width;
            texture.height = extent.height;
            texture.format = Format::R8G8B8A8_UNORM;
            texture.usage = TextureUsage::RenderTarget | TextureUsage::ShaderResource;

            intermediate = device.CreateTexture(texture);

            ResourceBinding input;
            input.binding = 0;
            input.texture = intermediate;

            bindings = device.CreateResourceSet({display, &input, 1});

            intermediateState = ResourceState::Undefined;
        }

        dyf::RHI::ResourceScope frameResources(device);
        auto* commands = frameResources.Keep(device.AcquireCommandList());

        const ResourceBarrierDesc before{nullptr, intermediate, intermediateState, ResourceState::RenderTarget, {}};
        commands->ResourceBarrier(&before, 1);

        float parameters[4] = {0, 0, 0, 0};

        for (uint32_t pass = 0; pass < 2; ++pass) {
            if (pass) {
                const ResourceBarrierDesc writeAfterWrite{nullptr, intermediate, ResourceState::RenderTarget, ResourceState::RenderTarget, {}};
                commands->ResourceBarrier(&writeAfterWrite, 1);
            }

            ColorAttachment output;
            output.texture = intermediate;
            output.loadOp = pass ? LoadOp::Load : LoadOp::Clear;
            output.storeOp = StoreOp::Store;
            output.clearColor[3] = 1;

            commands->BeginRendering({&output, 1, nullptr});
            commands->BindGraphicsPipeline(producer);
            commands->SetViewport({0, 0, float(extent.width), float(extent.height), 0, 1});
            commands->SetScissor({0, 0, extent.width, extent.height});

            parameters[0] = float(pass);
            commands->SetInlineConstants(0, sizeof(parameters), parameters);
            commands->DrawInstanced(3, 1, 0, 0);
            commands->EndRendering();

        }

        const ResourceBarrierDesc ready[] = {
            {nullptr, intermediate, ResourceState::RenderTarget, ResourceState::ShaderResource, {}},
            {nullptr, target, ResourceState::Present, ResourceState::RenderTarget, {}}
        };
        commands->ResourceBarrier(ready, 2);

        ColorAttachment color;
        color.texture = target;
        color.loadOp = LoadOp::Discard;
        color.storeOp = StoreOp::Store;

        commands->BeginRendering({&color, 1, nullptr});
        commands->BindGraphicsPipeline(display);
        commands->BindResourceSet(bindings);
        commands->SetViewport({0, 0, float(extent.width), float(extent.height), 0, 1});
        commands->SetScissor({0, 0, extent.width, extent.height});

        parameters[0] = 0;
        commands->SetInlineConstants(0, sizeof(parameters), parameters);
        commands->DrawInstanced(3, 1, 0, 0);
        commands->EndRendering();

        ResourceBarrierDesc after{nullptr, target, ResourceState::RenderTarget, ResourceState::Present, {}};
        commands->ResourceBarrier(&after, 1);

        (void)commands->Close();
        (void)device.Submit(&commands, 1);
        intermediateState = ResourceState::ShaderResource;

        (void)device.Present();
    }

    if (bindings) {
        device.DestroyResourceSet(bindings);
    }
    if (intermediate) {
        device.DestroyTexture(intermediate);
    }

    (void)device.WaitIdle();
    return 0;

}
