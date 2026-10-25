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
    float sizeTime[4] = {960, 640, 0, 0};
    float settings[4] = {1, .45f, .8f, .9f};
    float jitter[4] = {};
    float extra[4] = {};
};

int main() {
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

    // 예제 폴더에서 연결한 엔진의 백엔드에 맞는 명령으로 셰이더를 컴파일한다.
    // Metal의 macosx SDK는 대상에 따라 iphoneos 또는 iphonesimulator로 바꾼다.
    // Vulkan: glslc -fshader-stage=vert Shaders/vertex.glsl -o Shaders/vertex.bin
    // D3D12: dxc -T vs_6_0 -E main Shaders/vertex.hlsl -Fo Shaders/vertex.bin
    // Metal: xcrun -sdk macosx metal -c Shaders/vertex.metal -o Shaders/vertex.air
    //        xcrun -sdk macosx metallib Shaders/vertex.air -o Shaders/vertex.bin
    std::ifstream vertexFile("Shaders/vertex.bin", std::ios::binary);
    const std::vector<char> vertexBinary{
        std::istreambuf_iterator<char>(vertexFile), std::istreambuf_iterator<char>()};
    // Vulkan: glslc -fshader-stage=frag Shaders/shadowDepth.glsl -o Shaders/shadowDepth.bin
    // D3D12: dxc -T ps_6_0 -E main Shaders/shadowDepth.hlsl -Fo Shaders/shadowDepth.bin
    // Metal: xcrun -sdk macosx metal -c Shaders/shadowDepth.metal -o Shaders/shadowDepth.air
    //        xcrun -sdk macosx metallib Shaders/shadowDepth.air -o Shaders/shadowDepth.bin
    std::ifstream shadowDepthFile("Shaders/shadowDepth.bin", std::ios::binary);
    const std::vector<char> shadowDepthBinary{
        std::istreambuf_iterator<char>(shadowDepthFile), std::istreambuf_iterator<char>()};
    // Vulkan: glslc -fshader-stage=frag Shaders/lighting.glsl -o Shaders/lighting.bin
    // D3D12: dxc -T ps_6_0 -E main Shaders/lighting.hlsl -Fo Shaders/lighting.bin
    // Metal: xcrun -sdk macosx metal -c Shaders/lighting.metal -o Shaders/lighting.air
    //        xcrun -sdk macosx metallib Shaders/lighting.air -o Shaders/lighting.bin
    std::ifstream lightingFile("Shaders/lighting.bin", std::ios::binary);
    const std::vector<char> lightingBinary{
        std::istreambuf_iterator<char>(lightingFile), std::istreambuf_iterator<char>()};
#ifdef DY_EXAMPLE_METAL
    constexpr const char* vertexEntryPoint = "vertexMain";
    constexpr const char* shadowDepthEntryPoint = "fragmentMain";
    constexpr const char* lightingEntryPoint = "fragmentMain";
#else
    constexpr const char* vertexEntryPoint = "main";
    constexpr const char* shadowDepthEntryPoint = "main";
    constexpr const char* lightingEntryPoint = "main";
#endif

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
