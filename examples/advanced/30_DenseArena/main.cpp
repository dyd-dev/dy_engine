#include "dyf/Camera.h"
#include "dyf/Platform/Time.h"
#include "dyf/Platform/Window.h"
#include "dyf/Renderer.h"
#include "dyf/Scene.h"
#include "../PerformanceComparison.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

#ifndef DY_DENSE_ARENA_ASSETS
#define DY_DENSE_ARENA_ASSETS ""
#endif

namespace
{
    constexpr float pi = 3.14159265358979323846f;
    using dyf::Math::float3;
    struct Placement { float3 position; float size = 1, angle = 0; };
    struct MovingSprite { dyf::EntityHandle entity; uint32_t index, kind; float size; };

    dyf::Math::float4x4 SpriteTransform(float3 position, float size = 1, float angle = 0, float facing = 1)
    {
        auto result = dyf::Math::float4x4::Identity();
        const float c = std::cos(angle) * size, s = std::sin(angle) * size;
        result.m[0] = c * facing; result.m[1] = s * facing;
        result.m[4] = -s; result.m[5] = c;
        result.m[12] = position.x; result.m[13] = position.y; result.m[14] = position.z;
        return result;
    }

    uint32_t Hash(uint32_t value)
    {
        value ^= value >> 16; value *= 0x7feb352du;
        value ^= value >> 15; value *= 0x846ca68bu;
        return value ^ (value >> 16);
    }
}

