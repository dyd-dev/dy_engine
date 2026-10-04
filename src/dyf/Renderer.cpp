#include "dyf/Renderer.h"
#include <cstdio>
#include "dyf/Canvas.h"
#include "SceneAlgorithms.h"

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
#include <chrono>
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
#include "dyf/RHI/ResourceScope.h"

using namespace dyf;

namespace
{
	[[nodiscard]] inline Math::float3 NormalizeDirection(const Math::float3& direction)
	{
		return Math::NormalizeOr(direction, Math::float3(0.0f, 0.0f, -1.0f));
	}

	[[nodiscard]] inline Math::float3 OrthogonalUp(const Math::float3& direction, const Math::float3& up)
	{
		const Math::float3 projected = up - direction * Math::Dot(up, direction);
		const Math::float3 fallback = std::fabs(direction.z) < 0.99f
			? Math::float3(0.0f, 0.0f, 1.0f)
			: Math::float3(0.0f, 1.0f, 0.0f);
		return Math::NormalizeOr(projected, Math::NormalizeOr(
			fallback - direction * Math::Dot(fallback, direction),
			Math::float3(1.0f, 0.0f, 0.0f)));
	}
}

namespace
{
    // 예상한 실패는 stderr와 반환값으로 전달한다. throw 후 즉시 catch하는 우회는 두지 않는다.
    bool RendererFailure(const char* message)
    {
        std::fprintf(stderr,"dyf: %s\n",message);
        return false;
    }
    bool ValidateRendererConfig(const RendererConfig& desc)
    {
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
        if(desc.lighting.shadowQuality > ShadowQuality::High)
            return RendererFailure("Invalid shadow configuration.");
        return true;
    }
}
bool Renderer::SetConfig(const RendererConfig& desc)
{
    try
    {
        if(!ValidateRendererConfig(desc)) return false;
        pendingConfig=std::make_unique<RendererConfig>(desc);return true;
    }
    catch(const std::exception& error){std::fprintf(stderr,"dyf: Renderer settings: %s\n",error.what());return false;}
}
bool Renderer::SetClearColor(Math::float4 value) {auto desc=GetConfig();desc.clearColor=value;return SetConfig(desc);}
bool Renderer::SetVSync(bool value) {auto desc=GetConfig();desc.vsync=value;return SetConfig(desc);}
bool Renderer::SetLighting(const LightingConfig& value) {auto desc=GetConfig();desc.lighting=value;return SetConfig(desc);}
bool Renderer::SetLightingEnabled(bool value) {auto desc=GetConfig();desc.lighting.enabled=value;return SetConfig(desc);}
bool Renderer::SetShadowsEnabled(bool value) {auto desc=GetConfig();desc.lighting.shadows=value;return SetConfig(desc);}
bool Renderer::SetHdrEnabled(bool value) {auto desc=GetConfig();desc.enableHdrRendering=value;return SetConfig(desc);}
bool Renderer::SetExposure(float value) {auto desc=GetConfig();desc.exposure=value;return SetConfig(desc);}
bool Renderer::SetProfilerVisible(bool value) {auto desc=GetConfig();desc.enableProfilerHud=value;return SetConfig(desc);}
bool Renderer::SetReadbackEnabled(bool value) {auto desc=GetConfig();desc.allowReadback=value;return SetConfig(desc);}
bool Renderer::SetShaders(const RendererShaderDesc& desc)
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
        bool sameBindings = next.vertexBindings.size() == shaderSources.vertexBindings.size();
        for(size_t i = 0; sameBindings && i < next.vertexBindings.size(); ++i)
        {
            const auto& left = next.vertexBindings[i];
            const auto& right = shaderSources.vertexBindings[i];
            sameBindings = left.binding == right.binding && left.type == right.type && left.count == right.count &&
                left.stages == right.stages && left.type != RHI::ResourceBindingType::StaticSampler;
        }
        if(next.bytes == shaderSources.bytes && next.entries == shaderSources.entries && sameBindings &&
            next.additionalConstantBytes == shaderSources.additionalConstantBytes)
        { pendingShaderSources = {}; shadersPending = false; shaderViewCurrent=false; return true; }
        pendingShaderSources=std::move(next);shadersPending=true;shaderViewCurrent=false;return true;
    }
    catch(const std::exception& error){std::fprintf(stderr,"dyf: Shader override: %s\n",error.what());return false;}
}

RHI::ShaderDesc Renderer::ShaderDescription(ShaderSlot slot,const RHI::ShaderDesc& stock) const
{
    // 지정하지 않은 단계는 기본 셰이더를 사용하고, 지정한 단계만 교체한다.
    if(shaderSources.bytes[slot].empty())return stock;
    return {stock.stage,shaderSources.entries[slot].c_str(),shaderSources.bytes[slot].data(),shaderSources.bytes[slot].size()};
}
bool Renderer::ApplySettings()
{
    if(canvasFramePending)
    {
        const bool recovered=device->Present();
        canvasFramePending=false;
        if(!recovered) return RendererFailure("Canvas frame recovery failed.");
    }
    if(!pendingConfig && !shadersPending) return true;
    const auto& nextConfig = pendingConfig ? *pendingConfig : config;
    const bool shadows = config.lighting.enabled && config.lighting.shadows;
    const bool nextShadows = nextConfig.lighting.enabled && nextConfig.lighting.shadows;
    const bool rebuild = shadersPending || shadows != nextShadows ||
        config.enableHdrRendering != nextConfig.enableHdrRendering ||
        config.enableInstancing != nextConfig.enableInstancing;
    const bool outputChanged = config.vsync != nextConfig.vsync ||
        config.allowReadback != nextConfig.allowReadback;
    RHI::SwapchainDesc output;
    output.format = RHI::Format::B8G8R8A8_UNORM;
    output.presentMode = nextConfig.vsync ? RHI::PresentMode::Fifo : RHI::PresentMode::Immediate;
    output.allowReadback = nextConfig.allowReadback;
    output.window = windowHandle;
    if(!rebuild)
    {
        if(outputChanged && !device->CreateSwapchain(output))
            return RendererFailure("The requested output settings are unavailable.");
        // 색·노출·조명 상수·표시는 다음 프레임 입력만 바꾼다. 메시와 텍스처 캐시는 유지한다.
        // 그림자 품질에 따른 깊이 텍스처 크기 변경은 EnsureShadowDepthTarget에서 처리한다.
        config=nextConfig;
        pendingConfig.reset();
        shaderViews.clear();shaderViewCurrent=false;
        return true;
    }
    Renderer next(*device, windowHandle, nextConfig);
    next.shaderSources=shadersPending ? pendingShaderSources : shaderSources;
    if(pipeline && !next.InitializeMesh())
        return RendererFailure("Renderer settings could not create the requested pipeline.");
    if(outputChanged && !device->CreateSwapchain(output))
            return RendererFailure("The requested output settings are unavailable.");
    SwapResources(next);
    pendingConfig.reset();
    pendingShaderSources={};shadersPending=false;
    shaderViews.clear();shaderViewCurrent=false;
    return true;
}
void Renderer::SwapResources(Renderer& other)
{
    using std::swap;
    swap(config,other.config);
    swap(shaderSources,other.shaderSources);
    swap(materialStates,other.materialStates);
    swap(vertexShader,other.vertexShader);
    swap(fragmentShader,other.fragmentShader);
    swap(shadowVertexShader,other.shadowVertexShader);
    swap(canvasVertexShader,other.canvasVertexShader);
    swap(canvasFragmentShader,other.canvasFragmentShader);
    swap(toneVertexShader,other.toneVertexShader);
    swap(toneFragmentShader,other.toneFragmentShader);
    swap(pipeline,other.pipeline);
    swap(shadowPipeline,other.shadowPipeline);
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
    swap(mainSetScratch,other.mainSetScratch);
    swap(shadowSetScratch,other.shadowSetScratch);
    swap(mainBindingScratch,other.mainBindingScratch);
    swap(mainCombinedBindingScratch,other.mainCombinedBindingScratch);
    swap(shadowBindingScratch,other.shadowBindingScratch);

    swap(m_meshes,other.m_meshes);
    swap(m_textures,other.m_textures);
    swap(m_indices,other.m_indices);
}

