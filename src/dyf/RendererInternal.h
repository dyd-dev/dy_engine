#pragma once
#include "../Platform/Profiler/CollectorInternal.h"

#include "dyf/Renderer.h"
#include "ShaderLayout.h"
#include "DepthPrepassValidation.h"
#include "dyf/RenderExtension.h"
#include "dyf/RHI/IDevice.h"
#include "Platform/Profiler/FrameProfiler.h"
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <tuple>

namespace dyf
{
class BasicEnvironmentResources;
class BasicLightingResources;
class GpuVisibility;
class OutputPresentation;
struct RenderFrame;

struct Renderer::Impl
{
    Renderer& owner;
    ~Impl();

    [[nodiscard]] bool Render(const MeshData&, Image* readback = nullptr);
    [[nodiscard]] bool Render(const MeshData&, const Camera&, Image* readback = nullptr);
    [[nodiscard]] bool Render(const MeshData&, const MaterialDesc&, const Camera&, Image* readback = nullptr);
    [[nodiscard]] bool Render(const Scene&, Image* readback = nullptr);
    [[nodiscard]] bool Render(const Scene&, const Camera&, Image* readback = nullptr);
    [[nodiscard]] bool Render(const Canvas&, Image* readback = nullptr);
    [[nodiscard]] bool Render(const Scene&, const Canvas& overlay, Image* readback = nullptr);
    [[nodiscard]] bool Render(const Scene&, const Camera&, const Canvas& overlay, Image* readback = nullptr);

    [[nodiscard]] bool RenderToTexture(const Scene&, const Camera&, RHI::TextureHandle target,
        RHI::ResourceState before);

    [[nodiscard]] bool CaptureEnvironment(const Scene&,Math::float3 position,uint32_t faceSize,
        float nearPlane,float farPlane,HdrImage& result,bool includeEmitters=true,HdrImage* visibility=nullptr);

    [[nodiscard]] bool Render(const Scene&, const Camera&, const std::vector<RendererDrawDesc>&,
        const Canvas* overlay = nullptr, Image* readback = nullptr);
    [[nodiscard]] bool Render(const Scene&, const std::vector<RendererDrawDesc>&,
        const Canvas* overlay = nullptr, Image* readback = nullptr);
    [[nodiscard]] RHI::IDevice& GetDevice() const { return *device; }

    [[nodiscard]] const Platform::ProfilerTimingSnapshot& GetProfilerTimingSnapshot() const { return m_profiler.GetSnapshot(); }

    [[nodiscard]] const RendererStatistics& GetLastRenderStatistics() const { return lastStatistics; }
    [[nodiscard]] RHI::FenceHandle GetLastRenderCompletion() const { return lastRenderCompletion; }
    bool SetClearColor(Math::float4);
    bool SetVSync(bool);
    [[nodiscard]] const LightingConfig& GetLighting() const { return GetConfig().lighting; }
    void InvalidateHistory() { InvalidateFrameHistory(); visibilityKeys.clear(); }
    bool SetLighting(const LightingConfig&);
    bool SetLightingEnabled(bool);
    bool SetShadowsEnabled(bool);
    bool SetHdrEnabled(bool);
    bool SetExposure(float);
    bool SetProfilerVisible(bool);
    bool SetReadbackEnabled(bool);
    bool AddExtension(std::unique_ptr<RenderExtension>);
    [[nodiscard]] const RendererConfig& GetConfig() const { return pendingConfig ? *pendingConfig : config; }
    bool SetConfig(const RendererConfig&);
    bool SetShaders(const RendererShaderDesc&);
    [[nodiscard]] RendererShaderDesc GetShaders() const;

