#pragma once

#include "dyf/RHI/RenderGraph.h"
#include <array>
#include <chrono>
#include <string>
#include <vector>

namespace dyf
{
// GPU-only log-average luminance and time-based adaptation. The 4x4 pyramid
// stores mean log luminance plus a bounded coverage fraction, not large sums
// or pixel counts that would overflow RGBA16F at ordinary display resolutions.
class ExposureMeter
{
public:
    explicit ExposureMeter(RHI::IDevice& device):m_device(device) {}
    ~ExposureMeter();
    ExposureMeter(const ExposureMeter&)=delete;
    ExposureMeter& operator=(const ExposureMeter&)=delete;
    bool Prepare(uint32_t width,uint32_t height,RHI::ShaderHandle fullscreenVertex,float adaptationSpeed);
    bool AddPasses(RHI::RenderGraph&,RHI::TextureHandle source,RHI::RGResourceHandle sourceGraph);
    void CommitFrame();
    void Invalidate();
    RHI::TextureHandle GetImage() const {return m_output;}
    RHI::RGResourceHandle GetHandle() const {return m_outputGraph;}

private:
    using Clock=std::chrono::steady_clock;
    struct Target
    {
        RHI::TextureHandle image=nullptr;
        RHI::ResourceState state=RHI::ResourceState::Undefined;
    };
    struct CachedSet
    {
        std::array<RHI::TextureHandle,2> images={};
        RHI::ResourceSetHandle set=nullptr;
        uint64_t used=0;
    };
    bool PreparePipeline(RHI::ShaderHandle vertex);
    bool CreateTarget(Target&,uint32_t width,uint32_t height);
    void ReleaseTargets();
    bool AddDraw(RHI::RenderGraph&,const std::string& name,RHI::TextureHandle source,
        RHI::RGResourceHandle sourceGraph,RHI::TextureHandle previous,RHI::RGResourceHandle previousGraph,
        Target& output,RHI::RGResourceHandle outputGraph,const std::array<float,4>& operation);
    RHI::IDevice& m_device;
    RHI::ShaderHandle m_shader=nullptr;
    RHI::PipelineHandle m_pipeline=nullptr;
    std::vector<Target> m_levels;
    std::array<Target,2> m_history={};
    std::vector<CachedSet> m_sets;
    RHI::TextureHandle m_output=nullptr;
    RHI::RGResourceHandle m_outputGraph;
    Clock::time_point m_frameTime={},m_previousTime={};
    uint32_t m_width=0,m_height=0,m_write=0;
    uint64_t m_frame=0;
    float m_adaptationSpeed=1.5f;
    bool m_valid=false,m_pending=false;
};
}
