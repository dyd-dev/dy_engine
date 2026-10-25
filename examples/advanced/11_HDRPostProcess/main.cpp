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
    float exposure = 1;
    float threshold = 1;
    float bloomStrength = .35f;
    float emissiveIntensity = 12;
};

int main() {
    const auto readShader = [](const char* path) {
        std::ifstream file(path, std::ios::binary);
        return std::vector<char>{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    };

    Params params;
    dyf::Platform::Window window(960, 640, "Advanced / 11_HDRPostProcess");
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
    constexpr const char* brightEntryPoint = "fragmentMain";
    constexpr const char* blurEntryPoint = "fragmentMain";
    constexpr const char* tonemapEntryPoint = "fragmentMain";
#else
    constexpr const char* vertexEntryPoint = "main";
    constexpr const char* sceneEntryPoint = "main";
    constexpr const char* brightEntryPoint = "main";
    constexpr const char* blurEntryPoint = "main";
    constexpr const char* tonemapEntryPoint = "main";
#endif
    const auto vertexBinary = readShader("Shaders/vertex.bin");
    const auto sceneBinary = readShader("Shaders/scene.bin");
    const auto brightBinary = readShader("Shaders/bright.bin");
    const auto blurBinary = readShader("Shaders/blur.bin");
    const auto tonemapBinary = readShader("Shaders/tonemap.bin");

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
    auto brightShader = resources.Keep(device.CreateShader({
        ShaderStage::Fragment,
        brightEntryPoint,
        brightBinary.data(),
        brightBinary.size()
    }));
    auto blurShader = resources.Keep(device.CreateShader({
        ShaderStage::Fragment,
        blurEntryPoint,
        blurBinary.data(),
        blurBinary.size()
    }));
    auto tonemapShader = resources.Keep(device.CreateShader({
        ShaderStage::Fragment,
        tonemapEntryPoint,
        tonemapBinary.data(),
        tonemapBinary.size()
    }));

    SamplerDesc sampler;
    sampler.minFilter = SamplerFilter::Linear;
    sampler.magFilter = SamplerFilter::Linear;
    sampler.mipFilter = SamplerFilter::Nearest;
    sampler.addressU = sampler.addressV = sampler.addressW = SamplerAddressMode::ClampToEdge;
    sampler.mipLodBias = sampler.minLod = sampler.maxLod = 0;

    ColorAttachmentDesc hdrColorDesc{Format::R16G16B16A16_FLOAT, {}, ColorWriteMask::All};
    ColorAttachmentDesc ldrColorDesc{DefaultSwapchainFormat, {}, ColorWriteMask::All};

    GraphicsPipelineDesc sceneDesc;
    sceneDesc.vertexShader = vertexShader;
    sceneDesc.fragmentShader = sceneShader;
    sceneDesc.topology = PrimitiveTopology::TriangleList;
    sceneDesc.raster.fillMode = FillMode::Solid;
    sceneDesc.raster.cullMode = CullMode::None;
    sceneDesc.raster.frontFace = FrontFace::CounterClockwise;
    sceneDesc.colorAttachments = &hdrColorDesc;
    sceneDesc.colorAttachmentCount = 1;
    sceneDesc.layout.inlineConstantSize = sizeof(Params);
    sceneDesc.layout.inlineConstantStages = ShaderStageFlags::Fragment;
    sceneDesc.layout.inlineConstantBinding = 15;
    auto scenePipeline = resources.Keep(device.CreateGraphicsPipeline(sceneDesc));

    std::vector<ResourceBindingLayout> brightBindings;
    brightBindings.push_back({0, ResourceBindingType::SampledTexture, 1, ShaderStageFlags::Fragment, {}});
    brightBindings.push_back({8, ResourceBindingType::StaticSampler, 1, ShaderStageFlags::Fragment, sampler});

    GraphicsPipelineDesc brightDesc;
    brightDesc.vertexShader = vertexShader;
    brightDesc.fragmentShader = brightShader;
    brightDesc.topology = PrimitiveTopology::TriangleList;
    brightDesc.raster.fillMode = FillMode::Solid;
    brightDesc.raster.cullMode = CullMode::None;
    brightDesc.raster.frontFace = FrontFace::CounterClockwise;
    brightDesc.colorAttachments = &hdrColorDesc;
    brightDesc.colorAttachmentCount = 1;
    brightDesc.layout = {brightBindings.data(), uint32_t(brightBindings.size()), sizeof(Params), ShaderStageFlags::Fragment, 15};
    auto brightPipeline = resources.Keep(device.CreateGraphicsPipeline(brightDesc));

    std::vector<ResourceBindingLayout> blurBindings;
    blurBindings.push_back({0, ResourceBindingType::SampledTexture, 1, ShaderStageFlags::Fragment, {}});
    blurBindings.push_back({8, ResourceBindingType::StaticSampler, 1, ShaderStageFlags::Fragment, sampler});

    GraphicsPipelineDesc blurDesc;
    blurDesc.vertexShader = vertexShader;
    blurDesc.fragmentShader = blurShader;
    blurDesc.topology = PrimitiveTopology::TriangleList;
    blurDesc.raster.fillMode = FillMode::Solid;
    blurDesc.raster.cullMode = CullMode::None;
    blurDesc.raster.frontFace = FrontFace::CounterClockwise;
    blurDesc.colorAttachments = &hdrColorDesc;
    blurDesc.colorAttachmentCount = 1;
    blurDesc.layout = {blurBindings.data(), uint32_t(blurBindings.size()), sizeof(Params), ShaderStageFlags::Fragment, 15};
    auto blurPipeline = resources.Keep(device.CreateGraphicsPipeline(blurDesc));

    std::vector<ResourceBindingLayout> tonemapBindings;
    tonemapBindings.push_back({0, ResourceBindingType::SampledTexture, 1, ShaderStageFlags::Fragment, {}});
    tonemapBindings.push_back({1, ResourceBindingType::SampledTexture, 1, ShaderStageFlags::Fragment, {}});
    tonemapBindings.push_back({8, ResourceBindingType::StaticSampler, 1, ShaderStageFlags::Fragment, sampler});

    GraphicsPipelineDesc tonemapDesc;
    tonemapDesc.vertexShader = vertexShader;
    tonemapDesc.fragmentShader = tonemapShader;
    tonemapDesc.topology = PrimitiveTopology::TriangleList;
    tonemapDesc.raster.fillMode = FillMode::Solid;
    tonemapDesc.raster.cullMode = CullMode::None;
    tonemapDesc.raster.frontFace = FrontFace::CounterClockwise;
    tonemapDesc.colorAttachments = &ldrColorDesc;
    tonemapDesc.colorAttachmentCount = 1;
    tonemapDesc.layout = {tonemapBindings.data(), uint32_t(tonemapBindings.size()), sizeof(Params), ShaderStageFlags::Fragment, 15};
    auto tonemapPipeline = resources.Keep(device.CreateGraphicsPipeline(tonemapDesc));

    std::unique_ptr<dyf::RHI::ResourceScope> imageResources;
    uint32_t imageWidth = 0;
    uint32_t imageHeight = 0;
    bool initializeImages = true;

    TextureHandle hdrScene = nullptr;
    TextureHandle bloomHorizontal = nullptr;
    TextureHandle bloomVertical = nullptr;
    ResourceSetHandle brightSet = nullptr;
    ResourceSetHandle blurSet = nullptr;
    ResourceSetHandle tonemapSet = nullptr;

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

            TextureDesc hdrSceneDesc;
            hdrSceneDesc.width = imageWidth;
            hdrSceneDesc.height = imageHeight;
            hdrSceneDesc.format = Format::R16G16B16A16_FLOAT;
            hdrSceneDesc.usage = TextureUsage::RenderTarget | TextureUsage::ShaderResource;
            hdrScene = imageResources->Keep(device.CreateTexture(hdrSceneDesc));

            TextureDesc bloomHorizontalDesc;
            bloomHorizontalDesc.width = imageWidth;
            bloomHorizontalDesc.height = imageHeight;
            bloomHorizontalDesc.format = Format::R16G16B16A16_FLOAT;
            bloomHorizontalDesc.usage = TextureUsage::RenderTarget | TextureUsage::ShaderResource;
            bloomHorizontal = imageResources->Keep(device.CreateTexture(bloomHorizontalDesc));

            TextureDesc bloomVerticalDesc;
            bloomVerticalDesc.width = imageWidth;
            bloomVerticalDesc.height = imageHeight;
            bloomVerticalDesc.format = Format::R16G16B16A16_FLOAT;
            bloomVerticalDesc.usage = TextureUsage::RenderTarget | TextureUsage::ShaderResource;
            bloomVertical = imageResources->Keep(device.CreateTexture(bloomVerticalDesc));

            std::vector<ResourceBinding> brightSetBindings;
            brightSetBindings.push_back({0, 0, nullptr, hdrScene, 0, 0, {}});
            brightSet = imageResources->Keep(device.CreateResourceSet({brightPipeline, brightSetBindings.data(), uint32_t(brightSetBindings.size())}));

            std::vector<ResourceBinding> blurSetBindings;
            blurSetBindings.push_back({0, 0, nullptr, bloomHorizontal, 0, 0, {}});
            blurSet = imageResources->Keep(device.CreateResourceSet({blurPipeline, blurSetBindings.data(), uint32_t(blurSetBindings.size())}));

            std::vector<ResourceBinding> tonemapSetBindings;
            tonemapSetBindings.push_back({0, 0, nullptr, hdrScene, 0, 0, {}});
            tonemapSetBindings.push_back({1, 0, nullptr, bloomVertical, 0, 0, {}});
            tonemapSet = imageResources->Keep(device.CreateResourceSet({tonemapPipeline, tonemapSetBindings.data(), uint32_t(tonemapSetBindings.size())}));
        }

        params.screenSize[0] = float(imageWidth);
        params.screenSize[1] = float(imageHeight);

        dyf::RHI::ResourceScope frameResources(device);
        auto* commands = frameResources.Keep(device.AcquireCommandList());

        {
            const ResourceBarrierDesc hdrSceneBarrier{nullptr, hdrScene, initializeImages ? ResourceState::Undefined : ResourceState::ShaderResource, ResourceState::RenderTarget, {}};
            commands->ResourceBarrier(&hdrSceneBarrier, 1);

            std::array<ColorAttachment, 1> colors{};
            colors[0].texture = hdrScene;
            colors[0].loadOp = LoadOp::Clear;
            colors[0].storeOp = StoreOp::Store;

            commands->BeginRendering({colors.data(), uint32_t(colors.size()), nullptr});
            commands->BindGraphicsPipeline(scenePipeline);
            commands->SetViewport({0, 0, float(imageWidth), float(imageHeight)});
            commands->SetScissor({0, 0, imageWidth, imageHeight});
            commands->SetInlineConstants(0, sizeof(params), &params);
            commands->DrawInstanced(3, 1, 0, 0);
            commands->EndRendering();

            const ResourceBarrierDesc hdrSceneReady{nullptr, hdrScene, ResourceState::RenderTarget, ResourceState::ShaderResource, {}};
            commands->ResourceBarrier(&hdrSceneReady, 1);
        }

        {
            const ResourceBarrierDesc bloomHorizontalBarrier{nullptr, bloomHorizontal, initializeImages ? ResourceState::Undefined : ResourceState::ShaderResource, ResourceState::RenderTarget, {}};
            commands->ResourceBarrier(&bloomHorizontalBarrier, 1);

            std::array<ColorAttachment, 1> colors{};
            colors[0].texture = bloomHorizontal;
            colors[0].loadOp = LoadOp::Clear;
            colors[0].storeOp = StoreOp::Store;

            commands->BeginRendering({colors.data(), uint32_t(colors.size()), nullptr});
            commands->BindGraphicsPipeline(brightPipeline);
            commands->BindResourceSet(brightSet);
            commands->SetViewport({0, 0, float(imageWidth), float(imageHeight)});
            commands->SetScissor({0, 0, imageWidth, imageHeight});
            commands->SetInlineConstants(0, sizeof(params), &params);
            commands->DrawInstanced(3, 1, 0, 0);
            commands->EndRendering();

            const ResourceBarrierDesc bloomHorizontalReady{nullptr, bloomHorizontal, ResourceState::RenderTarget, ResourceState::ShaderResource, {}};
            commands->ResourceBarrier(&bloomHorizontalReady, 1);
        }

        {
            const ResourceBarrierDesc bloomVerticalBarrier{nullptr, bloomVertical, initializeImages ? ResourceState::Undefined : ResourceState::ShaderResource, ResourceState::RenderTarget, {}};
            commands->ResourceBarrier(&bloomVerticalBarrier, 1);

            std::array<ColorAttachment, 1> colors{};
            colors[0].texture = bloomVertical;
            colors[0].loadOp = LoadOp::Clear;
            colors[0].storeOp = StoreOp::Store;

            commands->BeginRendering({colors.data(), uint32_t(colors.size()), nullptr});
            commands->BindGraphicsPipeline(blurPipeline);
            commands->BindResourceSet(blurSet);
            commands->SetViewport({0, 0, float(imageWidth), float(imageHeight)});
            commands->SetScissor({0, 0, imageWidth, imageHeight});
            commands->SetInlineConstants(0, sizeof(params), &params);
            commands->DrawInstanced(3, 1, 0, 0);
            commands->EndRendering();

            const ResourceBarrierDesc bloomVerticalReady{nullptr, bloomVertical, ResourceState::RenderTarget, ResourceState::ShaderResource, {}};
            commands->ResourceBarrier(&bloomVerticalReady, 1);
        }

        {
            const ResourceBarrierDesc backbufferBarrier{nullptr, backbuffer, ResourceState::Present, ResourceState::RenderTarget, {}};
            commands->ResourceBarrier(&backbufferBarrier, 1);

            std::array<ColorAttachment, 1> colors{};
            colors[0].texture = backbuffer;
            colors[0].loadOp = LoadOp::Clear;
            colors[0].storeOp = StoreOp::Store;

            commands->BeginRendering({colors.data(), uint32_t(colors.size()), nullptr});
            commands->BindGraphicsPipeline(tonemapPipeline);
            commands->BindResourceSet(tonemapSet);
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

        (void)device.Present();
    }

    (void)device.WaitIdle();
    return 0;

}
