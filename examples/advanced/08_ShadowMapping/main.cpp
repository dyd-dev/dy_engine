// 데스크톱은 main의 루프, iOS는 CreateShadowMapping으로 만든 객체의 Tick을 사용한다.
// Draw는 그림자 깊이 패스 → 그림자 맵을 읽는 조명 패스 순서로 명령을 제출한다.
#include "../common/Example.h"
#ifndef DY_ADVANCED_IOS
#include "dyf/Platform/Window.h"
#endif
#include <array>
#include <vector>

using namespace dyf::RHI;
using namespace advanced;
namespace {
// 같은 앱에 두 예제를 연결해도 셰이더 데이터의 이름이 충돌하지 않는다.
#include "vertex.h"
#include "shadowDepth.h"
#include "lighting.h"

Options ParseOptions(int argc, char **argv) {
    Options options;
    options.params.settings[1] = 0.0015f;
    options.params.settings[2] = 1;
    options.params.settings[3] = 0;
    options.params.extra[0] = 8;
    options.params.extra[1] = 4;

    // 실행 인자를 읽어 표시 모드와 관찰할 값을 변경한다. 도움말 요청은 여기서 종료한다.
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--help") {
            std::puts("ShadowMapping\n--shadow-type directional|cascaded|point, --mode "
                      "shadow|unshadowed|depth|visibility|regions\n--bias [0,0.1], --pcf [0,2] (integer), "
                      "--split [4,16], --light-height [2,8], --exposure [0.01,20]\nCascades use two manually "
                      "fitted camera-distance intervals; point atlas stores radial depth.\n--frames N "
                      "(0=unlimited), --capture path.ppm, --time SECONDS, --validation\n--capture defaults "
                      "to 1 frames unless --frames is supplied.");
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
            if (value == "shadow")
                options.params.sizeTime[3] = 0;
            else if (value == "unshadowed")
                options.params.sizeTime[3] = 1;
            else if (value == "depth")
                options.params.sizeTime[3] = 2;
            else if (value == "visibility")
                options.params.sizeTime[3] = 3;
            else if (value == "regions")
                options.params.sizeTime[3] = 4;
            else
                throw std::runtime_error("Unknown mode: " + value);
        } else if (arg == "--shadow-type") {
            if (value == "directional")
                options.params.settings[3] = 0;
            else if (value == "cascaded")
                options.params.settings[3] = 1;
            else if (value == "point")
                options.params.settings[3] = 2;
            else
                throw std::runtime_error("Unknown shadow type.");
        } else if (arg == "--bias") {
            double n = Number(value, 0, 0.1);
            options.params.settings[1] = float(n);
        } else if (arg == "--pcf") {
            double n = Number(value, 0, 2);
            if (std::floor(n) != n)
                throw std::runtime_error("This option requires an integer.");
            options.params.settings[2] = float(n);
        } else if (arg == "--split") {
            double n = Number(value, 4, 16);
            options.params.extra[0] = float(n);
        } else if (arg == "--light-height") {
            double n = Number(value, 2, 8);
            options.params.extra[1] = float(n);
        } else if (arg == "--exposure") {
            double n = Number(value, 0.01, 20);
            options.params.settings[0] = float(n);
        } else
            throw std::runtime_error("Unknown option: " + arg);
    }

    // 캡처할 마지막 프레임을 결정한다. 캡처 요청에는 유한한 프레임 수가 필요하다.
    if (!options.capture.empty() && !options.framesSpecified)
        options.frameLimit = 1;
    if (!options.capture.empty() && options.frameLimit == 0)
        throw std::runtime_error("Capture requires a finite frame count.");

    return options;
}

