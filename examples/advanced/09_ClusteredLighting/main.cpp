#include "dyf/Platform/Window.h"
#include "dyf/RHI.h"

#include <iterator>
#include <fstream>
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
    params.extra[2] = 64;
    dyf::Platform::Window window(960, 640, "Advanced / 09_ClusteredLighting");

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
    // Vulkan: glslc -fshader-stage=comp Shaders/cluster.glsl -o Shaders/cluster.bin
    // D3D12: dxc -T cs_6_0 -E main Shaders/cluster.hlsl -Fo Shaders/cluster.bin
    // Metal: xcrun -sdk macosx metal -c Shaders/cluster.metal -o Shaders/cluster.air
    //        xcrun -sdk macosx metallib Shaders/cluster.air -o Shaders/cluster.bin
    std::ifstream clusterFile("Shaders/cluster.bin", std::ios::binary);
    const std::vector<char> clusterBinary{
        std::istreambuf_iterator<char>(clusterFile), std::istreambuf_iterator<char>()};
    // Vulkan: glslc -fshader-stage=frag Shaders/lighting.glsl -o Shaders/lighting.bin
    // D3D12: dxc -T ps_6_0 -E main Shaders/lighting.hlsl -Fo Shaders/lighting.bin
    // Metal: xcrun -sdk macosx metal -c Shaders/lighting.metal -o Shaders/lighting.air
    //        xcrun -sdk macosx metallib Shaders/lighting.air -o Shaders/lighting.bin
    std::ifstream lightingFile("Shaders/lighting.bin", std::ios::binary);
    const std::vector<char> lightingBinary{
        std::istreambuf_iterator<char>(lightingFile), std::istreambuf_iterator<char>()};
#ifdef DY_EXAMPLE_METAL
    constexpr const char* vertexEntryPoint = "vertexMain";
    constexpr const char* clusterEntryPoint = "computeMain";
    constexpr const char* lightingEntryPoint = "fragmentMain";
#else
    constexpr const char* vertexEntryPoint = "main";
    constexpr const char* clusterEntryPoint = "main";
    constexpr const char* lightingEntryPoint = "main";
