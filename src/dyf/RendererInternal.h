#pragma once
#include "../Platform/Profiler/CollectorInternal.h"

#include "dyf/Renderer.h"
#include "ShaderLayout.h"
#include "dyf/RenderExtension.h"
#include "dyf/RHI/IDevice.h"
#include "Platform/Profiler/FrameProfiler.h"
#include <deque>
#include <map>
#include <mutex>
#include <string>

namespace dyf
{
class BasicEnvironmentResources;
class BasicLightingResources;
class GpuVisibility;
class OutputPresentation;
struct RenderFrame;

struct Renderer::Impl
{
    // Facade 자신을 빌리는 역참조다. 확장 모듈과 라이브 등록에 공개 Renderer를 전달하되 서로 unique_ptr로 소유하지 않는다.
    Renderer& owner;
    ~Impl();

    // 입력 조합별 편의 진입점이다. Scene은 RenderScene, Canvas는 RenderCanvas로 모이며 Mesh는 임시 Scene을 만든다.
    [[nodiscard]] bool Render(const MeshData&, Image* readback = nullptr);
    [[nodiscard]] bool Render(const MeshData&, const Camera&, Image* readback = nullptr);
    [[nodiscard]] bool Render(const MeshData&, const MaterialDesc&, const Camera&, Image* readback = nullptr);
    [[nodiscard]] bool Render(const Scene&, Image* readback = nullptr);
    [[nodiscard]] bool Render(const Scene&, const Camera&, Image* readback = nullptr);
    [[nodiscard]] bool Render(const Canvas&, Image* readback = nullptr);
    [[nodiscard]] bool Render(const Scene&, const Canvas& overlay, Image* readback = nullptr);
    [[nodiscard]] bool Render(const Scene&, const Camera&, const Canvas& overlay, Image* readback = nullptr);

    // 외부 소유 텍스처 렌더링과 정적 환경 캡처도 같은 캐시를 활용하지만 창의 획득·표시 경로와는 구분한다.
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
    // lastRenderCompletion은 성공한 draw 제출의 표식이다. ready/prepared 같은 CPU 상태가 GPU 완료 펜스를 대신하지 않는다.
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
    // 최신 요청을 우선 조회한다. config만 실제 적용값이며 pendingConfig가 있으면 요청값과 적용값이 다를 수 있다.
    [[nodiscard]] const RendererConfig& GetConfig() const { return pendingConfig ? *pendingConfig : config; }
    bool SetConfig(const RendererConfig&);
    bool SetShaders(const RendererShaderDesc&);
    [[nodiscard]] RendererShaderDesc GetShaders() const;

    // 라이브 source를 등록하고 프레임 경계에서 제어 요청을 읽어 설정 적용 결과를 게시한다. liveRequests는 요청 ID이며 성공한 적용 프레임에 완료 처리한다.
    void InitializeLiveProfiling() noexcept;
    bool PrepareLiveFrame(Platform::LiveFrameKey) noexcept;
    void FinishLiveSettings(bool,Platform::LiveFrameKey) noexcept;
    void PublishLiveGeometryAvailability(bool customDraws) noexcept;
    Platform::LiveId liveId=0;
    std::array<Platform::ControlRegistration,16> liveControls;
    std::array<Platform::LiveId,16> liveRequests{};
    std::array<Platform::ControlRegistration,5> liveCatalog;
    bool liveCustomDraws=false;
    // 같은 프레임에서 설정을 중복 적용하지 않기 위한 프레임 번호다. liveAppliedConfig는 라이브 revision 변화를 비교할 마지막 적용 설정이다.
    Platform::LiveId livePreparedFrame=0;
    RendererConfig liveAppliedConfig;
    bool SupportsBindlessMaterials() const;

