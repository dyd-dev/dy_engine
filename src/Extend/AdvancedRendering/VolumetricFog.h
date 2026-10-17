#pragma once

#include "dyf/Camera.h"
#include "dyf/RHI/RenderGraph.h"
#include <array>
#include <vector>

namespace dyf
{
class VolumetricFog
{
public:
    struct Settings
    {
        bool enabled=false;
        float density=.01f,heightFalloff=.1f,baseHeight=0;
        Math::float3 albedo={.9f,.95f,1.f};
        float anisotropy=.35f,maxDistance=80.f,resolutionScale=.5f;
        uint32_t steps=48;
    };
    struct LightingResources
    {
        RHI::BufferHandle lights=nullptr,shadows=nullptr;
        RHI::TextureHandle shadowDepth=nullptr;
        RHI::RGResourceHandle lightsHandle,shadowsHandle,shadowDepthHandle;
    };
    VolumetricFog()=default;
    ~VolumetricFog();
    VolumetricFog(const VolumetricFog&)=delete;
    VolumetricFog& operator=(const VolumetricFog&)=delete;
    // 업로드 목록을 제출한 뒤 CommitUploads를 호출한다. 실패하면 Reset으로 폐기한다.
    bool Prepare(RHI::IDevice&,RHI::ICommandList& uploads,const Camera&,
        uint32_t width,uint32_t height,const Settings&);
    bool AddPasses(RHI::RenderGraph&,RHI::TextureHandle hdr,RHI::RGResourceHandle hdrHandle,
        RHI::TextureHandle depth,RHI::RGResourceHandle depthHandle,const LightingResources&);
    void CommitUploads();
    void CommitFrame();
    void Reset();
    RHI::TextureHandle GetColor() const {return m_colorOutput;}
    RHI::RGResourceHandle GetHandle() const {return m_colorHandle;}

private:
    struct Frame
    {
        Math::float4x4 inverseViewProjection,view;
        Math::float4 camera,size,fog,scattering;
        std::array<uint32_t,4> options;
    };
    struct CachedSet
    {
        std::array<RHI::TextureHandle,4> images={};
        RHI::BufferHandle lights=nullptr,shadows=nullptr;
        RHI::ResourceSetHandle resources=nullptr;
        uint64_t used=0;
    };
    bool PreparePipeline();
    void ReleaseTargets();
    void ReleaseSets();
    RHI::ResourceSetHandle Resources(const std::array<RHI::TextureHandle,4>&,
        RHI::BufferHandle lights,RHI::BufferHandle shadows);
    RHI::IDevice* m_device=nullptr;
    RHI::ShaderHandle m_vertex=nullptr,m_fragment=nullptr;
    RHI::PipelineHandle m_pipeline=nullptr;
    RHI::BufferHandle m_frame=nullptr,m_noShadow=nullptr;
    RHI::TextureHandle m_fog=nullptr,m_color=nullptr,m_colorOutput=nullptr;
    RHI::ResourceState m_fogState=RHI::ResourceState::Undefined,m_colorState=RHI::ResourceState::Undefined;
    RHI::RGResourceHandle m_colorHandle;
    Settings m_settings;
    std::vector<CachedSet> m_sets;
    uint64_t m_cacheFrame=0;
    uint32_t m_width=0,m_height=0;
    bool m_uploadReady=false,m_uploadPending=false,m_pending=false,m_prepared=false;
};
}
