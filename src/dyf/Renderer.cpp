#include "../Platform/Profiler/CollectorInternal.h"
#include "RendererInternal.h"
#include "RendererDiagnostics.h"
#include <cstdio>
#include "dyf/Canvas.h"
#include "SceneAlgorithms.h"
#include "BasicEnvironmentResources.h"
#include "BasicLightingResources.h"
#include "OutputPresentation.h"
#include "OutputColor.h"
#include "dyf/RenderExtension.h"
#include "GpuVisibility.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

#include "dyf/Image.h"
#include "dyf/RHI/Readback.h"
#include <cstring>
#include <exception>
#include "ShaderLayout.h"
#include "dyf/Platform/Profiler.h"
#include "dyf/Platform/Window.h"
#include "dyf/Camera.h"
#include "dyf/Scene.h"
#include "dyf/Math/Math.h"
#include "dyf/RHI/Buffer.h"
#include "dyf/RHI/ICommandList.h"
#include "dyf/RHI/RenderGraph.h"
#include <stdexcept>
#include "dyf/RHI/IDevice.h"
#include "dyf/RHI/Pipeline.h"
#include "dyf/RHI/Shader.h"
#include "dyf/RHI/Texture.h"

using namespace dyf;


namespace
{
    // GPU 자원 생성 전에 설정의 숫자 범위와 기능 조합을 검사한다. 조건을 어기면 RendererFailure에 오류 문자열을 넘겨 stderr에 남기고 false를 반환한다.
    bool ValidateRendererConfig(const RendererConfig& desc)
    {
        if(!ValidateOutputConfig(desc.postProcess)) // 색보정/샤프닝 입력이 허용 범위를 벗어나면 설정을 거부한다.
            return RendererFailure("Invalid color grading or sharpening configuration.");
        for(const auto& color : {Math::float3{desc.clearColor.x,desc.clearColor.y,desc.clearColor.z},
            desc.lighting.ambientColor,desc.lighting.environment.diffuseColor,desc.lighting.environment.specularColor})
            if(!std::isfinite(color.x) || !std::isfinite(color.y) || !std::isfinite(color.z)) // RGB 중 NaN·무한대가 있으면 이후 조명 계산에 전달하지 않는다.
                return RendererFailure("Invalid Renderer configuration.");
        if(!std::isfinite(desc.clearColor.w) || // 알파가 비유한 값이거나 노출·광원 강도가 비유한/음수이면 실패한다.
            !std::isfinite(desc.exposure) || desc.exposure<0 ||
            !std::isfinite(desc.lighting.ambientIntensity) || desc.lighting.ambientIntensity<0 ||
            !std::isfinite(desc.lighting.environment.diffuseIntensity) || desc.lighting.environment.diffuseIntensity<0 ||
            !std::isfinite(desc.lighting.environment.specularIntensity) || desc.lighting.environment.specularIntensity<0)
            return RendererFailure("Invalid Renderer configuration.");
        if(!std::isfinite(desc.hdrRenderScale) || desc.hdrRenderScale<1 || desc.hdrRenderScale>2) // HDR 배율이 유한한 1..2가 아니면 거부한다.
            return RendererFailure("HDR render scale must be finite and between 1 and 2.");
        if((desc.renderWidth==0)!=(desc.renderHeight==0)) // 폭/높이 중 하나만 고정한 불완전한 출력 크기는 거부한다.
            return RendererFailure("Fixed rendering requires both width and height, or zero for both.");
        if(desc.texturePreparationThreadCount==0) // 텍스처 준비 작업자가 0명인 설정은 허용하지 않는다.
            return RendererFailure("Texture preparation requires at least one thread.");
        // 상태 전환 방향마다 서로 다른 기준을 적용해 잦은 전환을 막는 방식(히스테리시스)의 폭을 검사한다.
        if(!std::isfinite(desc.lodPixelError) || desc.lodPixelError<=0 || // LOD 픽셀 오차는 유한한 양수, 히스테리시스 폭은 유한한 0 이상 0.5 미만이어야 한다.
            !std::isfinite(desc.lodScreenSize) || desc.lodScreenSize<=0 || desc.lodScreenSize>1 ||
            !std::isfinite(desc.lodHysteresis) || desc.lodHysteresis<0 || desc.lodHysteresis>=0.5f)
            return RendererFailure("LOD requires positive pixel error, screen size in (0, 1], and hysteresis in [0, 0.5).");
        if(desc.lighting.shadowQuality > ShadowQuality::High) // 정의된 그림자 품질 열거 범위 밖의 값은 거부한다.
            return RendererFailure("Invalid shadow configuration.");
        if(desc.lighting.shadowBlockerSamples<1 || desc.lighting.shadowBlockerSamples>32 || // 표본 수가 1..32 밖이거나 필터 반경이 음수/비유한 값이면 실패한다.
            desc.lighting.shadowFilterSamples<1 || desc.lighting.shadowFilterSamples>32 ||
            !std::isfinite(desc.lighting.shadowMaxFilterRadius) || desc.lighting.shadowMaxFilterRadius<0)
            return RendererFailure("Soft shadows require 1..32 search/filter samples and a finite non-negative filter radius.");
        if(!std::isfinite(desc.lighting.shadowDistance) || desc.lighting.shadowDistance<0 || // 그림자 거리는 유한한 0 이상, 캐스케이드 혼합 계수는 유한한 0 이상 1 이하가 아니면 거부한다.
            !std::isfinite(desc.lighting.cascadeSplitLambda) || desc.lighting.cascadeSplitLambda<0 ||
            desc.lighting.cascadeSplitLambda>1)
            return RendererFailure("Shadow distance must be non-negative and cascade split lambda must be in [0, 1].");
        if(desc.lighting.maxDirectionalShadowLights>4 || desc.lighting.maxPointShadowLights>16 || // 기본 셰이더가 담을 수 있는 광원별 그림자 예산을 넘으면 거부한다.
            desc.lighting.maxSpotShadowLights>16)
            return RendererFailure("Stock shadow-light budgets support up to 4 directional, 16 point and 16 spot lights.");
        const auto& environment=desc.lighting.environment;
        if(!std::isfinite(environment.intensity) || environment.intensity<0 || // 환경광 강도가 비유한 값 또는 음수이거나 회전각이 비유한 값이면 요청을 거부한다.
            !std::isfinite(environment.rotationRadians)) return RendererFailure("Invalid HDR environment configuration.");
        for(const auto& probe:environment.probes)
        {
            if(!probe.radiance.IsValid() || !std::isfinite(probe.intensity) || probe.intensity<0 || // 프로브의 HDR 이미지가 유효하지 않거나 강도가 비유한 값/음수이면 거부한다. 가장자리 전환 거리도 유한한 양수여야 한다.
                !std::isfinite(probe.fadeDistance) || probe.fadeDistance<=0)
                return RendererFailure("Environment probes require HDR radiance, non-negative intensity and positive fade distance.");
            const float positions[]={probe.position.x,probe.position.y,probe.position.z};
            const float lows[]={probe.boxMin.x,probe.boxMin.y,probe.boxMin.z};
            const float highs[]={probe.boxMax.x,probe.boxMax.y,probe.boxMax.z};
            for(uint32_t axis=0;axis<3;++axis)
            {
                const float position=positions[axis],low=lows[axis],high=highs[axis];
                if(!std::isfinite(position) || !std::isfinite(low) || !std::isfinite(high) || // 비어 있거나 비유한 영향 상자, 또는 상자 내부가 아닌 프로브 위치를 거부한다.
                    low>=high || position<=low || position>=high)
                    return RendererFailure("Environment probe position must be inside its finite, non-empty influence box.");
            }
        }
        const auto& advanced=desc.advanced;
        if(advanced.transparencyLayers<1 || advanced.transparencyLayers>2) // 지원하는 굴절 레이어 수 1 또는 2 외의 값은 거부한다.
            return RendererFailure("Raster transmission supports one or two layers.");
        if(advanced.surfaceDebugView>SurfaceDebugView::Motion) // 지원하지 않는 표면 진단 모드는 거부한다.
            return RendererFailure("Invalid surface debug view.");
        for(float value:{advanced.aoRadius,advanced.aoStrength,advanced.rayDistance,advanced.giStrength,
            advanced.reflectionStrength,advanced.rayThickness,advanced.bloomStrength,advanced.bloomThreshold,advanced.bloomKnee})
            if(!std::isfinite(value) || value<0) return RendererFailure("Invalid advanced rendering configuration."); // 효과 세기·반경·임계값 중 음수/비유한 값이 있으면 실패한다.
        if(advanced.aoRadius<=0 || advanced.rayDistance<=0 || advanced.rayThickness<=0 || // 추적 반경/두께가 양수가 아니거나 이력 가중치가 [0,1) 밖이면 거부한다.
            !std::isfinite(advanced.historyWeight) || advanced.historyWeight<0 || advanced.historyWeight>=1)
            return RendererFailure("Invalid advanced rendering radii or history weight.");
        if(!std::isfinite(advanced.exposureKey) || advanced.exposureKey<=0 || // 자동 노출 기준은 양수, 적응 속도는 유한한 0 이상이어야 한다.
            !std::isfinite(advanced.exposureAdaptationSpeed) || advanced.exposureAdaptationSpeed<0)
            return RendererFailure("Auto exposure requires a positive key and non-negative adaptation speed.");
        const auto& fog=advanced.fog;
        for(float value:{fog.density,fog.heightFalloff,fog.albedo.x,fog.albedo.y,fog.albedo.z})
            if(!std::isfinite(value) || value<0) return RendererFailure("Invalid volumetric fog density or albedo."); // 안개 밀도·높이 감쇠·알베도에 음수/비유한 값이 있으면 실패한다.
        if(!std::isfinite(fog.baseHeight) || !std::isfinite(fog.anisotropy) || std::abs(fog.anisotropy)>=.95f || // 안개 높이와 산란 방향성이 유한해야 하며 방향성의 절댓값은 0.95 미만이어야 한다. 최대 거리는 유한한 양수, 표본 수는 1~256이어야 한다.
        // 해상도 비율은 유한한 0 초과 1 이하, 알베도 각 성분은 1 이하여야 한다. 음수와 비유한 알베도는 앞의 검사에서 이미 거부했다.
            !std::isfinite(fog.maxDistance) || fog.maxDistance<=0 || fog.steps<1 || fog.steps>256 ||
            !std::isfinite(fog.resolutionScale) || fog.resolutionScale<=0 || fog.resolutionScale>1 ||
            fog.albedo.x>1 || fog.albedo.y>1 || fog.albedo.z>1)
            return RendererFailure("Invalid volumetric fog configuration.");
        return true;
    }
}
// 유효한 요청을 별도 RendererConfig로 복사해 예약한다. 실패하거나 예외가 나면 config의 실제 적용값을 바꾸지 않는다.
bool Renderer::Impl::SetConfig(const RendererConfig& desc)
{
    try
    {
        if(!ValidateRendererConfig(desc)) return false; // 입력 검증 실패 시 새 pendingConfig를 만들지 않는다.
        if(desc.debugWireframe && !device->Supports(RHI::Feature::Wireframe)) // 와이어프레임 요청을 장치가 지원하지 않으면 예약도 거부한다.
            return RendererFailure("Wireframe diagnostic is unsupported by this device.");
        pendingConfig=std::make_unique<RendererConfig>(desc);return true;
    }
    catch(const std::exception& error){std::fprintf(stderr,"dyf: Renderer settings: %s\n",error.what());return false;}
}
// 단일 항목 setter들은 GetConfig로 최신 예약 상태를 가져와 수정하므로 연속 요청이 앞선 미적용 변경을 덮어 없애지 않는다.
bool Renderer::Impl::SetClearColor(Math::float4 value) {auto desc=GetConfig();desc.clearColor=value;return SetConfig(desc);}
bool Renderer::Impl::SetVSync(bool value) {auto desc=GetConfig();desc.vsync=value;return SetConfig(desc);}
bool Renderer::Impl::SetLighting(const LightingConfig& value) {auto desc=GetConfig();desc.lighting=value;return SetConfig(desc);}
bool Renderer::Impl::SetLightingEnabled(bool value) {auto desc=GetConfig();desc.lighting.enabled=value;return SetConfig(desc);}
bool Renderer::Impl::SetShadowsEnabled(bool value) {auto desc=GetConfig();desc.lighting.shadows=value;return SetConfig(desc);}
bool Renderer::Impl::SetHdrEnabled(bool value) {auto desc=GetConfig();desc.enableHdrRendering=value;return SetConfig(desc);}
bool Renderer::Impl::SetExposure(float value) {auto desc=GetConfig();desc.exposure=value;return SetConfig(desc);}
bool Renderer::Impl::SetProfilerVisible(bool value) {auto desc=GetConfig();desc.enableProfilerHud=value;return SetConfig(desc);}
bool Renderer::Impl::SetReadbackEnabled(bool value) {auto desc=GetConfig();desc.allowReadback=value;return SetConfig(desc);}
// 창 프레임이 열려 있지 않을 때 확장 모듈을 등록하고 이력을 무효화한다. 성공하면 extensions가 모듈을 소유하며 등록 순서대로 패스를 추가한다. 실패해도 전달받은 unique_ptr를 호출자에게 돌려주지 않으므로 해당 모듈은 소멸한다.
bool Renderer::Impl::AddExtension(std::unique_ptr<RenderExtension> extension)
{
    if(!extension || framePending) return RendererFailure("Render extensions must be registered between frames."); // 확장이 비었거나 창 프레임이 열려 있으면 등록을 거부한다.
    try
    {
        InvalidateFrameHistory();
        extensions.push_back(std::move(extension));
        return true;
    }
    catch(const std::exception& error)
    {
        std::fprintf(stderr,"dyf: Render extension registration: %s\n",error.what());return false;
    }
}
// 셰이더 바이트·진입점·추가 바인딩을 내용으로 비교한다. sampler의 float는 비트 표현을 비교해 복사된 기본 NaN도 동일한 설정으로 인정한다.
bool Renderer::Impl::ShaderSources::operator==(const ShaderSources& other) const
{
    if(bytes!=other.bytes || entries!=other.entries || additionalConstantBytes!=other.additionalConstantBytes ||
        stockShadowCoverage!=other.stockShadowCoverage ||
        vertexBindings.size()!=other.vertexBindings.size()) return false; // 바이트·진입점·추가 상수 크기·기본 그림자 커버리지 계약·바인딩 개수 중 하나라도 다르면 다른 셰이더 설정이다.
    const auto sameFloat=[](float left,float right) {return std::memcmp(&left,&right,sizeof(float))==0;};
    for(size_t i=0;i<vertexBindings.size();++i)
    {
        const auto& left=vertexBindings[i];const auto& right=other.vertexBindings[i];
        if(left.binding!=right.binding || left.type!=right.type || left.count!=right.count || left.stages!=right.stages) // 바인딩 위치·종류·개수·가시 셰이더 단계가 다르면 같지 않다.
            return false;
        const auto& a=left.staticSampler;const auto& b=right.staticSampler;
        if(a.minFilter!=b.minFilter || a.magFilter!=b.magFilter || a.mipFilter!=b.mipFilter || // 샘플러 동작이나 float의 저장 표현이 다르면 같은 레이아웃으로 재사용하지 않는다.
            a.addressU!=b.addressU || a.addressV!=b.addressV || a.addressW!=b.addressW ||
            a.borderColor!=b.borderColor || a.maxAnisotropy!=b.maxAnisotropy ||
            !sameFloat(a.mipLodBias,b.mipLodBias) || !sameFloat(a.minLod,b.minLod) || !sameFloat(a.maxLod,b.maxLod))
            return false;
    }
    return true;
}
// 각 단계의 종류·바이트·진입점을 검사한 뒤 자체 저장소에 복사한다. 입력 포인터의 호출자 수명과 다음 프레임 적용 시점을 분리하는 부분이다.
bool Renderer::Impl::SetShaders(const RendererShaderDesc& desc)
{
    try
    {
        const std::array<RHI::ShaderDesc,ShaderCount> inputs={desc.meshVertex,desc.meshFragment,desc.shadowVertex,
            desc.canvasVertex,desc.canvasFragment,desc.toneMapVertex,desc.toneMapFragment};
        const std::array<RHI::ShaderStage,ShaderCount> stages={RHI::ShaderStage::Vertex,RHI::ShaderStage::Fragment,
            RHI::ShaderStage::Vertex,RHI::ShaderStage::Vertex,RHI::ShaderStage::Fragment,
            RHI::ShaderStage::Vertex,RHI::ShaderStage::Fragment};
        ShaderSources next;
        next.vertexBindings = desc.vertexBindings;
        next.additionalConstantBytes = desc.additionalConstantBytes;
        next.stockShadowCoverage = desc.stockShadowCoverage;
        for(size_t i=0;i<inputs.size();++i)
        {
            const auto& shader=inputs[i];
            if(!shader.binary && !shader.binarySize && !shader.entryPoint)continue; // 세 입력을 모두 생략한 슬롯은 기본 셰이더를 사용한다.
            if(shader.stage!=stages[i])return RendererFailure("Shader override has the wrong stage."); // 해당 슬롯의 정점/fragment 단계와 다르면 요청을 거부한다.
            if(!shader.binary || !shader.binarySize || !shader.entryPoint || !*shader.entryPoint) // 부분적으로만 제공한 바이트/진입점 또는 빈 진입점은 거부한다.
                return RendererFailure("A shader override requires bytes and an entry point.");
            const auto* begin=static_cast<const uint8_t*>(shader.binary);
            next.bytes[i].assign(begin,begin+shader.binarySize);
            next.entries[i]=shader.entryPoint;
        }
        if(next == shaderSources) // 현재 적용 셰이더와 같으면 대기 교체를 취소하고 기존 GPU 파이프라인을 유지한다.
        { pendingShaderSources = {}; shadersPending = false; shaderView=nullptr; return true; }
        pendingShaderSources=std::move(next);shadersPending=true;shaderView=nullptr;return true;
    }
    catch(const std::exception& error){std::fprintf(stderr,"dyf: Shader override: %s\n",error.what());return false;}
}