    // 프레임별 광원 행렬과 아틀라스 배치를 CPU에서 계산한 결과다. viewCount는 실제 그림자 뷰 수이며 GPU 완료 건수가 아니다.
    struct ShadowData
    {
        RendererShadowConstants constants={};
        uint32_t viewCount=0,columns=1,rows=1,resolution=1;
        float directionalDistance=0;
    };
    // 재질 이미지의 해석 결과를 텍스처 핸들과 비트 플래그로 캐시한다. 빈 입력은 기본 대체 텍스처로 해석한다.
    struct SceneMaterialState
    {
        std::array<RHI::TextureHandle,ToIndex(MaterialTextureKind::Count)> textures={};
        uint32_t textureFlags=0;
    };
    // 메시 원본 하나에 대응하는 정점·원본 인덱스·LOD 인덱스 버퍼와 경계다. ready는 해당 버퍼 업로드의 제출 성공, prepared는 필요한 버퍼 모두의 제출 성공 또는 빈 메시 처리를 뜻한다. 어느 쪽도 GPU 완료 판정은 아니다.
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
        RHI::BufferHandle vertexBuffer=nullptr,indexBuffer=nullptr;
        uint32_t indexCount=0;
        std::vector<IndexLod> lods;
        Math::Bounds3 bounds;
        // 원본을 오래 살려 두지 않고 포인터 정체성과 생존을 확인한다. 다른 MeshData로 바뀌면 이전 GPU 버퍼를 폐기하고 다시 준비한다.
        std::weak_ptr<const MeshData> source;
        bool vertexReady=false,indexReady=false,prepared=false,lodsGenerated=false;
    };
    // 여러 그림자 뷰가 공유하는 월드 경계와 수치 오차 크기다. 광원별 반복 계산을 줄이면서 경계 바깥 판정을 보수적으로 한다.
    struct ShadowBounds
    {
        Math::Bounds3 bounds;
        std::array<double,3> magnitude={};
    };
    // 파이프라인과 전체 바인딩을 키로 자원 세트를 재사용한다. 해시는 후보 검색용이고 unusedFrames는 캐시에서 제거할 시기를 정한다.
    struct CachedSet { RHI::ResourceSetHandle set=nullptr; uint32_t unusedFrames=0; size_t keyHash=0; };
    // 제출에 성공한 그림자 아틀라스의 재사용 기준과 이번 프레임 후보를 나눈다. Scene·Mesh 생존, 상수 바이트와 타깃 일치를 검사하며 GPU 완료 확인을 이 캐시로 대신하지 않는다.
    struct ShadowAtlasCache
    {
        bool valid=false,pending=false,reused=false;
        RHI::TextureHandle target=nullptr;
        std::weak_ptr<Scene> scene;
        std::vector<uint8_t> bytes,pendingBytes;
        std::vector<std::weak_ptr<const MeshData>> meshes,pendingMeshes;
    };
    // Image의 공유 원본을 보관하며 텍스처와 현재 RHI 상태를 함께 추적한다. 같은 픽셀 원본도 색 공간이 다르면 다른 해석이다.
    struct TextureSlot
    {
        Image source;
        RHI::TextureHandle texture=nullptr;
        RHI::ResourceState state=RHI::ResourceState::Undefined;
    };
    // Canvas 이미지의 텍스처·자원 세트와 프레임 사용 이력을 묶는다. CanvasVertexSlot은 정점 버퍼의 용량 재사용을 위한 별도 저장소다.
    struct CanvasImageSlot
    {
        Image source;
        RHI::TextureHandle texture=nullptr;
        RHI::ResourceSetHandle set=nullptr;
        uint32_t unusedFrames=0;
        bool used=false;
    };
    struct CanvasVertexSlot { RHI::BufferHandle buffer=nullptr; bool ready=false; };
    // Canvas 자원 변경을 draw 제출 성공 뒤 Commit으로 확정한다. 확정하지 못하고 소멸하면 이번에 새로 만든 정점 버퍼와 추가한 이미지 자원만 해제하고 기존 캐시는 남긴다.
    struct CanvasFrame
    {
        Impl& owner;
        size_t imageCount;
        bool committed=false;
        explicit CanvasFrame(Impl&);
        ~CanvasFrame();
        void Commit();
    };
    // 창 프레임을 확보한 뒤 조기 반환해도 Present를 시도하도록 하는 범위 보호 객체다. 이 보호는 GPU 완료 대기와 다른 책임이다.
    struct FrameEnd
    {
        RHI::IDevice& device;
        bool& pending;
        ~FrameEnd();
    };
    enum ShaderSlot : uint32_t {MeshVertex,MeshFragment,ShadowVertex,CanvasVertex,CanvasFragment,ToneVertex,ToneFragment,ShaderCount};
    // 외부 ShaderDesc의 일시적인 포인터를 자체 vector/string으로 소유한다. 바인딩과 추가 상수 크기도 함께 비교하여 같은 요청의 불필요한 재생성을 피한다.
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
    RHI::PipelineHandle MaterialPipeline(const MaterialDesc&,bool mirrored=false) const;
    bool CaptureFrame(Image&);
    bool Initialize();
    bool InitializeMesh(RHI::TextureHandle output = nullptr,bool compositeAlpha = false);
    bool UsesBindlessMaterials() const;
    bool UsesStockGeometry() const;
    bool UsesAdvancedRendering() const;
    bool NeedsSurfacePass() const;
    // 확장 모듈의 공급/요구 자원 계약을 수집한다.
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
    // 카메라/모션 이력, 가시성, 조명, 효과 준비를 먼저 수행하고 성공한 draw 제출 뒤 CommitFrameResources로 이번 프레임 이력을 확정한다.
    bool PrepareFrameResources(RenderFrame&,RHI::ICommandList&);
    void CommitFrameResources(const RenderFrame&);
    void InvalidateFrameHistory(bool release = false);
    bool UsesInstanceStream() const;
    bool UpdateInstanceBuffer(const Scene&,RHI::ICommandList&);
    // 인스턴스 revision과 조명·그림자 바이트의 재사용 기준을 업로드 제출 성공 후에만 바꾼다.
    void CommitFrameData();
    RHI::ResourceSetHandle CachedResourceSet(RHI::PipelineHandle,const std::vector<RHI::ResourceBinding>&,std::vector<CachedSet>&);
    void PruneResourceSets(std::vector<CachedSet>&);
    RHI::PipelineLayoutDesc MeshLayout(bool bindless, std::vector<RHI::ResourceBindingLayout>& bindings) const;
    // 소유한 RHI 핸들과 보조 객체를 정리하는 한곳의 해제 경계다. 장치 포인터가 유효한 동안 실행된다.
    void Shutdown();
    // 준비→업로드 제출→그래프 구성/기록→draw 제출→readback/Present를 조율한다. 패스 기록 함수 자체는 최종 제출이나 GPU 완료를 맡지 않는다.
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

