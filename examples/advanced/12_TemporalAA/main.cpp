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
    float sizeTime[4] = {960, 640, 0, 0};
    float settings[4] = {1, .45f, .8f, .9f};
    float jitter[4] = {};
    float extra[4] = {};
};

int main() {
    Params params;
    dyf::Platform::Window window(960, 640, "Advanced / TemporalAA");

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

    // 예제 폴더에서 연결한 엔진의 백엔드에 맞는 명령으로 셰이더를 컴파일한다.
    // Metal의 macosx SDK는 대상에 따라 iphoneos 또는 iphonesimulator로 바꾼다.
    // Vulkan: glslc -fshader-stage=vert Shaders/vertex.glsl -o Shaders/vertex.bin
    // D3D12: dxc -T vs_6_0 -E main Shaders/vertex.hlsl -Fo Shaders/vertex.bin
    // Metal: xcrun -sdk macosx metal -c Shaders/vertex.metal -o Shaders/vertex.air
    //        xcrun -sdk macosx metallib Shaders/vertex.air -o Shaders/vertex.bin
    std::ifstream vertexFile("Shaders/vertex.bin", std::ios::binary);
    const std::vector<char> vertexBinary{
        std::istreambuf_iterator<char>(vertexFile), std::istreambuf_iterator<char>()};
    // Vulkan: glslc -fshader-stage=frag Shaders/scene.glsl -o Shaders/scene.bin
    // D3D12: dxc -T ps_6_0 -E main Shaders/scene.hlsl -Fo Shaders/scene.bin
    // Metal: xcrun -sdk macosx metal -c Shaders/scene.metal -o Shaders/scene.air
    //        xcrun -sdk macosx metallib Shaders/scene.air -o Shaders/scene.bin
    std::ifstream sceneFile("Shaders/scene.bin", std::ios::binary);
    const std::vector<char> sceneBinary{
        std::istreambuf_iterator<char>(sceneFile), std::istreambuf_iterator<char>()};
    // Vulkan: glslc -fshader-stage=frag Shaders/resolve.glsl -o Shaders/resolve.bin
    // D3D12: dxc -T ps_6_0 -E main Shaders/resolve.hlsl -Fo Shaders/resolve.bin
    // Metal: xcrun -sdk macosx metal -c Shaders/resolve.metal -o Shaders/resolve.air
    //        xcrun -sdk macosx metallib Shaders/resolve.air -o Shaders/resolve.bin
    std::ifstream resolveFile("Shaders/resolve.bin", std::ios::binary);
    const std::vector<char> resolveBinary{
        std::istreambuf_iterator<char>(resolveFile), std::istreambuf_iterator<char>()};
    // Vulkan: glslc -fshader-stage=frag Shaders/present.glsl -o Shaders/present.bin
    // D3D12: dxc -T ps_6_0 -E main Shaders/present.hlsl -Fo Shaders/present.bin
    // Metal: xcrun -sdk macosx metal -c Shaders/present.metal -o Shaders/present.air
    //        xcrun -sdk macosx metallib Shaders/present.air -o Shaders/present.bin
    std::ifstream presentFile("Shaders/present.bin", std::ios::binary);
    const std::vector<char> presentBinary{
        std::istreambuf_iterator<char>(presentFile), std::istreambuf_iterator<char>()};
#ifdef DY_EXAMPLE_METAL
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

    std::vector<ResourceBindingLayout> sceneBindings;
    std::array<ColorAttachmentDesc, 2> sceneColors{};
    for (auto& attachment : sceneColors)
        attachment = {Format::R16G16B16A16_FLOAT, {}, ColorWriteMask::All};
    GraphicsPipelineDesc sceneDesc;
    sceneDesc.vertexShader = vertexShader;
    sceneDesc.fragmentShader = sceneShader;
    sceneDesc.topology = PrimitiveTopology::TriangleList;
    sceneDesc.raster = {FillMode::Solid, CullMode::None, FrontFace::CounterClockwise, 0, 0, 0};
    sceneDesc.colorAttachments = sceneColors.data();
    sceneDesc.colorAttachmentCount = uint32_t(sceneColors.size());
    sceneDesc.layout = {sceneBindings.data(), uint32_t(sceneBindings.size()), sizeof(Params),
                        ShaderStageFlags::Fragment, 15};
    auto scenePipeline = resources.Keep(device.CreateGraphicsPipeline(sceneDesc));

    std::vector<ResourceBindingLayout> resolveBindings;
    resolveBindings.push_back({0, ResourceBindingType::SampledTexture, 1, ShaderStageFlags::Fragment, {}});
    resolveBindings.push_back({1, ResourceBindingType::SampledTexture, 1, ShaderStageFlags::Fragment, {}});
    resolveBindings.push_back({2, ResourceBindingType::SampledTexture, 1, ShaderStageFlags::Fragment, {}});
    resolveBindings.push_back({8, ResourceBindingType::StaticSampler, 1, ShaderStageFlags::Fragment, sampler});
    std::array<ColorAttachmentDesc, 1> resolveColors{};
    for (auto& attachment : resolveColors)
        attachment = {Format::R16G16B16A16_FLOAT, {}, ColorWriteMask::All};
    GraphicsPipelineDesc resolveDesc;
    resolveDesc.vertexShader = vertexShader;
    resolveDesc.fragmentShader = resolveShader;
    resolveDesc.topology = PrimitiveTopology::TriangleList;
    resolveDesc.raster = {FillMode::Solid, CullMode::None, FrontFace::CounterClockwise, 0, 0, 0};
    resolveDesc.colorAttachments = resolveColors.data();
    resolveDesc.colorAttachmentCount = uint32_t(resolveColors.size());
    resolveDesc.layout = {resolveBindings.data(), uint32_t(resolveBindings.size()), sizeof(Params),
                          ShaderStageFlags::Fragment, 15};
    auto resolvePipeline = resources.Keep(device.CreateGraphicsPipeline(resolveDesc));

    std::vector<ResourceBindingLayout> presentBindings;
    presentBindings.push_back({0, ResourceBindingType::SampledTexture, 1, ShaderStageFlags::Fragment, {}});
    presentBindings.push_back({8, ResourceBindingType::StaticSampler, 1, ShaderStageFlags::Fragment, sampler});
    std::array<ColorAttachmentDesc, 1> presentColors{};
    for (auto& attachment : presentColors)
        attachment = {DefaultSwapchainFormat, {}, ColorWriteMask::All};
    GraphicsPipelineDesc presentDesc;
    presentDesc.vertexShader = vertexShader;
    presentDesc.fragmentShader = presentShader;
    presentDesc.topology = PrimitiveTopology::TriangleList;
    presentDesc.raster = {FillMode::Solid, CullMode::None, FrontFace::CounterClockwise, 0, 0, 0};
    presentDesc.colorAttachments = presentColors.data();
    presentDesc.colorAttachmentCount = uint32_t(presentColors.size());
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

        params.sizeTime[0] = float(imageWidth);
        params.sizeTime[1] = float(imageHeight);
        params.sizeTime[2] = float(frame) / 60.0f;

        const bool historyValid = !initializeImages;
        params.extra[0] = historyValid ? 1.0f : 0.0f;
        params.extra[3] = historyValid ? previousTime : params.sizeTime[2];
        params.jitter[2] = historyValid ? previousJitter[0] : 0;
        params.jitter[3] = historyValid ? previousJitter[1] : 0;
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
            commands->SetViewport({0, 0, float(imageWidth), float(imageHeight), 0, 1});
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
            commands->SetViewport({0, 0, float(imageWidth), float(imageHeight), 0, 1});
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
            commands->SetViewport({0, 0, float(imageWidth), float(imageHeight), 0, 1});
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
        previousTime = params.sizeTime[2];
        previousJitter[0] = params.jitter[0];
        previousJitter[1] = params.jitter[1];

        ++frame;
        (void)device.Present();
    }

    (void)device.WaitIdle();
    return 0;
}