// 해당 슬롯에 사용자 바이트가 있으면 소유한 저장소를 가리키는 RHI 설명을 만들고, 없으면 stock 설명을 그대로 사용한다.
RHI::ShaderDesc Renderer::Impl::ShaderDescription(ShaderSlot slot,const RHI::ShaderDesc& stock) const
{
    if(shaderSources.bytes[slot].empty())return stock;
    return {stock.stage,shaderSources.entries[slot].c_str(),shaderSources.bytes[slot].data(),shaderSources.bytes[slot].size()};
}
// BeginFrame 뒤 조기 실패해도 열린 창 프레임을 닫으려는 범위 정리다. Present 예외는 소멸자 밖으로 던지지 않고 다음 ApplySettings 복구 경계에 맡긴다.
Renderer::Impl::FrameEnd::~FrameEnd()
{
    if(!pending) return; // 열린 창 프레임이 없으면 복구 Present가 필요 없다.
    try { (void)device.Present(); pending=false; }
    catch(...) { /* ApplySettings retries recovery before changing output. */ }
}
// 먼저 이전 창 프레임을 복구한다. 파이프라인에 영향이 없는 설정은 config만 갱신하고, 영향이 있으면 임시 Impl을 준비한 뒤 성공 시 자원을 교환한다.
bool Renderer::Impl::ApplySettings()
{
    if(framePending) // 이전 호출이 프레임을 닫지 못했으면 설정 변경 전에 Present 복구를 시도한다.
    {
        const bool recovered=device->Present();
        framePending=false;
        if(!recovered) return RendererFailure("Canvas frame recovery failed."); // Present가 false이면 framePending은 지운 상태로 실패를 반환하며 새 설정 적용을 중단한다. pendingConfig와 셰이더 요청은 지우지 않는다.
    }
    if(!pendingConfig && !shadersPending) return true; // 적용할 설정/셰이더 요청이 없으면 그대로 성공한다.
    const auto& nextConfig = pendingConfig ? *pendingConfig : config;
    const bool shadows = config.lighting.enabled && config.lighting.direct && config.lighting.shadows;
    const bool nextShadows = nextConfig.lighting.enabled && nextConfig.lighting.direct && nextConfig.lighting.shadows;
    // 셰이더·그림자 사용·HDR·고급 경로·인스턴싱·와이어프레임·bindless 변경은 파이프라인 구성이 달라질 수 있으므로 교체 경로를 선택한다.
    const bool rebuild = shadersPending || shadows != nextShadows ||
        config.enableHdrRendering != nextConfig.enableHdrRendering ||
        config.advanced.enabled != nextConfig.advanced.enabled ||
        config.enableInstancing != nextConfig.enableInstancing ||
        config.debugWireframe != nextConfig.debugWireframe ||
        config.enableBindlessMaterials != nextConfig.enableBindlessMaterials;
    // VSync/readback 변경은 창 출력 생성 조건이 달라진다. 요청된 스왑체인을 만들지 못하면 새 설정을 확정하지 않는다.
    const bool outputChanged = config.vsync != nextConfig.vsync ||
        config.allowReadback != nextConfig.allowReadback;
    RHI::SwapchainDesc output;
    output.format = RHI::DefaultSwapchainFormat;
    output.presentMode = nextConfig.vsync ? RHI::PresentMode::Fifo : RHI::PresentMode::Immediate;
    output.allowReadback = nextConfig.allowReadback;
    output.window = windowHandle;
    if(!rebuild) // 파이프라인 재생성이 필요 없으면 출력 설정과 config만 갱신하는 경로로 간다.
    {
        if(outputChanged && !device->CreateSwapchain(output)) // 출력 조건이 바뀌었는데 스왑체인 생성이 실패하면 적용하지 않는다.
            return RendererFailure("The requested output settings are unavailable.");
        // 색·노출·조명 상수·표시는 다음 프레임 입력만 바꾼다. 메시와 텍스처 캐시는 유지한다.
        // 그림자 품질에 따른 깊이 텍스처 크기 변경은 EnsureShadowDepthTarget에서 처리한다.
        InvalidateFrameHistory();
        config=nextConfig;
        pendingConfig.reset();
        shaderViews.clear();shaderView=nullptr;
        return true;
    }
    // 같은 장치를 빌리는 임시 Impl에 새 설정/셰이더를 준비한다. 교환 전 실패하면 임시 객체가 새 자원을 정리하고 기존 Impl은 유지된다.
    Impl next(owner,*device, windowHandle, nextConfig);
    next.shaderSources=shadersPending ? pendingShaderSources : shaderSources;
    if(pipeline && !next.InitializeMesh()) // 기존 메시 파이프라인이 사용 중이면 교체 파이프라인도 미리 성공해야 한다.
        return RendererFailure("Renderer settings could not create the requested pipeline.");
    if(outputChanged && !device->CreateSwapchain(output)) // 교체 자원이 준비되어도 출력 조건 적용이 실패하면 기존 자원과 교환하지 않는다.
            return RendererFailure("The requested output settings are unavailable.");
    InvalidateFrameHistory();
    SwapResources(next);
    pendingConfig.reset();
    pendingShaderSources={};shadersPending=false;
    shaderViews.clear();shaderView=nullptr;
    return true;
}
// 설정에 종속된 파이프라인·상수·자원 세트·Canvas/HDR 상태를 교환한다. 교환된 과거 자원은 임시 Impl의 소멸자가 해제한다.
void Renderer::Impl::SwapResources(Impl& other)
{
    using std::swap;
    // 설정·셰이더 교체에도 같은 장치와 고정 정점·이미지 형식을 쓰므로 장치 소유권·source ID·제출 펜스·업로드 캐시와 확정 상태는 교환하지 않는다.
    // 파이프라인 종속 바인딩만 교체해 같은 Scene 전체를 다시 업로드하는 일을 막는다.
    swap(config,other.config);
    swap(shaderSources,other.shaderSources);
    swap(materialStates,other.materialStates);
    swap(vertexShader,other.vertexShader);
    swap(fragmentShader,other.fragmentShader);
    swap(depthPrepassFragmentShader,other.depthPrepassFragmentShader);
    swap(noAreaFragmentShader,other.noAreaFragmentShader);
    swap(noAreaDepthPrepassFragmentShader,other.noAreaDepthPrepassFragmentShader);
    swap(shadowVertexShader,other.shadowVertexShader);
    swap(shadowFragmentShader,other.shadowFragmentShader);
    swap(canvasVertexShader,other.canvasVertexShader);
    swap(canvasFragmentShader,other.canvasFragmentShader);
    swap(toneVertexShader,other.toneVertexShader);
    swap(toneFragmentShader,other.toneFragmentShader);
    swap(pipeline,other.pipeline);
    swap(materialPipelines,other.materialPipelines);
    swap(depthPrepassPipelines,other.depthPrepassPipelines);
    swap(noAreaMaterialPipelines,other.noAreaMaterialPipelines);
    swap(noAreaDepthPrepassPipelines,other.noAreaDepthPrepassPipelines);
    swap(wireframePipelines,other.wireframePipelines);
    swap(mainEntityOrder,other.mainEntityOrder);
    swap(shadowPipeline,other.shadowPipeline);
    swap(shadowMaterialPipelines,other.shadowMaterialPipelines);
    swap(compactShadowPipelines,other.compactShadowPipelines);
    swap(canvasPipeline,other.canvasPipeline);
    swap(canvasWhite,other.canvasWhite);
    swap(canvasImages,other.canvasImages);
    swap(canvasVertices,other.canvasVertices);
    swap(canvasPendingVertices,other.canvasPendingVertices);
    swap(preparedCanvases,other.preparedCanvases);
    swap(canvasVertexCursor,other.canvasVertexCursor);
    swap(tonePipeline,other.tonePipeline);
    swap(toneSet,other.toneSet);
    swap(meshColorFormat,other.meshColorFormat);
    swap(toneColorFormat,other.toneColorFormat);
    swap(meshCompositeAlpha,other.meshCompositeAlpha);
    swap(depthStencilTarget,other.depthStencilTarget);
    swap(shadowDepthTarget,other.shadowDepthTarget);
    swap(shadowAtlasCache,other.shadowAtlasCache);
    swap(hdrTarget,other.hdrTarget);
    swap(defaultMaterialTextures,other.defaultMaterialTextures);
    swap(depthStencilState,other.depthStencilState);
    swap(shadowDepthState,other.shadowDepthState);
    swap(hdrState,other.hdrState);
    swap(lightingBuffer,other.lightingBuffer);
    swap(shadowMatrixBuffer,other.shadowMatrixBuffer);
    swap(instanceBuffer,other.instanceBuffer);swap(materialIndexBuffer,other.materialIndexBuffer);
    swap(lightingReady,other.lightingReady);swap(shadowReady,other.shadowReady);
    swap(instanceReady,other.instanceReady);swap(materialIndexReady,other.materialIndexReady);
    swap(mainSets,other.mainSets);swap(shadowSets,other.shadowSets);
    swap(instanceScene,other.instanceScene);
    swap(pendingInstanceScene,other.pendingInstanceScene);
    swap(instanceRevision,other.instanceRevision);
    swap(pendingInstanceRevision,other.pendingInstanceRevision);
    swap(instanceCount,other.instanceCount);
    swap(pendingInstanceCount,other.pendingInstanceCount);
    swap(instanceScratch,other.instanceScratch);
    swap(instanceRanges,other.instanceRanges);
    swap(lightingBytes,other.lightingBytes);
    swap(pendingLightingBytes,other.pendingLightingBytes);
    swap(shadowBytes,other.shadowBytes);
    swap(pendingShadowBytes,other.pendingShadowBytes);
    swap(materialIndexBytes,other.materialIndexBytes);
    swap(materialIndexScratch,other.materialIndexScratch);
    swap(lightingIndexScratch,other.lightingIndexScratch);
    swap(materialPages,other.materialPages);
    swap(gpuVisibility,other.gpuVisibility);swap(visibilityDraws,other.visibilityDraws);
    swap(visibilityKeys,other.visibilityKeys);swap(visibilityScene,other.visibilityScene);
    swap(visibilityActive,other.visibilityActive);
    swap(cpuDrawInputs,other.cpuDrawInputs);swap(cpuDraws,other.cpuDraws);
    swap(cpuBatchIndices,other.cpuBatchIndices);swap(cpuInstanceEntities,other.cpuInstanceEntities);
    swap(cpuMeshCounts,other.cpuMeshCounts);
    swap(cpuInstanceScratch,other.cpuInstanceScratch);swap(cpuInstanceBytes,other.cpuInstanceBytes);
    swap(cpuInstanceBuffer,other.cpuInstanceBuffer);swap(cpuInstanceReady,other.cpuInstanceReady);
    swap(cpuBatchesActive,other.cpuBatchesActive);swap(cpuCulledEntities,other.cpuCulledEntities);
    swap(mainSetScratch,other.mainSetScratch);
    swap(shadowSetScratch,other.shadowSetScratch);
    swap(mainBindingScratch,other.mainBindingScratch);
    swap(mainCombinedBindingScratch,other.mainCombinedBindingScratch);
    swap(shadowBindingScratch,other.shadowBindingScratch);
    swap(shadowCasterScratch,other.shadowCasterScratch);
    swap(shadowViewScratch,other.shadowViewScratch);

}

