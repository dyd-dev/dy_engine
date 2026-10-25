#include "dyf/RHI.h"
#include "dyf/Platform/Window.h"
#include <vector>
#include <iterator>
#include <fstream>
#include <cstdint>
#include <memory>

using namespace dyf::RHI;

struct alignas(16) Params {
    float screenSize[2] = {};
    float exposure = 1;
    float roughness = .45f;
    float metallic = .8f;
};

int main() {
    const auto readShader = [](const char* path) {
        std::ifstream file(path, std::ios::binary);
        return std::vector<char>{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    };

    Params params;
    dyf::Platform::Window window(960, 640, "Advanced / 07_PBRLighting");
    std::unique_ptr<IDevice> owner(IDevice::Create({}));
    auto& device = *owner;
    ResourceScope resources(device);
    SwapchainDesc swapchain;
    swapchain.window = window.GetHandle();
    swapchain.format = DefaultSwapchainFormat;
    (void)device.CreateSwapchain(swapchain);

#ifdef __APPLE__
    constexpr const char* vertexEntryPoint = "vertexMain";
    constexpr const char* lightingEntryPoint = "fragmentMain";
#else
    constexpr const char* vertexEntryPoint = "main";
    constexpr const char* lightingEntryPoint = "main";
#endif
    const auto vertexBinary = readShader("Shaders/vertex.bin");
    const auto lightingBinary = readShader("Shaders/lighting.bin");

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
    lightingDesc.raster.fillMode = FillMode::Solid;
    lightingDesc.raster.cullMode = CullMode::None;
    lightingDesc.raster.frontFace = FrontFace::CounterClockwise;
    lightingDesc.colorAttachments = &colorDesc;
    lightingDesc.colorAttachmentCount = 1;
    lightingDesc.layout.inlineConstantSize = sizeof(Params);
    lightingDesc.layout.inlineConstantStages = ShaderStageFlags::Fragment;
    lightingDesc.layout.inlineConstantBinding = 15;
    auto lightingPipeline = resources.Keep(device.CreateGraphicsPipeline(lightingDesc));

    while (window.IsRunning()) {
        window.PollEvents();
        if (!window.IsRunning()) break;
        if (!device.BeginFrame()) continue;
        auto backbuffer = device.GetBackBuffer();
        params.screenSize[0] = float(backbuffer->GetDesc().width);
        params.screenSize[1] = float(backbuffer->GetDesc().height);

        const auto dimensions = backbuffer->GetDesc();

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
            commands->SetViewport({0, 0, float(dimensions.width), float(dimensions.height)});
            commands->SetScissor({0, 0, dimensions.width, dimensions.height});
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
    }
    (void)device.WaitIdle();
}