    // 지오메트리·재질·그림자/메인·Canvas·후처리로 나뉜 구현 파일의 연결 선언이다. 실제 호출 순서는 RenderScene에서 확인한다.
    bool PrepareGeometry(const Scene&,RHI::IDevice*);
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

    // Create가 장치를 만든 경우에만 ownedDevice가 소유한다. device는 소유/대여 두 경우 모두 실제 사용하는 장치를 가리키며 먼저 선언된 ownedDevice는 멤버 소멸 순서상 마지막 쪽에 정리된다.
    std::unique_ptr<RHI::IDevice> ownedDevice;
    RHI::IDevice* device=nullptr;
    const void* windowHandle=nullptr;
    // 현재 적용 상태와 다음 적용 후보를 분리한다. shaderViews는 GetShaders에서 반환한 포인터가 설정 적용 전까지 유지되도록 복사본을 보관한다.
    RendererConfig config;
    std::unique_ptr<RendererConfig> pendingConfig;
    ShaderSources shaderSources,pendingShaderSources;
    mutable std::deque<ShaderSources> shaderViews;
    mutable const ShaderSources* shaderView=nullptr;
    bool shadersPending=false;
    // BeginFrame 성공 뒤 Present로 닫지 못한 창 프레임이 있는지 나타낸다. 제출 완료나 GPU 실행 중 여부를 나타내는 플래그는 아니다.
    bool framePending=false;
    bool m_environmentCapture=false,m_environmentCaptureEmitters=true;
    bool m_environmentCaptureVisibility=false;
    // 해당 프레임의 사용자 draw 입력까지 포함해 표준 지오메트리 최적화가 가능한지 기록한다.
    bool stockDrawInputs=false;
    // 현재 설정으로 만든 셰이더/파이프라인 캐시다. 재질의 양면·블렌드·좌우 반전 조합은 별도의 파이프라인 배열로 보관한다.
    RHI::ShaderHandle vertexShader=nullptr,fragmentShader=nullptr,shadowVertexShader=nullptr;
    RHI::ShaderHandle shadowFragmentShader=nullptr;
    RHI::ShaderHandle canvasVertexShader=nullptr,canvasFragmentShader=nullptr;
    RHI::ShaderHandle toneVertexShader=nullptr,toneFragmentShader=nullptr;
    RHI::PipelineHandle pipeline=nullptr,shadowPipeline=nullptr;
    std::array<RHI::PipelineHandle,7> shadowMaterialPipelines={};
    std::array<RHI::PipelineHandle,7> materialPipelines={};
    std::array<RHI::PipelineHandle,4> wireframePipelines={};
    std::vector<uint32_t> mainEntityOrder;
    RHI::PipelineHandle canvasPipeline=nullptr,tonePipeline=nullptr;
    // Canvas의 기본 흰 이미지·이미지 슬롯·정점 슬롯과 이번 프레임 준비 목록이다. pending 슬롯은 성공한 프레임과 실패한 준비를 구분한다.
    Image canvasWhite;
    std::vector<CanvasImageSlot> canvasImages;
    std::vector<CanvasVertexSlot> canvasVertices,canvasPendingVertices;
    std::vector<uint32_t> canvasDrawImages;
    size_t canvasVertexCursor=0;
    RHI::Format meshColorFormat=RHI::Format::Unknown,toneColorFormat=RHI::Format::Unknown;
    bool meshCompositeAlpha=false;
    // 메인 깊이·그림자 아틀라스·HDR 중간 출력을 소유한다. 아래 ResourceState 값은 다음 그래프 import에 필요한 마지막 확정 상태다.
    RHI::TextureHandle depthStencilTarget=nullptr,shadowDepthTarget=nullptr,hdrTarget=nullptr;
    std::array<RHI::TextureHandle,3> defaultMaterialTextures={};
    RHI::ResourceState depthStencilState=RHI::ResourceState::Undefined;
    RHI::ResourceState shadowDepthState=RHI::ResourceState::Undefined,hdrState=RHI::ResourceState::Undefined;
    // 프레임 상수·인스턴스·재질 인덱스 버퍼다. ready와 pending 바이트/Scene/revision을 나누어 실패한 기록을 다음 프레임 재사용 기준으로 삼지 않는다.
    RHI::BufferHandle lightingBuffer=nullptr,shadowMatrixBuffer=nullptr;
    RHI::BufferHandle instanceBuffer=nullptr,materialIndexBuffer=nullptr;
    bool lightingReady=false,shadowReady=false,instanceReady=false,materialIndexReady=false;
    bool materialIndexRecorded=false;
    std::weak_ptr<Scene> instanceScene,pendingInstanceScene;
    uint64_t instanceRevision=0,pendingInstanceRevision=0;
    uint32_t instanceCount=0,pendingInstanceCount=0;
    // 업로드할 CPU 행렬과 변경 구간 임시 저장소다. 인스턴스 하나가 행렬 두 개의 128바이트 간격을 차지한다.
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
    // bindless 재질을 텍스처 페이지로 나누고 자원 세트에 연결하기 위한 재사용 임시 배열이다.
    std::vector<MaterialPageScratch> materialPages;
    std::vector<RHI::ResourceSetHandle> mainSetScratch,shadowSetScratch;
    std::vector<RHI::ResourceBinding> mainBindingScratch,mainCombinedBindingScratch,shadowBindingScratch;
    std::vector<ShadowBounds> shadowBoundsScratch;
    ShadowAtlasCache shadowAtlasCache;
    std::array<float,4> shadowCascadeRadii={};
    float shadowCascadeNear=0,shadowCascadeFar=0,shadowCascadeAspect=0;
    float shadowCascadeSplitLambda=.65f;
    bool shadowCascadeOrthographic=false;
    uint32_t shadowCascadeCount=0;
    std::vector<CachedSet> mainSets,shadowSets;
    // 현재 명령 기록 중의 통계와 최근 제출 성공 통계를 나눈다. lastRenderCompletion은 설정 자원 교환에도 유지한다.
    RendererStatistics recordingStatistics,lastStatistics;
    RHI::FenceHandle lastRenderCompletion;
    // Scene의 CPU 원본과 대응하는 기하·LOD·텍스처 캐시다. 설정 변경 시 같은 장치·고정 입력 형식의 업로드 자원을 그대로 유지한다.
    std::vector<SceneMeshState> m_meshes;
    std::weak_ptr<Scene> lodScene;
    std::vector<uint8_t> entityLods;
    std::vector<std::weak_ptr<const MeshData>> lodMeshSources;
    bool lodDebugActive=false,wireframeActive=false;
    std::vector<TextureSlot> m_textures;
    std::map<std::pair<const uint8_t*,ColorSpace>,uint32_t> m_indices;
    std::vector<SceneMaterialState> materialStates;
    // 확장 모듈이 환경/지역광을 공급하지 않을 때 사용할 기본 자원이다. extensions는 등록 순서를 유지하며 Impl이 단독 소유한다.
    std::unique_ptr<BasicEnvironmentResources> basicEnvironment;
    std::unique_ptr<BasicLightingResources> basicLighting;
    std::unique_ptr<OutputPresentation> outputPresentation;
    RenderLightingResources frameLighting;
    std::vector<std::unique_ptr<RenderExtension>> extensions;
    RHI::TextureHandle frameAmbientOcclusion=nullptr;
    // GPU 간접 draw 배치를 CPU에서 다시 연결하는 대표 객체·메시·재질·LOD 정보다. 실제 가시 인스턴스 수는 이 배열 길이와 다르다.
    struct VisibilityDraw
    {
        uint32_t entity=0,mesh=0,material=0,level=0;
    };
    std::unique_ptr<GpuVisibility> gpuVisibility;
    std::vector<VisibilityDraw> visibilityDraws;
    std::vector<uint64_t> visibilityKeys;
    std::weak_ptr<Scene> visibilityScene;
    bool visibilityActive=false;
    // 재투영 이력의 장면 정체성과 이전/대기 행렬을 보관한다. draw 제출 성공 시 pendingModels를 previousModels로 옮긴다.
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

    // 시간 측정과 HUD 표본을 보관한다. GPU 시간 배열과 유효성 배열을 분리하므로 미완료·사용 불가 값을 실제 0ms 측정으로 취급하지 않는다.
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