// owner와 장치는 빌리는 참조/포인터로 연결하고 프로파일러도 같은 장치에 묶는다. Create가 장치를 직접 만든 경우에만 뒤에서 ownedDevice를 채운다.
Renderer::Impl::Impl(Renderer& owner,RHI::IDevice& borrowedDevice,const void* window,const RendererConfig& desc)
    : owner(owner),device(&borrowedDevice),windowHandle(window),config(desc),m_profiler(borrowedDevice)
{
}
// 먼저 장치에 연결한 제어와 등록 핸들을 해제하고 live 객체를 은퇴시킨 뒤 Shutdown으로 GPU 자원을 정리한다.
Renderer::Impl::~Impl()
{
    if(liveId)device->ReleaseLiveControls(liveId); // 등록된 라이브 source가 있을 때만 장치의 제어 연결을 해제한다.
    for(auto& control:liveCatalog)control=Platform::ControlRegistration{};
    for(auto& control:liveControls)control=Platform::ControlRegistration{};
    Platform::RetireLiveObject(liveId);
    Shutdown();
}

// 설정을 생략한 생성 오버로드는 기본 RendererConfig로 전달한다.
std::unique_ptr<Renderer> Renderer::Create(const void* window)
{
    return Create(window, RendererConfig{});
}
// 장치를 직접 생성하는 경로다. 대여 장치용 Create가 성공한 뒤 소유권을 Renderer에 넘기므로 중간 실패 시 임시 unique_ptr가 장치를 정리한다.
std::unique_ptr<Renderer> Renderer::Create(const void* window, const RendererConfig& desc)
{
    try
    {
    if(!window){RendererFailure("Renderer requires a window.");return nullptr;} // 창 핸들이 없으면 Renderer를 만들 수 없다.
    auto owned = std::unique_ptr<RHI::IDevice>(RHI::IDevice::Create({2}));
    if(!owned){RendererFailure("RHI device creation failed.");return nullptr;} // 장치 생성 실패면 Renderer 생성도 중단한다.
    auto renderer = Create(*owned, window, desc);
    if(!renderer)return nullptr; // Renderer 초기화가 실패했으면 장치 소유권을 넘기지 않는다.
    renderer->m_impl->ownedDevice = std::move(owned);
    return renderer;

    }
    catch(const std::exception& error){std::fprintf(stderr,"dyf: Renderer creation: %s\n",error.what());return nullptr;}
}
// 외부 장치를 빌리는 기본 설정 생성 경로다. 장치 삭제 책임은 호출자에게 남는다.
std::unique_ptr<Renderer> Renderer::Create(RHI::IDevice& device, const void* window)
{
    return Create(device, window, RendererConfig{});
}
// 창·설정·스왑체인 초기화가 모두 유효할 때만 Renderer를 반환한다. 예외도 stderr와 nullptr로 변환한다.
std::unique_ptr<Renderer> Renderer::Create(RHI::IDevice& device, const void* window, const RendererConfig& desc)
{
    try
    {
    if(!window){RendererFailure("Renderer requires a window.");return nullptr;} // 대여 장치 경로도 창 핸들을 필수로 검사한다.
    if(!ValidateRendererConfig(desc))return nullptr; // 유효하지 않은 초기 설정이면 객체를 만들지 않는다.
    auto renderer = std::unique_ptr<Renderer>(new Renderer(device, window, desc));
    if(!renderer->m_impl->Initialize()){RendererFailure("Renderer initialization failed.");return nullptr;} // 스왑체인 초기화 실패면 생성한 임시 Renderer를 정리한다.
    renderer->m_impl->InitializeLiveProfiling();
    return renderer;

    }
    catch(const std::exception& error){std::fprintf(stderr,"dyf: Renderer creation: %s\n",error.what());return nullptr;}
}
// Scene 오버로드들은 선택 카메라와 overlay 포인터만 정리하여 RenderScene 한 경로로 모은다. 카메라가 없으면 출력 비율로 기본 카메라를 만든다.
bool Renderer::Impl::Render(const Scene& scene, Image* readback)
{
    return RenderScene(scene,nullptr,nullptr,readback);
}
bool Renderer::Impl::Render(const Scene& scene, const Camera& camera, Image* readback)
{
    return RenderScene(scene,&camera,nullptr,readback);
}
bool Renderer::Impl::Render(const Scene& scene, const Canvas& overlay, Image* readback)
{
    return RenderScene(scene,nullptr,&overlay,readback);
}
bool Renderer::Impl::Render(const Scene& scene, const Camera& camera, const Canvas& overlay, Image* readback)
{
    return RenderScene(scene,&camera,&overlay,readback);
}
// Canvas 전용 호출도 device/source/frame을 라이브 프레임에 연결하고 같은 설정 적용 경계를 지난다. 실패는 false이며 예외 메시지를 남긴다.
bool Renderer::Impl::Render(const Canvas& canvas, Image* readback)
{
    InitializeLiveProfiling();PublishLiveGeometryAvailability(false);
    Platform::LiveFrameScope liveFrame(Platform::FindLiveObject(device),liveId);
    device->PrepareLiveControls(liveFrame.GetKey());
    try
    {
        if(!PrepareLiveFrame(liveFrame.GetKey()))return false; // 프레임 경계의 설정 적용이 실패하면 Canvas 기록을 시작하지 않는다.
        if(readback && !config.allowReadback) return RendererFailure("Readback was not enabled."); // 캡처 요청이 있지만 실제 적용 설정에서 허용하지 않았으면 실패한다.
        const bool rendered=RenderCanvas(canvas,readback);
        liveFrame.SetOutcome(rendered?Platform::LiveOutcome::Completed:Platform::LiveOutcome::Failed);
        return rendered;
    }
    catch(const std::exception& error)
    {
        std::fprintf(stderr,"dyf: Canvas render: %s\n",error.what());
        return false;
    }
}
// Mesh만 받은 편의 경로는 임시 Scene에 기본 방향광을 추가하며, Scene을 직접 받은 경로는 등록된 광원만 사용한다.
bool Renderer::Impl::Render(const MeshData& mesh,Image* readback)
{
    try
    {
        Scene scene;
        if(!scene.Add(mesh)) return RendererFailure("Mesh scene construction failed."); // 메시를 임시 Scene에 넣지 못하면 렌더링하지 않는다.
        (void)scene.Add(DirectionalLight{});
        return Render(scene,readback);
    }
    catch(const std::exception& error)
    {
        std::fprintf(stderr,"dyf: Mesh render: %s\n",error.what());
        return false;
    }
}
// 재질을 생략한 경우 기본 MaterialDesc를 사용한다.
bool Renderer::Impl::Render(const MeshData& mesh,const Camera& camera,Image* readback)
{
    return Render(mesh,MaterialDesc{},camera,readback);
}
// 명시한 메시·재질·카메라로 임시 Scene을 구성한다. Scene 생성 실패나 예외를 렌더 성공으로 처리하지 않는다.
bool Renderer::Impl::Render(const MeshData& mesh,const MaterialDesc& material,const Camera& camera,Image* readback)
{
    try
    {
        Scene scene;
        if(!scene.Add(mesh,material)) return RendererFailure("Mesh scene construction failed."); // 메시/재질 Scene 생성 실패를 호출자에게 전달한다.
        (void)scene.Add(DirectionalLight{});
        return Render(scene,camera,readback);
    }
    catch(const std::exception& error)
    {
        std::fprintf(stderr,"dyf: Mesh render: %s\n",error.what());
        return false;
    }
}

