#include "dyf/RHI.h"
#include "dyf/Platform/Window.h"
#include <iterator>
#include <fstream>
#include <cstdint>
#include <memory>
#include <chrono>
#include <vector>
using namespace dyf::RHI;
int main() {
    const auto readShader = [](const char* path) {
        std::ifstream file(path, std::ios::binary);
        return std::vector<char>{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    };

    dyf::Platform::Window window(800, 600, "Advanced / Frames and Present");
    std::unique_ptr<IDevice> owner(IDevice::Create({}));

    auto& device = *owner;

    dyf::RHI::ResourceScope resources(device);

    SwapchainDesc swapchain;
    swapchain.window = window.GetHandle();
    swapchain.format = DefaultSwapchainFormat;
    (void)device.CreateSwapchain(swapchain);

#ifdef __APPLE__
    constexpr const char* vertexEntryPoint = "vertexMain";
    constexpr const char* fragmentEntryPoint = "fragmentMain";
#else
    constexpr const char* vertexEntryPoint = "main";
    constexpr const char* fragmentEntryPoint = "main";
#endif
    const auto vertexBinary = readShader("Shaders/vertex.bin");
    const auto fragmentBinary = readShader("Shaders/fragment.bin");

    auto* vertex = resources.Keep(device.CreateShader(
        {ShaderStage::Vertex, vertexEntryPoint, vertexBinary.data(), vertexBinary.size()}));
    auto* fragment = resources.Keep(device.CreateShader(
        {ShaderStage::Fragment, fragmentEntryPoint, fragmentBinary.data(), fragmentBinary.size()}));
    ColorAttachmentDesc colorFormat{swapchain.format, {}, ColorWriteMask::All};
    GraphicsPipelineDesc desc;
    desc.vertexShader = vertex;
    desc.fragmentShader = fragment;
    desc.topology = PrimitiveTopology::TriangleList;
    desc.raster.fillMode = FillMode::Solid;
    desc.raster.cullMode = CullMode::None;
    desc.raster.frontFace = FrontFace::CounterClockwise;
    desc.colorAttachments = &colorFormat;
    desc.colorAttachmentCount = 1;
    desc.layout.inlineConstantSize = 16;
    desc.layout.inlineConstantStages = ShaderStageFlags::Vertex | ShaderStageFlags::Fragment;
    desc.layout.inlineConstantBinding = 15;
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
        commands->SetViewport({0, 0, float(target->GetDesc().width), float(target->GetDesc().height)});
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
