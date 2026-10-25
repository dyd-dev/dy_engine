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

    // 예제 폴더에서 연결한 엔진의 백엔드에 맞는 명령으로 셰이더를 컴파일한다.
    // Metal의 macosx SDK는 대상에 따라 iphoneos 또는 iphonesimulator로 바꾼다.
    // Vulkan: glslc -fshader-stage=vert Shaders/vertex.glsl -o Shaders/vertex.bin
    // D3D12: dxc -T vs_6_0 -E main Shaders/vertex.hlsl -Fo Shaders/vertex.bin
    // Metal: xcrun -sdk macosx metal -c Shaders/program.metal -o Shaders/vertex.air
    //        xcrun -sdk macosx metallib Shaders/vertex.air -o Shaders/vertex.bin
    std::ifstream vertexFile("Shaders/vertex.bin", std::ios::binary);
    const std::vector<char> vertexBinary{
        std::istreambuf_iterator<char>(vertexFile), std::istreambuf_iterator<char>()};
    // Vulkan: glslc -fshader-stage=frag Shaders/fragment.glsl -o Shaders/fragment.bin
    // D3D12: dxc -T ps_6_0 -E main Shaders/fragment.hlsl -Fo Shaders/fragment.bin
    // Metal: xcrun -sdk macosx metal -c Shaders/program.metal -o Shaders/fragment.air
    //        xcrun -sdk macosx metallib Shaders/fragment.air -o Shaders/fragment.bin
    std::ifstream fragmentFile("Shaders/fragment.bin", std::ios::binary);
    const std::vector<char> fragmentBinary{
        std::istreambuf_iterator<char>(fragmentFile), std::istreambuf_iterator<char>()};
#ifdef DY_EXAMPLE_METAL
    constexpr const char* vertexEntryPoint = "vertexMain";
    constexpr const char* fragmentEntryPoint = "fragmentMain";
#else
    constexpr const char* vertexEntryPoint = "main";
    constexpr const char* fragmentEntryPoint = "main";
#endif

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
