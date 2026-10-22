#include "dyf/Extends/Tracy.h"
#include "dyf/Extends/RenderDoc.h"
#include "dyf/Camera.h"
#include "dyf/Extends/AdvancedRendering.h"
#include "dyf/Extends/Model/Model.h"
#include "dyf/Extends/Model/ModelScene.h"
#include "dyf/Extends/Model/ModelRenderer.h"
#include "dyf/Platform/Time.h"
#include "dyf/Platform/Window.h"
#include "dyf/RHI/IDevice.h"
#include "dyf/Renderer.h"
#include "../PerformanceComparison.h"
#include "AnimatedActor.h"
#include "KnightMaterials.h"
#include "../CameraPath.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <exception>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <filesystem>
#include <memory>
#include <mutex>
#include <locale>
#include <numeric>
#include <string>
#include <sstream>
#include <stdexcept>
#include <utility>
#include <vector>

#ifndef DY_SHOWCASE_DEFAULT_SCENE
#define DY_SHOWCASE_DEFAULT_SCENE ""
#endif
#ifndef DY_SHOWCASE_DEFAULT_ENVIRONMENT
#define DY_SHOWCASE_DEFAULT_ENVIRONMENT ""
#endif
#ifndef DY_SHOWCASE_EMERALD_SCENE
#define DY_SHOWCASE_EMERALD_SCENE ""
#endif
#ifndef DY_SHOWCASE_EMERALD_ENVIRONMENT
#define DY_SHOWCASE_EMERALD_ENVIRONMENT ""
#endif
#ifndef DY_SHOWCASE_KNIGHT_ACTOR
#define DY_SHOWCASE_KNIGHT_ACTOR ""
#endif
#ifndef DY_SHOWCASE_COMPARISON_SCENE
#define DY_SHOWCASE_COMPARISON_SCENE ""
#endif
#ifndef DY_SHOWCASE_COMPARISON_CAMERA
#define DY_SHOWCASE_COMPARISON_CAMERA ""
#endif

namespace
{
// The public snapshot contains publication windows, not individual GPU frames.
// Keep those windows separate from exact per-submission wall times/statistics.
struct BenchmarkFrame
{
    double wall = 0, renderWall = 0, submissionInterval = 0, sceneTime = 0;
    dyf::Platform::ProfilerTimingSample cpu;
    bool hasCpu = false;
    dyf::RendererStatistics statistics;
};

struct BenchmarkTrace
{
    struct Diagnostic { uint32_t submission; const char* name; double milliseconds; };
    std::vector<Diagnostic> diagnostics;
    std::atomic<uint32_t> diagnosticSubmission{0};
    std::atomic<bool> diagnosticsFailed{false};
    std::mutex diagnosticsMutex;
    const bool cpuZones;
    dyf::Platform::ProfilerTimingSample sample;
    bool received = false;
    void AddDiagnostic(uint32_t submission,const char* name,double milliseconds) noexcept
    {
        try
        {
            if(cpuZones)
            {
                std::lock_guard<std::mutex> lock(diagnosticsMutex);
                diagnostics.push_back({submission,name,milliseconds});
            }
            else diagnostics.push_back({submission,name,milliseconds});
        }
        catch(...) { diagnosticsFailed.store(true,std::memory_order_relaxed); }
    }
    std::vector<Diagnostic> TakeDiagnostics()
    {
        std::lock_guard<std::mutex> lock(diagnosticsMutex);
        return std::move(diagnostics);
    }
#ifdef DY_PROFILER_TRACING_ENABLED
    inline static std::atomic<BenchmarkTrace*> active{nullptr};
    dyf::Platform::TraceSink sink = {};
    const dyf::Platform::TraceSink* previous = nullptr;
    struct ZoneEntry
    {
        uint64_t token,previousToken,beginNs;
        uint32_t submission;
        const char* name;
        BenchmarkTrace* trace;
        void (*previousEnd)(uint64_t) noexcept;
    };
    struct ZoneState { std::vector<ZoneEntry> entries; uint64_t nextToken=1; };
    static ZoneState& ThreadZones() noexcept
    {
        thread_local ZoneState state;
        return state;
    }
    static uint64_t CaptureBegin(const dyf::Platform::SourceLocation& location) noexcept
    {
        auto* trace=active.load(std::memory_order_acquire);
        if(!trace) return 0;
        auto& state=ThreadZones();
        if(!state.nextToken)
        {
            trace->diagnosticsFailed.store(true,std::memory_order_relaxed);
            return 0;
        }
        const uint64_t token=state.nextToken++;
        const char* name=nullptr;
        if(location.name && std::strcmp(location.name,"RendererShadow")==0) name="CPU zone RendererShadow";
        else if(location.name && std::strcmp(location.name,"RendererMainForward")==0) name="CPU zone RendererMainForward";
        else if(location.name && std::strcmp(location.name,"Native replay")==0) name="CPU zone Native replay";
        else if(location.name && std::strcmp(location.name,"Validate + native submit")==0) name="CPU zone Validate + native submit";
        const uint32_t submission=trace->diagnosticSubmission.load(std::memory_order_acquire);
        try
        {
            // Reserve our token before opening the previous adapter's zone: failed
            // bookkeeping must not leave an unmatched zone in that adapter.
            state.entries.push_back({token,0,0,submission,name,trace,trace->previous?trace->previous->endZone:nullptr});
        }
        catch(...)
        {
            trace->diagnosticsFailed.store(true,std::memory_order_relaxed);
            return 0;
        }
        const uint64_t previousToken=trace->previous && trace->previous->beginZone?
            trace->previous->beginZone(location):0;
        // A previous adapter may emit nested zones and reallocate this thread's vector.
        const auto found=std::find_if(state.entries.begin(),state.entries.end(),
            [token](const ZoneEntry& entry) { return entry.token==token; });
        if(found!=state.entries.end())
        {
            found->previousToken=previousToken;
            if(name && submission) found->beginNs=dyf::Platform::NowNs();
        }
        return token;
    }
    static void CaptureEnd(uint64_t token) noexcept
    {
        auto& entries=ThreadZones().entries;
        const auto found=std::find_if(entries.begin(),entries.end(),
            [token](const ZoneEntry& entry) { return entry.token==token; });
        if(found==entries.end()) return; // Includes allocation-failed begin token 0.
        const ZoneEntry entry=*found;
        const uint64_t endNs=entry.name && entry.submission?dyf::Platform::NowNs():0;
        // Erase by token, rather than assuming zones finish in stack order.
        entries.erase(found);
        if(entry.previousEnd) entry.previousEnd(entry.previousToken);
        if(entry.name && entry.submission)
            entry.trace->AddDiagnostic(entry.submission,entry.name,dyf::Platform::Milliseconds(entry.beginNs,endNs));
    }
    static void Capture(const dyf::Platform::ProfilerTimingSample& value) noexcept
    {
        auto* trace=active.load(std::memory_order_acquire);
        if(!trace) return;
        trace->sample = value;
        trace->received = true;
        if(trace->previous && trace->previous->frameSample) trace->previous->frameSample(value);
    }
    static void CaptureValue(const char* name, double value, bool raw) noexcept
    {
        auto* trace=active.load(std::memory_order_acquire);
        if(!trace) return;
        const uint32_t submission=trace->diagnosticSubmission.load(std::memory_order_acquire);
        if(submission && name &&
            (std::strcmp(name,"Vulkan queue submit ms")==0 ||
             std::strcmp(name,"Vulkan present submit ms")==0 ||
             std::strcmp(name,"Vulkan queue present ms")==0 ||
             std::strcmp(name,"Vulkan queue present CPU ms")==0 ||
             std::strcmp(name,"GPU graph span ms")==0 ||
             std::strcmp(name,"GPU upload to graph ms")==0))
        {
            trace->AddDiagnostic(submission,name,value);
        }
        if(trace->previous && trace->previous->value) trace->previous->value(name,value,raw);
    }
#endif
    explicit BenchmarkTrace(bool captureCpuZones) : cpuZones(captureCpuZones)
    {
#ifdef DY_PROFILER_TRACING_ENABLED
        previous = dyf::Platform::SetTraceSink(nullptr);
        if(previous) sink = *previous;
        sink.frameSample = Capture;
        sink.value = CaptureValue;
        if(cpuZones) { sink.beginZone=CaptureBegin; sink.endZone=CaptureEnd; }
        active.store(this,std::memory_order_release);
        (void)dyf::Platform::SetTraceSink(&sink);
#endif
    }
    ~BenchmarkTrace()
    {
#ifdef DY_PROFILER_TRACING_ENABLED
        (void)dyf::Platform::SetTraceSink(previous);
        active.store(nullptr,std::memory_order_release);
#endif
    }
};

void BenchmarkSummary(std::ostream& output, const char* name, std::vector<double> values)
{
    if(values.empty()) { output << "# summary " << name << " samples=0 unavailable\n"; return; }
    std::sort(values.begin(), values.end());
    const auto percentile = [&](double fraction) {
        return values[static_cast<size_t>(std::ceil(fraction * values.size())) - 1];
    };
    output << "# summary " << name << " samples=" << values.size()
        << " mean_ms=" << std::accumulate(values.begin(), values.end(), 0.0) / values.size()
        << " p50_ms=" << percentile(.5) << " p95_ms=" << percentile(.95)
        << " p99_ms=" << percentile(.99) << '\n';
}
}

