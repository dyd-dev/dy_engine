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
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

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
                return 0;
            }
            else throw std::runtime_error("Unknown or incomplete option; use --help.");
        }
        if(assets.empty()) throw std::runtime_error("The arena's KayKit asset directory is not configured.");

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
                device.reset(RHI::IDevice::Create(deviceDescription));
                if(!device) throw std::runtime_error("Arena device creation failed.");
                renderer = Renderer::Create(*device, nativeWindow, config);
                if(!renderer || !renderer->AddExtension(std::make_unique<AdvancedRendering>()))
                    throw std::runtime_error("Arena renderer creation failed.");
                modelRenderer = std::make_unique<ModelRenderer>(*renderer);
                rendererWindow = nativeWindow;
                nativeWindowChanged = false;
                clock.Reset();
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
