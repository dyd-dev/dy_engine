#pragma once
#include <array>
#include <memory>
#include <string>
#include <vector>
#include "dyf/RendererConfig.h"
#include "dyf/Platform/ProfilerSampler.h"
#include "dyf/Material.h"
#include "dyf/RHI/IDevice.h"
#include "dyf/RHI/ResourceHandles.h"
#include "dyf/RHI/ResourceState.h"
#include "dyf/RHI/Submission.h"
#include "dyf/RHI/Shader.h"
#include "dyf/RHI/Binding.h"
#include "dyf/RHI/Format.h"

namespace dyf::RHI { class IDevice; }
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
    // 표준 48바이트 정점 형식의 변형 결과는 고급 렌더 패스에서도 공유한다.
    bool stockGeometry = false;
    RHI::BufferHandle previousVertexBuffer = nullptr;
    Math::Bounds3 localBounds;
    bool hasLocalBounds = false;
    bool deformationHistoryValid = true;
    uint64_t geometryRevision = 0;
};
class Scene;
class Canvas;
class RenderExtension;

// 쉬운 설정과 CPU 입력을 공개 RHI 명령으로 조합한다. GPU 자원은 이 객체가 직접 소유한다.
struct RendererStatistics
{
    uint32_t mainDrawCount=0,mainInstanceCount=0,culledEntityCount=0;
    // Submitted indexed-triangle work. Shadow counts include every atlas view.
    uint64_t mainTriangleCount=0,shadowTriangleCount=0;
    uint32_t shadowDrawCount=0,shadowInstanceCount=0;
    // Visible main-pass objects, grouped by selected index level (0 = original).
    std::array<uint32_t,4> lodInstanceCounts={};
    uint64_t mainOriginalTriangleCount=0;
    // GPU가 정하는 가시 개수는 CPU 제출 개수와 구분한다.
    bool gpuVisibility=false;
    uint32_t indirectBatchCount=0;
    // 요청한 토글과 구분하여 해당 프레임에서 사용하는 경로와 실제 자원 크기를 보여준다.
    bool gpuOcclusion=false,clusteredLighting=false,surfacePrepass=false;
    uint32_t renderWidth=0,renderHeight=0,outputWidth=0,outputHeight=0;
    uint32_t shadowViewCount=0,shadowViewResolution=0;
    float directionalShadowDistance=0;
    bool reusedShadowAtlas=false;
};
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

    // Startup-only static radiance capture: six opaque/masked raster views,
    // decoded to a linear HDR lat-long image (4*faceSize by 2*faceSize).
    // Uses existing geometry/texture caches and synchronous readback; never call
    // per frame or concurrently with Render. Excludes glass, IBL and screen GI.
    // includeEmitters=false captures reflected direct light for diffuse probes;
    // true also retains visible emission for specular probes. No scene edits.
    // Restores renderer settings; failure leaves result unchanged.
    // visibility를 지정하면 같은 원본 깊이에서 월드 거리(R)·거리 제곱(G)의 파노라마도 얻는다.
    [[nodiscard]] bool CaptureEnvironment(const Scene&,Math::float3 position,uint32_t faceSize,
        float nearPlane,float farPlane,HdrImage& result,bool includeEmitters=true,HdrImage* visibility=nullptr);

    // RHI를 직접 조합하는 확장만 사용하는 입력이다. 배열 순서는 Scene의 객체 ID 순서다.
    [[nodiscard]] bool Render(const Scene&, const Camera&, const std::vector<RendererDrawDesc>&,
        const Canvas* overlay = nullptr, Image* readback = nullptr);
    [[nodiscard]] bool Render(const Scene&, const std::vector<RendererDrawDesc>&,
        const Canvas* overlay = nullptr, Image* readback = nullptr);
    [[nodiscard]] RHI::IDevice& GetDevice() const;

    // Published window Scene timings, approximately every 200 ms. CPU wall-clock
    // stages and completed asynchronous GPU frame/pass samples are separate.
    // Reading the snapshot never waits. RenderToTexture does not add timing samples.
    [[nodiscard]] const Platform::ProfilerTimingSnapshot& GetProfilerTimingSnapshot() const;

    // Most recent Scene call: submitted main/shadow counts, or zero before submission.
    // Main excludes the separate Surface/transparent passes; Canvas/HUD are excluded.
    [[nodiscard]] const RendererStatistics& GetLastRenderStatistics() const;
    // Published at draw submission, even when later readback/Present fails. Survives setting swaps.
    [[nodiscard]] RHI::FenceHandle GetLastRenderCompletion() const;
    bool SetClearColor(Math::float4);
    bool SetVSync(bool);
    // 조명 설정을 읽거나 교체한다. 변경은 다음 Render에서 적용한다.
    [[nodiscard]] const LightingConfig& GetLighting() const { return GetConfig().lighting; }
    // 시점 이동이나 애니메이션 탐색 뒤 이전 프레임의 재투영 이력을 버린다.
    void InvalidateHistory();
    bool SetLighting(const LightingConfig&);
    bool SetLightingEnabled(bool);
    bool SetShadowsEnabled(bool);
    bool SetHdrEnabled(bool);
    bool SetExposure(float);
    bool SetProfilerVisible(bool);
    bool SetReadbackEnabled(bool);
    // 등록한 확장은 렌더러가 소유하며 매 프레임 같은 순서로 패스를 추가한다.
    bool AddExtension(std::unique_ptr<RenderExtension>);
    [[nodiscard]] const RendererConfig& GetConfig() const;
    bool SetConfig(const RendererConfig&);
    // 선택적 셰이더 교체다. 입력 바이트와 진입점은 복사하며 다음 Render에서 적용한다.
    bool SetShaders(const RendererShaderDesc&);
    // 반환한 셰이더 바이트와 진입점은 다음 설정 적용까지 유효한 읽기 뷰다.
    [[nodiscard]] RendererShaderDesc GetShaders() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
    Renderer(RHI::IDevice&,const void* window,const RendererConfig&);
};
}