// 요청 중인 셰이더가 있으면 그 내용을 읽는다. 반환 포인터가 원본 pending 교체로 무효화되지 않도록 deque 안의 복사본 뷰를 사용한다.
RendererShaderDesc Renderer::Impl::GetShaders() const
{
    if(!shaderView) // 현재 읽기 뷰가 없을 때만 선택된 설정의 안정된 복사본을 찾거나 만든다.
    {
        const auto& selected=shadersPending ? pendingShaderSources : shaderSources;
        const auto found=std::find(shaderViews.begin(),shaderViews.end(),selected);
        if(found!=shaderViews.end()) shaderView=&*found; // 같은 내용의 기존 뷰가 있으면 재사용하고 없으면 deque에 추가한다.
        else
        {
            shaderViews.push_back(selected);
            shaderView=&shaderViews.back();
        }
    }
    // 뷰의 바이트/문자열을 가리키는 ShaderDesc를 반환한다. 바인딩 vector는 복사되지만 바이트·진입점 포인터는 다음 설정 적용 전까지만 보장한다.
    const auto& source = *shaderView;
    RendererShaderDesc desc;
    RHI::ShaderDesc* output[] = {&desc.meshVertex, &desc.meshFragment, &desc.shadowVertex,
        &desc.canvasVertex, &desc.canvasFragment, &desc.toneMapVertex, &desc.toneMapFragment};
    const RHI::ShaderStage stages[] = {RHI::ShaderStage::Vertex, RHI::ShaderStage::Fragment,
        RHI::ShaderStage::Vertex, RHI::ShaderStage::Vertex, RHI::ShaderStage::Fragment,
        RHI::ShaderStage::Vertex, RHI::ShaderStage::Fragment};
    for(size_t i = 0; i < ShaderCount; ++i)
        if(!source.bytes[i].empty()) *output[i] = {stages[i], source.entries[i].c_str(), source.bytes[i].data(), source.bytes[i].size()}; // 사용자 교체 슬롯만 포인터 설명을 채우며 나머지는 빈 기본값을 남긴다.
    desc.vertexBindings = source.vertexBindings;
    desc.additionalConstantBytes = source.additionalConstantBytes;
    desc.stockShadowCoverage = source.stockShadowCoverage;
    return desc;
}

