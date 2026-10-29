#include "../../examples/HostedExample.h"
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
#include "../PlatformProfile.h"
#include "../ShowcaseHud.h"
#include "AnimatedActor.h"
#include "BistroNight.h"
#include "KnightMaterials.h"
#include "../CameraPath.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
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
#ifndef DY_SHOWCASE_BISTRO_EXTERIOR_SCENE
#define DY_SHOWCASE_BISTRO_EXTERIOR_SCENE ""
#endif
#ifndef DY_SHOWCASE_BISTRO_WINE_SCENE
#define DY_SHOWCASE_BISTRO_WINE_SCENE ""
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
    const bool wsiTiming;
    const bool gpuTiming;
    struct NativeTiming {dyf::Platform::FrameTimingEvent event;std::string name;};
    std::vector<NativeTiming> nativeTimings;
    inline static std::atomic<BenchmarkTrace*> active{nullptr};
    dyf::Platform::TraceSink sink = {};
    const dyf::Platform::TraceSink* previous = nullptr;
    bool attached=false;
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
    static void CaptureGpuTiming(const dyf::Platform::GpuTimingSample& sample) noexcept
    {
        auto* trace=active.load(std::memory_order_acquire);
        if(!trace)return;
        const auto submission=trace->diagnosticSubmission.load(std::memory_order_acquire);
        if(submission)
        {
            trace->AddDiagnostic(submission,"GPU split frame span",sample.frameMilliseconds);
            trace->AddDiagnostic(submission,"GPU split upload to graph",sample.uploadToGraphMilliseconds);
            trace->AddDiagnostic(submission,"GPU split graph",sample.graphMilliseconds);
        }
        if(trace->previous && trace->previous->gpuTiming)trace->previous->gpuTiming(sample);
    }
    static void CaptureFrameTiming(const dyf::Platform::FrameTimingEvent& event) noexcept
    {
        auto* trace=active.load(std::memory_order_acquire);
        if(!trace)return;
        try {
            std::lock_guard<std::mutex> lock(trace->diagnosticsMutex);
            trace->nativeTimings.push_back({event,event.name?event.name:""});
        } catch(...) {trace->diagnosticsFailed.store(true,std::memory_order_relaxed);}
        if(trace->previous && trace->previous->frameTiming)trace->previous->frameTiming(event);
    }
