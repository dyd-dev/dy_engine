#include "dyf/RHI.h"
#include "dyf/Platform/Window.h"
#include <iterator>
#include <fstream>
#include <cstdint>
#include <memory>
#include <cstdio>
#include <string>
#include <utility>
#include <chrono>
#include <vector>
using namespace dyf::RHI;
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
    const auto fragmentBinary = loadShader(dyf::RHI::ShaderStage::Fragment, "Shaders/fragment.glsl", "Shaders/fragment.bin");
#ifdef DY_EXAMPLE_METAL
    constexpr const char* vertexEntryPoint = "vertexMain";
    constexpr const char* fragmentEntryPoint = "fragmentMain";
#else
    constexpr const char* vertexEntryPoint = "main";
    constexpr const char* fragmentEntryPoint = "main";
#endif
    if(vertexBinary.empty() || fragmentBinary.empty()) return 1;

    dyf::Platform::Window window(800, 600, "Advanced / Frames and Present");
    DeviceDesc deviceDesc;
    deviceDesc.maxFramesInFlight = 2;
    std::unique_ptr<IDevice> owner(IDevice::Create(deviceDesc));

    auto& device = *owner;

    dyf::RHI::ResourceScope resources(device);

    SwapchainDesc swapchain;
    swapchain.window = window.GetHandle();
    swapchain.format = DefaultSwapchainFormat;
    swapchain.minimumImageCount = 2;
    swapchain.presentMode = PresentMode::Fifo;
    (void)device.CreateSwapchain(swapchain);

    auto* vertex = resources.Keep(device.CreateShader(
        {ShaderStage::Vertex, vertexEntryPoint, vertexBinary.data(), vertexBinary.size()}));
    auto* fragment = resources.Keep(device.CreateShader(
        {ShaderStage::Fragment, fragmentEntryPoint, fragmentBinary.data(), fragmentBinary.size()}));
    ColorAttachmentDesc colorFormat{swapchain.format, {}, ColorWriteMask::All};
    GraphicsPipelineDesc desc;
    desc.vertexShader = vertex;
    desc.fragmentShader = fragment;
    desc.topology = PrimitiveTopology::TriangleList;
    desc.raster = {FillMode::Solid, CullMode::None, FrontFace::CounterClockwise, 0, 0, 0};
    desc.colorAttachments = &colorFormat;
    desc.colorAttachmentCount = 1;
    desc.layout = {nullptr, 0, 16, ShaderStageFlags::Vertex | ShaderStageFlags::Fragment, 15};
    auto* pipeline = resources.Keep(device.CreateGraphicsPipeline(desc));

    std::vector<ICommandList*> lists(2);
    std::vector<FenceHandle> fences(2);
    for (auto& list : lists)
        list = resources.Keep(device.AcquireCommandList());
    const auto start = std::chrono::steady_clock::now();
    uint32_t frame = 0;

    while (window.IsRunning()) {
        window.PollEvents();
        if (!window.IsRunning()) break;

        const uint32_t slot = frame % 2;

        if (fences[slot]) (void)device.Wait(fences[slot], UINT64_MAX);
        (void)device.ResetCommandList(lists[slot]);
        if (!device.BeginFrame()) continue;

        auto* target = device.GetBackBuffer();
        auto* commands = lists[slot];
        ResourceBarrierDesc before{nullptr, target, ResourceState::Present, ResourceState::RenderTarget, {}};
        commands->ResourceBarrier(&before, 1);
        ColorAttachment color;
        color.texture = target;
        color.loadOp = LoadOp::Discard;
        color.storeOp = StoreOp::Store;
        commands->BeginRendering({&color, 1, nullptr});
        commands->BindGraphicsPipeline(pipeline);
        commands->SetViewport({0, 0, float(target->GetDesc().width), float(target->GetDesc().height), 0, 1});
        commands->SetScissor({0, 0, target->GetDesc().width, target->GetDesc().height});
        const float time = std::chrono::duration<float>(std::chrono::steady_clock::now() - start).count();
        const float settings[4] = {time, 0, 0, 0};
        commands->SetInlineConstants(0, sizeof(settings), settings);
        commands->DrawInstanced(3, 1, 0, 0);
        commands->EndRendering();
        ResourceBarrierDesc after{nullptr, target, ResourceState::RenderTarget, ResourceState::Present, {}};
        commands->ResourceBarrier(&after, 1);
        (void)commands->Close();
        (void)device.Submit({&commands, 1, nullptr, 0}, fences[slot]);

        (void)device.Present();
        ++frame;

    }

    (void)device.WaitIdle();
    return 0;
}
