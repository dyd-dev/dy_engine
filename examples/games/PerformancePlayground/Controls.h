#pragma once
#include <dyf/Renderer.h>
#include <string_view>

namespace playground {
inline bool RequestToggle(dyf::Renderer& renderer, std::string_view key) {
    // GetConfig includes pending requests, so simultaneous keys do not overwrite each other.
    auto config = renderer.GetConfig();
    if(key == "instancing") config.enableInstancing = !config.enableInstancing;
    else if(key == "culling") config.enableFrustumCulling = !config.enableFrustumCulling;
    else if(key == "lighting") config.lighting.enabled = !config.lighting.enabled;
    else if(key == "shadows") config.lighting.shadows = !config.lighting.shadows;
    else if(key == "parallelRecording") config.enableParallelRenderGraph = !config.enableParallelRenderGraph;
    else if(key == "vsync") config.vsync = !config.vsync;
    else return false;
    return renderer.SetConfig(config);
}
}
