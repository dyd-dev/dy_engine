#include "dyf/RHI.h"
#include "dyf/Platform/Window.h"
#include <vector>
#include <iterator>
#include <fstream>
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
    const auto lightingBinary = loadShader(dyf::RHI::ShaderStage::Fragment, "Shaders/lighting.glsl", "Shaders/lighting.bin");
#ifdef DY_EXAMPLE_METAL
    constexpr const char* vertexEntryPoint = "vertexMain";
    constexpr const char* lightingEntryPoint = "fragmentMain";
#else
    constexpr const char* vertexEntryPoint = "main";
    constexpr const char* lightingEntryPoint = "main";
#endif
    if(vertexBinary.empty() || lightingBinary.empty()) return 1;

    Params params;
    dyf::Platform::Window window(960, 640, "Advanced / 07_PBRLighting");
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
    auto lightingShader =
        resources.Keep(device.CreateShader({ShaderStage::Fragment, lightingEntryPoint,
                                            lightingBinary.data(), lightingBinary.size()}));
    ColorAttachmentDesc colorDesc{DefaultSwapchainFormat, {}, ColorWriteMask::All};
    GraphicsPipelineDesc lightingDesc;
    lightingDesc.vertexShader = vertexShader;
    lightingDesc.fragmentShader = lightingShader;
    lightingDesc.topology = PrimitiveTopology::TriangleList;
    lightingDesc.raster = {FillMode::Solid, CullMode::None, FrontFace::CounterClockwise, 0, 0, 0};
    lightingDesc.colorAttachments = &colorDesc;
    lightingDesc.colorAttachmentCount = 1;
    lightingDesc.layout = {nullptr, 0, sizeof(Params),
                           ShaderStageFlags::Fragment, 15};
    auto lightingPipeline = resources.Keep(device.CreateGraphicsPipeline(lightingDesc));

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
        imageWidth = dimensions.width;
        imageHeight = dimensions.height;

        ResourceScope frameResources(device);
        auto *commands = frameResources.Keep(device.AcquireCommandList());

        {
            const ResourceBarrierDesc backbufferBarrier{
                nullptr, backbuffer, ResourceState::Present, ResourceState::RenderTarget, {}};
            commands->ResourceBarrier(&backbufferBarrier, 1);
            ColorAttachment colorAttachment{};
            colorAttachment.texture = backbuffer;
            colorAttachment.loadOp = LoadOp::Clear;
            colorAttachment.storeOp = StoreOp::Store;
            commands->BeginRendering({&colorAttachment, 1, nullptr});
            commands->BindGraphicsPipeline(lightingPipeline);
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

        (void)device.Present();
        ++frame;
    }
    (void)device.WaitIdle();
}
