#include "dyf/RHI.h"
#include "dyf/Platform/Window.h"

#include <vector>
#include <iterator>
#include <fstream>
#include <cstdint>
#include <memory>
#include <cstdio>
#include <string>
#include <utility>

using namespace dyf::RHI;

int main() {
    // 예제 폴더에서 실행한다. 컴파일러가 포함된 엔진은 GLSL을 컴파일하고, 그 외에는 .bin을 읽는다.
    const auto loadShader = [](dyf::RHI::ShaderStage stage, const char* sourcePath, const char* binaryPath)
    {
        bool compile = dyf::RHI::IsShaderCompilerAvailable();
        const char* path = compile ? sourcePath : binaryPath;
        std::ifstream file(path, std::ios::binary);
        if(!file && compile)
        {
            compile = false;
            path = binaryPath;
            file.open(path, std::ios::binary);
        }
        if(!file) { std::fprintf(stderr, "Cannot open shader: %s\n", path); return std::vector<uint8_t>{}; }
        const std::string data{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
        if(!compile) return std::vector<uint8_t>(data.begin(), data.end());
        auto result = dyf::RHI::CompileShader({stage, data, "main", sourcePath});
        if(!result.diagnostics.empty()) std::fprintf(stderr, "%s\n", result.diagnostics.c_str());
        return std::move(result.binary);
    };
    const auto vertexBinary = loadShader(dyf::RHI::ShaderStage::Vertex, "Shaders/vertex.glsl", "Shaders/vertex.bin");
    const auto fragmentBinary = loadShader(dyf::RHI::ShaderStage::Fragment, "Shaders/fragment.glsl", "Shaders/fragment.bin");
    const auto producerBinary = loadShader(dyf::RHI::ShaderStage::Fragment, "Shaders/producer.glsl", "Shaders/producer.bin");
#ifdef DY_EXAMPLE_METAL
    constexpr const char* vertexEntryPoint = "vertexMain";
    constexpr const char* fragmentEntryPoint = "fragmentMain";
    constexpr const char* producerEntryPoint = "producerMain";
#else
    constexpr const char* vertexEntryPoint = "main";
    constexpr const char* fragmentEntryPoint = "main";
    constexpr const char* producerEntryPoint = "main";
#endif
    if(vertexBinary.empty() || fragmentBinary.empty() || producerBinary.empty()) return 1;

    dyf::Platform::Window window(800, 600, "Advanced / 03_DepthSampling");

    DeviceDesc deviceDesc;

    std::unique_ptr<IDevice> owner(IDevice::Create(deviceDesc));

    auto& device = *owner;

    dyf::RHI::ResourceScope resources(device);

    SwapchainDesc swapchain;
    swapchain.window = window.GetHandle();
    swapchain.format = DefaultSwapchainFormat;
    swapchain.minimumImageCount = 2;

    (void)device.CreateSwapchain(swapchain);

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
        output.clearDepth = 1;

        commands->BeginRendering({nullptr, 0, &output});
        commands->BindGraphicsPipeline(producer);
        commands->SetViewport({0, 0, float(extent.width), float(extent.height), 0, 1});
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
        commands->SetViewport({0, 0, float(extent.width), float(extent.height), 0, 1});
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
