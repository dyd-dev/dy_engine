#include "dyf/RHI.h"
#include "dyf/Platform/Window.h"
#include "vertex.h"
#include "fragment.h"
#include "producer.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace dyf::RHI;

namespace {

// =============================================================================
// [설정값 및 옵션 정의]
// 깊이 샘플링 예제의 실행 옵션 (CLI 인자 파싱 결과 저장)
// =============================================================================
struct Options {
    uint32_t frames = 0;           // 실행할 총 프레임 수 (0이면 무한 반복)
    uint32_t flight = 2;           // 동시 In-flight 프레임 수 (1 ~ 4)
    bool validation = false;       // RHI 유효성 검사 계층 활성화 여부
    bool wait = false;             // 각 패스 종료 후 GPU 완료 대기(Fence Wait) 여부
    float nearPlane = 0.1f;        // 투영 원근 투영 Near 평면 거리
    float farPlane = 100.0f;       // 투영 Far 평면 거리
    std::string mode = "default";  // 깊이 표시 모드: "default", "split", "raw", "linear"
    std::string capture;           // 결과 캡처 이미지 경로 (.ppm)
};

// 명령행 인자 파싱 함수
Options Parse(int argc, char** argv) {
    Options options;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];

        auto value = [&]() -> std::string {
            if (++i == argc) {
                throw std::runtime_error("Missing option value");
            }
            return argv[i];
        };

        if (arg == "--frames") {
            options.frames = uint32_t(std::stoul(value()));
        } else if (arg == "--flight") {
            options.flight = uint32_t(std::stoul(value()));
        } else if (arg == "--near") {
            options.nearPlane = std::stof(value());
        } else if (arg == "--far") {
            options.farPlane = std::stof(value());
        } else if (arg == "--mode") {
            options.mode = value();
        } else if (arg == "--capture") {
            options.capture = value();
        } else if (arg == "--validation") {
            options.validation = true;
        } else if (arg == "--wait") {
            options.wait = true;
        } else {
            throw std::runtime_error("Unknown option: " + arg);
        }
    }

    if (options.flight < 1 || options.flight > 4 ||
        !std::isfinite(options.nearPlane) || !std::isfinite(options.farPlane) ||
        options.nearPlane <= 0 || options.farPlane <= options.nearPlane) {
        throw std::runtime_error("Invalid option range");
    }

    if (!options.capture.empty() && !options.frames) {
        options.frames = 1;
    }

    return options;
}

// 조건 검증 도우미 함수
void Check(bool ok, const char* operation) {
    if (!ok) {
        throw std::runtime_error(operation);
    }
}

// =============================================================================
// [화면 캡처 저장]
// 렌더 타깃 텍스처를 CPU로 읽어와 PPM 파일로 저장
// =============================================================================
void SaveCapture(IDevice& device, TextureHandle target, const std::string& path) {
    TextureReadback readback;
    Check(device.ReadTexture(target, readback), "ReadTexture failed");

    std::ofstream file(path, std::ios::binary);
    Check(bool(file), "Cannot open capture");

    file << "P6\n" << readback.width << " " << readback.height << "\n255\n";

    const bool bgra = (readback.format == Format::B8G8R8A8_UNORM ||
                       readback.format == Format::B8G8R8A8_UNORM_SRGB);

    for (uint32_t y = 0; y < readback.height; ++y) {
        for (uint32_t x = 0; x < readback.width; ++x) {
            const auto* p = readback.pixels.data() + size_t(y) * readback.rowPitch + x * 4;
            const char rgb[3] = {
                char(p[bgra ? 2 : 0]),
                char(p[1]),
                char(p[bgra ? 0 : 2])
            };
            file.write(rgb, 3);
        }
    }

    Check(bool(file), "Capture write failed");
}

// 통계 분석 및 출력 함수
void Statistics(const char* name, std::vector<double> samples) {
    if (samples.empty()) {
        return;
    }

    std::sort(samples.begin(), samples.end());
    double sum = 0;
    for (double sample : samples) {
        sum += sample;
    }

    std::cout << name << ": samples=" << samples.size()
              << " mean=" << sum / samples.size() << " ms"
              << " median=" << samples[samples.size() / 2] << " ms"
              << " p95=" << samples[(samples.size() - 1) * 95 / 100] << " ms\n";
}

} // namespace

