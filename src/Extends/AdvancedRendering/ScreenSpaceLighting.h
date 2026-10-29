#pragma once

#include "dyf/Camera.h"
#include "dyf/Material.h"
#include "dyf/RHI.h"
#include "dyf/Renderer.h"
#include "dyf/RenderExtension.h"
#include <array>
#include <unordered_map>
#include <vector>

namespace dyf
{
// Optional raster passes composed only from public RHI commands. The owning
// Renderer supplies stock-format geometry and a sampleable D32 depth target.
class ScreenSpaceLighting
{
public:
    using SurfaceDraw=RenderGeometryDraw;
    struct Settings
    {
        bool ambientOcclusion=true, diffuseGI=true, reflections=true;
        bool frustumCulling=true;
        float aoRadius=1.2f, aoStrength=1.0f, rayDistance=12.0f;
        float giStrength=1.0f, reflectionStrength=1.0f, thickness=0.25f;
        uint32_t frameIndex=0,aoFrameIndex=0;
        uint32_t surfaceMask=7; // normal/roughness/AO, albedo/metal, motion/linear-depth
    };
    struct GraphResources
    {
        RHI::RGResourceHandle normal, albedo, motion, ao, color;
    };
    ScreenSpaceLighting()=default;
    ~ScreenSpaceLighting();
    ScreenSpaceLighting(const ScreenSpaceLighting&)=delete;
    ScreenSpaceLighting& operator=(const ScreenSpaceLighting&)=delete;

    // Uploads are recorded into the caller's list, to be submitted before graph
    // execution. On recording/submission failure call Reset() before retrying.
    bool Prepare(RHI::IDevice&,RHI::ICommandList& uploads,RHI::TextureHandle depth,
        const Camera&,const std::vector<SurfaceDraw>&,const Settings&);
    bool Import(RHI::RenderGraph&);
    bool AddSurface(RHI::RenderGraph&,RHI::RGResourceHandle depth,
        RHI::RGResourceHandle instances={},RHI::RGResourceHandle arguments={});
    bool AddAO(RHI::RenderGraph&);
    // envSpec is exactly the linear HDR environment-specular contribution added
    // by the main pass. SSR replaces only its confidence-weighted part; it must
    // not be a copy of total radiance or the unweighted environment map.
    // With both effects disabled no pass is added and GetColor()/graph.color
    // alias the input HDR. envSpec is only required when reflections are enabled.
    bool AddLighting(RHI::RenderGraph&,RHI::RGResourceHandle hdr,RHI::TextureHandle hdrTexture,
        RHI::RGResourceHandle envSpec,RHI::TextureHandle envSpecTexture);
    // Shows the actual opaque/masked Surface attachments, without post effects.
    // Depth uses log2(1+viewDepth)/log2(1+farPlane); motion shows +/-32 source
    // pixels per frame in red/green. The caller supplies its camera's far plane.
    bool AddDebugView(RHI::RenderGraph&,RHI::TextureHandle output,RHI::RGResourceHandle outputGraph,
        SurfaceDebugView,float farPlane=160.f);
    void CommitFrame();
    void Invalidate();
    void Reset();