#ifdef DY_PROFILER_TRACING_ENABLED
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
    explicit BenchmarkTrace(bool captureCpuZones,bool captureWsiTiming,bool captureGpuTiming)
        : cpuZones(captureCpuZones),wsiTiming(captureWsiTiming),gpuTiming(captureGpuTiming)
    {
        bool attach=wsiTiming || gpuTiming;
#ifdef DY_PROFILER_TRACING_ENABLED
        attach=true;
#endif
        if(!attach)return;
        previous = dyf::Platform::SetTraceSink(nullptr);
        if(previous) sink = *previous;
#ifdef DY_PROFILER_TRACING_ENABLED
        sink.frameSample = Capture;
        sink.value = CaptureValue;
        if(cpuZones) { sink.beginZone=CaptureBegin; sink.endZone=CaptureEnd; }
#endif
        if(wsiTiming)sink.frameTiming=CaptureFrameTiming;
        if(gpuTiming)sink.gpuTiming=CaptureGpuTiming;
        active.store(this,std::memory_order_release);
        (void)dyf::Platform::SetTraceSink(&sink);
        attached=true;
    }
    ~BenchmarkTrace()
    {
        if(!attached)return;
        (void)dyf::Platform::SetTraceSink(previous);
        active.store(nullptr,std::memory_order_release);
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
        bool night=false;
        std::string scenePreset="exterior";
        bool presetSpecified=false;
        uint32_t benchmarkFrames = 0, benchmarkWarmup = 120;
        double benchmarkStep = 0;
        bool benchmarkRequested = false;
        bool benchmarkCpuZones = false;
        bool benchmarkHud = false;
        bool benchmarkWsiTiming = false;
        bool benchmarkGpuTiming = false;
        bool cpuCulling = false;
        auto profile = showcase::PlatformProfile::Automatic();
        uint32_t recordingWorkers = 0;
        bool shadowQualitySpecified = false;
        uint32_t framesInFlight = 2;
        unsigned requestedComparisonMode = UINT32_MAX;
        std::string benchmarkOutput;
        std::string benchmarkCaptureDirectory;
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
            std::fputs("Usage: showcase [exterior|night|interior|emerald|sponza|comparison|scene.gltf|scene.glb|scene.fbx|scene.obj] [options]\n"
                "  --scene-preset NAME     none, exterior, interior, emerald, sponza; explicit source conventions/camera/lighting\n"
                "  night / --night          Bistro exterior with dark sky and showcase-authored practical lights\n"
                "  comparison               Same Sponza asset and 60 s CSV as the legacy-renderer branch\n"
                "  --actor FILE|knight      Original animated glTF/GLB/FBX, Y-up; knight uses Intel's original FBX\n"
                "  --actor-position X Y Z   Actor feet in normalized Z-up world; default: opening view\n"
                "  --actor-size N           Actor largest dimension (default 2.4)\n"
                "  --actor-clip N           Zero-based animation clip (default 0)\n"
                "  --comparison-mode NAME  Initial F5/F6 comparison: shadows, parallel, instancing, lod, skinning\n"
                "  --path FILE              Shared comparison CSV, seconds/eye/target/FOV/up (11 columns)\n"
                "  --export-camera FILE     Write a 60 Hz camera CSV without creating a window or GPU device\n"
                "  --start N --paused       Hold an identical comparison timestamp in both renderers\n"
                "  --benchmark-frames N     Submit N measured frames starting at --start, then exit\n"
                "  --benchmark-warmup N     Submit N warm-up frames and drain GPU work first (default 120)\n"
                "  --benchmark-step N       Scene seconds per measured submission (default 0 = frozen); warm-up stays at --start\n"
                "                          Pose k uses min(tour duration, start + k*step), independent of render speed/retries\n"
                "  --benchmark-output FILE  New CSV: wall/statistics per frame, CPU/GPU profiler windows separately\n"
                "  --benchmark-capture-dir DIR  Write first/middle/last measured frames as PPM; use a separate quality run\n"
                "                          HUD is hidden by default; mobile keeps VSync. Selected profile quality is preserved\n"
                "  --benchmark-hud          Show the normal FPS/GPU HUD and run progress with fixed controls\n"
                "  --benchmark-cpu-zones    Optional CPU Shadow/MainForward/native replay/submit diagnostics (requires tracing)\n"
                "  --benchmark-wsi-timing   Write native frame-tagged host timings to output.wsi.csv\n"
                "  --benchmark-gpu-timing   Split completed GPU spans using existing upload/graph timestamps\n"
                "  --profile MODE           auto (default), desktop, mobile; mobile uses 720p/512px textures\n"
                "  --recording-workers N    CPU recording workers, 1..64 (desktop up to 4, mobile up to 2)\n"
                "  --frames-in-flight N     Maximum GPU frames in flight, 1..4 (default 2)\n"
                "  --cpu-culling            Use CPU frustum culling/instancing; disable GPU culling and Hi-Z occlusion\n"
                "  --no-probes              Skip 3 spatial probe bakes; interior keeps its single room fallback\n"
                "  --no-gui                 Legacy compatibility flag; the Canvas HUD is always used\n"
                "  --shadow-quality MODE    low, medium, high (desktop high, mobile medium)\n"
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
            if(argument == "--no-gui") continue; // Keep older host/launch arguments compatible with the Canvas HUD.
            if(argument == "--no-probes") bakeSpatialProbes = false;
            else if(argument == "--profile" && i + 1 < argc)
                profile = showcase::PlatformProfile::Parse(argv[++i]);
            else if(argument == "--scene-preset" && i + 1 < argc)
            { scenePreset=argv[++i];presetSpecified=true; }
            else if(argument == "--night") night=true;
            else if(argument == "--cpu-culling") cpuCulling = true;
            else if(argument == "--comparison-mode" && i + 1 < argc)
            {
                const std::string mode = argv[++i];
                if(mode == "shadows") requestedComparisonMode = 0;
                else if(mode == "parallel") requestedComparisonMode = 1;
                else if(mode == "instancing") requestedComparisonMode = 2;
                else if(mode == "lod") requestedComparisonMode = 3;
                else if(mode == "skinning") requestedComparisonMode = 4;
                else throw std::runtime_error("Unknown comparison mode: " + mode);
            }
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
            else if(argument == "--benchmark-capture-dir" && i + 1 < argc)
            { benchmarkRequested = true; benchmarkCaptureDirectory = argv[++i]; }
            else if(argument == "--benchmark-hud")
            { benchmarkRequested = true; benchmarkHud = true; }
            else if(argument == "--benchmark-cpu-zones")
            { benchmarkRequested = true; benchmarkCpuZones = true; }
            else if(argument == "--benchmark-wsi-timing")
            { benchmarkRequested = true; benchmarkWsiTiming = true; }
            else if(argument == "--benchmark-gpu-timing")
            { benchmarkRequested = true; benchmarkGpuTiming = true; }
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
                shadowQualitySpecified = true;
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
            { model.path = argument; sceneSpecified = true;if(!presetSpecified)scenePreset="none"; }
            else { usage(); return 1; }
        }
        if(benchmarkRequested && (!benchmarkFrames || benchmarkOutput.empty() || !exportCameraPath.empty()))
            throw std::runtime_error("Benchmark requires positive --benchmark-frames and --benchmark-output; camera export is a separate mode.");
        if(benchmarkRequested && std::filesystem::exists(benchmarkOutput))
            throw std::runtime_error("Benchmark output already exists: " + benchmarkOutput);
        if(benchmarkWsiTiming && std::filesystem::exists(benchmarkOutput+".wsi.csv"))
            throw std::runtime_error("WSI timing output already exists: " + benchmarkOutput+".wsi.csv");
#ifndef DY_PROFILER_TRACING_ENABLED
        if(benchmarkCpuZones) throw std::runtime_error("--benchmark-cpu-zones requires a build with profiler tracing enabled.");
#endif
        if(model.path=="night") {night=true;model.path="exterior";}
        if(!presetSpecified && (model.path=="exterior" || model.path=="interior" ||
            model.path=="emerald" || model.path=="sponza")) scenePreset=model.path;
        if(model.path == "exterior") model.path = DY_SHOWCASE_BISTRO_EXTERIOR_SCENE;
        if(model.path == "interior") model.path = DY_SHOWCASE_BISTRO_WINE_SCENE;
        if(model.path == "emerald") model.path = DY_SHOWCASE_EMERALD_SCENE;
        if((model.path == "sponza" || model.path == "comparison") && std::string(DY_SHOWCASE_COMPARISON_SCENE).empty())
            throw std::runtime_error("Sponza scene path is not configured. Set DY_SHOWCASE_COMPARISON_SCENE when configuring CMake, or pass a model file path directly with --scene-preset sponza.");
        if(model.path == "sponza") model.path = DY_SHOWCASE_COMPARISON_SCENE;
        if(model.path=="comparison")
        {
            legacyComparison=true;
            scenePreset="sponza";
            model.path=DY_SHOWCASE_COMPARISON_SCENE;
            if(cameraPath.empty()) cameraPath=DY_SHOWCASE_COMPARISON_CAMERA;
            if(model.path.empty() || cameraPath.empty())
                throw std::runtime_error("Sponza comparison asset or camera path is not configured");
        }
        const bool knightPreset=actorDescription.path=="knight";
        if(knightPreset)
        {
            actorDescription.path=DY_SHOWCASE_KNIGHT_ACTOR;
            if(actorDescription.path.empty())
            {
                std::fputs("Intel Knight path is unavailable; set DY_SHOWCASE_KNIGHT_ACTOR when configuring the build.\n",stderr);
                return 1;
            }
        }
        if(knightPreset)
        {
            showcase::ApplyIntelKnightMaterials(actorDescription);
            std::fputs("Intel Knight: original Maya PBR values and PNGs explicitly bound; animation remains in the original FBX.\n",stderr);
        }
        if(requestedComparisonMode == 4 && actorDescription.path.empty())
            throw std::runtime_error("The skinning comparison requires --actor knight or --actor FILE.");
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
        if(scenePreset!="none" && scenePreset!="exterior" && scenePreset!="interior" &&
            scenePreset!="emerald" && scenePreset!="sponza")
            throw std::runtime_error("Unknown scene preset: "+scenePreset);
        const bool bistro = scenePreset=="exterior" || scenePreset=="interior";
        const bool wine = scenePreset=="interior";
        const bool bistroInterior = scenePreset=="interior";
        const bool emerald = scenePreset=="emerald";
        std::fprintf(stderr,"Scene preset: %s (material interpretation never depends on the filename).\n",scenePreset.c_str());
        if(night && (!bistro || bistroInterior))
            throw std::runtime_error("Night lighting is a Bistro exterior preset.");
        model.normalizedSize = 80;
        // ufbx preserves source axes; Bistro, Emerald Day and New Sponza are Y-up.
        model.transform = Math::RotationX(pi * .5f);
        model.loadOptions.maxSourceBytes = 8ull * 1024 * 1024 * 1024;
        model.loadOptions.maxParserInputBytes = model.loadOptions.maxSourceBytes;
        model.loadOptions.maxParserBytes = model.loadOptions.maxSourceBytes;
        model.loadOptions.maxDecodedBytes = model.loadOptions.maxSourceBytes;
        if(!recordingWorkers) recordingWorkers = profile.Workers();
        if(profile.mobile && !shadowQualitySpecified) shadowQuality = ShadowQuality::Medium;
        model.loadOptions.maxTextureDimension = profile.TextureLimit();
        model.loadOptions.textureDecodeThreadCount = profile.Workers();
        actorDescription.loadOptions.maxTextureDimension = profile.TextureLimit();
        actorDescription.loadOptions.textureDecodeThreadCount = profile.Workers();
        if(profile.mobile)
        {
            // Bound each import as well as its retained textures; these are ceilings,
            // not allocations, and do not describe the whole process's peak memory.
            model.loadOptions.maxSourceBytes = model.loadOptions.maxParserInputBytes = 512ull*1024*1024;
            model.loadOptions.maxParserBytes = model.loadOptions.maxDecodedBytes = 1536ull*1024*1024;
            actorDescription.loadOptions.maxSourceBytes = actorDescription.loadOptions.maxParserInputBytes = 512ull*1024*1024;
            actorDescription.loadOptions.maxParserBytes = actorDescription.loadOptions.maxDecodedBytes = 1536ull*1024*1024;
        }
        std::fprintf(stderr,"Showcase profile: %s; material texture limit=%u (0=source), recording workers=%u.\n",
            profile.Name(),profile.TextureLimit(),recordingWorkers);
        if(emerald || bistro)
        {
            // ORCA's Falcor convention: G/B = roughness/metalness, R unused;
            // texture alpha uses the reference renderer's 0.5 coverage test.
            // https://github.com/NVIDIAGameWorks/Falcor/blob/master/docs/usage/scene-formats.md
            model.loadOptions.importLights = true;
            model.loadOptions.fbxSpecularTextureIsOrm = true;
            model.loadOptions.fbxSpecularTextureHasOcclusion = false;
            model.loadOptions.fbxBaseColorTextureHasOpacity = true;
            model.loadOptions.fbxBumpTextureIsNormalMap = true;
            model.loadOptions.fbxBaseColorAlphaMode = MaterialAlphaMode::Mask;
            model.loadOptions.normalMapYSign = -1;
        }
        if(bistro)
        {
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
                optics.alphaMode = MaterialAlphaMode::Opaque;
                optics.doubleSided = true;
                // Repair dangling "map1" references on TextureUV meshes only;
                // other meshes with an authored map1 UV0 keep their source mapping.
                if(optics.name == "TransparentGlass") optics.fbxFallbackTextureUvSet = "TextureUV";
            }
        }
        ModelScene scene;
        ModelInstanceID modelInstance = ModelInstanceID::Invalid;
        std::fprintf(stderr, "Loading environment: %s\n", model.path.c_str());
        ModelAssetID environmentAsset=ModelAssetID::Invalid;
        if(!LoadModelAsset(scene,model.path,model.loadOptions,&environmentAsset)) return 1;
        model.transform=model.transform*scene.GetModelAsset(environmentAsset).sourceToYUp;
        if(!InstantiateModel(scene,environmentAsset,model,&modelInstance)) return 1;
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
                // The Wine bundle explicitly supplies this emitter calibration.
                // Analytic lights below provide illumination; emission alone does not.
                if(wine) material.emissiveColor = material.emissiveColor * 1000.0f;
                if(night) material.emissiveColor = material.emissiveColor * 40.0f;
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
        float3 authoredCameraForward={1,0,0},authoredCameraUp={0,1,0};
        float cameraFarPlane = 160;
        const auto sampleAuthoredCamera = [&](double seconds, Shot& result) {
            std::copy(cameraBindPose.begin(), cameraBindPose.end(), cameraLocalPose.begin());
            const float clipTime = cameraAnimation.duration * float(std::clamp(seconds / authoredCameraSeconds, 0.0, 1.0));
            if(!SampleAnimationClip(cameraAnimation, clipTime, cameraLocalPose)) return false;
            auto transform = sourceToWorld;
            for(const auto& local : cameraLocalPose) transform = transform * ComposeTransform(local);
            const auto eye = Math::TransformPoint(transform, {0, 0, 0});
            result = {seconds, eye, eye + Math::Normalize(Math::TransformVector(transform, authoredCameraForward)),
                authoredCameraFov, Math::Normalize(Math::TransformVector(transform, authoredCameraUp))};
            return true;
        };
        if(bistro || emerald)
        {
            const auto cameraNode = std::find_if(asset.nodes.begin(), asset.nodes.end(), [](const ModelNode& node) {
                return node.name == "Camera";
            });
            if(cameraNode == asset.nodes.end())
            {
                std::fprintf(stderr,"%s has no imported Camera node; an authored camera is required.\n",filename.c_str());
                return 1;
            }
            const uint32_t cameraIndex = static_cast<uint32_t>(cameraNode - asset.nodes.begin());
            const auto sourceCamera=std::find_if(asset.cameras.begin(),asset.cameras.end(),[&](const ModelCamera& camera) {
                return camera.nodeIndex==cameraIndex;
            });
            if(sourceCamera==asset.cameras.end() || !sourceCamera->perspective)
                throw std::runtime_error("The selected showcase preset requires an authored perspective Camera.");
            authoredCameraFov=sourceCamera->verticalFovRadians*180/pi;
            authoredCameraForward=sourceCamera->localForward;authoredCameraUp=sourceCamera->localUp;
            std::fprintf(stderr,"Source camera: vertical FOV %.6f degrees; source clipping %.9g..%.9g (viewer clipping .03..%.3f after normalization).\n",
                authoredCameraFov,sourceCamera->nearPlane,sourceCamera->farPlane,cameraFarPlane);
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
        else if(scenePreset!="sponza")
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
        if(night)
        {
            showcase::AddBistroNightLights(scene,sampleTour(0).eye);
            for(uint32_t i=0;i<scene.DirectionalLights().size();++i)
            {
                auto handle=scene.GetLight<DirectionalLight>(i);
                auto light=handle.Get();
                light.color={.40f,.55f,1.f};light.intensity=.45f;
                if(!handle.Set(light)) return 1;
            }
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

        const uint32_t renderWidth=profile.Width(),renderHeight=profile.Height();
        DY_EXAMPLE_WINDOW(window, renderWidth, renderHeight,night ? "dy_engine / Bistro Night - Space pause | R restart | Tab free camera" :
            "dy_engine / Cinematic Showcase - Space pause | R restart | Tab free camera");
#ifdef DY_PROFILER_TRACING_ENABLED
        dyf::TracyAdapter tracy;
#endif
        if(!window.GetHandle()) return 1;
        RendererConfig config;
        config.allowReadback=!benchmarkCaptureDirectory.empty();
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
        if(!bistroInterior && !night && !LoadHdrImage(environmentPath, config.lighting.environment.radiance,profile.EnvironmentLimit()))
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
        if(night)
        {
            config.lighting.environment.radiance=showcase::BistroNightSky();
            config.lighting.environment.intensity=1;
            config.lighting.environment.diffuseIntensity=2.5f;
            config.lighting.environment.background=true;
            config.exposure=.45f;
            config.clearColor={.006f,.012f,.032f,1};
            config.advanced.bloomStrength=.075f;
            std::fputs("Bistro night: daylight HDRI replaced by a dark procedural sky; weak blue moonlight, practical emitters and fixed exposure 0.45.\n",stderr);
        }
        config.enableHdrRendering = true;
        config.hdrRenderScale = 1.0f;
        config.advanced.enabled = true;
        config.advanced.temporalAA = false;
        config.advanced.transparencyLayers = glassLayers;
        if(legacyComparison)
        {
            config.enableLod=false;
            std::fprintf(stderr,"Legacy comparison: shared Sponza/60 s path, %ux%u, TAA off, original LOD0 geometry.\n",renderWidth,renderHeight);
        }
        // Preserve the authored sun/sky radiance; meter exposure in the HDR graph.
        config.advanced.autoExposure = emerald;
        // Quality captures compare the same exposure target without wall-clock adaptation drift.
        if(!benchmarkCaptureDirectory.empty())config.advanced.exposureAdaptationSpeed=1000000;
        // The room probe supplies reflected diffuse illumination without noisy
        // screen-space emitter samples or a second copy of that same bounce.
        if(bistroInterior) config.advanced.diffuseGI = false;
        config.enableProfilerHud = false;
        config.enableParallelRenderGraph = true;
        config.enableGpuCulling = true;
        config.enableOcclusionCulling = true;
        if(cpuCulling) config.enableGpuCulling = config.enableOcclusionCulling = false;
        config.recordingThreadCount = recordingWorkers;
        config.texturePreparationThreadCount = profile.Workers();
        // Preserve FIFO for normal runs; some Wayland drivers do not support Immediate.
        // Only desktop benchmarks request unpaced presentation.
        config.vsync = !benchmarkRequested || profile.mobile || window.GetNativeHandle().kind == Platform::NativeWindowKind::UIKit;
        // The sink and its callback context outlive renderer/device worker shutdown,
        // including exception paths. Tracy above also outlives this temporary adapter.
        std::unique_ptr<BenchmarkTrace> benchmarkTrace;
        std::unique_ptr<RHI::IDevice> diagnosticDevice;
        std::unique_ptr<Renderer> renderer;
        const void* rendererWindow = nullptr;
        uint64_t rendererSurfaceGeneration = 0;
        bool nativeWindowChanged = false;
        const auto createRenderer = [&](const void* nativeWindow) {
            if(renderer || diagnosticDevice)
                throw std::logic_error("Cinematic GPU owners must be released before recreation.");
            if(framesInFlight != 2)
            {
                RHI::DeviceDesc desc;
                desc.maxFramesInFlight = framesInFlight;
                diagnosticDevice.reset(RHI::IDevice::Create(desc));
                if(!diagnosticDevice) throw std::runtime_error("Cinematic device creation failed.");
            }
            renderer = diagnosticDevice
                ? Renderer::Create(*diagnosticDevice, nativeWindow, config)
                : Renderer::Create(nativeWindow, config);
            if(!renderer || !renderer->AddExtension(std::make_unique<dyf::AdvancedRendering>()))
                throw std::runtime_error("Cinematic renderer creation failed.");
            rendererWindow = nativeWindow;
            rendererSurfaceGeneration = window.GetSurfaceGeneration();
            nativeWindowChanged = false;
        };
        const auto waitForEvents = [&](double seconds = .1) {
            Platform::Window::WaitEvents(seconds);
            if(renderer && (window.GetHandle() != rendererWindow ||
                window.GetSurfaceGeneration() != rendererSurfaceGeneration)) nativeWindowChanged = true;
        };
        createRenderer(window.GetHandle());
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
            const uint32_t probeFaceSize=profile.ProbeFaceSize();
            std::fprintf(stderr,"Preparing Bistro lighting: %u probes, 2 HDR captures each, %u px faces; startup only, actor excluded.\n",captureCount,probeFaceSize);
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
                if(!renderer->CaptureEnvironment(scene,probe.position,probeFaceSize,.03f,160,probe.radiance,true,&probe.visibility)) return 1;
                std::fprintf(stderr,"Probe %u/%u: reflected diffuse HDR capture 2/2, emitters excluded...\n",i + 1,captureCount);
                if(!renderer->CaptureEnvironment(scene,probe.position,probeFaceSize,.03f,160,probe.diffuseRadiance,false)) return 1;
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
                std::fprintf(stderr,"Bistro spatial lighting ready: %zu local %u x %u HDR probes, overlapping influence boxes and global fallback. I toggles cached probes; no per-frame capture.\n",
                    cachedProbes.size(),probeFaceSize*4,probeFaceSize*2);
            else std::fputs("Interior room fallback ready; spatial probe baking disabled by --no-probes.\n",stderr);
        }
        else if(bistro)
            std::fputs("Bistro spatial probe baking skipped by --no-probes; preserving the original global HDR environment.\n",stderr);
        showcase::AnimatedActor actor;
        if(!actor.Load(scene,actorDescription,actorClip,shots.front().eye,shots.front().target,actorPositioned)) return 1;
        auto modelRenderer = std::make_unique<ModelRenderer>(*renderer);
        auto savedSkinningMode = modelRenderer->GetSkinningExecutionMode();
        auto savedModelOptimization = modelRenderer->GetOptimizationConfig();
        Camera camera;
        Platform::Time clock;
        Shot pose = shots.front();
        double tourTime=std::clamp(initialTourTime,0.0,tourSeconds),sceneTime=tourTime,metricsTime=0;
        float yaw = 0, pitch = 0;
        uint32_t frames = 0;
        bool paused=initiallyPaused,freeCamera=false,metrics=false,ready=false;
        bool windEnabled = !windObjects.empty();
        showcase::PerformanceComparison comparison;
        const auto* hostFont = std::getenv("DY_EXAMPLES_FONT");
        showcase::ShowcaseHud hud(hostFont ? hostFont : "");
        using Group=showcase::ControlGroup;
        Shot baselinePose = pose;
        double baselineTourTime = 0, baselineSceneTime = 0;
        bool baselineFreeCamera = false;
        bool baselineWindEnabled = windEnabled;
        // Indoor point shadows are camera independent; exterior CSM is not.
        unsigned comparisonMode = requestedComparisonMode != UINT32_MAX ? requestedComparisonMode :
            emerald ? 3u : bistroInterior ? 0u : 1u;
        const char* const comparisonLabels[] = {"STATIC SHADOW CACHE", "PARALLEL RECORDING", "INSTANCING", "INDEXED LOD", "CPU / GPU DEFORMATION"};
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
            "F8 comparison: static shadows / parallel recording / instancing / LOD / CPU-GPU deformation\n"
            "F5 baseline OFF (CPU for deformation) | F6 candidate ON (GPU compute) at saved view | F7 clear comparison\n"
            "Preparing first frame: uploading scene textures and meshes...\n", stderr);

        const auto releaseCpuImageCache=[&] {
            uint64_t released=0;
            for(const auto& material:scene.Materials())
                for(const Image* image:{&material.baseColorTexture,&material.metallicRoughnessTexture,
                    &material.normalTexture,&material.occlusionTexture,&material.emissiveTexture})
                    released+=image->ReleaseDecodedPixels();
            std::fprintf(stderr,"[Showcase] Released decoded DDS cache after upload: %llu bytes; original blocks retained.\n",
                static_cast<unsigned long long>(released));
        };
        if(benchmarkRequested)
        {
            const auto benchmarkLabel=std::filesystem::path(benchmarkOutput).stem().string();
            const double benchmarkStartTime = tourTime;
            pose = sampleTour(tourTime);
            if(!camera.LookAt(pose.eye, pose.target, pose.up) ||
                !camera.SetPerspective(float(renderWidth) / renderHeight, pose.fov * pi / 180, .03f, cameraFarPlane)) return 1;
            benchmarkTrace=std::make_unique<BenchmarkTrace>(benchmarkCpuZones,benchmarkWsiTiming,benchmarkGpuTiming);
            auto& trace=*benchmarkTrace;
            trace.diagnostics.reserve(size_t(benchmarkFrames)*(benchmarkCpuZones?128:7));
            if(benchmarkWsiTiming)trace.nativeTimings.reserve((size_t(benchmarkWarmup)+benchmarkFrames)*12);
            std::vector<BenchmarkFrame> measured;
            measured.reserve(benchmarkFrames);
            std::vector<std::pair<uint32_t,Platform::ProfilerTimingSnapshot>> windows;
            uint64_t submitted = 0, lastSequence = renderer->GetProfilerTimingSnapshot().sequence;
            uint32_t unfocusedMeasuredSubmissions = 0;
            bool discardBoundaryWindow = true;
            int measuredWidth = 0, measuredHeight = 0;
            if(!renderer->GetDevice().WaitIdle()) throw std::runtime_error("Benchmark startup GPU drain failed.");
            if(benchmarkStep == 0)
                std::fprintf(stderr,"Benchmark: %u warm-up + %u measured submissions, fixed scene/camera/actor/wind at %.6f s; VSync %s, HUD %s.\n",
                    benchmarkWarmup,benchmarkFrames,tourTime,renderer->GetAppliedConfig().vsync ? "on" : "off",benchmarkHud ? "on" : "off");
            else
                std::fprintf(stderr,"Benchmark: %u warm-up submissions at %.6f s, then %u measured submissions at %.9f scene s/submission; tour clamps at %.6f s; VSync %s, HUD %s.\n",
                    benchmarkWarmup,benchmarkStartTime,benchmarkFrames,benchmarkStep,tourSeconds,renderer->GetAppliedConfig().vsync ? "on" : "off",benchmarkHud ? "on" : "off");
            Platform::CpuTimer submissionIntervalTimer, measurementTimer;
            while(measured.size() < benchmarkFrames)
            {
                window.PollEvents();
                if(nativeWindowChanged || window.GetHandle() != rendererWindow ||
                    window.GetSurfaceGeneration() != rendererSurfaceGeneration)
                    throw std::runtime_error("Benchmark native surface changed; restart the measurement on the new surface.");
                if(!dy_examples::ExampleRunning(window) || dy_examples::ExampleInput(window).WasPressed(Key::Escape))
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
                const bool capture=!benchmarkCaptureDirectory.empty() && submitted>=benchmarkWarmup &&
                    (measured.empty() || measured.size()==benchmarkFrames/2 || measured.size()+1==benchmarkFrames);
                Image captured;
                if(benchmarkHud)
                {
                    hud.Update(window,{{Key::Escape,"Stop benchmark"}});
                    // Keep overlay cost and scene settings fixed throughout the comparison.
                    hud.SetProfilerVisible(true);
                    hud.ClosePanels();
                    Canvas overlay(uint32_t(size.width),uint32_t(size.height));
                    char status[160];
                    const bool warming=submitted<benchmarkWarmup;
                    std::snprintf(status,sizeof(status),"%s | %s %u / %u | Tour %.1f / %.0f s",
                        benchmarkLabel.c_str(),warming?"WARM-UP":"MEASURING",
                        warming?uint32_t(submitted+1):uint32_t(measured.size()+1),
                        warming?benchmarkWarmup:benchmarkFrames,tourTime,tourSeconds);
                    hud.Draw(overlay,*renderer,emerald?"EMERALD SQUARE":bistro?"BISTRO":"CINEMATIC",status);
                    if(actor.Loaded()) {if(!modelRenderer->Render(scene,camera,overlay,capture?&captured:nullptr))return 1;}
                    else if(!renderer->Render(scene,camera,overlay,capture?&captured:nullptr))return 1;
                }
                else if(actor.Loaded()) { if(!modelRenderer->Render(scene,camera,capture?&captured:nullptr)) return 1; }
                else if(!renderer->Render(scene,camera,capture?&captured:nullptr)) return 1;
                BenchmarkFrame frame;
                frame.renderWall = renderTimer.ElapsedMilliseconds();
                frame.wall = frameTimer.ElapsedMilliseconds();
                frame.sceneTime = sceneTime;
                const auto completion = renderer->GetLastRenderCompletion();
                if(!completion || completion == previous) { waitForEvents(.001); continue; }
                ++submitted;
                if(submitted==1)releaseCpuImageCache();
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
                if(!window.HasFocus()) ++unfocusedMeasuredSubmissions;
                measured.push_back(frame);
                if(capture)
                {
                    if(!captured.IsValid())throw std::runtime_error("Benchmark image capture failed.");
                    std::filesystem::create_directories(benchmarkCaptureDirectory);
                    const auto path=std::filesystem::path(benchmarkCaptureDirectory)/("frame-"+std::to_string(measured.size())+".ppm");
                    if(std::filesystem::exists(path))throw std::runtime_error("Benchmark image already exists: "+path.string());
                    std::ofstream image(path,std::ios::binary);
                    image<<"P6\n"<<captured.GetWidth()<<' '<<captured.GetHeight()<<"\n255\n";
                    const auto& pixels=captured.GetPixels();
                    for(size_t i=0;i<pixels.size();i+=4)image.write(reinterpret_cast<const char*>(pixels.data()+i),3);
                    if(!image)throw std::runtime_error("Benchmark image write failed.");
                }
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
                << benchmarkStartTime << " camera_actor_wind_frozen=" << (benchmarkStep == 0) << " vsync=" << applied.vsync << " hud=" << benchmarkHud << '\n'
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
                << "# window measured_focused_submissions=" << measured.size()-unfocusedMeasuredSubmissions
                << " measured_unfocused_submissions=" << unfocusedMeasuredSubmissions << '\n'
                << "# measured_submission_wall_ms=" << submissionSpanMilliseconds
                << " final_gpu_drain_ms=" << finalDrainMilliseconds
                << " measured_through_gpu_drain_ms=" << completedSpanMilliseconds
                << " gpu_completed_submissions_per_second=" << measured.size()*1000.0/completedSpanMilliseconds << '\n'
                << "# profile=" << profile.Name() << " texture_limit=" << profile.TextureLimit()
                << " environment_limit=" << profile.EnvironmentLimit() << " probe_face=" << profile.ProbeFaceSize() << '\n'
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
            if(benchmarkWsiTiming)
            {
                std::ofstream timingOutput(benchmarkOutput+".wsi.csv");
                timingOutput.imbue(std::locale::classic());
                timingOutput << "# Native BeginFrame sequence; prefetch belongs to the frame triggering it.\n"
                    << "# Includes warmup; native Present rows retain the original worker frame, regardless of delivery order.\n"
                    << "# Host call intervals, not GPU execution or display completion timestamps.\n"
                    << "source,frame,event,begin_ns,end_ns,duration_ms,result\n" << std::setprecision(12);
                for(const auto& timing:trace.nativeTimings)
                {
                    const auto& event=timing.event;
                    timingOutput << event.source << ',' << event.frame << ',' << std::quoted(timing.name) << ','
                        << event.beginNs << ',' << event.endNs << ',' << Platform::Milliseconds(event.beginNs,event.endNs)
                        << ',' << event.result << '\n';
                }
                timingOutput.close();
                if(!timingOutput)throw std::runtime_error("Cannot write WSI timing output: "+benchmarkOutput+".wsi.csv");
            }
            std::fprintf(stderr,"Benchmark CSV written: %s (%zu measured submissions, %zu interior profiler windows, %llu GPU samples).\n",
                benchmarkOutput.c_str(),measured.size(),windows.size(),static_cast<unsigned long long>(gpuCount));
            return 0;
        }

        while(dy_examples::ExampleRunning(window))
        {
            window.PollEvents();
            if(!dy_examples::BeginExampleFrame(window)) break;
            const auto& input = dy_examples::ExampleInput(window);
            if(!dy_examples::ExampleRunning(window)) break;
            const auto nativeWindow = window.GetHandle();
            if(renderer && (nativeWindowChanged || nativeWindow != rendererWindow ||
                window.GetSurfaceGeneration() != rendererSurfaceGeneration))
            {
                config = renderer->GetConfig();
                savedSkinningMode = modelRenderer->GetSkinningExecutionMode();
                savedModelOptimization = modelRenderer->GetOptimizationConfig();
                const bool drained = renderer->GetDevice().WaitIdle();
                // The model extension borrows Renderer; Renderer may borrow diagnosticDevice.
                modelRenderer.reset();
                renderer.reset();
                diagnosticDevice.reset();
                rendererWindow = nullptr;
                nativeWindowChanged = false;
                comparison.Cancel("native surface changed");
                ready = false;
                frames = 0;
                metricsTime = 0;
                clock.Reset();
                if(!drained) throw std::runtime_error("Cinematic GPU drain failed during native surface replacement.");
            }
            const auto size = window.GetFramebufferSize();
            const bool focused = window.HasFocus();
            // Present once before requiring focus: Wayland maps the first buffer.
            const bool suspended = !nativeWindow || window.IsMinimized() || size.width <= 0 || size.height <= 0 || (ready && !focused);
            const double elapsed = clock.Tick(suspended || !focused);
            if(suspended)
            {
                comparison.Cancel("window inactive or minimized");
                window.SetCursorMode(Platform::CursorMode::Normal);
                waitForEvents();
                continue;
            }
            if(!renderer)
            {
                createRenderer(nativeWindow);
                modelRenderer = std::make_unique<ModelRenderer>(*renderer);
                if(!modelRenderer->SetOptimizationConfig(savedModelOptimization) ||
                    !modelRenderer->SetSkinningExecutionMode(savedSkinningMode))
                    throw std::runtime_error("Cinematic model configuration could not be restored after native surface replacement.");
            }
            const auto& applied=renderer->GetAppliedConfig();
            const bool hadProfiler=hud.IsProfilerVisible(),hadPanel=hud.IsPanelOpen();
            const bool gpuAvailable=renderer->GetDevice().Supports(RHI::Feature::Compute) &&
                renderer->GetDevice().Supports(RHI::Feature::IndirectDraw);
            hud.Update(window, {
                {Key::Space,paused?"Resume scene":"Pause scene",paused},
                {Key::R,"Restart tour"},
                {Key::Tab,"Free camera",freeCamera,true,Group::Camera},
                {Key::Left,"Tour: back 5 seconds"},{Key::Right,"Tour: forward 5 seconds"},
                {Key::Home,"Tour: first pose"},{Key::End,"Tour: last pose"},
                {Key::N,"Wind animation",windEnabled,!windObjects.empty()},
                {Key::F5,"Freeze and capture A"},{Key::F6,"Capture same-state B",false,comparison.HasBaseline()},
                {Key::F7,"Clear comparison"},{Key::F8,"Change comparison mode"},
                {Key::M,"Console frame statistics",metrics},
                {Key::Escape,"Back to examples"},
#ifdef DY_RENDERDOC_ENABLED
                {Key::F12,"RenderDoc capture"},
#endif
                {Key::Num1,"Lighting",applied.lighting.enabled,true,Group::Render},
                {Key::Num2,"Shadows",applied.lighting.shadows,true,Group::Render},
                {Key::Num3,"HDR rendering",applied.enableHdrRendering,true,Group::Render},
                {Key::Num4,"Frustum culling",applied.enableFrustumCulling,true,Group::Render},
                {Key::Num5,"Instancing",applied.enableInstancing,true,Group::Render},
                {Key::Num6,"Parallel recording",applied.enableParallelRenderGraph,true,Group::Render},
                {Key::U,"GPU culling",applied.enableGpuCulling,gpuAvailable,Group::Render},
                {Key::O,"Hi-Z occlusion",applied.enableOcclusionCulling,gpuAvailable,Group::Render},
                {Key::Num7,"Ambient occlusion",applied.advanced.ambientOcclusion,true,Group::Render},
                {Key::Num8,"Screen-space GI",applied.advanced.diffuseGI,true,Group::Render},
                {Key::Num9,"Reflections",applied.advanced.reflections,true,Group::Render},
                {Key::Num0,"Temporal AA",applied.advanced.temporalAA,true,Group::Render},
                {Key::B,"Bloom",applied.advanced.bloom,true,Group::Render},
                {Key::F,"Fog",applied.advanced.fog.enabled,true,Group::Render},
                {Key::K,"Sharpening",applied.postProcess.sharpening,true,Group::Render},
                {Key::J,"Clustered lighting",applied.lighting.clusteredLighting,true,Group::Render},
                {Key::P,"Soft shadows",applied.lighting.shadowMaxFilterRadius>0,true,Group::Render},
                {Key::F10,"Two-layer transparency",applied.advanced.transparencyLayers==2,true,Group::Render},
                {Key::I,"Spatial probes",!applied.lighting.environment.probes.empty(),bakeSpatialProbes&&!cachedProbes.empty(),Group::Render},
                {Key::T,"Transparency",applied.advanced.transparency,true,Group::Render},
                {Key::V,"Cycle surface view",applied.advanced.surfaceDebugView!=SurfaceDebugView::None,true,Group::Render},
                {Key::G,"Advanced rendering",applied.advanced.enabled,true,Group::Render},
                {Key::F1,"Direct lighting",applied.lighting.direct,true,Group::Render},
                {Key::F2,"Diffuse environment",applied.lighting.environment.diffuse,true,Group::Render},
                {Key::F3,"Specular environment",applied.lighting.environment.specular,true,Group::Render},
                {Key::F4,"Environment background",applied.lighting.environment.background,true,Group::Render},
                {Key::L,"Indexed LOD",applied.enableLod,true,Group::Render},
                {Key::C,"LOD colors",applied.debugLodColor,true,Group::Render},
                {Key::F9,"Wireframe",applied.debugWireframe,renderer->GetDevice().Supports(RHI::Feature::Wireframe),Group::Render},
                {Key::LeftBracket,"LOD: finer",false,true,Group::Render},
                {Key::RightBracket,"LOD: coarser",false,true,Group::Render},
                {Key::Minus,"Exposure down",false,true,Group::Render},
                {Key::Equal,"Exposure up",false,true,Group::Render},
                {Key::W,"Move forward",false,freeCamera,Group::Camera,true},
                {Key::S,"Move back",false,freeCamera,Group::Camera,true},
                {Key::A,"Move left",false,freeCamera,Group::Camera,true},
                {Key::D,"Move right",false,freeCamera,Group::Camera,true},
                {Key::Q,"Move down",false,freeCamera,Group::Camera,true},
                {Key::E,"Move up",false,freeCamera,Group::Camera,true},
                {Key::LeftShift,"Move faster",false,freeCamera,Group::Camera,true}
            });
            if(hud.Pressed(Key::Escape)) {dy_examples::RequestExampleClose(window);break;}
            if(hadProfiler!=hud.IsProfilerVisible() || hadPanel!=hud.IsPanelOpen()) comparison.Cancel("HUD changed");
#ifdef DY_RENDERDOC_ENABLED
            if(hud.Pressed(Key::F12)) (void)dyf::RenderDocCapture::TriggerNextFrame();
#endif
            const auto sceneDrag=hud.SceneDragDelta();
            const bool pointerLook=!hud.CapturesPointer() && input.IsDown(MouseButton::Right);
            const auto previousPose = pose;
            bool historyDiscontinuity = false;
            if(hud.Pressed(Key::F7)) comparison.Clear();
            if(hud.Pressed(Key::F8))
            {
                comparison.Clear();
                comparisonMode = (comparisonMode + 1) % 5;
                std::fprintf(stderr, "Comparison mode: %s\n", comparisonLabels[comparisonMode]);
                if(comparisonMode == 4)
                    std::fputs("Deformation comparison: F5 CPU skinning / F6 GPU compute; an animated actor is required.\n", stderr);
            }
            const bool captureA = hud.Pressed(Key::F5);
            const bool captureB = hud.Pressed(Key::F6);
            if(hud.Pressed(Key::N))
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
            if(hud.Pressed(Key::Space) || hud.Pressed(Key::R) || hud.Pressed(Key::Tab) ||
                pointerLook || sceneDrag.x!=0 || sceneDrag.y!=0 || hud.Down(Key::W) || hud.Down(Key::A) ||
                hud.Down(Key::S) || hud.Down(Key::D) || hud.Down(Key::Q) || hud.Down(Key::E) ||
                hud.Pressed(Key::Left) || hud.Pressed(Key::Right) ||
                hud.Pressed(Key::Home) || hud.Pressed(Key::End))
                comparison.Cancel("camera or tour input");
            if(hud.Pressed(Key::Space))
            {
                paused = !paused;
                std::fprintf(stderr, "Tour %s at %.1f s%s\n", paused ? "paused" : "enabled", tourTime,
                    freeCamera ? " | free camera active; Tab returns to tour" : "");
            }
            const bool restart = hud.Pressed(Key::R);
            if(restart)
            {
                tourTime = 0;
                sceneTime = 0;
                paused = freeCamera = false;
                historyDiscontinuity = true;
                std::fputs("Tour restarted (playing).\n", stderr);
            }
            const bool seek = hud.Pressed(Key::Left) || hud.Pressed(Key::Right) ||
                hud.Pressed(Key::Home) || hud.Pressed(Key::End);
            if(seek)
            {
                if(hud.Pressed(Key::Home)) tourTime = 0;
                else if(hud.Pressed(Key::End)) tourTime = tourSeconds;
                else tourTime = std::clamp(tourTime + (hud.Pressed(Key::Right) ? 5.0 : -5.0), 0.0, tourSeconds);
                sceneTime = tourTime;
                freeCamera = false;
                historyDiscontinuity = true;
                std::fprintf(stderr,"Tour seek: %.1f/%.0f s.\n",tourTime,tourSeconds);
            }
            if(hud.Pressed(Key::Tab))
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
            window.SetCursorMode(freeCamera && pointerLook ?
                Platform::CursorMode::Locked : Platform::CursorMode::Normal);
            if(comparison.IsRunning() || captureA || (captureB && comparison.HasBaseline()))
            {
                // Keep the exact authored/free-camera pose throughout both captures.
                pose = baselinePose;
            }
            else if(freeCamera)
            {
                if(pointerLook || sceneDrag.x!=0 || sceneDrag.y!=0)
                {
                    const auto mouse = pointerLook ? input.GetCursorDelta() : sceneDrag;
                    yaw -= float(mouse.x) * .0025f;
                    pitch = std::clamp(pitch - float(mouse.y) * .0025f, -1.5f, 1.5f);
                }
                const float3 forward{std::cos(pitch) * std::cos(yaw), std::cos(pitch) * std::sin(yaw), std::sin(pitch)};
                const auto right = Math::Normalize(Math::Cross(forward, {0, 0, 1}));
                const auto movement = forward * (float(hud.Down(Key::W)) - float(hud.Down(Key::S))) +
                    right * (float(hud.Down(Key::D)) - float(hud.Down(Key::A))) +
                    float3(0, 0, float(hud.Down(Key::E)) - float(hud.Down(Key::Q)));
                const float speed = hud.Down(Key::LeftShift) || hud.Down(Key::RightShift) ? 32.0f : 8.0f;
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
                modelRenderer->InvalidateHistory();
                renderer->InvalidateHistory();
            }
            if(!camera.LookAt(pose.eye, pose.target, pose.up) ||
                !camera.SetPerspective(float(config.renderWidth ? config.renderWidth : size.width) /
                    (config.renderHeight ? config.renderHeight : size.height), pose.fov * pi / 180, .03f, cameraFarPlane)) return 1;

            config = renderer->GetConfig();
            bool changed = false;
            static constexpr Key settingKeys[] = {
                Key::Num1,Key::Num2,Key::Num3,Key::Num4,Key::Num5,Key::Num6,Key::U,Key::O,
                Key::Num7,Key::Num8,Key::Num9,Key::Num0,Key::B,Key::F,Key::K,Key::J,Key::P,
                Key::F10,Key::I,Key::T,Key::V,Key::G,Key::F1,Key::F2,Key::F3,Key::F4,
                Key::L,Key::C,Key::F9,Key::LeftBracket,Key::RightBracket
            };
            for(const auto key : settingKeys)
            {
                if(!hud.Pressed(key)) continue;
                switch(key)
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
            if(hud.Pressed(Key::Minus) || hud.Pressed(Key::Equal))
            {
                config.exposure = std::clamp(config.exposure * (hud.Pressed(Key::Equal) ? 1.15f : 1 / 1.15f), .001f, 20.0f);
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
            if(hud.Pressed(Key::M))
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
                text << model.path << " preset=" << scenePreset << " cameraPath=" << cameraPath << " " << size.width << 'x' << size.height << " mode=" << comparisonMode
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
                    << " hud=" << hud.IsProfilerVisible() << " panel=" << hud.IsPanelOpen() << " lighting=" << current.lighting.enabled
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
                if(comparisonMode != 4) text << " skinning=" << static_cast<int>(modelRenderer->GetSkinningExecutionMode());
                for(float value : camera.view.m) text << ' ' << value;
                for(float value : camera.projection.m) text << ' ' << value;
                return text.str();
            };
            if(captureA || captureB)
            {
                // Keep profiler drawing out of both benchmark runs. Timing
                // collection remains active; F11 opens the breakdown afterwards.
                config.enableProfilerHud = false;
                hud.SetProfilerVisible(false);
                hud.ClosePanels();
                if(config.debugLodColor || config.debugWireframe)
                    std::fputs("Disable C LOD colors and F9 wireframe before collecting normal-rendering performance.\n", stderr);
                else if(comparisonMode == 0 && (!bistroInterior || !config.lighting.enabled ||
                    !config.lighting.direct || !config.lighting.shadows))
                    std::fputs("Static-shadow comparison needs the interior point lights and enabled shadows. Use F8 for another mode.\n", stderr);
                else if(comparisonMode == 4 && !actor.Loaded())
                    std::fputs("CPU/GPU deformation comparison requires --actor FILE or --actor knight.\n", stderr);
                else
                {
                    const auto previousMode = modelRenderer->GetSkinningExecutionMode();
                    const auto captureMode = captureB ? SkinningExecutionMode::ComputePreSkin : SkinningExecutionMode::Cpu;
                    // Preflight the compute pipeline before beginning capture;
                    // rejection leaves the existing mode and application alive.
                    if(comparisonMode == 4 && !modelRenderer->SetSkinningExecutionMode(captureMode))
                        std::fputs("GPU deformation comparison is unavailable; capture was not started.\n", stderr);
                    else if(comparison.Begin(captureB, conditions(), comparisonLabels[comparisonMode]))
                    {
                        if(comparisonMode == 0) config.lighting.reuseStaticShadows = captureB;
                        if(comparisonMode == 1) config.enableParallelRenderGraph = captureB;
                        if(comparisonMode == 2) config.enableInstancing = captureB;
                        if(comparisonMode == 3) config.enableLod = captureB;
                        if(comparisonMode == 4)
                            std::fprintf(stderr, "Deformation %c: %s at the same frozen scene/actor pose.\n",
                                captureB ? 'B' : 'A', captureB ? "GPU compute" : "CPU skinning");
                        if(!renderer->SetConfig(config)) return 1;
                    }
                    else if(comparisonMode == 4 && !modelRenderer->SetSkinningExecutionMode(previousMode))
                        std::fputs("Could not restore the previous deformation mode after capture rejection.\n", stderr);
                }
            }
            comparison.CheckConditions(conditions());
            const auto previous = renderer->GetLastRenderCompletion();
            Canvas overlay(uint32_t(size.width),uint32_t(size.height));
            comparison.Draw(overlay,uint32_t(size.width),uint32_t(std::max(0.f,float(size.height)-hud.BottomInset())));
            char status[120];
            std::snprintf(status,sizeof(status),"Tour %.0f / %.0f s / %s",tourTime,tourSeconds,
                freeCamera?"Free camera":paused?"Paused":"Playing");
            hud.Draw(overlay,*renderer,emerald?"EMERALD SQUARE":bistro?"BISTRO":"CINEMATIC",status);
            if(actor.Loaded())
            { if(!modelRenderer->Render(scene,camera,overlay)) return 1; }
            else if(!renderer->Render(scene,camera,overlay)) return 1;
            const auto completion = renderer->GetLastRenderCompletion();
            comparison.Update(elapsed, bool(completion && completion != previous),
                renderer->GetProfilerTimingSnapshot(), renderer->GetLastRenderStatistics());
            if(completion && completion != previous && metrics) ++frames;
            if(!ready && completion)
            {
                ready = true;
                releaseCpuImageCache();
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
            if(completion == previous) waitForEvents(.001);
        }
        return 0;
    }
    catch(const std::exception& error)
    {
        std::fprintf(stderr, "Cinematic Showcase: %s\n", error.what());
        return 1;
    }
}
