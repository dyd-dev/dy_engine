#include "dyf/Platform/Window.h"
#include "dyf/RHI.h"

#include <iterator>
#include <fstream>
#include <cstdint>
#include <memory>
#include <vector>

using namespace dyf::RHI;

struct alignas(16) Params {
    float screenSize[2] = {};
    float time = 0;
    uint32_t lightCount = 64;
    float exposure = 1;
};

int main() {
    const auto readShader = [](const char* path) {
        std::ifstream file(path, std::ios::binary);
        return std::vector<char>{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    };

    Params params;
    dyf::Platform::Window window(960, 640, "Advanced / 09_ClusteredLighting");
    std::unique_ptr<IDevice> owner(IDevice::Create({}));

    auto& device = *owner;

    dyf::RHI::ResourceScope resources(device);

    SwapchainDesc swap;
    swap.window = window.GetHandle();
    swap.format = DefaultSwapchainFormat;

    (void)device.CreateSwapchain(swap);

#ifdef __APPLE__
    constexpr const char* vertexEntryPoint = "vertexMain";
    constexpr const char* clusterEntryPoint = "computeMain";
    constexpr const char* lightingEntryPoint = "fragmentMain";
#else
    constexpr const char* vertexEntryPoint = "main";
    constexpr const char* clusterEntryPoint = "main";
    constexpr const char* lightingEntryPoint = "main";
#endif
    const auto vertexBinary = readShader("Shaders/vertex.bin");
    const auto clusterBinary = readShader("Shaders/cluster.bin");
    const auto lightingBinary = readShader("Shaders/lighting.bin");

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
    lightingDesc.raster.fillMode = FillMode::Solid;
    lightingDesc.raster.cullMode = CullMode::None;
    lightingDesc.raster.frontFace = FrontFace::CounterClockwise;
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

        params.screenSize[0] = float(dimensions.width);
        params.screenSize[1] = float(dimensions.height);
        params.time = float(frame) / 60.0f;

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
            commands->SetViewport({0, 0, float(dimensions.width), float(dimensions.height)});
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
