#include "dyf/Platform/Window.h"
#include "dyf/RHI.h"

#include <iterator>
#include <fstream>
#include <array>
#include <cstdint>
#include <memory>
#include <vector>

using namespace dyf::RHI;

struct alignas(16) Params {
    float screenSize[2] = {};
    float time = 0;
    float exposure = 1;
    float density = .22f;
    float heightFalloff = .7f;
    uint32_t stepCount = 64;
};

int main() {
    const auto readShader = [](const char* path) {
        std::ifstream file(path, std::ios::binary);
        return std::vector<char>{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    };

    Params params;
    dyf::Platform::Window window(960, 640, "Advanced / 13_VolumetricFog");
    std::unique_ptr<IDevice> owner(IDevice::Create({}));

    auto& device = *owner;

    dyf::RHI::ResourceScope resources(device);

    SwapchainDesc swap;
    swap.window = window.GetHandle();
    swap.format = DefaultSwapchainFormat;

    (void)device.CreateSwapchain(swap);

#ifdef __APPLE__
    constexpr const char* vertexEntryPoint = "vertexMain";
    constexpr const char* sceneEntryPoint = "fragmentMain";
    constexpr const char* fogEntryPoint = "fragmentMain";
#else
    constexpr const char* vertexEntryPoint = "main";
    constexpr const char* sceneEntryPoint = "main";
    constexpr const char* fogEntryPoint = "main";
#endif
    const auto vertexBinary = readShader("Shaders/vertex.bin");
    const auto sceneBinary = readShader("Shaders/scene.bin");
    const auto fogBinary = readShader("Shaders/fog.bin");

    auto vertexShader = resources.Keep(device.CreateShader({
        ShaderStage::Vertex,
        vertexEntryPoint,
        vertexBinary.data(),
        vertexBinary.size()
    }));
    auto sceneShader = resources.Keep(device.CreateShader({
        ShaderStage::Fragment,
        sceneEntryPoint,
        sceneBinary.data(),
        sceneBinary.size()
    }));
    auto fogShader = resources.Keep(device.CreateShader({
        ShaderStage::Fragment,
        fogEntryPoint,
        fogBinary.data(),
        fogBinary.size()
    }));

    SamplerDesc sampler;
    sampler.minFilter = SamplerFilter::Linear;
    sampler.magFilter = SamplerFilter::Linear;
    sampler.mipFilter = SamplerFilter::Nearest;
    sampler.addressU = sampler.addressV = sampler.addressW = SamplerAddressMode::ClampToEdge;
    sampler.mipLodBias = sampler.minLod = sampler.maxLod = 0;

    ColorAttachmentDesc sceneColorDesc{Format::R16G16B16A16_FLOAT, {}, ColorWriteMask::All};
    ColorAttachmentDesc fogColorDesc{DefaultSwapchainFormat, {}, ColorWriteMask::All};

    GraphicsPipelineDesc sceneDesc;
    sceneDesc.vertexShader = vertexShader;
    sceneDesc.fragmentShader = sceneShader;
    sceneDesc.topology = PrimitiveTopology::TriangleList;
    sceneDesc.raster.fillMode = FillMode::Solid;
    sceneDesc.raster.cullMode = CullMode::None;
    sceneDesc.raster.frontFace = FrontFace::CounterClockwise;
    sceneDesc.colorAttachments = &sceneColorDesc;
    sceneDesc.colorAttachmentCount = 1;
    sceneDesc.layout.inlineConstantSize = sizeof(Params);
    sceneDesc.layout.inlineConstantStages = ShaderStageFlags::Fragment;
    sceneDesc.layout.inlineConstantBinding = 15;
    auto scenePipeline = resources.Keep(device.CreateGraphicsPipeline(sceneDesc));

    std::vector<ResourceBindingLayout> fogBindings;
    fogBindings.push_back({0, ResourceBindingType::SampledTexture, 1, ShaderStageFlags::Fragment, {}});
    fogBindings.push_back({8, ResourceBindingType::StaticSampler, 1, ShaderStageFlags::Fragment, sampler});

    GraphicsPipelineDesc fogDesc;
    fogDesc.vertexShader = vertexShader;
    fogDesc.fragmentShader = fogShader;
    fogDesc.topology = PrimitiveTopology::TriangleList;
    fogDesc.raster.fillMode = FillMode::Solid;
    fogDesc.raster.cullMode = CullMode::None;
    fogDesc.raster.frontFace = FrontFace::CounterClockwise;
    fogDesc.colorAttachments = &fogColorDesc;
    fogDesc.colorAttachmentCount = 1;
    fogDesc.layout = {fogBindings.data(), uint32_t(fogBindings.size()), sizeof(Params), ShaderStageFlags::Fragment, 15};
    auto fogPipeline = resources.Keep(device.CreateGraphicsPipeline(fogDesc));

    std::unique_ptr<dyf::RHI::ResourceScope> imageResources;
    uint32_t imageWidth = 0;
    uint32_t imageHeight = 0;
    bool initializeImages = true;

    TextureHandle sceneDepth = nullptr;
    ResourceSetHandle fogSet = nullptr;

    uint64_t frame = 0;

    while (window.IsRunning()) {
        window.PollEvents();
        if (!window.IsRunning()) {
            break;
        }

        if (!device.BeginFrame()) continue;

        auto backbuffer = device.GetBackBuffer();

        const auto dimensions = backbuffer->GetDesc();

        if (dimensions.width != imageWidth || dimensions.height != imageHeight) {
            (void)device.WaitIdle();
            imageResources = std::make_unique<dyf::RHI::ResourceScope>(device);
            imageWidth = dimensions.width;
            imageHeight = dimensions.height;
            initializeImages = true;

            TextureDesc sceneDepthDesc;
            sceneDepthDesc.width = imageWidth;
            sceneDepthDesc.height = imageHeight;
            sceneDepthDesc.format = Format::R16G16B16A16_FLOAT;
            sceneDepthDesc.usage = TextureUsage::RenderTarget | TextureUsage::ShaderResource;
            sceneDepth = imageResources->Keep(device.CreateTexture(sceneDepthDesc));

            std::vector<ResourceBinding> fogSetBindings;
            fogSetBindings.push_back({0, 0, nullptr, sceneDepth, 0, 0, {}});
            fogSet = imageResources->Keep(device.CreateResourceSet({fogPipeline, fogSetBindings.data(), uint32_t(fogSetBindings.size())}));
        }

        params.screenSize[0] = float(imageWidth);
        params.screenSize[1] = float(imageHeight);
        params.time = float(frame) / 60.0f;

        dyf::RHI::ResourceScope frameResources(device);
        auto* commands = frameResources.Keep(device.AcquireCommandList());

        {
            const ResourceBarrierDesc sceneDepthBarrier{nullptr, sceneDepth, initializeImages ? ResourceState::Undefined : ResourceState::ShaderResource, ResourceState::RenderTarget, {}};
            commands->ResourceBarrier(&sceneDepthBarrier, 1);

            std::array<ColorAttachment, 1> colors{};
            colors[0].texture = sceneDepth;
            colors[0].loadOp = LoadOp::Clear;
            colors[0].storeOp = StoreOp::Store;

            commands->BeginRendering({colors.data(), uint32_t(colors.size()), nullptr});
            commands->BindGraphicsPipeline(scenePipeline);
            commands->SetViewport({0, 0, float(imageWidth), float(imageHeight)});
            commands->SetScissor({0, 0, imageWidth, imageHeight});
            commands->SetInlineConstants(0, sizeof(params), &params);
            commands->DrawInstanced(3, 1, 0, 0);
            commands->EndRendering();

            const ResourceBarrierDesc sceneDepthReady{nullptr, sceneDepth, ResourceState::RenderTarget, ResourceState::ShaderResource, {}};
            commands->ResourceBarrier(&sceneDepthReady, 1);
        }

        {
            const ResourceBarrierDesc backbufferBarrier{nullptr, backbuffer, ResourceState::Present, ResourceState::RenderTarget, {}};
            commands->ResourceBarrier(&backbufferBarrier, 1);

            std::array<ColorAttachment, 1> colors{};
            colors[0].texture = backbuffer;
            colors[0].loadOp = LoadOp::Clear;
            colors[0].storeOp = StoreOp::Store;

            commands->BeginRendering({colors.data(), uint32_t(colors.size()), nullptr});
            commands->BindGraphicsPipeline(fogPipeline);
            commands->BindResourceSet(fogSet);
            commands->SetViewport({0, 0, float(imageWidth), float(imageHeight)});
            commands->SetScissor({0, 0, imageWidth, imageHeight});
            commands->SetInlineConstants(0, sizeof(params), &params);
            commands->DrawInstanced(3, 1, 0, 0);
            commands->EndRendering();
        }

        const ResourceBarrierDesc presentBarrier{nullptr, backbuffer, ResourceState::RenderTarget, ResourceState::Present, {}};
        commands->ResourceBarrier(&presentBarrier, 1);

        (void)commands->Close();
        (void)device.Submit(&commands, 1);

        initializeImages = false;
        ++frame;

        (void)device.Present();
    }

    (void)device.WaitIdle();
    return 0;

}
