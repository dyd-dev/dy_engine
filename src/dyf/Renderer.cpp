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
    bool ValidateRendererConfig(const RendererConfig& desc)
    {
        if(!ValidateOutputConfig(desc.postProcess))
            return RendererFailure("Invalid color grading or sharpening configuration.");
        for(const auto& color : {Math::float3{desc.clearColor.x,desc.clearColor.y,desc.clearColor.z},
            desc.lighting.ambientColor,desc.lighting.environment.diffuseColor,desc.lighting.environment.specularColor})
            if(!std::isfinite(color.x) || !std::isfinite(color.y) || !std::isfinite(color.z))
                return RendererFailure("Invalid Renderer configuration.");
        if(!std::isfinite(desc.clearColor.w) ||
            !std::isfinite(desc.exposure) || desc.exposure<0 ||
            !std::isfinite(desc.lighting.ambientIntensity) || desc.lighting.ambientIntensity<0 ||
            !std::isfinite(desc.lighting.environment.diffuseIntensity) || desc.lighting.environment.diffuseIntensity<0 ||
            !std::isfinite(desc.lighting.environment.specularIntensity) || desc.lighting.environment.specularIntensity<0)
            return RendererFailure("Invalid Renderer configuration.");
        if(!std::isfinite(desc.hdrRenderScale) || desc.hdrRenderScale<1 || desc.hdrRenderScale>2)
            return RendererFailure("HDR render scale must be finite and between 1 and 2.");
        if((desc.renderWidth==0)!=(desc.renderHeight==0))
            return RendererFailure("Fixed rendering requires both width and height, or zero for both.");
        if(desc.texturePreparationThreadCount==0)
            return RendererFailure("Texture preparation requires at least one thread.");
        if(!std::isfinite(desc.lodPixelError) || desc.lodPixelError<=0 ||
            !std::isfinite(desc.lodHysteresis) || desc.lodHysteresis<0 || desc.lodHysteresis>=0.5f)
            return RendererFailure("LOD requires positive pixel error and hysteresis in [0, 0.5).");
        if(desc.lighting.shadowQuality > ShadowQuality::High)
            return RendererFailure("Invalid shadow configuration.");
        if(desc.lighting.shadowBlockerSamples<1 || desc.lighting.shadowBlockerSamples>32 ||
            desc.lighting.shadowFilterSamples<1 || desc.lighting.shadowFilterSamples>32 ||
            !std::isfinite(desc.lighting.shadowMaxFilterRadius) || desc.lighting.shadowMaxFilterRadius<0)
            return RendererFailure("Soft shadows require 1..32 search/filter samples and a finite non-negative filter radius.");
        if(!std::isfinite(desc.lighting.shadowDistance) || desc.lighting.shadowDistance<0 ||
            !std::isfinite(desc.lighting.cascadeSplitLambda) || desc.lighting.cascadeSplitLambda<0 ||
            desc.lighting.cascadeSplitLambda>1)
            return RendererFailure("Shadow distance must be non-negative and cascade split lambda must be in [0, 1].");
        if(desc.lighting.maxDirectionalShadowLights>4 || desc.lighting.maxPointShadowLights>16 ||
            desc.lighting.maxSpotShadowLights>16)
            return RendererFailure("Stock shadow-light budgets support up to 4 directional, 16 point and 16 spot lights.");
        const auto& environment=desc.lighting.environment;
        if(!std::isfinite(environment.intensity) || environment.intensity<0 ||
            !std::isfinite(environment.rotationRadians)) return RendererFailure("Invalid HDR environment configuration.");
        for(const auto& probe:environment.probes)
        {
            if(!probe.radiance.IsValid() || !std::isfinite(probe.intensity) || probe.intensity<0 ||
                !std::isfinite(probe.fadeDistance) || probe.fadeDistance<=0)
                return RendererFailure("Environment probes require HDR radiance, non-negative intensity and positive fade distance.");
            const float positions[]={probe.position.x,probe.position.y,probe.position.z};
            const float lows[]={probe.boxMin.x,probe.boxMin.y,probe.boxMin.z};
            const float highs[]={probe.boxMax.x,probe.boxMax.y,probe.boxMax.z};
            for(uint32_t axis=0;axis<3;++axis)
            {
                const float position=positions[axis],low=lows[axis],high=highs[axis];
                if(!std::isfinite(position) || !std::isfinite(low) || !std::isfinite(high) ||
                    low>=high || position<=low || position>=high)
                    return RendererFailure("Environment probe position must be inside its finite, non-empty influence box.");
            }
        }
        const auto& advanced=desc.advanced;
        if(advanced.transparencyLayers<1 || advanced.transparencyLayers>2)
            return RendererFailure("Raster transmission supports one or two layers.");
        if(advanced.surfaceDebugView>SurfaceDebugView::Motion)
            return RendererFailure("Invalid surface debug view.");
        for(float value:{advanced.aoRadius,advanced.aoStrength,advanced.rayDistance,advanced.giStrength,
            advanced.reflectionStrength,advanced.rayThickness,advanced.bloomStrength,advanced.bloomThreshold,advanced.bloomKnee})
            if(!std::isfinite(value) || value<0) return RendererFailure("Invalid advanced rendering configuration.");
        if(advanced.aoRadius<=0 || advanced.rayDistance<=0 || advanced.rayThickness<=0 ||
            !std::isfinite(advanced.historyWeight) || advanced.historyWeight<0 || advanced.historyWeight>=1)
            return RendererFailure("Invalid advanced rendering radii or history weight.");
        if(!std::isfinite(advanced.exposureKey) || advanced.exposureKey<=0 ||
            !std::isfinite(advanced.exposureAdaptationSpeed) || advanced.exposureAdaptationSpeed<0)
            return RendererFailure("Auto exposure requires a positive key and non-negative adaptation speed.");
        const auto& fog=advanced.fog;
        for(float value:{fog.density,fog.heightFalloff,fog.albedo.x,fog.albedo.y,fog.albedo.z})
            if(!std::isfinite(value) || value<0) return RendererFailure("Invalid volumetric fog density or albedo.");
        if(!std::isfinite(fog.baseHeight) || !std::isfinite(fog.anisotropy) || std::abs(fog.anisotropy)>=.95f ||
            !std::isfinite(fog.maxDistance) || fog.maxDistance<=0 || fog.steps<1 || fog.steps>256 ||
            !std::isfinite(fog.resolutionScale) || fog.resolutionScale<=0 || fog.resolutionScale>1 ||
            fog.albedo.x>1 || fog.albedo.y>1 || fog.albedo.z>1)
            return RendererFailure("Invalid volumetric fog configuration.");
        return true;
    }
}
bool Renderer::Impl::SetConfig(const RendererConfig& desc)
{
    try
    {
        if(!ValidateRendererConfig(desc)) return false;
        if(desc.debugWireframe && !device->Supports(RHI::Feature::Wireframe))
            return RendererFailure("Wireframe diagnostic is unsupported by this device.");
        pendingConfig=std::make_unique<RendererConfig>(desc);return true;
    }
    catch(const std::exception& error){std::fprintf(stderr,"dyf: Renderer settings: %s\n",error.what());return false;}
}
bool Renderer::Impl::SetClearColor(Math::float4 value) {auto desc=GetConfig();desc.clearColor=value;return SetConfig(desc);}
bool Renderer::Impl::SetVSync(bool value) {auto desc=GetConfig();desc.vsync=value;return SetConfig(desc);}
bool Renderer::Impl::SetLighting(const LightingConfig& value) {auto desc=GetConfig();desc.lighting=value;return SetConfig(desc);}
bool Renderer::Impl::SetLightingEnabled(bool value) {auto desc=GetConfig();desc.lighting.enabled=value;return SetConfig(desc);}
bool Renderer::Impl::SetShadowsEnabled(bool value) {auto desc=GetConfig();desc.lighting.shadows=value;return SetConfig(desc);}
bool Renderer::Impl::SetHdrEnabled(bool value) {auto desc=GetConfig();desc.enableHdrRendering=value;return SetConfig(desc);}
bool Renderer::Impl::SetExposure(float value) {auto desc=GetConfig();desc.exposure=value;return SetConfig(desc);}
bool Renderer::Impl::SetProfilerVisible(bool value) {auto desc=GetConfig();desc.enableProfilerHud=value;return SetConfig(desc);}
bool Renderer::Impl::SetReadbackEnabled(bool value) {auto desc=GetConfig();desc.allowReadback=value;return SetConfig(desc);}
bool Renderer::Impl::AddExtension(std::unique_ptr<RenderExtension> extension)
{
    if(!extension || framePending) return RendererFailure("Render extensions must be registered between frames.");
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
bool Renderer::Impl::ShaderSources::operator==(const ShaderSources& other) const
{
    if(bytes!=other.bytes || entries!=other.entries || additionalConstantBytes!=other.additionalConstantBytes ||
        stockShadowCoverage!=other.stockShadowCoverage ||
        vertexBindings.size()!=other.vertexBindings.size()) return false;
    // Compare float representations so copied default NaNs also match.
    const auto sameFloat=[](float left,float right) {return std::memcmp(&left,&right,sizeof(float))==0;};
    for(size_t i=0;i<vertexBindings.size();++i)
    {
        const auto& left=vertexBindings[i];const auto& right=other.vertexBindings[i];
        if(left.binding!=right.binding || left.type!=right.type || left.count!=right.count || left.stages!=right.stages)
            return false;
        const auto& a=left.staticSampler;const auto& b=right.staticSampler;
        if(a.minFilter!=b.minFilter || a.magFilter!=b.magFilter || a.mipFilter!=b.mipFilter ||
            a.addressU!=b.addressU || a.addressV!=b.addressV || a.addressW!=b.addressW ||
            a.borderColor!=b.borderColor || a.maxAnisotropy!=b.maxAnisotropy ||
            !sameFloat(a.mipLodBias,b.mipLodBias) || !sameFloat(a.minLod,b.minLod) || !sameFloat(a.maxLod,b.maxLod))
            return false;
    }
    return true;
}
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
            if(!shader.binary && !shader.binarySize && !shader.entryPoint)continue;
            if(shader.stage!=stages[i])return RendererFailure("Shader override has the wrong stage.");
            if(!shader.binary || !shader.binarySize || !shader.entryPoint || !*shader.entryPoint)
                return RendererFailure("A shader override requires bytes and an entry point.");
            const auto* begin=static_cast<const uint8_t*>(shader.binary);
            next.bytes[i].assign(begin,begin+shader.binarySize);
            next.entries[i]=shader.entryPoint;
        }
        // 호출 범위에서 쓴 셰이더를 되돌렸다가 다시 선택한 경우 기존 GPU 파이프라인을 유지한다.
        if(next == shaderSources)
        { pendingShaderSources = {}; shadersPending = false; shaderView=nullptr; return true; }
        pendingShaderSources=std::move(next);shadersPending=true;shaderView=nullptr;return true;
    }
    catch(const std::exception& error){std::fprintf(stderr,"dyf: Shader override: %s\n",error.what());return false;}
}