// 객체별 RHI 입력이 있는 Scene 호출도 RenderScene에 모아 같은 검증·프레임 처리를 받는다.
bool Renderer::Impl::Render(const Scene& scene, const Camera& camera, const std::vector<RendererDrawDesc>& draws,
    const Canvas* overlay, Image* readback)
{ return RenderScene(scene, &camera, overlay, readback, &draws); }
bool Renderer::Impl::Render(const Scene& scene, const std::vector<RendererDrawDesc>& draws,
    const Canvas* overlay, Image* readback)
{ return RenderScene(scene, nullptr, overlay, readback, &draws); }

// 창의 스왑체인과 기본 back buffer 형식만 준비한다. 메시 파이프라인과 장면 자원은 실제 렌더 경로에서 필요할 때 만든다.
bool Renderer::Impl::Initialize()
{
	RHI::IDevice* nativeDevice = device;
	if(nativeDevice == nullptr) return false; // 장치가 없으면 초기화할 수 없다.

	RHI::SwapchainDesc swapchainDesc = {};
	swapchainDesc.window=windowHandle;
    swapchainDesc.format = RHI::DefaultSwapchainFormat;
	swapchainDesc.minimumImageCount = 2;
	swapchainDesc.allowReadback = config.allowReadback;
	swapchainDesc.presentMode = config.vsync ? RHI::PresentMode::Fifo : RHI::PresentMode::Immediate;
	if(!nativeDevice->CreateSwapchain(swapchainDesc)) return false; // 요청한 창 출력 생성이 실패하면 초기화를 중단한다.

	RHI::TextureHandle backBuffer = nativeDevice->GetBackBuffer();
	if(backBuffer == nullptr || backBuffer->GetDesc().format != swapchainDesc.format) return false; // back buffer가 없거나 요청 형식과 다르면 생성 성공으로 처리하지 않는다.

    return true;
}

