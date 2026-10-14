#include "dyf/Platform/Window.h"
#include "dyf/RHI.h"
#include "vertex.h"
#include "fragment.h"
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace RHI = dyf::RHI;
using Clock = std::chrono::steady_clock;

namespace {
void Check(bool ok, const char* operation) {
    if (!ok) throw std::runtime_error(operation);
}

// 비교할 실행 모드와 프레임 수, 캡처·검증 옵션을 보관한다.
struct Options {
    std::string mode;
    uint32_t frames = 0;
    std::string capture;
    bool validation = false;
};

// 실행 인자를 읽고 프레임 수가 유효한 정수인지 확인한다. 도움말 요청이면 실행을 마친다.
bool Parse(int argc, char** argv, Options& options, const char* modes, const char* description) {
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "--help") {
            std::printf("%s\n--mode %s --frames N (0 = unlimited) --capture image.ppm --validation\n", description,
                        modes);
            return false;
        }
        if (argument == "--validation")
            options.validation = true;
        else if ((argument == "--mode" || argument == "--frames" || argument == "--capture") && i + 1 < argc) {
            const std::string value = argv[++i];
            if (argument == "--mode")
                options.mode = value;
            else if (argument == "--capture")
                options.capture = value;
            else {
                char* end = nullptr;
                const auto count = std::strtoull(value.c_str(), &end, 10);
                Check(!value.empty() && value[0] != '-' && end && *end == '\0' && count <= UINT32_MAX,
                      "--frames requires an unsigned 32-bit integer");
                options.frames = static_cast<uint32_t>(count);
            }
        } else
            throw std::runtime_error("Unknown or incomplete argument; use --help");
    }
    return true;
}

// GPU의 색상 이미지를 읽어 RGB 순서의 PPM 파일로 저장한다.
void Capture(RHI::IDevice& device, RHI::TextureHandle texture, const std::string& path) {
    RHI::TextureReadback image;
    Check(device.ReadTexture(texture, image), "ReadTexture failed (this backend may not support pixel readback)");
    std::ofstream file(path, std::ios::binary);
    Check(bool(file), "Cannot open capture file");
    file << "P6\n" << image.width << ' ' << image.height << "\n255\n";
    const bool bgra = image.format == RHI::Format::B8G8R8A8_UNORM || image.format == RHI::Format::B8G8R8A8_UNORM_SRGB;
    for (uint32_t y = 0; y < image.height; ++y)
        for (uint32_t x = 0; x < image.width; ++x) {
            const auto* p = image.pixels.data() + size_t(y) * image.rowPitch + size_t(x) * 4;
            const char rgb[3] = {char(p[bgra ? 2 : 0]), char(p[1]), char(p[bgra ? 0 : 2])};
            file.write(rgb, 3);
        }
    Check(bool(file), "Capture write failed");
    std::printf("capture=%s (%ux%u)\n", path.c_str(), image.width, image.height);
}
}

