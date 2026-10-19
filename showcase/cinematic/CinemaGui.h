#pragma once

#include "../../examples/HostedExample.h"
#include "dyf/Extends/ImGui/ImGui.h"
#include "dyf/Extends/Model/ModelRenderer.h"
#include "dyf/Extends/Model/ModelScene.h"
#include "dyf/Platform/Window.h"
#include "dyf/RHI/IDevice.h"
#include "dyf/RHI/ICommandList.h"
#include "dyf/RHI/Texture.h"
#include "dyf/Renderer.h"
#include "../CameraPath.h"
#include "../PerformanceComparison.h"
#include <imgui.h>
#include <algorithm>
#include <bitset>
#include <cstdio>
#include <stdexcept>

namespace showcase
{
// Widgets emit the same actions as the cinematic keyboard controls. The scene
// keeps its own renderer/configuration and is composed behind movable panels.
class CinemaGui
{
public:
    CinemaGui(dyf::Platform::Window& window, dyf::Renderer& renderer)
        : m_window(window), m_renderer(renderer), m_gui(dyf::Gui::Create(window, renderer.GetDevice()))
    {
        if(!m_gui) throw std::runtime_error("Cannot initialize cinematic GUI.");
        ImGui::GetIO().IniFilename = nullptr;
        ImGui::StyleColorsDark();
        const auto kind = window.GetNativeHandle().kind;
        m_touch = kind == dyf::Platform::NativeWindowKind::Android || kind == dyf::Platform::NativeWindowKind::UIKit;
        if(m_touch) ImGui::GetStyle().ScaleAllSizes(1.35f);
    }
    ~CinemaGui()
    {
        if(m_textureId) m_gui->UnregisterTexture(m_textureId);
        m_renderer.GetDevice().DestroyTexture(m_texture);
    }

    bool Pressed(dyf::Platform::Key key) const { return m_actions.test(static_cast<unsigned>(key)); }
    bool Held(dyf::Platform::Key key) const { return m_held.test(static_cast<unsigned>(key)); }
    bool WantsKeyboard() const { return m_gui->WantsKeyboard(); }
    bool WantsMouse() const { return m_gui->WantsMouse(); }
    bool TouchCamera() const { return m_touch; }
    bool Presented() const { return m_presented; }
    double seekSeconds = -1;
    bool cameraEdited = false, resolutionChanged = false;
    dyf::Math::float3 eye;
    float yaw = 0, pitch = 0;
    uint32_t renderWidth = 0, renderHeight = 0;
    int skinning = -1;

