#include "dyf/RHI.h"
#include "dyf/Platform/Window.h"
#include <vector>
#include <iterator>
#include <fstream>
#include <cstdint>
#include <memory>

using namespace dyf::RHI;

int main() {
    const auto readShader = [](const char* path) {
        std::ifstream file(path, std::ios::binary);
        return std::vector<char>{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    };

    dyf::Platform::Window window(800, 600, "Advanced / Depth and Reverse-Z");
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

    desc.depthStencil.format = Format::D32_FLOAT;
    desc.depthStencil.depthTestEnabled = true;
    desc.depthStencil.depthWriteEnabled = true;
    desc.depthStencil.depthCompareOp = CompareOp::Greater;

    auto* pipeline = resources.Keep(device.CreateGraphicsPipeline(desc));
    TextureHandle depth = nullptr;
    ResourceState depthState = ResourceState::Undefined;

    while (window.IsRunning()) {
        window.PollEvents();
        if (!window.IsRunning()) break;
        if (!device.BeginFrame()) continue;
        auto* target = device.GetBackBuffer();
        const auto& extent = target->GetDesc();

        if (!depth || depth->GetDesc().width != extent.width || depth->GetDesc().height != extent.height) {
            if (depth) device.DestroyTexture(depth);
            TextureDesc texture;
            texture.width = extent.width;
            texture.height = extent.height;
            texture.format = Format::D32_FLOAT;
            texture.usage = TextureUsage::DepthStencil;
            depth = device.CreateTexture(texture);

            depthState = ResourceState::Undefined;
        }

        dyf::RHI::ResourceScope frameResources(device);
        auto* commands = frameResources.Keep(device.AcquireCommandList());
        const ResourceBarrierDesc barriers[] = {
            {nullptr, target, ResourceState::Present, ResourceState::RenderTarget, {}},
            {nullptr, depth, depthState, ResourceState::DepthWrite, {}}};
        commands->ResourceBarrier(barriers, 2);

        ColorAttachment color;
        color.texture = target;
        color.loadOp = LoadOp::Clear;
        color.storeOp = StoreOp::Store;
        color.clearColor[0] = .03f;
        color.clearColor[1] = .04f;
        color.clearColor[2] = .07f;
        color.clearColor[3] = 1;
        DepthStencilAttachment attachment;
        attachment.texture = depth;
        attachment.state = ResourceState::DepthWrite;
        attachment.depthLoadOp = LoadOp::Clear;
        attachment.depthStoreOp = StoreOp::Store;
        attachment.clearDepth = 0.f;
        commands->BeginRendering({&color, 1, &attachment});
        commands->BindGraphicsPipeline(pipeline);
        commands->SetViewport({0, 0, float(extent.width), float(extent.height)});
        commands->SetScissor({0, 0, extent.width, extent.height});

        const float settings[4] = {1.f, 0, .1f, 100000.f};
        commands->SetInlineConstants(0, sizeof(settings), settings);
        commands->DrawInstanced(6, 4, 0, 0);
        commands->EndRendering();

        ResourceBarrierDesc end{nullptr, target, ResourceState::RenderTarget, ResourceState::Present, {}};
        commands->ResourceBarrier(&end, 1);
        (void)commands->Close();
        (void)device.Submit(&commands, 1);
        depthState = ResourceState::DepthWrite;

        (void)device.Present();
    }

    if (depth) device.DestroyTexture(depth);
    (void)device.WaitIdle();
    return 0;
}
