#include "dyf/RHI.h"
#include "dyf/Platform/Window.h"
#include "dyf/Platform/Profiler.h"
#include "dyf/Platform/RenderDocCapture.h"
#include "vertex.h"
#include "fragment.h"

#include <algorithm>
#include <chrono>
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
// 명령줄 인수(CLI)로 전달받는 실행 옵션 구조체
// =============================================================================
struct Options {
    uint32_t frames = 0;       // 실행할 총 프레임 수 (0이면 창을 닫을 때까지 무한 반복)
    uint32_t flight = 2;       // 동시 처리 가능한 In-flight 프레임 수 (1 ~ 4)
    uint32_t work = 96;        // 픽셀 셰이더 내 부하 연산 강도 (0 ~ 2048)
    bool validation = false;   // RHI 유효성 검사 계층(Validation Layer) 활성화 여부
    std::string capture;       // 캡처 이미지 저장 경로 (.ppm 포맷)
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
        } else if (arg == "--work") {
            options.work = uint32_t(std::stoul(value()));
        } else if (arg == "--capture") {
            options.capture = value();
        } else if (arg == "--validation") {
            options.validation = true;
        } else {
            throw std::runtime_error("Unknown option: " + arg);
        }
    }

    if (options.flight < 1 || options.flight > 4 || options.work > 2048) {
        throw std::runtime_error("Invalid option range");
    }

    if (!options.capture.empty() && !options.frames) {
        options.frames = 1;
    }

    return options;
}

// 조건 검사 헬퍼 함수
void Check(bool ok, const char* operation) {
    if (!ok) {
        throw std::runtime_error(operation);
    }
}

