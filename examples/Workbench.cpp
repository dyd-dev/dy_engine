#include "ExampleHost.h"
#include "ExampleCatalog.h"
#include "SceneAssets.h"
#include "../showcase/PerformanceComparison.h"
#include "../showcase/cinematic/KnightMaterials.h"
#include "dyf/Camera.h"
#include "dyf/Extends/AdvancedRendering.h"
#include "dyf/Extends/ImGui/ImGui.h"
#include "dyf/Extends/Model/Model.h"
#include "dyf/Extends/Model/ModelScene.h"
#include "dyf/Extends/Model/ModelRenderer.h"
#include "dyf/Extends/RenderDoc.h"
#include "dyf/Extends/Tracy.h"
#include "dyf/Platform/Window.h"
#include "dyf/RHI/IDevice.h"
#include "dyf/RHI/ICommandList.h"
#include "dyf/RHI/Texture.h"
#include "dyf/Renderer.h"
#include "dyf/Scene.h"
#include <imgui.h>
#include <imgui_internal.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <iomanip>
#include <iterator>
#include <locale>
#include <memory>
#include <sstream>
#include <stdexcept>

#ifdef __APPLE__
#include <TargetConditionals.h>
#endif

namespace dy_examples
{
// This survives module switches. It holds CPU inputs and session UI state only;
// renderer, device, GUI, viewport textures and deformation buffers never live here.
struct WorkbenchState
{
    struct CaptureTotals
    {
        uint64_t frames=0;
        double host=0, modelPrepare=0, modelCall=0, animation=0;
        void Add(double hostMs,double prepareMs,double callMs,double animationMs)
        { ++frames; host+=hostMs; modelPrepare+=prepareMs; modelCall+=callMs; animation+=animationMs; }
        double Mean(double total) const { return frames?total/double(frames):0; }
    };
    struct AssetPresence { bool sourceExists=false, modelAvailable=false; };
    dyf::RendererConfig config;
    dyf::ModelOptimizationConfig modelPolicies;
    std::filesystem::path assetRoot, repositoryRoot;
    std::shared_ptr<dyf::ModelScene> scene;
    std::vector<dyf::ModelInstanceID> instances;
    std::string message, sceneName="Material and shadow baseline", selectedExampleName, layout;
    const SceneAsset* selectedAsset=nullptr;
    char assetDirectory[1024]{}, modelPath[2048]{}, filter[128]{}, exampleArguments[1024]{};
    bool paused=false, repeat=true, turntable=false, controlsChanged=false, resetLayout=true;
    bool showAssets=true, showExamples=true, showControls=true, showAnimation=true, showPerformance=true;
    float speed=1, time=0, yaw=-1.05f, pitch=.25f, distance=9, fov=55;
    float target[3]={0,0,0};
    float normalizedSize=8, exposure=1, localSourceRadius=0, directionalAngularRadiusDegrees=.266f;
    int skinning=0, copies=1, category=0, resolution=0;
#if defined(__ANDROID__) || (defined(__APPLE__) && TARGET_OS_IPHONE)
    int importBudgetMiB=128;
#else
    int importBudgetMiB=2048;
#endif
    std::array<AssetPresence,std::size(sceneAssets)> assetPresence{};
    dyf::Camera camera;
    double frameMs=0, hostLoopMs=0, animationMs=0, modelCallMs=0, loadMs=0;
    std::array<float,240> frameHistory{};
    size_t historyCursor=0;
    showcase::PerformanceComparison comparison;
    CaptureTotals currentHostCapture, baselineHostCapture, candidateHostCapture;
    int comparisonAxis=0, requestedCapture=0;
    std::string baselineChoice, candidateChoice, contentSignature="built-in material-and-shadow scene v1";
    uint64_t sceneRevision=1;
};

namespace
{
namespace fs = std::filesystem;
using namespace dyf;
using Clock = std::chrono::steady_clock;
constexpr float Pi = 3.14159265358979323846f;
struct PackagedSceneAsset
{
    const char* originalPath;
    const char* relativePath;
};
constexpr PackagedSceneAsset PackagedSceneAssets[] = {
    {"",""},
#include "PackagedSceneAssets.inc"
};
#if defined(__ANDROID__) || (defined(__APPLE__) && TARGET_OS_IPHONE)
constexpr int MaxActorCopies = 4;
constexpr uint32_t TextureDecodeWorkers = 2;
#else
constexpr int MaxActorCopies = 64;
constexpr uint32_t TextureDecodeWorkers = 4;
#endif
#if defined(__ANDROID__)
constexpr const char* PlatformName="Android";
#elif defined(__APPLE__) && TARGET_OS_IPHONE
constexpr const char* PlatformName="iOS";
#elif defined(__APPLE__)
constexpr const char* PlatformName="macOS";
#elif defined(_WIN32)
constexpr const char* PlatformName="Windows";
#else
constexpr const char* PlatformName="Linux";
#endif
#ifdef DY_EXAMPLES_BACKEND
constexpr const char* BackendName=DY_EXAMPLES_BACKEND;
#else
constexpr const char* BackendName="Configured backend";
#endif

enum class ComparisonAxis
{
    Deformation, ParallelRecording, Instancing, CpuCulling, GpuCulling, Occlusion,
    ModelBuffers, ModelUploads, GeometryCache, InfluenceCache, TransformUploads,
    ResourceSetCache, BindlessMaterials, ClusteredLighting, StaticShadows
};
constexpr const char* ComparisonAxes[] = {
    "Deformation execution", "Parallel recording", "Instancing", "CPU frustum culling",
    "GPU frustum culling", "Hi-Z occlusion", "Deformation buffer reuse", "Unchanged model uploads",
    "Model geometry cache", "Skin influence cache", "Incremental transform uploads",
    "Resource set cache", "Bindless materials", "Clustered lighting", "Static shadow cache"
};

struct Viewport
{
    RHI::IDevice& device;
    Gui& gui;
    RHI::TextureHandle texture = nullptr;
    RHI::ResourceState state = RHI::ResourceState::Undefined;
    uint64_t id = 0;
    ~Viewport() { if(id) gui.UnregisterTexture(id); if(texture) device.DestroyTexture(texture); }
    bool Resize(uint32_t width, uint32_t height)
    {
        if(texture && texture->GetDesc().width == width && texture->GetDesc().height == height) return true;
        RHI::TextureDesc desc;
        desc.width=width; desc.height=height; desc.format=RHI::Format::B8G8R8A8_UNORM_SRGB;
        desc.usage=RHI::TextureUsage::RenderTarget|RHI::TextureUsage::ShaderResource;
        auto* next=device.CreateTexture(desc);
        if(!next) return false;
        const auto nextId=gui.RegisterTexture(next);
        if(!nextId) { device.DestroyTexture(next); return false; }
        if(id) gui.UnregisterTexture(id);
        if(texture) device.DestroyTexture(texture);
        texture=next; id=nextId; state=RHI::ResourceState::Undefined;
        return true;
    }
};

struct LoadedScene
{
    std::shared_ptr<ModelScene> scene;
    std::vector<ModelInstanceID> instances;
    HdrImage environment;
    std::string error, name, contentSignature;
};

const std::vector<AnimationClip>& AnimationClips(const ModelScene& scene,const ModelInstance& instance)
{
    const auto* asset=scene.TryGetModelAsset(instance.assetId);
    return asset?asset->animations:instance.clips;
}

std::vector<std::string> Arguments(const char* text)
{
    // Arguments go straight to argv, never through a shell.
    std::vector<std::string> result;
    std::string value;
    char quote=0;
    bool started=false;
    for(const char* p=text;;++p)
    {
        const char c=*p;
        if(!c) { if(quote) throw std::runtime_error("Unclosed argument quote."); if(started) result.push_back(value); break; }
        if(quote) { if(c==quote) quote=0; else value+=c; started=true; }
        else if(c=='\'' || c=='"') { quote=c; started=true; }
        else if(c==' ' || c=='\t') { if(started) { result.push_back(value); value.clear(); started=false; } }
        else { value+=c; started=true; }
    }
    return result;
}

class Workspace : public WorkbenchState
{
public:
    Workspace(Renderer& r, Gui& g, fs::path assets, fs::path repository,
        std::shared_ptr<WorkbenchState> memory)
        : renderer(r), gui(g), model(r), savedState(std::move(memory))
    {
        const bool restoring=savedState && savedState->scene;
        try
        {
            if(restoring)
            {
                static_cast<WorkbenchState&>(*this)=std::move(*savedState);
                repositoryRoot=std::move(repository);
                skinning=std::clamp(skinning,0,2);
                if(!model.SetSkinningExecutionMode(SkinningExecutionMode(skinning)))
                    skinning=int(model.GetSkinningExecutionMode());
                model.SetOptimizationConfig(modelPolicies);
                frameMs=hostLoopMs=animationMs=modelCallMs=0;
                comparison.Cancel("workbench renderer recreated"); requestedCapture=0;
                if(!layout.empty()) ImGui::LoadIniSettingsFromMemory(layout.data(),layout.size());
                resetLayout=layout.empty();
            }
            else
            {
                assetRoot=std::move(assets); repositoryRoot=std::move(repository);
                std::snprintf(assetDirectory,sizeof(assetDirectory),"%s",assetRoot.u8string().c_str());
                config=renderer.GetConfig();
                skinning=int(model.GetSkinningExecutionMode());
                scene=std::make_shared<ModelScene>();
                MaterialDesc red; red.baseColor={.8f,.18f,.07f,1}; red.roughnessFactor=.35f;
                auto box=scene->Add(CreateCubeMesh(2),red);
                MaterialDesc ground; ground.baseColor={.23f,.26f,.3f,1}; ground.roughnessFactor=.8f;
                auto floor=scene->Add(CreateCubeMesh(1),ground,Math::Translation({0,0,-1.2f})*Math::Scaling({12,12,.2f}));
                if(!box || !floor) throw std::runtime_error("Cannot initialize the scene.");
                AddLight(*scene);
                message="Ready. Choose a local asset, or load an original model package.";
            }
            RefreshAssets();
        }
        catch(...)
        {
            if(restoring) *savedState=std::move(static_cast<WorkbenchState&>(*this));
            throw;
        }
    }
    ~Workspace()
    {
        comparison.Cancel("workbench suspended"); requestedCapture=0;
        modelPolicies=model.GetOptimizationConfig();
        // Dear ImGui owns this returned buffer. Copy it before the GUI context
        // is destroyed; IniFilename remains null, so no settings file is written.
        try
        {
            size_t size=0;
            if(const auto* settings=ImGui::SaveIniSettingsToMemory(&size)) layout.assign(settings,size);
        }
        catch(...) { /* Retain the previous in-memory layout if allocation failed. */ }
        if(savedState) *savedState=std::move(static_cast<WorkbenchState&>(*this));
    }
    Renderer& renderer;
    Gui& gui;
    ModelRenderer model;
    std::shared_ptr<WorkbenchState> savedState;
    std::future<LoadedScene> loading;
    Clock::time_point loadStarted;
    struct PerformanceReadout
    {
        CaptureTotals totals;
        uint64_t sceneFrames=0;
        double frameMs=0,skinningGpuMs=-1;
        uint32_t entities=0;
        Platform::ProfilerTimingSnapshot timing;
        RendererStatistics stats;
        GuiStats ui;
        bool ready=false;
        double SceneMean(double total) const { return sceneFrames?total/double(sceneFrames):0; }
    } performanceReadout;
    CaptureTotals pendingReadout;
    double pendingFrameMs=0;
    uint64_t pendingSceneFrames=0;
    Clock::time_point readoutPublishedAt=Clock::now();
    const Example* pendingExample=nullptr;
    std::vector<std::string> pendingArguments;

