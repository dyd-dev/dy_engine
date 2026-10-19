#pragma once

#include <array>
#include <string_view>
#include "dyf/RHI/Feature.h"

namespace example_catalog
{
enum class Status { Implemented, Limited, Unimplemented };

struct Entry
{
    const char* target;
    const char* title;
    const char* category;
    const char* summary;
    const char* requirements;
    Status status;
    bool presentation;
    std::array<dyf::RHI::Feature, 4> requiredFeatures;
    unsigned requiredFeatureCount;
};

// Features describe the default invocation. Format, pipeline and optional mode
// requirements still need the checks performed by the example itself.
inline constexpr std::array<Entry, 23> AdvancedExamples = {{
    {"AdvancedProfiling", "01 / Frame profiling", "Measurement",
     "CPU stage distributions and optional GPU timestamp measurements.",
     "Rasterization; GPU timing additionally needs timestamp queries.", Status::Implemented, true,
     {dyf::RHI::Feature::Rasterization}, 1},
    {"AdvancedDepthReverseZ", "02 / Reverse Z", "Depth and synchronization",
     "Compare ordinary and reversed depth tests with adjustable near/far planes.",
     "Rasterization and a supported depth attachment.", Status::Implemented, false,
     {dyf::RHI::Feature::Rasterization}, 1},
    {"AdvancedDepthSampling", "03 / Depth sampling", "Depth and synchronization",
     "Render depth, then inspect raw and linearized depth in a second pass.",
     "Rasterization and a sampleable depth attachment.", Status::Implemented, false,
     {dyf::RHI::Feature::Rasterization}, 1},
    {"AdvancedStencilMask", "04 / Stencil mask", "Depth and synchronization",
     "Use stencil writes and tests to form a mask and outline.",
     "Rasterization and the example's depth/stencil format.", Status::Implemented, false,
     {dyf::RHI::Feature::Rasterization}, 1},
    {"AdvancedResourceBarriers", "05 / Resource barriers", "Depth and synchronization",
     "Transition an offscreen producer into a sampled consumer; compare explicit waits.",
     "Rasterization and a texture supporting rendering and sampling.", Status::Implemented, false,
     {dyf::RHI::Feature::Rasterization}, 1},
    {"AdvancedFramesAndPresent", "06 / Frames and present", "Measurement",
     "Compare frames in flight and requested present modes using CPU stage timings.",
     "Rasterization and the requested swapchain present mode.", Status::Implemented, false,
     {dyf::RHI::Feature::Rasterization}, 1},
    {"AdvancedPBRLighting", "07 / Analytic PBR lighting", "Lighting",
     "PBR and Lambert lighting on an analytic sphere/plane scene.",
     "Rasterization; isolated shader scene, without imported mesh rendering.", Status::Limited, false,
     {dyf::RHI::Feature::Rasterization}, 1},
    {"AdvancedShadowMapping", "08 / Analytic shadow mapping", "Lighting",
     "Depth atlas and PCF shadows for analytic geometry; directional, cascaded and point modes.",
     "Rasterization and a sampleable depth attachment; isolated analytic scene.", Status::Limited, false,
     {dyf::RHI::Feature::Rasterization}, 1},
    {"AdvancedClusteredLighting", "09 / Clustered lighting", "Lighting",
     "Compute builds fixed world-space light lists for an analytic lighting pass.",
     "Rasterization and compute; fixed 16 x 16 x 8 grid and up to 64 lights.", Status::Limited, false,
     {dyf::RHI::Feature::Rasterization, dyf::RHI::Feature::Compute}, 2},
    {"AdvancedDeferredLighting", "10 / Deferred lighting", "Lighting",
     "Three G-buffer attachments and deferred lighting for an analytic scene.",
     "Rasterization and supported MRT/attachment formats; isolated analytic scene.", Status::Limited, false,
     {dyf::RHI::Feature::Rasterization}, 1},
    {"AdvancedHDRPostProcess", "11 / HDR and bloom", "Post process",
     "HDR rendering, bright extraction, separable blur and tone mapping.",
     "Rasterization and supported HDR render/sample formats; isolated shader scene.", Status::Limited, false,
     {dyf::RHI::Feature::Rasterization}, 1},
    {"AdvancedTemporalAA", "12 / Temporal AA pattern", "Post process",
     "Jitter, motion, history reprojection and neighborhood clamping on a moving 2D pattern.",
     "Rasterization and history attachments; not a mesh-animation velocity demonstration.", Status::Limited, false,
     {dyf::RHI::Feature::Rasterization}, 1},
    {"AdvancedVolumetricFog", "13 / Volumetric fog", "Post process",
     "Fragment-shader ray marching integrates fog over an analytic scene.",
     "Rasterization and render/sample formats; no ray-tracing feature required.", Status::Limited, false,
     {dyf::RHI::Feature::Rasterization}, 1},
    {"AdvancedInstancingBatching", "14 / Instancing", "Submission",
     "Compare 256 draw calls with one instanced draw of the same quads.",
     "Rasterization.", Status::Implemented, true,
     {dyf::RHI::Feature::Rasterization}, 1},
    {"AdvancedResourceReuse", "15 / Resource reuse", "Submission",
     "Compare per-frame buffer creation with retained buffers and allocation/upload counters.",
     "Rasterization.", Status::Implemented, true,
     {dyf::RHI::Feature::Rasterization}, 1},
    {"AdvancedBindlessMaterials", "16 / Fixed texture arrays", "Submission",
     "Four populated texture slots indexed uniformly per draw; not general bindless materials.",
     "Default array mode needs descriptor indexing; --mode material provides the alternate path.", Status::Limited, false,
     {dyf::RHI::Feature::Rasterization, dyf::RHI::Feature::DescriptorIndexing}, 2},
    {"AdvancedIndirectDrawing", "17 / GPU-generated indirect draws", "Submission",
     "Compute generates draw and dispatch arguments consumed by GPU indirect commands.",
     "Rasterization, compute, indirect draw and indirect dispatch.", Status::Implemented, true,
     {dyf::RHI::Feature::Rasterization, dyf::RHI::Feature::Compute,
      dyf::RHI::Feature::IndirectDraw, dyf::RHI::Feature::IndirectDispatch}, 4},
    {"AdvancedGPUCulling", "18 / GPU frustum culling", "Submission",
     "Compute filters object spheres into visible indices and indirect instance counts.",
     "Rasterization, compute and indirect draw; no occlusion or meshlet LOD stage.", Status::Implemented, true,
     {dyf::RHI::Feature::Rasterization, dyf::RHI::Feature::Compute, dyf::RHI::Feature::IndirectDraw}, 3},
    {"AdvancedMultithreadRecording", "19 / Parallel recording (headless)", "Measurement",
     "Record independent RenderGraph passes on workers, then consume their textures.",
     "Headless command-line demonstration; Null checks contracts without native GPU readback.", Status::Limited, false,
     {}, 0},
    {"AdvancedAsyncCompute", "20 / Async compute", "Submission",
     "Status-only placeholder: no separate compute queue or queue synchronization path.",
     "Unimplemented; exits with status 77.", Status::Unimplemented, false,
     {}, 0},
    {"AdvancedRenderGraph", "21 / RenderGraph barriers", "Submission",
     "Compare manual barriers with compiled RenderGraph dependencies for the same two passes.",
     "Rasterization.", Status::Implemented, true,
     {dyf::RHI::Feature::Rasterization}, 1},
    {"AdvancedMeshShaderBasics", "22 / Mesh shader basics", "Geometry",
     "An actual mesh pipeline generates triangles without a vertex buffer.",
     "Mesh-shader support; unsupported devices exit with status 77.", Status::Implemented, false,
     {dyf::RHI::Feature::MeshShader}, 1},
    {"AdvancedMeshletCullingLOD", "23 / Meshlet culling and LOD", "Geometry",
     "Status-only placeholder: meshlet generation, visibility and LOD rendering are absent.",
     "Unimplemented; exits with status 77.", Status::Unimplemented, false,
     {}, 0},
}};

[[nodiscard]] inline constexpr const Entry* Find(std::string_view target)
{
    for(const auto& entry : AdvancedExamples)
        if(target == entry.target) return &entry;
    return nullptr;
}

[[nodiscard]] inline constexpr const char* StatusLabel(Status status)
{
    switch(status)
    {
    case Status::Implemented: return "Implemented";
    case Status::Limited: return "Limited";
    case Status::Unimplemented: return "Unimplemented";
    }
    return "Unknown";
}

// RT examples remain in source, but are outside the scene host's selection.
[[nodiscard]] inline constexpr bool IsRayTracing(std::string_view target)
{
    return target == "AdvancedAccelerationStructures" ||
           target == "AdvancedRayQueryShadows" ||
           target == "AdvancedRayTracingReflections" ||
           target == "AdvancedRayTracingDenoising" ||
           target == "AdvancedHybridRendering";
}
}
