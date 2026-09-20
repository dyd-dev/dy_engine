#pragma once
#include <array>
#include <chrono>
#include <deque>
#include <memory>
#include <string>
#include <map>
#include <vector>
#include "dyf/RendererConfig.h"
#include "dyf/Platform/ProfilerSampler.h"
#include "dyf/Material.h"
#include "dyf/RHI/IDevice.h"
#include "dyf/RHI/ResourceHandles.h"
#include "dyf/RHI/ResourceState.h"
#include "dyf/RHI/Query.h"
#include "dyf/RHI/Submission.h"
#include "dyf/RHI/Shader.h"
#include "dyf/RHI/Binding.h"
#include "dyf/RHI/Format.h"

namespace dyf::RHI { class IDevice; class ICommandList; struct PipelineLayoutDesc; }
namespace dyf
{
class Image;
struct Camera;
struct MeshData;
// 선택적으로 교체할 RHI 셰이더 입력이다. 생략한 단계에는 기본 셰이더를 사용한다.
struct RendererShaderDesc
{
    RHI::ShaderDesc meshVertex;
    RHI::ShaderDesc meshFragment;
    RHI::ShaderDesc shadowVertex;
    RHI::ShaderDesc canvasVertex;
    RHI::ShaderDesc canvasFragment;
    RHI::ShaderDesc toneMapVertex;
    RHI::ShaderDesc toneMapFragment;
    // 선택적 정점 셰이더 입력이다. 기본 바인딩과 기본 상수 뒤에 추가한다.
    std::vector<RHI::ResourceBindingLayout> vertexBindings;
    uint32_t additionalConstantBytes = 0;
};
// 직접 준비한 RHI 입력이다. Scene에는 GPU 자원을 넣지 않는다.
struct RendererDrawDesc
{
    // 생략하면 Scene의 Mesh를 사용한다. 형식: float3 위치·float3 법선·float2 UV·float4 탄젠트(48바이트).
    RHI::BufferHandle vertexBuffer = nullptr;
    std::vector<RHI::ResourceBinding> vertexResources;
    // 기본 draw 상수 192바이트 뒤에 전달하며 additionalConstantBytes와 크기를 맞춘다.
    std::vector<uint8_t> inlineConstants;
};
class Scene;
class Canvas;

// 쉬운 설정과 CPU 입력을 공개 RHI 명령으로 조합한다. GPU 자원은 이 객체가 직접 소유한다.
class Renderer
{
public:
    [[nodiscard]] static std::unique_ptr<Renderer> Create(const void* window);
    [[nodiscard]] static std::unique_ptr<Renderer> Create(const void* window, const RendererConfig&);
    [[nodiscard]] static std::unique_ptr<Renderer> Create(RHI::IDevice& device, const void* window);
    [[nodiscard]] static std::unique_ptr<Renderer> Create(RHI::IDevice& device, const void* window, const RendererConfig&);
    ~Renderer();
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    // false는 실패이며 원인은 stderr로 출력한다. 프레임을 확보하지 못한 경우는 true로 생략한다.
    // 생략한 프레임의 readback은 비운다. Mesh overload는 기본 광원, Scene overload는 등록된 광원만 사용한다.
    [[nodiscard]] bool Render(const MeshData&, Image* readback = nullptr);
    [[nodiscard]] bool Render(const MeshData&, const Camera&, Image* readback = nullptr);
    [[nodiscard]] bool Render(const MeshData&, const MaterialDesc&, const Camera&, Image* readback = nullptr);
    [[nodiscard]] bool Render(const Scene&, Image* readback = nullptr);
    [[nodiscard]] bool Render(const Scene&, const Camera&, Image* readback = nullptr);
    [[nodiscard]] bool Render(const Canvas&, Image* readback = nullptr);
    [[nodiscard]] bool Render(const Scene&, const Canvas& overlay, Image* readback = nullptr);
    [[nodiscard]] bool Render(const Scene&, const Camera&, const Canvas& overlay, Image* readback = nullptr);

    // Caller-owned RGBA/BGRA8 RenderTarget | ShaderResource texture. Submits rendering
    // and leaves the texture in ShaderResource. Does not acquire or present a frame.
    // Pass Undefined for a new texture, its actual state on later calls. The caller
    // retains the texture. Profiler HUD/Canvas overlays belong to the window render path.
    // Prefer SRGB targets for Gui::RegisterTexture: UNORM output already carries display gamma.
    [[nodiscard]] bool RenderToTexture(const Scene&, const Camera&, RHI::TextureHandle target,
        RHI::ResourceState before);

