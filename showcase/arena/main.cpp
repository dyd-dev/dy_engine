#include "dyf/Extends/Tracy.h"
#include "dyf/Extends/RenderDoc.h"
#include "dyf/Extends/AdvancedRendering.h"
#include "dyf/Extends/Model/ModelRenderer.h"
#include "dyf/Extends/Model/ModelScene.h"
#include "dyf/Camera.h"
#include "dyf/Platform/Time.h"
#include "dyf/Platform/Window.h"
#include "dyf/Renderer.h"
#include "../PerformanceComparison.h"
#include "../ShowcaseHud.h"
#include "DungeonScene.h"
#include "ArenaActors.h"
#include "ArenaModels.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <iomanip>
#include <locale>
#include <memory>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifndef DY_ARENA_GAME_ASSETS
#define DY_ARENA_GAME_ASSETS ""
#endif

int main(int argc, char** argv)
{
    try
    {
        using namespace dyf;
        using namespace showcase::arena;
        using Platform::Key;
        std::filesystem::path assets = DY_ARENA_GAME_ASSETS;
        std::string fontPath;
        uint32_t actorCount = 72, frameLimit = 0;
        uint32_t benchmarkFrames = 0, benchmarkWarmup = 120;
        double benchmarkStep = 1.0/60.0;
        bool benchmarkRequested = false;
        bool benchmarkSkinCache = true;
        std::string benchmarkOutput;
        bool paused = false, profiler = true, validation = false;
        const auto number = [](std::string_view value) {
            uint32_t result = 0;
            const auto parsed = std::from_chars(value.data(), value.data()+value.size(), result);
            if(parsed.ec != std::errc{} || parsed.ptr != value.data()+value.size())
                throw std::runtime_error("Expected an unsigned integer.");
            return result;
        };
        for(int i=1; i<argc; ++i)
        {
            const std::string_view argument = argv[i];
            if(argument == "--assets" && i+1<argc) assets = argv[++i];
            else if(argument == "--font" && i+1<argc) fontPath = argv[++i];
            else if((argument == "--actors" || argument == "--objects") && i+1<argc)
            {
                actorCount = number(argv[++i]);
                if(actorCount < 12 || actorCount > 512)
                    throw std::runtime_error("--actors requires a count from 12 to 512.");
            }
            else if(argument == "--frames" && i+1<argc) frameLimit = number(argv[++i]);
            else if(argument == "--benchmark-frames" && i+1<argc)
            { benchmarkRequested = true; benchmarkFrames = number(argv[++i]); }
            else if(argument == "--benchmark-warmup" && i+1<argc)
            { benchmarkRequested = true; benchmarkWarmup = number(argv[++i]); }
            else if(argument == "--benchmark-output" && i+1<argc)
            { benchmarkRequested = true; benchmarkOutput = argv[++i]; }
            else if(argument == "--benchmark-skin-cache" && i+1<argc)
            {
                benchmarkRequested = true;
                const std::string_view value=argv[++i];
                if(value!="on" && value!="off")
                    throw std::runtime_error("--benchmark-skin-cache requires on or off.");
                benchmarkSkinCache=value=="on";
            }
            else if(argument == "--benchmark-step" && i+1<argc)
            {
                benchmarkRequested = true;
                const std::string value = argv[++i]; size_t consumed = 0;
                benchmarkStep = std::stod(value, &consumed);
                if(consumed != value.size() || !std::isfinite(benchmarkStep) || benchmarkStep < 0 || benchmarkStep > .1)
                    throw std::runtime_error("--benchmark-step requires seconds from 0 to 0.1.");
            }
            else if(argument == "--paused") paused = true;
            else if(argument == "--profiler") profiler = true;
            else if(argument == "--validation") validation = true;
            else if(argument == "--touch") {} // Kept for existing launch commands; pointer controls are always available.
            else if(argument == "--help")
            {
                std::puts("arena [--assets KayKitFolder] [--actors 72] [--paused] [--font PATH] [--frames N] [--profiler] [--validation] [--touch]\n"
                    "EMBERKEEP / The Sunken Courtyard - an animated 3D presentation scene.\n"
                    "Space pause | R restart | Tab camera tour | O overview | WASD/arrows pan | scroll zoom\n"
                    "F1 controls | H labels | Escape exit | F11 profiler | F12 RenderDoc\n"
                    "4 frustum | 5 instancing | 6 parallel recording | U GPU culling | L LOD | V VSync\n"
                    "F5 freeze and capture A | F6 same-state B | F7 clear\n"
                    "Pointer/touch controls, render settings and GPU pass timings are available on screen.\n"
                    "A/B freezes the scene/camera, disables GPU culling/VSync/profiler, and changes only CPU instancing.");
                std::puts("Benchmark: --benchmark-frames N --benchmark-output NEW.csv [--benchmark-warmup 120] [--benchmark-step 0.016666666666666666]\n"
                    "--benchmark-skin-cache on|off compares shared skin transforms with per-vertex evaluation (default on).\n"
                    "Warm-up freezes at t=0; measurement advances only after successful submissions.\n"
                    "Benchmark disables VSync/HUD, preserves render quality, drains GPU work and refuses existing CSV files.");
                return 0;
            }
            else throw std::runtime_error("Unknown or incomplete option; use --help.");
        }
        if(assets.empty()) throw std::runtime_error("The arena's KayKit asset directory is not configured.");
        if(benchmarkRequested && (!benchmarkFrames || benchmarkOutput.empty() || frameLimit || paused))
            throw std::runtime_error("Benchmark requires positive --benchmark-frames and --benchmark-output; omit --frames/--paused (use --benchmark-step 0 to freeze).");
        if(benchmarkRequested && std::filesystem::exists(benchmarkOutput))
            throw std::runtime_error("Benchmark output already exists: " + benchmarkOutput);

        ModelScene scene;
        auto dungeon = BuildDungeon(scene);
        AddKayKitProps(scene, assets, &dungeon.obstacles);
        ArenaActors actors(scene, dungeon.obstacles, actorCount);
        ArenaModels characters(scene, assets, actorCount);
        characters.Update(actors, 0);

        Platform::Window window(1920, 1080, "EMBERKEEP / The Sunken Courtyard");
#ifdef DY_PROFILER_TRACING_ENABLED
        TracyAdapter tracy;
#endif
        if(!window.GetHandle()) throw std::runtime_error("Window creation failed.");
        RendererConfig config;
        config.clearColor = {.012f, .022f, .035f, 1};
        config.enableHdrRendering = true;
        config.exposure = 1.05f;
        config.lighting.ambientColor = {.48f, .61f, .78f};
        config.lighting.ambientIntensity = .18f;
        config.lighting.shadows = true;
        config.lighting.shadowQuality = ShadowQuality::Medium;
        config.lighting.shadowDistance = 65;
        config.lighting.maxDirectionalShadowLights = 1;
        config.lighting.maxPointShadowLights = 0;
        config.lighting.maxSpotShadowLights = 0;
        config.advanced.enabled = true;
        config.advanced.ambientOcclusion = true;
        config.advanced.aoRadius = .8f;
        config.advanced.aoStrength = .8f;
        config.advanced.diffuseGI = false;
        config.advanced.reflections = false;
        config.advanced.temporalAA = false;
        config.advanced.bloom = true;
        config.advanced.bloomStrength = .09f;
        config.advanced.bloomThreshold = 1.2f;
        config.advanced.fog.enabled = true;
        config.advanced.fog.density = .009f;
        config.advanced.fog.heightFalloff = .45f;
        config.advanced.fog.albedo = {.42f, .59f, .68f};
        config.advanced.fog.maxDistance = 70;
        config.advanced.fog.steps = 24;
        config.postProcess.contrast = 1.06f;
        config.postProcess.saturation = .94f;
        config.enableProfilerHud = false;
        config.enableParallelRenderGraph = true;
        config.recordingThreadCount = 4;
        config.enableInstancing = true;
        config.enableGpuCulling = false;
        if(benchmarkRequested) { config.vsync = false; profiler = false; }
        RHI::DeviceDesc deviceDescription;
        deviceDescription.enableValidation = validation;
        std::unique_ptr<RHI::IDevice> device;
        std::unique_ptr<Renderer> renderer;
        std::unique_ptr<ModelRenderer> modelRenderer;
        const void* rendererWindow = nullptr;
        Camera camera;
        Platform::Time clock;
        showcase::ShowcaseHud hud(fontPath);
        hud.SetProfilerVisible(profiler);
        showcase::PerformanceComparison comparison;
        bool ready = false, overview = false, showHud = true, orbit = true;
        float panX = 0, panY = 0, zoom = 1, cameraAngle = 0;
        double sceneTime = 0;
        uint32_t submitted = 0;
        int renderedWidth = 0, renderedHeight = 0;
        bool nativeWindowChanged = false;
        const auto createRenderer = [&](const void* nativeWindow) {
            // ModelRenderer references Renderer, which in turn references device.
            // Replacements must destroy those owners in the reverse order first.
            if(modelRenderer || renderer || device) throw std::logic_error("Arena GPU owners must be released before recreation.");
            device.reset(RHI::IDevice::Create(deviceDescription));
            if(!device) throw std::runtime_error("Arena device creation failed.");
            renderer = Renderer::Create(*device, nativeWindow, config);
            if(!renderer || !renderer->AddExtension(std::make_unique<AdvancedRendering>()))
                throw std::runtime_error("Arena renderer creation failed.");
            modelRenderer = std::make_unique<ModelRenderer>(*renderer);
            if(benchmarkRequested)
            {
                auto optimization=modelRenderer->GetOptimizationConfig();
                optimization.cacheSkinTransforms=benchmarkSkinCache;
                if(!modelRenderer->SetOptimizationConfig(optimization))
                    throw std::runtime_error("Arena model optimization configuration failed.");
            }
            rendererWindow = nativeWindow;
            nativeWindowChanged = false;
            clock.Reset();
        };
        const auto waitForEvents = [&](double seconds) {
            Platform::Window::WaitEvents(seconds);
            // TERM_WINDOW and INIT_WINDOW may reuse the same native address.
            // Remember a loss observed while waiting before polling again.
            if(renderer && window.GetHandle() != rendererWindow) nativeWindowChanged = true;
        };

        const auto conditions = [&](const RendererConfig& settings, int width, int height) {
            char state[640];
            std::snprintf(state, sizeof(state),
                "|time=%.17g|actors=%u|pan=%.9g,%.9g|zoom=%.9g|angle=%.9g|orbit=%d|overview=%d|size=%dx%d|labels=%d|help=%d|cull=%d|gpu=%d|lod=%d|parallel=%d|workers=%u|vsync=%d|profiler=%d",
                sceneTime, actorCount, panX, panY, zoom, cameraAngle, orbit, overview,
                width, height, showHud, hud.IsPanelOpen(), settings.enableFrustumCulling, settings.enableGpuCulling,
                settings.enableLod, settings.enableParallelRenderGraph, settings.recordingThreadCount,
                settings.vsync, profiler);
            return assets.string()+state;
        };
        std::fprintf(stderr, "EMBERKEEP / The Sunken Courtyard\n"
            "Animated presentation scene. KayKit CC0 assets: %s\n"
            "%u skeleton actors plus one Knight. Space pauses; F1 shows scene controls.\n",
            assets.string().c_str(), actorCount);

        if(benchmarkRequested) createRenderer(window.GetHandle());
        if(benchmarkRequested)
        {
            const auto benchmarkWindow = window.GetHandle();
            struct Frame
            {
                double sceneTime, interval, wall, renderWall;
                RendererStatistics statistics;
            };
            std::vector<Frame> measured;
            measured.reserve(benchmarkFrames);
            std::vector<std::pair<uint32_t, Platform::ProfilerTimingSnapshot>> windows;
            uint64_t lastSequence = renderer->GetProfilerTimingSnapshot().sequence;
            uint64_t successful = 0, animatedIndex = 0;
            bool discardBoundaryWindow = true;
            int measuredWidth = 0, measuredHeight = 0;
            if(!device->WaitIdle()) throw std::runtime_error("Arena benchmark startup GPU drain failed.");
            std::fprintf(stderr, "Arena benchmark: %u frozen warm-up + %u measured submissions at %.17g scene s/submission; VSync/HUD off.\n",
                benchmarkWarmup, benchmarkFrames, benchmarkStep);
            Platform::CpuTimer intervalTimer, measurementTimer;
            while(measured.size() < benchmarkFrames)
            {
                window.PollEvents();
                if(!window.IsRunning() || window.GetInput().WasPressed(Key::Escape))
                    throw std::runtime_error("Arena benchmark interrupted before all submissions completed.");
                const auto size = window.GetFramebufferSize();
                if(window.GetHandle() != benchmarkWindow || window.IsMinimized() || size.width <= 0 || size.height <= 0)
                    throw std::runtime_error("Arena benchmark lost its drawable surface.");
                if(successful >= benchmarkWarmup && measured.empty())
                { measuredWidth = size.width; measuredHeight = size.height; }
                if(!measured.empty() && (size.width != measuredWidth || size.height != measuredHeight))
                    throw std::runtime_error("Arena benchmark output dimensions changed during measurement.");
                Platform::CpuTimer frameTimer;
                const uint64_t poseIndex = successful < benchmarkWarmup ? 0 : measured.size();
                sceneTime = double(poseIndex)*benchmarkStep;
                // Do not advance animation again when WSI skips an acquisition.
                if(poseIndex != animatedIndex)
                {
                    actors.SetTime(sceneTime);
                    characters.Update(actors, float(benchmarkStep));
                    animatedIndex = poseIndex;
                }
                cameraAngle = -.18f*std::sin(float(sceneTime)*.065f);
                if(!camera.LookAt({23*std::sin(cameraAngle), -23*std::cos(cameraAngle), 26}, {0, 0, .6f}) ||
                    !camera.SetOrthographic(27.f*float(size.width)/float(size.height), 27, .1f, 130))
                    throw std::runtime_error("Arena benchmark camera setup failed.");
                const auto previous = renderer->GetLastRenderCompletion();
                Platform::CpuTimer renderTimer;
                if(!modelRenderer->Render(scene, camera)) throw std::runtime_error("Arena benchmark rendering failed.");
                Frame frame{sceneTime, 0, frameTimer.ElapsedMilliseconds(), renderTimer.ElapsedMilliseconds(), {}};
                const auto completion = renderer->GetLastRenderCompletion();
                if(!completion || completion == previous)
                { Platform::Window::WaitEvents(.001); continue; }
                ++successful;
                if(successful <= benchmarkWarmup)
                {
                    if(successful == benchmarkWarmup)
                    {
                        if(!device->WaitIdle()) throw std::runtime_error("Arena benchmark warm-up GPU drain failed.");
                        lastSequence = renderer->GetProfilerTimingSnapshot().sequence;
                        intervalTimer.Reset(); measurementTimer.Reset();
                        std::fputs("Arena benchmark warm-up complete; recording measured submissions.\n", stderr);
                    }
                    continue;
                }
                frame.interval = intervalTimer.ElapsedMilliseconds(); intervalTimer.Reset();
                frame.statistics = renderer->GetLastRenderStatistics();
                measured.push_back(frame);
                const auto& timing = renderer->GetProfilerTimingSnapshot();
                if(timing.sequence != lastSequence)
                {
                    lastSequence = timing.sequence;
                    if(discardBoundaryWindow) discardBoundaryWindow = false;
                    else windows.emplace_back(uint32_t(measured.size()), timing);
                }
            }
            const double submissionWall = measurementTimer.ElapsedMilliseconds();
            Platform::CpuTimer drainTimer;
            if(!device->WaitIdle()) throw std::runtime_error("Arena benchmark final GPU drain failed.");
            const double finalDrain = drainTimer.ElapsedMilliseconds();
            const double completedWall = measurementTimer.ElapsedMilliseconds();
            const auto& applied = renderer->GetAppliedConfig();
            const auto& stats = measured.front().statistics;
            std::ostringstream output; output.imbue(std::locale::classic()); output << std::setprecision(17);
            output << "# arena benchmark v1\n# assets=" << std::quoted(assets.string())
                << " skeleton_actor_capacity=" << actorCount << " knight_actors=1 entities_end=" << scene.GetEntityCount() << '\n'
                << "# warmup_submissions=" << benchmarkWarmup << " warmup_time_seconds=0 measured_submissions=" << measured.size()
                << " benchmark_step_seconds=" << benchmarkStep << " measured_start_time_seconds=0 measured_end_time_seconds=" << sceneTime
                << " vsync=0 hud=0\n# render=" << stats.renderWidth << 'x' << stats.renderHeight
                << " output=" << measuredWidth << 'x' << measuredHeight << '\n'
                << "# measured_submission_wall_ms=" << submissionWall << " final_gpu_drain_ms=" << finalDrain
                << " measured_through_gpu_drain_ms=" << completedWall
                << " gpu_completed_submissions_per_second=" << measured.size()*1000.0/completedWall << '\n'
                << "# quality hdr=" << applied.enableHdrRendering << " scale=" << applied.hdrRenderScale
                << " exposure=" << applied.exposure << " shadows=" << applied.lighting.shadows
                << " shadow_quality=" << int(applied.lighting.shadowQuality) << " shadow_distance=" << applied.lighting.shadowDistance
                << " advanced=" << applied.advanced.enabled << " ao=" << applied.advanced.ambientOcclusion
                << " gi=" << applied.advanced.diffuseGI << " ssr=" << applied.advanced.reflections
                << " taa=" << applied.advanced.temporalAA << " bloom=" << applied.advanced.bloom
                << " transparency=" << applied.advanced.transparency << " auto_exposure=" << applied.advanced.autoExposure
                << " fog=" << applied.advanced.fog.enabled << " fog_steps=" << applied.advanced.fog.steps
                << " fog_density=" << applied.advanced.fog.density << '\n'
                << "# paths cpu_culling=" << applied.enableFrustumCulling << " gpu_culling=" << applied.enableGpuCulling
                << " occlusion=" << applied.enableOcclusionCulling << " instancing=" << applied.enableInstancing
                << " lod=" << applied.enableLod << " lod_error=" << applied.lodPixelError
                << " parallel=" << applied.enableParallelRenderGraph << " workers=" << applied.recordingThreadCount
                << " skin_transform_cache=" << modelRenderer->GetAppliedOptimizationConfig().cacheSkinTransforms << '\n'
                << "# device adapter=" << deviceDescription.adapterIndex << " frames_in_flight=" << deviceDescription.maxFramesInFlight
                << " validation=" << deviceDescription.enableValidation << " timestamp_supported=" << device->Supports(RHI::Feature::TimestampQuery)
                << " timestamp_bits=" << device->GetTimestampValidBits() << " timestamp_period_ns=" << device->GetTimestampPeriodNanoseconds()
                << " gpu_name=see_native_device_startup_log\n# arguments=";
            for(int i=1; i<argc; ++i) output << std::quoted(argv[i]) << ' ';
            output << "\n# submission_interval_ms includes skipped attempts, event polling and host waits; measurement starts after warm-up drain.\n"
                << "# Completed-submission throughput includes final GPU drain and excludes warm-up; it is not display presentation FPS.\n"
                << "# CPU/GPU publication windows are averages, separate from frame rows; the first warm-up boundary window and unpublished trailing samples are omitted.\n"
                << "# Work-only wall times cover only successful attempts and include ModelRenderer preparation; renderer CPU/GPU windows exclude its preceding deformation work.\n";
            enum Column { Kind, Index, SceneTime, Interval, Wall, RenderWall, Cpu, Acquire, Prepare, Record, Submit, Present,
                Sequence, CpuSamples, Gpu, GpuSamples, Draws, ShadowDraws, Instances, Triangles, ShadowTriangles, Culled,
                Batches, GpuVisibility, GpuOcclusion, ShadowReuse, RenderWidth, RenderHeight, OutputWidth, OutputHeight,
                Pass, PassMs, PassSamples, ColumnCount };
            using Row = std::array<std::string, ColumnCount>;
            const auto set = [](Row& row, Column column, const auto& value) {
                std::ostringstream text; text.imbue(std::locale::classic()); text << std::setprecision(17) << value;
                row[column] = text.str();
            };
            const auto write = [&](const Row& row) {
                for(size_t i=0; i<row.size(); ++i)
                {
                    if(i) output << ',';
                    if(row[i].find_first_of(",\"\r\n") == std::string::npos) output << row[i];
                    else { output << '"'; for(char c : row[i]) { if(c=='"') output << '"'; output << c; } output << '"'; }
                }
                output << '\n';
            };
            output << "record_type,index,scene_time_seconds,submission_interval_ms,frame_work_wall_ms,render_work_wall_ms,cpu_render_ms,cpu_acquire_ms,cpu_prepare_ms,cpu_record_ms,cpu_submit_ms,cpu_present_ms,profiler_sequence,cpu_samples,gpu_frame_ms,gpu_samples,main_draws,shadow_draws,main_instances,main_triangles,shadow_triangles,culled_entities,indirect_batches,gpu_visibility,gpu_occlusion,shadow_reused,render_width,render_height,output_width,output_height,gpu_pass_name,gpu_pass_ms,gpu_pass_samples\n";
            std::vector<double> intervals;
            intervals.reserve(measured.size());
            for(size_t i=0; i<measured.size(); ++i)
            {
                const auto& frame = measured[i]; const auto& s = frame.statistics;
                Row row{}; row[Kind] = "frame"; set(row, Index, i+1); set(row, SceneTime, frame.sceneTime);
                set(row, Interval, frame.interval); set(row, Wall, frame.wall); set(row, RenderWall, frame.renderWall);
                set(row, Draws, s.mainDrawCount); set(row, ShadowDraws, s.shadowDrawCount); set(row, ShadowTriangles, s.shadowTriangleCount);
                if(!s.gpuVisibility)
                { set(row, Instances, s.mainInstanceCount); set(row, Triangles, s.mainTriangleCount); set(row, Culled, s.culledEntityCount); }
                set(row, Batches, s.indirectBatchCount); set(row, GpuVisibility, s.gpuVisibility); set(row, GpuOcclusion, s.gpuOcclusion);
                set(row, ShadowReuse, s.reusedShadowAtlas); set(row, RenderWidth, s.renderWidth); set(row, RenderHeight, s.renderHeight);
                set(row, OutputWidth, s.outputWidth); set(row, OutputHeight, s.outputHeight); write(row);
                intervals.push_back(frame.interval);
            }
            double cpuTotal = 0, gpuTotal = 0; uint64_t cpuCount = 0, gpuCount = 0;
            for(const auto& sample : windows)
            {
                const auto& s = sample.second;
                Row row{}; row[Kind] = "profiler_window"; set(row, Index, sample.first); set(row, Sequence, s.sequence);
                set(row, Cpu, s.cpuRenderAverageMilliseconds); set(row, CpuSamples, s.frameCount);
                set(row, Acquire, s.cpuAcquireAverageMilliseconds); set(row, Prepare, s.cpuPrepareAverageMilliseconds);
                set(row, Record, s.cpuRecordAverageMilliseconds); set(row, Submit, s.cpuSubmitAverageMilliseconds);
                set(row, Present, s.cpuPresentAverageMilliseconds);
                cpuTotal += s.cpuRenderAverageMilliseconds*s.frameCount; cpuCount += s.frameCount;
                if(s.hasGpuFrame && s.gpuFrameSampleCount)
                {
                    set(row, Gpu, s.gpuFrameAverageMilliseconds); set(row, GpuSamples, s.gpuFrameSampleCount);
                    gpuTotal += s.gpuFrameAverageMilliseconds*s.gpuFrameSampleCount; gpuCount += s.gpuFrameSampleCount;
                }
                write(row);
                for(const auto& pass : s.gpuPasses)
                {
                    Row passRow{}; passRow[Kind] = "gpu_pass"; set(passRow, Index, sample.first); set(passRow, Sequence, s.sequence);
                    passRow[Pass] = pass.name; set(passRow, PassMs, pass.averageMilliseconds); set(passRow, PassSamples, pass.sampleCount); write(passRow);
                }
            }
            std::sort(intervals.begin(), intervals.end());
            const auto percentile = [&](double fraction) { return intervals[size_t(std::ceil(fraction*intervals.size()))-1]; };
            output << "# summary submission_interval samples=" << intervals.size()
                << " mean_ms=" << std::accumulate(intervals.begin(), intervals.end(), 0.0)/intervals.size()
                << " p50_ms=" << percentile(.5) << " p95_ms=" << percentile(.95) << " p99_ms=" << percentile(.99) << '\n'
                << "# summary profiler_cpu samples=" << cpuCount;
            if(cpuCount) output << " mean_ms=" << cpuTotal/cpuCount;
            else output << " unavailable";
            output << "\n# summary profiler_gpu samples=" << gpuCount;
            if(gpuCount) output << " mean_ms=" << gpuTotal/gpuCount;
            else output << " unavailable";
            output << '\n';
            // C11 exclusive creation also protects against another run creating
            // the requested path after the initial argument validation.
            const auto closeFile = [](std::FILE* value) { (void)std::fclose(value); };
            std::unique_ptr<std::FILE, decltype(closeFile)> file(std::fopen(benchmarkOutput.c_str(), "wx"), closeFile);
            if(!file) throw std::runtime_error("Cannot create new arena benchmark output: " + benchmarkOutput);
            const auto csv = output.str();
            if(std::fwrite(csv.data(), 1, csv.size(), file.get()) != csv.size())
                throw std::runtime_error("Arena benchmark output write failed: " + benchmarkOutput);
            if(std::fclose(file.release()) != 0) throw std::runtime_error("Arena benchmark output close failed: " + benchmarkOutput);
            std::fprintf(stderr, "Arena benchmark: %.8f GPU-completed submissions/s, mean interval %.6f ms, p95 %.6f ms, p99 %.6f ms; CSV %s.\n",
                measured.size()*1000.0/completedWall, std::accumulate(intervals.begin(), intervals.end(), 0.0)/intervals.size(),
                percentile(.95), percentile(.99), benchmarkOutput.c_str());
            return 0;
        }

        while(window.IsRunning() && (!frameLimit || submitted < frameLimit))
        {
            window.PollEvents();
            const auto& input = window.GetInput();
            if(!window.IsRunning()) break;
            const auto nativeWindow = window.GetHandle();
            if(renderer && (nativeWindowChanged || nativeWindow != rendererWindow))
            {
                // Android can replace ANativeWindow while keeping this scene alive.
                // Recreate the GPU owners together; keep the CPU scene and staging.
                config = renderer->GetConfig();
                modelRenderer.reset();
                renderer.reset();
                device.reset();
                rendererWindow = nullptr;
                nativeWindowChanged = false;
                comparison.Cancel("native window changed");
                ready = false;
                renderedWidth = renderedHeight = 0;
                clock.Reset();
            }
            const auto size = window.GetFramebufferSize();
#if defined(DY_ARENA_MOBILE) && DY_ARENA_MOBILE
            const bool inactive = !window.HasFocus();
#else
            const bool inactive = false;
#endif
            const bool suspended = !nativeWindow || inactive || window.IsMinimized() || size.width <= 0 || size.height <= 0;
            const double elapsed = clock.Tick(suspended);
            if(suspended)
            {
                comparison.Cancel("window inactive, minimized or without a drawable surface");
                waitForEvents(.1);
                continue;
            }
            if(!renderer)
            {
                createRenderer(nativeWindow);
            }
            const auto& applied = renderer->GetAppliedConfig();
            using Group=showcase::ControlGroup;
            const bool gpuAvailable=device->Supports(RHI::Feature::Compute) && device->Supports(RHI::Feature::IndirectDraw);
            hud.Update(window, {
                {Key::Space,paused?"Resume scene":"Pause scene",paused},
                {Key::R,"Restart scene"},
                {Key::Tab,"Camera tour",orbit,true,Group::Camera},
                {Key::O,"Wide overview",overview,true,Group::Camera},
                {Key::H,"Scene labels",showHud},
                {Key::F5,"Capture A: instancing off"},
                {Key::F6,"Capture B: instancing on",false,comparison.HasBaseline()},
                {Key::F7,"Clear comparison"},
                {Key::Escape,"Back to examples"},
#ifdef DY_RENDERDOC_ENABLED
                {Key::F12,"RenderDoc capture"},
#endif
                {Key::Num4,"Frustum culling",applied.enableFrustumCulling,true,Group::Render},
                {Key::Num5,"Instancing",applied.enableInstancing,true,Group::Render},
                {Key::Num6,"Parallel recording",applied.enableParallelRenderGraph,true,Group::Render},
                {Key::U,"GPU culling",applied.enableGpuCulling,gpuAvailable,Group::Render},
                {Key::L,"Indexed LOD",applied.enableLod,true,Group::Render},
                {Key::V,"VSync",applied.vsync,true,Group::Render},
                {Key::W,"Pan forward",false,true,Group::Camera,true},
                {Key::S,"Pan back",false,true,Group::Camera,true},
                {Key::A,"Pan left",false,true,Group::Camera,true},
                {Key::D,"Pan right",false,true,Group::Camera,true},
                {Key::Equal,"Zoom in",false,true,Group::Camera,true},
                {Key::Minus,"Zoom out",false,true,Group::Camera,true}
            });
            if(hud.Pressed(Key::Escape)) {window.RequestClose();break;}
            if(hud.Pressed(Key::F1)) hud.ToggleControls();
            if(profiler != hud.IsProfilerVisible()) comparison.Cancel("profiler changed");
            profiler=hud.IsProfilerVisible();
            if(hud.Pressed(Key::F5) || hud.Pressed(Key::F6)) hud.ClosePanels();
#ifdef DY_RENDERDOC_ENABLED
            if(hud.Pressed(Key::F12)) (void)RenderDocCapture::TriggerNextFrame();
#endif
            const auto fired = [&](Key key) {return hud.Pressed(key);};
            bool restarted = false;
            if(fired(Key::R))
            {
                comparison.Clear();
                sceneTime = 0;
                characters.Reset();
                modelRenderer->InvalidateHistory();
                cameraAngle = panX = panY = 0;
                zoom = 1;
                restarted = true;
            }
            if(fired(Key::Space)) { comparison.Cancel("pause changed"); paused = !paused; }
            if(fired(Key::Tab)) { comparison.Cancel("camera mode changed"); orbit = !orbit; }
            if(fired(Key::O)) { comparison.Cancel("camera changed"); overview = !overview; }
            if(hud.Pressed(Key::H)) { comparison.Cancel("labels changed"); showHud = !showHud; }

            const float moveX = float(hud.Down(Key::D) || hud.Down(Key::Right)) - float(hud.Down(Key::A) || hud.Down(Key::Left));
            const float moveY = float(hud.Down(Key::W) || hud.Down(Key::Up)) - float(hud.Down(Key::S) || hud.Down(Key::Down));
            const auto drag=hud.SceneDragDelta();
            const bool dragging=drag.x!=0 || drag.y!=0;
            const float scroll = hud.CapturesPointer() || dragging ? 0 : float(input.GetScrollDelta().y);
            const float zoomAxis=float(hud.Down(Key::Equal))-float(hud.Down(Key::Minus));
            if(moveX != 0 || moveY != 0 || scroll != 0 || zoomAxis != 0 || dragging)
            {
                comparison.Cancel("camera moved");
                orbit = false;
                const float dt = float(std::clamp(elapsed, 0.0, .1));
                panX = std::clamp(panX + moveX*dt*8 - float(drag.x)*.025f, -12.f, 12.f);
                panY = std::clamp(panY + moveY*dt*8 + float(drag.y)*.025f, -12.f, 12.f);
                zoom = std::clamp(zoom*std::exp(-(scroll+zoomAxis*dt*10)*.08f), .55f, 1.65f);
            }

            config = renderer->GetConfig();
            bool changed = false;
            const auto toggle = [&](Key action, bool& value) {
                if(fired(action)) { value = !value; changed = true; }
            };
            toggle(Key::Num4, config.enableFrustumCulling);
            toggle(Key::Num5, config.enableInstancing);
            toggle(Key::Num6, config.enableParallelRenderGraph);
            toggle(Key::U, config.enableGpuCulling);
            toggle(Key::L, config.enableLod);
            toggle(Key::V, config.vsync);
            if(changed) comparison.Cancel("renderer setting changed");
            if(fired(Key::F7)) comparison.Clear();
            if(fired(Key::F5))
            {
                paused = true;
                hud.ClosePanels();
                config.enableInstancing = false;
                config.enableGpuCulling = false;
                config.vsync = false;
                config.enableProfilerHud = false;
                profiler = false;
                hud.SetProfilerVisible(false);
                changed = true;
                (void)comparison.Begin(false, conditions(config, size.width, size.height), "EMBERKEEP / CPU INSTANCING");
            }
            else if(fired(Key::F6) && comparison.Begin(true,
                conditions(config, size.width, size.height), "EMBERKEEP / CPU INSTANCING"))
            {
                paused = true;
                config.enableInstancing = true;
                changed = true;
            }
            if(changed && !renderer->SetConfig(config)) throw std::runtime_error("Renderer setting rejected.");

            // ModelRenderer uploads/deforms before the renderer acquires its frame.
            // During resize, let the old work drain and the swapchain recover
            // before adding another model preparation submission to the queue.
            if(!changed && (size.width != renderedWidth || size.height != renderedHeight) &&
                !device->BeginFrame())
            {
                if(device->IsLost()) throw std::runtime_error("Arena device was lost while resizing.");
                waitForEvents(.001);
                continue;
            }

            const float delta = ready && !paused && !restarted ? float(std::clamp(elapsed, 0.0, .1)) : 0;
            sceneTime += delta;
            actors.SetTime(sceneTime);
            characters.Update(actors, delta);
            if(orbit) cameraAngle = -.18f*std::sin(float(sceneTime)*.065f);
            const float viewHeight = (overview ? 43.f : 27.f)*zoom;
            if(!camera.LookAt({panX+23*std::sin(cameraAngle), panY-23*std::cos(cameraAngle), 26},
                    {panX, panY, .6f}) ||
                !camera.SetOrthographic(viewHeight*float(size.width)/float(size.height), viewHeight, .1f, 130))
                throw std::runtime_error("Camera setup failed.");
            comparison.CheckConditions(conditions(config, size.width, size.height));

            Canvas overlay(uint32_t(size.width), uint32_t(size.height));
            const float bottomInset=hud.BottomInset();
            char status[120];
            std::snprintf(status,sizeof(status),"%u actors / %02u:%02u / %s",actorCount+1,
                uint32_t(sceneTime)/60,uint32_t(sceneTime)%60,paused?"Paused":orbit?"Camera tour":"Manual camera");
            comparison.Draw(overlay,uint32_t(size.width),uint32_t(std::max(0.f,float(size.height)-bottomInset)));
            hud.Draw(overlay,*renderer,showHud?"EMBERKEEP":"",status);
            const auto previous = renderer->GetLastRenderCompletion();
            if(!modelRenderer->Render(scene, camera, overlay)) throw std::runtime_error("Arena rendering failed.");
            const auto completion = renderer->GetLastRenderCompletion();
            const bool didSubmit = completion && completion != previous;
            if(didSubmit)
            {
                ++submitted;
                renderedWidth = size.width;
                renderedHeight = size.height;
            }
            comparison.Update(elapsed, didSubmit, renderer->GetProfilerTimingSnapshot(), renderer->GetLastRenderStatistics());
            if(!ready && completion) { ready = true; clock.Reset(); }
            if(!didSubmit) waitForEvents(.001);
        }
        return 0;
    }
    catch(const std::exception& error)
    {
        std::fprintf(stderr, "EMBERKEEP: %s\n", error.what());
        return 1;
    }
}
