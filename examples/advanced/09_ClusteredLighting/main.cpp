#include "dyf.h"
#include "dyf/RHI.h"
#include "vertex.h"
#include "cluster.h"
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
// GLSL Push Constants, HLSL b15, Metal buffer(15) 대응 (64바이트)
// =============================================================================
struct alignas(16) Params {
    float sizeTime[4] = {960, 640, 0, 0}; // width, height, time, mode(0:clustered, 1:all, 2:count)
    float settings[4] = {1, 0.45f, 0.8f, 0.9f}; // exposure, roughness, metallic, unused
    float jitter[4] = {};
    float extra[4] = {}; // extra[2]: active light count (default 64)
};

static_assert(sizeof(Params) == 64 && alignof(Params) == 16);
static_assert(offsetof(Params, settings) == 16 &&
              offsetof(Params, jitter) == 32 &&
              offsetof(Params, extra) == 48);

// 수치 인자 검증 및 파싱 함수
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
    params.extra[2] = 64; // 기본 활성 조명 개수: 64개

    uint64_t frameLimit = 0;
    bool framesSpecified = false;
    bool validation = false;
    bool fixedTime = false;
    float timeValue = 0;

    // 명령행 옵션 파싱
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];

        if (arg == "--help") {
            std::puts("ClusteredLighting\n"
                      "World-space clusters: 16x16x8; 64-light capacity; outside volume uses all lights.\n"
                      "--mode clustered|all|count, --lights [1,64], --exposure [0.01,20]\n"
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
            if (value == "clustered") {
                params.sizeTime[3] = 0;
            } else if (value == "all") {
                params.sizeTime[3] = 1;
            } else if (value == "count") {
                params.sizeTime[3] = 2;
            } else {
                throw std::runtime_error("Unknown mode: " + value);
            }
        } else if (arg == "--lights") {
            double n = Number(value, 1, 64);
            if (std::floor(n) != n) {
                throw std::runtime_error("This option requires an integer.");
            }
            params.extra[2] = float(n);
        } else if (arg == "--exposure") {
            params.settings[0] = float(Number(value, 0.01, 20));
        } else {
            throw std::runtime_error("Unknown option: " + arg);
        }
    }


    // 1. 윈도우 생성 (해상도: 960x640)
    dyf::Platform::Window window(960, 640, "Advanced / 09_ClusteredLighting");
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

    // 컴퓨트 지원 여부 확인
    if (!device.Supports(Feature::Compute)) {
        std::puts("Unsupported: this RHI backend has no Compute dispatch/storage-buffer implementation. No CPU fallback is substituted.");
        return 77;
    }

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

    // 4. 셰이더 모듈 로드
    auto vertexShader = resources.Keep(device.CreateShader({
        ShaderStage::Vertex,
        ShaderData::vertexEntryPoint,
        ShaderData::vertex,
        ShaderData::vertexSize
    }));
    auto clusterShader = resources.Keep(device.CreateShader({
        ShaderStage::Compute,
        ShaderData::clusterEntryPoint,
        ShaderData::cluster,
        ShaderData::clusterSize
    }));
    auto lightingShader = resources.Keep(device.CreateShader({
        ShaderStage::Fragment,
        ShaderData::lightingEntryPoint,
        ShaderData::lighting,
        ShaderData::lightingSize
    }));

    // 5. 조명 계산 그래픽스 파이프라인 생성
    std::vector<ResourceBindingLayout> lightingBindings;
    lightingBindings.push_back({0, ResourceBindingType::ReadOnlyStorageBuffer, 1, ShaderStageFlags::Fragment, {}});

    ColorAttachmentDesc colorDesc{Format::B8G8R8A8_UNORM, {}, ColorWriteMask::All};

    GraphicsPipelineDesc lightingDesc;
    lightingDesc.vertexShader = vertexShader;
    lightingDesc.fragmentShader = lightingShader;
    lightingDesc.topology = PrimitiveTopology::TriangleList;
    lightingDesc.raster = {FillMode::Solid, CullMode::None, FrontFace::CounterClockwise, 0, 0, 0};
    lightingDesc.colorAttachments = &colorDesc;
    lightingDesc.colorAttachmentCount = 1;
    lightingDesc.layout = {
        lightingBindings.data(),
        uint32_t(lightingBindings.size()),
        sizeof(Params),
        ShaderStageFlags::Fragment,
        15
    };

    auto lightingPipeline = resources.Keep(device.CreateGraphicsPipeline(lightingDesc));

    // 6. 클러스터 그리드 스토리지 버퍼 할당
    constexpr uint32_t clusterCount = 16 * 16 * 8;
    constexpr uint32_t wordsPerCluster = 65;

    BufferDesc listDesc;
    listDesc.size = clusterCount * wordsPerCluster * sizeof(uint32_t);
    listDesc.stride = sizeof(uint32_t);
    listDesc.usage = BufferUsage::Storage;
    listDesc.initialState = ResourceState::Undefined;

    auto lightLists = resources.Keep(device.CreateBuffer(listDesc));

    // 7. 클러스터 빌드 컴퓨트 파이프라인 생성
    ResourceBindingLayout computeBinding{0, ResourceBindingType::ReadWriteStorageBuffer, 1, ShaderStageFlags::Compute, {}};
    ComputePipelineDesc computeDesc;
    computeDesc.computeShader = clusterShader;
    computeDesc.layout = {&computeBinding, 1, sizeof(Params), ShaderStageFlags::Compute, 15};

    computeDesc.threadGroupSize[0] = 64;
    auto clusterPipeline = resources.Keep(device.CreateComputePipeline(computeDesc));

    ResourceBinding listBinding{0, 0, lightLists, nullptr, 0, listDesc.size, {}};
    auto clusterSet = resources.Keep(device.CreateResourceSet({clusterPipeline, &listBinding, 1}));
    auto lightingSet = resources.Keep(device.CreateResourceSet({lightingPipeline, &listBinding, 1}));

    bool listsInitialized = false;
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

        params.sizeTime[0] = float(dimensions.width);
        params.sizeTime[1] = float(dimensions.height);
        params.sizeTime[2] = fixedTime ? timeValue : float(frame) / 60.0f;

        ResourceScope frameResources(device);
        auto* commands = frameResources.Keep(device.AcquireCommandList());

        // --- 1단계: GPU 컴퓨트 클러스터 빌드 ---
        const ResourceBarrierDesc writable{
            lightLists,
            nullptr,
            listsInitialized ? ResourceState::ShaderResource : ResourceState::Undefined,
            ResourceState::UnorderedAccess,
            {}
        };
        commands->ResourceBarrier(&writable, 1);

        commands->BindComputePipeline(clusterPipeline);
        commands->BindResourceSet(clusterSet);
        commands->SetInlineConstants(0, sizeof(params), &params);
        commands->Dispatch(clusterCount / 64, 1, 1);

        const ResourceBarrierDesc readable{
            lightLists,
            nullptr,
            ResourceState::UnorderedAccess,
            ResourceState::ShaderResource,
            {}
        };
        commands->ResourceBarrier(&readable, 1);

        // --- 2단계: 프래그먼트 셰이더 조명 렌더 패스 ---
        {
            const ResourceBarrierDesc backbufferBarrier{nullptr, backbuffer, ResourceState::Present, ResourceState::RenderTarget, {}};
            commands->ResourceBarrier(&backbufferBarrier, 1);

            ColorAttachment colorAttachment{};
            colorAttachment.texture = backbuffer;
            colorAttachment.loadOp = LoadOp::Clear;
            colorAttachment.storeOp = StoreOp::Store;

            commands->BeginRendering({&colorAttachment, 1, nullptr});
            commands->BindGraphicsPipeline(lightingPipeline);
            commands->BindResourceSet(lightingSet);
            commands->SetViewport({0, 0, float(dimensions.width), float(dimensions.height), 0, 1});
            commands->SetScissor({0, 0, dimensions.width, dimensions.height});

            commands->SetInlineConstants(0, sizeof(params), &params);
            commands->DrawInstanced(3, 1, 0, 0);
            commands->EndRendering();
        }

        // 백버퍼 프레젠트 상태 전이
        const ResourceBarrierDesc presentBarrier{nullptr, backbuffer, ResourceState::RenderTarget, ResourceState::Present, {}};
        commands->ResourceBarrier(&presentBarrier, 1);

        if (!commands->Close() || !device.Submit(&commands, 1)) {
            throw std::runtime_error("Frame recording/submission failed.");
        }

        listsInitialized = true;
        ++frame;


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
    std::fprintf(stderr, "ClusteredLighting: %s\n", error.what());
    return 1;
}