Renderer::Renderer(RHI::IDevice& borrowedDevice,const void* window,const RendererConfig& desc)
    : device(&borrowedDevice),windowHandle(window),config(desc)
{
}
Renderer::~Renderer() { Shutdown(); }

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
    renderer->ownedDevice = std::move(owned);
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
    if(!renderer->Initialize()){RendererFailure("Renderer initialization failed.");return nullptr;}
    return renderer;

    }
    catch(const std::exception& error){std::fprintf(stderr,"dyf: Renderer creation: %s\n",error.what());return nullptr;}
}
bool Renderer::Render(const Scene& scene, Image* readback)
{
    return RenderScene(scene,nullptr,nullptr,readback);
}
bool Renderer::Render(const Scene& scene, const Camera& camera, Image* readback)
{
    return RenderScene(scene,&camera,nullptr,readback);
}
bool Renderer::Render(const Scene& scene, const Canvas& overlay, Image* readback)
{
    return RenderScene(scene,nullptr,&overlay,readback);
}
bool Renderer::Render(const Scene& scene, const Camera& camera, const Canvas& overlay, Image* readback)
{
    return RenderScene(scene,&camera,&overlay,readback);
}
bool Renderer::Render(const Canvas& canvas, Image* readback)
{
    try
    {
        if(!ApplySettings()) return false;
        if(readback && !config.allowReadback) return RendererFailure("Readback was not enabled.");
        return RenderCanvas(canvas,readback);
    }
    catch(const std::exception& error)
    {
        std::fprintf(stderr,"dyf: Canvas render: %s\n",error.what());
        return false;
    }
}
// Scene을 생략한 편의 호출에만 기본 방향광을 추가한다. Scene의 광원 구성을 자동 보충하지 않는다.
bool Renderer::Render(const MeshData& mesh,Image* readback)
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
bool Renderer::Render(const MeshData& mesh,const Camera& camera,Image* readback)
{
    return Render(mesh,MaterialDesc{},camera,readback);
}
bool Renderer::Render(const MeshData& mesh,const MaterialDesc& material,const Camera& camera,Image* readback)
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

RendererShaderDesc Renderer::GetShaders() const
{
    if(!shaderViewCurrent)
    {
        shaderViews.push_back(shadersPending ? pendingShaderSources : shaderSources);
        shaderViewCurrent=true;
    }
    const auto& source = shaderViews.back();
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
    return desc;
}

bool Renderer::Render(const Scene& scene, const Camera& camera, const std::vector<RendererDrawDesc>& draws,
    const Canvas* overlay, Image* readback)
{ return RenderScene(scene, &camera, overlay, readback, &draws); }
bool Renderer::Render(const Scene& scene, const std::vector<RendererDrawDesc>& draws,
    const Canvas* overlay, Image* readback)
{ return RenderScene(scene, nullptr, overlay, readback, &draws); }

bool Renderer::Initialize()
{
	RHI::IDevice* nativeDevice = device;
	if(nativeDevice == nullptr) return false;

	RHI::SwapchainDesc swapchainDesc = {};
	swapchainDesc.window=windowHandle;
    swapchainDesc.format = RHI::Format::B8G8R8A8_UNORM;
	swapchainDesc.minimumImageCount = 2;
	swapchainDesc.allowReadback = config.allowReadback;
	swapchainDesc.presentMode = config.vsync ? RHI::PresentMode::Fifo : RHI::PresentMode::Immediate;
	if(!nativeDevice->CreateSwapchain(swapchainDesc)) return false;

	RHI::TextureHandle backBuffer = nativeDevice->GetBackBuffer();
	if(backBuffer == nullptr || backBuffer->GetDesc().format != swapchainDesc.format) return false;

    return true;
}

bool Renderer::UsesStockGeometry() const
{
    return shaderSources.bytes[MeshVertex].empty() && shaderSources.bytes[MeshFragment].empty() &&
        shaderSources.bytes[ShadowVertex].empty() && shaderSources.vertexBindings.empty() &&
        shaderSources.additionalConstantBytes==0;
}
bool Renderer::UsesInstanceStream() const { return config.enableInstancing && UsesStockGeometry(); }
bool Renderer::UsesBindlessMaterials() const
{
    // 사용자 Fragment는 기본 레이아웃 계약을 유지한다. 기본 셰이더의 최적화 가능 여부는 RHI에 묻는다.
    if(!shaderSources.bytes[MeshFragment].empty()) return false;
    std::vector<RHI::ResourceBindingLayout> bindings;
    return device->Supports(MeshLayout(true, bindings));
}

bool Renderer::InitializeMesh(RHI::TextureHandle output,bool compositeAlpha)
{
    if(!output) output=device->GetBackBuffer();
    if(!output) return false;
    const auto colorFormat=config.enableHdrRendering ? RHI::Format::R16G16B16A16_FLOAT : output->GetDesc().format;
    const bool shadows = config.lighting.enabled && config.lighting.shadows;
    if(pipeline != nullptr && meshColorFormat==colorFormat && meshCompositeAlpha==compositeAlpha)
        return (defaultMaterialTextures[0] || CreateDefaultMaterialTextures(device)) &&
            EnsureDepthStencilTarget(device,output);
    RHI::IDevice* nativeDevice = device;
    const auto stockShaders = DefaultShaders(shadows,UsesBindlessMaterials(),UsesInstanceStream());

	if(!vertexShader) vertexShader = nativeDevice->CreateShader(ShaderDescription(MeshVertex,stockShaders.meshVertex));
	if(!fragmentShader) fragmentShader = nativeDevice->CreateShader(ShaderDescription(MeshFragment,stockShaders.meshFragment));
	if(shadows && !shadowVertexShader)
	{
		shadowVertexShader = nativeDevice->CreateShader(ShaderDescription(ShadowVertex,stockShaders.shadowVertex));
	}
	if(vertexShader == nullptr || fragmentShader == nullptr ||
		(shadows && shadowVertexShader == nullptr)) return false;
    nativeDevice->DestroyPipeline(pipeline);pipeline=nullptr;
	if(!BuildPipelineStates(nativeDevice,colorFormat,compositeAlpha)) return false;
    meshColorFormat=colorFormat;
    meshCompositeAlpha=compositeAlpha;
    if(!defaultMaterialTextures[0] && !CreateDefaultMaterialTextures(nativeDevice)) return false;
	return EnsureDepthStencilTarget(nativeDevice,output);
}