// =============================================================================
// [화면 캡처 저장]
// 렌더링된 텍스처를 CPU로 읽어와 PPM 이미지 파일로 저장
// =============================================================================
void SaveCapture(IDevice& device, TextureHandle target, const std::string& path) {
    TextureReadback readback;
    Check(device.ReadTexture(target, readback), "ReadTexture failed");

    std::ofstream file(path, std::ios::binary);
    Check(bool(file), "Cannot open capture");

    // PPM (P6 바이너리) 헤더 기록
    file << "P6\n" << readback.width << " " << readback.height << "\n255\n";

    const bool bgra = (readback.format == Format::B8G8R8A8_UNORM ||
                       readback.format == Format::B8G8R8A8_UNORM_SRGB);

    // 픽셀 데이터 변환 및 기록 (BGRA -> RGB 또는 RGBA -> RGB)
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

// =============================================================================
// [통계 분석 및 출력]
// 수집된 시간 샘플(ms)에 대한 표본 수, 평균, 중앙값, 95백분위수(p95) 계산 및 출력
// =============================================================================
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
    // 도움말(--help) 출력
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--help") {
            std::cout << "Profiling: --work 0..2048 --flight 1..4; GPU timestamp is optional, CPU timings always measured.\n"
                      << "Common: --frames N --capture path.ppm --validation\n"
                      << "F12 requests RenderDoc capture when the API is injected; PIX/RenderDoc show the Profiling pass event.\n";
            return 0;
        }
    }

    // 명령행 옵션 파싱
    const auto options = Parse(argc, argv);

    // 1. 윈도우 생성 (해상도: 800x600)
    dyf::Platform::Window window(800, 600, "Advanced / Profiling");
    Check(window.GetHandle() != nullptr, "Window creation failed");

    // 2. RHI 렌더 디바이스 생성
    DeviceDesc deviceDesc;
    deviceDesc.enableValidation = options.validation;
    deviceDesc.maxFramesInFlight = options.flight;

    std::unique_ptr<IDevice> owner(IDevice::Create(deviceDesc));
    Check(owner != nullptr, "Device creation failed");
    auto& device = *owner;

    // 래스터라이제이션 지원 여부 확인
    if (!device.Supports(Feature::Rasterization)) {
        std::cout << "UNSUPPORTED: this backend does not rasterize.\n";
        return 77;
    }

    // 디바이스 리소스 자동 관리를 위한 Scope
    ResourceScope resources(device);

    // 3. 스왑체인(Swapchain) 생성
    SwapchainDesc swapchain;
    swapchain.window = window.GetHandle();
    swapchain.format = Format::B8G8R8A8_UNORM;
    swapchain.minimumImageCount = 2;
    swapchain.allowReadback = !options.capture.empty();

    Check(device.CreateSwapchain(swapchain), "Requested swapchain configuration unsupported");

    // 4. 셰이더 및 그래픽스 파이프라인 생성
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

    auto* pipeline = resources.Keep(device.CreateGraphicsPipeline(desc));

    // 5. GPU 타임스탬프 쿼리 풀 생성 (하드웨어 지원 확인)
    const bool gpuTiming = device.Supports(Feature::TimestampQuery);
    std::cout << "GPU timestamp: " << (gpuTiming ? "available" : "UNSUPPORTED; CPU timing only") << '\n';

    auto* query = gpuTiming ? resources.Keep(device.CreateTimestampQuery({options.flight * 2})) : nullptr;

    // 6. 프레임 동시 실행(In-Flight)을 위한 커맨드 리스트 및 펜스 준비
    std::vector<ICommandList*> lists(options.flight);
    std::vector<FenceHandle> fences(options.flight);
    for (auto& list : lists) {
        list = resources.Keep(device.AcquireCommandList());
    }

    // 성능 측정을 위한 타이밍 데이터 저장 벡터 (CPU 기록, 커맨드 제출, 프레임 전체, GPU 실행)
    std::vector<double> recordMs, submitMs, frameMs, gpuMs;
    const auto milliseconds = [](auto begin, auto end) {
        return std::chrono::duration<double, std::milli>(end - begin).count();
    };

    // 완료된 GPU 타임스탬프 쿼리 결과 읽기 람다
    auto readQuery = [&](uint32_t slot) {
        if (!query || !fences[slot]) {
            return;
        }
        uint64_t ticks[2];
        Check(device.ReadTimestamps(query, slot * 2, 2, ticks), "Completed timestamp query unavailable");

        const uint32_t bits = device.GetTimestampValidBits();
        const uint64_t mask = (bits >= 64) ? UINT64_MAX : ((uint64_t{1} << bits) - 1);
        gpuMs.push_back(double((ticks[1] - ticks[0]) & mask) * device.GetTimestampPeriodNanoseconds() / 1e6);
    };

    uint32_t frame = 0;
    auto frameStart = std::chrono::steady_clock::now();

    // =========================================================================
    // [메인 렌더 루프]
    // =========================================================================
    while (!options.frames || frame < options.frames) {
        // 윈도우 OS 이벤트 처리
        window.PollEvents();
        if (!window.IsRunning()) {
            break;
        }

        const uint32_t slot = frame % options.flight;

        // 이전 프레임의 GPU 작업 완료 대기 및 타임스탬프 결과 회수
        if (fences[slot]) {
            Check(device.Wait(fences[slot], UINT64_MAX), "Fence wait failed");
            readQuery(slot);
            fences[slot] = {};
        }

        // 커맨드 리스트 재설정 및 프레임 시작
        Check(device.ResetCommandList(lists[slot]), "ResetCommandList failed");
        if (!device.BeginFrame()) {
            Check(!device.IsLost(), "Device lost");
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }

        auto* target = device.GetBackBuffer();
        auto* commands = lists[slot];

        // --- 커맨드 기록 구간 (CPU Record 측정) ---
        const auto recordStart = std::chrono::steady_clock::now();
        DY_PROFILE_CPU_ZONE_NAMED("AdvancedProfiling::Record");

        commands->BeginDebugEvent("Profiling pass");
        commands->InsertDebugMarker("Timestamp begin");

        // 시작 GPU 타임스탬프 기록
        if (query) {
            commands->ResetTimestamps(query, slot * 2, 2);
            commands->WriteTimestamp(query, slot * 2);
        }

        // 백버퍼 렌더 타깃 전환 배리어
        ResourceBarrierDesc begin{nullptr, target, ResourceState::Present, ResourceState::RenderTarget, {}};
        commands->ResourceBarrier(&begin, 1);

        // 렌더 패스 시작 및 파이프라인 바인딩
        ColorAttachment color;
        color.texture = target;
        color.loadOp = LoadOp::Clear;
        color.storeOp = StoreOp::Store;
        color.clearColor[3] = 1.0f;

        commands->BeginRendering({&color, 1, nullptr});
        commands->BindGraphicsPipeline(pipeline);

        // 뷰포트 및 시저 사각형 설정
        commands->SetViewport({0, 0, float(target->GetDesc().width), float(target->GetDesc().height), 0, 1});
        commands->SetScissor({0, 0, target->GetDesc().width, target->GetDesc().height});

        // 인라인 상수(워크로드 파라미터) 전달 및 드로우 콜 호출
        const float parameters[4] = {float(options.work), 0, 0, 0};
        commands->SetInlineConstants(0, sizeof(parameters), parameters);
        commands->DrawInstanced(3, 1, 0, 0);

        commands->EndRendering();

        // 프레젠트 상태로 전환 배리어
        ResourceBarrierDesc end{nullptr, target, ResourceState::RenderTarget, ResourceState::Present, {}};
        commands->ResourceBarrier(&end, 1);

        // 종료 GPU 타임스탬프 기록
        if (query) {
            commands->WriteTimestamp(query, slot * 2 + 1);
        }

        commands->EndDebugEvent();
        Check(commands->Close(), "Close failed");

        // --- 커맨드 제출 구간 (CPU Submit 측정) ---
        const auto submitStart = std::chrono::steady_clock::now();
        Check(device.Submit({&commands, 1, nullptr, 0}, fences[slot]), "Submit failed");
        const auto submitEnd = std::chrono::steady_clock::now();

        // 캡처 요청 시 이미지 파일 저장
        if (!options.capture.empty() && frame + 1 == options.frames) {
            SaveCapture(device, target, options.capture);
        }

        // 화면 출력(Present)
        Check(device.Present(), "Present failed");
        DY_PROFILE_FRAME_MARK();

        // 프레임 타이밍 표본 저장
        recordMs.push_back(milliseconds(recordStart, submitStart));
        submitMs.push_back(milliseconds(submitStart, submitEnd));

        const auto frameEnd = std::chrono::steady_clock::now();
        frameMs.push_back(milliseconds(frameStart, frameEnd));
        frameStart = frameEnd;
        ++frame;

        // 120 프레임마다 평균/중앙값 통계 출력
        if (frame % 120 == 0) {
            Statistics("CPU record", recordMs);
            Statistics("CPU Submit (native replay included)", submitMs);
            Statistics("GPU pass", gpuMs);
        }

        // 장시간 실행 시 메모리 무한 증가 방지 (최근 4096개 유지)
        for (auto* samples : {&recordMs, &submitMs, &frameMs, &gpuMs}) {
            if (samples->size() > 4096) {
                samples->erase(samples->begin(), samples->begin() + 2048);
            }
        }
    }

    // =========================================================================
    // [종료 처리 및 최종 통계]
    // =========================================================================
    for (uint32_t slot = 0; slot < options.flight; ++slot) {
        if (fences[slot]) {
            Check(device.Wait(fences[slot], UINT64_MAX), "Final wait failed");
            readQuery(slot);
        }
    }

    std::cout << "Cold frames included; capture/validation can affect measurement. GPU pass excludes Present.\n";
    Statistics("CPU record", recordMs);
    Statistics("CPU Submit", submitMs);
    Statistics("CPU frame incl waits/present", frameMs);
    Statistics("GPU pass", gpuMs);

    Check(device.WaitIdle(), "WaitIdle failed");
    return 0;

} catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
}