int main(int argc, char** argv) {
    try {
        // 같은 사각형들을 개별 호출로 그릴지, 한 번의 인스턴싱 호출로 그릴지 선택한다.
        Options options;
        options.mode = "instanced";
        if (!Parse(argc, argv, options, "draws|instanced",
                   "Identical picture: 256 individual draws versus one instanced draw."))
            return 0;
        Check(options.mode == "draws" || options.mode == "instanced", "Invalid --mode");

        // 창과 그래픽 장치, 캡처 옵션을 반영한 스왑체인을 준비한다.
        dyf::Platform::Window window(800, 600, "Advanced / Instancing and batching");
        Check(window.GetHandle() != nullptr, "Window creation failed");
        RHI::DeviceDesc deviceDesc;
        deviceDesc.enableValidation = options.validation;
        std::unique_ptr<RHI::IDevice> deviceOwner(RHI::IDevice::Create(deviceDesc));
        Check(bool(deviceOwner), "Device creation failed");
        auto& device = *deviceOwner;
        RHI::ResourceScope resources(device);
        RHI::SwapchainDesc swapchain;
        swapchain.window = window.GetHandle();
        swapchain.format = RHI::Format::B8G8R8A8_UNORM;
        swapchain.minimumImageCount = 2;
        swapchain.presentMode = RHI::PresentMode::Fifo;
        swapchain.allowReadback = !options.capture.empty();
        if (ShaderData::vertexSize == 0) {
            swapchain.initialWidth = 800;
            swapchain.initialHeight = 600;
        }
        Check(device.CreateSwapchain(swapchain), "Swapchain creation failed");

        // 사각형 하나의 정점과 격자 전체의 인스턴스 데이터를 만든다.
        // 두 실행 모드는 이 데이터를 함께 사용하므로 호출 묶음만 달라진다.
        struct Vertex {
            float x, y;
        };
        struct Instance {
            float x, y, scaleX, scaleY, r, g, b, a;
        };
        const std::array<Vertex, 6> vertices{
            {{-0.5f, -0.5f}, {0.5f, -0.5f}, {0.5f, 0.5f}, {-0.5f, -0.5f}, {0.5f, 0.5f}, {-0.5f, 0.5f}}};
        constexpr uint32_t side = 16, objectCount = side * side;
        std::array<Instance, objectCount> instances{};
        for (uint32_t i = 0; i < objectCount; ++i) {
            const uint32_t x = i % side, y = i / side;
            instances[i] = {
                -0.9f + (x + 0.5f) * 1.8f / side, -0.9f + (y + 0.5f) * 1.8f / side, 1.55f / side, 1.55f / side,
                0.2f + 0.7f * x / (side - 1),     0.2f + 0.7f * y / (side - 1),     0.8f,         1.0f};
        }
        RHI::BufferDesc vertexDesc;
        vertexDesc.size = sizeof(vertices);
        vertexDesc.usage = RHI::BufferUsage::Vertex;
        vertexDesc.initialState = RHI::ResourceState::CopyDestination;
        auto* vertexBuffer = resources.Keep(device.CreateBuffer(vertexDesc));
        RHI::BufferDesc instanceDesc;
        instanceDesc.size = sizeof(instances);
        instanceDesc.usage = RHI::BufferUsage::Vertex;
        instanceDesc.initialState = RHI::ResourceState::CopyDestination;
        auto* instanceBuffer = resources.Keep(device.CreateBuffer(instanceDesc));

        // 같은 셰이더에 정점별 좌표와 인스턴스별 위치·크기·색상을 서로 다른 입력 슬롯으로 전달한다.
        auto* vs = resources.Keep(device.CreateShader(
            {RHI::ShaderStage::Vertex, ShaderData::vertexEntryPoint, ShaderData::vertex, ShaderData::vertexSize}));
        auto* fs = resources.Keep(device.CreateShader({RHI::ShaderStage::Fragment, ShaderData::fragmentEntryPoint,
                                                       ShaderData::fragment, ShaderData::fragmentSize}));
        const std::array<RHI::VertexBufferLayout, 2> input{
            {{0, sizeof(Vertex), RHI::VertexStepMode::Vertex}, {1, sizeof(Instance), RHI::VertexStepMode::Instance}}};
        const std::array<RHI::VertexAttribute, 3> attributes{{{0, 0, RHI::Format::R32G32_FLOAT, 0},
                                                              {1, 1, RHI::Format::R32G32B32A32_FLOAT, 0},
                                                              {2, 1, RHI::Format::R32G32B32A32_FLOAT, 16}}};
        RHI::ColorAttachmentDesc output{swapchain.format, {}, RHI::ColorWriteMask::All};
        RHI::GraphicsPipelineDesc pipelineDesc;
        pipelineDesc.vertexShader = vs;
        pipelineDesc.fragmentShader = fs;
        pipelineDesc.topology = RHI::PrimitiveTopology::TriangleList;
        pipelineDesc.vertexBuffers = input.data();
        pipelineDesc.vertexBufferCount = uint32_t(input.size());
        pipelineDesc.vertexAttributes = attributes.data();
        pipelineDesc.vertexAttributeCount = uint32_t(attributes.size());
        pipelineDesc.raster = {RHI::FillMode::Solid, RHI::CullMode::None, RHI::FrontFace::CounterClockwise, 0, 0, 0};
        pipelineDesc.colorAttachments = &output;
        pipelineDesc.colorAttachmentCount = 1;
        auto* pipeline = resources.Keep(device.CreateGraphicsPipeline(pipelineDesc));

        uint32_t frame = 0;
        uint64_t drawCalls = 0;
        double recordMilliseconds = 0.0;
        bool captured = false;

        // 렌더링 가능한 프레임에서 명령을 기록하고, 지정한 프레임 수 또는 창 종료까지 반복한다.
        while (window.IsRunning() && (options.frames == 0 || frame < options.frames)) {
            window.PollEvents();
            if (!window.IsRunning()) break;
            if (!device.BeginFrame()) {
                Check(!device.IsLost(), "Device lost during BeginFrame");
                continue; // 최소화·크기 변경으로 준비되지 않은 프레임은 세지 않는다.
            }
            // 제출한 명령이 GPU 완료까지 자원을 유지하므로 이 프레임의 ResourceScope는 끝에서 해제할 수 있다.
            RHI::ResourceScope frameResources(device);
            auto* commands = frameResources.Keep(device.AcquireCommandList());

            // 첫 프레임에는 정점·인스턴스 데이터를 업로드한다. 이 예제의 CPU 기록 시간에는 업로드 명령 기록도 포함된다.
            const auto recordStart = Clock::now();
            if (frame == 0) {
                Check(device.UpdateBuffer(*commands, vertexBuffer, 0, vertices.data(), sizeof(vertices)),
                      "Vertex upload failed");
                const RHI::ResourceBarrierDesc uploaded{
                    vertexBuffer, nullptr, RHI::ResourceState::CopyDestination, RHI::ResourceState::VertexBuffer, {}};
                commands->ResourceBarrier(&uploaded, 1);
            }
            if (frame == 0) {
                Check(device.UpdateBuffer(*commands, instanceBuffer, 0, instances.data(), sizeof(instances)),
                      "Instance upload failed");
                const RHI::ResourceBarrierDesc uploaded{
                    instanceBuffer, nullptr, RHI::ResourceState::CopyDestination, RHI::ResourceState::VertexBuffer, {}};
                commands->ResourceBarrier(&uploaded, 1);
            }

            auto* backBuffer = device.GetBackBuffer(); // 크기 변경이 반영된 현재 이미지를 사용한다.
            const auto& extent = backBuffer->GetDesc();
            const RHI::ResourceBarrierDesc begin{
                nullptr, backBuffer, RHI::ResourceState::Present, RHI::ResourceState::RenderTarget, {}};
            commands->ResourceBarrier(&begin, 1);
            RHI::ColorAttachment color;
            color.texture = backBuffer;
            color.loadOp = RHI::LoadOp::Clear;
            color.storeOp = RHI::StoreOp::Store;
            color.clearColor[0] = 0.025f;
            color.clearColor[1] = 0.035f;
            color.clearColor[2] = 0.055f;
            color.clearColor[3] = 1.0f;
            commands->BeginRendering({&color, 1, nullptr});
            commands->BindGraphicsPipeline(pipeline);
            commands->BindVertexBuffer(0, vertexBuffer, 0);
            commands->BindVertexBuffer(1, instanceBuffer, 0);
            commands->SetViewport({0, 0, float(extent.width), float(extent.height), 0, 1});
            commands->SetScissor({0, 0, extent.width, extent.height});

            // 인스턴싱은 전체 객체를 한 번에 그린다. 개별 모드는 같은 데이터를 객체마다 한 번씩 호출한다.
            if (options.mode == "instanced") {
                commands->DrawInstanced(uint32_t(vertices.size()), objectCount, 0, 0);
                ++drawCalls;
            } else {
                // firstInstance로 같은 인스턴스 속성의 i번째 원소를 선택한다.
                for (uint32_t i = 0; i < objectCount; ++i)
                    commands->DrawInstanced(uint32_t(vertices.size()), 1, 0, i);
                drawCalls += objectCount;
            }
            commands->EndRendering();

            // 그리기를 마친 뒤 CPU 명령 기록 시간을 합산하고 GPU에 제출한다. 캡처와 화면 표시는 그 뒤에 수행한다.
            const RHI::ResourceBarrierDesc end{
                nullptr, backBuffer, RHI::ResourceState::RenderTarget, RHI::ResourceState::Present, {}};
            commands->ResourceBarrier(&end, 1);
            Check(commands->Close(), "Command recording failed");
            recordMilliseconds += std::chrono::duration<double, std::milli>(Clock::now() - recordStart).count();
            Check(device.Submit(&commands, 1), "Submission failed");
            if (!captured && !options.capture.empty() && (options.frames == 0 || frame + 1 == options.frames)) {
                Capture(device, backBuffer, options.capture);
                captured = true;
            }
            Check(device.Present(), "Present failed");
            ++frame;
        }

        // GPU 작업 완료를 기다린 뒤 호출 횟수와 CPU 기록 시간을 출력한다. GPU 실행 시간은 측정하지 않는다.
        Check(device.WaitIdle(), "WaitIdle failed");
        std::printf("mode=%s frames=%u objects=%u draw_calls=%llu cpu_record_ms=%.3f avg_record_us=%.3f\n",
                    options.mode.c_str(), frame, objectCount, static_cast<unsigned long long>(drawCalls),
                    recordMilliseconds, frame ? recordMilliseconds * 1000.0 / frame : 0.0);
        std::puts(
            "Both modes use the same instance-rate attributes and pixels; only draw grouping differs. CPU recording time excludes GPU execution, present and capture.");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