// =============================================================================
// [메인 진입점]
// =============================================================================
int main(int argc, char** argv) try {
    // 도움말 출력
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--help") {
            std::cout << "DepthSampling: --mode split|raw|linear --near 0.1 --far 100; split shows raw depth left, linear depth right.\n"
                      << "Common: --frames N --capture path.ppm --validation\n";
            return 0;
        }
    }

    const auto options = Parse(argc, argv);

    // 1. 윈도우 생성 (해상도: 800x600)
    dyf::Platform::Window window(800, 600, "Advanced / DepthSampling");
    Check(window.GetHandle() != nullptr, "Window creation failed");

    // 2. RHI 렌더 디바이스 생성
    DeviceDesc deviceDesc;
    deviceDesc.enableValidation = options.validation;
    deviceDesc.maxFramesInFlight = options.flight;

    std::unique_ptr<IDevice> owner(IDevice::Create(deviceDesc));
    Check(owner != nullptr, "Device creation failed");
    auto& device = *owner;

    if (!device.Supports(Feature::Rasterization)) {
        std::cout << "UNSUPPORTED: this backend does not rasterize.\n";
        return 77;
    }

    ResourceScope resources(device);

    // 3. 스왑체인 생성
    SwapchainDesc swapchain;
    swapchain.window = window.GetHandle();
    swapchain.format = Format::B8G8R8A8_UNORM;
    swapchain.minimumImageCount = 2;
    swapchain.allowReadback = !options.capture.empty();

    Check(device.CreateSwapchain(swapchain), "Requested swapchain configuration unsupported");

    // 4. 셰이더 모듈 생성
    auto* vertex = resources.Keep(device.CreateShader({
        ShaderStage::Vertex,
        ShaderData::vertexEntryPoint,
        ShaderData::vertex,
        ShaderData::vertexSize
    }));
    auto* fragment = resources.Keep(device.CreateShader({
        ShaderStage::Fragment,
        ShaderData::fragmentEntryPoint,
        ShaderData::fragment,
        ShaderData::fragmentSize
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

    // 옵션 모드 유효성 검증
    Check(options.mode == "default" || options.mode == "split" ||
          options.mode == "raw" || options.mode == "linear", "Invalid mode");

    // 5. 깊이 생성(Producer) 파이프라인 생성 (컬러 타깃 없이 D32_FLOAT 깊이 버퍼에 기록)
    auto* producerShader = resources.Keep(device.CreateShader({
        ShaderStage::Fragment,
        ShaderData::producerEntryPoint,
        ShaderData::producer,
        ShaderData::producerSize
    }));

    GraphicsPipelineDesc producerDesc = desc;
    producerDesc.fragmentShader = producerShader;
    producerDesc.colorAttachments = nullptr;
    producerDesc.colorAttachmentCount = 0;
    producerDesc.depthStencil.format = Format::D32_FLOAT;
    producerDesc.depthStencil.depthWriteEnabled = true;

    auto* producer = resources.Keep(device.CreateGraphicsPipeline(producerDesc));

    // 6. 깊이 표시(Display) 파이프라인 생성 (깊이 텍스처를 셰이더 리소스로 샘플링하여 시각화)
    SamplerDesc sampler;
    sampler.minFilter = sampler.magFilter = sampler.mipFilter = SamplerFilter::Nearest;
    sampler.addressU = sampler.addressV = sampler.addressW = SamplerAddressMode::ClampToEdge;
    sampler.mipLodBias = sampler.minLod = sampler.maxLod = 0;

    const ResourceBindingLayout layout[] = {
        {0, ResourceBindingType::SampledTexture, 1, ShaderStageFlags::Fragment, {}},
        {1, ResourceBindingType::StaticSampler, 1, ShaderStageFlags::Fragment, sampler}
    };
    desc.layout.bindings = layout;
    desc.layout.bindingCount = 2;

    auto* display = resources.Keep(device.CreateGraphicsPipeline(desc));

    // 깊이 버퍼 텍스처 및 리소스 바인딩 세트 핸들
    TextureHandle intermediate = nullptr;
    ResourceSetHandle bindings = nullptr;
    ResourceState intermediateState = ResourceState::Undefined;

    uint32_t frame = 0;
    std::vector<double> cpuMs;

    // =========================================================================
    // [메인 렌더 루프]
    // =========================================================================
    while (!options.frames || frame < options.frames) {
        window.PollEvents();
        if (!window.IsRunning()) {
            break;
        }

        if (!device.BeginFrame()) {
            Check(!device.IsLost(), "Device lost");
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }

        auto* target = device.GetBackBuffer();
        const auto& extent = target->GetDesc();

        // 윈도우 크기 변경 시 중간 깊이 텍스처(intermediate) 및 리소스 세트 재할당
        if (!intermediate || intermediate->GetDesc().width != extent.width || intermediate->GetDesc().height != extent.height) {
            if (bindings) {
                device.DestroyResourceSet(bindings);
            }
            if (intermediate) {
                device.DestroyTexture(intermediate);
            }

            TextureDesc texture;
            texture.width = extent.width;
            texture.height = extent.height;
            texture.format = Format::D32_FLOAT;
            texture.usage = TextureUsage::DepthStencil | TextureUsage::ShaderResource;

            intermediate = device.CreateTexture(texture);
            Check(intermediate != nullptr, "Intermediate texture creation failed");

            ResourceBinding input;
            input.binding = 0;
            input.texture = intermediate;

            bindings = device.CreateResourceSet({display, &input, 1});
            Check(bindings != nullptr, "ResourceSet creation failed");
            intermediateState = ResourceState::Undefined;
        }

        ResourceScope frameResources(device);
        auto* commands = frameResources.Keep(device.AcquireCommandList());
        const auto start = std::chrono::steady_clock::now();

        // --- 1단계: 깊이 텍스처를 쓰기 상태(DepthWrite)로 전이 ---
        const ResourceBarrierDesc before{nullptr, intermediate, intermediateState, ResourceState::DepthWrite, {}};
        commands->ResourceBarrier(&before, 1);

        const float nearPlane = options.nearPlane;
        const float farPlane = options.farPlane;
        float parameters[4] = {0, 0, nearPlane, farPlane};

        // --- 2단계: 깊이 생성 패스 (Producer Pass) ---
        // 각 패스는 명시적으로 Begin/EndRendering을 호출하여 기록함
        for (uint32_t pass = 0; pass < 1; ++pass) {
            DepthStencilAttachment output;
            output.texture = intermediate;
            output.state = ResourceState::DepthWrite;
            output.depthLoadOp = LoadOp::Clear;
            output.depthStoreOp = StoreOp::Store;
            output.clearDepth = 1;

            commands->BeginRendering({nullptr, 0, &output});
            commands->BindGraphicsPipeline(producer);
            commands->SetViewport({0, 0, float(extent.width), float(extent.height), 0, 1});
            commands->SetScissor({0, 0, extent.width, extent.height});

            parameters[0] = float(pass);
            commands->SetInlineConstants(0, sizeof(parameters), parameters);
            commands->DrawInstanced(3, 1, 0, 0);
            commands->EndRendering();

            // --wait 옵션 활성화 시 패스별 동기화 제출 및 대기
            if (options.wait) {
                Check(commands->Close(), "Producer close failed");
                FenceHandle completion;
                Check(device.Submit({&commands, 1, nullptr, 0}, completion), "Producer submit failed");
                Check(device.Wait(completion, UINT64_MAX), "Producer wait failed");
                commands = frameResources.Keep(device.AcquireCommandList());
            }
        }

        // --- 3단계: 리소스 배리어 전이 (깊이 버퍼 -> 셰이더 리소스, 백버퍼 -> 렌더 타깃) ---
        const ResourceBarrierDesc ready[] = {
            {nullptr, intermediate, ResourceState::DepthWrite, ResourceState::ShaderResource, {}},
            {nullptr, target, ResourceState::Present, ResourceState::RenderTarget, {}}
        };
        commands->ResourceBarrier(ready, 2);

        // --- 4단계: 깊이 표시 패스 (Display Pass) ---
        ColorAttachment color;
        color.texture = target;
        color.loadOp = LoadOp::Discard;
        color.storeOp = StoreOp::Store;

        commands->BeginRendering({&color, 1, nullptr});
        commands->BindGraphicsPipeline(display);
        commands->BindResourceSet(bindings);
        commands->SetViewport({0, 0, float(extent.width), float(extent.height), 0, 1});
        commands->SetScissor({0, 0, extent.width, extent.height});

        // 모드 상수 설정 (0: default/split, 1: raw, 2: linear)
        parameters[0] = options.mode == "raw" ? 1.f : options.mode == "linear" ? 2.f : 0.f;
        commands->SetInlineConstants(0, sizeof(parameters), parameters);
        commands->DrawInstanced(3, 1, 0, 0);
        commands->EndRendering();

        // 백버퍼 프레젠트 상태 전이
        ResourceBarrierDesc after{nullptr, target, ResourceState::RenderTarget, ResourceState::Present, {}};
        commands->ResourceBarrier(&after, 1);

        Check(commands->Close(), "Display close failed");
        Check(device.Submit(&commands, 1), "Display submit failed");
        intermediateState = ResourceState::ShaderResource;

        // CPU 타이밍 기록
        cpuMs.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
        if (cpuMs.size() > 4096) {
            cpuMs.erase(cpuMs.begin(), cpuMs.begin() + 2048);
        }

        if (!options.capture.empty() && frame + 1 == options.frames) {
            SaveCapture(device, target, options.capture);
        }

        Check(device.Present(), "Present failed");
        ++frame;
    }

    // =========================================================================
    // [종료 및 자원 해제]
    // =========================================================================
    Statistics("CPU record/submit incl optional waits", cpuMs);

    if (bindings) {
        device.DestroyResourceSet(bindings);
    }
    if (intermediate) {
        device.DestroyTexture(intermediate);
    }

    Check(device.WaitIdle(), "WaitIdle failed");
    return 0;

} catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
}