    void InitializeLiveProfiling() noexcept;
    bool PrepareLiveFrame(Platform::LiveFrameKey) noexcept;
    void FinishLiveSettings(bool,Platform::LiveFrameKey) noexcept;
    void PublishLiveGeometryAvailability(bool customDraws) noexcept;
    Platform::LiveId liveId=0;
    std::array<Platform::ControlRegistration,16> liveControls;
    std::array<Platform::LiveId,16> liveRequests{};
    std::array<Platform::ControlRegistration,5> liveCatalog;
    bool liveCustomDraws=false;
    Platform::LiveId livePreparedFrame=0;
    RendererConfig liveAppliedConfig;
    bool SupportsBindlessMaterials() const;

    struct ShadowData
    {
        RendererShadowConstants constants={};
        uint32_t viewCount=0,columns=1,rows=1,resolution=1;
        float directionalDistance=0;
    };
    struct SceneMaterialState
    {
        std::array<RHI::TextureHandle,ToIndex(MaterialTextureKind::Count)> textures={};
        uint32_t textureFlags=0;
    };
    struct CompactShadowVertex
    {
        float px,py,pz,u,v;
    };
    static_assert(sizeof(CompactShadowVertex)==20);
    struct ShadowArenaRange
    {
        uint64_t generation=0;
        uint32_t baseVertex=0,indexByteOffset=0;
        RHI::Format indexFormat=RHI::Format::R32_UINT;
        bool sortSafeInputs=false;
    };
    struct SceneMeshState
    {
        struct IndexLod
        {
            RHI::BufferHandle buffer=nullptr;
            uint32_t count=0;
            float error=0;
            bool ready=false;
            std::vector<uint32_t> indices;
        };
        RHI::BufferHandle vertexBuffer=nullptr,indexBuffer=nullptr,shadowVertexBuffer=nullptr,shadowIndexBuffer=nullptr,mainIndexBuffer=nullptr;
        RHI::Format shadowIndexFormat=RHI::Format::R32_UINT;
        ShadowArenaRange shadowArena;
        uint32_t indexCount=0;
        std::vector<IndexLod> lods;
        Math::Bounds3 bounds;
        std::weak_ptr<const MeshData> source;
        // Ready caches also record geometry that needs no extra buffers.
        bool vertexReady=false,indexReady=false,shadowVertexReady=false,shadowIndexReady=false,mainIndexReady=false,prepared=false,lodsGenerated=false;
    };
    struct ShadowBounds
    {
        Math::Bounds3 bounds;
        std::array<double,3> magnitude={};
    };
    struct ShadowCaster
    {
        uint32_t entity=0,mesh=0,material=0,batch=0;
        bool mirrored=false,compact=false,arena=false,sortSafe=false;
        RHI::PipelineHandle pipeline=nullptr;
        RHI::ResourceSetHandle set=nullptr;
        ShadowBounds worldBounds;
    };
    struct CachedSet { RHI::ResourceSetHandle set=nullptr; uint32_t unusedFrames=0; size_t keyHash=0; };
    struct ShadowAtlasCache
    {
        bool valid=false,pending=false,reused=false;
        RHI::TextureHandle target=nullptr;
        std::weak_ptr<Scene> scene;
        std::vector<uint8_t> bytes,pendingBytes;
        std::vector<std::weak_ptr<const MeshData>> meshes,pendingMeshes;
    };
    struct TextureSlot
    {
        Image source;
        RHI::TextureHandle texture=nullptr;
        RHI::ResourceState state=RHI::ResourceState::Undefined;
        bool used=false;
    };
    struct CanvasImageSlot
    {
        Image source;
        RHI::TextureHandle texture=nullptr;
        RHI::ResourceSetHandle set=nullptr;
        uint32_t unusedFrames=0;
        bool used=false;
    };
    struct CanvasVertexSlot { RHI::BufferHandle buffer=nullptr; bool ready=false; };
    struct CanvasFrame
    {
        Impl& owner;
        size_t imageCount;
        bool committed=false;
        explicit CanvasFrame(Impl&);
        ~CanvasFrame();
        void Commit();
    };
    struct FrameEnd
    {
        RHI::IDevice& device;
        bool& pending;
        ~FrameEnd();
    };
    enum ShaderSlot : uint32_t {MeshVertex,MeshFragment,ShadowVertex,CanvasVertex,CanvasFragment,ToneVertex,ToneFragment,ShaderCount};
    struct ShaderSources
    {
        std::array<std::vector<uint8_t>,ShaderCount> bytes;
        std::array<std::string,ShaderCount> entries;
        std::vector<RHI::ResourceBindingLayout> vertexBindings;
        uint32_t additionalConstantBytes = 0;
        bool operator==(const ShaderSources&) const;
    };