    // AO is in R. GBA are internal positive-view-depth/oct-normal filter guides.
    RHI::TextureHandle GetAO() const { return m_settings.ambientOcclusion?m_textures[AO]:m_whiteAO; }
    // xy는 팔면체 월드 법선, z는 지각적 거칠기(unlit은 음수), w는 원본 AO다.
    RHI::TextureHandle GetNormal() const { return m_textures[Normal]; }
    // xy = currentUV - previousUV, z = positive linear view depth,
    // w = previous positive linear view depth (>0 valid; invalid history is 0).
    // 카메라·오브젝트 이동과 이전 정점 버퍼의 변형을 함께 반영한다.
    RHI::TextureHandle GetMotion() const { return m_textures[Motion]; }
    RHI::TextureHandle GetAlbedoMetal() const { return m_textures[Albedo]; }
    RHI::TextureHandle GetColor() const { return m_colorOutput; }
    const GraphResources& GetGraphResources() const { return m_graph; }

private:
    enum ImageIndex : uint32_t {Normal,Albedo,Motion,AORaw,AOHorizontal,AO,GI,Reflection,Color,ImageCount};
    struct DrawGPU
    {
        Math::float4x4 model,previousMVP,normalMatrix;
        Math::float4 baseColor,materialParams;
        uint32_t flags=0,alphaMode=0,doubleSided=0,pipelineIndex=0;
        Math::float4 alpha;
    };
    struct FrameGPU
    {
        Math::float4x4 view,projection,inverseProjection,viewProjection;
        Math::float4 size,ao,lighting,options;
        std::array<uint32_t,4> outputs={};
        Math::float4x4 inversePreviousProjection;
    };
    struct SurfaceSetKey
    {
        std::array<RHI::TextureHandle,ToIndex(MaterialTextureKind::Count)> textures={};
        RHI::PipelineHandle pipeline=nullptr;
        bool operator==(const SurfaceSetKey& other) const
        {return pipeline==other.pipeline && textures==other.textures;}
    };
    struct SurfaceSetHash
    {
        size_t operator()(const SurfaceSetKey& key) const
        {
            size_t hash=std::hash<RHI::PipelineHandle>{}(key.pipeline);
            for(auto* texture:key.textures)
                hash^=std::hash<RHI::TextureHandle>{}(texture)+0x9e3779b9u+(hash<<6)+(hash>>2);
            return hash;
        }
    };
    struct ScreenSet
    {
        std::array<RHI::TextureHandle,8> textures={};
        RHI::PipelineHandle pipeline=nullptr;
        RHI::ResourceSetHandle set=nullptr;
        uint32_t unusedFrames=0;
    };
    struct DenoiseTarget
    {
        RHI::TextureHandle image=nullptr;
        RHI::ResourceState state=RHI::ResourceState::Undefined;
    };
    struct EffectHistory
    {
        // 각 슬롯은 색/신뢰도, 로그 휘도 모멘트, 깊이/법선/거칠기다.
        std::array<std::array<DenoiseTarget,3>,2> frames={};
        std::array<DenoiseTarget,2> spatial={};
        uint32_t write=0;
        bool valid=false,pending=false;
    };
    struct DenoiseSet
    {
        std::array<RHI::TextureHandle,6> images={};
        RHI::PipelineHandle pipeline=nullptr;
        RHI::ResourceSetHandle set=nullptr;
        uint32_t unusedFrames=0;
    };
    bool CreatePipelines(uint32_t surfaceMask);
    void PrepareDepthPlane(bool enabled);
    bool CreateDenoisePipelines();
    bool PrepareDenoise(uint32_t effect,bool enabled);
    void ReleaseDenoise(uint32_t effect);
    RHI::ResourceSetHandle DenoiseResources(RHI::PipelineHandle,const std::array<RHI::TextureHandle,6>&);
    bool AddDenoise(RHI::RenderGraph&,uint32_t effect,RHI::TextureHandle raw,RHI::RGResourceHandle rawGraph,
        RHI::TextureHandle& filtered,RHI::RGResourceHandle& filteredGraph,
        RHI::TextureHandle& guide,RHI::RGResourceHandle& guideGraph);
    bool Upload(RHI::ICommandList&,RHI::BufferHandle&,bool& ready,
        const void*,uint32_t,uint32_t,RHI::BufferUsage,RHI::ResourceState,uint32_t capacityBytes=0);
    RHI::ResourceSetHandle SurfaceResources(const SurfaceDraw&,RHI::PipelineHandle);
    RHI::ResourceSetHandle ScreenResources(RHI::PipelineHandle,RHI::TextureHandle source,
        RHI::TextureHandle hdr,RHI::TextureHandle environment,RHI::TextureHandle reflection,
        RHI::TextureHandle guide=nullptr);
    void DrawScreen(RHI::ICommandList&,RHI::PipelineHandle,RHI::ResourceSetHandle,
        RHI::TextureHandle output,const std::array<float,4>&);
    void DestroySets();
    RHI::IDevice* m_device=nullptr;
    RHI::TextureHandle m_depth=nullptr;
    RHI::TextureHandle m_depthPlane=nullptr;
    RHI::ResourceState m_depthPlaneState=RHI::ResourceState::Undefined;
    RHI::RGResourceHandle m_depthPlaneHandle;
    RHI::ShaderHandle m_depthCopyShader=nullptr;
    RHI::PipelineHandle m_depthCopyPipeline=nullptr;
    RHI::ResourceSetHandle m_depthCopySet=nullptr;
    bool m_depthPlaneAttempted=false,m_depthPlaneActive=false;
    RHI::TextureHandle m_whiteAO=nullptr,m_colorOutput=nullptr;
    std::array<RHI::TextureHandle,ImageCount> m_textures={};
    std::array<RHI::ResourceState,ImageCount> m_states={};
    std::array<RHI::RGResourceHandle,ImageCount> m_handles={};
    std::array<bool,ImageCount> m_imported={};
    RHI::RGResourceHandle m_frameHandle,m_drawHandle,m_depthHandle;
    GraphResources m_graph;
    std::array<RHI::ShaderHandle,6> m_shaders={};
    std::array<RHI::PipelineHandle,32> m_surfacePipelines={};
    std::array<RHI::ShaderHandle,3> m_compactSurfaceShaders={};
    RHI::PipelineHandle m_screenPipeline=nullptr,m_lightingPipeline=nullptr;
    RHI::PipelineHandle m_debugPipeline=nullptr;
    RHI::Format m_debugFormat=RHI::Format::Unknown;
    RHI::BufferHandle m_frameBuffer=nullptr,m_drawBuffer=nullptr;
    bool m_frameReady=false,m_drawReady=false,m_historyValid=false,m_prepared=false;
    uint32_t m_width=0,m_height=0;
    Settings m_settings;
    FrameGPU m_uploadedFrame={};
    Math::float4x4 m_previousInverseProjection={};
    std::vector<DrawGPU> m_gpuDraws,m_gpuScratch;
    std::vector<SurfaceDraw> m_draws;
    std::vector<uint32_t> m_drawOrder;
    std::vector<double> m_drawDepths;
    std::vector<RHI::ResourceSetHandle> m_drawSets;
    std::vector<ScreenSet> m_screenSets;
    std::unordered_map<SurfaceSetKey,RHI::ResourceSetHandle,SurfaceSetHash> m_surfaceSets;
    std::array<EffectHistory,2> m_effectHistory={};
    std::array<RHI::ShaderHandle,2> m_denoiseShaders={};
    std::array<RHI::PipelineHandle,2> m_denoisePipelines={};
    std::vector<DenoiseSet> m_denoiseSets;
};
}
