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

// Shared only by the builtin preset. Standalone/custom providers never gain
// native filtering merely by placing a value in a formerly unused GPU word.
struct EnvironmentFilteringProof
{
    const RenderFrame* frame=nullptr;
    const RHI::IDevice* device=nullptr;
    RenderEnvironmentResources resources={};
    bool nativeFiltering=false;
};

std::unique_ptr<RenderExtension> CreateEnvironmentLightingModule(std::shared_ptr<EnvironmentFilteringProof> proof={});
std::unique_ptr<RenderExtension> CreateClusteredLightingModule();
std::unique_ptr<RenderExtension> CreateSurfaceLightingModule();
std::unique_ptr<RenderExtension> CreateFogModule();
std::unique_ptr<RenderExtension> CreateTransparencyModule(std::shared_ptr<EnvironmentFilteringProof> proof={});
std::unique_ptr<RenderExtension> CreatePostProcessModule();
}