    Impl(Renderer&,RHI::IDevice&,const void* window,const RendererConfig&);
    static RendererShaderDesc DefaultShaders(bool shadows = false, bool bindless = false, bool instanced = false);
    static RHI::ShaderDesc DefaultShadowFragment();
    static RHI::ShaderDesc DefaultDepthPrepassFragment(bool shadows,bool bindless);
    static RHI::ShaderDesc DefaultNoAreaFragment(bool shadows,bool bindless,bool depthPrepass);
    RHI::PipelineHandle MaterialPipeline(const MaterialDesc&,bool mirrored=false) const;
    bool CaptureFrame(Image&);
    bool Initialize();
    bool InitializeMesh(RHI::TextureHandle output = nullptr,bool compositeAlpha = false);
    bool UsesBindlessMaterials() const;
    bool UsesStockGeometry() const;
    RHI::BufferHandle MainIndexBuffer(const SceneMeshState&,uint32_t,const MaterialDesc&,const RendererDrawDesc*) const;
    bool UsesAdvancedRendering() const;
    bool NeedsSurfacePass() const;
    RenderRequirements CollectRequirements() const;
    bool ValidateModules(const RenderRequirements&) const;
    bool UsesSeparateTransparency() const;
    bool HasOutputExtension() const;
    void PrepareRenderGeometry(RenderFrame&) const;
    void UpdateFrameLighting(RenderFrame&,bool graphReady=false) const;
    bool AddExtensionPasses(RenderStage,RenderFrame&,RHI::RenderGraph&);
    bool UsesGpuVisibility() const;
    bool PrepareVisibility(const Scene&,const Camera&,RHI::ICommandList&,uint32_t,uint32_t,
        const std::vector<RendererDrawDesc>*);
    bool PrepareCpuBatches(const Scene&,const Camera&,RHI::ICommandList&,const std::vector<RendererDrawDesc>*,bool motion);
    bool PrepareFrameResources(RenderFrame&,RHI::ICommandList&);
    void CommitFrameResources(const RenderFrame&);
    void InvalidateFrameHistory(bool release = false);
    bool UsesInstanceStream() const;
    bool UpdateInstanceBuffer(const Scene&,RHI::ICommandList&);
    void CommitFrameData();
    RHI::ResourceSetHandle CachedResourceSet(RHI::PipelineHandle,const std::vector<RHI::ResourceBinding>&,std::vector<CachedSet>&);
    void PruneResourceSets(std::vector<CachedSet>&);
    RHI::PipelineLayoutDesc MeshLayout(bool bindless, std::vector<RHI::ResourceBindingLayout>& bindings) const;
    void Shutdown();
    bool RenderScene(const Scene&,const Camera*,const Canvas*,Image*,const std::vector<RendererDrawDesc>* = nullptr,
        RHI::TextureHandle output = nullptr, RHI::ResourceState before = RHI::ResourceState::Present);
    bool BuildPipelineStates(RHI::IDevice*,RHI::Format colorFormat,bool compositeAlpha);
    bool CreateDefaultMaterialTextures(RHI::IDevice*);
    bool EnsureDepthStencilTarget(RHI::IDevice*,RHI::TextureHandle output,bool sampled);
    bool EnsureShadowDepthTarget(RHI::IDevice*,uint32_t columns,uint32_t rows,uint32_t resolution);
    void UpdateMaterialStates(const Scene&);
    bool UpdateLightingBuffer(const Scene&,const Camera&,RHI::IDevice*,RHI::ICommandList&);
    bool UpdateShadowBuffer(RHI::IDevice*,RHI::ICommandList&,const ShadowData&);
    void BuildShadows(ShadowData&,const Scene&,const Camera&,const std::vector<RendererDrawDesc>*);
    bool ReuseShadowAtlas(const Scene&,const ShadowData&,const std::vector<RendererDrawDesc>*);
    void CommitShadowAtlas(const Scene&);
    bool ApplySettings();
    void SwapResources(Impl&);
    RHI::ShaderDesc ShaderDescription(ShaderSlot,const RHI::ShaderDesc& stock) const;

