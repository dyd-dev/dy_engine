// 데스크톱은 main의 루프, iOS는 CreatePBRLighting으로 만든 객체의 Tick을 사용한다.
// Renderer 생성 시 파이프라인을 준비하고, Draw에서 전체 화면 PBR 패스를 제출한다.
#include "../common/Example.h"
#ifndef DY_ADVANCED_IOS
#include "dyf/Platform/Window.h"
#endif

using namespace dyf::RHI;
using namespace advanced;
namespace {
// 같은 앱에 두 예제를 연결해도 셰이더 데이터의 이름이 충돌하지 않는다.
#include "vertex.h"
#include "lighting.h"

Options ParseOptions(int argc, char **argv) {
    Options options;

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--help") {
            std::puts("PBRLighting\n--mode pbr|lambert|normal, --roughness [0.045,1], --metallic [0,1], "
                      "--exposure [0.01,20]\n--frames N (0=unlimited), --capture path.ppm, --time SECONDS, "
                      "--validation\n--capture defaults to 1 frames unless --frames is supplied.");
            options.help = true;
            return options;
        }
        if (arg == "--validation") {
            options.validation = true;
            continue;
        }
        if (i + 1 >= argc)
            throw std::runtime_error("Missing value for " + arg);
        std::string value = argv[++i];
        if (arg == "--frames") {
            double n = Number(value, 0, 10000000);
            if (std::floor(n) != n)
                throw std::runtime_error("Frame count must be an integer.");
            options.frameLimit = uint64_t(n);
            options.framesSpecified = true;
        } else if (arg == "--capture")
            options.capture = value;
        else if (arg == "--time") {
            options.timeValue = float(Number(value, 0, 1000000));
            options.fixedTime = true;
        } else if (arg == "--mode") {
            if (value == "pbr")
                options.params.sizeTime[3] = 0;
            else if (value == "lambert")
                options.params.sizeTime[3] = 1;
            else if (value == "normal")
                options.params.sizeTime[3] = 2;
            else
                throw std::runtime_error("Unknown mode: " + value);
        } else if (arg == "--roughness") {
            double n = Number(value, 0.045, 1);
            options.params.settings[1] = float(n);
        } else if (arg == "--metallic") {
            double n = Number(value, 0, 1);
            options.params.settings[2] = float(n);
        } else if (arg == "--exposure") {
            double n = Number(value, 0.01, 20);
            options.params.settings[0] = float(n);
        } else
            throw std::runtime_error("Unknown option: " + arg);
    }
    if (!options.capture.empty() && !options.framesSpecified)
        options.frameLimit = 1;
    if (!options.capture.empty() && options.frameLimit == 0)
        throw std::runtime_error("Capture requires a finite frame count.");
    return options;
}

class Renderer final : public Example {
  public:
    Renderer(const void *window, const Options &options) : Example(window, options) {
        auto vertexShader = resources.Keep(device.CreateShader(
            {ShaderStage::Vertex, ShaderData::vertexEntryPoint, ShaderData::vertex, ShaderData::vertexSize}));
        auto lightingShader =
            resources.Keep(device.CreateShader({ShaderStage::Fragment, ShaderData::lightingEntryPoint,
                                                ShaderData::lighting, ShaderData::lightingSize}));
        // PBR 조명을 그릴 파이프라인을 준비한다.
        ColorAttachmentDesc colorDesc{ExampleColorFormat, {}, ColorWriteMask::All};
        GraphicsPipelineDesc lightingDesc;
        lightingDesc.vertexShader = vertexShader;
        lightingDesc.fragmentShader = lightingShader;
        lightingDesc.topology = PrimitiveTopology::TriangleList;
        lightingDesc.raster = {FillMode::Solid, CullMode::None, FrontFace::CounterClockwise, 0, 0, 0};
        lightingDesc.colorAttachments = &colorDesc;
        lightingDesc.colorAttachmentCount = 1;
        lightingDesc.layout = {nullptr, 0, sizeof(Params),
                               ShaderStageFlags::Fragment, 15};
        lightingPipeline = resources.Keep(device.CreateGraphicsPipeline(lightingDesc));
    }

  private:
    void Draw(TextureHandle backbuffer) override {
        const auto dimensions = backbuffer->GetDesc();
        imageWidth = dimensions.width;
        imageHeight = dimensions.height;

        ResourceScope frameResources(device);
        auto *commands = frameResources.Keep(device.AcquireCommandList());

        // 전체 화면 삼각형으로 절차적 구·바닥을 계산한다. 정점 수 3은 구체 개수가 아니다.
        {
            const ResourceBarrierDesc backbufferBarrier{
                nullptr, backbuffer, ResourceState::Present, ResourceState::RenderTarget, {}};
            commands->ResourceBarrier(&backbufferBarrier, 1);
            ColorAttachment colorAttachment{};
            colorAttachment.texture = backbuffer;
            colorAttachment.loadOp = LoadOp::Clear;
            colorAttachment.storeOp = StoreOp::Store;
            commands->BeginRendering({&colorAttachment, 1, nullptr});
            commands->BindGraphicsPipeline(lightingPipeline);
            commands->SetViewport({0, 0, float(imageWidth), float(imageHeight), 0, 1});
            commands->SetScissor({0, 0, imageWidth, imageHeight});
            commands->SetInlineConstants(0, sizeof(params), &params);
            commands->DrawInstanced(3, 1, 0, 0);
            commands->EndRendering();
        }

        const ResourceBarrierDesc presentBarrier{
            nullptr, backbuffer, ResourceState::RenderTarget, ResourceState::Present, {}};
        commands->ResourceBarrier(&presentBarrier, 1);
        if (!commands->Close() || !device.Submit(&commands, 1))
            throw std::runtime_error("Frame recording/submission failed.");
        // GPU 명령 제출에 성공한 뒤 공통 Tick()에서 프레임 수를 갱신한다.
    }

    PipelineHandle lightingPipeline = nullptr;
};
} // namespace

// iOS 앱의 진입점. 플랫폼 화면 주소를 받되 옵션 해석과 렌더링은 기존 예제를 사용한다.
std::unique_ptr<Example> advanced::CreatePBRLighting(const void *view, int argc, char **argv) {
    const auto options = ParseOptions(argc, argv);
    if (options.help)
        return nullptr;
    return std::make_unique<Renderer>(view, options);
}

#if !defined(DY_ADVANCED_IOS) && !defined(__ANDROID__)
int main(int argc, char **argv) try {
    const auto options = ParseOptions(argc, argv);
    if (options.help)
        return 0;
    dyf::Platform::Window window(960, 640, "Advanced / 07_PBRLighting");
    if (!window.GetHandle())
        throw std::runtime_error("Window creation failed.");
    Renderer renderer(window.GetHandle(), options);
    while (window.IsRunning()) {
        window.PollEvents();
        if (!window.IsRunning() || !renderer.Tick())
            break;
    }
    renderer.WaitIdle();
    return 0;
} catch (const std::exception &error) {
    std::fprintf(stderr, "PBRLighting: %s\n", error.what());
    return 1;
}
#endif
