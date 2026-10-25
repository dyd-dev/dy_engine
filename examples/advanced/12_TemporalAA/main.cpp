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
    float previousTime = 0;
    float jitter[2] = {};
    float previousJitter[2] = {};
    float historyWeight = .9f;
    float historyValid = 0;
};

int main() {
    const auto readShader = [](const char* path) {
        std::ifstream file(path, std::ios::binary);
        return std::vector<char>{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    };

    Params params;
    dyf::Platform::Window window(960, 640, "Advanced / TemporalAA");
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
    constexpr const char* resolveEntryPoint = "fragmentMain";
    constexpr const char* presentEntryPoint = "fragmentMain";
#else
    constexpr const char* vertexEntryPoint = "main";
    constexpr const char* sceneEntryPoint = "main";
    constexpr const char* resolveEntryPoint = "main";
    constexpr const char* presentEntryPoint = "main";
#endif
    const auto vertexBinary = readShader("Shaders/vertex.bin");
    const auto sceneBinary = readShader("Shaders/scene.bin");
    const auto resolveBinary = readShader("Shaders/resolve.bin");
    const auto presentBinary = readShader("Shaders/present.bin");

    auto vertexShader = resources.Keep(device.CreateShader(
        {ShaderStage::Vertex, vertexEntryPoint, vertexBinary.data(), vertexBinary.size()}));
    auto sceneShader = resources.Keep(device.CreateShader(
        {ShaderStage::Fragment, sceneEntryPoint, sceneBinary.data(), sceneBinary.size()}));
    auto resolveShader = resources.Keep(device.CreateShader(
        {ShaderStage::Fragment, resolveEntryPoint, resolveBinary.data(), resolveBinary.size()}));
    auto presentShader = resources.Keep(device.CreateShader(
        {ShaderStage::Fragment, presentEntryPoint, presentBinary.data(), presentBinary.size()}));
    SamplerDesc sampler;
    sampler.minFilter = SamplerFilter::Linear;
    sampler.magFilter = SamplerFilter::Linear;
    sampler.mipFilter = SamplerFilter::Nearest;
    sampler.addressU = sampler.addressV = sampler.addressW = SamplerAddressMode::ClampToEdge;
    sampler.mipLodBias = sampler.minLod = sampler.maxLod = 0;

    std::array<ColorAttachmentDesc, 2> sceneColors{};
    for (auto& attachment : sceneColors)
        attachment = {Format::R16G16B16A16_FLOAT, {}, ColorWriteMask::All};
    GraphicsPipelineDesc sceneDesc;
    sceneDesc.vertexShader = vertexShader;
    sceneDesc.fragmentShader = sceneShader;
    sceneDesc.topology = PrimitiveTopology::TriangleList;
    sceneDesc.raster.fillMode = FillMode::Solid;
    sceneDesc.raster.cullMode = CullMode::None;
    sceneDesc.raster.frontFace = FrontFace::CounterClockwise;
    sceneDesc.colorAttachments = sceneColors.data();
    sceneDesc.colorAttachmentCount = uint32_t(sceneColors.size());
    sceneDesc.layout.inlineConstantSize = sizeof(Params);
    sceneDesc.layout.inlineConstantStages = ShaderStageFlags::Fragment;
    sceneDesc.layout.inlineConstantBinding = 15;
    auto scenePipeline = resources.Keep(device.CreateGraphicsPipeline(sceneDesc));

    std::vector<ResourceBindingLayout> resolveBindings;
    resolveBindings.push_back({0, ResourceBindingType::SampledTexture, 1, ShaderStageFlags::Fragment, {}});
    resolveBindings.push_back({1, ResourceBindingType::SampledTexture, 1, ShaderStageFlags::Fragment, {}});
    resolveBindings.push_back({2, ResourceBindingType::SampledTexture, 1, ShaderStageFlags::Fragment, {}});
    resolveBindings.push_back({8, ResourceBindingType::StaticSampler, 1, ShaderStageFlags::Fragment, sampler});
    ColorAttachmentDesc resolveColor{Format::R16G16B16A16_FLOAT, {}, ColorWriteMask::All};
    GraphicsPipelineDesc resolveDesc;
    resolveDesc.vertexShader = vertexShader;
    resolveDesc.fragmentShader = resolveShader;
    resolveDesc.topology = PrimitiveTopology::TriangleList;
    resolveDesc.raster.fillMode = FillMode::Solid;
    resolveDesc.raster.cullMode = CullMode::None;
    resolveDesc.raster.frontFace = FrontFace::CounterClockwise;
    resolveDesc.colorAttachments = &resolveColor;
    resolveDesc.colorAttachmentCount = 1;
    resolveDesc.layout = {resolveBindings.data(), uint32_t(resolveBindings.size()), sizeof(Params),
                          ShaderStageFlags::Fragment, 15};
    auto resolvePipeline = resources.Keep(device.CreateGraphicsPipeline(resolveDesc));

    std::vector<ResourceBindingLayout> presentBindings;
    presentBindings.push_back({0, ResourceBindingType::SampledTexture, 1, ShaderStageFlags::Fragment, {}});
    presentBindings.push_back({8, ResourceBindingType::StaticSampler, 1, ShaderStageFlags::Fragment, sampler});
    ColorAttachmentDesc presentColor{DefaultSwapchainFormat, {}, ColorWriteMask::All};
    GraphicsPipelineDesc presentDesc;
    presentDesc.vertexShader = vertexShader;
    presentDesc.fragmentShader = presentShader;
    presentDesc.topology = PrimitiveTopology::TriangleList;
    presentDesc.raster.fillMode = FillMode::Solid;
    presentDesc.raster.cullMode = CullMode::None;
    presentDesc.raster.frontFace = FrontFace::CounterClockwise;
    presentDesc.colorAttachments = &presentColor;
    presentDesc.colorAttachmentCount = 1;
    presentDesc.layout.bindings = presentBindings.data();
    presentDesc.layout.bindingCount = uint32_t(presentBindings.size());
    auto presentPipeline = resources.Keep(device.CreateGraphicsPipeline(presentDesc));

    std::unique_ptr<dyf::RHI::ResourceScope> imageResources;
    uint32_t imageWidth = 0, imageHeight = 0;
    bool initializeImages = true;
    TextureHandle currentColor = nullptr;
    TextureHandle velocity = nullptr;
    TextureHandle history0 = nullptr;
    TextureHandle history1 = nullptr;
    ResourceSetHandle resolve0 = nullptr;
    ResourceSetHandle resolve1 = nullptr;
    ResourceSetHandle present0 = nullptr;
    ResourceSetHandle present1 = nullptr;
    float previousTime = 0;
    float previousJitter[2] = {};
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
            TextureDesc currentColorDesc;
            currentColorDesc.width = imageWidth;
            currentColorDesc.height = imageHeight;
            currentColorDesc.format = Format::R16G16B16A16_FLOAT;
            currentColorDesc.usage = TextureUsage::RenderTarget | TextureUsage::ShaderResource;
            currentColor = imageResources->Keep(device.CreateTexture(currentColorDesc));
            TextureDesc velocityDesc;
            velocityDesc.width = imageWidth;
            velocityDesc.height = imageHeight;
            velocityDesc.format = Format::R16G16B16A16_FLOAT;
            velocityDesc.usage = TextureUsage::RenderTarget | TextureUsage::ShaderResource;
            velocity = imageResources->Keep(device.CreateTexture(velocityDesc));
            TextureDesc history0Desc;
            history0Desc.width = imageWidth;
            history0Desc.height = imageHeight;
            history0Desc.format = Format::R16G16B16A16_FLOAT;
            history0Desc.usage = TextureUsage::RenderTarget | TextureUsage::ShaderResource;
            history0 = imageResources->Keep(device.CreateTexture(history0Desc));
            TextureDesc history1Desc;
            history1Desc.width = imageWidth;
            history1Desc.height = imageHeight;
            history1Desc.format = Format::R16G16B16A16_FLOAT;
            history1Desc.usage = TextureUsage::RenderTarget | TextureUsage::ShaderResource;
            history1 = imageResources->Keep(device.CreateTexture(history1Desc));

            std::vector<ResourceBinding> resolve0Bindings;
            resolve0Bindings.push_back({0, 0, nullptr, currentColor, 0, 0, {}});
            resolve0Bindings.push_back({1, 0, nullptr, velocity, 0, 0, {}});
            resolve0Bindings.push_back({2, 0, nullptr, history1, 0, 0, {}});
            resolve0 = imageResources->Keep(device.CreateResourceSet(
                {resolvePipeline, resolve0Bindings.data(), uint32_t(resolve0Bindings.size())}));
            std::vector<ResourceBinding> resolve1Bindings;
            resolve1Bindings.push_back({0, 0, nullptr, currentColor, 0, 0, {}});
            resolve1Bindings.push_back({1, 0, nullptr, velocity, 0, 0, {}});
            resolve1Bindings.push_back({2, 0, nullptr, history0, 0, 0, {}});
            resolve1 = imageResources->Keep(device.CreateResourceSet(
                {resolvePipeline, resolve1Bindings.data(), uint32_t(resolve1Bindings.size())}));
            std::vector<ResourceBinding> present0Bindings;
            present0Bindings.push_back({0, 0, nullptr, history0, 0, 0, {}});
            present0 = imageResources->Keep(device.CreateResourceSet(
                {presentPipeline, present0Bindings.data(), uint32_t(present0Bindings.size())}));
            std::vector<ResourceBinding> present1Bindings;
            present1Bindings.push_back({0, 0, nullptr, history1, 0, 0, {}});
            present1 = imageResources->Keep(device.CreateResourceSet(
                {presentPipeline, present1Bindings.data(), uint32_t(present1Bindings.size())}));
        }

        params.screenSize[0] = float(imageWidth);
        params.screenSize[1] = float(imageHeight);
        params.time = float(frame) / 60.0f;

        const bool historyValid = !initializeImages;
        params.historyValid = historyValid ? 1.0f : 0.0f;
        params.previousTime = historyValid ? previousTime : params.time;
        params.previousJitter[0] = historyValid ? previousJitter[0] : 0;
        params.previousJitter[1] = historyValid ? previousJitter[1] : 0;
        // Halton(2, 3) 수열의 첫 여덟 위치를 반복한다.
        constexpr float sampleOffsets[8][2] = {
            {0, -1.f/6}, {-.25f, 1.f/6}, {.25f, -7.f/18}, {-.375f, -1.f/18},
            {.125f, 5.f/18}, {-.125f, -5.f/18}, {.375f, 1.f/18}, {-.4375f, 7.f/18}};
        params.jitter[0] = sampleOffsets[frame % 8][0];
        params.jitter[1] = sampleOffsets[frame % 8][1];
        auto historyOutput = (frame % 2 == 0) ? history0 : history1;
        auto resolveSet = (frame % 2 == 0) ? resolve0 : resolve1;
        auto presentSet = (frame % 2 == 0) ? present0 : present1;

        dyf::RHI::ResourceScope frameResources(device);
        auto* commands = frameResources.Keep(device.AcquireCommandList());

        if (initializeImages) {

            {
                const ResourceBarrierDesc barrier{
                    nullptr, history0, ResourceState::Undefined, ResourceState::RenderTarget, {}};
                commands->ResourceBarrier(&barrier, 1);
                ColorAttachment a;
                a.texture = history0;
                a.loadOp = LoadOp::Clear;
                a.storeOp = StoreOp::Store;
                commands->BeginRendering({&a, 1, nullptr});
                commands->EndRendering();
                const ResourceBarrierDesc ready{
                    nullptr, history0, ResourceState::RenderTarget, ResourceState::ShaderResource, {}};
                commands->ResourceBarrier(&ready, 1);
            }
            {
                const ResourceBarrierDesc barrier{
                    nullptr, history1, ResourceState::Undefined, ResourceState::RenderTarget, {}};
                commands->ResourceBarrier(&barrier, 1);
                ColorAttachment a;
                a.texture = history1;
                a.loadOp = LoadOp::Clear;
                a.storeOp = StoreOp::Store;
                commands->BeginRendering({&a, 1, nullptr});
                commands->EndRendering();
                const ResourceBarrierDesc ready{
                    nullptr, history1, ResourceState::RenderTarget, ResourceState::ShaderResource, {}};
                commands->ResourceBarrier(&ready, 1);
            }
        }

        {
            const ResourceBarrierDesc currentColorBarrier{
                nullptr, currentColor, initializeImages ? ResourceState::Undefined : ResourceState::ShaderResource, ResourceState::RenderTarget, {}};
            commands->ResourceBarrier(&currentColorBarrier, 1);
            const ResourceBarrierDesc velocityBarrier{
                nullptr, velocity, initializeImages ? ResourceState::Undefined : ResourceState::ShaderResource, ResourceState::RenderTarget, {}};
            commands->ResourceBarrier(&velocityBarrier, 1);
            std::array<ColorAttachment, 2> colors{};
            colors[0].texture = currentColor;
            colors[0].loadOp = LoadOp::Clear;
            colors[0].storeOp = StoreOp::Store;
            colors[1].texture = velocity;
            colors[1].loadOp = LoadOp::Clear;
            colors[1].storeOp = StoreOp::Store;
            commands->BeginRendering({colors.data(), uint32_t(colors.size()), nullptr});
            commands->BindGraphicsPipeline(scenePipeline);
            commands->SetViewport({0, 0, float(imageWidth), float(imageHeight)});
            commands->SetScissor({0, 0, imageWidth, imageHeight});
            commands->SetInlineConstants(0, sizeof(params), &params);
            commands->DrawInstanced(3, 1, 0, 0);
            commands->EndRendering();
            const ResourceBarrierDesc currentColorReady{
                nullptr, currentColor, ResourceState::RenderTarget, ResourceState::ShaderResource, {}};
            commands->ResourceBarrier(&currentColorReady, 1);
            const ResourceBarrierDesc velocityReady{
                nullptr, velocity, ResourceState::RenderTarget, ResourceState::ShaderResource, {}};
            commands->ResourceBarrier(&velocityReady, 1);
        }

        {
            const ResourceBarrierDesc historyOutputBarrier{
                nullptr, historyOutput, ResourceState::ShaderResource, ResourceState::RenderTarget, {}};
            commands->ResourceBarrier(&historyOutputBarrier, 1);
            std::array<ColorAttachment, 1> colors{};
            colors[0].texture = historyOutput;
            colors[0].loadOp = LoadOp::Clear;
            colors[0].storeOp = StoreOp::Store;
            commands->BeginRendering({colors.data(), uint32_t(colors.size()), nullptr});
            commands->BindGraphicsPipeline(resolvePipeline);
            commands->BindResourceSet(resolveSet);
            commands->SetViewport({0, 0, float(imageWidth), float(imageHeight)});
            commands->SetScissor({0, 0, imageWidth, imageHeight});
            commands->SetInlineConstants(0, sizeof(params), &params);
            commands->DrawInstanced(3, 1, 0, 0);
            commands->EndRendering();
            const ResourceBarrierDesc historyOutputReady{
                nullptr, historyOutput, ResourceState::RenderTarget, ResourceState::ShaderResource, {}};
            commands->ResourceBarrier(&historyOutputReady, 1);
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
            commands->BindGraphicsPipeline(presentPipeline);
            commands->BindResourceSet(presentSet);
            commands->SetViewport({0, 0, float(imageWidth), float(imageHeight)});
            commands->SetScissor({0, 0, imageWidth, imageHeight});
            commands->DrawInstanced(3, 1, 0, 0);
            commands->EndRendering();
        }

        const ResourceBarrierDesc presentBarrier{
            nullptr, backbuffer, ResourceState::RenderTarget, ResourceState::Present, {}};
        commands->ResourceBarrier(&presentBarrier, 1);
        (void)commands->Close();
        (void)device.Submit(&commands, 1);
        initializeImages = false;
        previousTime = params.time;
        previousJitter[0] = params.jitter[0];
        previousJitter[1] = params.jitter[1];

        ++frame;
        (void)device.Present();
    }

    (void)device.WaitIdle();
    return 0;
}