// 메시 정점/fragment/그림자 정점 교체와 추가 바인딩·상수가 없는지를 확인한다. 표준 지오메트리 전제의 인스턴싱·LOD·고급 패스 판단에 쓰인다.
bool Renderer::Impl::UsesStockGeometry() const
{
    return shaderSources.bytes[MeshVertex].empty() && shaderSources.bytes[MeshFragment].empty() &&
        shaderSources.bytes[ShadowVertex].empty() && shaderSources.vertexBindings.empty() &&
        shaderSources.additionalConstantBytes==0;
}
// 설정상 인스턴싱 요청과 표준 셰이더 계약을 함께 만족해야 128바이트 인스턴스 스트림을 사용한다.
bool Renderer::Impl::UsesInstanceStream() const { return config.enableInstancing && UsesStockGeometry(); }
// 행렬 변환이 넓이·부피를 얼마나 바꾸고 방향을 뒤집는지 나타내는 값(행렬식)이 음수이면 면 방향이 반전된다.
// 호출부가 이 반전 여부를 mirrored로 전달하며, 표준 재질은 블렌드·양면·mirrored 조합으로 파이프라인을 고른다. 사용자 fragment는 공통 파이프라인을 쓴다.
RHI::PipelineHandle Renderer::Impl::MaterialPipeline(const MaterialDesc& material,bool mirrored) const
{
    if(!shaderSources.bytes[MeshFragment].empty()) return pipeline; // 사용자 fragment는 기본 공통 파이프라인 경로를 사용한다.
    if(wireframeActive && !UsesBlendedRendering(material)) // 와이어프레임 진단 중 불투명/마스크 재질이면 전용 선 파이프라인을 고른다.
        return wireframePipelines[(material.doubleSided ? 1u : 0u) | (mirrored ? 2u : 0u)];
    const uint32_t variant=(UsesBlendedRendering(material) ? 2u : 0u) |
        (material.doubleSided ? 1u : 0u) | (mirrored ? 4u : 0u);
    if(frameNoAreaLights && stockDrawInputs && UsesStockGeometry() && !wireframeActive)
    {
        const auto selected=frameOpaqueDepthComplete && !UsesBlendedRendering(material)
            ? noAreaDepthPrepassPipelines[variant] : noAreaMaterialPipelines[variant];
        if(selected)return selected;
    }
    if(frameOpaqueDepthComplete && !UsesBlendedRendering(material))
        return depthPrepassPipelines[variant];
    return variant ? materialPipelines[variant-1] : pipeline;
}
// 요청값과 실제 레이아웃 지원 여부를 함께 확인한다. 토글 true만으로 bindless 경로 사용을 단정하지 않는다.
bool Renderer::Impl::UsesBindlessMaterials() const
{return config.enableBindlessMaterials&&SupportsBindlessMaterials();}
// 사용자 fragment는 기본 레이아웃을 유지하고, 표준 fragment의 실제 MeshLayout 지원 여부를 RHI에 질의한다.
bool Renderer::Impl::SupportsBindlessMaterials() const
{
    // 사용자 Fragment는 기본 레이아웃 계약을 유지한다. 기본 셰이더의 최적화 가능 여부는 RHI에 묻는다.
    if(!shaderSources.bytes[MeshFragment].empty()) return false;
    std::vector<RHI::ResourceBindingLayout> bindings;
    return device->Supports(MeshLayout(true, bindings));
}

