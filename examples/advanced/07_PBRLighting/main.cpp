#include "dyf.h"
#include "dyf/RHI.h"
#include "vertex.h"
#include "lighting.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using namespace dyf::RHI;

namespace {

// =============================================================================
// [셰이더 상수 버퍼 구조체]
// GLSL Push Constants, HLSL b15, Metal buffer(15) 대응 (총 64바이트)
// =============================================================================
struct alignas(16) Params {
    float sizeTime[4] = {960, 640, 0, 0}; // width, height, time, mode(0:pbr, 1:lambert, 2:normal)
    float settings[4] = {1, 0.45f, 0.8f, 0.9f}; // exposure, roughness, metallic, unused
    float jitter[4] = {};
    float extra[4] = {};
};

static_assert(sizeof(Params) == 64 && alignof(Params) == 16);
static_assert(offsetof(Params, settings) == 16 &&
              offsetof(Params, jitter) == 32 &&
              offsetof(Params, extra) == 48);

// 수치 인자 검증 및 파싱 도우미 함수
double Number(const std::string& value, double minimum, double maximum) {
    size_t consumed = 0;
    double result = std::stod(value, &consumed);
    if (consumed != value.size() || !std::isfinite(result) || result < minimum || result > maximum) {
        throw std::runtime_error("Numeric option out of range: " + value);
    }
    return result;
}


} // namespace