    bool Busy() const { return loading.valid(); }
    static void AddLight(ModelScene& s)
    {
        if(s.DirectionalLights().empty())
        {
            DirectionalLight sun; sun.direction={.4f,-.6f,1}; sun.intensity=3;
            if(!s.Add(sun)) throw std::runtime_error("Cannot add the preview light.");
        }
    }
    fs::path ResolveAsset(const fs::path& base,const fs::path& relative) const
    {
        const auto path=base/relative;
        std::error_code ec;
        if(fs::is_regular_file(path,ec)) return path;
        const auto suffix=relative.lexically_normal().generic_u8string();
        for(const auto& packaged:PackagedSceneAssets)
        {
            const std::string_view original(packaged.originalPath);
            if(suffix.empty() || original.size()<suffix.size() ||
                original.compare(original.size()-suffix.size(),suffix.size(),suffix)!=0) continue;
            if(original.size()!=suffix.size() && original[original.size()-suffix.size()-1]!='/') continue;
            const auto installed=repositoryRoot/fs::u8path(packaged.relativePath);
            if(fs::is_regular_file(installed,ec)) return installed;
        }
        return path;
    }
    fs::path Path(const SceneAsset& asset) const
    {
        const auto path=ResolveAsset(asset.root==SceneAssetRoot::External?assetRoot:repositoryRoot,fs::u8path(asset.relativePath));
        std::error_code ec;
        if(fs::is_regular_file(path,ec)) return path;
        // Native application packages organize repository assets by example target.
        if(std::string_view(asset.id)=="fox") return repositoryRoot/"DyfModelAnim/Models/Fox.glb";
        if(std::string_view(asset.id)=="shiba") return repositoryRoot/"DyfModelAnim/Models/shiba/scene.FBX";
        return path;
    }
    void RefreshAssets()
    {
        for(size_t i=0;i<std::size(sceneAssets);++i)
        {
            std::error_code ec;
            assetPresence[i].sourceExists=fs::is_regular_file(Path(sceneAssets[i]),ec);
            assetPresence[i].modelAvailable=assetPresence[i].sourceExists && CatalogCanLoadModel(sceneAssets[i]);
        }
    }
    bool Available(const SceneAsset& asset) const
    {
        for(size_t i=0;i<std::size(sceneAssets);++i) if(&sceneAssets[i]==&asset) return assetPresence[i].modelAvailable;
        return false;
    }
    void ApplyConfig()
    {
        if(!controlsChanged) return;
        if(!renderer.SetConfig(config)) { message="Renderer rejected the selected settings."; config=renderer.GetConfig(); }
        controlsChanged=false;
    }
    void ApplyAnimationControls()
    {
        for(auto id:instances)
        {
            if(AnimationClips(*scene,scene->GetModelInstance(id)).empty()) continue;
            if(!scene->SetAnimationSpeed(id,speed) || !scene->SetAnimationLoop(id,repeat) || !scene->SetAnimationPaused(id,paused))
                message="Animation settings could not be applied; see stderr.";
        }
    }
    void ClearComparison()
    {
        comparison.Clear(); requestedCapture=0;
        currentHostCapture={}; baselineHostCapture={}; candidateHostCapture={};
        baselineChoice.clear(); candidateChoice.clear();
    }
    std::string ComparisonChoice() const
    {
        const auto axis=ComparisonAxis(comparisonAxis);
        if(axis==ComparisonAxis::Deformation)
        {
            switch(model.GetSkinningExecutionMode())
            {
            case SkinningExecutionMode::VertexShader: return "Vertex shader request (advanced raster may pre-skin)";
            case SkinningExecutionMode::ComputePreSkin: return "GPU compute pre-skin";
            case SkinningExecutionMode::Cpu: return "CPU deformation";
            }
        }
        const auto policy=model.GetOptimizationConfig();
        bool enabled=false;
        switch(axis)
        {
        case ComparisonAxis::ParallelRecording: enabled=config.enableParallelRenderGraph; break;
        case ComparisonAxis::Instancing: enabled=config.enableInstancing; break;
        case ComparisonAxis::CpuCulling: enabled=config.enableFrustumCulling; break;
        case ComparisonAxis::GpuCulling: enabled=config.enableGpuCulling; break;
        case ComparisonAxis::Occlusion: enabled=config.enableOcclusionCulling; break;
        case ComparisonAxis::ModelBuffers: enabled=policy.reuseFrameBuffers; break;
        case ComparisonAxis::ModelUploads: enabled=policy.skipUnchangedUploads; break;
        case ComparisonAxis::GeometryCache: enabled=policy.cacheStaticMeshes; break;
        case ComparisonAxis::InfluenceCache: enabled=policy.cacheInfluences; break;
        case ComparisonAxis::TransformUploads: enabled=config.enableIncrementalTransformUploads; break;
        case ComparisonAxis::ResourceSetCache: enabled=config.enableResourceSetCache; break;
        case ComparisonAxis::BindlessMaterials: enabled=config.enableBindlessMaterials; break;
        case ComparisonAxis::ClusteredLighting: enabled=config.lighting.clusteredLighting; break;
        case ComparisonAxis::StaticShadows: enabled=config.lighting.reuseStaticShadows; break;
        case ComparisonAxis::Deformation: break;
        }
        return std::string(enabled?"On":"Off")+(axis==ComparisonAxis::ParallelRecording?
            " / "+std::to_string(config.recordingThreadCount)+" recording workers":"");
    }
    std::string ComparisonConditions(uint32_t width,uint32_t height,uint32_t outputWidth,uint32_t outputHeight) const
    {
        std::ostringstream value;
        value.imbue(std::locale::classic());
        value<<std::hexfloat<<"scene="<<sceneRevision<<':'<<contentSignature<<";geometry="
            <<scene->GetEntityCount()<<','<<scene->Meshes().size()<<','<<scene->Materials().size()
            <<";size="<<width<<','<<height<<','<<outputWidth<<','<<outputHeight<<";axis="<<comparisonAxis;
        const auto vector3=[&](Math::float3 v) { value<<','<<v.x<<','<<v.y<<','<<v.z; };
        value<<";view"; for(float element:camera.view.m) value<<','<<element;
        value<<";projection"; for(float element:camera.projection.m) value<<','<<element;
        value<<";pose="<<time<<','<<paused<<','<<repeat<<','<<speed;
        for(auto id:instances)
        {
            const auto& instance=scene->GetModelInstance(id);
            value<<';'<<ToIndex(instance.assetId)<<','<<instance.playback.clipIndex<<','<<instance.playback.time
                <<','<<instance.playback.speed<<','<<instance.playback.playing<<','<<instance.playback.loop;
            for(float element:instance.rootTransform.m) value<<','<<element;
        }
        value<<";quality="<<config.vsync<<','<<config.allowReadback<<','<<config.enableHdrRendering
            <<','<<config.renderWidth<<','<<config.renderHeight<<','<<config.hdrRenderScale<<','<<config.exposure
            <<','<<config.enableProfilerHud<<','<<config.profilerStartsExpanded<<','<<config.texturePreparationThreadCount
            <<','<<config.enableLod<<','<<config.lodPixelError<<','<<config.lodHysteresis<<','<<config.debugLodColor
            <<','<<config.debugWireframe<<','<<config.clearColor.x<<','<<config.clearColor.y<<','<<config.clearColor.z<<','<<config.clearColor.w;
        const auto& light=config.lighting;
        value<<";lighting="<<light.enabled<<','<<light.direct<<','<<light.shadows<<','<<int(light.shadowQuality)
            <<','<<light.shadowDistance<<','<<light.cascadeSplitLambda<<','<<light.maxDirectionalShadowLights
            <<','<<light.maxPointShadowLights<<','<<light.maxSpotShadowLights<<','<<light.ambientIntensity
            <<','<<light.shadowBlockerSamples<<','<<light.shadowFilterSamples<<','<<light.shadowMaxFilterRadius;
        vector3(light.ambientColor);
        const auto& environment=light.environment;
        value<<";environment="<<environment.diffuse<<','<<environment.diffuseIntensity<<','<<environment.specular
            <<','<<environment.specularIntensity<<','<<environment.intensity<<','<<environment.rotationRadians
            <<','<<environment.background<<','<<environment.localProbe<<','<<environment.probes.size();
        vector3(environment.diffuseColor); vector3(environment.specularColor); vector3(environment.probePosition);
        vector3(environment.probeBoxMin); vector3(environment.probeBoxMax);
        for(const auto& probe:environment.probes)
        {
            vector3(probe.position); vector3(probe.boxMin); vector3(probe.boxMax);
            value<<','<<probe.fadeDistance<<','<<probe.intensity<<','<<probe.maxReflectionResolution;
        }
        const auto& advanced=config.advanced;
        value<<";advanced="<<advanced.enabled<<','<<advanced.ambientOcclusion<<','<<advanced.diffuseGI
            <<','<<advanced.reflections<<','<<advanced.temporalAA<<','<<advanced.bloom<<','<<advanced.transparency
            <<','<<int(advanced.surfaceDebugView)<<','<<advanced.aoRadius<<','<<advanced.aoStrength
            <<','<<advanced.rayDistance<<','<<advanced.giStrength<<','<<advanced.reflectionStrength<<','<<advanced.rayThickness
            <<','<<advanced.bloomStrength<<','<<advanced.bloomThreshold<<','<<advanced.bloomKnee
            <<','<<advanced.historyWeight<<','<<advanced.autoExposure<<','<<advanced.exposureKey
            <<','<<advanced.exposureAdaptationSpeed<<','<<advanced.transparencyLayers;
        const auto& fog=advanced.fog;
        value<<";fog="<<fog.enabled<<','<<fog.density<<','<<fog.heightFalloff<<','<<fog.baseHeight
            <<','<<fog.anisotropy<<','<<fog.maxDistance<<','<<fog.resolutionScale<<','<<fog.steps; vector3(fog.albedo);
        const auto& post=config.postProcess;
        value<<";post="<<post.temperature<<','<<post.tint<<','<<post.contrast<<','<<post.saturation
            <<','<<post.sharpening<<','<<post.sharpness; vector3(post.lift); vector3(post.gamma); vector3(post.gain);
        const auto axis=ComparisonAxis(comparisonAxis);
        const auto setting=[&](ComparisonAxis field,bool enabled) { if(axis!=field) value<<';'<<int(field)<<'='<<enabled; };
        if(axis!=ComparisonAxis::Deformation) value<<";deformation="<<int(model.GetSkinningExecutionMode());
        setting(ComparisonAxis::ParallelRecording,config.enableParallelRenderGraph);
        if(axis!=ComparisonAxis::ParallelRecording) value<<";workers="<<config.recordingThreadCount;
        setting(ComparisonAxis::Instancing,config.enableInstancing); setting(ComparisonAxis::CpuCulling,config.enableFrustumCulling);
        setting(ComparisonAxis::GpuCulling,config.enableGpuCulling); setting(ComparisonAxis::Occlusion,config.enableOcclusionCulling);
        const auto policy=model.GetOptimizationConfig();
        setting(ComparisonAxis::ModelBuffers,policy.reuseFrameBuffers); setting(ComparisonAxis::ModelUploads,policy.skipUnchangedUploads);
        setting(ComparisonAxis::GeometryCache,policy.cacheStaticMeshes); setting(ComparisonAxis::InfluenceCache,policy.cacheInfluences);
        setting(ComparisonAxis::TransformUploads,config.enableIncrementalTransformUploads);
        setting(ComparisonAxis::ResourceSetCache,config.enableResourceSetCache); setting(ComparisonAxis::BindlessMaterials,config.enableBindlessMaterials);
        setting(ComparisonAxis::ClusteredLighting,light.clusteredLighting); setting(ComparisonAxis::StaticShadows,light.reuseStaticShadows);
        value<<";canvas="<<config.enableCanvasBatching<<','<<config.enableCanvasImageCache<<','<<config.enableCanvasVertexBufferReuse;
        return value.str();
    }
    void PrepareComparison(const Viewport& viewport,bool visible,uint32_t outputWidth,uint32_t outputHeight)
    {
        if(!visible || Busy())
        {
            comparison.Cancel(visible?"asset import in progress":"Scene viewport is hidden");
            if(requestedCapture) message="Show a loaded Scene viewport before starting a comparison.";
            requestedCapture=0;
            return;
        }
        if(!comparison.IsRunning() && !requestedCapture) return;
        const auto size=viewport.texture->GetDesc();
        if(requestedCapture)
        {
            const bool candidate=requestedCapture==2;
            requestedCapture=0;
            paused=true; turntable=false; ApplyAnimationControls();
            const auto conditions=ComparisonConditions(size.width,size.height,outputWidth,outputHeight);
            if(comparison.Begin(candidate,conditions,ComparisonAxes[comparisonAxis]))
            {
                currentHostCapture={};
                if(candidate) candidateChoice=ComparisonChoice();
                else { baselineChoice=ComparisonChoice(); baselineHostCapture={}; candidateHostCapture={}; candidateChoice.clear(); }
                message="Comparison capture: frozen view and pose; warming up before collecting samples.";
            }
            else message="B requires A at the same scene, view, pose, output size and quality. Change only the selected comparison option.";
        }
        comparison.CheckConditions(ComparisonConditions(size.width,size.height,outputWidth,outputHeight));
    }
    void UpdatePerformanceReadout(double elapsedMilliseconds,bool submitted)
    {
        pendingReadout.Add(elapsedMilliseconds,submitted?model.GetCpuPrepareMilliseconds():0,
            submitted?modelCallMs:0,animationMs);
        pendingFrameMs+=frameMs;
        if(submitted) ++pendingSceneFrames;
        const auto now=Clock::now();
        if(now-readoutPublishedAt<std::chrono::seconds(1)) return;

        // Only the numerical UI is throttled. Graph samples and A/B captures
        // continue to use each completed frame and the original profiler data.
        performanceReadout.totals=pendingReadout;
        performanceReadout.sceneFrames=pendingSceneFrames;
        performanceReadout.frameMs=pendingReadout.Mean(pendingFrameMs);
        performanceReadout.skinningGpuMs=pendingSceneFrames?model.GetGpuMilliseconds():-1;
        performanceReadout.entities=scene->GetEntityCount();
        performanceReadout.timing=renderer.GetProfilerTimingSnapshot();
        performanceReadout.stats=renderer.GetLastRenderStatistics();
        performanceReadout.ui=gui.GetStats();
        performanceReadout.ready=true;
        pendingReadout={}; pendingFrameMs=0; pendingSceneFrames=0;
        readoutPublishedAt=now;
    }
    void CompleteFrame(double elapsedMilliseconds,bool submitted)
    {
        hostLoopMs=elapsedMilliseconds;
        UpdatePerformanceReadout(elapsedMilliseconds,submitted);
        if(!comparison.IsRunning()) return;
        const bool candidate=comparison.IsCandidateCapture();
        const auto phase=comparison.GetCapturePhase();
        comparison.Update(elapsedMilliseconds*.001,submitted,renderer.GetProfilerTimingSnapshot(),renderer.GetLastRenderStatistics());
        if(phase==showcase::PerformanceComparison::CapturePhase::Measuring && submitted)
            currentHostCapture.Add(elapsedMilliseconds,model.GetCpuPrepareMilliseconds(),modelCallMs,animationMs);
        if(!comparison.IsRunning())
        {
            if(candidate) candidateHostCapture=currentHostCapture;
            else baselineHostCapture=currentHostCapture;
            std::fprintf(stderr,"Workbench comparison %c: host loop %.3f ms, model Prepare %.3f ms, model+renderer call %.3f ms, animation %.3f ms (%llu submitted frames).\n",
                candidate?'B':'A',currentHostCapture.Mean(currentHostCapture.host),currentHostCapture.Mean(currentHostCapture.modelPrepare),
                currentHostCapture.Mean(currentHostCapture.modelCall),currentHostCapture.Mean(currentHostCapture.animation),
                static_cast<unsigned long long>(currentHostCapture.frames));
            message="Comparison capture complete. The pose remains paused; change only the selected comparison option for B.";
        }
    }
    void StartLoad(fs::path path, fs::path environment={}, std::string name={})
    {
        if(Busy() || comparison.IsRunning()) return;
        std::error_code pathError;
        if(!fs::is_regular_file(path,pathError)) { message="Model package is missing or unreadable: "+path.u8string(); return; }
        const float size=normalizedSize;
        const int count=std::clamp(copies,1,MaxActorCopies);
        const uint64_t budget=uint64_t(std::clamp(importBudgetMiB,16,65536))*1024*1024;
        const auto file=path.filename().u8string();
        const bool bistro=file.rfind("Bistro",0)==0;
        const bool emerald=file.rfind("EmeraldSquare",0)==0;
        message="Loading "+path.filename().u8string()+". The original buffers and textures must remain beside it.";
        loadStarted=Clock::now();
        ClearComparison();
        loading=std::async(std::launch::async,[path=std::move(path),environment=std::move(environment),name=std::move(name),size,count,budget,bistro,emerald]() {
            LoadedScene result;
            result.name=name.empty()?path.filename().u8string():name;
            try
            {
                result.contentSignature=path.lexically_normal().u8string();
                std::error_code metadataError;
                const auto bytes=fs::file_size(path,metadataError);
                if(!metadataError) result.contentSignature+=";source-bytes="+std::to_string(bytes);
                const auto modified=fs::last_write_time(path,metadataError);
                if(!metadataError) result.contentSignature+=";source-time="+std::to_string(modified.time_since_epoch().count());
                result.scene=std::make_shared<ModelScene>();
                ModelSceneDesc desc; desc.path=path.u8string(); desc.normalizedSize=size;
                desc.transform=Math::RotationX(Pi*.5f);
                if(path.filename()=="Knight_USD_002.fbx") showcase::ApplyIntelKnightMaterials(desc);
                desc.loadOptions.textureDecodeThreadCount=TextureDecodeWorkers;
                // Large source packages remain opt-in; do not decode them at launcher startup.
                desc.loadOptions.maxSourceBytes=budget;
                desc.loadOptions.maxParserInputBytes=desc.loadOptions.maxSourceBytes;
                desc.loadOptions.maxParserBytes=desc.loadOptions.maxSourceBytes;
                desc.loadOptions.maxDecodedBytes=desc.loadOptions.maxSourceBytes;
                desc.loadOptions.importLights=bistro||emerald;
                desc.loadOptions.fbxSpecularTextureIsOrm=bistro||emerald;
                desc.loadOptions.fbxBaseColorTextureHasOpacity=bistro||emerald;
                desc.loadOptions.normalMapYSign=bistro||emerald?-1.f:1.f;
                ModelAssetID asset;
                if(!LoadModelAsset(*result.scene,desc.path,desc.loadOptions,&asset)) throw std::runtime_error("Model import failed; see stderr.");
                for(int i=0;i<count;++i)
                {
                    if(count>1) desc.position={float(i%8)*size*1.1f,float(i/8)*size*1.1f,0};
                    ModelInstanceID instance;
                    if(!InstantiateModel(*result.scene,asset,desc,&instance)) throw std::runtime_error("Model instance creation failed.");
                    result.instances.push_back(instance);
                    if(!AnimationClips(*result.scene,result.scene->GetModelInstance(instance)).empty() && !result.scene->PlayAnimation(instance,0))
                        throw std::runtime_error("Animation initialization failed.");
                }
                AddLight(*result.scene);
                if(!environment.empty() && fs::is_regular_file(environment) && !LoadHdrImage(environment.u8string(),result.environment))
                    throw std::runtime_error("HDR environment import failed.");
            }
            catch(const std::exception& e) { result.error=e.what(); result.scene.reset(); }
            return result;
        });
    }
    void Tick(float delta)
    {
        if(Busy() && loading.wait_for(std::chrono::seconds(0))==std::future_status::ready)
        {
            auto result=loading.get();
            loadMs=std::chrono::duration<double,std::milli>(Clock::now()-loadStarted).count();
            if(!result.scene) message=result.error;
            else
            {
                scene=std::move(result.scene); instances=std::move(result.instances); sceneName=std::move(result.name);
                contentSignature=std::move(result.contentSignature); ++sceneRevision;
                config.lighting.environment.radiance=std::move(result.environment);
                config.lighting.environment.intensity=1;
                config.lighting.environment.background=config.lighting.environment.radiance.IsValid();
                config.lighting.ambientIntensity=config.lighting.environment.radiance.IsValid()?0.f:.08f;
                config.advanced.autoExposure=false;
                time=0; distance=normalizedSize*1.8f; std::fill(std::begin(target),std::end(target),0.f);
                model.InvalidateHistory(); renderer.InvalidateHistory(); controlsChanged=true;
                message="Loaded "+sceneName+". GPU geometry/texture preparation occurs on the first visible frame.";
                ApplyAnimationControls();
            }
        }
        const bool frozen=paused || comparison.IsRunning();
        if(!frozen) time+=delta*speed;
        if(turntable && !frozen) yaw+=delta*.18f;
        const auto begin=Clock::now();
        if(scene && !scene->UpdateAnimations(frozen?0.f:delta)) message="Animation update failed; see stderr.";
        animationMs=std::chrono::duration<double,std::milli>(Clock::now()-begin).count();
    }
    void Dock()
    {
        auto* viewport=ImGui::GetMainViewport();
        const auto dock=ImGui::GetID("WorkbenchDock");
        ImGui::DockSpaceOverViewport(dock,viewport,ImGuiDockNodeFlags_PassthruCentralNode);
        if(!resetLayout) return;
        resetLayout=false;
        // The same movable windows also work without docking on compact displays.
        ImGui::DockBuilderRemoveNode(dock);
        ImGui::DockBuilderAddNode(dock,ImGuiDockNodeFlags_DockSpace);
        ImGui::DockBuilderSetNodeSize(dock,viewport->WorkSize);
        ImGuiID center=dock, left=0,right=0,bottom=0;
        if(viewport->WorkSize.x>=850)
        {
            ImGui::DockBuilderSplitNode(center,ImGuiDir_Left,.23f,&left,&center);
            ImGui::DockBuilderSplitNode(center,ImGuiDir_Right,.31f,&right,&center);
            ImGui::DockBuilderSplitNode(center,ImGuiDir_Down,.28f,&bottom,&center);
        }
        else { left=center; right=center; bottom=center; }
        ImGui::DockBuilderDockWindow("Scene",center);
        ImGui::DockBuilderDockWindow("Assets",left);
        ImGui::DockBuilderDockWindow("Examples",left);
        ImGui::DockBuilderDockWindow("Rendering",right);
        ImGui::DockBuilderDockWindow("Animation and camera",right);
        ImGui::DockBuilderDockWindow("Performance",bottom);
        ImGui::DockBuilderFinish(dock);
    }
    void Menus()
    {
        if(!ImGui::BeginMainMenuBar()) return;
        if(ImGui::BeginMenu("Window"))
        {
            ImGui::MenuItem("Assets",nullptr,&showAssets); ImGui::MenuItem("Examples",nullptr,&showExamples);
            ImGui::MenuItem("Rendering",nullptr,&showControls); ImGui::MenuItem("Animation and camera",nullptr,&showAnimation);
            ImGui::MenuItem("Performance",nullptr,&showPerformance);
            if(ImGui::MenuItem("Reset layout")) resetLayout=true;
            ImGui::EndMenu();
        }
        ImGui::Text("%s",sceneName.c_str());
        ImGui::EndMainMenuBar();
    }
    void Assets()
    {
        if(!showAssets) return;
        if(ImGui::Begin("Assets",&showAssets))
        {
            ImGui::InputText("Asset directory",assetDirectory,sizeof(assetDirectory));
            if(ImGui::Button("Use directory")) { assetRoot=fs::u8path(assetDirectory); RefreshAssets(); }
            ImGui::SameLine();
            if(ImGui::Button("Refresh catalog")) RefreshAssets();
            ImGui::InputText("Filter",filter,sizeof(filter));
            ImGui::InputFloat("Model size",&normalizedSize,.5f,2.f);
            normalizedSize=std::clamp(normalizedSize,.1f,1000.f);
            ImGui::SliderInt("Actor instances",&copies,1,MaxActorCopies);
            ImGui::BeginDisabled(Busy());
            ImGui::InputInt("Import budget (MiB)",&importBudgetMiB,128,1024);
            importBudgetMiB=std::clamp(importBudgetMiB,16,65536);
            ImGui::EndDisabled();
            ImGui::TextWrapped("The budget applies separately to source, parser and decoded data for the next import. It is not a peak RAM limit. Image workers: %u.",TextureDecodeWorkers);
            ImGui::InputText("Model file",modelPath,sizeof(modelPath));
            ImGui::BeginDisabled(Busy() || comparison.IsRunning());
            if(ImGui::Button("Load file")) StartLoad(fs::u8path(modelPath));
            ImGui::EndDisabled();
            ImGui::SeparatorText("Scene and animation catalog");
            if(ImGui::BeginChild("catalog",{0,ImGui::GetContentRegionAvail().y*.56f},ImGuiChildFlags_Borders))
            for(size_t assetIndex=0;assetIndex<std::size(sceneAssets);++assetIndex)
            {
                const auto& asset=sceneAssets[assetIndex];
                if(*filter && std::string(asset.name).find(filter)==std::string::npos && std::string(asset.category).find(filter)==std::string::npos) continue;
                const bool available=assetPresence[assetIndex].modelAvailable;
                const bool sourceExists=assetPresence[assetIndex].sourceExists;
                const char* state=available?"[local] ":!sourceExists?"[missing] ":
                    asset.availability==SceneAssetAvailability::ConversionRequired?"[source] ":"[2D] ";
                const auto label=std::string(state)+asset.name;
                if(ImGui::Selectable(label.c_str(),selectedAsset==&asset)) selectedAsset=&asset;
            }
            ImGui::EndChild();
            if(selectedAsset)
            {
                ImGui::TextWrapped("%s",selectedAsset->workload);
                ImGui::TextWrapped("%s",selectedAsset->platformNotes);
                ImGui::TextWrapped("License: %s",selectedAsset->license);
                ImGui::TextWrapped("%s",AssetAvailabilityName(selectedAsset->availability));
                ImGui::TextWrapped("%s",selectedAsset->sourceUrl);
                ImGui::BeginDisabled(Busy() || comparison.IsRunning() || !Available(*selectedAsset));
                if(ImGui::Button("Load selected scene"))
                {
                    const auto base=selectedAsset->root==SceneAssetRoot::External?assetRoot:repositoryRoot;
                    StartLoad(Path(*selectedAsset),*selectedAsset->environmentPath?ResolveAsset(base,fs::u8path(selectedAsset->environmentPath)):fs::path{},selectedAsset->name);
                }
                ImGui::EndDisabled();
            }
            if(Busy()) ImGui::TextUnformatted("Importing original model package...");
            ImGui::TextWrapped("%s",message.c_str());
        }
        ImGui::End();
    }
    void Examples(const Example* entries,size_t count)
    {
        if(!showExamples) return;
        if(ImGui::Begin("Examples",&showExamples))
        {
            const char* categories[]={"All","dyf","RHI","advanced","showcase"};
            ImGui::Combo("Category",&category,categories,5);
            ImGui::TextWrapped("Choose a module, edit its startup controls, then Open. Every module uses this window and the common GUI; Back restores this scene and panel layout.");
            const Example* choice=nullptr;
            for(size_t i=0;i<count;++i) if(selectedExampleName==entries[i].name) choice=&entries[i];
            if(ImGui::BeginChild("example-list",{0,std::max(100.f,ImGui::GetContentRegionAvail().y*.42f)},ImGuiChildFlags_Borders))
            for(size_t i=0;i<count;++i)
            {
                const auto& entry=entries[i];
                if(example_catalog::IsRayTracing(entry.name) || (category && std::string_view(entry.category)!=categories[category])) continue;
                const auto* metadata=example_catalog::Find(entry.name);
                ImGui::PushID(int(i));
                if(ImGui::Selectable(entry.name,selectedExampleName==entry.name))
                { selectedExampleName=entry.name; choice=&entry; }
                if(metadata && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                {
                    ImGui::BeginTooltip(); ImGui::TextUnformatted(metadata->summary);
                    ImGui::TextWrapped("%s",metadata->requirements); ImGui::EndTooltip();
                }
                ImGui::PopID();
            }
            ImGui::EndChild();
            if(choice)
            {
                const auto* metadata=example_catalog::Find(choice->name);
                bool supported=!example_catalog::IsRayTracing(choice->name);
                if(metadata)
                {
                    supported&=metadata->status!=example_catalog::Status::Unimplemented;
                    for(unsigned f=0;f<metadata->requiredFeatureCount;++f)
                    {
                        // The material mode remains usable when only the array
                        // variant's descriptor-indexing requirement is absent.
                        if(std::string_view(choice->name)=="AdvancedBindlessMaterials" &&
                            metadata->requiredFeatures[f]==RHI::Feature::DescriptorIndexing) continue;
                        supported&=renderer.GetDevice().Supports(metadata->requiredFeatures[f]);
                    }
                    ImGui::TextWrapped("%s",metadata->summary);
                    ImGui::TextWrapped("%s",metadata->requirements);
                }
                ImGui::PushID(choice->name);
                EditExampleArguments(*choice);
                ImGui::PopID();
                ImGui::InputText("Extra arguments",exampleArguments,sizeof(exampleArguments));
                ImGui::BeginDisabled(!supported || Busy() || comparison.IsRunning());
                if(ImGui::Button("Open selected module",{-1,0}))
                {
                    try
                    {
                        pendingArguments=GetExampleArguments(*choice);
                        const auto extra=Arguments(exampleArguments);
                        pendingArguments.insert(pendingArguments.end(),extra.begin(),extra.end());
                        pendingExample=choice;
                    }
                    catch(const std::exception& e) { message=e.what(); }
                }
                ImGui::EndDisabled();
            }
        }
        ImGui::End();
    }
    void Rendering()
    {
        if(!showControls) { ApplyConfig(); return; }
        if(ImGui::Begin("Rendering",&showControls))
        {
            ImGui::BeginDisabled(comparison.IsRunning());
            auto toggle=[&](const char* label,bool& value) { controlsChanged|=ImGui::Checkbox(label,&value); };
            if(ImGui::CollapsingHeader("Execution and visibility",ImGuiTreeNodeFlags_DefaultOpen))
            {
                const char* skinModes[]={"Vertex shader (basic path)","GPU compute pre-skin","CPU skinning"};
                const bool computeSupported=renderer.GetDevice().Supports(RHI::Feature::Compute);
                if(ImGui::BeginCombo("Deformation",skinModes[skinning]))
                {
                    for(int mode=0;mode<3;++mode)
                    {
                        ImGui::BeginDisabled(mode==1 && !computeSupported);
                        if(ImGui::Selectable(skinModes[mode],skinning==mode))
                        {
                            if(model.SetSkinningExecutionMode(static_cast<SkinningExecutionMode>(mode))) skinning=mode;
                            else message="Selected deformation mode is unavailable.";
                            renderer.InvalidateHistory();
                        }
                        ImGui::EndDisabled();
                    }
                    ImGui::EndCombo();
                }
                ImGui::TextWrapped(computeSupported?
                    "Advanced surface rendering pre-skins on GPU for either GPU selection. CPU selection performs CPU deformation.":
                    "Compute is unavailable. Advanced surface rendering uses CPU deformation; the basic path can use vertex-shader skinning.");
                toggle("Parallel recording",config.enableParallelRenderGraph);
                int workers=int(config.recordingThreadCount);
                if(ImGui::SliderInt("Recording workers",&workers,1,16)) { config.recordingThreadCount=uint32_t(workers); controlsChanged=true; }
                toggle("CPU frustum culling",config.enableFrustumCulling); toggle("Instancing",config.enableInstancing);
                const bool gpuVisibilitySupported=computeSupported && renderer.GetDevice().Supports(RHI::Feature::IndirectDraw);
                const bool gpuVisibilityReady=gpuVisibilitySupported && config.enableInstancing && config.advanced.enabled && config.enableHdrRendering;
                ImGui::BeginDisabled(!gpuVisibilityReady);
                toggle("GPU frustum culling",config.enableGpuCulling);
                ImGui::EndDisabled();
                ImGui::BeginDisabled(!gpuVisibilityReady || !config.enableGpuCulling);
                toggle("Hi-Z occlusion",config.enableOcclusionCulling);
                ImGui::EndDisabled();
                if(!gpuVisibilitySupported) ImGui::TextDisabled("GPU visibility requires compute and indirect draws.");
                else if(!gpuVisibilityReady) ImGui::TextDisabled("GPU visibility requires instancing and advanced HDR rendering.");
                toggle("Indexed LOD",config.enableLod);
                controlsChanged|=ImGui::SliderFloat("LOD pixel error",&config.lodPixelError,.1f,8.f);
                controlsChanged|=ImGui::SliderFloat("LOD hysteresis",&config.lodHysteresis,0,.45f);
                toggle("LOD colors",config.debugLodColor);
                ImGui::BeginDisabled(!renderer.GetDevice().Supports(RHI::Feature::Wireframe));
                toggle("Wireframe",config.debugWireframe); ImGui::EndDisabled();
                auto policies=model.GetOptimizationConfig();
                bool changed=false;
                changed|=ImGui::Checkbox("Reuse deformation buffers",&policies.reuseFrameBuffers);
                changed|=ImGui::Checkbox("Skip unchanged model uploads",&policies.skipUnchangedUploads);
                changed|=ImGui::Checkbox("Cache model geometry",&policies.cacheStaticMeshes);
                changed|=ImGui::Checkbox("Cache skin influences",&policies.cacheInfluences);
                if(changed) model.SetOptimizationConfig(policies);
                toggle("Incremental transform uploads",config.enableIncrementalTransformUploads);
                toggle("Resource set cache",config.enableResourceSetCache);
                ImGui::BeginDisabled(config.advanced.enabled && config.enableHdrRendering);
                toggle("Bindless materials",config.enableBindlessMaterials);
                ImGui::EndDisabled();
                if(config.advanced.enabled && config.enableHdrRendering)
                    ImGui::TextDisabled("Advanced rendering uses standard material bindings.");
            }
            if(ImGui::CollapsingHeader("Lighting and shadows",ImGuiTreeNodeFlags_DefaultOpen))
            {
                toggle("Lighting",config.lighting.enabled); toggle("Direct lighting",config.lighting.direct);
                toggle("Shadows",config.lighting.shadows); toggle("Static shadow cache",config.lighting.reuseStaticShadows);
                int quality=int(config.lighting.shadowQuality);
                if(ImGui::Combo("Shadow quality",&quality,"Low\0Medium\0High\0")) { config.lighting.shadowQuality=ShadowQuality(quality); controlsChanged=true; }
                const auto budget=[&](const char* label,uint32_t& value,int maximum)
                { int count=int(value); if(ImGui::SliderInt(label,&count,0,maximum)) { value=uint32_t(count); controlsChanged=true; } };
                budget("Directional shadow lights",config.lighting.maxDirectionalShadowLights,4);
                budget("Point shadow lights",config.lighting.maxPointShadowLights,16);
                budget("Spot shadow lights",config.lighting.maxSpotShadowLights,16);
                controlsChanged|=ImGui::SliderFloat("Shadow distance",&config.lighting.shadowDistance,0,200);
                controlsChanged|=ImGui::SliderFloat("Cascade split",&config.lighting.cascadeSplitLambda,0,1);
                int search=int(config.lighting.shadowBlockerSamples), samples=int(config.lighting.shadowFilterSamples);
                if(ImGui::SliderInt("Blocker samples",&search,1,32)) { config.lighting.shadowBlockerSamples=uint32_t(search); controlsChanged=true; }
                if(ImGui::SliderInt("Filter samples",&samples,1,32)) { config.lighting.shadowFilterSamples=uint32_t(samples); controlsChanged=true; }
                controlsChanged|=ImGui::SliderFloat("Soft-shadow radius (0 = PCF)",&config.lighting.shadowMaxFilterRadius,0,32);
                ImGui::BeginDisabled(scene->PointLights().empty() && scene->SpotLights().empty());
                ImGui::SliderFloat("Point/spot source radius",&localSourceRadius,0,10);
                if(ImGui::Button("Apply point/spot source radius"))
                {
                    bool changed=false, success=true;
                    for(uint32_t i=0;i<scene->PointLights().size();++i)
                    {
                        auto handle=scene->GetLight<PointLight>(i); auto light=handle.Get();
                        if(light.sourceRadius==localSourceRadius) continue;
                        light.sourceRadius=localSourceRadius;
                        if(handle.Set(light)) changed=true; else success=false;
                    }
                    for(uint32_t i=0;i<scene->SpotLights().size();++i)
                    {
                        auto handle=scene->GetLight<SpotLight>(i); auto light=handle.Get();
                        if(light.sourceRadius==localSourceRadius) continue;
                        light.sourceRadius=localSourceRadius;
                        if(handle.Set(light)) changed=true; else success=false;
                    }
                    if(changed) { ++sceneRevision; renderer.InvalidateHistory(); }
                    if(!success) message="Some light source radii could not be changed; see stderr.";
                }
                ImGui::EndDisabled();
                ImGui::BeginDisabled(scene->DirectionalLights().empty());
                ImGui::SliderFloat("Directional radius (degrees)",&directionalAngularRadiusDegrees,0,5);
                if(ImGui::Button("Apply directional source radius"))
                {
                    const float radius=directionalAngularRadiusDegrees*Pi/180;
                    bool changed=false, success=true;
                    for(uint32_t i=0;i<scene->DirectionalLights().size();++i)
                    {
                        auto handle=scene->GetLight<DirectionalLight>(i); auto light=handle.Get();
                        if(light.angularRadius==radius) continue;
                        light.angularRadius=radius;
                        if(handle.Set(light)) changed=true; else success=false;
                    }
                    if(changed) { ++sceneRevision; renderer.InvalidateHistory(); }
                    if(!success) message="Some directional source radii could not be changed; see stderr.";
                }
                ImGui::EndDisabled();
                ImGui::TextWrapped("Apply sets every light of the indicated type. Point/spot radius uses normalized world units; directional radius uses degrees. Zero radius keeps fixed PCF.");
                toggle("Diffuse IBL",config.lighting.environment.diffuse); toggle("Specular IBL",config.lighting.environment.specular);
                toggle("Sky background",config.lighting.environment.background);
                const bool hasLocalLights=!scene->PointLights().empty() || !scene->SpotLights().empty() ||
                    !scene->RectAreaLights().empty() || !scene->DiscAreaLights().empty();
                const bool clusteredReady=renderer.GetDevice().Supports(RHI::Feature::Compute) &&
                    config.advanced.enabled && config.enableHdrRendering && hasLocalLights;
                ImGui::BeginDisabled(!clusteredReady);
                toggle("Clustered lighting",config.lighting.clusteredLighting);
                ImGui::EndDisabled();
                if(!clusteredReady) ImGui::TextDisabled("Clustering requires compute, advanced HDR rendering and local lights.");
                controlsChanged|=ImGui::SliderFloat("Ambient intensity",&config.lighting.ambientIntensity,0,1);
                controlsChanged|=ImGui::SliderFloat("Environment intensity",&config.lighting.environment.intensity,0,20);
            }
            if(ImGui::CollapsingHeader("Surface and post process",ImGuiTreeNodeFlags_DefaultOpen))
            {
                toggle("HDR rendering",config.enableHdrRendering); toggle("Advanced raster",config.advanced.enabled);
                toggle("SSAO",config.advanced.ambientOcclusion); toggle("SSGI",config.advanced.diffuseGI);
                toggle("SSR",config.advanced.reflections); toggle("Temporal AA",config.advanced.temporalAA);
                toggle("Bloom",config.advanced.bloom); toggle("Transparency",config.advanced.transparency);
                int layers=int(config.advanced.transparencyLayers);
                if(ImGui::SliderInt("Glass layers",&layers,1,2)) { config.advanced.transparencyLayers=uint32_t(layers); controlsChanged=true; }
                int debug=int(config.advanced.surfaceDebugView);
                if(ImGui::Combo("Surface view",&debug,"Off\0Base color\0Normal\0Roughness\0Metallic\0Depth\0Motion\0")) { config.advanced.surfaceDebugView=SurfaceDebugView(debug); controlsChanged=true; }
                controlsChanged|=ImGui::SliderFloat("AO radius",&config.advanced.aoRadius,.01f,8);
                controlsChanged|=ImGui::SliderFloat("AO strength",&config.advanced.aoStrength,0,3);
                controlsChanged|=ImGui::SliderFloat("Ray distance",&config.advanced.rayDistance,.1f,100);
                controlsChanged|=ImGui::SliderFloat("GI strength",&config.advanced.giStrength,0,3);
                controlsChanged|=ImGui::SliderFloat("Reflection strength",&config.advanced.reflectionStrength,0,3);
                controlsChanged|=ImGui::SliderFloat("History weight",&config.advanced.historyWeight,0,.99f);
                controlsChanged|=ImGui::SliderFloat("Bloom strength",&config.advanced.bloomStrength,0,.5f);
                toggle("Automatic exposure",config.advanced.autoExposure);
                controlsChanged|=ImGui::SliderFloat("Exposure",&config.exposure,.001f,8.f,"%.3f",ImGuiSliderFlags_Logarithmic);
                toggle("Sharpening",config.postProcess.sharpening);
                controlsChanged|=ImGui::SliderFloat("Sharpness",&config.postProcess.sharpness,0,1);
                controlsChanged|=ImGui::SliderFloat("Temperature",&config.postProcess.temperature,-1,1);
                controlsChanged|=ImGui::SliderFloat("Tint",&config.postProcess.tint,-1,1);
                controlsChanged|=ImGui::SliderFloat("Contrast",&config.postProcess.contrast,.1f,2);
                controlsChanged|=ImGui::SliderFloat("Saturation",&config.postProcess.saturation,0,2);
                controlsChanged|=ImGui::ColorEdit3("Lift",&config.postProcess.lift.x);
                controlsChanged|=ImGui::SliderFloat3("Gamma",&config.postProcess.gamma.x,.1f,3);
                controlsChanged|=ImGui::SliderFloat3("Gain",&config.postProcess.gain.x,0,3);
            }
            if(ImGui::CollapsingHeader("Fog"))
            {
                toggle("Volumetric fog",config.advanced.fog.enabled);
                controlsChanged|=ImGui::SliderFloat("Density",&config.advanced.fog.density,0,.2f);
                controlsChanged|=ImGui::SliderFloat("Height falloff",&config.advanced.fog.heightFalloff,0,2);
                controlsChanged|=ImGui::SliderFloat("Base height",&config.advanced.fog.baseHeight,-20,20);
                controlsChanged|=ImGui::SliderFloat("Anisotropy",&config.advanced.fog.anisotropy,-.9f,.9f);
                controlsChanged|=ImGui::SliderFloat("Fog distance",&config.advanced.fog.maxDistance,1,200);
                controlsChanged|=ImGui::SliderFloat("Fog resolution",&config.advanced.fog.resolutionScale,.1f,1);
                int steps=int(config.advanced.fog.steps);
                if(ImGui::SliderInt("Fog steps",&steps,1,128)) { config.advanced.fog.steps=uint32_t(steps); controlsChanged=true; }
            }
            if(ImGui::CollapsingHeader("Output"))
            {
                toggle("VSync",config.vsync);
                controlsChanged|=ImGui::SliderFloat("HDR supersampling",&config.hdrRenderScale,1,2);
                ImGui::Combo("Scene resolution",&resolution,"Viewport pixels\0Display 1920 x 1080\0Mobile 1280 x 720\0Compact 960 x 540\0");
            }
            ImGui::EndDisabled();
        }
        ImGui::End();
        ApplyConfig();
    }
    void Animation()
    {
        if(!showAnimation) return;
        if(ImGui::Begin("Animation and camera",&showAnimation))
        {
            ImGui::BeginDisabled(comparison.IsRunning());
            if(ImGui::Checkbox("Pause",&paused)) for(auto id:instances) scene->SetAnimationPaused(id,paused);
            ImGui::SameLine();
            if(ImGui::Button("Restart"))
            {
                time=0;
                for(auto id:instances)
                {
                    auto& instance=scene->GetModelInstance(id); instance.playback.time=0;
                }
                model.InvalidateHistory(); renderer.InvalidateHistory();
            }
            if(ImGui::SliderFloat("Playback speed",&speed,0,3)) for(auto id:instances) scene->SetAnimationSpeed(id,speed);
            if(ImGui::Checkbox("Loop",&repeat)) for(auto id:instances) scene->SetAnimationLoop(id,repeat);
            ImGui::Text("Scene time %.2f s",time);
            for(size_t i=0;i<instances.size();++i)
            {
                auto& instance=scene->GetModelInstance(instances[i]);
                const auto& clips=AnimationClips(*scene,instance);
                if(clips.empty()) continue;
                ImGui::PushID(int(i));
                const auto current=instance.playback.clipIndex;
                if(ImGui::BeginCombo("Clip",current<clips.size()?clips[current].name.c_str():"None"))
                {
                    for(uint32_t c=0;c<clips.size();++c)
                        if(ImGui::Selectable(clips[c].name.c_str(),c==current))
                        { scene->PlayAnimation(instances[i],c,repeat); model.InvalidateHistory(); }
                    ImGui::EndCombo();
                }
                if(current<clips.size())
                {
                    float position=instance.playback.time;
                    if(ImGui::SliderFloat("Clip time",&position,0,clips[current].duration))
                    { instance.playback.time=position; scene->UpdateAnimations(0); model.InvalidateHistory(); renderer.InvalidateHistory(); }
                }
                ImGui::PopID();
            }
            ImGui::SeparatorText("Camera");
            ImGui::Checkbox("Turntable",&turntable);
            bool changed=false;
            changed|=ImGui::SliderFloat("Yaw",&yaw,-Pi,Pi);
            changed|=ImGui::SliderFloat("Pitch",&pitch,-1.5f,1.5f);
            changed|=ImGui::SliderFloat("Distance",&distance,.2f,1000.f,"%.2f",ImGuiSliderFlags_Logarithmic);
            changed|=ImGui::SliderFloat("Field of view",&fov,15,110);
            changed|=ImGui::DragFloat3("Target",target,.05f);
            if(changed) renderer.InvalidateHistory();
            ImGui::TextWrapped("Drag right mouse to orbit; middle mouse to pan; wheel or two-finger scroll to zoom. On touch, drag the Scene to orbit.");
            ImGui::EndDisabled();
        }
        ImGui::End();
    }
    void Performance()
    {
        if(!showPerformance) return;
        if(ImGui::Begin("Performance",&showPerformance))
        {
            const auto& readout=performanceReadout;
            const auto& t=readout.timing;
            ImGui::Text("%s / %s",PlatformName,BackendName);
            const auto& device=renderer.GetDevice();
            const auto supported=[&](RHI::Feature feature) { return device.Supports(feature)?"yes":"no"; };
            ImGui::Text("Compute %s | timestamps %s | wireframe %s | indirect %s | descriptor indexing %s",
                supported(RHI::Feature::Compute),supported(RHI::Feature::TimestampQuery),supported(RHI::Feature::Wireframe),
                supported(RHI::Feature::IndirectDraw),supported(RHI::Feature::DescriptorIndexing));
            ImGui::TextWrapped("Numbers refresh once per second. Host/model times are interval averages; renderer times use the latest profiler averages.");
            if(!readout.ready) ImGui::TextUnformatted("Collecting performance samples...");
            else if(ImGui::BeginTable("Live timings",2,ImGuiTableFlags_RowBg|ImGuiTableFlags_BordersInnerV))
            {
                ImGui::TableSetupColumn("Timing",ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("Value",ImGuiTableColumnFlags_WidthFixed,100);
                const bool hasScene=readout.sceneFrames!=0;
                const auto row=[&](const char* label,double value,const char* format="%.3f ms",bool available=true)
                {
                    ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextUnformatted(label);
                    ImGui::TableNextColumn();
                    if(available) ImGui::Text(format,value); else ImGui::TextUnformatted("N/A");
                };
                row("Host cadence",readout.frameMs>0?1000/readout.frameMs:0,"%.1f FPS");
                row("Host frame interval",readout.frameMs);
                row("Complete host loop",readout.totals.Mean(readout.totals.host));
                row("Animation update",readout.totals.Mean(readout.totals.animation));
                row("Model Prepare",readout.SceneMean(readout.totals.modelPrepare),"%.3f ms",hasScene);
                row("Model + renderer call",readout.SceneMean(readout.totals.modelCall),"%.3f ms",hasScene);
                row("Renderer CPU",t.cpuRenderAverageMilliseconds,"%.3f ms",hasScene && t.frameCount!=0);
                row("CPU prepare",t.cpuPrepareAverageMilliseconds,"%.3f ms",hasScene && t.frameCount!=0);
                row("CPU record",t.cpuRecordAverageMilliseconds,"%.3f ms",hasScene && t.frameCount!=0);
                row("CPU submit",t.cpuSubmitAverageMilliseconds,"%.3f ms",hasScene && t.frameCount!=0);
                row("Renderer GPU span",t.gpuFrameAverageMilliseconds,"%.3f ms",hasScene && t.hasGpuFrame);
                row("GPU skin pass (last)",readout.skinningGpuMs,"%.3f ms",hasScene && readout.skinningGpuMs>=0);
                row("Last import",loadMs,"%.1f ms");
                ImGui::EndTable();
            }
            ImGui::TextWrapped("Renderer CPU stages and GPU span exclude model preparation/deformation, GUI and presentation. GPU span may include queue idle time. Model Prepare includes uploads and frame-slot waits; host loop includes GUI, acquisition and presentation. These intervals overlap; do not add them.");
            const auto& stats=readout.stats;
            ImGui::Text("Entities %u | main draws %u",readout.entities,stats.mainDrawCount);
            ImGui::Text("Shadow draws %u | triangles %llu",stats.shadowDrawCount,static_cast<unsigned long long>(stats.mainTriangleCount));
            ImGui::Text("Active paths: GPU visibility %s | Hi-Z %s | clustered lights %s",
                stats.gpuVisibility?"on":"off",stats.gpuOcclusion?"on":"off",stats.clusteredLighting?"on":"off");
            if(stats.gpuVisibility) ImGui::Text("Indirect batches %u; GPU visible counts are not read back",stats.indirectBatchCount);
            const auto& ui=readout.ui;
            ImGui::Text("GUI %u draws | %llu upload bytes",ui.drawCalls,static_cast<unsigned long long>(ui.uploadBytes));
            ImGui::BeginDisabled(comparison.IsRunning());
            if(ImGui::Combo("Comparison option",&comparisonAxis,ComparisonAxes,int(std::size(ComparisonAxes)))) ClearComparison();
            ImGui::EndDisabled();
            ImGui::TextWrapped("Current option: %s",ComparisonChoice().c_str());
            ImGui::BeginDisabled(Busy() || comparison.IsRunning());
            if(ImGui::Button("Capture A")) requestedCapture=1;
            ImGui::SameLine();
            ImGui::BeginDisabled(!comparison.HasBaseline());
            if(ImGui::Button("Capture B")) requestedCapture=2;
            ImGui::EndDisabled(); ImGui::EndDisabled();
            ImGui::SameLine();
            if(ImGui::Button(comparison.IsRunning()?"Cancel capture":"Clear comparison"))
            {
                if(comparison.IsRunning()) { comparison.Cancel("cancelled in GUI"); message="Capture cancelled; completed A remains available."; }
                else ClearComparison();
            }
            ImGui::TextWrapped("Capture A freezes the camera and pose. Warm-up: at least 1 s / 10 frames. Capture: at least 3 s / 30 frames. Then change only the selected option and capture B. The pose stays paused afterward.");
            if(comparison.IsRunning())
                ImGui::Text("%c %s %.1f s / %u frames",comparison.IsCandidateCapture()?'B':'A',
                    comparison.GetCapturePhase()==showcase::PerformanceComparison::CapturePhase::Measuring?"capturing":"warming up",
                    comparison.GetPhaseElapsedSeconds(),comparison.GetPhaseFrameCount());
            if(!comparison.IsRunning() && comparison.HasBaseline())
            {
                const auto& a=comparison.GetBaselineTiming();
                const auto& b=comparison.GetCandidateTiming();
                const bool hasB=comparison.HasCandidate();
                ImGui::TextWrapped("A: %s | B: %s",baselineChoice.c_str(),hasB?candidateChoice.c_str():"not captured");
                ImGui::Text("Weighted renderer CPU samples A %u / B %u; submitted host frames A %llu / B %llu",
                    a.frameCount,hasB?b.frameCount:0,static_cast<unsigned long long>(baselineHostCapture.frames),
                    static_cast<unsigned long long>(hasB?candidateHostCapture.frames:0));
                if(ImGui::BeginTable("Comparison results",4,ImGuiTableFlags_RowBg|ImGuiTableFlags_BordersInnerV))
                {
                    ImGui::TableSetupColumn("Milliseconds"); ImGui::TableSetupColumn("A");
                    ImGui::TableSetupColumn("B"); ImGui::TableSetupColumn("B - A"); ImGui::TableHeadersRow();
                    const auto row=[&](const char* name,double before,double after,bool hasA=true,bool hasAfter=true)
                    {
                        ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextUnformatted(name);
                        ImGui::TableNextColumn(); if(hasA) ImGui::Text("%.3f",before); else ImGui::TextUnformatted("N/A");
                        ImGui::TableNextColumn(); if(hasB && hasAfter) ImGui::Text("%.3f",after); else ImGui::TextUnformatted("N/A");
                        ImGui::TableNextColumn(); if(hasA && hasB && hasAfter) ImGui::Text("%+.3f",after-before); else ImGui::TextUnformatted("N/A");
                    };
                    row("Complete host loop",baselineHostCapture.Mean(baselineHostCapture.host),candidateHostCapture.Mean(candidateHostCapture.host));
                    row("Animation update",baselineHostCapture.Mean(baselineHostCapture.animation),candidateHostCapture.Mean(candidateHostCapture.animation));
                    row("Model Prepare",baselineHostCapture.Mean(baselineHostCapture.modelPrepare),candidateHostCapture.Mean(candidateHostCapture.modelPrepare));
                    row("Model + renderer CPU call",baselineHostCapture.Mean(baselineHostCapture.modelCall),candidateHostCapture.Mean(candidateHostCapture.modelCall));
                    row("Renderer CPU",a.cpuRenderAverageMilliseconds,b.cpuRenderAverageMilliseconds);
                    row("Renderer CPU prepare",a.cpuPrepareAverageMilliseconds,b.cpuPrepareAverageMilliseconds);
                    row("Renderer CPU record",a.cpuRecordAverageMilliseconds,b.cpuRecordAverageMilliseconds);
                    row("Renderer CPU submit",a.cpuSubmitAverageMilliseconds,b.cpuSubmitAverageMilliseconds);
                    row("Renderer GPU span",a.gpuFrameAverageMilliseconds,b.gpuFrameAverageMilliseconds,a.hasGpuFrame,b.hasGpuFrame);
                    const auto pass=[&](const Platform::ProfilerTimingSnapshot& timing,const std::string& name) -> const Platform::ProfilerPassTiming*
                    { for(const auto& p:timing.gpuPasses) if(p.name==name) return &p; return nullptr; };
                    for(const auto& p:a.gpuPasses)
                    { const auto* other=hasB?pass(b,p.name):nullptr; row(p.name.c_str(),p.averageMilliseconds,other?other->averageMilliseconds:0,p.sampleCount!=0,other && other->sampleCount); }
                    if(hasB) for(const auto& p:b.gpuPasses)
                        if(!pass(a,p.name)) row(p.name.c_str(),0,p.averageMilliseconds,false,p.sampleCount!=0);
                    ImGui::EndTable();
                }
                ImGui::Text("GPU span samples A %u / B %u",a.gpuFrameSampleCount,hasB?b.gpuFrameSampleCount:0);
                ImGui::TextWrapped("Renderer snapshots are weighted by their published CPU frame counts and completed GPU/pass sample counts. Host/model means use each completed submitted frame. No extra GPU wait or readback is inserted.");
            }
#ifdef DY_RENDERDOC_ENABLED
            ImGui::BeginDisabled(comparison.IsRunning() || !RenderDocCapture::IsAvailable());
            if(ImGui::Button("RenderDoc: capture next frame") && !RenderDocCapture::TriggerNextFrame()) message="RenderDoc capture request was not accepted.";
            ImGui::EndDisabled();
#endif
            ImGui::PlotLines("Host frame ms",frameHistory.data(),int(frameHistory.size()),int(historyCursor%frameHistory.size()),"60 FPS = 16.67 ms",0,66.67f,{0,70});
            if(ImGui::BeginTable("Passes",3,ImGuiTableFlags_RowBg|ImGuiTableFlags_ScrollY|ImGuiTableFlags_BordersInnerV,{0,180}))
            {
                ImGui::TableSetupColumn("GPU pass"); ImGui::TableSetupColumn("ms"); ImGui::TableSetupColumn("samples"); ImGui::TableHeadersRow();
                for(const auto& pass:t.gpuPasses)
                { ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextUnformatted(pass.name.c_str()); ImGui::TableNextColumn(); ImGui::Text("%.3f",pass.averageMilliseconds); ImGui::TableNextColumn(); ImGui::Text("%u",pass.sampleCount); }
                ImGui::EndTable();
            }
        }
        ImGui::End();
    }
    bool ScenePanel(Viewport& view)
    {
        bool visible=false;
        if(ImGui::Begin("Scene"))
        {
            auto size=ImGui::GetContentRegionAvail();
            if(size.x>=16 && size.y>=16)
            {
                const auto scale=ImGui::GetIO().DisplayFramebufferScale;
                uint32_t width=uint32_t(size.x*scale.x),height=uint32_t(size.y*scale.y);
                if(resolution) { const uint32_t widths[]={0,1920,1280,960},heights[]={0,1080,720,540}; width=widths[resolution]; height=heights[resolution]; }
                width=std::clamp(width,16u,4096u); height=std::clamp(height,16u,4096u);
                if(!view.Resize(width,height)) { ImGui::End(); return false; }
                ImGui::Image(ImTextureID(view.id),size);
                visible=true;
                if(ImGui::IsItemHovered() && !comparison.IsRunning() && !requestedCapture)
                {
                    const auto& io=ImGui::GetIO();
                    if(ImGui::IsMouseDragging(ImGuiMouseButton_Right) || (io.MouseSource==ImGuiMouseSource_TouchScreen && ImGui::IsMouseDragging(ImGuiMouseButton_Left)))
                    { yaw-=io.MouseDelta.x*.006f; pitch=std::clamp(pitch+io.MouseDelta.y*.006f,-1.5f,1.5f); }
                    if(ImGui::IsMouseDragging(ImGuiMouseButton_Middle)) { target[0]-=io.MouseDelta.x*distance*.002f; target[2]+=io.MouseDelta.y*distance*.002f; }
                    distance=std::clamp(distance*std::exp(-io.MouseWheel*.12f),.2f,1000.f);
                }
                const Math::float3 offset={std::cos(pitch)*std::cos(yaw),std::cos(pitch)*std::sin(yaw),std::sin(pitch)};
                const Math::float3 center={target[0],target[1],target[2]};
                if(!camera.LookAt(center+offset*distance,center) || !camera.SetPerspective(float(width)/height,fov*Pi/180,.03f,2000))
                { ImGui::End(); return false; }
            }
        }
        ImGui::End();
        return visible;
    }
};
}

int OpenWorkbench(Platform::Window& window,std::shared_ptr<WorkbenchState>& memory,
    const Example* examples,std::size_t exampleCount,const Example*& selected,
    std::vector<std::string>& arguments,const std::string& status,
    const fs::path& assetRoot,const fs::path& repositoryRoot,const fs::path& initialScene)
{
#ifdef DY_PROFILER_TRACING_ENABLED
    TracyAdapter tracy;
#endif
#ifdef DY_RENDERDOC_ENABLED
    (void)RenderDocCapture::Initialize();
#endif
    if(!window.GetHandle()) return 1;
    window.SetTitle("dy_engine / Examples workbench");
    selected=nullptr; arguments.clear();
    if(!memory) memory=std::make_shared<WorkbenchState>();
    const bool restoring=bool(memory->scene);
    RendererConfig config=restoring?memory->config:RendererConfig{};
    if(!restoring)
    {
        config.enableProfilerHud=false; config.enableHdrRendering=true; config.advanced.enabled=true;
        config.advanced.temporalAA=false; config.enableParallelRenderGraph=true; config.recordingThreadCount=4;
        config.lighting.shadows=true; config.lighting.ambientIntensity=.08f;
    }
    auto renderer=Renderer::Create(window.GetHandle(),config);
    if(!renderer || !renderer->AddExtension(std::make_unique<AdvancedRendering>())) return 1;
    auto& device=renderer->GetDevice();
    auto gui=Gui::Create(window,device);
    if(!gui) return 1;
    ImGui::GetIO().ConfigFlags|=ImGuiConfigFlags_DockingEnable;
    ImGui::GetIO().IniFilename=nullptr; // Layout is session state; do not write application settings.
    if(const auto* font=std::getenv("DY_EXAMPLES_FONT"))
    {
        std::error_code fontError;
        if(fs::is_regular_file(fs::u8path(font),fontError))
            ImGui::GetIO().Fonts->AddFontFromFileTTF(font,16.f);
    }
    ImGui::StyleColorsDark();
    Workspace app(*renderer,*gui,assetRoot,repositoryRoot,memory);
    if(!status.empty()) app.message=status;
    if(!initialScene.empty() && !restoring)
    {
        const SceneAsset* found=nullptr;
        for(const auto& asset:sceneAssets) if(initialScene.u8string()==asset.id) found=&asset;
        if(found && !CatalogCanLoadModel(*found)) app.message=std::string(found->name)+": "+AssetAvailabilityName(found->availability)+"; use its supported example or convert the source first.";
        else if(found)
        {
            const auto base=found->root==SceneAssetRoot::External?assetRoot:repositoryRoot;
            app.StartLoad(app.Path(*found),*found->environmentPath?app.ResolveAsset(base,fs::u8path(found->environmentPath)):fs::path{},found->name);
        }
        else app.StartLoad(initialScene);
    }
    Viewport viewport{device,*gui};
    auto previous=Clock::now();
    while(window.IsRunning() && !app.pendingExample)
    {
        const auto loopStarted=Clock::now();
        window.PollEvents();
        if(!window.IsRunning()) break;
        const auto size=window.GetFramebufferSize();
        if(window.IsMinimized() || size.width<=0 || size.height<=0)
        { app.comparison.Cancel("window minimized"); Platform::Window::WaitEvents(.05); previous=Clock::now(); continue; }
        const auto now=Clock::now();
        const float elapsed=std::chrono::duration<float>(now-previous).count(); previous=now;
        app.frameMs=elapsed*1000; app.frameHistory[app.historyCursor++%app.frameHistory.size()]=float(app.frameMs);
        app.Tick(std::min(.1f,elapsed));
        if(!gui->BeginFrame())
        { app.comparison.Cancel("GUI frame unavailable"); if(device.IsLost()) return 1; Platform::Window::WaitEvents(.01); continue; }
        app.Menus(); app.Dock(); app.Assets(); app.Examples(examples,exampleCount); app.Rendering(); app.Animation(); app.Performance();
        const bool visible=app.ScenePanel(viewport);
        app.PrepareComparison(viewport,visible,uint32_t(size.width),uint32_t(size.height));
        gui->EndFrame();
        const auto modelStarted=Clock::now();
        if(visible && !app.model.RenderToTexture(*app.scene,app.camera,viewport.texture,viewport.state))
        { std::fprintf(stderr,"Scene viewport render failed.\n"); return 1; }
        app.modelCallMs=visible?std::chrono::duration<double,std::milli>(Clock::now()-modelStarted).count():0;
        if(visible) viewport.state=RHI::ResourceState::ShaderResource;
        // Offscreen rendering may apply VSync/swapchain settings. Acquire the host
        // frame afterward, and retain this completed scene/GUI draw while waiting.
        while(!device.BeginFrame())
        {
            app.comparison.Cancel("host frame acquisition delayed");
            if(device.IsLost()) return 1;
            window.PollEvents();
            if(!window.IsRunning()) break;
            Platform::Window::WaitEvents(.001);
        }
        if(!window.IsRunning()) break;
        {
            RHI::ResourceScope resources(device);
            auto* commands=resources.Keep(device.AcquireCommandList());
            auto* output=device.GetBackBuffer();
            if(!commands || !output) return 1;
            const RHI::ResourceBarrierDesc begin{nullptr,output,RHI::ResourceState::Present,RHI::ResourceState::RenderTarget,{}};
            commands->ResourceBarrier(&begin,1);
            RHI::ColorAttachment color; color.texture=output; color.loadOp=RHI::LoadOp::Clear; color.storeOp=RHI::StoreOp::Store; color.clearColor[3]=1;
            commands->BeginRendering({&color,1,nullptr}); commands->EndRendering();
            const RHI::ResourceBarrierDesc end{nullptr,output,RHI::ResourceState::RenderTarget,RHI::ResourceState::Present,{}};
            commands->ResourceBarrier(&end,1);
            if(!gui->Record(*commands,output) || !commands->Close() || !device.Submit(&commands,1) || !device.Present()) return 1;
        }
        app.CompleteFrame(std::chrono::duration<double,std::milli>(Clock::now()-loopStarted).count(),visible);
    }
    if(!device.WaitIdle()) return 1;
    selected=app.pendingExample; arguments=std::move(app.pendingArguments);
    return 0;
}
}
