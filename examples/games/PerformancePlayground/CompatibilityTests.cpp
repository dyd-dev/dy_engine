#include "Controls.h"
#include "Evidence.h"
#include <cstdio>
#include <sstream>
#include <stdexcept>

namespace {
void Check(bool condition, const char* message) {
    if(!condition) throw std::runtime_error(message);
}
std::string Timing(const dyf::Platform::ProfilerTimingSnapshot& snapshot, bool shadows = true) {
    std::ostringstream out;
    playground::WriteTimingEvidence(out, snapshot, shadows);
    return out.str();
}
void CheckTiming() {
    dyf::Platform::ProfilerTimingSnapshot snapshot;
    auto text = Timing(snapshot);
    Check(text.find("\"gpuMain\":{\"state\":\"Pending\",\"milliseconds\":null}") != std::string::npos,
        "unpublished GPU measurements must be pending, not zero");
    snapshot.sequence = 1;
    text = Timing(snapshot);
    Check(text.find("\"gpuFrame\":{\"state\":\"Unsupported\",\"milliseconds\":null}") != std::string::npos,
        "unsupported GPU measurements must remain null");
    snapshot.gpuTimingSupported = true;
    snapshot.gpuMainAverageMilliseconds = 99;
    text = Timing(snapshot, false);
    Check(text.find("\"gpuMain\":{\"state\":\"Pending\",\"milliseconds\":null}") != std::string::npos,
        "missing completed samples must not expose a stale numeric value");
    Check(text.find("\"gpuShadow\":{\"state\":\"Unavailable\",\"milliseconds\":null}") != std::string::npos,
        "disabled shadows must not claim a measurement");
    snapshot.hasGpuMain = true;
    snapshot.gpuMainAverageMilliseconds = 0;
    snapshot.hasGpuFrame = true;
    snapshot.gpuFrameAverageMilliseconds = 1.25;
    text = Timing(snapshot);
    Check(text.find("\"gpuMain\":{\"state\":\"Ready\",\"milliseconds\":0}") != std::string::npos,
        "a measured zero is valid and differs from a missing sample");
    Check(text.find("\"gpuFrame\":{\"state\":\"Ready\",\"milliseconds\":1.25}") != std::string::npos,
        "ready GPU duration must retain the public snapshot value");
}
void CheckControls() {
    using namespace dyf;
    Platform::Window window(320, 240, "Playground API verification");
    Check(window.GetHandle() != nullptr, "native test window");
    RendererConfig config;
    config.vsync = false;
    config.enableProfilerHud = false;
    config.allowReadback = true;
    config.recordingThreadCount = 2;
    auto renderer = Renderer::Create(window.GetHandle(), config);
    Check(bool(renderer), "native renderer");
    Scene scene;
    Check(bool(scene.Add(CreateCubeMesh())), "test geometry");
    Check(bool(scene.Add(DirectionalLight{})), "test shadow caster light");
    Camera camera;
    Check(camera.LookAt({3, -5, 3}, {0, 0, 0}) && camera.SetPerspective(4.f / 3, 1.05f, .06f, 100), "test camera");
    Check(playground::RequestToggle(*renderer, "instancing"), "request instancing off");
    Check(playground::RequestToggle(*renderer, "culling"), "request culling off");
    Check(!renderer->GetConfig().enableInstancing && !renderer->GetConfig().enableFrustumCulling,
        "same-frame requests must preserve both pending changes");
    Check(renderer->GetAppliedConfig().enableInstancing && renderer->GetAppliedConfig().enableFrustumCulling,
        "pending settings must not be reported as already applied");
    Check(!playground::RequestToggle(*renderer, "unknown"), "unknown controls must be rejected");
    Image image;
    Platform::Window::PollEvents();
    Check(renderer->Render(scene, camera, &image) && image.IsValid(), "first native render/readback");
    Check(!renderer->GetAppliedConfig().enableInstancing && !renderer->GetAppliedConfig().enableFrustumCulling,
        "both settings must apply on the next render");
    for(const auto* key : {"instancing", "culling", "lighting", "shadows", "parallelRecording", "vsync"})
        Check(playground::RequestToggle(*renderer, key), "request supported control");
    Check(renderer->Render(scene, camera, &image) && image.IsValid(), "parallel render/readback after toggles");
    const auto& applied = renderer->GetAppliedConfig();
    Check(applied.enableInstancing && applied.enableFrustumCulling && !applied.lighting.enabled &&
        applied.lighting.shadows && applied.enableParallelRenderGraph && applied.vsync,
        "all six controls must reach their intended public settings");
    Check(applied.allowReadback && applied.recordingThreadCount == 2, "unrelated renderer settings must survive toggles");
    Check(playground::RequestToggle(*renderer, "lighting"), "restore lighting while shadows are enabled");
    Check(renderer->Render(scene, camera, &image) && image.IsValid(), "lit shadow pipeline must render after settings apply");
}
}

int main() try {
    CheckTiming();
    CheckControls();
    std::puts("PASS GPU availability, pending/applied controls, native readback and parallel recording");
    return 0;
} catch(const std::exception& error) {
    std::fprintf(stderr, "FAIL: %s\n", error.what());
    return 1;
}
