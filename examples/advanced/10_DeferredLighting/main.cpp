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
    uint32_t lightCount = 24;
    float exposure = 1;
    float roughness = .45f;
    float metallic = .8f;
};

int main() {
    const auto readShader = [](const char* path) {
        std::ifstream file(path, std::ios::binary);
        return std::vector<char>{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    };

    Params params;
    dyf::Platform::Window window(960, 640, "Advanced / DeferredLighting");
    std::unique_ptr<IDevice> owner(IDevice::Create({}));

    auto& device = *owner;

    dyf::RHI::ResourceScope resources(device);
    SwapchainDesc swap;
    swap.window = window.GetHandle();
    swap.format = DefaultSwapchainFormat;
    (void)device.CreateSwapchain(swap);

#ifdef __APPLE__
    constexpr const char* vertexEntryPoint = "vertexMain";
    constexpr const char* geometryEntryPoint = "fragmentMain";
    constexpr const char* lightingEntryPoint = "fragmentMain";
#else
    constexpr const char* vertexEntryPoint = "main";
    constexpr const char* geometryEntryPoint = "main";
    constexpr const char* lightingEntryPoint = "main";
#endif
    const auto vertexBinary = readShader("Shaders/vertex.bin");
    const auto geometryBinary = readShader("Shaders/geometry.bin");
    const auto lightingBinary = readShader("Shaders/lighting.bin");

    auto vertexShader = resources.Keep(device.CreateShader(
        {ShaderStage::Vertex, vertexEntryPoint, vertexBinary.data(), vertexBinary.size()}));
    auto geometryShader = resources.Keep(device.CreateShader(
        {ShaderStage::Fragment, geometryEntryPoint, geometryBinary.data(), geometryBinary.size()}));
    auto lightingShader = resources.Keep(device.CreateShader(
        {ShaderStage::Fragment, lightingEntryPoint, lightingBinary.data(), lightingBinary.size()}));
    SamplerDesc sampler;
    sampler.minFilter = SamplerFilter::Nearest;
    sampler.magFilter = SamplerFilter::Nearest;
    sampler.mipFilter = SamplerFilter::Nearest;
    sampler.addressU = sampler.addressV = sampler.addressW = SamplerAddressMode::ClampToEdge;
    sampler.mipLodBias = sampler.minLod = sampler.maxLod = 0;

    std::array<ColorAttachmentDesc, 3> geometryColors{};
    for (auto& attachment : geometryColors)
        attachment = {Format::R16G16B16A16_FLOAT, {}, ColorWriteMask::All};
    GraphicsPipelineDesc geometryDesc;
    geometryDesc.vertexShader = vertexShader;
    geometryDesc.fragmentShader = geometryShader;
    geometryDesc.topology = PrimitiveTopology::TriangleList;
    geometryDesc.raster.fillMode = FillMode::Solid;
    geometryDesc.raster.cullMode = CullMode::None;
    geometryDesc.raster.frontFace = FrontFace::CounterClockwise;
    geometryDesc.colorAttachments = geometryColors.data();
    geometryDesc.colorAttachmentCount = uint32_t(geometryColors.size());
    geometryDesc.layout.inlineConstantSize = sizeof(Params);
    geometryDesc.layout.inlineConstantStages = ShaderStageFlags::Fragment;
    geometryDesc.layout.inlineConstantBinding = 15;
    auto geometryPipeline = resources.Keep(device.CreateGraphicsPipeline(geometryDesc));

    std::vector<ResourceBindingLayout> lightingBindings;
    lightingBindings.push_back({0, ResourceBindingType::SampledTexture, 1, ShaderStageFlags::Fragment, {}});
    lightingBindings.push_back({1, ResourceBindingType::SampledTexture, 1, ShaderStageFlags::Fragment, {}});
    lightingBindings.push_back({2, ResourceBindingType::SampledTexture, 1, ShaderStageFlags::Fragment, {}});
    lightingBindings.push_back({8, ResourceBindingType::StaticSampler, 1, ShaderStageFlags::Fragment, sampler});
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

    std::unique_ptr<dyf::RHI::ResourceScope> imageResources;
    uint32_t imageWidth = 0, imageHeight = 0;
    bool initializeImages = true;
    TextureHandle positionBuffer = nullptr;
    TextureHandle normalBuffer = nullptr;
    TextureHandle albedoBuffer = nullptr;
    ResourceSetHandle lightingSet = nullptr;

    uint64_t frame = 0;

    while (window.IsRunning()) {
        window.PollEvents();
        if (!window.IsRunning()) break;
        if (!device.BeginFrame()) continue;
        auto backbuffer = device.GetBackBuffer();

        const auto dimensions = backbuffer->GetDesc();

        if (dimensions.width != imageWidth || dimensions.height != imageHeight) {
            (void)device.WaitIdle();
            imageResources = std::make_unique<dyf::RHI::ResourceScope>(device);
            imageWidth = dimensions.width;
            imageHeight = dimensions.height;
            initializeImages = true;
            TextureDesc positionBufferDesc;
            positionBufferDesc.width = imageWidth;
            positionBufferDesc.height = imageHeight;
            positionBufferDesc.format = Format::R16G16B16A16_FLOAT;
            positionBufferDesc.usage = TextureUsage::RenderTarget | TextureUsage::ShaderResource;
            positionBuffer = imageResources->Keep(device.CreateTexture(positionBufferDesc));
            TextureDesc normalBufferDesc;
            normalBufferDesc.width = imageWidth;
            normalBufferDesc.height = imageHeight;
            normalBufferDesc.format = Format::R16G16B16A16_FLOAT;
            normalBufferDesc.usage = TextureUsage::RenderTarget | TextureUsage::ShaderResource;
            normalBuffer = imageResources->Keep(device.CreateTexture(normalBufferDesc));
            TextureDesc albedoBufferDesc;
            albedoBufferDesc.width = imageWidth;
            albedoBufferDesc.height = imageHeight;
            albedoBufferDesc.format = Format::R16G16B16A16_FLOAT;
            albedoBufferDesc.usage = TextureUsage::RenderTarget | TextureUsage::ShaderResource;
            albedoBuffer = imageResources->Keep(device.CreateTexture(albedoBufferDesc));

            std::vector<ResourceBinding> lightingSetBindings;
            lightingSetBindings.push_back({0, 0, nullptr, positionBuffer, 0, 0, {}});
            lightingSetBindings.push_back({1, 0, nullptr, normalBuffer, 0, 0, {}});
            lightingSetBindings.push_back({2, 0, nullptr, albedoBuffer, 0, 0, {}});
            lightingSet = imageResources->Keep(device.CreateResourceSet(
                {lightingPipeline, lightingSetBindings.data(), uint32_t(lightingSetBindings.size())}));
        }

        params.screenSize[0] = float(imageWidth);
        params.screenSize[1] = float(imageHeight);
        params.time = float(frame) / 60.0f;

        dyf::RHI::ResourceScope frameResources(device);
        auto* commands = frameResources.Keep(device.AcquireCommandList());

        {
            const ResourceBarrierDesc positionBufferBarrier{
                nullptr, positionBuffer, initializeImages ? ResourceState::Undefined : ResourceState::ShaderResource, ResourceState::RenderTarget, {}};
            commands->ResourceBarrier(&positionBufferBarrier, 1);
            const ResourceBarrierDesc normalBufferBarrier{
                nullptr, normalBuffer, initializeImages ? ResourceState::Undefined : ResourceState::ShaderResource, ResourceState::RenderTarget, {}};
            commands->ResourceBarrier(&normalBufferBarrier, 1);
            const ResourceBarrierDesc albedoBufferBarrier{
                nullptr, albedoBuffer, initializeImages ? ResourceState::Undefined : ResourceState::ShaderResource, ResourceState::RenderTarget, {}};
            commands->ResourceBarrier(&albedoBufferBarrier, 1);
            std::array<ColorAttachment, 3> colors{};
            colors[0].texture = positionBuffer;
            colors[0].loadOp = LoadOp::Clear;
            colors[0].storeOp = StoreOp::Store;
            colors[1].texture = normalBuffer;
            colors[1].loadOp = LoadOp::Clear;
            colors[1].storeOp = StoreOp::Store;
            colors[2].texture = albedoBuffer;
            colors[2].loadOp = LoadOp::Clear;
            colors[2].storeOp = StoreOp::Store;
            commands->BeginRendering({colors.data(), uint32_t(colors.size()), nullptr});
            commands->BindGraphicsPipeline(geometryPipeline);
            commands->SetViewport({0, 0, float(imageWidth), float(imageHeight)});
            commands->SetScissor({0, 0, imageWidth, imageHeight});
            commands->SetInlineConstants(0, sizeof(params), &params);
            commands->DrawInstanced(3, 1, 0, 0);
            commands->EndRendering();
            const ResourceBarrierDesc positionBufferReady{
                nullptr, positionBuffer, ResourceState::RenderTarget, ResourceState::ShaderResource, {}};
            commands->ResourceBarrier(&positionBufferReady, 1);
            const ResourceBarrierDesc normalBufferReady{
                nullptr, normalBuffer, ResourceState::RenderTarget, ResourceState::ShaderResource, {}};
            commands->ResourceBarrier(&normalBufferReady, 1);
            const ResourceBarrierDesc albedoBufferReady{
                nullptr, albedoBuffer, ResourceState::RenderTarget, ResourceState::ShaderResource, {}};
            commands->ResourceBarrier(&albedoBufferReady, 1);
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
            commands->SetViewport({0, 0, float(imageWidth), float(imageHeight)});
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

        ++frame;
        (void)device.Present();
    }

    (void)device.WaitIdle();
    return 0;
}
