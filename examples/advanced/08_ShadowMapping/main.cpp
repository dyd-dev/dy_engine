#include "dyf/RHI.h"
#include "dyf/Platform/Window.h"
#include <iterator>
#include <fstream>
#include <array>
#include <vector>
#include <cstdint>
#include <memory>
#include <cstdio>
#include <string>
#include <utility>

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
    const auto vertexBinary = loadShader(dyf::RHI::ShaderStage::Vertex, "Shaders/vertex.glsl", "Shaders/vertex.bin");
    const auto shadowDepthBinary = loadShader(dyf::RHI::ShaderStage::Fragment, "Shaders/shadowDepth.glsl", "Shaders/shadowDepth.bin");
    const auto lightingBinary = loadShader(dyf::RHI::ShaderStage::Fragment, "Shaders/lighting.glsl", "Shaders/lighting.bin");
#ifdef DY_EXAMPLE_METAL
    constexpr const char* vertexEntryPoint = "vertexMain";
    constexpr const char* shadowDepthEntryPoint = "fragmentMain";
    constexpr const char* lightingEntryPoint = "fragmentMain";
#else
    constexpr const char* vertexEntryPoint = "main";
    constexpr const char* shadowDepthEntryPoint = "main";
    constexpr const char* lightingEntryPoint = "main";
