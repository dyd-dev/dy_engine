#include "dyf/RHI.h"
#include "dyf/Platform/Window.h"
#include <vector>
#include <iterator>
#include <fstream>
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

    // 예제 폴더에서 연결한 엔진의 백엔드에 맞는 명령으로 셰이더를 컴파일한다.
    // Metal의 macosx SDK는 대상에 따라 iphoneos 또는 iphonesimulator로 바꾼다.
    // Vulkan: glslc -fshader-stage=vert Shaders/vertex.glsl -o Shaders/vertex.bin
    // D3D12: dxc -T vs_6_0 -E main Shaders/vertex.hlsl -Fo Shaders/vertex.bin
    // Metal: xcrun -sdk macosx metal -c Shaders/vertex.metal -o Shaders/vertex.air
    //        xcrun -sdk macosx metallib Shaders/vertex.air -o Shaders/vertex.bin
    std::ifstream vertexFile("Shaders/vertex.bin", std::ios::binary);
    const std::vector<char> vertexBinary{
        std::istreambuf_iterator<char>(vertexFile), std::istreambuf_iterator<char>()};
    // Vulkan: glslc -fshader-stage=frag Shaders/lighting.glsl -o Shaders/lighting.bin
    // D3D12: dxc -T ps_6_0 -E main Shaders/lighting.hlsl -Fo Shaders/lighting.bin
    // Metal: xcrun -sdk macosx metal -c Shaders/lighting.metal -o Shaders/lighting.air
    //        xcrun -sdk macosx metallib Shaders/lighting.air -o Shaders/lighting.bin
    std::ifstream lightingFile("Shaders/lighting.bin", std::ios::binary);
    const std::vector<char> lightingBinary{
        std::istreambuf_iterator<char>(lightingFile), std::istreambuf_iterator<char>()};
#ifdef DY_EXAMPLE_METAL
    constexpr const char* vertexEntryPoint = "vertexMain";
    constexpr const char* lightingEntryPoint = "fragmentMain";
#else
    constexpr const char* vertexEntryPoint = "main";
    constexpr const char* lightingEntryPoint = "main";
#endif

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
