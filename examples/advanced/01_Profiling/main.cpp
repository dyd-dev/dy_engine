#include "dyf/RHI.h"
#include "dyf/Platform/Window.h"
#include "dyf/Platform/Profiler.h"

#include <vector>
#include <iterator>
#include <fstream>
#include <array>
#include <chrono>
#include <cstdint>
#include <iostream>
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

    dyf::Platform::Window window(800, 600, "Advanced / 01_Profiling");
    std::unique_ptr<IDevice> owner(IDevice::Create({}));
    auto& device = *owner;
    ResourceScope resources(device);
    SwapchainDesc swapchain;
    swapchain.window = window.GetHandle();
    swapchain.format = DefaultSwapchainFormat;
    swapchain.minimumImageCount = 2;
    (void)device.CreateSwapchain(swapchain);

    auto* vertex = resources.Keep(device.CreateShader({
        ShaderStage::Vertex,
        vertexEntryPoint,
        vertexBinary.data(),
        vertexBinary.size()
    }));
    auto* fragment = resources.Keep(device.CreateShader({
        ShaderStage::Fragment,
        fragmentEntryPoint,
        fragmentBinary.data(),
        fragmentBinary.size()
    }));

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

    // 슬롯당 두 타임스탬프를 기록하고 해당 제출의 펜스가 끝난 뒤 읽는다.
    const bool gpuTiming = device.Supports(Feature::TimestampQuery);
    auto query = gpuTiming ? resources.Keep(device.CreateTimestampQuery({4})) : nullptr;
    std::array<ICommandList*, 2> lists{};
    std::array<FenceHandle, 2> fences{};
    for (auto& list : lists) list = resources.Keep(device.AcquireCommandList());
    uint64_t frame = 0;
    double gpuMilliseconds = 0;

    while (window.IsRunning()) {
        window.PollEvents();
        if (!window.IsRunning()) break;
        const uint32_t slot = frame % 2;
        if (fences[slot]) {
            (void)device.Wait(fences[slot], UINT64_MAX);
            if (query) {
                uint64_t ticks[2]{};
                (void)device.ReadTimestamps(query, slot * 2, 2, ticks);
                const auto bits = device.GetTimestampValidBits();
                const uint64_t mask = bits >= 64 ? UINT64_MAX : (uint64_t{1} << bits) - 1;
                gpuMilliseconds = double((ticks[1] - ticks[0]) & mask) *
                    device.GetTimestampPeriodNanoseconds() / 1e6;
            }
        }
        (void)device.ResetCommandList(lists[slot]);
        if (!device.BeginFrame()) continue;
        auto* target = device.GetBackBuffer();
        auto* commands = lists[slot];
        const auto recordStart = std::chrono::steady_clock::now();
        DY_PROFILE_CPU_ZONE_NAMED("AdvancedProfiling::Record");
        commands->BeginDebugEvent("Profiling pass");
        if (query) {
            commands->ResetTimestamps(query, slot * 2, 2);
            commands->WriteTimestamp(query, slot * 2);
        }
        ResourceBarrierDesc begin{nullptr, target, ResourceState::Present, ResourceState::RenderTarget, {}};
        commands->ResourceBarrier(&begin, 1);
        ColorAttachment color;
        color.texture = target;
        color.loadOp = LoadOp::Clear;
        color.storeOp = StoreOp::Store;
        color.clearColor[3] = 1;
        commands->BeginRendering({&color, 1, nullptr});
        commands->BindGraphicsPipeline(pipeline);
        commands->SetViewport({0, 0, float(target->GetDesc().width), float(target->GetDesc().height), 0, 1});
        commands->SetScissor({0, 0, target->GetDesc().width, target->GetDesc().height});
        const float parameters[4] = {96, 0, 0, 0};
        commands->SetInlineConstants(0, sizeof(parameters), parameters);
        commands->DrawInstanced(3, 1, 0, 0);
        commands->EndRendering();
        ResourceBarrierDesc end{nullptr, target, ResourceState::RenderTarget, ResourceState::Present, {}};
        commands->ResourceBarrier(&end, 1);
        if (query) commands->WriteTimestamp(query, slot * 2 + 1);
        commands->EndDebugEvent();
        (void)commands->Close();
        const auto submitStart = std::chrono::steady_clock::now();
        (void)device.Submit({&commands, 1, nullptr, 0}, fences[slot]);
        const auto submitEnd = std::chrono::steady_clock::now();
        (void)device.Present();
        DY_PROFILE_FRAME_MARK();

        if (++frame % 120 == 0) {
            std::cout << "CPU record: "
                << std::chrono::duration<double, std::milli>(submitStart - recordStart).count()
                << " ms, CPU submit: "
                << std::chrono::duration<double, std::milli>(submitEnd - submitStart).count() << " ms";
            if (query) std::cout << ", GPU pass: " << gpuMilliseconds << " ms";
            std::cout << '\n';
        }
    }
    (void)device.WaitIdle();
}
