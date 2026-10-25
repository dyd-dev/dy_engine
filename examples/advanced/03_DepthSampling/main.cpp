#include "dyf/RHI.h"
#include "dyf/Platform/Window.h"

#include <vector>
#include <iterator>
#include <fstream>
#include <cstdint>
#include <memory>

using namespace dyf::RHI;

int main() {
    const auto readShader = [](const char* path) {
        std::ifstream file(path, std::ios::binary);
        return std::vector<char>{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    };

    dyf::Platform::Window window(800, 600, "Advanced / 03_DepthSampling");
    std::unique_ptr<IDevice> owner(IDevice::Create({}));

    auto& device = *owner;

    dyf::RHI::ResourceScope resources(device);

    SwapchainDesc swapchain;
    swapchain.window = window.GetHandle();
    swapchain.format = DefaultSwapchainFormat;

    (void)device.CreateSwapchain(swapchain);

#ifdef __APPLE__
    constexpr const char* vertexEntryPoint = "vertexMain";
    constexpr const char* fragmentEntryPoint = "fragmentMain";
    constexpr const char* producerEntryPoint = "producerMain";
#else
    constexpr const char* vertexEntryPoint = "main";
    constexpr const char* fragmentEntryPoint = "main";
    constexpr const char* producerEntryPoint = "main";
#endif
    const auto vertexBinary = readShader("Shaders/vertex.bin");
    const auto fragmentBinary = readShader("Shaders/fragment.bin");
    const auto producerBinary = readShader("Shaders/producer.bin");

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
    desc.raster.fillMode = FillMode::Solid;
    desc.raster.cullMode = CullMode::None;
    desc.raster.frontFace = FrontFace::CounterClockwise;
    desc.colorAttachments = &colorFormat;
    desc.colorAttachmentCount = 1;
    desc.layout.inlineConstantSize = 16;
    desc.layout.inlineConstantStages = ShaderStageFlags::Vertex | ShaderStageFlags::Fragment;
    desc.layout.inlineConstantBinding = 15;

    auto* producerShader = resources.Keep(device.CreateShader({
        ShaderStage::Fragment,
        producerEntryPoint,
        producerBinary.data(),
        producerBinary.size()
    }));

    GraphicsPipelineDesc producerDesc = desc;
    producerDesc.fragmentShader = producerShader;
    producerDesc.colorAttachments = nullptr;
    producerDesc.colorAttachmentCount = 0;
    producerDesc.depthStencil.format = Format::D32_FLOAT;
    producerDesc.depthStencil.depthWriteEnabled = true;

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
            texture.format = Format::D32_FLOAT;
            texture.usage = TextureUsage::DepthStencil | TextureUsage::ShaderResource;

            intermediate = device.CreateTexture(texture);

            ResourceBinding input;
            input.binding = 0;
            input.texture = intermediate;

            bindings = device.CreateResourceSet({display, &input, 1});

            intermediateState = ResourceState::Undefined;
        }

        dyf::RHI::ResourceScope frameResources(device);
        auto* commands = frameResources.Keep(device.AcquireCommandList());

        const ResourceBarrierDesc before{nullptr, intermediate, intermediateState, ResourceState::DepthWrite, {}};
        commands->ResourceBarrier(&before, 1);

        const float nearPlane = .1f;
        const float farPlane = 100.f;
        float parameters[4] = {0, 0, nearPlane, farPlane};

        DepthStencilAttachment output;
        output.texture = intermediate;
        output.state = ResourceState::DepthWrite;
        output.depthLoadOp = LoadOp::Clear;
        output.depthStoreOp = StoreOp::Store;

        commands->BeginRendering({nullptr, 0, &output});
        commands->BindGraphicsPipeline(producer);
        commands->SetViewport({0, 0, float(extent.width), float(extent.height)});
        commands->SetScissor({0, 0, extent.width, extent.height});

        parameters[0] = 0.0f;
        commands->SetInlineConstants(0, sizeof(parameters), parameters);
        commands->DrawInstanced(3, 1, 0, 0);
        commands->EndRendering();

        const ResourceBarrierDesc ready[] = {
            {nullptr, intermediate, ResourceState::DepthWrite, ResourceState::ShaderResource, {}},
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
        commands->SetViewport({0, 0, float(extent.width), float(extent.height)});
        commands->SetScissor({0, 0, extent.width, extent.height});

        parameters[0] = 0.f;
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