    bool Begin(const dyf::RendererConfig& config, bool paused, bool freeCamera, bool wind, bool hasWind,
        double tourTime, double tourSeconds, const CameraKeyframe& pose, float cameraYaw, float cameraPitch,
        bool metrics, unsigned comparisonMode, const char* const* comparisonLabels,
        const PerformanceComparison& comparison, const dyf::ModelRenderer& model, bool hasActor)
    {
        using dyf::Platform::Key;
        m_actions.reset(); m_held.reset();
        seekSeconds = -1; cameraEdited = resolutionChanged = false;
        skinning = -1;
        eye = pose.eye; yaw = cameraYaw; pitch = cameraPitch;
        renderWidth = config.renderWidth; renderHeight = config.renderHeight;
        if(!m_gui->BeginFrame()) return false;
        dy_examples::DrawExampleControls();
        ImGui::SetNextWindowPos({16, 16}, ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize({350, 650}, ImGuiCond_FirstUseEver);
        if(ImGui::Begin("Cinematic controls"))
        {
            ImGui::Text("Tour %.1f / %.1f s", tourTime, tourSeconds);
            ImGui::TextUnformatted("Drag the panel title to move it.");
            const bool capturing = comparison.IsRunning();
            ImGui::BeginDisabled(capturing);
            if(ImGui::CollapsingHeader("Tour and camera", ImGuiTreeNodeFlags_DefaultOpen))
            {
                Toggle("Paused [Space]", paused, Key::Space);
                ImGui::SameLine(); Action("Restart [R]", Key::R);
                Toggle("Free camera [Tab]", freeCamera, Key::Tab);
                ImGui::BeginDisabled(!hasWind); Toggle("Wind [N]", wind, Key::N); ImGui::EndDisabled();
                double value = tourTime, minimum = 0;
                if(ImGui::SliderScalar("Tour time", ImGuiDataType_Double, &value, &minimum, &tourSeconds, "%.1f s"))
                    seekSeconds = value;
                Action("Start [Home]", Key::Home); ImGui::SameLine(); Action("End [End]", Key::End);
                Action("-5 s [Left]", Key::Left); ImGui::SameLine(); Action("+5 s [Right]", Key::Right);
                if(freeCamera)
                {
                    float position[] = {eye.x, eye.y, eye.z};
                    if(ImGui::DragFloat3("Position", position, .1f))
                    { eye = {position[0], position[1], position[2]}; cameraEdited = true; }
                    float angles[] = {yaw * 180 / Pi, pitch * 180 / Pi};
                    if(ImGui::DragFloat2("Yaw / pitch", angles, .25f))
                    { yaw = angles[0] * Pi / 180; pitch = std::clamp(angles[1] * Pi / 180, -1.5f, 1.5f); cameraEdited = true; }
                    Hold("Forward [W]", Key::W); ImGui::SameLine(); Hold("Back [S]", Key::S);
                    Hold("Left [A]", Key::A); ImGui::SameLine(); Hold("Right [D]", Key::D);
                    Hold("Up [E]", Key::E); ImGui::SameLine(); Hold("Down [Q]", Key::Q);
                    ImGui::Checkbox("Fast movement [Shift]", &m_fast);
                    if(m_fast) m_held.set(static_cast<unsigned>(Key::LeftShift));
                    ImGui::TextWrapped(m_touch ? "Drag the scene to look around." : "Hold RMB over the scene to look around.");
                }
            }
            if(ImGui::CollapsingHeader("Rendering", ImGuiTreeNodeFlags_DefaultOpen))
            {
                int resolution = config.renderWidth == 1920 && config.renderHeight == 1080 ? 0 :
                    config.renderWidth == 1280 && config.renderHeight == 720 ? 1 :
                    config.renderWidth == 960 && config.renderHeight == 540 ? 2 : 3;
                const char* choices[] = {"1920 x 1080", "1280 x 720", "960 x 540", "Window framebuffer"};
                if(ImGui::Combo("Scene resolution", &resolution, choices, 4))
                {
                    constexpr uint32_t widths[] = {1920, 1280, 960, 0}, heights[] = {1080, 720, 540, 0};
                    renderWidth = widths[resolution]; renderHeight = heights[resolution]; resolutionChanged = true;
                }
                ImGui::BeginDisabled(!hasActor);
                int mode = static_cast<int>(model.GetSkinningExecutionMode());
                const char* modes[] = {"Vertex shader preference", "GPU compute pre-skin", "CPU skinning"};
                if(ImGui::Combo("Actor deformation", &mode, modes, 3)) skinning = mode;
                ImGui::EndDisabled();
                ImGui::TextWrapped("Advanced surface passes use compute for vertex preference when supported, otherwise CPU fallback. Explicit CPU stays on the CPU; morph evaluation is unchanged.");
                Toggle("Lighting [1]", config.lighting.enabled, Key::Num1);
                Toggle("Shadows [2]", config.lighting.shadows, Key::Num2);
                Toggle("HDR [3]", config.enableHdrRendering, Key::Num3);
                Toggle("CPU frustum culling [4]", config.enableFrustumCulling, Key::Num4);
                Toggle("Instancing [5]", config.enableInstancing, Key::Num5);
                Toggle("Parallel recording [6]", config.enableParallelRenderGraph, Key::Num6);
                Toggle("GPU frustum / LOD [U]", config.enableGpuCulling, Key::U);
                Toggle("Hi-Z occlusion [O]", config.enableOcclusionCulling, Key::O);
                Toggle("Indexed LOD [L]", config.enableLod, Key::L);
                ImGui::Text("LOD pixel error %.3f", config.lodPixelError);
                Action("Halve [", Key::LeftBracket); ImGui::SameLine(); Action("Double ]", Key::RightBracket);
                Toggle("LOD colors [C]", config.debugLodColor, Key::C);
                Toggle("Wireframe [F9]", config.debugWireframe, Key::F9);
                ImGui::Text("Exposure %.3f", config.exposure);
                Action("Exposure -", Key::Minus); ImGui::SameLine(); Action("Exposure +", Key::Equal);
            }
            if(ImGui::CollapsingHeader("Advanced raster and lighting"))
            {
                Toggle("Advanced raster [G]", config.advanced.enabled, Key::G);
                Toggle("Ambient occlusion [7]", config.advanced.ambientOcclusion, Key::Num7);
                Toggle("Screen-space GI [8]", config.advanced.diffuseGI, Key::Num8);
                Toggle("Reflections [9]", config.advanced.reflections, Key::Num9);
                Toggle("Temporal AA [0]", config.advanced.temporalAA, Key::Num0);
                Toggle("Bloom [B]", config.advanced.bloom, Key::B);
                Toggle("Transparency [T]", config.advanced.transparency, Key::T);
                Toggle("Volumetric fog [F]", config.advanced.fog.enabled, Key::F);
                Toggle("Sharpening [K]", config.postProcess.sharpening, Key::K);
                Toggle("Clustered lights [J]", config.lighting.clusteredLighting, Key::J);
                Toggle("Soft shadows [P]", config.lighting.shadowMaxFilterRadius > 0, Key::P);
                ImGui::Text("Glass layers: %u", config.advanced.transparencyLayers);
                Action("Toggle glass layers [F10]", Key::F10);
                Toggle("Direct light [F1]", config.lighting.direct, Key::F1);
                Toggle("Diffuse IBL [F2]", config.lighting.environment.diffuse, Key::F2);
                Toggle("Specular IBL [F3]", config.lighting.environment.specular, Key::F3);
                Toggle("Sky background [F4]", config.lighting.environment.background, Key::F4);
                Toggle("Spatial probes [I]", !config.lighting.environment.probes.empty(), Key::I);
                const char* views[] = {"Off", "Base color", "Normal", "Roughness", "Metallic", "Depth", "Motion"};
                ImGui::Text("Surface: %s", views[static_cast<unsigned>(config.advanced.surfaceDebugView)]);
                Action("Next surface view [V]", Key::V);
            }
            Toggle("Statistics [F11]", config.enableProfilerHud, Key::F11);
            Toggle("Console submitted frames/s [M]", metrics, Key::M);
#ifdef DY_RENDERDOC_ENABLED
            Action("RenderDoc capture [F12]", Key::F12);
#endif
            ImGui::Separator();
            ImGui::TextWrapped("A/B: %s", comparisonLabels[comparisonMode]);
            Action("Next comparison [F8]", Key::F8);
            if(comparisonMode == 4)
                ImGui::TextWrapped(hasActor ? "A uses CPU skinning; B uses GPU compute on the same frozen scene and actor." :
                    "Load an animated actor with --actor to compare CPU and GPU deformation.");
            ImGui::BeginDisabled(comparisonMode == 4 && !hasActor);
            Action(comparisonMode == 4 ? "Capture A: CPU [F5]" : "Capture A: OFF [F5]", Key::F5);
            ImGui::BeginDisabled(!comparison.HasBaseline());
            Action(comparisonMode == 4 ? "Capture B: GPU compute [F6]" : "Capture B: ON [F6]", Key::F6);
            ImGui::EndDisabled();
            ImGui::EndDisabled();
            ImGui::EndDisabled();
            if(capturing)
                ImGui::Text("%c %s: %.1f s / %u frames", comparison.IsCandidateCapture() ? 'B' : 'A',
                    comparison.GetCapturePhase() == PerformanceComparison::CapturePhase::Warmup ? "warmup" : "capture",
                    comparison.GetPhaseElapsedSeconds(), comparison.GetPhaseFrameCount());
            Action("Cancel / clear A/B [F7]", Key::F7);
            Result("A", comparison.GetBaselineTiming(), comparison.HasBaseline());
            Result("B", comparison.GetCandidateTiming(), comparison.HasCandidate());
            Action("Close showcase [Esc]", Key::Escape);
        }
        ImGui::End();
        if(config.enableProfilerHud)
        {
            ImGui::SetNextWindowPos({385, 16}, ImGuiCond_FirstUseEver);
            ImGui::SetNextWindowSize({410, 360}, ImGuiCond_FirstUseEver);
            if(ImGui::Begin("Cinematic statistics"))
            {
                const auto& timing = m_renderer.GetProfilerTimingSnapshot();
                ImGui::Text("Window cadence %.1f FPS", ImGui::GetIO().Framerate);
                ImGui::TextWrapped("Frame interval includes host work between scene calls. CPU scene stages and the GPU scene span exclude GUI composition, acquire and present. Use --no-gui for the original window/HUD path.");
                ImGui::Text("CPU scene %.3f ms; prepare %.3f; record %.3f; submit %.3f",
                    timing.cpuRenderAverageMilliseconds, timing.cpuPrepareAverageMilliseconds,
                    timing.cpuRecordAverageMilliseconds, timing.cpuSubmitAverageMilliseconds);
                if(timing.hasGpuFrame) ImGui::Text("GPU scene %.3f ms", timing.gpuFrameAverageMilliseconds);
                else ImGui::TextUnformatted("GPU scene: unavailable");
                if(hasActor) ImGui::Text("Model preparation %.3f ms", model.GetCpuPrepareMilliseconds());
                if(hasActor && model.GetGpuMilliseconds() >= 0)
                    ImGui::Text("GPU skinning, last completed: %.3f ms", model.GetGpuMilliseconds());
                const auto& stats = m_renderer.GetLastRenderStatistics();
                ImGui::Text("Main draws %u; shadow draws %u", stats.mainDrawCount, stats.shadowDrawCount);
                if(stats.gpuVisibility) ImGui::TextUnformatted("GPU-selected instances/triangles: no readback");
                else ImGui::Text("Main instances %u; triangles %llu", stats.mainInstanceCount,
                    static_cast<unsigned long long>(stats.mainTriangleCount));
                for(const auto& pass : timing.gpuPasses)
                    ImGui::Text("%s: %.3f ms (%u)", pass.name.c_str(), pass.averageMilliseconds, pass.sampleCount);
            }
            ImGui::End();
        }
        const auto framebuffer = m_window.GetFramebufferSize();
        const uint32_t width = renderWidth ? renderWidth : static_cast<uint32_t>(framebuffer.width);
        const uint32_t height = renderHeight ? renderHeight : static_cast<uint32_t>(framebuffer.height);
        if(!PrepareTarget(width, height)) return false;
        auto* viewport = ImGui::GetMainViewport();
        const float scale = std::min(viewport->Size.x / width, viewport->Size.y / height);
        const ImVec2 extent(width * scale, height * scale);
        const ImVec2 origin(viewport->Pos.x + (viewport->Size.x - extent.x) * .5f,
            viewport->Pos.y + (viewport->Size.y - extent.y) * .5f);
        ImGui::GetBackgroundDrawList()->AddRectFilled(viewport->Pos,
            {viewport->Pos.x + viewport->Size.x, viewport->Pos.y + viewport->Size.y}, IM_COL32(0, 0, 0, 255));
        ImGui::GetBackgroundDrawList()->AddImage(static_cast<ImTextureID>(m_textureId), origin,
            {origin.x + extent.x, origin.y + extent.y});
        return true;
    }

    bool Render(const dyf::ModelScene& scene, const dyf::Camera& camera, dyf::ModelRenderer& model, bool hasActor)
    {
        using namespace dyf::RHI;
        m_presented = false;
        m_gui->EndFrame();
        if(hasActor ? !model.RenderToTexture(scene, camera, m_texture, m_state) :
            !m_renderer.RenderToTexture(scene, camera, m_texture, m_state)) return false;
        m_state = ResourceState::ShaderResource;
        auto& device = m_renderer.GetDevice();
        // Scene config changes can recreate the swapchain. Acquire only after
        // that offscreen submission, then compose once and present once.
        if(!device.BeginFrame()) return !device.IsLost();
        ResourceScope resources(device);
        auto* commands = resources.Keep(device.AcquireCommandList());
        auto* output = device.GetBackBuffer();
        if(!commands || !output) return false;
        ResourceBarrierDesc barrier{nullptr, output, ResourceState::Present, ResourceState::RenderTarget, {}};
        commands->ResourceBarrier(&barrier, 1);
        ColorAttachment color; color.texture = output; color.loadOp = LoadOp::Clear; color.storeOp = StoreOp::Store;
        color.clearColor[3] = 1;
        commands->BeginRendering({&color, 1, nullptr}); commands->EndRendering();
        barrier.before = ResourceState::RenderTarget; barrier.after = ResourceState::Present;
        commands->ResourceBarrier(&barrier, 1);
        m_presented = m_gui->Record(*commands, output) && commands->Close() && device.Submit(&commands, 1) && device.Present();
        return m_presented;
    }

private:
    using Key = dyf::Platform::Key;
    static constexpr float Pi = 3.14159265358979323846f;
    void Action(const char* label, Key key) { if(ImGui::Button(label)) m_actions.set(static_cast<unsigned>(key)); }
    void Toggle(const char* label, bool value, Key key) { if(ImGui::Checkbox(label, &value)) m_actions.set(static_cast<unsigned>(key)); }
    void Hold(const char* label, Key key)
    { ImGui::Button(label); if(ImGui::IsItemActive()) m_held.set(static_cast<unsigned>(key)); }
    static void Result(const char* label, const dyf::Platform::ProfilerTimingSnapshot& timing, bool available)
    {
        if(!available) return;
        ImGui::Text("%s: frame %.3f ms, CPU scene %.3f ms", label,
            timing.frameAverageMilliseconds, timing.cpuRenderAverageMilliseconds);
        if(timing.hasGpuFrame) ImGui::Text("GPU scene %.3f ms (%u samples)", timing.gpuFrameAverageMilliseconds, timing.gpuFrameSampleCount);
    }
    bool PrepareTarget(uint32_t width, uint32_t height)
    {
        using namespace dyf::RHI;
        if(!width || !height) return false;
        if(m_texture && m_texture->GetDesc().width == width && m_texture->GetDesc().height == height) return true;
        auto& device = m_renderer.GetDevice();
        TextureDesc desc; desc.width = width; desc.height = height; desc.format = Format::B8G8R8A8_UNORM_SRGB;
        desc.usage = TextureUsage::RenderTarget | TextureUsage::ShaderResource;
        auto* replacement = device.CreateTexture(desc);
        if(!replacement) return false;
        const auto id = m_gui->RegisterTexture(replacement);
        if(!id) { device.DestroyTexture(replacement); return false; }
        if(m_textureId) m_gui->UnregisterTexture(m_textureId);
        device.DestroyTexture(m_texture);
        m_texture = replacement; m_textureId = id; m_state = ResourceState::Undefined;
        return true;
    }
    dyf::Platform::Window& m_window;
    dyf::Renderer& m_renderer;
    std::unique_ptr<dyf::Gui> m_gui;
    dyf::RHI::TextureHandle m_texture = nullptr;
    dyf::RHI::ResourceState m_state = dyf::RHI::ResourceState::Undefined;
    uint64_t m_textureId = 0;
    std::bitset<349> m_actions, m_held;
    bool m_touch = false, m_fast = false, m_presented = false;
};
}
