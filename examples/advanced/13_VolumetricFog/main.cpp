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
    params.extra[0] = .22f;
    params.extra[1] = 64;
    params.settings[1] = .7f;
    dyf::Platform::Window window(960, 640, "Advanced / 13_VolumetricFog");

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
    // Vulkan: glslc -fshader-stage=frag Shaders/fog.glsl -o Shaders/fog.bin
    // D3D12: dxc -T ps_6_0 -E main Shaders/fog.hlsl -Fo Shaders/fog.bin
    // Metal: xcrun -sdk macosx metal -c Shaders/fog.metal -o Shaders/fog.air
    //        xcrun -sdk macosx metallib Shaders/fog.air -o Shaders/fog.bin
    std::ifstream fogFile("Shaders/fog.bin", std::ios::binary);
    const std::vector<char> fogBinary{
        std::istreambuf_iterator<char>(fogFile), std::istreambuf_iterator<char>()};
#ifdef DY_EXAMPLE_METAL
    constexpr const char* vertexEntryPoint = "vertexMain";
    constexpr const char* sceneEntryPoint = "fragmentMain";
    constexpr const char* fogEntryPoint = "fragmentMain";
#else
    constexpr const char* vertexEntryPoint = "main";
    constexpr const char* sceneEntryPoint = "main";
    constexpr const char* fogEntryPoint = "main";
#endif

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
    sceneDesc.raster = {FillMode::Solid, CullMode::None, FrontFace::CounterClockwise, 0, 0, 0};
    sceneDesc.colorAttachments = &sceneColorDesc;
    sceneDesc.colorAttachmentCount = 1;
    sceneDesc.layout = {nullptr, 0, sizeof(Params), ShaderStageFlags::Fragment, 15};
    auto scenePipeline = resources.Keep(device.CreateGraphicsPipeline(sceneDesc));

    std::vector<ResourceBindingLayout> fogBindings;
    fogBindings.push_back({0, ResourceBindingType::SampledTexture, 1, ShaderStageFlags::Fragment, {}});
    fogBindings.push_back({8, ResourceBindingType::StaticSampler, 1, ShaderStageFlags::Fragment, sampler});

    GraphicsPipelineDesc fogDesc;
    fogDesc.vertexShader = vertexShader;
    fogDesc.fragmentShader = fogShader;
    fogDesc.topology = PrimitiveTopology::TriangleList;
    fogDesc.raster = {FillMode::Solid, CullMode::None, FrontFace::CounterClockwise, 0, 0, 0};
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

        params.sizeTime[0] = float(imageWidth);
        params.sizeTime[1] = float(imageHeight);
        params.sizeTime[2] = float(frame) / 60.0f;

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
            commands->SetViewport({0, 0, float(imageWidth), float(imageHeight), 0, 1});
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