class Renderer final : public Example {
  public:
    Renderer(const void *window, const Options &options) : Example(window, options) {
        // 빌드 시 포함된 셰이더와 텍스처를 읽을 샘플러를 준비한다.
        auto vertexShader = resources.Keep(device.CreateShader(
            {ShaderStage::Vertex, ShaderData::vertexEntryPoint, ShaderData::vertex, ShaderData::vertexSize}));
        auto shadowDepthShader =
            resources.Keep(device.CreateShader({ShaderStage::Fragment, ShaderData::shadowDepthEntryPoint,
                                                ShaderData::shadowDepth, ShaderData::shadowDepthSize}));
        auto lightingShader =
            resources.Keep(device.CreateShader({ShaderStage::Fragment, ShaderData::lightingEntryPoint,
                                                ShaderData::lighting, ShaderData::lightingSize}));
        SamplerDesc sampler;
        sampler.minFilter = SamplerFilter::Nearest;
        sampler.magFilter = SamplerFilter::Nearest;
        sampler.mipFilter = SamplerFilter::Nearest;
        sampler.addressU = sampler.addressV = sampler.addressW = SamplerAddressMode::ClampToEdge;
        sampler.mipLodBias = sampler.minLod = sampler.maxLod = 0;

        // 광원에서 본 깊이를 그림자 텍스처에 기록할 파이프라인.
        std::vector<ResourceBindingLayout> shadowBindings;
        std::array<ColorAttachmentDesc, 0> shadowColors{};
        for (auto &attachment : shadowColors)
            attachment = {ExampleColorFormat, {}, ColorWriteMask::All};
        GraphicsPipelineDesc shadowDesc;
        shadowDesc.vertexShader = vertexShader;
        shadowDesc.fragmentShader = shadowDepthShader;
        shadowDesc.topology = PrimitiveTopology::TriangleList;
        shadowDesc.raster = {FillMode::Solid, CullMode::None, FrontFace::CounterClockwise, 0, 0, 0};
        shadowDesc.colorAttachments = shadowColors.data();
        shadowDesc.colorAttachmentCount = uint32_t(shadowColors.size());
        shadowDesc.depthStencil.format = Format::D32_FLOAT;
        shadowDesc.depthStencil.depthWriteEnabled = true;
        shadowDesc.depthStencil.depthTestEnabled = true;
        shadowDesc.depthStencil.depthCompareOp = CompareOp::Always;
        shadowDesc.layout = {shadowBindings.data(), uint32_t(shadowBindings.size()), sizeof(Params),
                             ShaderStageFlags::Fragment, 15};
        shadowPipeline = resources.Keep(device.CreateGraphicsPipeline(shadowDesc));

        // 앞 단계에서 만든 텍스처를 읽어 최종 조명 결과를 계산할 파이프라인.
        std::vector<ResourceBindingLayout> lightingBindings;
        lightingBindings.push_back(
            {0, ResourceBindingType::SampledTexture, 1, ShaderStageFlags::Fragment, {}});
        lightingBindings.push_back(
            {8, ResourceBindingType::StaticSampler, 1, ShaderStageFlags::Fragment, sampler});
        std::array<ColorAttachmentDesc, 1> lightingColors{};
        for (auto &attachment : lightingColors)
            attachment = {ExampleColorFormat, {}, ColorWriteMask::All};
        GraphicsPipelineDesc lightingDesc;
        lightingDesc.vertexShader = vertexShader;
        lightingDesc.fragmentShader = lightingShader;
        lightingDesc.topology = PrimitiveTopology::TriangleList;
        lightingDesc.raster = {FillMode::Solid, CullMode::None, FrontFace::CounterClockwise, 0, 0, 0};
        lightingDesc.colorAttachments = lightingColors.data();
        lightingDesc.colorAttachmentCount = uint32_t(lightingColors.size());
        lightingDesc.layout = {lightingBindings.data(), uint32_t(lightingBindings.size()), sizeof(Params),
                               ShaderStageFlags::Fragment, 15};
        lightingPipeline = resources.Keep(device.CreateGraphicsPipeline(lightingDesc));
    }