// =============================================================================
// [메인 진입점]
// =============================================================================
int main(int argc, char** argv) try {
    Params params;

    uint64_t frameLimit = 0;
    bool framesSpecified = false;
    bool validation = false;
    bool fixedTime = false;
    float timeValue = 0;

    // 명령행 인자 파싱
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];

        if (arg == "--help") {
            std::puts("PBRLighting\n"
                      "--mode pbr|lambert|normal, --roughness [0.045,1], --metallic [0,1], --exposure [0.01,20]\n"
                      "--frames N (0=unlimited), --time SECONDS, --validation");
            return 0;
        }

        if (arg == "--validation") {
            validation = true;
            continue;
        }

        if (i + 1 >= argc) {
            throw std::runtime_error("Missing value for " + arg);
        }

        std::string value = argv[++i];

        if (arg == "--frames") {
            double n = Number(value, 0, 10000000);
            if (std::floor(n) != n) {
                throw std::runtime_error("Frame count must be an integer.");
            }
            frameLimit = uint64_t(n);
            framesSpecified = true;
        } else if (arg == "--time") {
            timeValue = float(Number(value, 0, 1000000));
            fixedTime = true;
        } else if (arg == "--mode") {
            if (value == "pbr") {
                params.sizeTime[3] = 0;
            } else if (value == "lambert") {
                params.sizeTime[3] = 1;
            } else if (value == "normal") {
                params.sizeTime[3] = 2;
            } else {
                throw std::runtime_error("Unknown mode: " + value);
            }
        } else if (arg == "--roughness") {
            double n = Number(value, 0.045, 1);
            params.settings[1] = float(n);
        } else if (arg == "--metallic") {
            double n = Number(value, 0, 1);
            params.settings[2] = float(n);
        } else if (arg == "--exposure") {
            double n = Number(value, 0.01, 20);
            params.settings[0] = float(n);
        } else {
            throw std::runtime_error("Unknown option: " + arg);
        }
    }


    // 1. 윈도우 생성 (해상도: 960x640)
    dyf::Platform::Window window(960, 640, "Advanced / 07_PBRLighting");
    if (!window.GetHandle()) {
        throw std::runtime_error("Window creation failed.");
    }

    // 2. RHI 렌더 디바이스 생성
    DeviceDesc deviceDesc;
    deviceDesc.enableValidation = validation;

    std::unique_ptr<IDevice> owner(IDevice::Create(deviceDesc));
    if (!owner) {
        throw std::runtime_error("Device creation failed.");
    }
    auto& device = *owner;

    ResourceScope resources(device);

    // 3. 스왑체인 생성
    SwapchainDesc swap;
    swap.window = window.GetHandle();
    swap.format = Format::B8G8R8A8_UNORM;
    swap.minimumImageCount = 2;
    swap.presentMode = PresentMode::Fifo;

    if (!device.CreateSwapchain(swap)) {
        throw std::runtime_error("Swapchain creation failed.");
    }

    // 4. PBR 라이팅 셰이더 및 파이프라인 생성
    auto vertexShader = resources.Keep(device.CreateShader({
        ShaderStage::Vertex,
        ShaderData::vertexEntryPoint,
        ShaderData::vertex,
        ShaderData::vertexSize
    }));
    auto lightingShader = resources.Keep(device.CreateShader({
        ShaderStage::Fragment,
        ShaderData::lightingEntryPoint,
        ShaderData::lighting,
        ShaderData::lightingSize
    }));

    ColorAttachmentDesc colorDesc{Format::B8G8R8A8_UNORM, {}, ColorWriteMask::All};

    GraphicsPipelineDesc lightingDesc;
    lightingDesc.vertexShader = vertexShader;
    lightingDesc.fragmentShader = lightingShader;
    lightingDesc.topology = PrimitiveTopology::TriangleList;
    lightingDesc.raster = {FillMode::Solid, CullMode::None, FrontFace::CounterClockwise, 0, 0, 0};
    lightingDesc.colorAttachments = &colorDesc;
    lightingDesc.colorAttachmentCount = 1;
    lightingDesc.layout = {nullptr, 0, sizeof(Params), ShaderStageFlags::Fragment, 15};

    auto lightingPipeline = resources.Keep(device.CreateGraphicsPipeline(lightingDesc));

    uint64_t frame = 0;

    // =========================================================================
    // [메인 렌더 루프]
    // =========================================================================
    while (!frameLimit || frame < frameLimit) {
        window.PollEvents();
        if (!window.IsRunning()) {
            break;
        }

        if (!device.BeginFrame()) {
            if (device.IsLost()) {
                throw std::runtime_error("Device lost.");
            }
            continue;
        }

        auto backbuffer = device.GetBackBuffer();
        if (!backbuffer) {
            throw std::runtime_error("Backbuffer unavailable.");
        }

        const auto dimensions = backbuffer->GetDesc();

        // 프레임별 크기 및 시간 파라미터 업데이트
        params.sizeTime[0] = float(dimensions.width);
        params.sizeTime[1] = float(dimensions.height);
        params.sizeTime[2] = fixedTime ? timeValue : float(frame) / 60.0f;

        ResourceScope frameResources(device);
        auto* commands = frameResources.Keep(device.AcquireCommandList());

        // --- PBR 라이팅 렌더 패스 ---
        {
            // 백버퍼 렌더 타깃 상태 전이
            const ResourceBarrierDesc backbufferBarrier{nullptr, backbuffer, ResourceState::Present, ResourceState::RenderTarget, {}};
            commands->ResourceBarrier(&backbufferBarrier, 1);

            ColorAttachment colorAttachment{};
            colorAttachment.texture = backbuffer;
            colorAttachment.loadOp = LoadOp::Clear;
            colorAttachment.storeOp = StoreOp::Store;

            commands->BeginRendering({&colorAttachment, 1, nullptr});
            commands->BindGraphicsPipeline(lightingPipeline);
            commands->SetViewport({0, 0, float(dimensions.width), float(dimensions.height), 0, 1});
            commands->SetScissor({0, 0, dimensions.width, dimensions.height});

            // 상수 파라미터 전달 및 풀스크린 렌더링
            commands->SetInlineConstants(0, sizeof(params), &params);
            commands->DrawInstanced(3, 1, 0, 0);
            commands->EndRendering();
        }

        // 백버퍼 프레젠트 상태 전이 배리어
        const ResourceBarrierDesc presentBarrier{nullptr, backbuffer, ResourceState::RenderTarget, ResourceState::Present, {}};
        commands->ResourceBarrier(&presentBarrier, 1);

        if (!commands->Close() || !device.Submit(&commands, 1)) {
            throw std::runtime_error("Frame recording/submission failed.");
        }

        ++frame;


        // 화면 출력(Present)
        if (!device.Present()) {
            throw std::runtime_error("Present failed.");
        }
    }

    // =========================================================================
    // [종료 및 GPU 대기]
    // =========================================================================
    if (!device.WaitIdle()) {
        throw std::runtime_error("Final GPU wait failed.");
    }
    return 0;

} catch (const std::exception& error) {
    std::fprintf(stderr, "PBRLighting: %s\n", error.what());
    return 1;
}
