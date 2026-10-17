#pragma once
namespace dyf { class Renderer; }
namespace dyf::Platform { class Input; }
namespace dyf::Diagnostics
{
// Call once after PollEvents and before rendering. F11 changes this renderer's
// expanded HUD; F12 requests capture through the optional RenderDoc adapter.
// Returns false if a requested renderer settings change was rejected.
[[nodiscard]] bool UpdateProfilerControls(const Platform::Input&,Renderer* renderer=nullptr);
}
