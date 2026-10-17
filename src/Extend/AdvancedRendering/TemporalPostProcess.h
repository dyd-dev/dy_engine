#pragma once

#include "dyf/Camera.h"
#include "dyf/RendererConfig.h"
#include "dyf/RHI/RenderGraph.h"
#include "dyf/RHI/Format.h"
#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace dyf
{
    class ExposureMeter;
    // Renderer-owned RHI passes. No window, queue policy, readback, or CPU wait.
    class TemporalPostProcess
    {
    public:
        struct Settings
        {
            bool taa = true;
            bool bloom = true;
            bool debugColor = false; // linear display colors; skip exposure/tone mapping
            bool autoExposure = false;
            float exposureKey = 0.18f;
            float exposureAdaptationSpeed = 1.5f;
            float bloomStrength = 0.055f;
            float bloomThreshold = 1.0f;
            float bloomKnee = 0.5f;
            float historyWeight = 0.9f;
            PostProcessConfig output = {};
        };

        explicit TemporalPostProcess(RHI::IDevice& device);
        ~TemporalPostProcess();
        TemporalPostProcess(const TemporalPostProcess&) = delete;
        TemporalPostProcess& operator=(const TemporalPostProcess&) = delete;

        bool Prepare(uint32_t width, uint32_t height, RHI::Format outputFormat, const Settings& settings);
        // Pixel offsets use the render target's top-left origin. Apply to the
        // camera before both the surface pass and the main geometry pass.
        [[nodiscard]] Math::float2 JitterPixels() const;
        static void ApplyJitter(Camera& camera, Math::float2 pixels, uint32_t width, uint32_t height);
        // Both matrices use unjittered cameras. Sky ignores camera translation;
        // its colors still share geometry's jittered current-image sample grid.
        void SetSkyReprojection(const Camera& current,const Math::float4x4& previousView,
            const Math::float4x4& previousProjection,bool validHistory);

        // motionDepth: xy=currentUV-previousUV, z=current positive view depth,
        // w=previous positive view depth, or <=0 when previous geometry is invalid.
        // hdr and motionGraph are already imported by the caller. Internal
        // targets are imported here; every internal final state is ShaderResource.
        // Reactive R is transparent coverage. TAA requires this input; a black
        // 1x1 texture is sufficient when the scene has no transparent surfaces.
        bool AddPasses(RHI::RenderGraph& graph,
            RHI::TextureHandle hdr, RHI::RGResourceHandle hdrGraph,
            RHI::TextureHandle motionDepth, RHI::RGResourceHandle motionGraph,
            RHI::TextureHandle output, RHI::RGResourceHandle outputGraph,
            float exposure, RHI::TextureHandle reactive, RHI::RGResourceHandle reactiveGraph);
        // Call exactly once after successful submission of the added passes.
        void CommitFrame();
        void InvalidateHistory();

    private:
        enum Program : uint32_t { Resolve, Downsample, Upsample, Filmic, ProgramCount };
        struct Target
        {
            RHI::TextureHandle image = nullptr;
            RHI::ResourceState state = RHI::ResourceState::Undefined;
        };
        struct CachedSet
        {
            RHI::ResourceSetHandle set = nullptr;
            RHI::PipelineHandle pipeline = nullptr;
            std::array<RHI::TextureHandle, 4> images = {};
            uint64_t used = 0;
        };

        bool PreparePrograms(RHI::Format outputFormat);
        bool CreateTarget(Target& target, uint32_t width, uint32_t height);
        void ReleaseTargets();
        void ReleaseSets();
        RHI::ResourceSetHandle ResourceSet(Program program,
            const std::array<RHI::TextureHandle, 4>& images);
        bool AddDraw(RHI::RenderGraph& graph, const char* name, Program program,
            const std::array<RHI::TextureHandle, 4>& images,
            const std::array<RHI::RGResourceHandle, 4>& reads,
            RHI::TextureHandle output, RHI::RGResourceHandle outputGraph,
            const std::array<float, 8>& settings);

        RHI::IDevice& m_device;
        std::unique_ptr<ExposureMeter> m_exposure;
        Settings m_settings;
        uint32_t m_width = 0, m_height = 0;
        RHI::Format m_outputFormat = RHI::Format::Unknown;
        std::array<RHI::ShaderHandle, ProgramCount + 1> m_shaders = {};
        std::array<RHI::PipelineHandle, ProgramCount> m_pipelines = {};
        std::array<Target, 2> m_history = {};
        std::vector<Target> m_down, m_up;
        std::vector<CachedSet> m_sets;
        uint64_t m_frame = 0, m_cacheFrame = 0;
        uint32_t m_historyWrite = 0;
        Math::float2 m_previousJitterPixels = {0, 0};
        Math::float4x4 m_skyCurrentToPreviousClip = {};
        bool m_historyValid = false;
        bool m_pending = false, m_pendingHistory = false, m_pendingBloom = false;
    };
}