void Renderer::Shutdown()
{
    if(!device)return;
    // 생성한 RHI 자원은 Renderer에서 직접 해제한다. 보조 객체의 소멸 순서에 위임하지 않는다.
    device->DestroyResourceSet(toneSet);
    for(auto& image:canvasImages) {device->DestroyResourceSet(image.set);device->DestroyTexture(image.texture);}
    for(auto& vertices:canvasVertices)device->DestroyBuffer(vertices.buffer);
    for(auto& cached:mainSets)device->DestroyResourceSet(cached.set);
    for(auto& cached:shadowSets)device->DestroyResourceSet(cached.set);
    ReleaseGeometry(device);ReleaseTextures(device);
    for(const auto& sample:m_pending)device->DestroyTimestampQuery(sample.query);
    for(auto* texture:{depthStencilTarget,shadowDepthTarget,hdrTarget})if(texture)device->DestroyTexture(texture);
    for(auto* texture:defaultMaterialTextures)if(texture)device->DestroyTexture(texture);
    for(auto* buffer:{lightingBuffer,shadowMatrixBuffer,instanceBuffer,materialIndexBuffer})if(buffer)device->DestroyBuffer(buffer);
    for(auto* value:{pipeline,shadowPipeline,canvasPipeline,tonePipeline})if(value)device->DestroyPipeline(value);
    for(auto* shader:{vertexShader,fragmentShader,shadowVertexShader,canvasVertexShader,canvasFragmentShader,toneVertexShader,toneFragmentShader})
        if(shader)device->DestroyShader(shader);
}

bool Renderer::RenderToTexture(const Scene& scene,const Camera& camera,RHI::TextureHandle target,RHI::ResourceState before)
{
    lastStatistics={};
    if(!target) return RendererFailure("RenderToTexture requires a target.");
    const auto& desc=target->GetDesc();
    const auto usage=RHI::TextureUsage::RenderTarget|RHI::TextureUsage::ShaderResource;
    if(!desc.width || !desc.height || desc.depthOrArraySize!=1 || desc.mipLevels!=1 ||
        (desc.usage&usage)!=usage || !RHI::IsReadbackFormat(desc.format))
        return RendererFailure("RenderToTexture requires a single-mip 2D RGBA/BGRA8 renderable sampled texture.");
    if(before!=RHI::ResourceState::Undefined && before!=RHI::ResourceState::ShaderResource &&
        before!=RHI::ResourceState::RenderTarget)
        return RendererFailure("RenderToTexture before state must be Undefined, ShaderResource or RenderTarget.");
    return RenderScene(scene,&camera,nullptr,nullptr,nullptr,target,before);
}