    // RHI를 직접 조합하는 확장만 사용하는 입력이다. 배열 순서는 Scene의 객체 ID 순서다.
    [[nodiscard]] bool Render(const Scene&, const Camera&, const std::vector<RendererDrawDesc>&,
        const Canvas* overlay = nullptr, Image* readback = nullptr);
    [[nodiscard]] bool Render(const Scene&, const std::vector<RendererDrawDesc>&,
        const Canvas* overlay = nullptr, Image* readback = nullptr);
    [[nodiscard]] RHI::IDevice& GetDevice() const { return *device; }

    bool SetClearColor(Math::float4);
    bool SetVSync(bool);
    // 조명 설정을 읽거나 교체한다. 변경은 다음 Render에서 적용한다.
    [[nodiscard]] const LightingConfig& GetLighting() const { return GetConfig().lighting; }
    bool SetLighting(const LightingConfig&);
    bool SetLightingEnabled(bool);
    bool SetShadowsEnabled(bool);
    bool SetHdrEnabled(bool);
    bool SetExposure(float);
    bool SetProfilerVisible(bool);
    bool SetReadbackEnabled(bool);
    [[nodiscard]] const RendererConfig& GetConfig() const { return pendingConfig ? *pendingConfig : config; }
    bool SetConfig(const RendererConfig&);
    // 선택적 셰이더 교체다. 입력 바이트와 진입점은 복사하며 다음 Render에서 적용한다.
    bool SetShaders(const RendererShaderDesc&);
    // 반환한 셰이더 바이트는 다음 설정 적용까지 유효한 읽기 뷰다.
    [[nodiscard]] RendererShaderDesc GetShaders() const;

private:
    // 한 호출에서만 사용하는 명령 구성 데이터다. 구현 클래스나 PImpl을 소유하지 않는다.
    struct ShadowData;
    struct SceneMaterialState
    {
        std::array<RHI::TextureHandle,kMaterialTextureCount> textures={};
        uint32_t textureFlags=0;
    };
    struct SceneMeshState
    {
        RHI::BufferHandle vertexBuffer=nullptr,indexBuffer=nullptr;
        uint32_t indexCount=0;
        std::weak_ptr<const MeshData> source;
        bool vertexReady=false,indexReady=false,prepared=false;
    };
    struct TextureSlot
    {
        Image source;
        RHI::TextureHandle texture=nullptr;
        RHI::ResourceState state=RHI::ResourceState::Undefined;
    };
    struct GpuSample {RHI::TimestampQueryHandle query;RHI::FenceHandle completion;uint32_t slot;};
    enum ShaderSlot : uint32_t {MeshVertex,MeshFragment,ShadowVertex,CanvasVertex,CanvasFragment,ToneVertex,ToneFragment,ShaderCount};
    struct ShaderSources
    {
        std::array<std::vector<uint8_t>,ShaderCount> bytes;
        std::array<std::string,ShaderCount> entries;
        std::vector<RHI::ResourceBindingLayout> vertexBindings;
        uint32_t additionalConstantBytes = 0;
    };

    Renderer(RHI::IDevice&,const void* window,const RendererConfig&);
    static RendererShaderDesc DefaultShaders(bool shadows = false, bool bindless = false);
    bool CaptureFrame(Image&);
    bool Initialize();
    bool InitializeMesh(RHI::TextureHandle output = nullptr,bool compositeAlpha = false);
    bool UsesBindlessMaterials() const;
    RHI::PipelineLayoutDesc MeshLayout(bool bindless, std::vector<RHI::ResourceBindingLayout>& bindings) const;
    void Shutdown();
    bool RenderScene(const Scene&,const Camera*,const Canvas*,Image*,const std::vector<RendererDrawDesc>* = nullptr,
        RHI::TextureHandle output = nullptr, RHI::ResourceState before = RHI::ResourceState::Present);
    bool BuildPipelineStates(RHI::IDevice*,RHI::Format colorFormat,bool compositeAlpha);
    bool CreateDefaultMaterialTextures(RHI::IDevice*);
    bool EnsureDepthStencilTarget(RHI::IDevice*,RHI::TextureHandle output = nullptr);
    bool EnsureShadowDepthTarget(RHI::IDevice*,uint32_t columns,uint32_t rows,uint32_t resolution);
    void UpdateMaterialStates(const Scene&);
    bool UpdateLightingBuffer(const Scene&,const Camera&,RHI::IDevice*,RHI::ICommandList&);
    bool UpdateShadowBuffer(RHI::IDevice*,RHI::ICommandList&,const ShadowData&);
    void BuildShadows(ShadowData&,const Scene&,const Camera&);
    bool ApplySettings();
    void SwapResources(Renderer&);
    RHI::ShaderDesc ShaderDescription(ShaderSlot,const RHI::ShaderDesc& stock) const;

