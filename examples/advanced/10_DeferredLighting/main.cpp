#include "dyf.h"
#include "dyf/RHI.h"
#include "vertex.h"
#include "geometry.h"
#include "lighting.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using namespace dyf::RHI;
namespace {
// 지연 조명에 필요한 화면 정보와 재질·광원 설정을 셰이더에 전달한다.
// 네 개의 16바이트 배열은 GLSL·HLSL·Metal 셰이더와 같은 메모리 배치를 유지한다.
struct alignas(16) Params {
    float sizeTime[4] = {960, 640, 0, 0};
    float settings[4] = {1, 0.45f, 0.8f, 0.9f};
    float jitter[4] = {};
    float extra[4] = {};
};
static_assert(sizeof(Params) == 64 && alignof(Params) == 16);
static_assert(offsetof(Params, settings) == 16 && offsetof(Params, jitter) == 32 && offsetof(Params, extra) == 48);

// 숫자 뒤에 다른 문자가 붙었거나 허용 범위를 벗어난 입력은 거부한다.
double Number(const std::string& value, double minimum, double maximum) {
    size_t consumed = 0;
    double result = std::stod(value, &consumed);
    if (consumed != value.size() || !std::isfinite(result) || result < minimum || result > maximum)
        throw std::runtime_error("Numeric option out of range: " + value);
    return result;
}

// GPU의 렌더링 결과를 읽어 RGB 순서의 PPM 이미지로 저장한다.
void Capture(IDevice& device, TextureHandle texture, const std::string& path) {
    TextureReadback image;
    if (!device.ReadTexture(texture, image)) throw std::runtime_error("ReadTexture failed.");
    std::ofstream stream(path, std::ios::binary);
    if (!stream) throw std::runtime_error("Cannot open capture path.");
    stream << "P6\n" << image.width << " " << image.height << "\n255\n";
    const bool bgra = image.format == Format::B8G8R8A8_UNORM || image.format == Format::B8G8R8A8_UNORM_SRGB;
    for (uint32_t y = 0; y < image.height; y++)
        for (uint32_t x = 0; x < image.width; x++) {
            const auto* pixel = image.pixels.data() + size_t(y) * image.rowPitch + size_t(x) * 4;
            const char rgb[3] = {char(pixel[bgra ? 2 : 0]), char(pixel[1]), char(pixel[bgra ? 0 : 2])};
            stream.write(rgb, 3);
        }
    if (!stream) throw std::runtime_error("Capture write failed.");
    std::printf("Captured %s (%ux%u).\n", path.c_str(), image.width, image.height);
}

}

