#include "dyf/RHI.h"
#include "dyf/Platform/Window.h"
#include "vertex.h"
#include "fragment.h"
#include <chrono>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

using namespace dyf::RHI;

namespace {
// 일반 Z와 Reverse-Z를 비교할 실행 조건.
// 깊이 테스트·기록을 각각 끄거나, near/far 범위를 바꿔 결과를 관찰한다.
struct Options {
    uint32_t frames = 0;
    bool validation = false, reverse = false, noTest = false, noWrite = false;
    float nearPlane = .1f, farPlane = 100000.f;
    std::string capture;
};

// 실행 인자로 지정한 항목만 기본값에서 변경하고, 허용 범위를 검사한다.
Options Parse(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto value = [&]() -> std::string {
            if (++i == argc) throw std::runtime_error("Missing option value");
            return argv[i];
        };
        if (arg == "--frames")
            options.frames = static_cast<uint32_t>(std::stoul(value()));
        else if (arg == "--near")
            options.nearPlane = std::stof(value());
        else if (arg == "--far")
            options.farPlane = std::stof(value());
        else if (arg == "--capture")
            options.capture = value();
        else if (arg == "--validation")
            options.validation = true;
        else if (arg == "--reverse")
            options.reverse = true;
        else if (arg == "--no-depth-test")
            options.noTest = true;
        else if (arg == "--no-depth-write")
            options.noWrite = true;
        else
            throw std::runtime_error("Unknown option: " + arg);
    }
    if (!std::isfinite(options.nearPlane) || !std::isfinite(options.farPlane) || options.nearPlane <= 0 ||
        options.farPlane <= options.nearPlane)
        throw std::runtime_error("Invalid option range");
    if (!options.capture.empty() && !options.frames) options.frames = 1;
    return options;
}

// 실패한 작업의 이름을 예외로 전달해 main의 오류 처리에서 출력한다.
void Check(bool ok, const char* operation) {
    if (!ok) throw std::runtime_error(operation);
}

// GPU의 색상 이미지를 CPU로 읽어 PPM 파일로 저장한다.
// BGRA 형식도 파일에서는 RGB 순서가 되도록 채널을 맞춘다.
void SaveCapture(IDevice& device, TextureHandle target, const std::string& path) {
    TextureReadback readback;
    Check(device.ReadTexture(target, readback), "ReadTexture failed");
    std::ofstream file(path, std::ios::binary);
    Check(static_cast<bool>(file), "Cannot open capture");
    file << "P6\n" << readback.width << " " << readback.height << "\n255\n";
    const bool bgra = readback.format == Format::B8G8R8A8_UNORM || readback.format == Format::B8G8R8A8_UNORM_SRGB;
    for (uint32_t y = 0; y < readback.height; ++y)
        for (uint32_t x = 0; x < readback.width; ++x) {
            const auto* p = readback.pixels.data() + static_cast<size_t>(y) * readback.rowPitch + x * 4;
            const char rgb[3] = {static_cast<char>(p[bgra ? 2 : 0]), static_cast<char>(p[1]),
                                 static_cast<char>(p[bgra ? 0 : 2])};
            file.write(rgb, 3);
        }
    Check(static_cast<bool>(file), "Capture write failed");
}
}