// 출력 형식/알파 합성 조건이 같으면 기존 파이프라인을 재사용한다. 달라지면 필요한 셰이더를 확보하고 변형 파이프라인과 기본 텍스처를 준비한다.
bool Renderer::Impl::InitializeMesh(RHI::TextureHandle output,bool compositeAlpha)
{
    if(!output) output=device->GetBackBuffer(); // 명시한 출력이 없으면 현재 back buffer를 대상으로 삼는다.
    if(!output) return false; // 출력을 확보하지 못했으면 메시 자원을 준비하지 않는다.
    const auto colorFormat=config.enableHdrRendering ? RHI::Format::R16G16B16A16_FLOAT : output->GetDesc().format;
    const bool shadows = config.lighting.enabled && config.lighting.direct && config.lighting.shadows;
    if(pipeline != nullptr && meshColorFormat==colorFormat && meshCompositeAlpha==compositeAlpha) // 기존 형식/알파 조건이 같으면 파이프라인을 유지하고 기본 텍스처만 보충한다.
        return defaultMaterialTextures[0] || CreateDefaultMaterialTextures(device);
    RHI::IDevice* nativeDevice = device;
    const auto stockShaders = DefaultShaders(shadows,UsesBindlessMaterials(),UsesInstanceStream());

	if(!vertexShader) vertexShader = nativeDevice->CreateShader(ShaderDescription(MeshVertex,stockShaders.meshVertex)); // 정점 셰이더가 아직 없을 때만 기본/교체 설명으로 생성한다.
	if(!fragmentShader) fragmentShader = nativeDevice->CreateShader(ShaderDescription(MeshFragment,stockShaders.meshFragment)); // fragment 셰이더가 아직 없을 때만 생성한다.
    const bool depthPrepass=UsesStockGeometry() && UsesAdvancedRendering();
    if(depthPrepass && !depthPrepassFragmentShader)
        depthPrepassFragmentShader=nativeDevice->CreateShader(DefaultDepthPrepassFragment(shadows,UsesBindlessMaterials()));
    if(UsesStockGeometry())
    {
        if(!noAreaFragmentShader)
        {
            const auto desc=DefaultNoAreaFragment(shadows,UsesBindlessMaterials(),false);
            if(desc.binary && desc.binarySize)noAreaFragmentShader=nativeDevice->CreateShader(desc);
        }
        if(depthPrepass && !noAreaDepthPrepassFragmentShader)
        {
            const auto desc=DefaultNoAreaFragment(shadows,UsesBindlessMaterials(),true);
            if(desc.binary && desc.binarySize)noAreaDepthPrepassFragmentShader=nativeDevice->CreateShader(desc);
        }
    }
	if(shadows && !shadowVertexShader) // 그림자를 사용하면서 그림자 정점 셰이더가 없을 때 생성한다.
	{
		shadowVertexShader = nativeDevice->CreateShader(ShaderDescription(ShadowVertex,stockShaders.shadowVertex));
	}
	const bool stockCoverage=shaderSources.bytes[ShadowVertex].empty() || shaderSources.stockShadowCoverage;
	if(shadows && stockCoverage && !shadowFragmentShader)
		shadowFragmentShader = nativeDevice->CreateShader(DefaultShadowFragment());
	if(vertexShader == nullptr || fragmentShader == nullptr || (depthPrepass && !depthPrepassFragmentShader) ||
		(shadows && shadowVertexShader == nullptr) ||
		(shadows && stockCoverage && !shadowFragmentShader)) return false; // 필수 셰이더 중 하나라도 생성되지 않았으면 파이프라인 준비를 중단한다.
    // 이 경로는 이전 형식의 파이프라인을 해제한 뒤 새로 만든다. 아래 생성이 실패하면 false를 반환하며 이미 성공한 자원은 Impl의 정리/다음 준비 경로가 맡는다.
    nativeDevice->DestroyPipeline(pipeline);pipeline=nullptr;
    for(auto& value:materialPipelines) {nativeDevice->DestroyPipeline(value);value=nullptr;}
    for(auto& value:depthPrepassPipelines) {nativeDevice->DestroyPipeline(value);value=nullptr;}
    for(auto& value:noAreaMaterialPipelines) {nativeDevice->DestroyPipeline(value);value=nullptr;}
    for(auto& value:noAreaDepthPrepassPipelines) {nativeDevice->DestroyPipeline(value);value=nullptr;}
    for(auto& value:wireframePipelines) {nativeDevice->DestroyPipeline(value);value=nullptr;}
    nativeDevice->DestroyPipeline(shadowPipeline);shadowPipeline=nullptr;
    for(auto& value:shadowMaterialPipelines) {nativeDevice->DestroyPipeline(value);value=nullptr;}
    for(auto& value:compactShadowPipelines) {nativeDevice->DestroyPipeline(value);value=nullptr;}
	if(!BuildPipelineStates(nativeDevice,colorFormat,compositeAlpha)) return false; // 재질·그림자 파이프라인 묶음 생성 실패를 그대로 전파한다.
    meshColorFormat=colorFormat;
    meshCompositeAlpha=compositeAlpha;
    if(!defaultMaterialTextures[0] && !CreateDefaultMaterialTextures(nativeDevice)) return false; // 기본 텍스처가 없을 때만 생성하고 그 준비 실패도 렌더 실패로 처리한다.
	return true;
}