int main(int argc, char** argv) try {
    // Params의 기본 재질 설정을 사용하고, 광원 수의 기본값을 지정한다.
    Params params;
    params.extra[2] = 24;
    uint64_t frameLimit = 0;
    bool framesSpecified = false, validation = false, fixedTime = false;
    float timeValue = 0;
    std::string capture;

    // 실행 인자를 읽어 표시 모드와 관찰할 값을 변경한다. 도움말 요청은 여기서 종료한다.
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--help") {
            std::puts(
                "DeferredLighting\n--mode lit|normal|albedo|position, --lights [0,64], --roughness [0.045,1], --metallic [0,1], --exposure [0.01,20]\n--frames N (0=unlimited), --capture path.ppm, --time SECONDS, --validation\n--capture defaults to 1 frames unless --frames is supplied.");
            return 0;
        }
        if (arg == "--validation") {
            validation = true;
            continue;
        }
        if (i + 1 >= argc) throw std::runtime_error("Missing value for " + arg);
        std::string value = argv[++i];
        if (arg == "--frames") {
            double n = Number(value, 0, 10000000);
            if (std::floor(n) != n) throw std::runtime_error("Frame count must be an integer.");
            frameLimit = uint64_t(n);
            framesSpecified = true;
        } else if (arg == "--capture")
            capture = value;
        else if (arg == "--time") {
            timeValue = float(Number(value, 0, 1000000));
            fixedTime = true;
        } else if (arg == "--mode") {
            if (value == "lit")
                params.sizeTime[3] = 0;
            else if (value == "normal")
                params.sizeTime[3] = 1;
            else if (value == "albedo")
                params.sizeTime[3] = 2;
            else if (value == "position")
                params.sizeTime[3] = 3;
            else
                throw std::runtime_error("Unknown mode: " + value);
        } else if (arg == "--lights") {
            double n = Number(value, 0, 64);
            if (std::floor(n) != n) throw std::runtime_error("This option requires an integer.");
            params.extra[2] = float(n);
        } else if (arg == "--roughness") {
            double n = Number(value, 0.045, 1);
            params.settings[1] = float(n);
        } else if (arg == "--metallic") {
            double n = Number(value, 0, 1);
            params.settings[2] = float(n);
        } else if (arg == "--exposure") {
            double n = Number(value, 0.01, 20);
            params.settings[0] = float(n);
        } else
            throw std::runtime_error("Unknown option: " + arg);
    }

    // 캡처할 마지막 프레임을 결정한다. 캡처 요청에는 유한한 프레임 수가 필요하다.
    if (!capture.empty() && !framesSpecified) frameLimit = 1;
    if (!capture.empty() && frameLimit == 0) throw std::runtime_error("Capture requires a finite frame count.");

    // 창과 그래픽 장치, 화면 표시용 스왑체인을 생성한다.
    dyf::Platform::Window window(960, 640, "Advanced / DeferredLighting");
    if (!window.GetHandle()) throw std::runtime_error("Window creation failed.");
    DeviceDesc deviceDesc;
    deviceDesc.enableValidation = validation;
    std::unique_ptr<IDevice> owner(IDevice::Create(deviceDesc));
    if (!owner) throw std::runtime_error("Device creation failed.");
    auto& device = *owner;

    ResourceScope resources(device);
    SwapchainDesc swap;
    swap.window = window.GetHandle();
    swap.format = Format::B8G8R8A8_UNORM;
    swap.minimumImageCount = 2;
    swap.presentMode = PresentMode::Fifo;
    swap.allowReadback = !capture.empty();
    if (!device.CreateSwapchain(swap)) throw std::runtime_error("Swapchain creation failed.");

    // 빌드 시 포함된 셰이더와 텍스처를 읽을 샘플러를 준비한다.
    auto vertexShader = resources.Keep(device.CreateShader(
        {ShaderStage::Vertex, ShaderData::vertexEntryPoint, ShaderData::vertex, ShaderData::vertexSize}));
    auto geometryShader = resources.Keep(device.CreateShader(
        {ShaderStage::Fragment, ShaderData::geometryEntryPoint, ShaderData::geometry, ShaderData::geometrySize}));
    auto lightingShader = resources.Keep(device.CreateShader(
        {ShaderStage::Fragment, ShaderData::lightingEntryPoint, ShaderData::lighting, ShaderData::lightingSize}));
    SamplerDesc sampler;
    sampler.minFilter = SamplerFilter::Nearest;
    sampler.magFilter = SamplerFilter::Nearest;
    sampler.mipFilter = SamplerFilter::Nearest;
    sampler.addressU = sampler.addressV = sampler.addressW = SamplerAddressMode::ClampToEdge;
    sampler.mipLodBias = sampler.minLod = sampler.maxLod = 0;

    // 위치, 법선·거칠기, 색상·금속성을 세 버퍼에 기록할 파이프라인.
    std::vector<ResourceBindingLayout> geometryBindings;
    std::array<ColorAttachmentDesc, 3> geometryColors{};
    for (auto& attachment : geometryColors)
        attachment = {Format::R16G16B16A16_FLOAT, {}, ColorWriteMask::All};
    GraphicsPipelineDesc geometryDesc;
    geometryDesc.vertexShader = vertexShader;
    geometryDesc.fragmentShader = geometryShader;
    geometryDesc.topology = PrimitiveTopology::TriangleList;
    geometryDesc.raster = {FillMode::Solid, CullMode::None, FrontFace::CounterClockwise, 0, 0, 0};
    geometryDesc.colorAttachments = geometryColors.data();
    geometryDesc.colorAttachmentCount = uint32_t(geometryColors.size());
    geometryDesc.layout = {geometryBindings.data(), uint32_t(geometryBindings.size()), sizeof(Params),
                           ShaderStageFlags::Fragment, 15};
    auto geometryPipeline = resources.Keep(device.CreateGraphicsPipeline(geometryDesc));

    // 앞 단계에서 만든 텍스처를 읽어 최종 조명 결과를 계산할 파이프라인.
    std::vector<ResourceBindingLayout> lightingBindings;
    lightingBindings.push_back({0, ResourceBindingType::SampledTexture, 1, ShaderStageFlags::Fragment, {}});
    lightingBindings.push_back({1, ResourceBindingType::SampledTexture, 1, ShaderStageFlags::Fragment, {}});
    lightingBindings.push_back({2, ResourceBindingType::SampledTexture, 1, ShaderStageFlags::Fragment, {}});
    lightingBindings.push_back({8, ResourceBindingType::StaticSampler, 1, ShaderStageFlags::Fragment, sampler});
    std::array<ColorAttachmentDesc, 1> lightingColors{};
    for (auto& attachment : lightingColors)
        attachment = {Format::B8G8R8A8_UNORM, {}, ColorWriteMask::All};
    GraphicsPipelineDesc lightingDesc;
    lightingDesc.vertexShader = vertexShader;
    lightingDesc.fragmentShader = lightingShader;
    lightingDesc.topology = PrimitiveTopology::TriangleList;
    lightingDesc.raster = {FillMode::Solid, CullMode::None, FrontFace::CounterClockwise, 0, 0, 0};
    lightingDesc.colorAttachments = lightingColors.data();
    lightingDesc.colorAttachmentCount = uint32_t(lightingColors.size());
    lightingDesc.layout = {lightingBindings.data(), uint32_t(lightingBindings.size()), sizeof(Params),
                           ShaderStageFlags::Fragment, 15};
    auto lightingPipeline = resources.Keep(device.CreateGraphicsPipeline(lightingDesc));

    // 크기 변경 시 다시 만들 텍스처·리소스 세트와 프레임 사이에 유지할 상태를 보관한다.
    std::unique_ptr<ResourceScope> imageResources;
    uint32_t imageWidth = 0, imageHeight = 0;
    bool initializeImages = true;
    TextureHandle positionBuffer = nullptr;
    TextureHandle normalBuffer = nullptr;
    TextureHandle albedoBuffer = nullptr;
    ResourceSetHandle lightingSet = nullptr;

    uint64_t frame = 0;

    // 창 이벤트를 처리하고 렌더링 가능한 프레임에서만 작업을 진행한다.
    while (!frameLimit || frame < frameLimit) {
        window.PollEvents();
        if (!window.IsRunning()) break;
        if (!device.BeginFrame()) {
            if (device.IsLost()) throw std::runtime_error("Device lost.");
            continue;
        }
        auto backbuffer = device.GetBackBuffer();
        if (!backbuffer) throw std::runtime_error("Backbuffer unavailable.");
        const auto dimensions = backbuffer->GetDesc();

        // 화면 크기에 맞는 위치·법선·색상 버퍼를 만들고, 조명 단계에서 읽을 수 있도록 연결한다.
        if (dimensions.width != imageWidth || dimensions.height != imageHeight) {
            if (!device.WaitIdle()) throw std::runtime_error("Resize wait failed.");
            imageResources = std::make_unique<ResourceScope>(device);
            imageWidth = dimensions.width;
            imageHeight = dimensions.height;
            initializeImages = true;
            TextureDesc positionBufferDesc;
            positionBufferDesc.width = imageWidth;
            positionBufferDesc.height = imageHeight;
            positionBufferDesc.format = Format::R16G16B16A16_FLOAT;
            positionBufferDesc.usage = TextureUsage::RenderTarget | TextureUsage::ShaderResource;
            positionBuffer = imageResources->Keep(device.CreateTexture(positionBufferDesc));
            TextureDesc normalBufferDesc;
            normalBufferDesc.width = imageWidth;
            normalBufferDesc.height = imageHeight;
            normalBufferDesc.format = Format::R16G16B16A16_FLOAT;
            normalBufferDesc.usage = TextureUsage::RenderTarget | TextureUsage::ShaderResource;
            normalBuffer = imageResources->Keep(device.CreateTexture(normalBufferDesc));
            TextureDesc albedoBufferDesc;
            albedoBufferDesc.width = imageWidth;
            albedoBufferDesc.height = imageHeight;
            albedoBufferDesc.format = Format::R16G16B16A16_FLOAT;
            albedoBufferDesc.usage = TextureUsage::RenderTarget | TextureUsage::ShaderResource;
            albedoBuffer = imageResources->Keep(device.CreateTexture(albedoBufferDesc));

            std::vector<ResourceBinding> lightingSetBindings;
            lightingSetBindings.push_back({0, 0, nullptr, positionBuffer, 0, 0, {}});
            lightingSetBindings.push_back({1, 0, nullptr, normalBuffer, 0, 0, {}});
            lightingSetBindings.push_back({2, 0, nullptr, albedoBuffer, 0, 0, {}});
            lightingSet = imageResources->Keep(device.CreateResourceSet(
                {lightingPipeline, lightingSetBindings.data(), uint32_t(lightingSetBindings.size())}));
        }

        // 현재 화면 크기와 시간을 셰이더 설정에 반영한다. 시간 고정 옵션이 없으면 프레임 수로 계산한다.
        params.sizeTime[0] = float(imageWidth);
        params.sizeTime[1] = float(imageHeight);
        params.sizeTime[2] = fixedTime ? timeValue : float(frame) / 60.0f;

        ResourceScope frameResources(device);
        auto* commands = frameResources.Keep(device.AcquireCommandList());

        // 새로 만든 텍스처를 초기화하고, 이후 렌더링 단계에서 사용할 읽기 상태로 전환한다.
        if (initializeImages) {
            {
                const ResourceBarrierDesc barrier{
                    nullptr, positionBuffer, ResourceState::Undefined, ResourceState::RenderTarget, {}};
                commands->ResourceBarrier(&barrier, 1);
                ColorAttachment a;
                a.texture = positionBuffer;
                a.loadOp = LoadOp::Clear;
                a.storeOp = StoreOp::Store;
                commands->BeginRendering({&a, 1, nullptr});
                commands->EndRendering();
                const ResourceBarrierDesc ready{
                    nullptr, positionBuffer, ResourceState::RenderTarget, ResourceState::ShaderResource, {}};
                commands->ResourceBarrier(&ready, 1);
            }
            {
                const ResourceBarrierDesc barrier{
                    nullptr, normalBuffer, ResourceState::Undefined, ResourceState::RenderTarget, {}};
                commands->ResourceBarrier(&barrier, 1);
                ColorAttachment a;
                a.texture = normalBuffer;
                a.loadOp = LoadOp::Clear;
                a.storeOp = StoreOp::Store;
                commands->BeginRendering({&a, 1, nullptr});
                commands->EndRendering();
                const ResourceBarrierDesc ready{
                    nullptr, normalBuffer, ResourceState::RenderTarget, ResourceState::ShaderResource, {}};
                commands->ResourceBarrier(&ready, 1);
            }
            {
                const ResourceBarrierDesc barrier{
                    nullptr, albedoBuffer, ResourceState::Undefined, ResourceState::RenderTarget, {}};
                commands->ResourceBarrier(&barrier, 1);
                ColorAttachment a;
                a.texture = albedoBuffer;
                a.loadOp = LoadOp::Clear;
                a.storeOp = StoreOp::Store;
                commands->BeginRendering({&a, 1, nullptr});
                commands->EndRendering();
                const ResourceBarrierDesc ready{
                    nullptr, albedoBuffer, ResourceState::RenderTarget, ResourceState::ShaderResource, {}};
                commands->ResourceBarrier(&ready, 1);
            }
        }

        // 1. 장면의 위치, 법선·거칠기, 색상·금속성을 세 개의 G-buffer에 나누어 기록한다.
        {
            const ResourceBarrierDesc positionBufferBarrier{
                nullptr, positionBuffer, ResourceState::ShaderResource, ResourceState::RenderTarget, {}};
            commands->ResourceBarrier(&positionBufferBarrier, 1);
            const ResourceBarrierDesc normalBufferBarrier{
                nullptr, normalBuffer, ResourceState::ShaderResource, ResourceState::RenderTarget, {}};
            commands->ResourceBarrier(&normalBufferBarrier, 1);
            const ResourceBarrierDesc albedoBufferBarrier{
                nullptr, albedoBuffer, ResourceState::ShaderResource, ResourceState::RenderTarget, {}};
            commands->ResourceBarrier(&albedoBufferBarrier, 1);
            std::array<ColorAttachment, 3> colors{};
            colors[0].texture = positionBuffer;
            colors[0].loadOp = LoadOp::Clear;
            colors[0].storeOp = StoreOp::Store;
            colors[1].texture = normalBuffer;
            colors[1].loadOp = LoadOp::Clear;
            colors[1].storeOp = StoreOp::Store;
            colors[2].texture = albedoBuffer;
            colors[2].loadOp = LoadOp::Clear;
            colors[2].storeOp = StoreOp::Store;
            commands->BeginRendering({colors.data(), uint32_t(colors.size()), nullptr});
            commands->BindGraphicsPipeline(geometryPipeline);
            commands->SetViewport({0, 0, float(imageWidth), float(imageHeight), 0, 1});
            commands->SetScissor({0, 0, imageWidth, imageHeight});
            commands->SetInlineConstants(0, sizeof(params), &params);
            commands->DrawInstanced(3, 1, 0, 0);
            commands->EndRendering();
            const ResourceBarrierDesc positionBufferReady{
                nullptr, positionBuffer, ResourceState::RenderTarget, ResourceState::ShaderResource, {}};
            commands->ResourceBarrier(&positionBufferReady, 1);
            const ResourceBarrierDesc normalBufferReady{
                nullptr, normalBuffer, ResourceState::RenderTarget, ResourceState::ShaderResource, {}};
            commands->ResourceBarrier(&normalBufferReady, 1);
            const ResourceBarrierDesc albedoBufferReady{
                nullptr, albedoBuffer, ResourceState::RenderTarget, ResourceState::ShaderResource, {}};
            commands->ResourceBarrier(&albedoBufferReady, 1);
        }

        // 2. G-buffer에 저장한 표면 정보를 읽어 여러 광원의 조명 결과를 계산한다.
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

        // 마지막 프레임의 캡처를 저장한 뒤 결과를 화면에 표시한다.
        ++frame;
        if (!capture.empty() && frame == frameLimit) Capture(device, backbuffer, capture);
        if (!device.Present()) throw std::runtime_error("Present failed.");
    }

    if (!device.WaitIdle()) throw std::runtime_error("Final GPU wait failed.");
    return 0;
} catch (const std::exception& error) {
    std::fprintf(stderr, "DeferredLighting: %s\n", error.what());
    return 1;
}