#endif

    auto vertexShader = resources.Keep(device.CreateShader({
        ShaderStage::Vertex,
        vertexEntryPoint,
        vertexBinary.data(),
        vertexBinary.size()
    }));
    auto clusterShader = resources.Keep(device.CreateShader({
        ShaderStage::Compute,
        clusterEntryPoint,
        clusterBinary.data(),
        clusterBinary.size()
    }));
    auto lightingShader = resources.Keep(device.CreateShader({
        ShaderStage::Fragment,
        lightingEntryPoint,
        lightingBinary.data(),
        lightingBinary.size()
    }));

    std::vector<ResourceBindingLayout> lightingBindings;
    lightingBindings.push_back({0, ResourceBindingType::ReadOnlyStorageBuffer, 1, ShaderStageFlags::Fragment, {}});

    ColorAttachmentDesc colorDesc{DefaultSwapchainFormat, {}, ColorWriteMask::All};

    GraphicsPipelineDesc lightingDesc;
    lightingDesc.vertexShader = vertexShader;
    lightingDesc.fragmentShader = lightingShader;
    lightingDesc.topology = PrimitiveTopology::TriangleList;
    lightingDesc.raster = {FillMode::Solid, CullMode::None, FrontFace::CounterClockwise, 0, 0, 0};
    lightingDesc.colorAttachments = &colorDesc;
    lightingDesc.colorAttachmentCount = 1;
    lightingDesc.layout = {
        lightingBindings.data(),
        uint32_t(lightingBindings.size()),
        sizeof(Params),
        ShaderStageFlags::Fragment,
        15
    };

    auto lightingPipeline = resources.Keep(device.CreateGraphicsPipeline(lightingDesc));

    constexpr uint32_t clusterCount = 16 * 16 * 8;
    constexpr uint32_t wordsPerCluster = 65;

    BufferDesc listDesc;
    listDesc.size = clusterCount * wordsPerCluster * sizeof(uint32_t);
    listDesc.stride = sizeof(uint32_t);
    listDesc.usage = BufferUsage::Storage;
    listDesc.initialState = ResourceState::Undefined;

    auto lightLists = resources.Keep(device.CreateBuffer(listDesc));

    ResourceBindingLayout computeBinding{0, ResourceBindingType::ReadWriteStorageBuffer, 1, ShaderStageFlags::Compute, {}};
    ComputePipelineDesc computeDesc;
    computeDesc.computeShader = clusterShader;
    computeDesc.layout = {&computeBinding, 1, sizeof(Params), ShaderStageFlags::Compute, 15};

    computeDesc.threadGroupSize[0] = 64;
    auto clusterPipeline = resources.Keep(device.CreateComputePipeline(computeDesc));

    ResourceBinding listBinding{0, 0, lightLists, nullptr, 0, listDesc.size, {}};
    auto clusterSet = resources.Keep(device.CreateResourceSet({clusterPipeline, &listBinding, 1}));
    auto lightingSet = resources.Keep(device.CreateResourceSet({lightingPipeline, &listBinding, 1}));

    bool listsInitialized = false;
    uint64_t frame = 0;

    while (window.IsRunning()) {
        window.PollEvents();
        if (!window.IsRunning()) {
            break;
        }

        if (!device.BeginFrame()) continue;

        auto backbuffer = device.GetBackBuffer();

        const auto dimensions = backbuffer->GetDesc();

        params.sizeTime[0] = float(dimensions.width);
        params.sizeTime[1] = float(dimensions.height);
        params.sizeTime[2] = float(frame) / 60.0f;

        dyf::RHI::ResourceScope frameResources(device);
        auto* commands = frameResources.Keep(device.AcquireCommandList());

        const ResourceBarrierDesc writable{
            lightLists,
            nullptr,
            listsInitialized ? ResourceState::ShaderResource : ResourceState::Undefined,
            ResourceState::UnorderedAccess,
            {}
        };
        commands->ResourceBarrier(&writable, 1);

        commands->BindComputePipeline(clusterPipeline);
        commands->BindResourceSet(clusterSet);
        commands->SetInlineConstants(0, sizeof(params), &params);
        commands->Dispatch(clusterCount / 64, 1, 1);

        const ResourceBarrierDesc readable{
            lightLists,
            nullptr,
            ResourceState::UnorderedAccess,
            ResourceState::ShaderResource,
            {}
        };
        commands->ResourceBarrier(&readable, 1);

        {
            const ResourceBarrierDesc backbufferBarrier{nullptr, backbuffer, ResourceState::Present, ResourceState::RenderTarget, {}};
            commands->ResourceBarrier(&backbufferBarrier, 1);

            ColorAttachment colorAttachment{};
            colorAttachment.texture = backbuffer;
            colorAttachment.loadOp = LoadOp::Clear;
            colorAttachment.storeOp = StoreOp::Store;

            commands->BeginRendering({&colorAttachment, 1, nullptr});
            commands->BindGraphicsPipeline(lightingPipeline);
            commands->BindResourceSet(lightingSet);
            commands->SetViewport({0, 0, float(dimensions.width), float(dimensions.height), 0, 1});
            commands->SetScissor({0, 0, dimensions.width, dimensions.height});

            commands->SetInlineConstants(0, sizeof(params), &params);
            commands->DrawInstanced(3, 1, 0, 0);
            commands->EndRendering();
        }

        const ResourceBarrierDesc presentBarrier{nullptr, backbuffer, ResourceState::RenderTarget, ResourceState::Present, {}};
        commands->ResourceBarrier(&presentBarrier, 1);

        (void)commands->Close();
        (void)device.Submit(&commands, 1);

        listsInitialized = true;
        ++frame;

        (void)device.Present();
    }

    (void)device.WaitIdle();
    return 0;

}