bool Renderer::RenderScene(const Scene& scene, const Camera* selectedCamera, const Canvas* overlay, Image* readback,
    const std::vector<RendererDrawDesc>* draws,RHI::TextureHandle selectedOutput,RHI::ResourceState before)
{
    try
    {
        lastStatistics={};
        DY_PROFILE_CPU_ZONE_NAMED("Renderer::RenderScene");
        if(!ApplySettings())return false;
        if(!selectedOutput && Platform::Window::ConsumeKeyPress(Platform::Key::F11,windowHandle))
            config.profilerStartsExpanded = !config.profilerStartsExpanded;
        if(draws && draws->size() != scene.GetEntityCount()) return RendererFailure("Draw input count must match the scene.");
        if(draws) for(const auto& draw : *draws)
            if(draw.inlineConstants.size() != shaderSources.additionalConstantBytes)
                return RendererFailure("Draw constants do not match the selected shader layout.");
        const auto cpuStart=std::chrono::steady_clock::now();
        if(readback && !config.allowReadback) return RendererFailure("Renderer readback was not enabled.");
        RHI::IDevice* nativeDevice = device;
        if(!selectedOutput && !nativeDevice->BeginFrame()){if(nativeDevice->IsLost())return RendererFailure("RHI device was lost.");if(readback)*readback={};return true;}
        auto* output=selectedOutput ? selectedOutput : nativeDevice->GetBackBuffer();
        const auto after=selectedOutput ? RHI::ResourceState::ShaderResource : RHI::ResourceState::Present;
        Camera defaultCamera;
        const auto& targetDesc = output->GetDesc();
        if(!selectedCamera) defaultCamera.SetPerspective(targetDesc.width / static_cast<float>(targetDesc.height));
        const Camera& camera = selectedCamera ? *selectedCamera : defaultCamera;
        ShadowData shadows;
        BuildShadows(shadows,scene,camera);
        if(!InitializeMesh(output,selectedOutput!=nullptr)) return RendererFailure("Mesh pipeline creation failed.");


        if(!SyncTextures(scene, nativeDevice)) return RendererFailure("Scene texture upload failed.");
        materialStates.resize(scene.Materials().size());
        UpdateMaterialStates(scene);
        if(!EnsureDepthStencilTarget(nativeDevice,output) || !EnsureShadowDepthTarget(nativeDevice,shadows.columns,shadows.rows,shadows.resolution))
            return RendererFailure("Scene depth target creation failed.");

        if(config.enableHdrRendering && !PreparePostProcess(output))
            return RendererFailure("HDR pass creation failed.");
        if(!PrepareGeometry(scene, nativeDevice)) return RendererFailure("Mesh upload failed.");

        {
            // Replace only size-mismatched buffers. Roll back ownership and committed
            // states if any upload/close/submit step fails (including exceptions).
            struct FrameBuffers {
                Renderer& r;
                RHI::BufferHandle light,shadow,instances;
                bool lightReady,shadowReady,instancesReady,committed=false;
                explicit FrameBuffers(Renderer& owner):r(owner),light(r.lightingBuffer),shadow(r.shadowMatrixBuffer),instances(r.instanceBuffer),
                    lightReady(r.lightingReady),shadowReady(r.shadowReady),instancesReady(r.instanceReady) {}
                ~FrameBuffers() {
                    const auto finish=[&](RHI::BufferHandle& current,RHI::BufferHandle previous,bool& ready,bool oldReady) {
                        if(committed) {if(current!=previous)r.device->DestroyBuffer(previous);}
                        else {if(current!=previous)r.device->DestroyBuffer(current);current=previous;ready=oldReady;}
                    };
                    finish(r.lightingBuffer,light,r.lightingReady,lightReady);
                    finish(r.shadowMatrixBuffer,shadow,r.shadowReady,shadowReady);
                    finish(r.instanceBuffer,instances,r.instanceReady,instancesReady);
                }
            } buffers(*this);
            RHI::ResourceScope resources(*nativeDevice);
            auto* frameDataCommand=nativeDevice->AcquireCommandList();
            if(!frameDataCommand) return RendererFailure("Frame command list acquisition failed.");
            resources.Keep(frameDataCommand);
            const bool shadowUpdated=UpdateShadowBuffer(nativeDevice,*frameDataCommand,shadows);
            const bool lightingUpdated=shadowUpdated && UpdateLightingBuffer(scene,camera,nativeDevice,*frameDataCommand);
            const bool instancesUpdated=lightingUpdated && UpdateInstanceBuffer(scene,*frameDataCommand);
            const bool submitted=instancesUpdated && frameDataCommand->Close() && nativeDevice->Submit(&frameDataCommand,1);
            if(!submitted) return RendererFailure("Frame data upload/submission failed.");
            lightingReady=true;shadowReady=shadowMatrixBuffer!=nullptr;instanceReady=instanceBuffer!=nullptr;
            CommitFrameData();
            buffers.committed=true;
        }

        CanvasFrame canvasFrame(*this);
        recordingStatistics={};materialIndexRecorded=false;
        const auto frameWindow=std::max(1u,device->GetDesc().maxFramesInFlight);
        for(auto& set:mainSets)if(set.unusedFrames<frameWindow)++set.unusedFrames;
        for(auto& set:shadowSets)if(set.unusedFrames<frameWindow)++set.unusedFrames;
        struct CacheFrame {
            Renderer& owner;
            bool empty;
            ~CacheFrame() {
                if(empty) {
                    for(auto& set:owner.mainSets)set.unusedFrames=UINT32_MAX;
                    for(auto& set:owner.shadowSets)set.unusedFrames=UINT32_MAX;
                }
                owner.PruneResourceSets(owner.mainSets);owner.PruneResourceSets(owner.shadowSets);
            }
        } cacheFrame{*this,scene.GetEntityCount()==0};
        struct GpuSamples
        {
            Renderer& renderer;
            RHI::TimestampQueryHandle shadow=nullptr,main=nullptr;
            RHI::FenceHandle completion;
            ~GpuSamples()
            {
                if(shadow) renderer.SubmittedGpuSample(shadow,completion);
                if(main) renderer.SubmittedGpuSample(main,completion);
            }
        } samples{*this};
        if(!selectedOutput)
        {
            samples.shadow=BeginGpuSample(0);
            samples.main=BeginGpuSample(1);
        }
        const auto shadowQuery=samples.shadow, mainQuery=samples.main;
        using State = RHI::ResourceState;
        RHI::RenderGraph graph;
        const auto backBuffer = graph.ImportTexture("Output", output, before, after);
        const auto depth = graph.ImportTexture("Depth", depthStencilTarget, depthStencilState, State::DepthWrite);
        const auto color = config.enableHdrRendering
            ? graph.ImportTexture("HDR", hdrTarget, hdrState, State::ShaderResource) : backBuffer;
        const auto shadow = shadowDepthTarget
            ? graph.ImportTexture("ShadowDepth", shadowDepthTarget, shadowDepthState, State::ShaderResource)
            : RHI::RGResourceHandle{};
        const bool drawShadows = shadowPipeline && shadows.viewCount && shadowDepthTarget &&
            shadowMatrixBuffer && shadowDepthTarget->GetDesc().width;
        const auto require = [](bool success) {
            if(!success) throw std::runtime_error("Renderer pass recording failed.");
        };
        auto& shadowPass = graph.AddPass("Shadow");
        if(drawShadows) shadowPass.Write(shadow, State::DepthWrite);
        shadowPass.SetExecute([&](RHI::ICommandList* commands) {
            if(drawShadows) require(RecordShadowPass(scene, camera, shadows, *commands, draws, shadowQuery));
            else if(shadowQuery)
            {
                commands->ResetTimestamps(shadowQuery, 0, 2);
                commands->WriteTimestamp(shadowQuery, 0);
                commands->WriteTimestamp(shadowQuery, 1);
            }
        });
        auto& mainPass = graph.AddPass("MainForward");
        mainPass.Write(color, State::RenderTarget).Write(depth, State::DepthWrite);
        if(shadowDepthTarget) mainPass.Read(shadow, State::ShaderResource);
        mainPass.SetExecute([&](RHI::ICommandList* commands) {
            require(RecordMainPass(scene, camera, *commands, draws, mainQuery, output));
        });
        if(config.enableHdrRendering)
            graph.AddPass("ToneMap").Read(color, State::ShaderResource).Write(backBuffer, State::RenderTarget)
                .SetExecute([&](RHI::ICommandList* commands) {
                    require(RecordToneMap(*commands, output, config.exposure));
                });
        if(overlay)
            graph.AddPass("Overlay").Write(backBuffer, State::RenderTarget)
                .SetExecute([&](RHI::ICommandList* commands) {
                    require(RecordCanvas(*overlay, *commands, output, true, true));
                });
        if(!selectedOutput)
            graph.AddPass("Profiler HUD").Write(backBuffer, State::RenderTarget)
                .SetExecute([&](RHI::ICommandList* commands) {
                    const double cpuMs = std::chrono::duration<double,std::milli>(
                        std::chrono::steady_clock::now()-cpuStart).count();
                    RecordProfilerFrame(cpuMs, scene.GetEntityCount());
                    if(config.enableProfilerHud)
                    {
                        commands->BeginDebugEvent("Profiler HUD");
                        auto hud = BuildProfilerOverlay(output->GetDesc().width, output->GetDesc().height,
                            config.profilerStartsExpanded);
                        require(RecordCanvas(hud, *commands, output, true, true));
                        commands->EndDebugEvent();
                    }
                    if(mainQuery) commands->WriteTimestamp(mainQuery, 1);
                });
        if(!graph.Compile()) return RendererFailure("Scene RenderGraph compilation failed.");
        struct FrameCommands
        {
            RHI::IDevice& device;
            std::vector<RHI::ICommandList*> lists;
            ~FrameCommands() { for(auto* commands : lists) device.DestroyCommandList(commands); }
        } frame{*nativeDevice, {}};
        bool drawn;
        if(config.enableParallelRenderGraph && config.threadPool)
            drawn = graph.ExecuteParallel(nativeDevice, config.threadPool, frame.lists);
        else
        {
            frame.lists.reserve(1);
            auto* commands = nativeDevice->AcquireCommandList();
            frame.lists.push_back(commands);
            drawn = commands && graph.Execute(commands) && commands->Close();
        }
        RHI::FenceHandle completion;
        drawn = drawn && nativeDevice->Submit({frame.lists.data(),
            static_cast<uint32_t>(frame.lists.size()), nullptr, 0}, completion);
        if(drawn) {
            canvasFrame.Commit();
            samples.completion=completion;
            lastRenderCompletion=completion;
            lastStatistics=recordingStatistics;
            if(materialIndexRecorded) {materialIndexReady=true;materialIndexBytes.swap(materialIndexScratch);}
        }
        if(!drawn)return RendererFailure("Scene draw submission failed.");
        if(config.enableHdrRendering) hdrState=RHI::ResourceState::ShaderResource;
        depthStencilState = RHI::ResourceState::DepthWrite;
        shadowDepthState = shadowDepthTarget != nullptr
            ? RHI::ResourceState::ShaderResource
            : RHI::ResourceState::Undefined;
        if(readback && !CaptureFrame(*readback))return false;
        if(!selectedOutput)
        {
            if(!nativeDevice->Present()) return RendererFailure("Scene presentation failed.");
            DY_PROFILE_FRAME_MARK();
        }
        return true;
    }
    catch(const std::exception& error)
    {
        std::fprintf(stderr,"dyf: Render: %s\n",error.what());
        return false;
    }
}