    bool PrepareGeometry(const Scene&,RHI::IDevice*);
    static bool BuildCompactShadowGeometry(const std::vector<RendererVertex>&,const std::vector<uint32_t>&,
        std::vector<CompactShadowVertex>&,std::vector<uint32_t>&);
    bool PrepareShadowArena(const Scene&,RHI::IDevice*);
    void SelectLods(const Scene&,const Camera&,uint32_t width,uint32_t height,const std::vector<RendererDrawDesc>*);
    uint32_t EntityLod(uint32_t entity) const;
    static Math::float4 LodColor(uint32_t level);
    bool CreateMaterialResourceSets(const Scene&,RHI::ICommandList&,const std::vector<RendererDrawDesc>*,std::vector<RHI::ResourceSetHandle>&);
    bool RecordShadowPass(const Scene&,const Camera&,const ShadowData&,RHI::ICommandList&,
        const std::vector<RendererDrawDesc>*);
    bool RecordMainPass(const Scene&,const Camera&,RHI::ICommandList&,
        const std::vector<RendererDrawDesc>*,RHI::TextureHandle output);
    void DestroyMeshState(RHI::IDevice*,SceneMeshState&);
    void ReleaseGeometry(RHI::IDevice*);
    bool SyncTextures(const Scene&,RHI::IDevice*);
    void ReleaseTextures(RHI::IDevice*);
    RHI::TextureHandle ResolveTexture(const Image&) const;
    bool InitializeCanvas(RHI::Format);
    bool RecordCanvas(const Canvas&,RHI::ICommandList&,RHI::TextureHandle,bool overlay,bool graphManagedTarget = false);
    bool RenderCanvas(const Canvas&,Image*);
    bool PreparePostProcess(RHI::TextureHandle);
    bool RecordToneMap(RHI::ICommandList&,RHI::TextureHandle,float exposure,RHI::TextureHandle source = nullptr);
    void RecordProfilerFrame(Platform::ProfilerTimingSample,uint32_t entities);
    Canvas BuildProfilerOverlay(uint32_t width,uint32_t height,bool expanded) const;

