#pragma once
#include "ScreenSpaceLighting.h"
#include "dyf/RenderResources.h"

namespace dyf
{
// Sorted raster transparency; transmission samples the completed opaque HDR.
class TransparentRendering
{
public:
    TransparentRendering()=default;
    TransparentRendering(const TransparentRendering&)=delete;
    TransparentRendering& operator=(const TransparentRendering&)=delete;
    ~TransparentRendering();
    bool Prepare(RHI::IDevice&,RHI::ICommandList&,const Camera&,
        const std::vector<ScreenSpaceLighting::SurfaceDraw>&,const EnvironmentDesc&,
        const RenderEnvironmentResources&,bool frustumCulling,uint32_t layers=1);
    bool AddPasses(RHI::RenderGraph&,RHI::TextureHandle opaque,RHI::RGResourceHandle opaqueHandle,
        RHI::TextureHandle depth,RHI::RGResourceHandle depthHandle,RHI::BufferHandle lights,
        RHI::TextureHandle shadowDepth,RHI::RGResourceHandle shadowDepthHandle,RHI::BufferHandle shadowConstants,
        const RenderLightingResources&);
    bool HasDraws() const { return !m_draws.empty(); }
    RHI::TextureHandle GetColor() const { return m_color; }
    RHI::RGResourceHandle GetColorHandle() const { return m_colorHandle; }
    RHI::TextureHandle GetReactive() const { return m_reactive; }
    RHI::RGResourceHandle GetReactiveHandle() const { return m_reactiveHandle; }
    void Invalidate() {m_draws.clear();}
    void CommitFrame()
    {
        if(m_recorded) m_state=RHI::ResourceState::ShaderResource;
        if(m_layeredRecorded) m_rearState=m_nearestState=RHI::ResourceState::ShaderResource;
        if(m_noShadowPending) m_noShadowReady=true;
        m_recorded=false;m_layeredRecorded=false;m_noShadowPending=false;m_draws.clear();
    }
private:
    struct DrawGPU
    {
        Math::float4x4 model,normalMatrix;
        Math::float4 baseColor,emissive,material,optics,attenuation;
        uint32_t flags=0,doubleSided=0,pipelineIndex=0,pad1=0;
    };
    struct FrameGPU
    {
        Math::float4x4 viewProjection,view,inverseViewProjection;
        Math::float4 camera;
        RenderEnvironmentConstants environment;
        Math::float4 specularColor;
    };
    static_assert(sizeof(DrawGPU)==224 && sizeof(FrameGPU)==448);
    struct CachedSet
    {
        RHI::PipelineHandle pipeline=nullptr;
        std::vector<RHI::ResourceBinding> bindings;
        RHI::ResourceSetHandle set=nullptr;
        uint32_t unusedFrames=0;
    };
    bool CreatePipelines();
    bool Upload(RHI::ICommandList&,RHI::BufferHandle&,bool&,const void*,uint32_t,uint32_t,RHI::BufferUsage);
    RHI::ResourceSetHandle Resources(RHI::PipelineHandle,const std::vector<RHI::ResourceBinding>&);
    RHI::IDevice* m_device=nullptr;
    std::array<RHI::ShaderHandle,4> m_shaders={};
    std::array<RHI::PipelineHandle,4> m_pipelines={};
    std::array<RHI::PipelineHandle,4> m_depthPipelines={};
    RHI::PipelineHandle m_copyPipeline=nullptr;
    RHI::TextureHandle m_color=nullptr,m_reactive=nullptr,m_prefilter=nullptr;
    RHI::TextureHandle m_rearColor=nullptr,m_nearestDepth=nullptr;
    RHI::TextureHandle m_probePrefilter=nullptr;
    RHI::BufferHandle m_probeData=nullptr,m_probeParameters=nullptr;
    RHI::BufferHandle m_frameBuffer=nullptr,m_drawBuffer=nullptr,m_noShadowBuffer=nullptr;
    bool m_frameReady=false,m_drawReady=false,m_recorded=false;
    bool m_layeredRecorded=false;
    bool m_noShadowReady=false,m_noShadowPending=false;
    RHI::ResourceState m_state=RHI::ResourceState::Undefined;
    RHI::ResourceState m_rearState=RHI::ResourceState::Undefined,m_nearestState=RHI::ResourceState::Undefined;
    uint32_t m_layers=1;
    RHI::RGResourceHandle m_colorHandle;
    RHI::RGResourceHandle m_reactiveHandle;
    std::vector<ScreenSpaceLighting::SurfaceDraw> m_draws;
    std::vector<DrawGPU> m_data,m_uploaded;
    std::vector<CachedSet> m_sets;
};
}