RHI::PipelineLayoutDesc Renderer::MeshLayout(bool bindless, std::vector<RHI::ResourceBindingLayout>& bindings) const
{
	RHI::SamplerDesc materialSampler = {};
	materialSampler.minFilter = RHI::SamplerFilter::Linear;
	materialSampler.magFilter = RHI::SamplerFilter::Linear;
	materialSampler.mipFilter = RHI::SamplerFilter::Linear;
	materialSampler.addressU = RHI::SamplerAddressMode::Repeat;
	materialSampler.addressV = RHI::SamplerAddressMode::Repeat;
	materialSampler.addressW = RHI::SamplerAddressMode::Repeat;
	materialSampler.mipLodBias = 0.0f;
	materialSampler.minLod = 0.0f;
	materialSampler.maxLod = 0.0f;

	RHI::SamplerDesc shadowSampler = materialSampler;
	shadowSampler.addressU = RHI::SamplerAddressMode::ClampToEdge;
	shadowSampler.addressV = RHI::SamplerAddressMode::ClampToEdge;
	shadowSampler.addressW = RHI::SamplerAddressMode::ClampToEdge;

	const RHI::ShaderStageFlags vertexAndFragment =
		RHI::ShaderStageFlags::Vertex | RHI::ShaderStageFlags::Fragment;
    if(bindless)
    {
        bindings = {
            {0,RHI::ResourceBindingType::SampledTexture,28,RHI::ShaderStageFlags::Fragment,{}},
            {1,RHI::ResourceBindingType::ConstantBuffer,1,RHI::ShaderStageFlags::Fragment,{}},
            {8,RHI::ResourceBindingType::StaticSampler,1,RHI::ShaderStageFlags::Fragment,materialSampler},
            {29,RHI::ResourceBindingType::ReadOnlyStorageBuffer,1,RHI::ShaderStageFlags::Fragment,{}}
        };
    }
    else
    {
        bindings = {
            {0u,RHI::ResourceBindingType::SampledTexture,1,RHI::ShaderStageFlags::Fragment,{}},
            {1u,RHI::ResourceBindingType::ConstantBuffer,1,RHI::ShaderStageFlags::Fragment,{}},
            {4u,RHI::ResourceBindingType::SampledTexture,1,RHI::ShaderStageFlags::Fragment,{}},
            {5u,RHI::ResourceBindingType::SampledTexture,1,RHI::ShaderStageFlags::Fragment,{}},
            {6u,RHI::ResourceBindingType::SampledTexture,1,RHI::ShaderStageFlags::Fragment,{}},
            {7u,RHI::ResourceBindingType::SampledTexture,1,RHI::ShaderStageFlags::Fragment,{}},
            {8u,RHI::ResourceBindingType::StaticSampler,1,RHI::ShaderStageFlags::Fragment,materialSampler}
        };
    }
    if(config.lighting.enabled && config.lighting.shadows)
    {
        bindings.push_back({3u,RHI::ResourceBindingType::ConstantBuffer,1,vertexAndFragment,{}});
        bindings.push_back({bindless ? 28u : 2u,RHI::ResourceBindingType::SampledTexture,1,RHI::ShaderStageFlags::Fragment,{}});
        bindings.push_back({9u,RHI::ResourceBindingType::StaticSampler,1,RHI::ShaderStageFlags::Fragment,shadowSampler});
    }
    bindings.insert(bindings.end(),shaderSources.vertexBindings.begin(),shaderSources.vertexBindings.end());
    // 지원 조회와 실제 생성이 같은 바인딩·상수 크기를 사용한다.
    return {bindings.data(),static_cast<uint32_t>(bindings.size()),
        static_cast<uint32_t>(sizeof(DrawConstants))+shaderSources.additionalConstantBytes,
        vertexAndFragment,10u};
}

bool Renderer::BuildPipelineStates(RHI::IDevice* device,RHI::Format colorFormat,bool compositeAlpha)
{
	if(colorFormat == RHI::Format::Unknown) return false;

	const RHI::VertexBufferLayout vertexBuffer = {
		0,
		static_cast<uint32_t>(sizeof(RendererVertex)),
		RHI::VertexStepMode::Vertex
	};
	const std::array<RHI::VertexAttribute, 4> vertexAttributes = {{
		{ 0, 0, RHI::Format::R32G32B32_FLOAT, static_cast<uint32_t>(offsetof(RendererVertex, px)) },
		{ 1, 0, RHI::Format::R32G32B32_FLOAT, static_cast<uint32_t>(offsetof(RendererVertex, nx)) },
		{ 2, 0, RHI::Format::R32G32_FLOAT, static_cast<uint32_t>(offsetof(RendererVertex, u)) },
		{ 3, 0, RHI::Format::R32G32B32A32_FLOAT, static_cast<uint32_t>(offsetof(RendererVertex, tx)) }
	}};

    std::vector<RHI::VertexBufferLayout> vertexBuffers={vertexBuffer};
    std::vector<RHI::VertexAttribute> attributes(vertexAttributes.begin(),vertexAttributes.end());
    if(UsesInstanceStream()) {
        vertexBuffers.push_back({1,64,RHI::VertexStepMode::Instance});
        for(uint32_t column=0;column<4;++column)
            attributes.push_back({4+column,1,RHI::Format::R32G32B32A32_FLOAT,column*16});
    }
	const RHI::ColorAttachmentDesc colorAttachment = {
		colorFormat,
		{ true, RHI::BlendFactor::SourceAlpha, RHI::BlendFactor::OneMinusSourceAlpha, RHI::BlendOp::Add,
			RHI::BlendFactor::One, compositeAlpha ? RHI::BlendFactor::OneMinusSourceAlpha : RHI::BlendFactor::Zero, RHI::BlendOp::Add },
		RHI::ColorWriteMask::All
	};

	RHI::GraphicsPipelineDesc desc = {};
	desc.vertexShader = vertexShader;
	desc.fragmentShader = fragmentShader;
	desc.topology = RHI::PrimitiveTopology::TriangleList;
	desc.vertexBuffers = vertexBuffers.data();
	desc.vertexBufferCount = static_cast<uint32_t>(vertexBuffers.size());
	desc.vertexAttributes = attributes.data();
	desc.vertexAttributeCount = static_cast<uint32_t>(attributes.size());
	desc.raster = { RHI::FillMode::Solid, RHI::CullMode::Back, RHI::FrontFace::CounterClockwise, 0.0f, 0.0f, 0.0f };
	desc.depthStencil.format = RHI::Format::D32_FLOAT;
	desc.depthStencil.depthTestEnabled = true;
	desc.depthStencil.depthWriteEnabled = true;
	desc.depthStencil.depthCompareOp = RHI::CompareOp::Less;
	desc.colorAttachments = &colorAttachment;
	desc.colorAttachmentCount = 1;
    std::vector<RHI::ResourceBindingLayout> bindings;
    desc.layout=MeshLayout(UsesBindlessMaterials(),bindings);
    pipeline = device->CreateGraphicsPipeline(desc);
	if(pipeline == nullptr) return false;

	if(!config.lighting.enabled || !config.lighting.shadows || shadowPipeline) return true;
	if(shadowVertexShader == nullptr) return false;

	std::vector<RHI::ResourceBindingLayout> shadowBindings = {{
		{ 3u, RHI::ResourceBindingType::ConstantBuffer, 1, RHI::ShaderStageFlags::Vertex, {} },
	}};
    shadowBindings.insert(shadowBindings.end(), shaderSources.vertexBindings.begin(), shaderSources.vertexBindings.end());
	RHI::GraphicsPipelineDesc shadowDesc = {};
	shadowDesc.vertexShader = shadowVertexShader;
	shadowDesc.topology = RHI::PrimitiveTopology::TriangleList;
	shadowDesc.vertexBuffers = &vertexBuffer;
	shadowDesc.vertexBufferCount = 1;
	shadowDesc.vertexAttributes = vertexAttributes.data();
	shadowDesc.vertexAttributeCount = static_cast<uint32_t>(vertexAttributes.size());
	shadowDesc.raster = {
		RHI::FillMode::Solid,
		RHI::CullMode::None,
		RHI::FrontFace::CounterClockwise,
		0.0f,
		1.75f,
		0.0f
	};
	shadowDesc.depthStencil.format = RHI::Format::D32_FLOAT;
	shadowDesc.depthStencil.depthTestEnabled = true;
	shadowDesc.depthStencil.depthWriteEnabled = true;
	shadowDesc.depthStencil.depthCompareOp = RHI::CompareOp::Less;
	shadowDesc.layout = {
		shadowBindings.data(),
		static_cast<uint32_t>(shadowBindings.size()),
		static_cast<uint32_t>(sizeof(DrawConstants)) + shaderSources.additionalConstantBytes,
		RHI::ShaderStageFlags::Vertex,
		10u
	};
	shadowPipeline = device->CreateGraphicsPipeline(shadowDesc);
	return shadowPipeline != nullptr;
}