    // 메시·텍스처 업로드와 draw에 필요한 RHI 호출을 cpp 파일별로 나눈 멤버 함수다.
    bool PrepareGeometry(const Scene&,RHI::IDevice*);
    bool CreateMaterialResourceSets(const Scene&,RHI::ICommandList&,const std::vector<RendererDrawDesc>*,std::vector<RHI::ResourceSetHandle>&);
    bool RecordShadowPass(const Scene&,const Camera&,const ShadowData&,RHI::ICommandList&,
        const std::vector<RendererDrawDesc>*,RHI::TimestampQueryHandle);
    bool RecordMainPass(const Scene&,const Camera&,RHI::ICommandList&,
        const std::vector<RendererDrawDesc>*,RHI::TimestampQueryHandle,RHI::TextureHandle output);
    void DestroyMeshState(RHI::IDevice*,SceneMeshState&);
    void ReleaseGeometry(RHI::IDevice*);
    bool SyncTextures(const Scene&,RHI::IDevice*);
    void ReleaseTextures(RHI::IDevice*);
    RHI::TextureHandle ResolveTexture(const Image&) const;
    bool InitializeCanvas(RHI::Format);
    bool RecordCanvas(const Canvas&,RHI::ICommandList&,RHI::TextureHandle,bool overlay,bool graphManagedTarget = false);
    bool RenderCanvas(const Canvas&,Image*);
    bool PreparePostProcess(RHI::TextureHandle);
    bool RecordToneMap(RHI::ICommandList&,RHI::TextureHandle,float exposure);
    RHI::TimestampQueryHandle BeginGpuSample(uint32_t slot);
    void SubmittedGpuSample(RHI::TimestampQueryHandle,RHI::FenceHandle);
    void RecordProfilerFrame(double cpuMilliseconds,uint32_t entities);
    Canvas BuildProfilerOverlay(uint32_t width,uint32_t height,bool expanded) const;

    std::unique_ptr<RHI::IDevice> ownedDevice;
    RHI::IDevice* device=nullptr;
    const void* windowHandle=nullptr;
    RendererConfig config;
    std::unique_ptr<RendererConfig> pendingConfig;
    // GPU 셰이더 핸들과 달리 이것은 선택적 사용자 입력의 CPU 복사본이다.
    ShaderSources shaderSources,pendingShaderSources;
    bool shadersPending=false;
    RHI::ShaderHandle vertexShader=nullptr,fragmentShader=nullptr,shadowVertexShader=nullptr;
    RHI::ShaderHandle canvasVertexShader=nullptr,canvasFragmentShader=nullptr;
    RHI::ShaderHandle toneVertexShader=nullptr,toneFragmentShader=nullptr;
    RHI::PipelineHandle pipeline=nullptr,shadowPipeline=nullptr;
    RHI::PipelineHandle canvasPipeline=nullptr,tonePipeline=nullptr;
    RHI::Format meshColorFormat=RHI::Format::Unknown,toneColorFormat=RHI::Format::Unknown;
    bool meshCompositeAlpha=false;
    RHI::TextureHandle depthStencilTarget=nullptr,shadowDepthTarget=nullptr,hdrTarget=nullptr;
    std::array<RHI::TextureHandle,3> defaultMaterialTextures={};
    RHI::ResourceState depthStencilState=RHI::ResourceState::Undefined;
    RHI::ResourceState shadowDepthState=RHI::ResourceState::Undefined,hdrState=RHI::ResourceState::Undefined;
    RHI::BufferHandle lightingBuffer=nullptr,shadowMatrixBuffer=nullptr;
    std::vector<SceneMeshState> m_meshes;
    std::vector<TextureSlot> m_textures;
    std::map<std::pair<const uint8_t*,ColorSpace>,uint32_t> m_indices;
    std::vector<SceneMaterialState> materialStates;

    // 시간과 자원 수치는 같은 집계 주기로 보관한다. 표시는 Canvas 명령으로 작성한다.
    std::deque<GpuSample> m_pending;
    std::array<double,2> m_gpuMilliseconds={-1,-1};
    std::chrono::steady_clock::time_point m_previous={};
    std::array<float,120> m_history={};
    std::array<float,120> m_cpuHistory={},m_gpuHistory={};
    std::array<uint8_t,120> m_gpuHistoryValid={};
    float m_graphScaleMilliseconds=33.33f;
    uint32_t m_cursor=0,m_count=0,m_entities=0;
    double m_cpuMilliseconds=0;
    Platform::ProfilerSampler m_profilerSampler;
    Platform::ProfilerTimingSnapshot m_profilerSnapshot;
    RHI::ResourceAllocationCounters m_profilerResourceSnapshot;
};
}