  private:
    void Draw(TextureHandle backbuffer) override {
        const auto dimensions = backbuffer->GetDesc();

        // 첫 프레임 또는 화면 크기 변경 시 기존 GPU 작업을 기다리고 그림자 텍스처와 연결을 다시 만든다.
        // 그림자 텍스처 자체는 고정 크기 1536×1024를 사용한다.
        if (dimensions.width != imageWidth || dimensions.height != imageHeight) {
            if (!device.WaitIdle())
                throw std::runtime_error("Resize wait failed.");
            imageResources = std::make_unique<ResourceScope>(device);
            imageWidth = dimensions.width;
            imageHeight = dimensions.height;
            initializeImages = true;
            TextureDesc shadowMapDesc;
            shadowMapDesc.width = 1536;
            shadowMapDesc.height = 1024;
            shadowMapDesc.format = Format::D32_FLOAT;
            shadowMapDesc.usage = TextureUsage::DepthStencil | TextureUsage::ShaderResource;
            shadowMap = imageResources->Keep(device.CreateTexture(shadowMapDesc));

            std::vector<ResourceBinding> lightingSetBindings;
            lightingSetBindings.push_back({0, 0, nullptr, shadowMap, 0, 0, {}});
            lightingSet = imageResources->Keep(device.CreateResourceSet(
                {lightingPipeline, lightingSetBindings.data(), uint32_t(lightingSetBindings.size())}));
        }

        ResourceScope frameResources(device);
        auto *commands = frameResources.Keep(device.AcquireCommandList());

        // 새로 만든 텍스처를 초기화하고, 이후 렌더링 단계에서 사용할 읽기 상태로 전환한다.
        if (initializeImages) {
            {
                const ResourceBarrierDesc barrier{
                    nullptr, shadowMap, ResourceState::Undefined, ResourceState::DepthWrite, {}};
                commands->ResourceBarrier(&barrier, 1);
                DepthStencilAttachment a;
                a.texture = shadowMap;
                a.state = ResourceState::DepthWrite;
                a.depthLoadOp = LoadOp::Clear;
                a.depthStoreOp = StoreOp::Store;
                a.clearDepth = 1;
                commands->BeginRendering({nullptr, 0, &a});
                commands->EndRendering();
                const ResourceBarrierDesc ready{
                    nullptr, shadowMap, ResourceState::DepthWrite, ResourceState::ShaderResource, {}};
                commands->ResourceBarrier(&ready, 1);
            }
        }

        // 1. 선택한 그림자 방식에 따라 깊이 텍스처에 방향광 1개, 캐스케이드 2개 또는 점광원 6면을 기록한다.
        {
            const ResourceBarrierDesc shadowMapBarrier{
                nullptr, shadowMap, ResourceState::ShaderResource, ResourceState::DepthWrite, {}};
            commands->ResourceBarrier(&shadowMapBarrier, 1);
            DepthStencilAttachment depth;
            depth.texture = shadowMap;
            depth.state = ResourceState::DepthWrite;
            depth.depthLoadOp = LoadOp::Clear;
            depth.depthStoreOp = StoreOp::Store;
            depth.clearDepth = 1;
            commands->BeginRendering({nullptr, 0, &depth});
            commands->BindGraphicsPipeline(shadowPipeline);
            commands->SetViewport({0, 0, float(1536), float(1024), 0, 1});
            commands->SetScissor({0, 0, 1536, 1024});
            commands->SetInlineConstants(0, sizeof(params), &params);
            commands->DrawInstanced(3, 1, 0, 0);
            commands->EndRendering();
            const ResourceBarrierDesc shadowMapReady{
                nullptr, shadowMap, ResourceState::DepthWrite, ResourceState::ShaderResource, {}};
            commands->ResourceBarrier(&shadowMapReady, 1);
        }

        // 2. 해당 그림자 영역을 선택하고, 영역 경계를 넘지 않는 PCF 깊이 비교로 최종 화면을 그린다.
        {
            const ResourceBarrierDesc backbufferBarrier{
                nullptr, backbuffer, ResourceState::Present, ResourceState::RenderTarget, {}};
            commands->ResourceBarrier(&backbufferBarrier, 1);
            std::array<ColorAttachment, 1> colors{};
            colors[0].texture = backbuffer;
            colors[0].loadOp = LoadOp::Clear;
            colors[0].storeOp = StoreOp::Store;
            commands->BeginRendering({colors.data(), uint32_t(colors.size()), nullptr});
            commands->BindGraphicsPipeline(lightingPipeline);
            commands->BindResourceSet(lightingSet);
            commands->SetViewport({0, 0, float(imageWidth), float(imageHeight), 0, 1});
            commands->SetScissor({0, 0, imageWidth, imageHeight});
            commands->SetInlineConstants(0, sizeof(params), &params);
            commands->DrawInstanced(3, 1, 0, 0);
            commands->EndRendering();
        }

        // 최종 이미지를 표시용 상태로 전환하고 프레임 명령을 제출한다.
        const ResourceBarrierDesc presentBarrier{
            nullptr, backbuffer, ResourceState::RenderTarget, ResourceState::Present, {}};
        commands->ResourceBarrier(&presentBarrier, 1);
        if (!commands->Close() || !device.Submit(&commands, 1))
            throw std::runtime_error("Frame recording/submission failed.");
        // 명령 제출에 성공한 경우에만 CPU의 프레임 상태를 갱신한다. GPU 작업은 같은 큐의 제출 순서를 따른다.
        initializeImages = false;
    }

    PipelineHandle lightingPipeline = nullptr;
    PipelineHandle shadowPipeline = nullptr;
    bool initializeImages = true;
    TextureHandle shadowMap = nullptr;
    ResourceSetHandle lightingSet = nullptr;
};
} // namespace

// iOS 앱의 진입점. 예제 전환 시 새 객체를 만들어 파이프라인과 그림자 자원을 다시 준비한다.
std::unique_ptr<Example> advanced::CreateShadowMapping(const void *view, int argc, char **argv) {
    const auto options = ParseOptions(argc, argv);
    if (options.help)
        return nullptr;
    return std::make_unique<Renderer>(view, options);
}

#ifndef DY_ADVANCED_IOS
int main(int argc, char **argv) try {
    const auto options = ParseOptions(argc, argv);
    if (options.help)
        return 0;
    dyf::Platform::Window window(960, 640, "Advanced / ShadowMapping");
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
    std::fprintf(stderr, "ShadowMapping: %s\n", error.what());
    return 1;
}
#endif