int main(int argc, char** argv)
{
    try
    {
        using namespace dyf;
        using Math::float3;
        using Platform::Key;
        using Platform::MouseButton;
        constexpr float pi = 3.14159265358979323846f;
        double tourSeconds = 60.0;
        using Shot=showcase::CameraKeyframe;
        showcase::CameraPath comparisonPath;
        std::string cameraPath,exportCameraPath;
        double initialTourTime=0;
        bool initiallyPaused=false;
        uint32_t benchmarkFrames = 0, benchmarkWarmup = 120;
        double benchmarkStep = 0;
        bool benchmarkRequested = false;
        bool benchmarkCpuZones = false;
        bool cpuCulling = false;
        uint32_t recordingWorkers = 4;
        uint32_t framesInFlight = 2;
        std::string benchmarkOutput;
        const auto benchmarkCount = [](const char* value) {
            uint32_t count = 0;
            const auto end = value + std::char_traits<char>::length(value);
            const auto parsed = std::from_chars(value, end, count);
            if(parsed.ec != std::errc{} || parsed.ptr != end)
                throw std::runtime_error("Benchmark counts must be unsigned 32-bit integers.");
            return count;
        };
        const char* const surfaceViews[] = {"off", "base color", "normal", "roughness", "metallic", "depth", "motion"};
        // Intel New Sponza, centered and normalized to 80 units in Z-up world.
        std::vector<Shot> shots{
            {0, {-23.583370f, 7.369860f, -15.936993f}, {-2.309021f, 3.353316f, -11.648315f}, 58.512f},
            {20, {-19.594216f, 6.579154f, -15.919820f}, {9.097909f, 5.475611f, -13.492025f}, 54},
            {65, {-8.558784f, 5.475611f, -15.809466f}, {15.719168f, 5.475611f, -13.271316f}, 50},
            {105, {11.746412f, 5.475611f, -15.699111f}, {17.926255f, 5.475611f, -10.622812f}, 50},
            {125, {11.746412f, 5.475611f, -13.271316f}, {0.269563f, -4.456279f, -6.208639f}, 50},
            {170, {2.476649f, 5.475611f, -2.456592f}, {-15.180043f, 5.475611f, -12.829899f}, 50},
            {205, {-10.765870f, 5.475611f, -1.353049f}, {-24.008389f, 5.475611f, -12.829899f}, 50},
            {260, {-20.697759f, 5.475611f, -0.249506f}, {4.683736f, 5.475611f, -2.235883f}, 50},
            {280, {-19.594216f, 5.475611f, 20.276399f}, {-4.144611f, 5.475611f, -10.622812f}, 52},
            {300, {-35.043822f, 34.167735f, 44.554351f}, {-4.144611f, 5.475611f, -8.415726f}, 54}
        };
        ModelSceneDesc model;
        model.path = DY_SHOWCASE_DEFAULT_SCENE;
        ModelSceneDesc actorDescription;
        actorDescription.normalizedSize = 2.4f;
        actorDescription.transform = Math::RotationX(pi * .5f);
        actorDescription.loadOptions.textureDecodeThreadCount = 4;
        uint32_t actorClip = 0;
        bool actorPositioned = false, sceneSpecified = false, bakeSpatialProbes = true, legacyComparison = false;
        ShadowQuality shadowQuality = ShadowQuality::High;
        float shadowDistance = 0, shadowSplit = .65f, lightRadius = 0;
        uint32_t directionalShadowLights = 4, pointShadowLights = 16, spotShadowLights = 16;
        uint32_t glassLayers = 2;
        const auto usage = [] {
            std::fputs("Usage: showcase [exterior|interior|emerald|comparison|scene.gltf|scene.glb|scene.fbx|scene.obj] [options]\n"
                "  comparison               Same Sponza asset and 60 s CSV as the legacy-renderer branch\n"
                "  --actor FILE|knight      Original animated glTF/GLB/FBX, Y-up; knight uses Intel's original FBX\n"
                "  --actor-position X Y Z   Actor feet in normalized Z-up world; default: opening view\n"
                "  --actor-size N           Actor largest dimension (default 2.4)\n"
                "  --actor-clip N           Zero-based animation clip (default 0)\n"
                "  --path FILE              Shared comparison CSV, seconds/eye/target/FOV/up (11 columns)\n"
                "  --export-camera FILE     Write a 60 Hz camera CSV without creating a window or GPU device\n"
                "  --start N --paused       Hold an identical comparison timestamp in both renderers\n"
                "  --benchmark-frames N     Submit N measured frames starting at --start, then exit\n"
                "  --benchmark-warmup N     Submit N warm-up frames and drain GPU work first (default 120)\n"
                "  --benchmark-step N       Scene seconds per measured submission (default 0 = frozen); warm-up stays at --start\n"
                "                          Pose k uses min(tour duration, start + k*step), independent of render speed/retries\n"
                "  --benchmark-output FILE  New CSV: wall/statistics per frame, CPU/GPU profiler windows separately\n"
                "                          Benchmark disables VSync/HUD; render quality defaults are preserved\n"
                "  --benchmark-cpu-zones    Optional CPU Shadow/MainForward/native replay/submit diagnostics (requires tracing)\n"
                "  --recording-workers N    CPU recording workers, 1..64 (default 4)\n"
                "  --frames-in-flight N     Maximum GPU frames in flight, 1..4 (default 2)\n"
                "  --cpu-culling            Use CPU frustum culling/instancing; disable GPU culling and Hi-Z occlusion\n"
                "  --no-probes              Skip 3 spatial probe bakes; interior keeps its single room fallback\n"
                "  --shadow-quality MODE    low, medium, high (default high)\n"
                "  --shadow-distance N      Directional shadow view distance in world units; 0 keeps scene/camera fit\n"
                "  --shadow-split N         Cascade logarithmic split weight, 0..1 (default 0.65)\n"
                "  --shadow-lights D P S    Directional/point/spot shadow-light budgets, 0..4 / 0..16 / 0..16\n"
                "  --glass-layers N         Raster transmission layers, 1 or 2 (default 2)\n"
                "  --light-radius N         Point/spot source radius in normalized world units (default: imported value)\n"
                "  U GPU frustum/LOD | O Hi-Z occlusion | F fog | K sharpening | J clustered lights\n"
                "  I spatial probes on/off (Bistro; cached once before the actor loads)\n"
                "  P soft shadows / fixed PCF | F10 one/two glass layers\n"
                "  Left/Right seek 5 s | Home/End first/last pose\n",stderr);
        };
        for(int i = 1; i < argc; ++i)
        {
            const std::string argument = argv[i];
            if(argument == "--help") { usage(); return 0; }
            if(argument == "--no-probes") bakeSpatialProbes = false;
            else if(argument == "--cpu-culling") cpuCulling = true;
            else if(argument == "--benchmark-frames" && i + 1 < argc)
            { benchmarkRequested = true; benchmarkFrames = benchmarkCount(argv[++i]); }
            else if(argument == "--benchmark-warmup" && i + 1 < argc)
            { benchmarkRequested = true; benchmarkWarmup = benchmarkCount(argv[++i]); }
            else if(argument == "--benchmark-step" && i + 1 < argc)
            {
                benchmarkRequested = true;
                const std::string value = argv[++i]; size_t consumed = 0;
                benchmarkStep = std::stod(value,&consumed);
                if(consumed != value.size() || !std::isfinite(benchmarkStep) || benchmarkStep < 0)
                    throw std::runtime_error("Benchmark step must be a finite, nonnegative number of seconds.");
            }
            else if(argument == "--benchmark-output" && i + 1 < argc)
            { benchmarkRequested = true; benchmarkOutput = argv[++i]; }
            else if(argument == "--benchmark-cpu-zones")
            { benchmarkRequested = true; benchmarkCpuZones = true; }
            else if(argument == "--recording-workers" && i + 1 < argc)
            {
                recordingWorkers = benchmarkCount(argv[++i]);
                if(!recordingWorkers || recordingWorkers > 64)
                    throw std::runtime_error("Recording workers must be an integer from 1 to 64.");
            }
            else if(argument == "--frames-in-flight" && i + 1 < argc)
            {
                framesInFlight = benchmarkCount(argv[++i]);
                if(!framesInFlight || framesInFlight > 4)
                    throw std::runtime_error("Frames in flight must be an integer from 1 to 4.");
            }
            else if(argument == "--paused") initiallyPaused=true;
            else if(argument == "--start" && i + 1 < argc) initialTourTime=std::stod(argv[++i]);
            else if(argument == "--path" && i + 1 < argc) cameraPath=argv[++i];
            else if(argument == "--export-camera" && i + 1 < argc) exportCameraPath=argv[++i];
            else if(argument == "--glass-layers" && i + 1 < argc)
            {
                const auto layers = std::stoul(argv[++i]);
                if(layers<1 || layers>2) { usage(); return 1; }
                glassLayers = static_cast<uint32_t>(layers);
            }
            else if(argument == "--shadow-distance" && i + 1 < argc) shadowDistance = std::stof(argv[++i]);
            else if(argument == "--light-radius" && i + 1 < argc) lightRadius = std::stof(argv[++i]);
            else if(argument == "--shadow-split" && i + 1 < argc) shadowSplit = std::stof(argv[++i]);
            else if(argument == "--shadow-lights" && i + 3 < argc)
            {
                const auto directional = std::stoul(argv[++i]);
                const auto point = std::stoul(argv[++i]);
                const auto spot = std::stoul(argv[++i]);
                if(directional>4 || point>16 || spot>16) { usage(); return 1; }
                directionalShadowLights = static_cast<uint32_t>(directional);
                pointShadowLights = static_cast<uint32_t>(point);
                spotShadowLights = static_cast<uint32_t>(spot);
            }
            else if(argument == "--shadow-quality" && i + 1 < argc)
            {
                const std::string quality = argv[++i];
                if(quality == "low") shadowQuality = ShadowQuality::Low;
                else if(quality == "medium") shadowQuality = ShadowQuality::Medium;
                else if(quality == "high") shadowQuality = ShadowQuality::High;
                else { usage(); return 1; }
            }
            else if(argument == "--actor" && i + 1 < argc) actorDescription.path = argv[++i];
            else if(argument == "--actor-size" && i + 1 < argc)
                actorDescription.normalizedSize = std::stof(argv[++i]);
            else if(argument == "--actor-position" && i + 3 < argc)
            {
                actorDescription.position.x = std::stof(argv[++i]);
                actorDescription.position.y = std::stof(argv[++i]);
                actorDescription.position.z = std::stof(argv[++i]);
                actorPositioned = true;
            }
            else if(argument == "--actor-clip" && i + 1 < argc)
            {
                const auto clip = std::stoul(argv[++i]);
                if(clip > UINT32_MAX) throw std::runtime_error("Actor clip index is too large.");
                actorClip = static_cast<uint32_t>(clip);
            }
            else if(!argument.empty() && argument.front() != '-' && !sceneSpecified)
            { model.path = argument; sceneSpecified = true; }
            else { usage(); return 1; }
        }
        if(benchmarkRequested && (!benchmarkFrames || benchmarkOutput.empty() || !exportCameraPath.empty()))
            throw std::runtime_error("Benchmark requires positive --benchmark-frames and --benchmark-output; camera export is a separate mode.");
        if(benchmarkRequested && std::filesystem::exists(benchmarkOutput))
            throw std::runtime_error("Benchmark output already exists: " + benchmarkOutput);
#ifndef DY_PROFILER_TRACING_ENABLED
        if(benchmarkCpuZones) throw std::runtime_error("--benchmark-cpu-zones requires a build with profiler tracing enabled.");
#endif
        if(model.path == "exterior" || model.path == "interior")
            model.path = (std::filesystem::path(DY_SHOWCASE_DEFAULT_SCENE).parent_path() /
                (model.path == "exterior" ? "BistroExterior.fbx" : "BistroInterior_Wine.fbx")).string();
        if(model.path == "emerald") model.path = DY_SHOWCASE_EMERALD_SCENE;
        if(model.path=="comparison")
        {
            legacyComparison=true;
            model.path=DY_SHOWCASE_COMPARISON_SCENE;
            if(cameraPath.empty()) cameraPath=DY_SHOWCASE_COMPARISON_CAMERA;
            if(model.path.empty() || cameraPath.empty())
                throw std::runtime_error("Sponza comparison asset or camera path is not configured");
        }
        if(actorDescription.path=="knight")
        {
            actorDescription.path=DY_SHOWCASE_KNIGHT_ACTOR;
            if(actorDescription.path.empty())
            {
                std::fputs("Intel Knight path is unavailable; set DY_SHOWCASE_KNIGHT_ACTOR when configuring the build.\n",stderr);
                return 1;
            }
        }
        if(std::filesystem::path(actorDescription.path).filename()=="Knight_USD_002.fbx")
        {
            showcase::ApplyIntelKnightMaterials(actorDescription);
            std::fputs("Intel Knight: original Maya PBR values and 4K PNGs explicitly bound; animation remains in the original FBX.\n",stderr);
        }
        if(model.path.empty() || !std::isfinite(actorDescription.normalizedSize) || actorDescription.normalizedSize <= 0 ||
            !std::isfinite(actorDescription.position.x) || !std::isfinite(actorDescription.position.y) ||
            !std::isfinite(actorDescription.position.z))
        {
            usage();
            return 1;
        }
        if(!std::isfinite(shadowDistance) || shadowDistance < 0 ||
            !std::isfinite(shadowSplit) || shadowSplit < 0 || shadowSplit > 1 ||
            !std::isfinite(lightRadius) || lightRadius < 0 || !std::isfinite(initialTourTime) || initialTourTime<0)
        { usage(); return 1; }
        const auto filename = std::filesystem::path(model.path).filename().string();
        const bool bistro = filename.rfind("Bistro", 0) == 0;
        const bool wine = filename == "BistroInterior_Wine.fbx";
        const bool bistroInterior = wine || filename == "BistroInterior.fbx";
        const bool emerald = filename == "EmeraldSquare_Day.fbx";
        model.normalizedSize = 80;
        // ufbx preserves source axes; Bistro, Emerald Day and New Sponza are Y-up.
        model.transform = Math::RotationX(pi * .5f);
        model.loadOptions.maxSourceBytes = 8ull * 1024 * 1024 * 1024;
        model.loadOptions.maxParserInputBytes = model.loadOptions.maxSourceBytes;
        model.loadOptions.maxParserBytes = model.loadOptions.maxSourceBytes;
        model.loadOptions.maxDecodedBytes = model.loadOptions.maxSourceBytes;
        model.loadOptions.textureDecodeThreadCount = 4;
        if(emerald)
        {
            // EmeraldSquare README: BaseColor alpha = opacity, Specular RGB =
            // AO/roughness/metalness and tangent-space normals use DirectX Y.
            model.loadOptions.importLights = true;
            model.loadOptions.fbxSpecularTextureIsOrm = true;
            model.loadOptions.fbxBaseColorTextureHasOpacity = true;
            model.loadOptions.normalMapYSign = -1;
        }
        if(bistro)
        {
            model.loadOptions.fbxSpecularTextureIsOrm = true;
            model.loadOptions.fbxBaseColorTextureHasOpacity = true;
            model.loadOptions.normalMapYSign = -1;
            model.loadOptions.importLights = true;
            // Thickness is an explicit raster approximation, in meters here.
            // Colors are exp(-absorptionCoefficient * 0.01m) from the bundle .pyscene.
            model.loadOptions.materialOverrides = {
                {"TransparentGlass", 1, 1.55f, .003f},
                {"TransparentGlassWine", 1, 1.55f, .003f,
                    {std::exp(-1.0268063f), std::exp(-1.68015f), std::exp(-2.4680438f)}, .01f},
                {"Water", 1, 1.33f, .04f},
                {"Ice", 1, 1.31f, .03f},
                {"White_Wine", 1, 1.33f, .04f,
                    {std::exp(-.1228758f), std::exp(-.1651818f), std::exp(-.2030273f)}, .01f},
                {"Red_Wine", 1, 1.33f, .04f,
                    {std::exp(-1.1713133f), std::exp(-2.5191133f), std::exp(-2.9433867f)}, .01f},
                {"Beer", 1, 1.33f, .04f,
                    {std::exp(-.1178552f), std::exp(-.2545862f), std::exp(-.5837241f)}, .01f}
            };
            for(auto& optics : model.loadOptions.materialOverrides)
            {
                optics.roughnessFactor = optics.name == "Ice" ? .1f : 0;
                optics.metallicFactor = 0;
            }
        }
        ModelScene scene;
        ModelInstanceID modelInstance = ModelInstanceID::Invalid;
        std::fprintf(stderr, "Loading environment: %s\n", model.path.c_str());
        if(!AddModelToScene(scene, model, &modelInstance)) return 1;
        const auto* instance = scene.TryGetModelInstance(modelInstance);
        const auto& asset = scene.GetModelAsset(instance ? instance->assetId : static_cast<ModelAssetID>(0));
        auto sourceToWorld = Math::Translation(model.position) * model.transform;
        if(model.normalize && asset.hasBounds)
            sourceToWorld = sourceToWorld * Math::Scaling(asset.boundsLargestAxis > .0001f ?
                model.normalizedSize / asset.boundsLargestAxis : 1.0f) * Math::Translation(asset.boundsCenter * -1.0f);
        sourceToWorld = sourceToWorld * asset.assetTransform;
        if(instance) sourceToWorld = instance->rootTransform;
        struct WindObject
        {
            uint32_t nodeIndex;
            std::vector<std::pair<EntityHandle, Math::float4x4>> parts;
            Math::Bounds3 bounds;
            float amplitude;
        };
        std::vector<WindObject> windObjects;
        if(bistro && !bistroInterior && instance)
        {
            // Only independently imported leaves/flowers move. In particular,
            // never infer motion from a whole tree, flower pot or building name.
            for(const auto& binding : instance->bindings)
            {
                if(binding.nodeIndex >= asset.nodes.size()) continue;
                auto entity = scene.GetEntity(binding.entity);
                const auto material = entity.GetMaterial();
                const auto texture = std::filesystem::path(material.baseColorTexture.GetSourcePath()).stem().string();
                const auto& node = asset.nodes[binding.nodeIndex].name;
                const bool tree = node.find("_Linde_Tree_Large_") != std::string::npos &&
                    (texture == "Foliage_Linde_Tree_Large_Green_Leaves_BaseColor" ||
                     texture == "Foliage_Linde_Tree_Large_Orange_Leaves_BaseColor");
                const bool ivy = node.find("_Ivy_") != std::string::npos && texture == "Foliage_Ivy_leaf_a_BaseColor";
                const bool flowers = node.find("_Paris_Flowers_") != std::string::npos &&
                    (texture == "Foliage_Flowers_BaseColor" || texture == "Foliage_Paris_Flowers_BaseColor");
                if(!tree && !ivy && !flowers) continue;
                const auto meshIndex = ToIndex(scene.GetEntityMesh(binding.entity));
                if(meshIndex >= scene.Meshes().size() || !scene.Meshes()[meshIndex]) continue;
                auto object = std::find_if(windObjects.begin(), windObjects.end(), [&](const WindObject& value) {
                    return value.nodeIndex == binding.nodeIndex;
                });
                if(object == windObjects.end())
                {
                    windObjects.push_back({binding.nodeIndex, {}, {}, tree ? .004f : ivy ? .006f : .016f});
                    object = windObjects.end() - 1;
                }
                const auto rest = entity.GetTransform();
                object->parts.emplace_back(entity, rest);
                for(const auto& vertex : scene.Meshes()[meshIndex]->vertices)
                    object->bounds.Include(Math::TransformPoint(rest, vertex.position));
            }
            std::fprintf(stderr, "Wind: %zu independently imported leaf/flower groups; trunks, pots and architecture stay fixed. N toggles wind.\n",
                windObjects.size());
        }
        if(bistro)
        {
            // Bistro declares UnitScaleFactor=100 (meters); lengths follow scene normalization.
            const float unitsPerMeter = model.normalizedSize / std::max(asset.boundsLargestAxis, .0001f);
            std::vector<bool> updated(scene.Materials().size(), false);
            for(uint32_t i = 0; i < scene.GetEntityCount(); ++i)
            {
                const auto entity = static_cast<EntityID>(i);
                const auto materialIndex = ToIndex(scene.GetEntityMaterial(entity));
                if(materialIndex >= updated.size() || updated[materialIndex]) continue;
                auto handle = scene.GetEntity(entity);
                auto material = handle.GetMaterial();
                // All 201 Bistro Specular maps leave R at zero: no authored AO.
                // Keep G/B roughness/metalness; screen-space AO remains independent.
                material.occlusionStrength = 0;
                // The Wine bundle explicitly supplies this emitter calibration.
                // Analytic lights below provide illumination; emission alone does not.
                if(wine) material.emissiveColor = material.emissiveColor * 1000.0f;
                if(material.transmissionFactor > 0)
                {
                    // Bistro's legacy diffuse tint/factor is not a transmission tint.
                    // The explicit optical overrides above supply volume absorption.
                    material.baseColor.x = material.baseColor.y = material.baseColor.z = 1;
                    material.baseColorTexture = {};
                    material.thickness *= unitsPerMeter;
                    material.attenuationDistance *= unitsPerMeter;
                }
                if(!handle.SetMaterial(material)) return 1;
                updated[materialIndex] = true;
            }
        }
        if(wine && scene.PointLights().empty() && scene.SpotLights().empty())
        {
            ModelData lights;
            Math::Bounds3 bounds;
            const auto halfSize = float3(1, 1, 1) * (asset.boundsLargestAxis * .5f);
            bounds.min = asset.boundsCenter - halfSize;
            bounds.max = asset.boundsCenter + halfSize;
            bounds.valid = asset.hasBounds;
            const auto path = std::filesystem::path(model.path).parent_path() / "BistroInterior.fbx";
            if(!LoadModelLights(path.string(), lights, bounds, model.loadOptions) || lights.pointLights.empty())
            {
                std::fputs("Wine interior requires analytic lights from BistroInterior.fbx.\n", stderr);
                return 1;
            }
            const float scale = Math::Length(Math::TransformVector(sourceToWorld, {1, 0, 0}));
            for(auto light : lights.pointLights)
            {
                light.position = Math::TransformPoint(sourceToWorld, light.position);
                light.range *= scale;
                light.sourceRadius *= scale;
                light.intensity *= scale * scale;
                if(!scene.Add(light)) return 1;
            }
            std::fprintf(stderr, "Wine interior: restored %zu authored point lights from %s.\n",
                lights.pointLights.size(), path.string().c_str());
        }
        AnimationClip cameraAnimation;
        std::vector<NodeTransform> cameraBindPose, cameraLocalPose;
        const double authoredCameraSeconds = tourSeconds;
        float authoredCameraFov = 50;
        float cameraFarPlane = 160;
        const auto sampleAuthoredCamera = [&](double seconds, Shot& result) {
            std::copy(cameraBindPose.begin(), cameraBindPose.end(), cameraLocalPose.begin());
            const float clipTime = cameraAnimation.duration * float(std::clamp(seconds / authoredCameraSeconds, 0.0, 1.0));
            if(!SampleAnimationClip(cameraAnimation, clipTime, cameraLocalPose)) return false;
            auto transform = sourceToWorld;
            for(const auto& local : cameraLocalPose) transform = transform * ComposeTransform(local);
            const auto eye = Math::TransformPoint(transform, {0, 0, 0});
            result = {seconds, eye, eye + Math::Normalize(Math::TransformVector(transform, {1, 0, 0})),
                authoredCameraFov, Math::Normalize(Math::TransformVector(transform, {0, 1, 0}))};
            return true;
        };
        if(bistro || emerald)
        {
            if(emerald)
                std::fputs("Emerald Day: original Camera node path and Y-up meter coordinates; vertical FOV 50 degrees is a viewer approximation. The imported model does not expose the source focal-length/film-gate settings.\n",stderr);
            const auto cameraNode = std::find_if(asset.nodes.begin(), asset.nodes.end(), [](const ModelNode& node) {
                return node.name == "Camera";
            });
            if(cameraNode == asset.nodes.end())
            {
                std::fprintf(stderr,"%s has no imported Camera node; an authored camera is required.\n",filename.c_str());
                return 1;
            }
            const uint32_t cameraIndex = static_cast<uint32_t>(cameraNode - asset.nodes.begin());
            std::vector<uint32_t> cameraChain;
            for(int32_t node = static_cast<int32_t>(cameraIndex); node >= 0; node = asset.nodes[node].parentIndex)
            {
                if(static_cast<size_t>(node) >= asset.nodes.size() ||
                    std::find(cameraChain.begin(), cameraChain.end(), static_cast<uint32_t>(node)) != cameraChain.end()) return 1;
                cameraChain.push_back(static_cast<uint32_t>(node));
            }
            std::reverse(cameraChain.begin(), cameraChain.end());
            for(uint32_t node : cameraChain)
            {
                if(asset.nodes[node].parentIndex < -1) return 1;
                cameraBindPose.push_back(asset.nodes[node].bindTransform);
            }
            cameraLocalPose = cameraBindPose;
            for(const auto& clip : asset.animations)
            {
                if(clip.duration <= 0 || !std::any_of(clip.tracks.begin(), clip.tracks.end(), [&](const NodeAnimationTrack& track) {
                    return std::find(cameraChain.begin(), cameraChain.end(), track.nodeIndex) != cameraChain.end() &&
                        (track.translations.size() > 1 || track.rotations.size() > 1 || track.scales.size() > 1);
                })) continue;
                cameraAnimation.name = clip.name;
                cameraAnimation.duration = clip.duration;
                for(const auto& track : clip.tracks)
                {
                    const auto node = std::find(cameraChain.begin(), cameraChain.end(), track.nodeIndex);
                    if(node == cameraChain.end()) continue;
                    cameraAnimation.tracks.push_back(track);
                    cameraAnimation.tracks.back().nodeIndex = static_cast<uint32_t>(node - cameraChain.begin());
                }
                std::fprintf(stderr, "%s authored camera: %s | %.1f s rescaled to %.0f s.\n",
                    filename.c_str(), clip.name.c_str(), clip.duration, authoredCameraSeconds);
                break;
            }
            if(cameraAnimation.tracks.empty())
                std::fputs("No camera or ancestor animation found; holding the authored Camera node pose (Tab for free camera).\n", stderr);
            // Evaluate original position/quaternion keys each frame rather than
            // resampling eye/target/up to 301 shots (only 10 Hz over a 30 s tour).
            // The node chain retains FBX units, pivots, parent motion and roll.
            // FBX cameras use node origin/+X/+Y, not legacy Camera attributes.
            Shot openingPose;
            if(!sampleAuthoredCamera(0, openingPose)) return 1;
            shots = {openingPose, openingPose};
            shots.back().seconds = tourSeconds;
            const auto& opening = shots.front();
            const auto sceneCenter = model.normalize ? model.position :
                Math::TransformPoint(Math::Translation(model.position) * model.transform, asset.boundsCenter);
            std::fprintf(stderr, "%s normalized camera eye=(%.4f %.4f %.4f) target=(%.4f %.4f %.4f) | scene bounds center=(%.4f %.4f %.4f), largest dimension=%.4f\n",filename.c_str(),
                opening.eye.x, opening.eye.y, opening.eye.z, opening.target.x, opening.target.y, opening.target.z,
                sceneCenter.x, sceneCenter.y, sceneCenter.z, model.normalize ? model.normalizedSize : asset.boundsLargestAxis);
        }
        else if(filename.find("Sponza") == std::string::npos)
        {
            shots.clear();
            for(int i = 0; i <= 12; ++i)
            {
                const float angle = 2 * pi * i / 12;
                shots.push_back({25.0 * i, {72 * std::cos(angle), 72 * std::sin(angle), 22}, {0, 0, 0}, 50});
            }
            std::fputs("Generic Y-up scene: exterior orbit. Tab enables free camera.\n", stderr);
        }
        const double timelineScale = tourSeconds / shots.back().seconds;
        for(auto& shot : shots) shot.seconds *= timelineScale;
        const auto sampleTour=[&](double seconds) {
            if(!cameraPath.empty()) return comparisonPath.Sample(seconds);
            Shot shot;
            if(bistro || emerald)
            {
                if(!sampleAuthoredCamera(seconds,shot)) throw std::runtime_error("Camera sampling failed");
                return shot;
            }
            size_t segment=0;
            while(segment+2<shots.size() && seconds>shots[segment+1].seconds) ++segment;
            const auto& a=shots[segment];const auto& b=shots[segment+1];
            float t=float((seconds-a.seconds)/(b.seconds-a.seconds));
            if(shots.size()==10) t=t*t*(3-2*t);
            return Shot{seconds,a.eye+(b.eye-a.eye)*t,a.target+(b.target-a.target)*t,a.fov+(b.fov-a.fov)*t,
                Math::NormalizeOr(a.up+(b.up-a.up)*t,{0,0,1})};
        };
        if(!cameraPath.empty())
        {
            comparisonPath.Load(cameraPath);
            tourSeconds=comparisonPath.Duration();
            shots.front()=comparisonPath.Sample(0);
        }
        if(!exportCameraPath.empty())
        {
            if(std::filesystem::exists(exportCameraPath))
                throw std::runtime_error("Camera export already exists: "+exportCameraPath);
            std::ofstream output(exportCameraPath);
            if(!output) throw std::runtime_error("Cannot write camera export: "+exportCameraPath);
            output<<"# source="<<model.path<<"\n# centered, largest axis 80, Z-up, 1920x1080, near .03, far "
                <<cameraFarPlane<<"\n# seconds,eyeX,eyeY,eyeZ,targetX,targetY,targetZ,fovDegrees,upX,upY,upZ\n";
            output<<std::setprecision(17);
            const uint64_t intervals=static_cast<uint64_t>(std::ceil(tourSeconds*60));
            for(uint64_t i=0;i<=intervals;++i)
            {
                const double seconds=std::min(double(i)/60,tourSeconds);
                const auto shot=sampleTour(seconds);
                output<<seconds<<','<<shot.eye.x<<','<<shot.eye.y<<','<<shot.eye.z<<','
                    <<shot.target.x<<','<<shot.target.y<<','<<shot.target.z<<','<<shot.fov<<','
                    <<shot.up.x<<','<<shot.up.y<<','<<shot.up.z<<'\n';
            }
            output.close();
            if(!output) throw std::runtime_error("Camera export write failed");
            std::fprintf(stderr,"Camera export written: %s; no window or GPU device created.\n",exportCameraPath.c_str());
            return 0;
        }
        DirectionalLight sun;
        sun.direction = {-.38f, -.55f, .74f};
        sun.color = {1, .91f, .78f};
        sun.intensity = 3.5f;
        sun.shadowStrength = .85f;
        // 태양의 약 0.53도 시직경에서 각반경을 구한다.
        sun.angularRadius = .00465f;
        if(!bistroInterior && !emerald && scene.DirectionalLights().empty() && scene.PointLights().empty() && scene.SpotLights().empty() &&
            !scene.Add(sun)) return 1;
        if((bistro || emerald) && !bistroInterior)
            for(uint32_t i=0;i<scene.DirectionalLights().size();++i)
            {
                auto handle=scene.GetLight<DirectionalLight>(i);
                auto light=handle.Get();
                if(light.angularRadius==0) light.angularRadius=sun.angularRadius;
                if(!handle.Set(light)) return 1;
            }
        if(lightRadius>0)
        {
            // 원본에 없는 발광 크기는 추측하지 않고 명시한 반경만 사용한다.
            for(uint32_t i=0;i<scene.PointLights().size();++i)
            {
                auto handle=scene.GetLight<PointLight>(i);auto light=handle.Get();
                light.sourceRadius=lightRadius;if(!handle.Set(light)) return 1;
            }
            for(uint32_t i=0;i<scene.SpotLights().size();++i)
            {
                auto handle=scene.GetLight<SpotLight>(i);auto light=handle.Get();
                light.sourceRadius=lightRadius;if(!handle.Set(light)) return 1;
            }
        }

        constexpr uint32_t renderWidth=1920,renderHeight=1080;
        Platform::Window window(renderWidth, renderHeight, "dy_engine / Cinematic Showcase - Space pause | R restart | Tab free camera");
#ifdef DY_PROFILER_TRACING_ENABLED
        dyf::TracyAdapter tracy;
#endif
        if(!window.GetHandle()) return 1;
        RendererConfig config;
        config.renderWidth = renderWidth;
        config.renderHeight = renderHeight;
        config.clearColor = {.10f, .15f, .22f, 1};
        config.lighting.shadows = true;
        // HDR 환경·프로브의 확산광과 균일한 상수 주변광을 중복해서 더하지 않는다.
        config.lighting.ambientIntensity = 0;
        config.lighting.shadowQuality = shadowQuality;
        config.lighting.shadowDistance = shadowDistance;
        config.lighting.cascadeSplitLambda = shadowSplit;
        config.lighting.maxDirectionalShadowLights = directionalShadowLights;
        config.lighting.maxPointShadowLights = pointShadowLights;
        config.lighting.maxSpotShadowLights = spotShadowLights;
        const char* environmentPath = emerald ? DY_SHOWCASE_EMERALD_ENVIRONMENT : DY_SHOWCASE_DEFAULT_ENVIRONMENT;
        if(!bistroInterior && !LoadHdrImage(environmentPath, config.lighting.environment.radiance))
        {
            std::fprintf(stderr,"Could not load the scene's configured equirectangular HDR sky: %s\n",environmentPath);
            return 1;
        }
        if(bistro)
        {
            // Preserve the exterior bundle's sky/sun ratio, then expose scene-linear
            // radiance once in the filmic pass. Indoor lights use their own exposure.
            config.lighting.environment.intensity = bistroInterior ? 1.0f : 10.0f;
            config.lighting.environment.background = !bistroInterior;
            config.exposure = bistroInterior ? .12f : .04f;
        }
        config.enableHdrRendering = true;
        config.hdrRenderScale = 1.0f;
        config.advanced.enabled = true;
        config.advanced.temporalAA = false;
        config.advanced.transparencyLayers = glassLayers;
        if(legacyComparison)
        {
            config.enableLod=false;
            std::fputs("Legacy comparison: shared Sponza/60 s path, 1920x1080, TAA off, original LOD0 geometry.\n",stderr);
        }
        // Preserve the authored sun/sky radiance; meter exposure in the HDR graph.
        config.advanced.autoExposure = emerald;
        // The room probe supplies reflected diffuse illumination without noisy
        // screen-space emitter samples or a second copy of that same bounce.
        if(bistroInterior) config.advanced.diffuseGI = false;
        config.enableProfilerHud = false;
        config.enableParallelRenderGraph = true;
        config.enableGpuCulling = true;
        config.enableOcclusionCulling = true;
        if(cpuCulling) config.enableGpuCulling = config.enableOcclusionCulling = false;
        config.recordingThreadCount = recordingWorkers;
        config.texturePreparationThreadCount = 4;
        if(benchmarkRequested) config.vsync = config.enableProfilerHud = false;
        // The sink and its callback context outlive renderer/device worker shutdown,
        // including exception paths. Tracy above also outlives this temporary adapter.
        std::unique_ptr<BenchmarkTrace> benchmarkTrace;
        std::unique_ptr<RHI::IDevice> diagnosticDevice;
        if(framesInFlight != 2)
        {
            RHI::DeviceDesc desc;
            desc.maxFramesInFlight = framesInFlight;
            diagnosticDevice.reset(RHI::IDevice::Create(desc));
            if(!diagnosticDevice) return 1;
        }
        auto renderer = diagnosticDevice
            ? Renderer::Create(*diagnosticDevice, window.GetHandle(), config)
            : Renderer::Create(window.GetHandle(), config);
        if(!renderer) return 1;
        if(!renderer->AddExtension(std::make_unique<dyf::AdvancedRendering>())) return 1;
        std::vector<EnvironmentProbeDesc> cachedProbes;
        if(bistroInterior || (bistro && bakeSpatialProbes))
        {
            Math::Bounds3 room;
            for(uint32_t i = 0; i < scene.GetEntityCount(); ++i)
            {
                const auto entity = static_cast<EntityID>(i);
                const auto mesh = ToIndex(scene.GetEntityMesh(entity));
                if(mesh >= scene.Meshes().size() || !scene.Meshes()[mesh]) continue;
                const auto& transform = scene.GetTransform(entity);
                for(const auto& vertex : scene.Meshes()[mesh]->vertices)
                    room.Include(Math::TransformPoint(transform, vertex.position));
            }
            const auto span = room.max - room.min;
            if(!room.valid || !std::isfinite(span.x) || !std::isfinite(span.y) || !std::isfinite(span.z) ||
                span.x <= 0 || span.y <= 0 || span.z <= 0)
            {
                std::fputs("Bistro spatial probes require finite three-dimensional scene bounds.\n", stderr);
                return 1;
            }
            const auto halfBox = span * .22f;
            const double captureTimes[] = {0, tourSeconds * .5, tourSeconds * .85};
            const uint32_t captureCount = bakeSpatialProbes ? 3u : 1u;
            cachedProbes.reserve(captureCount);
            std::fprintf(stderr,"Preparing Bistro lighting: %u probes, 2 HDR captures each, 256 px faces; startup only, actor excluded.\n",captureCount);
            for(uint32_t i = 0; i < captureCount; ++i)
            {
                Shot shot;
                shot=sampleTour(captureTimes[i]);
                EnvironmentProbeDesc probe;
                const auto margin = span * .001f;
                probe.position = {
                    std::clamp(shot.eye.x, room.min.x + margin.x, room.max.x - margin.x),
                    std::clamp(shot.eye.y, room.min.y + margin.y, room.max.y - margin.y),
                    std::clamp(shot.eye.z, room.min.z + margin.z, room.max.z - margin.z)};
                if(Math::LengthSquared(probe.position - shot.eye) > 1e-10f)
                    std::fprintf(stderr,"Probe %u: authored camera %.1f s lies outside scene bounds; eye (%.3f %.3f %.3f) moved inside to (%.3f %.3f %.3f).\n",
                        i + 1,captureTimes[i],shot.eye.x,shot.eye.y,shot.eye.z,probe.position.x,probe.position.y,probe.position.z);
                probe.boxMin = probe.position - halfBox;
                probe.boxMax = probe.position + halfBox;
                probe.fadeDistance = std::min({halfBox.x,halfBox.y,halfBox.z}) * .45f;
                probe.intensity = 1;
                std::fprintf(stderr,"Probe %u/%u at tour %.1f s, world (%.3f %.3f %.3f): specular HDR capture 1/2...\n",
                    i + 1,captureCount,captureTimes[i],probe.position.x,probe.position.y,probe.position.z);
                if(!renderer->CaptureEnvironment(scene,probe.position,256,.03f,160,probe.radiance,true,&probe.visibility)) return 1;
                std::fprintf(stderr,"Probe %u/%u: reflected diffuse HDR capture 2/2, emitters excluded...\n",i + 1,captureCount);
                if(!renderer->CaptureEnvironment(scene,probe.position,256,.03f,160,probe.diffuseRadiance,false)) return 1;
                cachedProbes.push_back(std::move(probe));
            }
            config = renderer->GetConfig();
            auto& environment = config.lighting.environment;
            if(bistroInterior)
            {
                // 첫 캡처를 기존 실내 전역 대체 경로와 공유하며 별도로 다시 캡처하지 않는다.
                environment.radiance = cachedProbes.front().radiance;
                environment.diffuseRadiance = cachedProbes.front().diffuseRadiance;
                environment.localProbe = true;
                environment.probePosition = cachedProbes.front().position;
                environment.probeBoxMin = room.min;
                environment.probeBoxMax = room.max;
                environment.intensity = 1;
                environment.rotationRadians = 0;
                environment.background = false;
            }
            if(bakeSpatialProbes) environment.probes = cachedProbes;
            else cachedProbes.clear();
            if(!renderer->SetConfig(config)) return 1;
            if(bakeSpatialProbes)
                std::fprintf(stderr,"Bistro spatial lighting ready: %zu local 1024 x 512 HDR probes, overlapping influence boxes and global fallback. I toggles cached probes; no per-frame capture.\n",
                    cachedProbes.size());
            else std::fputs("Interior room fallback ready; spatial probe baking disabled by --no-probes.\n",stderr);
        }
        else if(bistro)
            std::fputs("Bistro spatial probe baking skipped by --no-probes; preserving the original global HDR environment.\n",stderr);
        showcase::AnimatedActor actor;
        if(!actor.Load(scene,actorDescription,actorClip,shots.front().eye,shots.front().target,actorPositioned)) return 1;
        ModelRenderer modelRenderer(*renderer);
        Camera camera;
        Platform::Time clock;
        Shot pose = shots.front();
        double tourTime=std::clamp(initialTourTime,0.0,tourSeconds),sceneTime=tourTime,metricsTime=0;
        float yaw = 0, pitch = 0;
        uint32_t frames = 0;
        bool paused=initiallyPaused,freeCamera=false,metrics=false,ready=false;
        bool windEnabled = !windObjects.empty();
        showcase::PerformanceComparison comparison;
        Shot baselinePose = pose;
        double baselineTourTime = 0, baselineSceneTime = 0;
        bool baselineFreeCamera = false;
        bool baselineWindEnabled = windEnabled;
        // Indoor point shadows are camera independent; exterior CSM is not.
        unsigned comparisonMode = emerald ? 3u : bistroInterior ? 0u : 1u;
        const char* const comparisonLabels[] = {"STATIC SHADOW CACHE", "PARALLEL RECORDING", "INSTANCING", "INDEXED LOD"};
        std::fputs("Space pause tour/actor/wind | R restart | Tab tour/free | RMB look | WASD/QE move | Shift faster | N wind\n"
            "Left/Right seek 5 s | Home/End first/last pose | U GPU frustum/LOD | O Hi-Z occlusion\n"
            "1 lit/base-color | 2 shadows | 3 HDR | 4 CPU culling | 5 instancing | 6 parallel recording\n"
            "7 AO | 8 screen-space GI | 9 reflections | 0 TAA | B bloom | T transparency | G advanced raster\n"
            "V surface view: color / normal / roughness / metallic / depth / motion / off\n"
            "L indexed LOD | C LOD colors (green/yellow/orange/purple = 0/1/2/3) | F9 wireframe | [ / ] LOD pixel error\n"
            "F1 direct light | F2 diffuse IBL | F3 specular IBL | F4 sky background\n"
            "F volumetric fog | K sharpening | J clustered lighting | I spatial probes\n"
            "P soft shadows / fixed PCF | F10 one/two glass layers\n"
            "- / + exposure | M submitted frames/s | F11 profiler | Escape close\n"
            "F8 comparison: static shadows / parallel recording / instancing / LOD\n"
            "F5 baseline OFF | F6 candidate ON at saved view | F7 clear comparison\n"
            "Preparing first frame: uploading scene textures and meshes...\n", stderr);

        if(benchmarkRequested)
        {
            const double benchmarkStartTime = tourTime;
            pose = sampleTour(tourTime);
            if(!camera.LookAt(pose.eye, pose.target, pose.up) ||
                !camera.SetPerspective(float(renderWidth) / renderHeight, pose.fov * pi / 180, .03f, cameraFarPlane)) return 1;
            benchmarkTrace=std::make_unique<BenchmarkTrace>(benchmarkCpuZones);
            auto& trace=*benchmarkTrace;
            trace.diagnostics.reserve(size_t(benchmarkFrames)*(benchmarkCpuZones?128:7));
            std::vector<BenchmarkFrame> measured;
            measured.reserve(benchmarkFrames);
            std::vector<std::pair<uint32_t,Platform::ProfilerTimingSnapshot>> windows;
            uint64_t submitted = 0, lastSequence = renderer->GetProfilerTimingSnapshot().sequence;
            bool discardBoundaryWindow = true;
            int measuredWidth = 0, measuredHeight = 0;
            if(!renderer->GetDevice().WaitIdle()) throw std::runtime_error("Benchmark startup GPU drain failed.");
            if(benchmarkStep == 0)
                std::fprintf(stderr,"Benchmark: %u warm-up + %u measured submissions, fixed scene/camera/actor/wind at %.6f s; VSync/HUD off.\n",
                    benchmarkWarmup,benchmarkFrames,tourTime);
            else
                std::fprintf(stderr,"Benchmark: %u warm-up submissions at %.6f s, then %u measured submissions at %.9f scene s/submission; tour clamps at %.6f s; VSync/HUD off.\n",
                    benchmarkWarmup,benchmarkStartTime,benchmarkFrames,benchmarkStep,tourSeconds);
            Platform::CpuTimer submissionIntervalTimer, measurementTimer;
            while(measured.size() < benchmarkFrames)
            {
                window.PollEvents();
                if(!window.IsRunning() || window.GetInput().WasPressed(Key::Escape))
                    throw std::runtime_error("Benchmark interrupted before all measured submissions completed.");
                const auto size = window.GetFramebufferSize();
                if(window.IsMinimized() || size.width <= 0 || size.height <= 0)
                    throw std::runtime_error("Benchmark requires a visible, non-minimized window.");
                if(submitted >= benchmarkWarmup && measured.empty())
                { measuredWidth = size.width; measuredHeight = size.height; }
                if(!measured.empty() && (size.width != measuredWidth || size.height != measuredHeight))
                    throw std::runtime_error("Benchmark output dimensions changed during measurement.");
                Platform::CpuTimer frameTimer;
                if(benchmarkStep > 0 && submitted >= benchmarkWarmup)
                {
                    // Advance only after a successful measured submission. Acquire retries
                    // therefore resample exactly the same authored camera/actor/wind pose.
                    tourTime = sceneTime = std::min(tourSeconds,benchmarkStartTime + double(measured.size())*benchmarkStep);
                    pose = sampleTour(tourTime);
                    if(!camera.LookAt(pose.eye,pose.target,pose.up) ||
                        !camera.SetPerspective(float(renderWidth)/renderHeight,pose.fov*pi/180,.03f,cameraFarPlane)) return 1;
                }
                if(!actor.Update(scene,sceneTime)) return 1;
                for(const auto& object : windObjects)
                {
                    const auto center = object.bounds.Center();
                    const float3 pivot{center.x, center.y, object.bounds.min.z};
                    const float phase = pivot.x * .61f + pivot.y * .43f, t = float(sceneTime);
                    const float breeze = .65f * std::sin(t * .9f + phase) +
                        .25f * std::sin(t * 1.7f + phase * .61f) + .1f * std::sin(t * 2.6f + phase * .37f);
                    const auto sway = Math::Translation(pivot) *
                        Math::RotationAxis({-.42f, .9f, 0}, windEnabled ? object.amplitude * breeze : 0) *
                        Math::RotationAxis({.9f, .42f, 0}, windEnabled ? object.amplitude * .25f * std::sin(t * 1.13f + phase * .83f) : 0) *
                        Math::Translation(pivot * -1.0f);
                    for(const auto& part : object.parts)
                    { auto entity = part.first; if(!entity.SetTransform(windEnabled ? sway * part.second : part.second)) return 1; }
                }
                const auto previous = renderer->GetLastRenderCompletion();
                trace.received = false;
                trace.diagnosticSubmission = submitted >= benchmarkWarmup ? static_cast<uint32_t>(measured.size()+1) : 0;
                Platform::CpuTimer renderTimer;
                if(actor.Loaded()) { if(!modelRenderer.Render(scene,camera)) return 1; }
                else if(!renderer->Render(scene,camera)) return 1;
                BenchmarkFrame frame;
                frame.renderWall = renderTimer.ElapsedMilliseconds();
                frame.wall = frameTimer.ElapsedMilliseconds();
                frame.sceneTime = sceneTime;
                const auto completion = renderer->GetLastRenderCompletion();
                if(!completion || completion == previous) { Platform::Window::WaitEvents(.001); continue; }
                ++submitted;
                if(submitted <= benchmarkWarmup)
                {
                    if(submitted == benchmarkWarmup)
                    {
                        // Complete every warm-up query before the first measured render polls it.
                        if(!renderer->GetDevice().WaitIdle()) throw std::runtime_error("Benchmark warm-up GPU drain failed.");
                        lastSequence = renderer->GetProfilerTimingSnapshot().sequence;
                        std::fputs("Benchmark warm-up completed; recording measured submissions.\n",stderr);
                        submissionIntervalTimer.Reset();
                        measurementTimer.Reset();
                    }
                    continue;
                }
                // Unlike work-only timers, this includes all skipped acquire attempts,
                // event polling, host sleeps and result collection between submissions.
                frame.submissionInterval = submissionIntervalTimer.ElapsedMilliseconds();
                submissionIntervalTimer.Reset();
                frame.cpu = trace.sample;
                frame.hasCpu = trace.received;
                frame.statistics = renderer->GetLastRenderStatistics();
                measured.push_back(frame);
                const auto& timing = renderer->GetProfilerTimingSnapshot();
                if(timing.sequence != lastSequence)
                {
                    lastSequence = timing.sequence;
                    // This publication can contain warm-up CPU samples and the just-drained
                    // warm-up GPU samples. Later publications contain measured work only.
                    if(discardBoundaryWindow) discardBoundaryWindow = false;
                    else windows.emplace_back(static_cast<uint32_t>(measured.size()),timing);
                }
            }
            const double submissionSpanMilliseconds = measurementTimer.ElapsedMilliseconds();
            Platform::CpuTimer drainTimer;
            if(!renderer->GetDevice().WaitIdle()) throw std::runtime_error("Benchmark final GPU drain failed.");
            // A native Present worker can publish its final timings during the drain.
            trace.diagnosticSubmission = 0;
            const double finalDrainMilliseconds = drainTimer.ElapsedMilliseconds();
            const double completedSpanMilliseconds = measurementTimer.ElapsedMilliseconds();
            auto diagnostics=trace.TakeDiagnostics();
            if(trace.diagnosticsFailed.load(std::memory_order_relaxed)) throw std::runtime_error("Benchmark diagnostic collection failed.");
            std::ofstream output(benchmarkOutput);
            output.imbue(std::locale::classic());
            output << std::setprecision(10);
            if(!output) throw std::runtime_error("Cannot write benchmark output: " + benchmarkOutput);
            const auto& applied = renderer->GetAppliedConfig();
            output << "# cinematic benchmark v2\n# " << (benchmarkStep == 0 ? "fixed_time_seconds=" : "start_time_seconds=")
                << benchmarkStartTime << " camera_actor_wind_frozen=" << (benchmarkStep == 0) << " vsync=0 hud=0\n"
                << "# benchmark_step_seconds=" << std::setprecision(17) << benchmarkStep
                << " measured_start_time_seconds=" << benchmarkStartTime << " measured_end_time_seconds=" << sceneTime
                << " tour_duration_seconds=" << tourSeconds << " warmup_time_seconds=" << benchmarkStartTime << '\n'
                << std::setprecision(10) << "# scene=" << std::quoted(model.path)
                << " camera_path=" << std::quoted(cameraPath) << " actor=" << std::quoted(actorDescription.path)
                << " actor_clip=" << actorClip << " actor_size=" << actorDescription.normalizedSize
                << " wind=" << windEnabled << " probes=" << applied.lighting.environment.probes.size()
                << " environment=" << std::quoted(environmentPath) << '\n'
                << "# warmup_submissions=" << benchmarkWarmup << " measured_submissions=" << measured.size()
                << " render=" << renderWidth << 'x' << renderHeight << " output=" << measuredWidth << 'x' << measuredHeight << '\n'
                << "# measured_submission_wall_ms=" << submissionSpanMilliseconds
                << " final_gpu_drain_ms=" << finalDrainMilliseconds
                << " measured_through_gpu_drain_ms=" << completedSpanMilliseconds
                << " gpu_completed_submissions_per_second=" << measured.size()*1000.0/completedSpanMilliseconds << '\n'
                << "# quality hdr=" << applied.enableHdrRendering << " scale=" << applied.hdrRenderScale
                << " exposure=" << applied.exposure << " shadows=" << applied.lighting.shadows
                << " shadow_quality=" << int(applied.lighting.shadowQuality) << " shadow_distance=" << applied.lighting.shadowDistance
                << " shadow_split=" << applied.lighting.cascadeSplitLambda << " glass_layers=" << applied.advanced.transparencyLayers
                << " advanced=" << applied.advanced.enabled << " ao=" << applied.advanced.ambientOcclusion
                << " gi=" << applied.advanced.diffuseGI << " ssr=" << applied.advanced.reflections
                << " taa=" << applied.advanced.temporalAA << " bloom=" << applied.advanced.bloom
                << " transparency=" << applied.advanced.transparency << " auto_exposure=" << applied.advanced.autoExposure << '\n'
                << "# paths gpu_culling=" << applied.enableGpuCulling << " occlusion=" << applied.enableOcclusionCulling
                << " cpu_culling=" << applied.enableFrustumCulling << " instancing=" << applied.enableInstancing
                << " lod=" << applied.enableLod << " lod_error=" << applied.lodPixelError
                << " parallel=" << applied.enableParallelRenderGraph << " workers=" << applied.recordingThreadCount
                << " live_profiling=";
#ifdef DY_LIVE_PROFILING_ENABLED
            output << 1;
#else
            output << 0;
#endif
            output << " frame_cpu_trace=";
#ifdef DY_PROFILER_TRACING_ENABLED
            output << 1;
#else
            output << 0;
#endif
            output << " benchmark_cpu_zones=" << benchmarkCpuZones << "\n# arguments=";
            for(int i=1;i<argc;++i) output << std::quoted(argv[i]) << ' ';
            const auto& deviceDescription = renderer->GetDevice().GetDesc();
            output << "\n# device adapter=" << deviceDescription.adapterIndex
                << " frames_in_flight=" << deviceDescription.maxFramesInFlight
                << " validation=" << deviceDescription.enableValidation
                << " timestamp_supported=" << renderer->GetDevice().Supports(RHI::Feature::TimestampQuery)
                << " timestamp_bits=" << renderer->GetDevice().GetTimestampValidBits()
                << " timestamp_period_ns=" << renderer->GetDevice().GetTimestampPeriodNanoseconds();
            output << '\n' << "# camera_view=";
            for(float value : camera.view.m) output << value << ' ';
            output << "\n# camera_projection=";
            for(float value : camera.projection.m) output << value << ' ';
            output << "\n# submission_interval_ms includes retries, event polling, host waits and result collection; first interval starts after warm-up drain.\n"
                << "# Work-only wall fields include only the successful actor/wind update and Render/Present attempt; skipped attempts are excluded.\n"
                << "# Completed-submission throughput includes the final GPU drain, excludes warm-up, and does not measure display presentation completion.\n"
                << "# cpu/gpu window rows are native profiler publication averages, not current-frame timings.\n"
                << "# First boundary publication excluded after warm-up GPU drain; unpublished trailing samples omitted.\n"
                << "# GPU samples are completed timestamp spans, exclude presentation; GPU percentiles unavailable from aggregate API.\n"
                << "# Empty frame CPU fields require a build with tracing enabled; empty GPU-selected counts are unknown.\n";
            output << "# diagnostic rows use gpu_pass_name/gpu_pass_ms for raw native-call, completed-GPU-sample, or explicitly labeled CPU-zone durations.\n"
                << "# Their index is the host submission attempt observing the value, not the measured GPU/present frame; GPU and asynchronous present diagnostics can be delayed.\n"
                << "# GPU diagnostic samples may include warm-up boundary samples; final present-worker timings are included during the drain.\n";
            if(benchmarkCpuZones)
                output << "# CPU zone diagnostics measure existing RendererShadow/RendererMainForward callbacks, Native replay, and Validate + native submit scopes.\n"
                    << "# Their index is captured at zone begin, including acquire retries; all replay samples are separate rows without per-pass attribution.\n"
                    << "# Surface/Transparent callback scopes are uninstrumented; their work can be included in anonymous native replay rows.\n"
                    << "# CPU zones may overlap on workers: do not sum them as frame time or interpret them as GPU timings/FPS.\n"
                    << "# Optional zone token bookkeeping/timing adds CPU profiling overhead; ordinary benchmark runs keep the previous zone callbacks.\n";
            enum Column { Kind, Index, SceneTime, Interval, Wall, RenderWall, Cpu, Acquire, Prepare, Record, Submit, Present,
                Sequence, CpuSamples, Gpu, GpuSamples, Draws, ShadowDraws, Instances, Triangles,
                ShadowTriangles, Culled, Batches, GpuVisibility, GpuOcclusion, ShadowReuse,
                RenderWidth, RenderHeight, OutputWidth, OutputHeight, Pass, PassMs, PassSamples, ColumnCount };
            output << "record_type,index,scene_time_seconds,submission_interval_ms,frame_work_wall_ms,render_work_wall_ms,cpu_render_ms,cpu_acquire_ms,cpu_prepare_ms,cpu_record_ms,cpu_submit_ms,cpu_present_ms,profiler_sequence,cpu_samples,gpu_frame_ms,gpu_samples,main_draws,shadow_draws,main_instances,main_triangles,shadow_triangles,culled_entities,indirect_batches,gpu_visibility,gpu_occlusion,shadow_reused,render_width,render_height,output_width,output_height,gpu_pass_name,gpu_pass_ms,gpu_pass_samples\n";
            using Row = std::array<std::string,ColumnCount>;
            const auto set = [](Row& row,Column column,const auto& value) {
                std::ostringstream text; text.imbue(std::locale::classic()); text << std::setprecision(10) << value;
                row[column] = text.str();
            };
            const auto write = [&](const Row& row) {
                for(size_t i=0;i<row.size();++i)
                {
                    if(i) output << ',';
                    if(row[i].find_first_of(",\"\r\n") == std::string::npos) output << row[i];
                    else { output << '"'; for(char c : row[i]) { if(c=='"') output << '"'; output << c; } output << '"'; }
                }
                output << '\n';
            };
            std::vector<double> intervals, walls, renders, cpus;
            intervals.reserve(measured.size()); walls.reserve(measured.size()); renders.reserve(measured.size()); cpus.reserve(measured.size());
            for(size_t i=0;i<measured.size();++i)
            {
                const auto& frame = measured[i]; const auto& s = frame.statistics;
                Row row = {}; row[Kind] = "frame"; set(row,Index,i+1);
                { std::ostringstream time; time.imbue(std::locale::classic()); time << std::setprecision(17) << frame.sceneTime; row[SceneTime] = time.str(); }
                set(row,Interval,frame.submissionInterval); set(row,Wall,frame.wall); set(row,RenderWall,frame.renderWall);
                intervals.push_back(frame.submissionInterval); walls.push_back(frame.wall); renders.push_back(frame.renderWall);
                if(frame.hasCpu)
                {
                    set(row,Cpu,frame.cpu.cpuRenderMilliseconds); set(row,Acquire,frame.cpu.cpuAcquireMilliseconds);
                    set(row,Prepare,frame.cpu.cpuPrepareMilliseconds); set(row,Record,frame.cpu.cpuRecordMilliseconds);
                    set(row,Submit,frame.cpu.cpuSubmitMilliseconds); set(row,Present,frame.cpu.cpuPresentMilliseconds);
                    set(row,CpuSamples,1); cpus.push_back(frame.cpu.cpuRenderMilliseconds);
                }
                set(row,Draws,s.mainDrawCount); set(row,ShadowDraws,s.shadowDrawCount); set(row,ShadowTriangles,s.shadowTriangleCount);
                if(!s.gpuVisibility) { set(row,Instances,s.mainInstanceCount); set(row,Triangles,s.mainTriangleCount); set(row,Culled,s.culledEntityCount); }
                set(row,Batches,s.indirectBatchCount); set(row,GpuVisibility,s.gpuVisibility); set(row,GpuOcclusion,s.gpuOcclusion);
                set(row,ShadowReuse,s.reusedShadowAtlas); set(row,RenderWidth,s.renderWidth); set(row,RenderHeight,s.renderHeight);
                set(row,OutputWidth,s.outputWidth); set(row,OutputHeight,s.outputHeight); write(row);
            }
            for(const auto& diagnostic : diagnostics)
            {
                Row row={};row[Kind]="diagnostic";set(row,Index,diagnostic.submission);
                row[Pass]=diagnostic.name;set(row,PassMs,diagnostic.milliseconds);write(row);
            }
            double gpuTotal = 0, cpuTotal = 0; uint64_t gpuCount = 0, cpuCount = 0;
            for(const auto& windowSample : windows)
            {
                const auto& s = windowSample.second;
                Row row = {}; row[Kind] = "profiler_window"; set(row,Index,windowSample.first); set(row,Sequence,s.sequence);
                set(row,CpuSamples,s.frameCount); set(row,Cpu,s.cpuRenderAverageMilliseconds);
                set(row,Acquire,s.cpuAcquireAverageMilliseconds); set(row,Prepare,s.cpuPrepareAverageMilliseconds);
                set(row,Record,s.cpuRecordAverageMilliseconds); set(row,Submit,s.cpuSubmitAverageMilliseconds);
                set(row,Present,s.cpuPresentAverageMilliseconds);
                cpuTotal += s.cpuRenderAverageMilliseconds*s.frameCount; cpuCount += s.frameCount;
                if(s.hasGpuFrame && s.gpuFrameSampleCount)
                {
                    set(row,Gpu,s.gpuFrameAverageMilliseconds); set(row,GpuSamples,s.gpuFrameSampleCount);
                    gpuTotal += s.gpuFrameAverageMilliseconds*s.gpuFrameSampleCount; gpuCount += s.gpuFrameSampleCount;
                }
                write(row);
                for(const auto& pass : s.gpuPasses)
                {
                    Row passRow = {}; passRow[Kind] = "gpu_pass"; set(passRow,Index,windowSample.first);
                    set(passRow,Sequence,s.sequence); passRow[Pass] = pass.name;
                    set(passRow,PassMs,pass.averageMilliseconds); set(passRow,PassSamples,pass.sampleCount); write(passRow);
                }
            }
            BenchmarkSummary(output,"submission_interval",intervals);
            BenchmarkSummary(output,"frame_work_wall",walls); BenchmarkSummary(output,"render_work_wall",renders);
            BenchmarkSummary(output,"per_frame_cpu_render",cpus);
            output << "# summary profiler_cpu samples=" << cpuCount;
            if(cpuCount) output << " mean_ms=" << cpuTotal/cpuCount;
            else output << " unavailable";
            output << "\n# summary profiler_gpu samples=" << gpuCount;
            if(gpuCount) output << " mean_ms=" << gpuTotal/gpuCount;
            else output << " unavailable";
            output << '\n';
            output.close();
            if(!output) throw std::runtime_error("Benchmark output write failed: " + benchmarkOutput);
            BenchmarkSummary(std::cout,"submission_interval",intervals);
            BenchmarkSummary(std::cout,"frame_work_wall",walls); BenchmarkSummary(std::cout,"render_work_wall",renders);
            std::fprintf(stderr,"Benchmark CSV written: %s (%zu measured submissions, %zu interior profiler windows, %llu GPU samples).\n",
                benchmarkOutput.c_str(),measured.size(),windows.size(),static_cast<unsigned long long>(gpuCount));
            return 0;
        }

        while(window.IsRunning())
        {
            window.PollEvents();
#ifdef DY_RENDERDOC_ENABLED
            if(window.GetInput().WasPressed(dyf::Platform::Key::F12))
                (void)dyf::RenderDocCapture::TriggerNextFrame();
#endif
            if(window.GetInput().WasPressed(dyf::Platform::Key::F11))
            {
                auto settings = renderer->GetConfig();
                settings.profilerStartsExpanded = !settings.profilerStartsExpanded;
                if(!renderer->SetConfig(settings)) return 1;
            }

            const auto& input = window.GetInput();
            if(input.WasPressed(Key::Escape)) window.RequestClose();
            if(!window.IsRunning()) break;
            const auto size = window.GetFramebufferSize();
            const bool focused = window.HasFocus();
            // Present once before requiring focus: Wayland maps the first buffer.
            const bool suspended = window.IsMinimized() || size.width <= 0 || size.height <= 0 || (ready && !focused);
            const double elapsed = clock.Tick(suspended || !focused);
            if(suspended)
            {
                comparison.Cancel("window inactive or minimized");
                window.SetCursorMode(Platform::CursorMode::Normal);
                Platform::Window::WaitEvents();
                continue;
            }
            const auto previousPose = pose;
            bool historyDiscontinuity = false;
            if(input.WasPressed(Key::F7)) comparison.Clear();
            if(input.WasPressed(Key::F8))
            {
                comparison.Clear();
                comparisonMode = (comparisonMode + 1) % 4;
                std::fprintf(stderr, "Comparison mode: %s\n", comparisonLabels[comparisonMode]);
            }
            const bool captureA = input.WasPressed(Key::F5);
            const bool captureB = input.WasPressed(Key::F6);
            if(input.WasPressed(Key::N))
            {
                comparison.Clear();
                windEnabled = !windEnabled && !windObjects.empty();
                if(windObjects.empty())
                    std::fputs("No eligible exterior leaf/flower groups in this scene.\n", stderr);
                else std::fprintf(stderr, "Wind %s | %zu leaf/flower groups.\n", windEnabled ? "on" : "off", windObjects.size());
            }
            if(captureA)
            {
                baselinePose = pose;
                baselineTourTime = tourTime;
                baselineSceneTime = sceneTime;
                baselineFreeCamera = freeCamera;
                baselineWindEnabled = windEnabled;
            }
            if(captureB && comparison.HasBaseline())
            {
                pose = baselinePose;
                tourTime = baselineTourTime;
                sceneTime = baselineSceneTime;
                freeCamera = baselineFreeCamera;
                windEnabled = baselineWindEnabled;
                historyDiscontinuity = true;
            }
            if(captureA || (captureB && comparison.HasBaseline())) paused = true;
            if(input.WasPressed(Key::Space) || input.WasPressed(Key::R) || input.WasPressed(Key::Tab) ||
                input.IsDown(MouseButton::Right) || input.IsDown(Key::W) || input.IsDown(Key::A) ||
                input.IsDown(Key::S) || input.IsDown(Key::D) || input.IsDown(Key::Q) || input.IsDown(Key::E) ||
                input.WasPressed(Key::Left) || input.WasPressed(Key::Right) ||
                input.WasPressed(Key::Home) || input.WasPressed(Key::End))
                comparison.Cancel("camera or tour input");
            if(input.WasPressed(Key::Space))
            {
                paused = !paused;
                std::fprintf(stderr, "Tour %s at %.1f s%s\n", paused ? "paused" : "enabled", tourTime,
                    freeCamera ? " | free camera active; Tab returns to tour" : "");
            }
            const bool restart = input.WasPressed(Key::R);
            if(restart)
            {
                tourTime = 0;
                sceneTime = 0;
                paused = freeCamera = false;
                historyDiscontinuity = true;
                std::fputs("Tour restarted (playing).\n", stderr);
            }
            const bool seek = input.WasPressed(Key::Left) || input.WasPressed(Key::Right) ||
                input.WasPressed(Key::Home) || input.WasPressed(Key::End);
            if(seek)
            {
                if(input.WasPressed(Key::Home)) tourTime = 0;
                else if(input.WasPressed(Key::End)) tourTime = tourSeconds;
                else tourTime = std::clamp(tourTime + (input.WasPressed(Key::Right) ? 5.0 : -5.0), 0.0, tourSeconds);
                sceneTime = tourTime;
                freeCamera = false;
                historyDiscontinuity = true;
                std::fprintf(stderr,"Tour seek: %.1f/%.0f s.\n",tourTime,tourSeconds);
            }
            if(input.WasPressed(Key::Tab))
            {
                freeCamera = !freeCamera;
                historyDiscontinuity = true;
                const auto forward = Math::Normalize(pose.target - pose.eye);
                yaw = std::atan2(forward.y, forward.x);
                pitch = std::asin(std::clamp(forward.z, -1.0f, 1.0f));
                std::fprintf(stderr, "Camera: %s | tour %s at %.1f s\n",
                    freeCamera ? "free (WASD/QE move, RMB look; Tab returns)" : "tour", paused ? "paused" : "enabled", tourTime);
            }
            if(ready && !paused && !freeCamera && !restart && !seek && !comparison.IsRunning()) tourTime = std::min(tourSeconds, tourTime + elapsed);
            // 배우와 바람은 자유 카메라·투어 종료 뒤에도 진행하고 Space·포커스 상실·A/B에서 멈춘다.
            if(ready && !paused && !restart && !seek && !comparison.IsRunning() && !captureA &&
                !(captureB && comparison.HasBaseline()) && (windEnabled || actor.Loaded())) sceneTime += elapsed;
            if(!actor.Update(scene,sceneTime)) return 1;
            for(const auto& object : windObjects)
            {
                const auto center = object.bounds.Center();
                const float3 pivot{center.x, center.y, object.bounds.min.z};
                const float phase = pivot.x * .61f + pivot.y * .43f;
                const float t = float(sceneTime);
                const float breeze = .65f * std::sin(t * .9f + phase) +
                    .25f * std::sin(t * 1.7f + phase * .61f) + .1f * std::sin(t * 2.6f + phase * .37f);
                const auto sway = Math::Translation(pivot) *
                    Math::RotationAxis({-.42f, .9f, 0}, windEnabled ? object.amplitude * breeze : 0) *
                    Math::RotationAxis({.9f, .42f, 0}, windEnabled ? object.amplitude * .25f * std::sin(t * 1.13f + phase * .83f) : 0) *
                    Math::Translation(pivot * -1.0f);
                for(const auto& part : object.parts)
                {
                    auto entity = part.first;
                    if(!entity.SetTransform(windEnabled ? sway * part.second : part.second)) return 1;
                }
            }
            window.SetCursorMode(freeCamera && input.IsDown(MouseButton::Right) ?
                Platform::CursorMode::Locked : Platform::CursorMode::Normal);
            if(comparison.IsRunning() || captureA || (captureB && comparison.HasBaseline()))
            {
                // Keep the exact authored/free-camera pose throughout both captures.
                pose = baselinePose;
            }
            else if(freeCamera)
            {
                if(input.IsDown(MouseButton::Right))
                {
                    const auto mouse = input.GetCursorDelta();
                    yaw -= float(mouse.x) * .0025f;
                    pitch = std::clamp(pitch - float(mouse.y) * .0025f, -1.5f, 1.5f);
                }
                const float3 forward{std::cos(pitch) * std::cos(yaw), std::cos(pitch) * std::sin(yaw), std::sin(pitch)};
                const auto right = Math::Normalize(Math::Cross(forward, {0, 0, 1}));
                const auto movement = forward * (float(input.IsDown(Key::W)) - float(input.IsDown(Key::S))) +
                    right * (float(input.IsDown(Key::D)) - float(input.IsDown(Key::A))) +
                    float3(0, 0, float(input.IsDown(Key::E)) - float(input.IsDown(Key::Q)));
                const float speed = input.IsDown(Key::LeftShift) || input.IsDown(Key::RightShift) ? 32.0f : 8.0f;
                pose.eye = pose.eye + Math::Normalize(movement) * (speed * float(std::min(elapsed, .1)));
                pose.target = pose.eye + forward;
                pose.up = {0, 0, 1};
            }
            else
            {
                pose=sampleTour(tourTime);
            }
            // 큰 시점 단절은 이전 색·조명 이력과 이전 변형 위치를 함께 폐기한다.
            if(ready && !freeCamera &&
                (Math::Length(pose.eye - previousPose.eye) > std::max(2.f,float(elapsed) * 64.f) ||
                 Math::Dot(Math::NormalizeOr(pose.target - pose.eye,{0,1,0}),
                    Math::NormalizeOr(previousPose.target - previousPose.eye,{0,1,0})) < .5f))
                historyDiscontinuity = true;
            if(historyDiscontinuity)
            {
                modelRenderer.InvalidateHistory();
                renderer->InvalidateHistory();
            }
            if(!camera.LookAt(pose.eye, pose.target, pose.up) ||
                !camera.SetPerspective(float(config.renderWidth) / config.renderHeight, pose.fov * pi / 180, .03f, cameraFarPlane)) return 1;

            config = renderer->GetConfig();
            bool changed = false;
            for(const auto& event : input.GetEvents())
            {
                if(event.type != Platform::InputEventType::Key || event.action != Platform::InputAction::Press) continue;
                switch(static_cast<Key>(event.code))
                {
                case Key::Num1: config.lighting.enabled = !config.lighting.enabled; break;
                case Key::Num2: config.lighting.shadows = !config.lighting.shadows; break;
                case Key::Num3: config.enableHdrRendering = !config.enableHdrRendering; break;
                case Key::Num4: config.enableFrustumCulling = !config.enableFrustumCulling; break;
                case Key::Num5: config.enableInstancing = !config.enableInstancing; break;
                case Key::Num6: config.enableParallelRenderGraph = !config.enableParallelRenderGraph; break;
                case Key::U: config.enableGpuCulling = !config.enableGpuCulling; break;
                case Key::O: config.enableOcclusionCulling = !config.enableOcclusionCulling; break;
                case Key::Num7: config.advanced.ambientOcclusion = !config.advanced.ambientOcclusion; break;
                case Key::Num8: config.advanced.diffuseGI = !config.advanced.diffuseGI; break;
                case Key::Num9: config.advanced.reflections = !config.advanced.reflections; break;
                case Key::Num0: config.advanced.temporalAA = !config.advanced.temporalAA; break;
                case Key::B: config.advanced.bloom = !config.advanced.bloom; break;
                case Key::F: config.advanced.fog.enabled = !config.advanced.fog.enabled; break;
                case Key::K: config.postProcess.sharpening = !config.postProcess.sharpening; break;
                case Key::J: config.lighting.clusteredLighting = !config.lighting.clusteredLighting; break;
                case Key::P: config.lighting.shadowMaxFilterRadius = config.lighting.shadowMaxFilterRadius>0 ? 0.f : 12.f; break;
                case Key::F10: config.advanced.transparencyLayers = config.advanced.transparencyLayers==1 ? 2u : 1u; break;
                case Key::I:
                    if(!bakeSpatialProbes)
                    {
                        std::fputs("Restart without --no-probes to bake spatial probes.\n",stderr);
                        continue;
                    }
                    if(cachedProbes.empty())
                    {
                        std::fputs("No cached spatial probes for this scene; global environment remains active.\n",stderr);
                        continue;
                    }
                    if(config.lighting.environment.probes.empty()) config.lighting.environment.probes = cachedProbes;
                    else config.lighting.environment.probes.clear();
                    break;
                case Key::T: config.advanced.transparency = !config.advanced.transparency; break;
                case Key::V: config.advanced.surfaceDebugView = static_cast<SurfaceDebugView>(
                    (static_cast<unsigned>(config.advanced.surfaceDebugView) + 1) % 7); break;
                case Key::G: config.advanced.enabled = !config.advanced.enabled; break;
                case Key::F1: config.lighting.direct = !config.lighting.direct; break;
                case Key::F2: config.lighting.environment.diffuse = !config.lighting.environment.diffuse; break;
                case Key::F3: config.lighting.environment.specular = !config.lighting.environment.specular; break;
                case Key::F4: config.lighting.environment.background = !config.lighting.environment.background; break;
                case Key::F11: config.enableProfilerHud = !config.enableProfilerHud; break;
                case Key::L: config.enableLod = !config.enableLod; break;
                case Key::C: config.debugLodColor = !config.debugLodColor; break;
                case Key::F9:
                    if(!config.debugWireframe && (!config.enableHdrRendering || !config.advanced.enabled))
                    {
                        std::fputs("Wireframe view requires HDR (3) and advanced raster (G).\n", stderr);
                        continue;
                    }
                    if(!config.debugWireframe && !renderer->GetDevice().Supports(RHI::Feature::Wireframe))
                    {
                        std::fputs("Native wireframe is unavailable on this device; use C LOD colors and F11 triangle counts.\n", stderr);
                        continue;
                    }
                    config.debugWireframe = !config.debugWireframe;
                    break;
                case Key::LeftBracket: config.lodPixelError = std::max(.125f, config.lodPixelError * .5f); break;
                case Key::RightBracket: config.lodPixelError = std::min(16.0f, config.lodPixelError * 2); break;
                default: continue;
                }
                changed = true;
            }
            if(input.WasPressed(Key::Minus) || input.WasPressed(Key::Equal))
            {
                config.exposure = std::clamp(config.exposure * (input.WasPressed(Key::Equal) ? 1.15f : 1 / 1.15f), .001f, 20.0f);
                changed = true;
            }
            if(changed)
            {
                comparison.Cancel("render settings changed");
                if(!renderer->SetConfig(config)) return 1;
                std::fprintf(stderr, "Shading %s | shadows %d | HDR %d | CPU culling %d | instancing %d | parallel %d | exposure %.2f\n",
                    config.lighting.enabled ? "lit" : "base-color", config.lighting.shadows, config.enableHdrRendering,
                    config.enableFrustumCulling, config.enableInstancing, config.enableParallelRenderGraph, config.exposure);
                std::fprintf(stderr, "Advanced %d | AO %d | SSGI %d | SSR %d | TAA %d | bloom %d | transparency %d\n",
                    config.advanced.enabled, config.advanced.ambientOcclusion, config.advanced.diffuseGI,
                    config.advanced.reflections, config.advanced.temporalAA, config.advanced.bloom, config.advanced.transparency);
                std::fprintf(stderr,"Fog %d | sharpening %d (%.2f) | clustered lighting %d\n",
                    config.advanced.fog.enabled,config.postProcess.sharpening,config.postProcess.sharpness,
                    config.lighting.clusteredLighting);
                std::fprintf(stderr,"Soft shadows %d (%u search / %u filter, max %.1f texels) | glass layers %u\n",
                    config.lighting.shadowMaxFilterRadius>0,config.lighting.shadowBlockerSamples,
                    config.lighting.shadowFilterSamples,config.lighting.shadowMaxFilterRadius,config.advanced.transparencyLayers);
                std::fprintf(stderr, "Direct %d | diffuse IBL %d | specular IBL %d | sky %d\n",
                    config.lighting.direct, config.lighting.environment.diffuse,
                    config.lighting.environment.specular, config.lighting.environment.background);
                std::fprintf(stderr,"Spatial probes %zu/%zu | outside influence boxes: %s\n",
                    config.lighting.environment.probes.size(),cachedProbes.size(),bistroInterior?"existing room probe":"global HDR environment");
                std::fprintf(stderr, "Surface view: %s\n", surfaceViews[static_cast<unsigned>(config.advanced.surfaceDebugView)]);
                std::fprintf(stderr, "LOD %d | pixel error %.2f | colors %d | wireframe %d\n",
                    config.enableLod, config.lodPixelError, config.debugLodColor, config.debugWireframe);
                std::fprintf(stderr,"GPU frustum/LOD %d | Hi-Z occlusion %d | GPU-selected visible object/triangle counts: N/A (no count readback).\n",
                    config.enableGpuCulling,config.enableOcclusionCulling);
            }
            if(input.WasPressed(Key::M))
            {
                metrics = !metrics;
                metricsTime = 0;
                frames = 0;
                std::fprintf(stderr, "Console metrics: %s\n", metrics ? "on" : "off");
            }
            const auto conditions = [&]()
            {
                const auto& current = config;
                std::ostringstream text;
                text.precision(17);
                text << model.path << " cameraPath=" << cameraPath << " " << size.width << 'x' << size.height << " mode=" << comparisonMode
                    << " render=" << current.renderWidth << 'x' << current.renderHeight
                    << " sceneTime=" << sceneTime << " wind=" << windEnabled
                    << " actor=" << actorDescription.path << " actorClip=" << actorClip
                    << " actorSize=" << actorDescription.normalizedSize << " actorTime=" << sceneTime
                    << " actorFeet=" << actorDescription.position.x << ',' << actorDescription.position.y << ',' << actorDescription.position.z
                    << " vsync=" << current.vsync << " hdr=" << current.enableHdrRendering
                    << " scale=" << current.hdrRenderScale << " exposure=" << current.exposure
                    << " autoExposure=" << current.advanced.autoExposure
                    << " exposureKey=" << current.advanced.exposureKey
                    << " exposureAdaptation=" << current.advanced.exposureAdaptationSpeed
                    << " fog=" << current.advanced.fog.enabled << ',' << current.advanced.fog.density
                    << ',' << current.advanced.fog.heightFalloff << ',' << current.advanced.fog.baseHeight
                    << ',' << current.advanced.fog.albedo.x << ',' << current.advanced.fog.albedo.y << ',' << current.advanced.fog.albedo.z
                    << ',' << current.advanced.fog.anisotropy << ',' << current.advanced.fog.maxDistance
                    << ',' << current.advanced.fog.resolutionScale << ',' << current.advanced.fog.steps
                    << " sharpening=" << current.postProcess.sharpening << ',' << current.postProcess.sharpness
                    << " clustered=" << current.lighting.clusteredLighting
                    << " hud=" << current.enableProfilerHud << " lighting=" << current.lighting.enabled
                    << current.lighting.direct << current.lighting.shadows << int(current.lighting.shadowQuality)
                    << " shadowDistance=" << current.lighting.shadowDistance
                    << " shadowSplit=" << current.lighting.cascadeSplitLambda
                    << " softShadows=" << current.lighting.shadowBlockerSamples << ','
                    << current.lighting.shadowFilterSamples << ',' << current.lighting.shadowMaxFilterRadius
                    << " localLightRadius=" << lightRadius
                    << " shadowBudget=" << current.lighting.maxDirectionalShadowLights << ','
                    << current.lighting.maxPointShadowLights << ',' << current.lighting.maxSpotShadowLights
                    << " env=" << current.lighting.environment.diffuse << current.lighting.environment.specular
                    << current.lighting.environment.background << " probeBake=" << bakeSpatialProbes
                    << " spatialProbes=" << current.lighting.environment.probes.size()
                    << " advanced=" << current.advanced.enabled
                    << current.advanced.ambientOcclusion << current.advanced.diffuseGI << current.advanced.reflections
                    << current.advanced.temporalAA << current.advanced.bloom << current.advanced.transparency
                    << " glassLayers=" << current.advanced.transparencyLayers << " surface="
                    << int(current.advanced.surfaceDebugView) << " cull=" << current.enableFrustumCulling
                    << " gpuCull=" << current.enableGpuCulling << " hiZ=" << current.enableOcclusionCulling
                    << " workers=" << current.recordingThreadCount;
                text << " lodError=" << current.lodPixelError << " lodHysteresis=" << current.lodHysteresis
                    << " lodColors=" << current.debugLodColor << " wireframe=" << current.debugWireframe;
                for(const auto& probe:current.lighting.environment.probes)
                    text << " probe=" << probe.position.x << ',' << probe.position.y << ',' << probe.position.z
                        << ',' << probe.boxMin.x << ',' << probe.boxMin.y << ',' << probe.boxMin.z
                        << ',' << probe.boxMax.x << ',' << probe.boxMax.y << ',' << probe.boxMax.z
                        << ',' << probe.fadeDistance << ',' << probe.intensity;
                if(comparisonMode != 0) text << " cache=" << current.lighting.reuseStaticShadows;
                if(comparisonMode != 1) text << " parallel=" << current.enableParallelRenderGraph;
                if(comparisonMode != 2) text << " instancing=" << current.enableInstancing;
                if(comparisonMode != 3) text << " lod=" << current.enableLod;
                for(float value : camera.view.m) text << ' ' << value;
                for(float value : camera.projection.m) text << ' ' << value;
                return text.str();
            };
            if(captureA || captureB)
            {
                // Keep profiler drawing out of both benchmark runs. Timing
                // collection remains active; F11 opens the breakdown afterwards.
                config.enableProfilerHud = false;
                if(config.debugLodColor || config.debugWireframe)
                    std::fputs("Disable C LOD colors and F9 wireframe before collecting normal-rendering performance.\n", stderr);
                else if(comparisonMode == 0 && (!bistroInterior || !config.lighting.enabled ||
                    !config.lighting.direct || !config.lighting.shadows))
                    std::fputs("Static-shadow comparison needs the interior point lights and enabled shadows. Use F8 for another mode.\n", stderr);
                else if(comparison.Begin(captureB, conditions(), comparisonLabels[comparisonMode]))
                {
                    if(comparisonMode == 0) config.lighting.reuseStaticShadows = captureB;
                    if(comparisonMode == 1) config.enableParallelRenderGraph = captureB;
                    if(comparisonMode == 2) config.enableInstancing = captureB;
                    if(comparisonMode == 3) config.enableLod = captureB;
                    if(!renderer->SetConfig(config)) return 1;
                }
            }
            comparison.CheckConditions(conditions());
            const auto previous = renderer->GetLastRenderCompletion();
            if(comparison.IsRunning() || comparison.HasBaseline())
            {
                Canvas overlay(uint32_t(size.width), uint32_t(size.height));
                comparison.Draw(overlay, uint32_t(size.width), uint32_t(size.height));
                if(actor.Loaded())
                { if(!modelRenderer.Render(scene,camera,overlay)) return 1; }
                else if(!renderer->Render(scene, camera, overlay)) return 1;
            }
            else if(actor.Loaded())
            { if(!modelRenderer.Render(scene,camera)) return 1; }
            else if(!renderer->Render(scene, camera)) return 1;
            const auto completion = renderer->GetLastRenderCompletion();
            comparison.Update(elapsed, bool(completion && completion != previous),
                renderer->GetProfilerTimingSnapshot(), renderer->GetLastRenderStatistics());
            if(completion && completion != previous && metrics) ++frames;
            if(!ready && completion)
            {
                ready = true;
                clock.Reset();
                std::fputs("First frame submitted.\n", stderr);
            }
            if(metrics) metricsTime += elapsed;
            if(metrics && metricsTime >= 1)
            {
                std::fprintf(stderr, "Tour %.1f/%.0f s | %s | submitted frames %.1f/s\n",
                    tourTime, tourSeconds, freeCamera ? "free" : paused ? "paused" : tourTime == tourSeconds ? "ended" : "playing", frames / metricsTime);
                const auto& statistics = renderer->GetLastRenderStatistics();
                if(statistics.gpuVisibility)
                    std::fprintf(stderr,"GPU visibility | indirect batches %u | visible objects/triangles N/A (no count readback).\n",
                        statistics.indirectBatchCount);
                metricsTime = 0;
                frames = 0;
            }
            if(completion == previous) Platform::Window::WaitEvents(.001);
        }
        return 0;
    }
    catch(const std::exception& error)
    {
        std::fprintf(stderr, "Cinematic Showcase: %s\n", error.what());
        return 1;
    }
}
