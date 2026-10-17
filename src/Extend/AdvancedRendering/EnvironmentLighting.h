#pragma once
#include "dyf/Camera.h"
#include "dyf/RendererConfig.h"
#include "dyf/RenderResources.h"
#include "dyf/RHI.h"
#include <array>
#include <vector>

namespace dyf
{
// 공용 RHI 명령으로 환경광과 배경 패스를 구성한다.
class EnvironmentLighting
{
public:
    using Constants=RenderEnvironmentConstants;
    static_assert(sizeof(Constants)==224);
    EnvironmentLighting()=default;
    ~EnvironmentLighting();
    EnvironmentLighting(const EnvironmentLighting&)=delete;
    EnvironmentLighting& operator=(const EnvironmentLighting&)=delete;
    // Upload commands must be submitted before graph execution. Reset after a
    // recording/submission failure before retrying; no CPU wait is introduced.
    bool Prepare(RHI::IDevice&,RHI::ICommandList& uploads,const EnvironmentDesc&,
        const Camera&,uint32_t width,uint32_t height,bool ambientOcclusion=true,bool active=true,
        const Math::float4x4* backgroundProjection=nullptr,uint32_t preparationThreads=1);
    bool Import(RHI::RenderGraph&);
    // RGB = SH irradiance/pi * environment intensity * authored AO * screen AO.
    // The main material shader multiplies this by kD * albedo exactly once.
    bool AddDiffuse(RHI::RenderGraph&,RHI::RGResourceHandle depth,RHI::TextureHandle depthTexture,
        RHI::RGResourceHandle normal,RHI::TextureHandle normalTexture,
        RHI::RGResourceHandle ao,RHI::TextureHandle aoTexture);
    // normal.xy is octahedral world normal packed into [0,1]. z roughness, w authored AO.
    bool AddSpecular(RHI::RenderGraph&,RHI::RGResourceHandle depth,RHI::TextureHandle depthTexture,
        RHI::RGResourceHandle normal,RHI::TextureHandle normalTexture,
        RHI::RGResourceHandle albedo,RHI::TextureHandle albedoTexture,
        RHI::RGResourceHandle ao,RHI::TextureHandle aoTexture);
    bool AddBackground(RHI::RenderGraph&,RHI::RGResourceHandle hdr,RHI::TextureHandle hdrTexture,
        RHI::RGResourceHandle depth,RHI::TextureHandle depthTexture);
    RHI::TextureHandle GetSpecular() const { return m_specularEnabled ? m_specular : m_blackSpecular; }
    RHI::TextureHandle GetDiffuse() const { return m_diffuseEnabled ? m_diffuse : m_blackSpecular; }
    RHI::RGResourceHandle GetDiffuseHandle() const { return m_diffuseHandle; }
    bool IsDiffuseEnabled() const { return m_diffuseEnabled; }
    bool IsSpecularEnabled() const { return m_specularEnabled; }
    bool IsBackgroundEnabled() const { return m_active && m_description.radiance.IsValid() && m_description.background; }
    RHI::RGResourceHandle GetSpecularHandle() const { return m_specularHandle; }
    RHI::BufferHandle GetConstants() const { return m_constants; }
    RHI::TextureHandle GetPrefilter() const { return m_prefilter; }
    const Constants& GetCPUConstants() const { return m_data; }
    RHI::BufferHandle GetProbeData() const { return m_probeData; }
    RHI::TextureHandle GetProbePrefilter() const { return m_probePrefilter; }
    RHI::BufferHandle GetProbeParameters() const { return m_probeParameters; }
    void CommitFrame();
    void Reset();
private:
    struct ProbeGPU
    {
        Math::float4 positionIntensity,boxMinFade,boxMaxMip;
        std::array<Math::float4,9> sh;
    };
    static_assert(sizeof(ProbeGPU)==192);
    struct ProbeCache
    {
        HdrImage source,diffuseSource,visibility;
        std::array<Math::float4,9> sh={};
        std::vector<std::vector<float>> prefilter;
        std::vector<std::vector<float>> moments;
        uint32_t width=0,height=0,maxReflectionResolution=0;
        float distanceScale=0;
    };
    struct FrameConstants
    {
        Math::float4x4 inverseViewProjection;
        Math::float4 cameraPosition;
        Constants environment;
        Math::float4 specularColor;
        // Translation-free inverse projection/view on the current camera sample grid.
        Math::float4x4 backgroundRayToWorld;
    };
    bool CreatePipelines();
    bool BuildEnvironment(RHI::ICommandList&,const HdrImage&,const HdrImage& diffuseSource,uint32_t preparationThreads);
    bool BuildProbes(RHI::ICommandList&,const std::vector<EnvironmentProbeDesc>&,uint32_t preparationThreads);
    bool UploadConstants(RHI::ICommandList&,RHI::BufferHandle&,bool& ready,
        std::vector<uint8_t>& cachedBytes,const void*,uint32_t,RHI::BufferUsage usage=RHI::BufferUsage::Constant);
    void ReadProbes(RHI::RenderGraph&,RHI::RenderGraphPass&);
    void Draw(RHI::ICommandList&,RHI::PipelineHandle,RHI::ResourceSetHandle,RHI::TextureHandle,bool load);
    RHI::IDevice* m_device=nullptr;
    HdrImage m_source,m_diffuseSource;
    EnvironmentDesc m_description;
    Constants m_data={};
    std::array<Math::float4,9> m_irradiance={};
    FrameConstants m_frame={};
    RHI::TextureHandle m_radiance=nullptr,m_prefilter=nullptr,m_brdf=nullptr,m_white=nullptr,m_specular=nullptr;
    RHI::TextureHandle m_diffuse=nullptr;
    RHI::TextureHandle m_blackSpecular=nullptr;
    RHI::BufferHandle m_constants=nullptr,m_frameBuffer=nullptr;
    RHI::TextureHandle m_probePrefilter=nullptr;
    RHI::BufferHandle m_probeData=nullptr,m_probeParameters=nullptr;
    std::vector<ProbeCache> m_probeCache;
    std::vector<size_t> m_probeIndices;
    std::vector<uint8_t> m_probeBytes,m_probeParameterBytes;
    bool m_probeReady=false,m_probeParametersReady=false;
    std::vector<uint8_t> m_constantBytes,m_frameBytes;
    bool m_constantsReady=false,m_frameReady=false,m_prepared=false;
    bool m_active=false,m_diffuseEnabled=false,m_specularEnabled=false,m_specularRecorded=false,m_diffuseRecorded=false;
    RHI::ResourceState m_specularState=RHI::ResourceState::Undefined;
    RHI::ResourceState m_diffuseState=RHI::ResourceState::Undefined;
    RHI::RGResourceHandle m_specularHandle,m_diffuseHandle;
    std::array<RHI::ShaderHandle,4> m_shaders={};
    RHI::PipelineHandle m_specularPipeline=nullptr,m_backgroundPipeline=nullptr;
    RHI::PipelineHandle m_diffusePipeline=nullptr;
    RHI::ResourceSetHandle m_specularSet=nullptr,m_backgroundSet=nullptr;
    RHI::ResourceSetHandle m_diffuseSet=nullptr;
    uint32_t m_width=0,m_height=0;
};
}
