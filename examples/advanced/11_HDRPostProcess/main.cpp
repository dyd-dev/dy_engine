#include "dyf/Platform/Window.h"
#include "dyf/RHI.h"
#include <cstdio>
#include <string>
#include <utility>

#include <iterator>
#include <fstream>
#include <array>
#include <cstdint>
#include <memory>
#include <vector>

using namespace dyf::RHI;

struct alignas(16) Params {
    float sizeTime[4] = {960, 640, 0, 0};
    float settings[4] = {1, .45f, .8f, .9f};
    float jitter[4] = {};
    float extra[4] = {};
};

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

    Params params;
    params.settings[1] = 1;
    params.settings[2] = .35f;
    params.extra[0] = 12;
    dyf::Platform::Window window(960, 640, "Advanced / 11_HDRPostProcess");

    DeviceDesc deviceDesc;

    std::unique_ptr<IDevice> owner(IDevice::Create(deviceDesc));

    auto& device = *owner;

    dyf::RHI::ResourceScope resources(device);

    SwapchainDesc swap;
    swap.window = window.GetHandle();
    swap.format = DefaultSwapchainFormat;
    swap.minimumImageCount = 2;
    swap.presentMode = PresentMode::Fifo;

    (void)device.CreateSwapchain(swap);

    const auto vertexBinary = loadShader(ShaderStage::Vertex, "Shaders/vertex.glsl", "Shaders/vertex.bin");
    const auto sceneBinary = loadShader(ShaderStage::Fragment, "Shaders/scene.glsl", "Shaders/scene.bin");
    const auto brightBinary = loadShader(ShaderStage::Fragment, "Shaders/bright.glsl", "Shaders/bright.bin");
    const auto blurBinary = loadShader(ShaderStage::Fragment, "Shaders/blur.glsl", "Shaders/blur.bin");
    const auto tonemapBinary = loadShader(ShaderStage::Fragment, "Shaders/tonemap.glsl", "Shaders/tonemap.bin");
#ifdef DY_EXAMPLE_METAL
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
    if(vertexBinary.empty() || sceneBinary.empty() || brightBinary.empty() ||
        blurBinary.empty() || tonemapBinary.empty()) return 1;

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
    sceneDesc.raster = {FillMode::Solid, CullMode::None, FrontFace::CounterClockwise, 0, 0, 0};
    sceneDesc.colorAttachments = &hdrColorDesc;
    sceneDesc.colorAttachmentCount = 1;
    sceneDesc.layout = {nullptr, 0, sizeof(Params), ShaderStageFlags::Fragment, 15};
    auto scenePipeline = resources.Keep(device.CreateGraphicsPipeline(sceneDesc));

    std::vector<ResourceBindingLayout> brightBindings;
    brightBindings.push_back({0, ResourceBindingType::SampledTexture, 1, ShaderStageFlags::Fragment, {}});
    brightBindings.push_back({8, ResourceBindingType::StaticSampler, 1, ShaderStageFlags::Fragment, sampler});

    GraphicsPipelineDesc brightDesc;
    brightDesc.vertexShader = vertexShader;
    brightDesc.fragmentShader = brightShader;
    brightDesc.topology = PrimitiveTopology::TriangleList;
    brightDesc.raster = {FillMode::Solid, CullMode::None, FrontFace::CounterClockwise, 0, 0, 0};
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
    blurDesc.raster = {FillMode::Solid, CullMode::None, FrontFace::CounterClockwise, 0, 0, 0};
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
    tonemapDesc.raster = {FillMode::Solid, CullMode::None, FrontFace::CounterClockwise, 0, 0, 0};
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

        params.sizeTime[0] = float(imageWidth);
        params.sizeTime[1] = float(imageHeight);
        params.sizeTime[2] = float(frame) / 60.0f;

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
            commands->SetViewport({0, 0, float(imageWidth), float(imageHeight), 0, 1});
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
            commands->SetViewport({0, 0, float(imageWidth), float(imageHeight), 0, 1});
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
            commands->SetViewport({0, 0, float(imageWidth), float(imageHeight), 0, 1});
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
            commands->SetViewport({0, 0, float(imageWidth), float(imageHeight), 0, 1});
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