bool Renderer::CreateDefaultMaterialTextures(RHI::IDevice* device)
{
	if(device == nullptr) return false;
	// Publish the complete, uploaded fallback set together. Until then a retry
	// must not mistake a partially created first texture for a prepared cache.
	struct PendingTextures
	{
		RHI::IDevice& device;
		std::array<RHI::TextureHandle,3> textures={};
		bool committed=false;
		~PendingTextures()
		{
			if(!committed) for(auto* texture:textures) device.DestroyTexture(texture);
		}
	} pending{*device};
	RHI::ResourceScope resources(*device);
	const std::array<std::array<uint8_t, 4>, 3> pixels = {{
		{{ 255, 255, 255, 255 }},
		{{ 128, 128, 255, 255 }},
		{{ 0, 0, 0, 255 }}
	}};

	RHI::TextureDesc desc = {};
	desc.width = 1;
	desc.height = 1;
	desc.format = RHI::Format::R8G8B8A8_UNORM;
	desc.usage = RHI::TextureUsage::ShaderResource;
	for(RHI::TextureHandle& texture : pending.textures)
	{
		texture = device->CreateTexture(desc);
		if(texture == nullptr) return false;
	}

	RHI::ICommandList* commandList = device->AcquireCommandList();
	if(commandList == nullptr) return false;
	resources.Keep(commandList);
	std::array<RHI::ResourceBarrierDesc, 3> beforeCopy = {};
	std::array<RHI::ResourceBarrierDesc, 3> barriers = {};
	uint32_t barrierCount = 0;
	for(uint32_t index = 0; index < pending.textures.size(); ++index)
	{
		beforeCopy[index].texture = pending.textures[index];
		beforeCopy[index].after = RHI::ResourceState::CopyDestination;
	}
	commandList->ResourceBarrier(beforeCopy.data(), static_cast<uint32_t>(beforeCopy.size()));
	for(uint32_t index = 0; index < pending.textures.size(); ++index)
	{
		if(!device->UpdateTexture(
				*commandList,
				pending.textures[index],
				0,
				0,
				pixels[index].data(),
				static_cast<uint32_t>(pixels[index].size()),
				4,
				4))
		{
			return false;
		}
		barriers[barrierCount].texture = pending.textures[index];
		barriers[barrierCount].before = RHI::ResourceState::CopyDestination;
		barriers[barrierCount].after = RHI::ResourceState::ShaderResource;
		++barrierCount;
	}
	if(barrierCount != 0) commandList->ResourceBarrier(barriers.data(), barrierCount);
	if(!commandList->Close() || !device->Submit(&commandList,1)) return false;
	defaultMaterialTextures=pending.textures;
	pending.committed=true;
	return true;
}

bool Renderer::EnsureDepthStencilTarget(RHI::IDevice* device,RHI::TextureHandle output)
{
	if(device == nullptr) return false;
	if(output == nullptr || output->GetDesc().width == 0u || output->GetDesc().height == 0u)
		return false;

	const bool recreate =
		depthStencilTarget == nullptr ||
		depthStencilTarget->GetDesc().width != output->GetDesc().width ||
		depthStencilTarget->GetDesc().height != output->GetDesc().height;

	if(!recreate) return true;

	if(depthStencilTarget != nullptr)
	{
		device->DestroyTexture(depthStencilTarget);
		depthStencilTarget = nullptr;
		depthStencilState = RHI::ResourceState::Undefined;
	}

	RHI::TextureDesc depthDesc = {};
	depthDesc.width = output->GetDesc().width;
	depthDesc.height = output->GetDesc().height;
	depthDesc.format = RHI::Format::D32_FLOAT;
	depthDesc.usage = RHI::TextureUsage::DepthStencil;
	depthStencilTarget = device->CreateTexture(depthDesc);
	return depthStencilTarget != nullptr;
}

bool Renderer::EnsureShadowDepthTarget(RHI::IDevice* device,uint32_t columns,uint32_t rows,uint32_t resolution)
{
	if(!config.lighting.enabled || !config.lighting.shadows) return true;
	if(device == nullptr) return false;
    const uint32_t width=resolution*columns;
    const uint32_t height=resolution*rows;
	if(resolution == 0) return false;
	if(shadowDepthTarget != nullptr &&
		shadowDepthTarget->GetDesc().width == width &&
		shadowDepthTarget->GetDesc().height == height)
	{
		return true;
	}

	if(shadowDepthTarget != nullptr)
	{
		device->DestroyTexture(shadowDepthTarget);
		shadowDepthTarget = nullptr;
		shadowDepthState = RHI::ResourceState::Undefined;
	}

	RHI::TextureDesc shadowDesc = {};
	shadowDesc.width = width;
	shadowDesc.height = height;
	shadowDesc.format = RHI::Format::D32_FLOAT;
	shadowDesc.usage = RHI::TextureUsage::DepthStencil | RHI::TextureUsage::ShaderResource;
	shadowDepthTarget = device->CreateTexture(shadowDesc);
	return shadowDepthTarget != nullptr;
}

