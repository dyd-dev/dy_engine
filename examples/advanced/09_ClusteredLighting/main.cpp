#include "dyf/Platform/Window.h"
#include "dyf/RHI.h"

#include <iterator>
#include <fstream>
#include <cstdint>
#include <memory>
#include <cstdio>
#include <string>
#include <utility>
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
    const auto vertexBinary = loadShader(dyf::RHI::ShaderStage::Vertex, "Shaders/vertex.glsl", "Shaders/vertex.bin");
    const auto clusterBinary = loadShader(dyf::RHI::ShaderStage::Compute, "Shaders/cluster.glsl", "Shaders/cluster.bin");
    const auto lightingBinary = loadShader(dyf::RHI::ShaderStage::Fragment, "Shaders/lighting.glsl", "Shaders/lighting.bin");
#ifdef DY_EXAMPLE_METAL
    constexpr const char* vertexEntryPoint = "vertexMain";
    constexpr const char* clusterEntryPoint = "computeMain";
    constexpr const char* lightingEntryPoint = "fragmentMain";
#else
    constexpr const char* vertexEntryPoint = "main";
    constexpr const char* clusterEntryPoint = "main";
    constexpr const char* lightingEntryPoint = "main";
#endif
    if(vertexBinary.empty() || clusterBinary.empty() || lightingBinary.empty()) return 1;

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
