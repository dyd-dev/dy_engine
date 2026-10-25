#include "dyf/RHI.h"
#include "dyf/Platform/Window.h"
#include <iterator>
#include <fstream>
#include <array>
#include <vector>
#include <cstdint>
#include <memory>

using namespace dyf::RHI;

struct alignas(16) Params {
    float screenSize[2] = {};
    float exposure = 1;
    float shadowBias = .0015f;
};

int main() {
    const auto readShader = [](const char* path) {
        std::ifstream file(path, std::ios::binary);
        return std::vector<char>{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    };

    Params params;
    dyf::Platform::Window window(960, 640, "Advanced / 08_ShadowMapping");
    std::unique_ptr<IDevice> owner(IDevice::Create({}));
    auto& device = *owner;
    ResourceScope resources(device);
    SwapchainDesc swapchain;
    swapchain.window = window.GetHandle();
    swapchain.format = DefaultSwapchainFormat;
    (void)device.CreateSwapchain(swapchain);

#ifdef __APPLE__
    constexpr const char* vertexEntryPoint = "vertexMain";
    constexpr const char* shadowDepthEntryPoint = "fragmentMain";
    constexpr const char* lightingEntryPoint = "fragmentMain";
#else
    constexpr const char* vertexEntryPoint = "main";
    constexpr const char* shadowDepthEntryPoint = "main";
    constexpr const char* lightingEntryPoint = "main";
#endif
    const auto vertexBinary = readShader("Shaders/vertex.bin");
    const auto shadowDepthBinary = readShader("Shaders/shadowDepth.bin");
    const auto lightingBinary = readShader("Shaders/lighting.bin");

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
    shadowDesc.raster.fillMode = FillMode::Solid;
    shadowDesc.raster.cullMode = CullMode::None;
    shadowDesc.raster.frontFace = FrontFace::CounterClockwise;
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
    ColorAttachmentDesc lightingColor{DefaultSwapchainFormat, {}, ColorWriteMask::All};
    GraphicsPipelineDesc lightingDesc;
    lightingDesc.vertexShader = vertexShader;
    lightingDesc.fragmentShader = lightingShader;
    lightingDesc.topology = PrimitiveTopology::TriangleList;
    lightingDesc.raster.fillMode = FillMode::Solid;
    lightingDesc.raster.cullMode = CullMode::None;
    lightingDesc.raster.frontFace = FrontFace::CounterClockwise;
    lightingDesc.colorAttachments = &lightingColor;
    lightingDesc.colorAttachmentCount = 1;
    lightingDesc.layout = {lightingBindings.data(), uint32_t(lightingBindings.size()), sizeof(Params),
                           ShaderStageFlags::Fragment, 15};
    auto lightingPipeline = resources.Keep(device.CreateGraphicsPipeline(lightingDesc));

    TextureDesc shadowMapDesc;
    shadowMapDesc.width = 1536;
    shadowMapDesc.height = 1024;
    shadowMapDesc.format = Format::D32_FLOAT;
    shadowMapDesc.usage = TextureUsage::DepthStencil | TextureUsage::ShaderResource;
    auto shadowMap = resources.Keep(device.CreateTexture(shadowMapDesc));
    ResourceBinding shadowImage;
    shadowImage.texture = shadowMap;
    auto lightingSet = resources.Keep(device.CreateResourceSet({lightingPipeline, &shadowImage, 1}));
    bool initializeImages = true;
    while (window.IsRunning()) {
        window.PollEvents();
        if (!window.IsRunning()) break;
        if (!device.BeginFrame()) continue;
        auto backbuffer = device.GetBackBuffer();
        params.screenSize[0] = float(backbuffer->GetDesc().width);
        params.screenSize[1] = float(backbuffer->GetDesc().height);

        const auto dimensions = backbuffer->GetDesc();

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
            commands->BeginRendering({nullptr, 0, &depth});
            commands->BindGraphicsPipeline(shadowPipeline);
            commands->SetViewport({0, 0, float(1536), float(1024)});
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
            commands->SetViewport({0, 0, float(dimensions.width), float(dimensions.height)});
            commands->SetScissor({0, 0, dimensions.width, dimensions.height});
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
    }
    (void)device.WaitIdle();
}