RHI::ShaderDesc Renderer::Impl::ShaderDescription(ShaderSlot slot,const RHI::ShaderDesc& stock) const
{
    // 지정하지 않은 단계는 기본 셰이더를 사용하고, 지정한 단계만 교체한다.
    if(shaderSources.bytes[slot].empty())return stock;
    return {stock.stage,shaderSources.entries[slot].c_str(),shaderSources.bytes[slot].data(),shaderSources.bytes[slot].size()};
}
Renderer::Impl::FrameEnd::~FrameEnd()
{
    if(!pending) return;
    try { (void)device.Present(); pending=false; }
    catch(...) { /* ApplySettings retries recovery before changing output. */ }
}
bool Renderer::Impl::ApplySettings()
{
    if(framePending)
    {
        const bool recovered=device->Present();
        framePending=false;
        if(!recovered) return RendererFailure("Canvas frame recovery failed.");
    }
    if(!pendingConfig && !shadersPending) return true;
    const auto& nextConfig = pendingConfig ? *pendingConfig : config;
    const bool shadows = config.lighting.enabled && config.lighting.direct && config.lighting.shadows;
    const bool nextShadows = nextConfig.lighting.enabled && nextConfig.lighting.direct && nextConfig.lighting.shadows;
    const bool rebuild = shadersPending || shadows != nextShadows ||
        config.enableHdrRendering != nextConfig.enableHdrRendering ||
        config.advanced.enabled != nextConfig.advanced.enabled ||
        config.enableInstancing != nextConfig.enableInstancing ||
        config.debugWireframe != nextConfig.debugWireframe ||
        config.enableBindlessMaterials != nextConfig.enableBindlessMaterials;
    const bool outputChanged = config.vsync != nextConfig.vsync ||
        config.allowReadback != nextConfig.allowReadback;
    RHI::SwapchainDesc output;
    output.format = RHI::DefaultSwapchainFormat;
    output.presentMode = nextConfig.vsync ? RHI::PresentMode::Fifo : RHI::PresentMode::Immediate;
    output.allowReadback = nextConfig.allowReadback;
    output.window = windowHandle;
    if(!rebuild)
    {
        if(outputChanged && !device->CreateSwapchain(output))
            return RendererFailure("The requested output settings are unavailable.");
        // 색·노출·조명 상수·표시는 다음 프레임 입력만 바꾼다. 메시와 텍스처 캐시는 유지한다.
        // 그림자 품질에 따른 깊이 텍스처 크기 변경은 EnsureShadowDepthTarget에서 처리한다.
        InvalidateFrameHistory();
        config=nextConfig;
        pendingConfig.reset();
        shaderViews.clear();shaderView=nullptr;
        return true;
    }
    Impl next(owner,*device, windowHandle, nextConfig);
    next.shaderSources=shadersPending ? pendingShaderSources : shaderSources;
    if(pipeline && !next.InitializeMesh())
        return RendererFailure("Renderer settings could not create the requested pipeline.");
    if(outputChanged && !device->CreateSwapchain(output))
            return RendererFailure("The requested output settings are unavailable.");
    InvalidateFrameHistory();
    SwapResources(next);
    pendingConfig.reset();
    pendingShaderSources={};shadersPending=false;
    shaderViews.clear();shaderView=nullptr;
    return true;
}
void Renderer::Impl::SwapResources(Impl& other)
{
    using std::swap;
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
    swap(canvasDrawImages,other.canvasDrawImages);
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

    // Scene uploads use the same device and fixed vertex/image formats across
    // settings and shader changes. Keep their ownership and committed states;
    // only pipeline-dependent bindings above belong to the replacement renderer.
    // Swapping these caches would retire the whole scene and upload it again.
}