void Renderer::UpdateMaterialStates(const Scene& scene)
{
	const uint32_t materialCount = scene.Materials().size();
	constexpr std::array<uint32_t,kMaterialTextureCount> defaults = {0u,0u,1u,0u,2u};
	for(uint32_t materialIndex = 0; materialIndex < materialCount; ++materialIndex)
	{
		const MaterialDesc& material = scene.Materials()[materialIndex];
		SceneMaterialState& materialState = materialStates[materialIndex];
		materialState.textures[ToIndex(MaterialTextureKind::BaseColor)] =
			ResolveTexture(material.baseColorTexture);
		materialState.textures[ToIndex(MaterialTextureKind::MetallicRoughness)] =
			ResolveTexture(material.metallicRoughnessTexture);
		materialState.textures[ToIndex(MaterialTextureKind::Normal)] =
			ResolveTexture(material.normalTexture);
		materialState.textures[ToIndex(MaterialTextureKind::Occlusion)] =
			ResolveTexture(material.occlusionTexture);
		materialState.textures[ToIndex(MaterialTextureKind::Emissive)] =
			ResolveTexture(material.emissiveTexture);

		materialState.textureFlags = 0u;
		for(uint32_t slot = 0; slot < kMaterialTextureCount; ++slot)
		{
			auto& texture = materialState.textures[slot];
			if(texture != nullptr)
				materialState.textureFlags |= 1u << slot;
			else
				texture = defaultMaterialTextures[defaults[slot]];
		}
	}
}

bool Renderer::UpdateLightingBuffer(
	const Scene& scene,
	const Camera& camera,
	RHI::IDevice* device,
	RHI::ICommandList& commandList)
{
	if(lightingBuffer && lightingBuffer->GetDesc().size!=sizeof(RendererLightingConstants)) {lightingBuffer=nullptr;lightingReady=false;}
	if(!lightingBuffer) lightingBuffer = device->CreateBuffer(RHI::BufferDesc{
		static_cast<uint32_t>(sizeof(RendererLightingConstants)),
		static_cast<uint32_t>(sizeof(RendererLightingConstants)),
		RHI::BufferUsage::Constant,
		RHI::ResourceState::CopyDestination
	});
	if(lightingBuffer == nullptr) return false;

    // Retain capacity and reproduce stable priority ordering without stable_sort scratch.
    const auto selectIndices=[&](const auto& lights,uint32_t capacity,uint32_t slot)->const std::vector<uint32_t>& {
        auto& indices=lightingIndexScratch[slot];indices.clear();
        indices.reserve(lights.size());
        for(uint32_t i=0;i<lights.size();++i)if(lights[i].enabled)indices.push_back(i);
        std::sort(indices.begin(),indices.end(),[&](uint32_t a,uint32_t b) {
            return lights[a].priority==lights[b].priority ? a<b : lights[a].priority>lights[b].priority;
        });
        if(indices.size()>capacity)indices.resize(capacity);
        return indices;
    };
	RendererLightingConstants constants = {};
	const auto& directionalIndices=selectIndices(scene.DirectionalLights(),static_cast<uint32_t>(std::size(constants.directionalLights)),0);
	const auto& pointIndices=selectIndices(scene.PointLights(),static_cast<uint32_t>(std::size(constants.pointLights)),1);
	const DirectionalLight* light = directionalIndices.empty() ? nullptr : &scene.DirectionalLights()[directionalIndices.front()];
	const PointLight* pointLight = pointIndices.empty() ? nullptr : &scene.PointLights()[pointIndices.front()];
	const Math::float3 lightDirection = light != nullptr
		? light->direction
		: Math::float3(0.0f, 0.0f, 1.0f);
	const Math::float3 lightColor = light != nullptr
		? light->color
		: Math::float3(0.0f, 0.0f, 0.0f);
	const float lightIntensity = light != nullptr ? light->intensity : 0.0f;
	const bool shadowsEnabled = shadowPipeline != nullptr &&
		light != nullptr && light->castShadow;
	const float shadowStrength = shadowsEnabled ? light->shadowStrength : 0.0f;

	constants.cameraPosition = Math::float4(
		camera.position.x,
		camera.position.y,
		camera.position.z,
		shadowsEnabled ? shadowStrength : 0.0f);
	constants.directionalLightDirection = Math::float4(
		lightDirection.x,
		lightDirection.y,
		lightDirection.z,
		shadowsEnabled ? 1.0f : 0.0f);
	constants.directionalLightColor = Math::float4(lightColor.x, lightColor.y, lightColor.z, lightIntensity);
	constants.ambientColor = Math::float4(
		config.lighting.ambientColor.x * config.lighting.environment.diffuseColor.x,
		config.lighting.ambientColor.y * config.lighting.environment.diffuseColor.y,
		config.lighting.ambientColor.z * config.lighting.environment.diffuseColor.z,
		config.lighting.ambientIntensity * config.lighting.environment.diffuseIntensity);
	constants.shadowParams = Math::float4(
		0.0007f,
		0.003f,
		0.0f,
		config.lighting.shadowQuality == ShadowQuality::High ? 2.0f : 1.0f);
	constants.pbrParams = Math::float4(
		0.04f,
		0.25f,
		config.enableHdrRendering ? -1.0f : (RHI::IsSrgbFormat(meshColorFormat) ? 0.0f : 1.0f),
		config.exposure);
	constants.environmentColor = Math::float4(
		config.lighting.environment.specularColor.x,
		config.lighting.environment.specularColor.y,
		config.lighting.environment.specularColor.z,
		config.lighting.environment.specularIntensity);
	if(pointLight != nullptr)
	{
		constants.pointLightPositionRange = Math::float4(
			pointLight->position.x, pointLight->position.y, pointLight->position.z, pointLight->range);
		constants.pointLightColorIntensity = Math::float4(
			pointLight->color.x, pointLight->color.y, pointLight->color.z, pointLight->intensity);
	}

    // 기본 셰이더의 고정 배열 크기에 맞춰 활성 광원을 선택하고 GPU 형식으로 변환한다.
        const auto& spotIndices=selectIndices(scene.SpotLights(),static_cast<uint32_t>(std::size(constants.spotLights)),2);
        const auto& rectAreaIndices=selectIndices(scene.RectAreaLights(),static_cast<uint32_t>(std::size(constants.rectAreaLights)),3);
        const auto& discAreaIndices=selectIndices(scene.DiscAreaLights(),static_cast<uint32_t>(std::size(constants.discAreaLights)),4);
		const uint32_t directionalCount = static_cast<uint32_t>(directionalIndices.size());
		for(uint32_t index = 0u; index < directionalCount; ++index)
		{
			const DirectionalLight& light = scene.DirectionalLights()[directionalIndices[index]];
			const Math::float3 direction = NormalizeDirection(light.direction);
			constants.directionalLights[index] = {
				Math::float4(direction.x, direction.y, direction.z, std::max(light.intensity, 0.0f)),
				Math::float4(light.color.x, light.color.y, light.color.z, 0.0f)
			};
		}

		const uint32_t pointCount = static_cast<uint32_t>(pointIndices.size());
		for(uint32_t index = 0u; index < pointCount; ++index)
		{
			const PointLight& light = scene.PointLights()[pointIndices[index]];
			constants.pointLights[index] = {
				Math::float4(light.position.x, light.position.y, light.position.z, std::max(light.range, 0.0f)),
				Math::float4(light.color.x, light.color.y, light.color.z, std::max(light.intensity, 0.0f))
			};
		}

		const uint32_t spotCount = static_cast<uint32_t>(spotIndices.size());
		for(uint32_t index = 0u; index < spotCount; ++index)
		{
			const SpotLight& light = scene.SpotLights()[spotIndices[index]];
			// 기존 기본 셰이더 정책: 입력 각도를 정렬하고 89도까지 제한하여 cos 값으로 전달한다.
			const auto cone=EffectiveSpotCone(light.innerConeRadians,light.outerConeRadians);
			const Math::float3 direction = NormalizeDirection(light.direction);
			constants.spotLights[index] = {
				Math::float4(light.position.x, light.position.y, light.position.z, std::max(light.range, 0.0f)),
				Math::float4(direction.x, direction.y, direction.z, std::cos(cone.outer)),
				Math::float4(light.color.x, light.color.y, light.color.z, std::max(light.intensity, 0.0f)),
				Math::float4(std::cos(cone.inner), 0.0f, 0.0f, 0.0f)
			};
		}

		const uint32_t rectAreaCount = static_cast<uint32_t>(rectAreaIndices.size());
		for(uint32_t index = 0u; index < rectAreaCount; ++index)
		{
			const RectAreaLight& light = scene.RectAreaLights()[rectAreaIndices[index]];
			const Math::float3 direction = NormalizeDirection(light.direction);
			const Math::float3 up = OrthogonalUp(direction, light.up);
			constants.rectAreaLights[index] = {
				Math::float4(light.position.x, light.position.y, light.position.z, std::max(light.intensity, 0.0f)),
				Math::float4(direction.x, direction.y, direction.z, light.width),
				Math::float4(up.x, up.y, up.z, light.height),
				Math::float4(light.color.x, light.color.y, light.color.z, 0.0f)
			};
		}

		const uint32_t discAreaCount = static_cast<uint32_t>(discAreaIndices.size());
		for(uint32_t index = 0u; index < discAreaCount; ++index)
		{
			const DiscAreaLight& light = scene.DiscAreaLights()[discAreaIndices[index]];
			const Math::float3 direction = NormalizeDirection(light.direction);
			const Math::float3 up = OrthogonalUp(direction, light.up);
			constants.discAreaLights[index] = {
				Math::float4(light.position.x, light.position.y, light.position.z, std::max(light.intensity, 0.0f)),
				Math::float4(direction.x, direction.y, direction.z, light.radius),
				Math::float4(up.x, up.y, up.z, 0.0f),
				Math::float4(light.color.x, light.color.y, light.color.z, 0.0f)
			};
		}

		constants.lightCounts = Math::float4(static_cast<float>(directionalCount), static_cast<float>(pointCount), static_cast<float>(spotCount), config.lighting.enabled ? 1.0f : 0.0f);
		constants.areaLightCounts = Math::float4(static_cast<float>(rectAreaCount), static_cast<float>(discAreaCount), 0.0f, 0.0f);

        // 조명을 끄면 모든 광원 평가를 생략한다. 재질의 기본색과 발광은 셰이더에서 유지한다.
        if(!config.lighting.enabled) { constants.lightCounts = {}; constants.areaLightCounts = {}; }

        constants.shadowLight = {1,0,constants.cameraPosition.w,constants.directionalLightDirection.w};

    const auto* first=reinterpret_cast<const uint8_t*>(&constants);
    pendingLightingBytes.assign(first,first+sizeof(constants));
    if(lightingReady && pendingLightingBytes==lightingBytes)return true;
	if(lightingReady) { const RHI::ResourceBarrierDesc barrier={lightingBuffer,nullptr,RHI::ResourceState::ConstantBuffer,RHI::ResourceState::CopyDestination,{}}; commandList.ResourceBarrier(&barrier,1); }
	if(!device->UpdateBuffer(
			commandList,
			lightingBuffer,
			0,
			&constants,
			static_cast<uint32_t>(sizeof(constants))))
		return false;
	const RHI::ResourceBarrierDesc after = {
		lightingBuffer, nullptr,
		RHI::ResourceState::CopyDestination,
		RHI::ResourceState::ConstantBuffer,
		{}
	};
	commandList.ResourceBarrier(&after, 1);
	return true;
}

