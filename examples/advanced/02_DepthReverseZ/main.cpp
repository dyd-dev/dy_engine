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

    dyf::Platform::Window window(800, 600, "Advanced / Depth and Reverse-Z");
    DeviceDesc deviceDesc;
    std::unique_ptr<IDevice> owner(IDevice::Create(deviceDesc));

    auto& device = *owner;

    dyf::RHI::ResourceScope resources(device);

    SwapchainDesc swapchain;
    swapchain.window = window.GetHandle();
    swapchain.format = DefaultSwapchainFormat;
    swapchain.minimumImageCount = 2;

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

    desc.depthStencil = {};
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
        commands->SetViewport({0, 0, float(extent.width), float(extent.height), 0, 1});
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
