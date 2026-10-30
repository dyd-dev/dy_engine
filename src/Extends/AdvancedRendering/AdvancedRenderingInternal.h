#pragma once

#include "dyf/RenderExtension.h"
#include <cstdio>

namespace dyf::Detail
{
inline bool AdvancedModuleEnabled(const Renderer& renderer)
{
    const auto& config=renderer.GetConfig();
    return config.advanced.enabled && config.enableHdrRendering;
}

inline bool AdvancedPassFailure(const char* message)
{
    std::fprintf(stderr,"dyf: %s\n",message);
    return false;
}

std::unique_ptr<RenderExtension> CreateEnvironmentLightingModule();
std::unique_ptr<RenderExtension> CreateClusteredLightingModule();
std::unique_ptr<RenderExtension> CreateSurfaceLightingModule();
std::unique_ptr<RenderExtension> CreateFogModule();
std::unique_ptr<RenderExtension> CreateTransparencyModule();
std::unique_ptr<RenderExtension> CreatePostProcessModule();
}