bool Renderer::UpdateShadowBuffer(
	RHI::IDevice* device,
	RHI::ICommandList& commandList,const ShadowData& shadows)
{
	if(!config.lighting.enabled || !config.lighting.shadows) return true;
	if(shadowMatrixBuffer && shadowMatrixBuffer->GetDesc().size!=sizeof(RendererShadowConstants)) {shadowMatrixBuffer=nullptr;shadowReady=false;}
	if(!shadowMatrixBuffer) shadowMatrixBuffer = device->CreateBuffer(RHI::BufferDesc{
		static_cast<uint32_t>(sizeof(RendererShadowConstants)),
		static_cast<uint32_t>(sizeof(RendererShadowConstants)),
		RHI::BufferUsage::Constant,
		RHI::ResourceState::CopyDestination
	});
	if(shadowMatrixBuffer == nullptr) return false;

    const auto& shadow=shadows.constants;
    const auto* first=reinterpret_cast<const uint8_t*>(&shadow);
    pendingShadowBytes.assign(first,first+sizeof(shadow));
    if(shadowReady && pendingShadowBytes==shadowBytes)return true;
    if(shadowReady) { const RHI::ResourceBarrierDesc barrier={shadowMatrixBuffer,nullptr,RHI::ResourceState::ConstantBuffer,RHI::ResourceState::CopyDestination,{}}; commandList.ResourceBarrier(&barrier,1); }

	if(!device->UpdateBuffer(
			commandList,
			shadowMatrixBuffer,
			0,
			&shadow,
			static_cast<uint32_t>(sizeof(shadow))))
		return false;
	const RHI::ResourceBarrierDesc after = {
		shadowMatrixBuffer, nullptr,
		RHI::ResourceState::CopyDestination,
		RHI::ResourceState::ConstantBuffer,
		{}
	};
	commandList.ResourceBarrier(&after, 1);
	return true;
}

// RHI의 행 간격과 BGRA 출력을 Image의 연속 RGBA 픽셀로 변환한다.
bool Renderer::CaptureFrame(Image& image)
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