int main(int argc, char** argv)
{
    try
    {
        using namespace dyf;
        using Platform::Key;
        std::filesystem::path assets = DY_DENSE_ARENA_ASSETS;
        uint32_t objectCount = 6000;
        bool paused = false;
        for(int i = 1; i < argc; ++i)
        {
            const std::string argument = argv[i];
            if(argument == "--assets" && i + 1 < argc) assets = argv[++i];
            else if(argument == "--objects" && i + 1 < argc)
            {
                const std::string number = argv[++i];
                const auto parsed = std::from_chars(number.data(), number.data() + number.size(), objectCount);
                if(parsed.ec != std::errc{} || parsed.ptr != number.data() + number.size() ||
                    objectCount < 100 || objectCount > 100000)
                    throw std::runtime_error("--objects requires an integer from 100 to 100000.");
            }
            else if(argument == "--paused") paused = true;
            else
            {
                std::fputs("AdvancedDenseArena [--assets KenneyTinyDungeonFolder] [--objects 6000] [--paused]\n"
                    "Space pause/play | R restart | O overview | 4 CPU culling | 5 instancing\n"
                    "F5 baseline | F6 instanced comparison | F7 clear\n"
                    "6 parallel recording | V VSync | M metrics | F11 profiler | Escape close\n", stderr);
                return argument == "--help" ? 0 : 1;
            }
        }
        if(assets.empty() || !std::filesystem::is_directory(assets / "Tiles"))
            throw std::runtime_error("Use --assets with the extracted Kenney Tiny Dungeon folder (containing Tiles/).");

        Scene scene;
        MeshData quad;
        quad.vertices = {
            {{-.5f, -.5f, 0}, {0, 0, 1}, {0, 1}},
            {{ .5f, -.5f, 0}, {0, 0, 1}, {1, 1}},
            {{ .5f,  .5f, 0}, {0, 0, 1}, {1, 0}},
            {{-.5f,  .5f, 0}, {0, 0, 1}, {0, 0}}
        };
        quad.indices = {0, 1, 2, 0, 2, 3};
        std::map<uint32_t, Image> images;
        auto tile = [&](uint32_t index) -> const Image& {
            auto found = images.find(index);
            if(found != images.end()) return found->second;
            char filename[32];
            std::snprintf(filename, sizeof(filename), "tile_%04u.png", index);
            Image image;
            if(!LoadImage((assets / "Tiles" / filename).string(), image))
                throw std::runtime_error(std::string("Cannot load Tiny Dungeon tile: ") + filename);
            return images.emplace(index, std::move(image)).first->second;
        };
        auto material = [](const Image& image) {
            MaterialDesc result;
            result.baseColorTexture = image;
            result.alphaMode = MaterialAlphaMode::Mask;
            result.alphaCutoff = .5f;
            result.doubleSided = true;
            return result;
        };

        // Register each sprite type consecutively: stock instancing can combine
        // equal materials and the shared mesh while keeping independent transforms.
        std::map<uint32_t, std::vector<Placement>> scenery;
        auto place = [&](uint32_t image, float x, float y, float z = 0, float size = 1) {
            scenery[image].push_back({{x, y, z}, size});
        };
        for(int room = 0; room < 9; ++room)
        {
            const int cx = (room % 3 - 1) * 32, cy = (room / 3 - 1) * 26;
            for(int y = -10; y <= 9; ++y)
                for(int x = -13; x <= 13; ++x)
                {
                    const uint32_t seed = Hash(uint32_t((x + 13) + (y + 10) * 27 + room * 547));
                    place(seed % 37 == 0 ? 42 : seed % 9 == 0 ? 49 : 48, float(cx + x), float(cy + y));
                }
            for(int x = -14; x <= 14; ++x)
            {
                if(!(room / 3 < 2 && std::abs(x) <= 2))
                {
                    place(x == -14 ? 1 : x == 14 ? 3 : 2, float(cx + x), float(cy + 11), .05f);
                    place(40, float(cx + x), float(cy + 10), .05f);
                }
                if(!(room / 3 > 0 && std::abs(x) <= 2))
                    place(x == -14 ? 25 : x == 14 ? 27 : 26, float(cx + x), float(cy - 11), .06f);
            }
            for(int y = -10; y <= 10; ++y)
            {
                if(!(room % 3 > 0 && std::abs(y) <= 2)) place(13, float(cx - 14), float(cy + y), .06f);
                if(!(room % 3 < 2 && std::abs(y) <= 2)) place(15, float(cx + 14), float(cy + y), .06f);
            }
            // Furniture islands leave the center free for moving enemies and bullets.
            for(int side : {-1, 1})
                for(int x : {-10, -6, 6, 10})
                {
                    const float y = float(cy + side * 7);
                    place((Hash(uint32_t(room * 97 + x + 32 + side)) & 1) ? 60 : 63,
                        float(cx + x), y, 2 - y * .01f);
                    place(61, float(cx + x + 1), y - 1, 2 - (y - 1) * .01f, .8f);
                }
            for(int side : {-1, 1})
            {
                place(18, float(cx + side * 9), float(cy + 9), 2.1f - (cy + 9) * .01f);
                place(21, float(cx + side * 5), float(cy + 9), 2.1f - (cy + 9) * .01f);
                place(65, float(cx + side * 12), float(cy - 8), 2 - (cy - 8) * .01f);
                place(66, float(cx + side * 11), float(cy - 6), 2 - (cy - 6) * .01f);
            }
            if(room % 3 < 2)
                for(int x = 14; x <= 18; ++x)
                    for(int y = -3; y <= 3; ++y)
                        place(y == -3 ? 26 : y == 3 ? 2 : 48, float(cx + x), float(cy + y), std::abs(y) == 3 ? .05f : 0);
            if(room / 3 < 2)
                for(int y = 10; y <= 15; ++y)
                    for(int x = -3; x <= 3; ++x)
                        place(x == -3 ? 13 : x == 3 ? 15 : 48, float(cx + x), float(cy + y), std::abs(x) == 3 ? .05f : 0);
        }
        for(const auto& [image, positions] : scenery)
        {
            EntityHandle source;
            const auto spriteMaterial = material(tile(image));
            for(const auto& placement : positions)
            {
                const auto transform = SpriteTransform(placement.position, placement.size, placement.angle);
                auto entity = source ? scene.AddInstance(source, transform) : scene.Add(quad, spriteMaterial, transform);
                if(!entity) throw std::runtime_error("Cannot create dungeon scenery.");
                if(!source) source = entity;
            }
        }

        // Pixel projectiles and fragments are immutable CPU images, created once.
        // Their pools recycle positions/lifetimes without adding GPU resources.
        auto particleImage = [](bool fragment, uint32_t color) {
            std::vector<uint8_t> pixels(8 * 8 * 4, 0);
            for(int y = 0; y < 8; ++y)
                for(int x = 0; x < 8; ++x)
                {
                    const int dx = x - 3, dy = y - 3;
                    if(fragment ? x < 2 || x > 5 || y < 2 || y > 4 : dx * dx + dy * dy > 10) continue;
                    const bool center = dx * dx + dy * dy < 4;
                    const size_t p = size_t(y * 8 + x) * 4;
                    pixels[p] = center ? 255 : color == 0 ? 246 : 76;
                    pixels[p + 1] = center ? 245 : color == 0 ? 112 : 201;
                    pixels[p + 2] = center ? 189 : color == 0 ? 78 : 231;
                    pixels[p + 3] = 255;
                }
            return Image(8, 8, std::move(pixels));
        };
        const uint32_t enemyCount = objectCount / 10, bulletCount = objectCount * 3 / 4;
        const uint32_t debrisCount = objectCount - enemyCount - bulletCount;
        std::vector<MovingSprite> moving;
        moving.reserve(objectCount);
        auto pool = [&](const Image& image, uint32_t count, uint32_t firstIndex, uint32_t kind, float size) {
            EntityHandle source;
            const auto spriteMaterial = material(image);
            for(uint32_t room = 0; room < 9; ++room)
                for(uint32_t i = 0; i < count; ++i)
                {
                    if(((firstIndex + i) / 12) % 9 != room) continue;
                    auto entity = source ? scene.AddInstance(source) : scene.Add(quad, spriteMaterial);
                    if(!entity) throw std::runtime_error("Cannot create the moving sprite pool.");
                    if(!source) source = entity;
                    moving.push_back({entity, firstIndex + i, kind, size});
                }
        };
        constexpr uint32_t enemyTiles[] = {108, 110, 120, 121, 122, 123};
        uint32_t first = 0;
        for(uint32_t type = 0; type < 6; ++type)
        {
            const uint32_t count = enemyCount / 6 + uint32_t(type < enemyCount % 6);
            pool(tile(enemyTiles[type]), count, first, 0, 1.0f);
            first += count;
        }
        pool(particleImage(false, 0), bulletCount / 2, 0, 1, .38f);
        pool(particleImage(false, 1), bulletCount - bulletCount / 2, bulletCount / 2, 1, .38f);
        pool(particleImage(true, 0), debrisCount, 0, 2, .42f);
        auto hero = scene.Add(quad, material(tile(98)));
        if(!hero) return 1;

        Platform::Window window(1280, 720, "dy_engine / Dense Arena - Space pause | 4 culling | 5 instancing | F11 profiler");
        if(!window.GetHandle()) return 1;
        RendererConfig config;
        config.clearColor = {.12f, .065f, .09f, 1};
        config.lighting.enabled = false;
        config.enableHdrRendering = false;
        config.enableGpuCulling = true;
        config.enableProfilerHud = true;
        config.profilerStartsExpanded = true;
        config.enableParallelRenderGraph = true;
        config.recordingThreadCount = 4;
        auto renderer = Renderer::Create(window.GetHandle(), config);
        if(!renderer) return 1;
        Camera camera;
        Platform::Time clock;
        double gameTime = 0, metricsTime = 0;
        double referenceTime = 0;
        uint32_t submitted = 0;
        bool ready = false, overview = false, metrics = false, referenceOverview = false;
        showcase::PerformanceComparison comparison;
        auto conditions = [&](double time, bool wholeMap, const RendererConfig& settings, int width, int height) {
            char state[512];
            std::snprintf(state, sizeof(state), "|objects=%u|seed=0|time=%.17g|overview=%d|size=%dx%d|cull=%d|parallel=%d|workers=%u|vsync=%d|hud=%d|metrics=%d|lit=%d|hdr=%d|advanced=%d|gpu=%d",
                objectCount, time, wholeMap, width, height, settings.enableFrustumCulling,
                settings.enableParallelRenderGraph, settings.recordingThreadCount, settings.vsync,
                settings.enableProfilerHud, metrics, settings.lighting.enabled, settings.enableHdrRendering, settings.advanced.enabled,settings.enableGpuCulling);
            return assets.string() + state;
        };
        std::fprintf(stderr, "Dense Arena: %u enemies, %u bullets, %u fragments; %u scene entities.\n"
            "Kenney Tiny Dungeon (CC0): %s\n"
            "Space pause/play | R deterministic restart | O overview\n"
            "4 frustum culling | 5 instancing | 6 parallel recording | U GPU culling | V VSync\n"
            "F5 freeze/baseline (instancing OFF) | F6 same-state candidate (ON) | F7 clear\n"
            "M submitted frames/s and draw counts | F11 profiler | Escape close\n", enemyCount, bulletCount,
            debrisCount, scene.GetEntityCount(), assets.string().c_str());

        while(window.IsRunning())
        {
            window.PollEvents();
            const auto& input = window.GetInput();
            if(input.WasPressed(Key::Escape)) window.RequestClose();
            if(!window.IsRunning()) break;
            const auto size = window.GetFramebufferSize();
            const bool focused = window.HasFocus();
            const bool suspended = window.IsMinimized() || size.width <= 0 || size.height <= 0 || (ready && !focused);
            const double elapsed = clock.Tick(suspended || !focused);
            if(suspended)
            {
                comparison.Cancel("window inactive or minimized");
                Platform::Window::WaitEvents();
                continue;
            }
            if(input.WasPressed(Key::Space) || input.WasPressed(Key::R) || input.WasPressed(Key::O))
                comparison.Cancel("game or camera controls changed");
            if(input.WasPressed(Key::Space)) paused = !paused;
            if(input.WasPressed(Key::R)) { gameTime = 0; paused = false; }
            if(input.WasPressed(Key::O)) overview = !overview;
            if(input.WasPressed(Key::M))
            {
                comparison.Cancel("console metrics changed");
                metrics = !metrics; metricsTime = 0; submitted = 0;
            }

            config = renderer->GetConfig();
            bool changed = false;
            for(const auto& event : input.GetEvents())
            {
                if(event.type != Platform::InputEventType::Key || event.action != Platform::InputAction::Press) continue;
                switch(static_cast<Key>(event.code))
                {
                case Key::Num4: config.enableFrustumCulling = !config.enableFrustumCulling; break;
                case Key::Num5: config.enableInstancing = !config.enableInstancing; break;
                case Key::Num6: config.enableParallelRenderGraph = !config.enableParallelRenderGraph; break;
                case Key::U: config.enableGpuCulling = !config.enableGpuCulling; break;
                case Key::V: config.vsync = !config.vsync; break;
                case Key::F11: config.enableProfilerHud = !config.enableProfilerHud; break;
                default: continue;
                }
                changed = true;
            }
            if(changed) comparison.Cancel("renderer settings changed");
            if(input.WasPressed(Key::F7)) comparison.Clear();
            // Both captures measure the scene with the same small progress
            // overlay; the expanded profiler is opened separately with F11.
            if(input.WasPressed(Key::F5) || input.WasPressed(Key::F6)) config.enableProfilerHud = false;
            if(input.WasPressed(Key::F5))
            {
                referenceTime = gameTime;
                referenceOverview = overview;
                paused = true;
                config.enableInstancing = false;
                // 이 비교에서는 GPU 컬링을 고정하여 인스턴싱만 비교한다.
                config.enableGpuCulling = false;
                changed = true;
                comparison.Begin(false, conditions(gameTime, overview, config, size.width, size.height), "DENSE ARENA - INSTANCING");
            }
            else if(input.WasPressed(Key::F6) && comparison.Begin(true,
                conditions(referenceTime, referenceOverview, config, size.width, size.height), "DENSE ARENA - INSTANCING"))
            {
                gameTime = referenceTime;
                overview = referenceOverview;
                paused = true;
                config.enableInstancing = true;
                changed = true;
            }
            if(changed)
            {
                if(!renderer->SetConfig(config)) return 1;
                std::fprintf(stderr, "CPU culling %d | instancing %d | parallel %d | VSync %d\n",
                    config.enableFrustumCulling, config.enableInstancing, config.enableParallelRenderGraph, config.vsync);
            }
            if(ready && !paused && !input.WasPressed(Key::R)) gameTime += elapsed;
            comparison.CheckConditions(conditions(gameTime, overview, config, size.width, size.height));
            const float t = float(std::fmod(gameTime, 3600.0));

            for(auto& sprite : moving)
            {
                const uint32_t i = sprite.index, seed = Hash(i + sprite.kind * 100003u);
                const int room = int((i / 12) % 9);
                const float cx = float((room % 3 - 1) * 32), cy = float((room / 3 - 1) * 26);
                const float phase = float(seed & 65535u) * (2 * pi / 65536);
                float x, y, angle = 0, scale = sprite.size, facing = 1;
                if(sprite.kind == 0)
                {
                    const float speed = .17f + float((seed >> 16) & 15u) * .009f;
                    x = cx + std::sin(t * speed + phase) * (6 + float(seed % 5));
                    y = cy + std::sin(t * speed * 1.7f + phase * 2) * 5.5f;
                    y += std::abs(std::sin(t * 8 + phase)) * .10f;
                    facing = std::cos(t * speed + phase) >= 0 ? 1 : -1;
                }
                else if(sprite.kind == 1)
                {
                    const float lifetime = 3.5f + float(seed % 17) * .08f;
                    const float age = std::fmod(t + float(i / 24) * .071f, lifetime);
                    const float cycle = std::floor((t + float(i / 24) * .071f) / lifetime);
                    const float direction = float(i % 24) * (2 * pi / 24) + cycle * .31f + phase * .12f;
                    x = std::cos(direction) * age * 3 + std::cos(phase) * 2;
                    y = std::sin(direction) * age * 2 + std::sin(phase) * 1.5f;
                    // Analytic wall bounces keep the fixed pools inside each room.
                    x = cx + 11 - std::abs(std::fmod(x + 55, 44.0f) - 22);
                    y = cy + 7 - std::abs(std::fmod(y + 35, 28.0f) - 14);
                    scale *= .8f + .2f * std::sin(age * 7);
                }
                else
                {
                    const float lifetime = 1.3f + float(seed % 19) * .04f;
                    const float age = std::fmod(t + phase, lifetime);
                    const float cycle = std::floor((t + phase) / lifetime);
                    const float burst = float(Hash(uint32_t(cycle) + i / 9) & 65535u) * (2 * pi / 65536);
                    x = cx + std::cos(burst) * 6 + std::cos(phase) * age * 2;
                    y = cy + std::sin(burst) * 4 + std::sin(phase) * age * 2 - age * age;
                    angle = phase + age * (3 + float(seed % 4));
                    scale *= std::max(.08f, 1 - age / lifetime);
                }
                // Snap motion to the source pixel grid; camera remains orthographic.
                x = std::round(x * 16) / 16; y = std::round(y * 16) / 16;
                const float z = (sprite.kind == 1 ? 3 : sprite.kind == 2 ? 1 : 2) - y * .01f + float(i % 97) * .00001f;
                if(!sprite.entity.SetTransform(SpriteTransform({x, y, z}, scale, angle, facing))) return 1;
            }
            const float heroX = std::round(std::cos(t * .32f) * 3 * 16) / 16;
            const float heroY = std::round(std::sin(t * .41f) * 2 * 16) / 16;
            if(!hero.SetTransform(SpriteTransform({heroX, heroY, 2.01f - heroY * .01f}, 1.15f))) return 1;
            const float cameraX = overview ? 0 : std::round(std::sin(t * .08f) * 5 * 16) / 16;
            const float cameraY = overview ? 0 : std::round(std::sin(t * .065f) * 3 * 16) / 16;
            const float viewHeight = overview ? std::max(82.0f, 98.0f * size.height / size.width) : 32;
            if(!camera.LookAt({cameraX, cameraY, 100}, {cameraX, cameraY, 0}, {0, 1, 0}) ||
                !camera.SetOrthographic(viewHeight * size.width / size.height, viewHeight, .1f, 110)) return 1;

            Canvas overlay(uint32_t(size.width), uint32_t(size.height));
            comparison.Draw(overlay, uint32_t(size.width), uint32_t(size.height));
            const auto previous = renderer->GetLastRenderCompletion();
            if(!renderer->Render(scene, camera, overlay)) return 1;
            const auto completion = renderer->GetLastRenderCompletion();
            comparison.Update(elapsed, completion && completion != previous,
                renderer->GetProfilerTimingSnapshot(), renderer->GetLastRenderStatistics());
            if(metrics && completion && completion != previous) ++submitted;
            if(!ready && completion) { ready = true; clock.Reset(); }
            if(metrics) metricsTime += elapsed;
            if(metrics && metricsTime >= 1)
            {
                const auto& stats = renderer->GetLastRenderStatistics();
                if(stats.gpuVisibility)
                    std::fprintf(stderr,"Arena %.2f s | %s | submitted %.1f/s | indirect batches %u | visible instances/triangles N/A\n",
                        gameTime,paused ? "paused" : "playing",submitted/metricsTime,stats.indirectBatchCount);
                else
                    std::fprintf(stderr, "Arena %.2f s | %s | submitted %.1f/s | draws %u | instances %u | CPU culled %u\n",
                        gameTime, paused ? "paused" : "playing", submitted / metricsTime, stats.mainDrawCount,
                        stats.mainInstanceCount, stats.culledEntityCount);
                metricsTime = 0; submitted = 0;
            }
            if(completion == previous) Platform::Window::WaitEvents(.001);
        }
        return 0;
    }
    catch(const std::exception& error)
    {
        std::fprintf(stderr, "Dense Arena: %s\n", error.what());
        return 1;
    }
}
