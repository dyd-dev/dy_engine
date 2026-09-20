#pragma once
#include "dyf/RHI/ResourceHandles.h"
#include <cstdint>
#include <memory>

namespace dyf
{
namespace Platform { class Window; }
namespace RHI { class IDevice; class ICommandList; }

struct GuiStats
{
    uint32_t vertices = 0, indices = 0, drawCalls = 0;
    uint64_t uploadBytes = 0; // Last Record: converted vertex + index bytes, excluding the atlas.
    uint64_t atlasBytes = 0;  // Persistent RGBA8 font texture, uploaded once.
};

// Link dy_imgui (or dyf::ImGui) and include <imgui.h> for the widget API.
// One Gui/context on the window thread. Window and device must outlive Gui.
// Creates its own current ImGui context and chains the Window's GLFW callbacks;
// do not replace those callbacks while Gui is alive. Destruction restores them.
// Add fonts/glyph ranges before the first BeginFrame. That call submits the
// one-time atlas upload. Fonts and atlas are immutable afterwards (legacy mode).
// Docking may be enabled by the application; secondary OS viewports are unsupported.
// Unregistered texture IDs and draw callbacks other than ResetRenderState are rejected.
class Gui
{
public:
    [[nodiscard]] static std::unique_ptr<Gui> Create(Platform::Window&, RHI::IDevice&);
    ~Gui();
    Gui(const Gui&) = delete;
    Gui& operator=(const Gui&) = delete;

    [[nodiscard]] bool BeginFrame(); // Poll window events first; false means no ImGui frame began.
    void EndFrame();                // Calls ImGui::Render(), after the application's widgets.
    // After EndFrame, outside any rendering scope. Loads target, Present -> RT -> Present.
    // The caller owns device BeginFrame/Close/Submit/Present and checks their results.
    [[nodiscard]] bool Record(RHI::ICommandList&, RHI::TextureHandle target);
    // A 2D color texture from this device with ShaderResource usage. Returns 0 on failure.
    // Keep the texture alive until UnregisterTexture, and in ShaderResource when drawing.
    // IDs are opaque, never reused, and passed to ImGui::Image as an ImTextureID.
    // Binding allocation is deferred until Record selects an output pipeline, if necessary.
    // Canvas shaders treat sampled UNORM RGB as linear and convert it for display.
    // For RenderToTexture scene images, use an SRGB target (e.g. B8G8R8A8_UNORM_SRGB).
    // RenderToTexture's UNORM output is already gamma-encoded; do not pass it directly to Image.
    [[nodiscard]] uint64_t RegisterTexture(RHI::TextureHandle);
    void UnregisterTexture(uint64_t); // Unknown IDs (including the font atlas ID 1) are ignored.
    [[nodiscard]] bool WantsMouse() const;
    [[nodiscard]] bool WantsKeyboard() const;
    [[nodiscard]] const GuiStats& GetStats() const;

private:
    explicit Gui(RHI::IDevice&);
    struct Impl;
    std::unique_ptr<Impl> impl;
};
}
