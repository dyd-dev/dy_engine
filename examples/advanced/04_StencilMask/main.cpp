#include "dyf/RHI.h"
#include "dyf/Platform/Window.h"
#include "vertex.h"
#include "fragment.h"
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
using namespace dyf::RHI;
namespace {
// 같은 스텐실 영역을 기준으로 마스크와 외곽선 결과를 비교할 실행 설정.
struct Options {
    uint32_t frames = 0, reference = 1;
    bool validation = false;
    std::string mode = "mask", capture;
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
        else if (arg == "--reference")
            options.reference = static_cast<uint32_t>(std::stoul(value()));
        else if (arg == "--mode")
            options.mode = value();
        else if (arg == "--capture")
            options.capture = value();
        else if (arg == "--validation")
            options.validation = true;
        else
            throw std::runtime_error("Unknown option: " + arg);
    }
    if (options.reference > 255) throw std::runtime_error("Invalid option range");
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
                << "StencilMask: --mode mask|outline --reference 0..255; circle is written with stencil reference 1.\nCommon: --frames N --capture path.ppm --validation\n";
            return 0;
        }
    const auto options = Parse(argc, argv);

    // 창과 그래픽 장치를 생성한다. 화면 그리기를 지원하지 않는 백엔드는 건너뛴다.
    dyf::Platform::Window window(800, 600, "Advanced / Stencil mask");
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

    // 첫 번째 파이프라인은 색상 없이 스텐실 값만 기록한다.
    // D24S8을 지원하지 않으면 D32S8을 사용하고, 텍스처에도 선택한 형식을 적용한다.
    Check(options.mode == "default" || options.mode == "mask" || options.mode == "outline", "Invalid mode");
    auto& stencil = desc.depthStencil;
    stencil.format = Format::D24_UNORM_S8_UINT;
    stencil.stencilEnabled = true;
    stencil.stencilReadMask = 255;
    stencil.stencilWriteMask = 255;
    stencil.front = stencil.back = {StencilOp::Keep, StencilOp::Keep, StencilOp::Replace, CompareOp::Always};
    colorFormat.writeMask = ColorWriteMask::None;
    if (!device.Supports(desc)) stencil.format = Format::D32_FLOAT_S8_UINT;
    if (!device.Supports(desc)) {
        std::cout << "UNSUPPORTED: no supported depth/stencil format.\n";
        return 77;
    }
    auto* stamp = resources.Keep(device.CreateGraphicsPipeline(desc));

    // 두 번째 파이프라인은 스텐실을 바꾸지 않고 비교 결과에 따라 색상만 출력한다.
    // 마스크는 기준값과 같은 영역, 외곽선은 다른 영역을 사용한다.
    colorFormat.writeMask = ColorWriteMask::All;
    stencil.stencilWriteMask = 0;
    stencil.front = stencil.back = {StencilOp::Keep, StencilOp::Keep, StencilOp::Keep,
                                    options.mode == "outline" ? CompareOp::NotEqual : CompareOp::Equal};
    auto* display = resources.Keep(device.CreateGraphicsPipeline(desc));
    TextureHandle depth = nullptr;
    ResourceState depthState = ResourceState::Undefined;
    uint32_t frame = 0;

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
            texture.format = stencil.format;
            texture.usage = TextureUsage::DepthStencil;
            depth = device.CreateTexture(texture);
            Check(depth != nullptr, "Depth/stencil texture creation failed");
            depthState = ResourceState::Undefined;
        }

        // 색상·깊이 텍스처를 렌더링에 사용할 상태로 전환하는 명령부터 기록한다.
        ResourceScope frameResources(device);
        auto* commands = frameResources.Keep(device.AcquireCommandList());
        ResourceBarrierDesc before[] = {{nullptr, target, ResourceState::Present, ResourceState::RenderTarget, {}},
                                        {nullptr, depth, depthState, ResourceState::DepthWrite, {}}};
        commands->ResourceBarrier(before, 2);

        // 배경색과 깊이·스텐실을 초기화한다. 스텐실을 0으로 지워 이번 프레임의 마스크를 새로 만든다.
        ColorAttachment color;
        color.texture = target;
        color.loadOp = LoadOp::Clear;
        color.storeOp = StoreOp::Store;
        color.clearColor[0] = .04f;
        color.clearColor[1] = .05f;
        color.clearColor[2] = .08f;
        color.clearColor[3] = 1;
        DepthStencilAttachment attachment;
        attachment.texture = depth;
        attachment.state = ResourceState::DepthWrite;
        attachment.depthLoadOp = attachment.stencilLoadOp = LoadOp::Clear;
        attachment.depthStoreOp = attachment.stencilStoreOp = StoreOp::Store;
        attachment.clearStencil = 0;
        commands->BeginRendering({&color, 1, &attachment});
        commands->SetViewport({0, 0, float(extent.width), float(extent.height), 0, 1});
        commands->SetScissor({0, 0, extent.width, extent.height});

        // 원 모양 영역에 스텐실 값 1을 기록한 뒤, 사용자가 정한 reference와 비교해 표시한다.
        commands->BindGraphicsPipeline(stamp);
        commands->SetStencilReference(1);
        float settings[4] = {0, float(extent.width) / float(extent.height), 0, 0};
        commands->SetInlineConstants(0, sizeof(settings), settings);
        commands->DrawInstanced(3, 1, 0, 0);
        commands->BindGraphicsPipeline(display);
        commands->SetStencilReference(options.reference);
        settings[0] = 1;
        settings[2] = options.mode == "outline" ? 1.f : 0.f;
        commands->SetInlineConstants(0, sizeof(settings), settings);
        commands->DrawInstanced(3, 1, 0, 0);

        // 두 단계의 렌더링을 마치고, 색상 이미지를 표시용 상태로 전환해 제출한다.
        commands->EndRendering();
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