// 도움말은 그래픽 장치 생성 없이 처리하고, 나머지 인자는 실행 설정에 반영한다.
int main(int argc, char** argv) try {
    for (int i = 1; i < argc; ++i)
        if (std::string(argv[i]) == "--help") {
            std::cout
                << "DepthReverseZ: --reverse --no-depth-test --no-depth-write --near 0.1 --far 100000; green is nearer than red.\nCommon: --frames N --capture path.ppm --validation\n";
            return 0;
        }
    const auto options = Parse(argc, argv);

    // 창과 그래픽 장치를 생성한다. 화면 그리기를 지원하지 않는 백엔드는 건너뛴다.
    dyf::Platform::Window window(800, 600, "Advanced / Depth and Reverse-Z");
    Check(window.GetHandle() != nullptr, "Window creation failed");
    DeviceDesc deviceDesc;
    deviceDesc.enableValidation = options.validation;
    std::unique_ptr<IDevice> owner(IDevice::Create(deviceDesc));
    Check(owner != nullptr, "Device creation failed");
    auto& device = *owner;
    if (!device.Supports(Feature::Rasterization)) {
        std::cout << "UNSUPPORTED: this backend does not rasterize.\n";
        return 77;
    }
    ResourceScope resources(device);

    // 화면 표시용 스왑체인을 준비하고, 캡처 요청이 있으면 이미지 읽기를 허용한다.
    SwapchainDesc swapchain;
    swapchain.window = window.GetHandle();
    swapchain.format = Format::B8G8R8A8_UNORM;
    swapchain.minimumImageCount = 2;
    swapchain.allowReadback = !options.capture.empty();

    Check(device.CreateSwapchain(swapchain), "Requested swapchain configuration unsupported");

    // 빌드 시 포함된 셰이더로 파이프라인을 구성한다. 실행 중 셰이더 파일을 읽지는 않는다.
    auto* vertex = resources.Keep(device.CreateShader(
        {ShaderStage::Vertex, ShaderData::vertexEntryPoint, ShaderData::vertex, ShaderData::vertexSize}));
    auto* fragment = resources.Keep(device.CreateShader(
        {ShaderStage::Fragment, ShaderData::fragmentEntryPoint, ShaderData::fragment, ShaderData::fragmentSize}));
    ColorAttachmentDesc colorFormat{swapchain.format, {}, ColorWriteMask::All};
    GraphicsPipelineDesc desc;
    desc.vertexShader = vertex;
    desc.fragmentShader = fragment;
    desc.topology = PrimitiveTopology::TriangleList;
    desc.raster = {FillMode::Solid, CullMode::None, FrontFace::CounterClockwise, 0, 0, 0};
    desc.colorAttachments = &colorFormat;
    desc.colorAttachmentCount = 1;
    desc.layout = {nullptr, 0, 16, ShaderStageFlags::Vertex | ShaderStageFlags::Fragment, 15};

    // 일반 Z는 작은 깊이, Reverse-Z는 큰 깊이를 앞쪽으로 판단한다.
    // 아래의 깊이 초기값과 셰이더 계산 방식도 같은 reverse 설정을 사용한다.
    desc.depthStencil = {};
    desc.depthStencil.format = Format::D32_FLOAT;
    desc.depthStencil.depthTestEnabled = !options.noTest;
    desc.depthStencil.depthWriteEnabled = !options.noWrite;
    desc.depthStencil.depthCompareOp = options.reverse ? CompareOp::Greater : CompareOp::Less;
    if (!device.Supports(desc)) {
        std::cout << "UNSUPPORTED: requested depth pipeline.\n";
        return 77;
    }
    auto* pipeline = resources.Keep(device.CreateGraphicsPipeline(desc));
    TextureHandle depth = nullptr;
    ResourceState depthState = ResourceState::Undefined;
    uint32_t frame = 0;
    std::cout << "Left: z=2/3. Right: z=10000/10000.002. Green should cover the shared region.\n";

    // 창이 닫히거나 지정한 프레임 수에 도달할 때까지 반복한다. frames가 0이면 횟수 제한이 없다.
    while (!options.frames || frame < options.frames) {
        window.PollEvents();
        if (!window.IsRunning()) break;
        if (!device.BeginFrame()) {
            Check(!device.IsLost(), "Device lost");
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        auto* target = device.GetBackBuffer();
        const auto& extent = target->GetDesc();

        // 첫 프레임과 화면 크기 변경 시, 화면과 같은 크기의 깊이 텍스처를 준비한다.
        if (!depth || depth->GetDesc().width != extent.width || depth->GetDesc().height != extent.height) {
            if (depth) device.DestroyTexture(depth);
            TextureDesc texture;
            texture.width = extent.width;
            texture.height = extent.height;
            texture.format = Format::D32_FLOAT;
            texture.usage = TextureUsage::DepthStencil;
            depth = device.CreateTexture(texture);
            Check(depth != nullptr, "Depth texture creation failed");
            depthState = ResourceState::Undefined;
        }

        // 색상·깊이 텍스처를 렌더링에 사용할 상태로 전환하는 명령부터 기록한다.
        ResourceScope frameResources(device);
        auto* commands = frameResources.Keep(device.AcquireCommandList());
        const ResourceBarrierDesc barriers[] = {
            {nullptr, target, ResourceState::Present, ResourceState::RenderTarget, {}},
            {nullptr, depth, depthState, ResourceState::DepthWrite, {}}};
        commands->ResourceBarrier(barriers, 2);

        // 이전 프레임의 색상과 깊이를 지운다. 가장 먼 깊이는 일반 Z에서 1, Reverse-Z에서 0이다.
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
        attachment.clearDepth = options.reverse ? 0.f : 1.f;
        commands->BeginRendering({&color, 1, &attachment});
        commands->BindGraphicsPipeline(pipeline);
        commands->SetViewport({0, 0, float(extent.width), float(extent.height), 0, 1});
        commands->SetScissor({0, 0, extent.width, extent.height});

        // Reverse-Z 여부와 near/far를 전달하고, 좌우에 빨강·초록 사각형을 한 쌍씩 그린다.
        // 정점 6개가 사각형 하나를 이루며, 네 사각형의 위치와 색상은 셰이더에서 결정한다.
        const float settings[4] = {options.reverse ? 1.f : 0.f, 0, options.nearPlane, options.farPlane};
        commands->SetInlineConstants(0, sizeof(settings), settings);
        commands->DrawInstanced(6, 4, 0, 0);
        commands->EndRendering();

        // 색상 이미지를 표시용 상태로 되돌린 뒤 기록한 명령을 GPU에 제출한다.
        ResourceBarrierDesc end{nullptr, target, ResourceState::RenderTarget, ResourceState::Present, {}};
        commands->ResourceBarrier(&end, 1);
        Check(commands->Close(), "Close failed");
        Check(device.Submit(&commands, 1), "Submit failed");
        depthState = ResourceState::DepthWrite;

        // 캡처는 마지막 프레임에서만 저장하고, 렌더링 결과를 창에 표시한다.
        if (!options.capture.empty() && frame + 1 == options.frames) SaveCapture(device, target, options.capture);
        Check(device.Present(), "Present failed");
        ++frame;
    }

    if (depth) device.DestroyTexture(depth);
    Check(device.WaitIdle(), "WaitIdle failed");
    return 0;
} catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
}