// 참조 자원 세트를 먼저 정리하고 확장/캐시/쿼리/텍스처/버퍼/파이프라인/셰이더를 장치로 반환한다. 여기의 해제 호출을 GPU 완료 측정으로 해석하지 않는다.
void Renderer::Impl::Shutdown()
{
    if(!device)return; // 장치 없이 자원 해제 함수를 호출하지 않는다.
    outputPresentation.reset();
    shadowAtlasCache.valid=false;shadowAtlasCache.pending=false;
    // Impl이 직접 보관한 RHI 핸들은 아래 Destroy 호출로 해제한다. 보조 객체가 소유한 자원은 reset·clear와 각 해제 함수가 정리하며, 장치는 이 정리가 끝날 때까지 살아 있어야 한다.
    device->DestroyResourceSet(toneSet);
    for(auto& image:canvasImages) {device->DestroyResourceSet(image.set);device->DestroyTexture(image.texture);}
    for(auto& vertices:canvasVertices)device->DestroyBuffer(vertices.buffer);
    for(auto& cached:mainSets)device->DestroyResourceSet(cached.set);
    for(auto& cached:shadowSets)device->DestroyResourceSet(cached.set);
    InvalidateFrameHistory(true);
    extensions.clear();
    ReleaseGeometry(device);ReleaseTextures(device);
    m_profiler.ResetGpuQueries();
    // 이하 반복문은 생성된 비어 있지 않은 핸들만 타입에 맞는 Destroy 함수로 정리한다. Scene 원본 소유권을 삭제하는 작업은 아니다.
    for(auto* texture:{depthStencilTarget,shadowDepthTarget,hdrTarget})if(texture)device->DestroyTexture(texture); // 메인 깊이·그림자 깊이·HDR 중간 출력 중 실제 생성된 텍스처만 해제한다.
    for(auto* texture:defaultMaterialTextures)if(texture)device->DestroyTexture(texture); // 생성된 기본 대체 텍스처만 해제한다.
    for(auto* buffer:{lightingBuffer,shadowMatrixBuffer,instanceBuffer,materialIndexBuffer,cpuInstanceBuffer})if(buffer)device->DestroyBuffer(buffer); // 존재하는 프레임 상수/인스턴스/재질 인덱스 버퍼만 해제한다.
    for(auto* value:{pipeline,shadowPipeline,canvasPipeline,tonePipeline})if(value)device->DestroyPipeline(value); // 생성된 메인·그림자·Canvas·톤 매핑 파이프라인만 해제한다.
    for(auto* value:materialPipelines)if(value)device->DestroyPipeline(value); // 생성된 재질 변형 파이프라인만 해제한다.
    for(auto* value:depthPrepassPipelines)if(value)device->DestroyPipeline(value);
    for(auto* value:noAreaMaterialPipelines)if(value)device->DestroyPipeline(value);
    for(auto* value:noAreaDepthPrepassPipelines)if(value)device->DestroyPipeline(value);
    for(auto* value:wireframePipelines)if(value)device->DestroyPipeline(value); // 생성된 와이어프레임 변형만 해제한다.
    for(auto* value:shadowMaterialPipelines)if(value)device->DestroyPipeline(value); // 생성된 그림자 재질 변형만 해제한다.
    for(auto* value:compactShadowPipelines)if(value)device->DestroyPipeline(value);
    for(auto* shader:{vertexShader,fragmentShader,depthPrepassFragmentShader,noAreaFragmentShader,noAreaDepthPrepassFragmentShader,shadowVertexShader,shadowFragmentShader,canvasVertexShader,canvasFragmentShader,toneVertexShader,toneFragmentShader})
        if(shader)device->DestroyShader(shader); // 실제로 존재하는 셰이더 핸들만 장치에 반환한다.
}



// readback의 형식·크기·행 간격·바이트 범위를 검증해 행 패딩과 BGRA 순서를 연속 RGBA Image로 변환하며, 실패하면 기존 image를 보존한다.
bool Renderer::Impl::CaptureFrame(Image& image)
{
    RHI::TextureReadback readback;
    if (!device->ReadTexture(device->GetBackBuffer(), readback)) // back buffer 읽기 실패면 오류를 남기고 기존 image를 보존한다.
        {std::fprintf(stderr,"dyf: Frame readback failed; enable RendererConfig::allowReadback on a native backend.\n");return false;}
    const uint64_t rowBytes = static_cast<uint64_t>(readback.width) * 4u;
    const uint64_t size = rowBytes * readback.height;
    if (!RHI::IsReadbackFormat(readback.format) || !readback.width || !readback.height || // 지원하는 RGBA/BGRA8 형식과 0이 아닌 폭/높이인지 검사한다. rowPitch는 한 행의 width*4바이트 이상이어야 한다.
    // 연속 출력 크기가 size_t 범위에 들어가고 마지막 행의 실제 픽셀 끝이 readback.pixels 안에 있어야 복사한다.
        readback.rowPitch < rowBytes || size > std::numeric_limits<size_t>::max() ||
        static_cast<uint64_t>(readback.height - 1) * readback.rowPitch + rowBytes > readback.pixels.size())
        {std::fprintf(stderr,"dyf: RHI returned an invalid readback image.\n");return false;}
    // GPU readback의 행 패딩을 제거한다. 마지막에 Image를 한 번 교체하므로 중간 실패가 기존 결과를 부분적으로 덮지 않는다.
    std::vector<uint8_t> pixels(static_cast<size_t>(size));
    for (uint32_t row = 0; row < readback.height; ++row)
        std::memcpy(pixels.data() + static_cast<size_t>(row) * rowBytes,
            readback.pixels.data() + static_cast<size_t>(row) * readback.rowPitch, static_cast<size_t>(rowBytes));
    if (readback.format == RHI::Format::B8G8R8A8_UNORM || readback.format == RHI::Format::B8G8R8A8_UNORM_SRGB) // BGRA 포맷이면 각 픽셀의 B/R을 바꿔 Image의 RGBA 순서로 맞춘다.
        for (size_t i = 0; i < pixels.size(); i += 4) std::swap(pixels[i], pixels[i + 2]);
    // Renderer의 UNORM 출력도 표시용 색으로 인코딩하므로 캡처 결과는 sRGB 이미지다.
    image = Image(readback.width, readback.height, std::move(pixels), ColorSpace::Srgb);
    return true;
}