#endif
    if(vertexBinary.empty() || shadowDepthBinary.empty() || lightingBinary.empty()) return 1;

    Params params;
    params.settings[1] = .0015f;
    dyf::Platform::Window window(960, 640, "Advanced / 08_ShadowMapping");
    std::unique_ptr<IDevice> owner(IDevice::Create({}));
    auto& device = *owner;
    ResourceScope resources(device);
    SwapchainDesc swapchain;
    swapchain.window = window.GetHandle();
    swapchain.format = DefaultSwapchainFormat;
    swapchain.minimumImageCount = 2;
    swapchain.presentMode = PresentMode::Fifo;
    (void)device.CreateSwapchain(swapchain);

    auto vertexShader = resources.Keep(device.CreateShader(
        {ShaderStage::Vertex, vertexEntryPoint, vertexBinary.data(), vertexBinary.size()}));
    auto shadowDepthShader =
        resources.Keep(device.CreateShader({ShaderStage::Fragment, shadowDepthEntryPoint,
                                            shadowDepthBinary.data(), shadowDepthBinary.size()}));
    auto lightingShader =
        resources.Keep(device.CreateShader({ShaderStage::Fragment, lightingEntryPoint,
                                            lightingBinary.data(), lightingBinary.size()}));
    SamplerDesc sampler;
    sampler.minFilter = SamplerFilter::Nearest;
    sampler.magFilter = SamplerFilter::Nearest;
    sampler.mipFilter = SamplerFilter::Nearest;
    sampler.addressU = sampler.addressV = sampler.addressW = SamplerAddressMode::ClampToEdge;
    sampler.mipLodBias = sampler.minLod = sampler.maxLod = 0;

    GraphicsPipelineDesc shadowDesc;
    shadowDesc.vertexShader = vertexShader;
    shadowDesc.fragmentShader = shadowDepthShader;
    shadowDesc.topology = PrimitiveTopology::TriangleList;
    shadowDesc.raster = {FillMode::Solid, CullMode::None, FrontFace::CounterClockwise, 0, 0, 0};
    shadowDesc.depthStencil.format = Format::D32_FLOAT;
    shadowDesc.depthStencil.depthWriteEnabled = true;
    shadowDesc.depthStencil.depthTestEnabled = true;
    shadowDesc.depthStencil.depthCompareOp = CompareOp::Always;
    auto shadowPipeline = resources.Keep(device.CreateGraphicsPipeline(shadowDesc));

    std::vector<ResourceBindingLayout> lightingBindings;
    lightingBindings.push_back(
        {0, ResourceBindingType::SampledTexture, 1, ShaderStageFlags::Fragment, {}});
    lightingBindings.push_back(
        {8, ResourceBindingType::StaticSampler, 1, ShaderStageFlags::Fragment, sampler});
    std::array<ColorAttachmentDesc, 1> lightingColors{};
    for (auto &attachment : lightingColors)
        attachment = {DefaultSwapchainFormat, {}, ColorWriteMask::All};
    GraphicsPipelineDesc lightingDesc;
    lightingDesc.vertexShader = vertexShader;
    lightingDesc.fragmentShader = lightingShader;
    lightingDesc.topology = PrimitiveTopology::TriangleList;
    lightingDesc.raster = {FillMode::Solid, CullMode::None, FrontFace::CounterClockwise, 0, 0, 0};
    lightingDesc.colorAttachments = lightingColors.data();
    lightingDesc.colorAttachmentCount = uint32_t(lightingColors.size());
    lightingDesc.layout = {lightingBindings.data(), uint32_t(lightingBindings.size()), sizeof(Params),
                           ShaderStageFlags::Fragment, 15};
    auto lightingPipeline = resources.Keep(device.CreateGraphicsPipeline(lightingDesc));

    std::unique_ptr<ResourceScope> imageResources;
    bool initializeImages = true;
    TextureHandle shadowMap = nullptr;
    ResourceSetHandle lightingSet = nullptr;
    uint32_t imageWidth = 0, imageHeight = 0;
    uint64_t frame = 0;
    while (window.IsRunning()) {
        window.PollEvents();
        if (!window.IsRunning()) break;
        if (!device.BeginFrame()) continue;
        auto backbuffer = device.GetBackBuffer();
        params.sizeTime[0] = float(backbuffer->GetDesc().width);
        params.sizeTime[1] = float(backbuffer->GetDesc().height);
        params.sizeTime[2] = float(frame) / 60.f;

        const auto dimensions = backbuffer->GetDesc();

        if (dimensions.width != imageWidth || dimensions.height != imageHeight) {
            (void)device.WaitIdle();
            imageResources = std::make_unique<ResourceScope>(device);
            imageWidth = dimensions.width;
            imageHeight = dimensions.height;
            initializeImages = true;
            TextureDesc shadowMapDesc;
            shadowMapDesc.width = 1536;
            shadowMapDesc.height = 1024;
            shadowMapDesc.format = Format::D32_FLOAT;
            shadowMapDesc.usage = TextureUsage::DepthStencil | TextureUsage::ShaderResource;
            shadowMap = imageResources->Keep(device.CreateTexture(shadowMapDesc));

            std::vector<ResourceBinding> lightingSetBindings;
            lightingSetBindings.push_back({0, 0, nullptr, shadowMap, 0, 0, {}});
            lightingSet = imageResources->Keep(device.CreateResourceSet(
                {lightingPipeline, lightingSetBindings.data(), uint32_t(lightingSetBindings.size())}));
        }

        ResourceScope frameResources(device);
        auto *commands = frameResources.Keep(device.AcquireCommandList());

        {
            const ResourceBarrierDesc shadowMapBarrier{
                nullptr, shadowMap, initializeImages ? ResourceState::Undefined : ResourceState::ShaderResource, ResourceState::DepthWrite, {}};
            commands->ResourceBarrier(&shadowMapBarrier, 1);
            DepthStencilAttachment depth;
            depth.texture = shadowMap;
            depth.state = ResourceState::DepthWrite;
            depth.depthLoadOp = LoadOp::Clear;
            depth.depthStoreOp = StoreOp::Store;
            depth.clearDepth = 1;
            commands->BeginRendering({nullptr, 0, &depth});
            commands->BindGraphicsPipeline(shadowPipeline);
            commands->SetViewport({0, 0, float(1536), float(1024), 0, 1});
            commands->SetScissor({0, 0, 1536, 1024});
            commands->DrawInstanced(3, 1, 0, 0);
            commands->EndRendering();
            const ResourceBarrierDesc shadowMapReady{
                nullptr, shadowMap, ResourceState::DepthWrite, ResourceState::ShaderResource, {}};
            commands->ResourceBarrier(&shadowMapReady, 1);
        }

        {
            const ResourceBarrierDesc backbufferBarrier{
                nullptr, backbuffer, ResourceState::Present, ResourceState::RenderTarget, {}};
            commands->ResourceBarrier(&backbufferBarrier, 1);
            std::array<ColorAttachment, 1> colors{};
            colors[0].texture = backbuffer;
            colors[0].loadOp = LoadOp::Clear;
            colors[0].storeOp = StoreOp::Store;
            commands->BeginRendering({colors.data(), uint32_t(colors.size()), nullptr});
            commands->BindGraphicsPipeline(lightingPipeline);
            commands->BindResourceSet(lightingSet);
            commands->SetViewport({0, 0, float(imageWidth), float(imageHeight), 0, 1});
            commands->SetScissor({0, 0, imageWidth, imageHeight});
            commands->SetInlineConstants(0, sizeof(params), &params);
            commands->DrawInstanced(3, 1, 0, 0);
            commands->EndRendering();
        }

        const ResourceBarrierDesc presentBarrier{
            nullptr, backbuffer, ResourceState::RenderTarget, ResourceState::Present, {}};
        commands->ResourceBarrier(&presentBarrier, 1);
        (void)commands->Close();
        (void)device.Submit(&commands, 1);
        initializeImages = false;

        (void)device.Present();
        ++frame;
    }
    (void)device.WaitIdle();
}