Renderer::Impl::Impl(Renderer& owner,RHI::IDevice& borrowedDevice,const void* window,const RendererConfig& desc)
    : owner(owner),device(&borrowedDevice),windowHandle(window),config(desc),m_profiler(borrowedDevice)
{
}
Renderer::Impl::~Impl()
{
    if(liveId)device->ReleaseLiveControls(liveId);
    for(auto& control:liveCatalog)control=Platform::ControlRegistration{};
    for(auto& control:liveControls)control=Platform::ControlRegistration{};
    Platform::RetireLiveObject(liveId);
    Shutdown();
}

std::unique_ptr<Renderer> Renderer::Create(const void* window)
{
    return Create(window, RendererConfig{});
}
std::unique_ptr<Renderer> Renderer::Create(const void* window, const RendererConfig& desc)
{
    try
    {
    if(!window){RendererFailure("Renderer requires a window.");return nullptr;}
    auto owned = std::unique_ptr<RHI::IDevice>(RHI::IDevice::Create({2}));
    if(!owned){RendererFailure("RHI device creation failed.");return nullptr;}
    auto renderer = Create(*owned, window, desc);
    if(!renderer)return nullptr;
    renderer->m_impl->ownedDevice = std::move(owned);
    return renderer;

    }
    catch(const std::exception& error){std::fprintf(stderr,"dyf: Renderer creation: %s\n",error.what());return nullptr;}
}
std::unique_ptr<Renderer> Renderer::Create(RHI::IDevice& device, const void* window)
{
    return Create(device, window, RendererConfig{});
}
std::unique_ptr<Renderer> Renderer::Create(RHI::IDevice& device, const void* window, const RendererConfig& desc)
{
    try
    {
    if(!window){RendererFailure("Renderer requires a window.");return nullptr;}
    if(!ValidateRendererConfig(desc))return nullptr;
    auto renderer = std::unique_ptr<Renderer>(new Renderer(device, window, desc));
    if(!renderer->m_impl->Initialize()){RendererFailure("Renderer initialization failed.");return nullptr;}
    renderer->m_impl->InitializeLiveProfiling();
    return renderer;

    }
    catch(const std::exception& error){std::fprintf(stderr,"dyf: Renderer creation: %s\n",error.what());return nullptr;}
}
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
bool Renderer::Impl::Render(const Canvas& canvas, Image* readback)
{
    InitializeLiveProfiling();PublishLiveGeometryAvailability(false);
    Platform::LiveFrameScope liveFrame(Platform::FindLiveObject(device),liveId);
    device->PrepareLiveControls(liveFrame.GetKey());
    try
    {
        if(!PrepareLiveFrame(liveFrame.GetKey()))return false;
        if(readback && !config.allowReadback) return RendererFailure("Readback was not enabled.");
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
// Scene을 생략한 편의 호출에만 기본 방향광을 추가한다. Scene의 광원 구성을 자동 보충하지 않는다.
bool Renderer::Impl::Render(const MeshData& mesh,Image* readback)
{
    try
    {
        Scene scene;
        if(!scene.Add(mesh)) return RendererFailure("Mesh scene construction failed.");
        (void)scene.Add(DirectionalLight{});
        return Render(scene,readback);
    }
    catch(const std::exception& error)
    {
        std::fprintf(stderr,"dyf: Mesh render: %s\n",error.what());
        return false;
    }
}
bool Renderer::Impl::Render(const MeshData& mesh,const Camera& camera,Image* readback)
{
    return Render(mesh,MaterialDesc{},camera,readback);
}
bool Renderer::Impl::Render(const MeshData& mesh,const MaterialDesc& material,const Camera& camera,Image* readback)
{
    try
    {
        Scene scene;
        if(!scene.Add(mesh,material)) return RendererFailure("Mesh scene construction failed.");
        (void)scene.Add(DirectionalLight{});
        return Render(scene,camera,readback);
    }
    catch(const std::exception& error)
    {
        std::fprintf(stderr,"dyf: Mesh render: %s\n",error.what());
        return false;
    }
}

RendererShaderDesc Renderer::Impl::GetShaders() const
{
    if(!shaderView)
    {
        const auto& selected=shadersPending ? pendingShaderSources : shaderSources;
        const auto found=std::find(shaderViews.begin(),shaderViews.end(),selected);
        if(found!=shaderViews.end()) shaderView=&*found;
        else
        {
            shaderViews.push_back(selected);
            shaderView=&shaderViews.back();
        }
    }
    const auto& source = *shaderView;
    RendererShaderDesc desc;
    RHI::ShaderDesc* output[] = {&desc.meshVertex, &desc.meshFragment, &desc.shadowVertex,
        &desc.canvasVertex, &desc.canvasFragment, &desc.toneMapVertex, &desc.toneMapFragment};
    const RHI::ShaderStage stages[] = {RHI::ShaderStage::Vertex, RHI::ShaderStage::Fragment,
        RHI::ShaderStage::Vertex, RHI::ShaderStage::Vertex, RHI::ShaderStage::Fragment,
        RHI::ShaderStage::Vertex, RHI::ShaderStage::Fragment};
    for(size_t i = 0; i < ShaderCount; ++i)
        if(!source.bytes[i].empty()) *output[i] = {stages[i], source.entries[i].c_str(), source.bytes[i].data(), source.bytes[i].size()};
    desc.vertexBindings = source.vertexBindings;
    desc.additionalConstantBytes = source.additionalConstantBytes;
    desc.stockShadowCoverage = source.stockShadowCoverage;
    return desc;
}

bool Renderer::Impl::Render(const Scene& scene, const Camera& camera, const std::vector<RendererDrawDesc>& draws,
    const Canvas* overlay, Image* readback)
{ return RenderScene(scene, &camera, overlay, readback, &draws); }
bool Renderer::Impl::Render(const Scene& scene, const std::vector<RendererDrawDesc>& draws,
    const Canvas* overlay, Image* readback)
{ return RenderScene(scene, nullptr, overlay, readback, &draws); }

bool Renderer::Impl::Initialize()
{
	RHI::IDevice* nativeDevice = device;
	if(nativeDevice == nullptr) return false;

	RHI::SwapchainDesc swapchainDesc = {};
	swapchainDesc.window=windowHandle;
    swapchainDesc.format = RHI::DefaultSwapchainFormat;
	swapchainDesc.minimumImageCount = 2;
	swapchainDesc.allowReadback = config.allowReadback;
	swapchainDesc.presentMode = config.vsync ? RHI::PresentMode::Fifo : RHI::PresentMode::Immediate;
	if(!nativeDevice->CreateSwapchain(swapchainDesc)) return false;

	RHI::TextureHandle backBuffer = nativeDevice->GetBackBuffer();
	if(backBuffer == nullptr || backBuffer->GetDesc().format != swapchainDesc.format) return false;

    return true;
}

bool Renderer::Impl::UsesStockGeometry() const
{
    return shaderSources.bytes[MeshVertex].empty() && shaderSources.bytes[MeshFragment].empty() &&
        shaderSources.bytes[ShadowVertex].empty() && shaderSources.vertexBindings.empty() &&
        shaderSources.additionalConstantBytes==0;
}
bool Renderer::Impl::UsesInstanceStream() const { return config.enableInstancing && UsesStockGeometry(); }
RHI::PipelineHandle Renderer::Impl::MaterialPipeline(const MaterialDesc& material,bool mirrored) const
{
    if(!shaderSources.bytes[MeshFragment].empty()) return pipeline;
    if(wireframeActive && !UsesBlendedRendering(material))
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
bool Renderer::Impl::UsesBindlessMaterials() const
{return config.enableBindlessMaterials&&SupportsBindlessMaterials();}
bool Renderer::Impl::SupportsBindlessMaterials() const
{
    // 사용자 Fragment는 기본 레이아웃 계약을 유지한다. 기본 셰이더의 최적화 가능 여부는 RHI에 묻는다.
    if(!shaderSources.bytes[MeshFragment].empty()) return false;
    std::vector<RHI::ResourceBindingLayout> bindings;
    return device->Supports(MeshLayout(true, bindings));
}

bool Renderer::Impl::InitializeMesh(RHI::TextureHandle output,bool compositeAlpha)
{
    if(!output) output=device->GetBackBuffer();
    if(!output) return false;
    const auto colorFormat=config.enableHdrRendering ? RHI::Format::R16G16B16A16_FLOAT : output->GetDesc().format;
    const bool shadows = config.lighting.enabled && config.lighting.direct && config.lighting.shadows;
    if(pipeline != nullptr && meshColorFormat==colorFormat && meshCompositeAlpha==compositeAlpha)
        return defaultMaterialTextures[0] || CreateDefaultMaterialTextures(device);
    RHI::IDevice* nativeDevice = device;
    const auto stockShaders = DefaultShaders(shadows,UsesBindlessMaterials(),UsesInstanceStream());

	if(!vertexShader) vertexShader = nativeDevice->CreateShader(ShaderDescription(MeshVertex,stockShaders.meshVertex));
	if(!fragmentShader) fragmentShader = nativeDevice->CreateShader(ShaderDescription(MeshFragment,stockShaders.meshFragment));
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
	if(shadows && !shadowVertexShader)
	{
		shadowVertexShader = nativeDevice->CreateShader(ShaderDescription(ShadowVertex,stockShaders.shadowVertex));
	}
	const bool stockCoverage=shaderSources.bytes[ShadowVertex].empty() || shaderSources.stockShadowCoverage;
	if(shadows && stockCoverage && !shadowFragmentShader)
		shadowFragmentShader = nativeDevice->CreateShader(DefaultShadowFragment());
	if(vertexShader == nullptr || fragmentShader == nullptr || (depthPrepass && !depthPrepassFragmentShader) ||
		(shadows && shadowVertexShader == nullptr) ||
		(shadows && stockCoverage && !shadowFragmentShader)) return false;
    nativeDevice->DestroyPipeline(pipeline);pipeline=nullptr;
    for(auto& value:materialPipelines) {nativeDevice->DestroyPipeline(value);value=nullptr;}
    for(auto& value:depthPrepassPipelines) {nativeDevice->DestroyPipeline(value);value=nullptr;}
    for(auto& value:noAreaMaterialPipelines) {nativeDevice->DestroyPipeline(value);value=nullptr;}
    for(auto& value:noAreaDepthPrepassPipelines) {nativeDevice->DestroyPipeline(value);value=nullptr;}
    for(auto& value:wireframePipelines) {nativeDevice->DestroyPipeline(value);value=nullptr;}
    nativeDevice->DestroyPipeline(shadowPipeline);shadowPipeline=nullptr;
    for(auto& value:shadowMaterialPipelines) {nativeDevice->DestroyPipeline(value);value=nullptr;}
    for(auto& value:compactShadowPipelines) {nativeDevice->DestroyPipeline(value);value=nullptr;}
	if(!BuildPipelineStates(nativeDevice,colorFormat,compositeAlpha)) return false;
    meshColorFormat=colorFormat;
    meshCompositeAlpha=compositeAlpha;
    if(!defaultMaterialTextures[0] && !CreateDefaultMaterialTextures(nativeDevice)) return false;
	return true;
}

void Renderer::Impl::Shutdown()
{
    if(!device)return;
    outputPresentation.reset();
    shadowAtlasCache.valid=false;shadowAtlasCache.pending=false;
    // 생성한 RHI 자원은 Renderer에서 직접 해제한다. 보조 객체의 소멸 순서에 위임하지 않는다.
    device->DestroyResourceSet(toneSet);
    for(auto& image:canvasImages) {device->DestroyResourceSet(image.set);device->DestroyTexture(image.texture);}
    for(auto& vertices:canvasVertices)device->DestroyBuffer(vertices.buffer);
    for(auto& cached:mainSets)device->DestroyResourceSet(cached.set);
    for(auto& cached:shadowSets)device->DestroyResourceSet(cached.set);
    InvalidateFrameHistory(true);
    extensions.clear();
    ReleaseGeometry(device);ReleaseTextures(device);
    m_profiler.ResetGpuQueries();
    for(auto* texture:{depthStencilTarget,shadowDepthTarget,hdrTarget})if(texture)device->DestroyTexture(texture);
    for(auto* texture:defaultMaterialTextures)if(texture)device->DestroyTexture(texture);
    for(auto* buffer:{lightingBuffer,shadowMatrixBuffer,instanceBuffer,materialIndexBuffer,cpuInstanceBuffer})if(buffer)device->DestroyBuffer(buffer);
    for(auto* value:{pipeline,shadowPipeline,canvasPipeline,tonePipeline})if(value)device->DestroyPipeline(value);
    for(auto* value:materialPipelines)if(value)device->DestroyPipeline(value);
    for(auto* value:depthPrepassPipelines)if(value)device->DestroyPipeline(value);
    for(auto* value:noAreaMaterialPipelines)if(value)device->DestroyPipeline(value);
    for(auto* value:noAreaDepthPrepassPipelines)if(value)device->DestroyPipeline(value);
    for(auto* value:wireframePipelines)if(value)device->DestroyPipeline(value);
    for(auto* value:shadowMaterialPipelines)if(value)device->DestroyPipeline(value);
    for(auto* value:compactShadowPipelines)if(value)device->DestroyPipeline(value);
    for(auto* shader:{vertexShader,fragmentShader,depthPrepassFragmentShader,noAreaFragmentShader,noAreaDepthPrepassFragmentShader,shadowVertexShader,shadowFragmentShader,canvasVertexShader,canvasFragmentShader,toneVertexShader,toneFragmentShader})
        if(shader)device->DestroyShader(shader);
}



// RHI의 행 간격과 BGRA 출력을 Image의 연속 RGBA 픽셀로 변환한다.
bool Renderer::Impl::CaptureFrame(Image& image)
{
    RHI::TextureReadback readback;
    if (!device->ReadTexture(device->GetBackBuffer(), readback))
        {std::fprintf(stderr,"dyf: Frame readback failed; enable RendererConfig::allowReadback on a native backend.\n");return false;}
    const uint64_t rowBytes = static_cast<uint64_t>(readback.width) * 4u;
    const uint64_t size = rowBytes * readback.height;
    if (!RHI::IsReadbackFormat(readback.format) || !readback.width || !readback.height ||
        readback.rowPitch < rowBytes || size > std::numeric_limits<size_t>::max() ||
        static_cast<uint64_t>(readback.height - 1) * readback.rowPitch + rowBytes > readback.pixels.size())
        {std::fprintf(stderr,"dyf: RHI returned an invalid readback image.\n");return false;}
    std::vector<uint8_t> pixels(static_cast<size_t>(size));
    for (uint32_t row = 0; row < readback.height; ++row)
        std::memcpy(pixels.data() + static_cast<size_t>(row) * rowBytes,
            readback.pixels.data() + static_cast<size_t>(row) * readback.rowPitch, static_cast<size_t>(rowBytes));
    if (readback.format == RHI::Format::B8G8R8A8_UNORM || readback.format == RHI::Format::B8G8R8A8_UNORM_SRGB)
        for (size_t i = 0; i < pixels.size(); i += 4) std::swap(pixels[i], pixels[i + 2]);
    // Renderer의 UNORM 출력도 표시용 색으로 인코딩하므로 캡처 결과는 sRGB 이미지다.
    image = Image(readback.width, readback.height, std::move(pixels), ColorSpace::Srgb);
    return true;
}