    std::unique_ptr<RHI::IDevice> ownedDevice;
    RHI::IDevice* device=nullptr;
    const void* windowHandle=nullptr;
    RendererConfig config;
    std::unique_ptr<RendererConfig> pendingConfig;
    ShaderSources shaderSources,pendingShaderSources;
    mutable std::deque<ShaderSources> shaderViews;
    mutable const ShaderSources* shaderView=nullptr;
    bool shadersPending=false;
    bool framePending=false;
    bool m_environmentCapture=false,m_environmentCaptureEmitters=true;
    bool m_environmentCaptureVisibility=false;
    bool stockDrawInputs=false;
    RHI::ShaderHandle vertexShader=nullptr,fragmentShader=nullptr,shadowVertexShader=nullptr;
    RHI::ShaderHandle shadowFragmentShader=nullptr;
    RHI::ShaderHandle depthPrepassFragmentShader=nullptr;
    RHI::ShaderHandle noAreaFragmentShader=nullptr,noAreaDepthPrepassFragmentShader=nullptr;
    RHI::ShaderHandle canvasVertexShader=nullptr,canvasFragmentShader=nullptr;
    RHI::ShaderHandle toneVertexShader=nullptr,toneFragmentShader=nullptr;
    RHI::PipelineHandle pipeline=nullptr,shadowPipeline=nullptr;
    std::array<RHI::PipelineHandle,7> shadowMaterialPipelines={};
    std::array<RHI::PipelineHandle,8> compactShadowPipelines={};
    std::array<RHI::PipelineHandle,7> materialPipelines={};
    std::array<RHI::PipelineHandle,8> depthPrepassPipelines={};
    std::array<RHI::PipelineHandle,8> noAreaMaterialPipelines={},noAreaDepthPrepassPipelines={};
    bool frameNoAreaLights=false,noAreaReported=false;
    bool frameOpaqueDepthComplete=false;
    bool depthPrepassReported=false;
    std::vector<uint64_t> depthPassRevisions;
    std::vector<uint64_t> depthProtectedInputs;
    DepthGeometrySnapshot depthGeometrySnapshot;
    Math::float4x4 depthGeometryView,depthGeometryProjection;
    bool depthGeometryUnchanged=false;
    std::array<RHI::PipelineHandle,4> wireframePipelines={};
    std::vector<uint32_t> mainEntityOrder;
    RHI::PipelineHandle canvasPipeline=nullptr,tonePipeline=nullptr;
    Image canvasWhite;
    std::vector<CanvasImageSlot> canvasImages;
    std::vector<CanvasVertexSlot> canvasVertices,canvasPendingVertices;
    std::vector<uint32_t> canvasDrawImages;
    size_t canvasVertexCursor=0;
    RHI::Format meshColorFormat=RHI::Format::Unknown,toneColorFormat=RHI::Format::Unknown;
    bool meshCompositeAlpha=false;
    RHI::TextureHandle depthStencilTarget=nullptr,shadowDepthTarget=nullptr,hdrTarget=nullptr;
    std::array<RHI::TextureHandle,3> defaultMaterialTextures={};
    RHI::ResourceState depthStencilState=RHI::ResourceState::Undefined;
    RHI::ResourceState shadowDepthState=RHI::ResourceState::Undefined,hdrState=RHI::ResourceState::Undefined;
    RHI::BufferHandle lightingBuffer=nullptr,shadowMatrixBuffer=nullptr;
    RHI::BufferHandle instanceBuffer=nullptr,materialIndexBuffer=nullptr;
    bool lightingReady=false,shadowReady=false,instanceReady=false,materialIndexReady=false;
    bool materialIndexRecorded=false;
    std::weak_ptr<Scene> instanceScene,pendingInstanceScene;
    uint64_t instanceRevision=0,pendingInstanceRevision=0;
    uint32_t instanceCount=0,pendingInstanceCount=0;
    std::vector<Math::float4x4> instanceScratch;
    struct InstanceRange {uint32_t first,count;};
    std::vector<InstanceRange> instanceRanges;
    std::vector<uint8_t> lightingBytes,pendingLightingBytes,shadowBytes,pendingShadowBytes;
    std::vector<uint32_t> materialIndexBytes,materialIndexScratch;
    std::array<std::vector<uint32_t>,5> lightingIndexScratch;
    struct MaterialPageScratch {
        std::vector<RHI::TextureHandle> textures;
        std::vector<uint32_t> materials;
    };
    std::vector<MaterialPageScratch> materialPages;
    std::vector<RHI::ResourceSetHandle> mainSetScratch,shadowSetScratch;
    std::vector<RHI::ResourceBinding> mainBindingScratch,mainCombinedBindingScratch,shadowBindingScratch;
    std::vector<ShadowCaster> shadowCasterScratch;
    std::vector<uint32_t> shadowViewScratch;
    std::vector<double> shadowBatchDepthScratch;
    bool shadowBatchSortReported=false;
    ShadowAtlasCache shadowAtlasCache;
    std::array<float,4> shadowCascadeRadii={};
    float shadowCascadeNear=0,shadowCascadeFar=0,shadowCascadeAspect=0;
    float shadowCascadeSplitLambda=.65f;
    bool shadowCascadeOrthographic=false;
    uint32_t shadowCascadeCount=0;
    std::vector<CachedSet> mainSets,shadowSets;
    RendererStatistics recordingStatistics,lastStatistics;
    RHI::FenceHandle lastRenderCompletion;
    std::vector<SceneMeshState> m_meshes;
    // Only this Impl owns arena buffers; mesh ranges never own/alias their handles.
    RHI::BufferHandle shadowArenaVertices=nullptr,shadowArenaIndices=nullptr;
    uint64_t shadowArenaGeneration=0;
    uint32_t shadowArenaStablePreparations=0;
    bool shadowArenaDirty=true,shadowArenaAttempted=false,shadowArenaReported=false;
    std::weak_ptr<Scene> lodScene;
    std::vector<uint8_t> entityLods;
    std::vector<std::weak_ptr<const MeshData>> lodMeshSources;
    bool lodDebugActive=false,wireframeActive=false;
    std::vector<TextureSlot> m_textures;
    std::vector<uint32_t> m_freeTextureSlots;
    std::map<std::pair<const uint8_t*,ColorSpace>,uint32_t> m_indices;
    std::vector<SceneMaterialState> materialStates;
    std::unique_ptr<BasicEnvironmentResources> basicEnvironment;
    std::unique_ptr<BasicLightingResources> basicLighting;
    std::unique_ptr<OutputPresentation> outputPresentation;
    RenderLightingResources frameLighting;
    std::vector<std::unique_ptr<RenderExtension>> extensions;
    RHI::TextureHandle frameAmbientOcclusion=nullptr;
    struct VisibilityDraw
    {
        uint32_t entity=0,mesh=0,material=0,level=0;
        uint32_t instanceOffset=0,instanceCount=1;
    };
    std::unique_ptr<GpuVisibility> gpuVisibility;
    std::vector<VisibilityDraw> visibilityDraws;
    std::vector<uint64_t> visibilityKeys;
    std::weak_ptr<Scene> visibilityScene;
    bool visibilityActive=false;
    using CpuDrawKey=std::tuple<uint32_t,uint32_t,uint32_t,uintptr_t,uintptr_t,bool,bool,bool>;
    struct CpuDrawInput {CpuDrawKey key;uint32_t entity;};
    std::vector<CpuDrawInput> cpuDrawInputs;
    std::vector<VisibilityDraw> cpuDraws;
    std::vector<uint32_t> cpuBatchIndices,cpuInstanceEntities,cpuMeshCounts;
    std::vector<Math::float4x4> cpuInstanceScratch,cpuInstanceBytes;
    RHI::BufferHandle cpuInstanceBuffer=nullptr;
    bool cpuInstanceReady=false,cpuBatchesActive=false;
    uint32_t cpuCulledEntities=0;
    std::weak_ptr<Scene> historyScene;
    std::vector<Math::float4x4> previousModels,pendingModels;
    Math::float4x4 previousViewProjection=Math::float4x4::Identity();
    Math::float4x4 previousCameraView=Math::float4x4::Identity();
    Math::float4x4 previousCameraProjection=Math::float4x4::Identity();
    Math::float4x4 pendingCameraView=Math::float4x4::Identity();
    Math::float4x4 pendingCameraProjection=Math::float4x4::Identity();
    Math::float3 previousCameraPosition={};
    uint32_t frameIndex=0;
    bool frameHistoryValid=false;

    Platform::FrameProfiler m_profiler;
    std::array<float,120> m_history={};
    std::array<float,120> m_cpuHistory={},m_gpuHistory={};
    std::array<uint8_t,120> m_gpuHistoryValid={};
    float m_graphScaleMilliseconds=33.33f;
    uint32_t m_cursor=0,m_count=0,m_entities=0;
    RendererStatistics m_profilerStatistics;
    RHI::ResourceSetHandle toneSet=nullptr;
};
}

