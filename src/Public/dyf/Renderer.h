#pragma once

#include <cstdint>
#include <vector>
#include "dyf/Math/Math.h"
#include "dyf/Image.h"
#include <array>
#include <memory>
#include <string>
#include "dyf/Platform/Profiler.h"
#include "dyf/Material.h"
#include "dyf/RHI/IDevice.h"
#include "dyf/RHI/Binding.h"
#include "dyf/RHI/Submission.h"
#include "dyf/RHI/Shader.h"
#include "dyf/RHI/Format.h"

namespace dyf::RHI { struct ProfilerAccess; }

namespace dyf
{
	// 그림자 품질을 선택한다. 해상도와 필터 등 구현 값은 Renderer가 정한다.
	enum class ShadowQuality : uint8_t { Low, Medium, High };
	enum class SurfaceDebugView : uint8_t { None, BaseColor, Normal, Roughness, Metallic, Depth, Motion };

	// 월드 공간의 한 지점에서 캡처한 정적 간접광·반사 정보다.
	struct EnvironmentProbeDesc
	{
		HdrImage radiance;
		HdrImage diffuseRadiance;
		Math::float3 position = {0, 0, 0};
		Math::float3 boxMin = {-1, -1, -1};
		Math::float3 boxMax = {1, 1, 1};
		float fadeDistance = 1;
		float intensity = 1;
		// 같은 위치의 방향별 첫 교차 거리와 거리 제곱이다. 비어 있으면 차폐를 알 수 없다.
		HdrImage visibility;
		// 0은 원본 해상도를 보존한다. 배열 저장 크기는 프로브 요청 중 가장 큰 크기를 사용한다.
		uint32_t maxReflectionResolution = 0;
	};
	// 표면에 더할 환경색과 세기다. 생략하면 기본 환경을 사용한다.
	struct EnvironmentDesc
	{
		Math::float3 diffuseColor = Math::float3(1.0f, 1.0f, 1.0f);
		float diffuseIntensity = 1.0f;
		Math::float3 specularColor = Math::float3(1.0f, 1.0f, 1.0f);
		float specularIntensity = 1.0f;
		// Equirectangular linear HDR sky. Z is up; rotation is around world Z.
		HdrImage radiance;
		// Optional diffuse-only panorama. Room captures can exclude directly visible
		// emitters here while retaining their shapes in specular radiance.
		HdrImage diffuseRadiance;
		float intensity = 1.0f;
		float rotationRadians = 0.0f;
		bool diffuse = true;
		bool specular = true;
		bool background = true;
		// A captured room probe uses a world-space box to correct specular parallax.
		// Diffuse SH remains a single spatial sample, not a lightmap or probe volume.
		bool localProbe = false;
		Math::float3 probePosition = Math::float3(0.0f, 0.0f, 0.0f);
		Math::float3 probeBoxMin = Math::float3(-1.0f, -1.0f, -1.0f);
		Math::float3 probeBoxMax = Math::float3(1.0f, 1.0f, 1.0f);
		// 영향 상자의 가장자리에서 전역 환경으로 전환하고 겹친 구역은 위치별로 혼합한다.
		std::vector<EnvironmentProbeDesc> probes;
	};

	// 원하는 조명 결과를 설정한다. 광원 종류와 표면 반응은 Light와 Material에서 정한다.
	struct LightingConfig
	{
		// 끄면 광원·환경광·그림자를 적용하지 않고 재질의 기본색과 발광을 표시한다.
		bool enabled = true;
		// Direct lights can be removed while indirect/environment lighting remains.
		bool direct = true;
		// ClusteredLighting 모듈의 공간 분류다. 끄거나 모듈을 생략하면 전체 광원을 순회한다.
		bool clusteredLighting = true;
		// 방향광·점광원·스폿 광원의 그림자를 켠다. 면적광 그림자는 아직 지원하지 않는다.
		bool shadows = false;
		ShadowQuality shadowQuality = ShadowQuality::Medium;
		// 방향광 그림자의 최대 시야 거리다. 0이면 기존 카메라·장면 범위로 맞춘다.
		float shadowDistance = 0;
		// 캐스케이드 경계를 균등 분할(0)과 로그 분할(1) 사이에서 정한다.
		float cascadeSplitLambda = 0.65f;
		// 우선순위가 높은 그림자 광원을 이 개수까지 선택한다. 0도 직접 조명은 유지한다.
		uint32_t maxDirectionalShadowLights = 4;
		uint32_t maxPointShadowLights = 16;
		uint32_t maxSpotShadowLights = 16;
		Math::float3 ambientColor = Math::float3(1.0f, 1.0f, 1.0f);
		float ambientIntensity = 0.035f;
		EnvironmentDesc environment = {};
		// 광원 행렬과 그림자 캐스터가 변하지 않았으면 아틀라스를 재사용한다.
		bool reuseStaticShadows = true;
		// 광원 반경이 있는 그림자의 탐색·필터 표본 수다. 각각 1..32를 지원한다.
		uint32_t shadowBlockerSamples = 8;
		uint32_t shadowFilterSamples = 12;
		// 반그림자 탐색과 필터의 최대 반경을 아틀라스 타일 텍셀로 정한다. 0이면 고정 PCF다.
		float shadowMaxFilterRadius = 12.0f;
	};
	// 높이별 밀도와 실제 광원의 단일 산란을 사용하는 볼륨 안개다.
	struct VolumetricFogConfig
	{
		bool enabled = false;
		float density = .01f, heightFalloff = .1f, baseHeight = 0;
		Math::float3 albedo = {.9f, .95f, 1.f};
		float anisotropy = .35f, maxDistance = 80.f, resolutionScale = .5f;
		uint32_t steps = 48;
	};
	// 선택한 고급 렌더 모듈의 설정이다. 표준 지오메트리와 HDR을 사용한다.
	// 화면 공간 추적의 범위를 벗어난 반사는 EnvironmentLighting 모듈의 IBL을 사용한다.
	struct AdvancedRenderingConfig
	{
		bool enabled = false;
		bool ambientOcclusion = true;
		bool diffuseGI = true;
		bool reflections = true;
		bool temporalAA = true;
		bool bloom = true;
		bool transparency = true;
		VolumetricFogConfig fog = {};
		SurfaceDebugView surfaceDebugView = SurfaceDebugView::None;
		float aoRadius = 1.2f;
		float aoStrength = 1.0f;
		float rayDistance = 12.0f;
		float giStrength = 1.0f;
		float reflectionStrength = 1.0f;
		float rayThickness = 0.25f;
		float bloomStrength = 0.055f;
		float bloomThreshold = 1.0f;
		float bloomKnee = 0.5f;
		float historyWeight = 0.9f;
		// GPU log-average metering before bloom. exposure remains a multiplier
		// (exposure compensation) when enabled. Key is a scene-linear target.
		bool autoExposure = false;
		float exposureKey = 0.18f;
		// Log-luminance adaptation rate in inverse seconds; 0 holds after reset.
		float exposureAdaptationSpeed = 1.5f;
		// 1은 불투명 배경만, 2는 가까운 유리 뒤의 투명층까지 굴절 배경에 반영한다.
		uint32_t transparencyLayers = 1;
	};

    // 출력 단계의 색보정이다. 기본값은 중립이며 장면의 HDR·모션 히스토리를 바꾸지 않는다.
    struct PostProcessConfig
    {
        // -1..1 상대 조절축이며 켈빈 온도가 아니다. 양수는 따뜻한 색·마젠타 방향이다.
        float temperature = 0;
        float tint = 0;
        float contrast = 1;
        float saturation = 1;
        Math::float3 lift = {0,0,0};
        Math::float3 gamma = {1,1,1};
        Math::float3 gain = {1,1,1};
        bool sharpening = false;
        float sharpness = 0.2f;
    };
	// 렌더러의 설정 값이다. 지정하지 않은 항목은 아래 기본값을 사용한다.
	struct RendererConfig
	{
		bool vsync = true;
		bool allowReadback = false;
		Math::float4 clearColor = Math::float4(0.08f, 0.10f, 0.14f, 1.0f);
		LightingConfig lighting = {};
		AdvancedRenderingConfig advanced = {};
		PostProcessConfig postProcess = {};
		bool enableHdrRendering = false;
		// 둘 다 0이면 창 크기를 따른다. 지정하면 장면 해상도를 고정하고 비율을 유지해 창에 표시한다.
		uint32_t renderWidth = 0, renderHeight = 0;
		// HDR scene color/depth scale, 1..2. Tone mapping reduces to output size;
		// disabled HDR renders directly at output size.
		float hdrRenderScale = 1;
		float exposure = 1;
		// Scene/Mesh 렌더링의 HUD다. Canvas만 Render하는 경로에는 표시하지 않는다.
		bool enableProfilerHud = true;
		bool profilerStartsExpanded = false;
        // The device owns recording workers. Disabled records the same graph serially.
        bool enableParallelRenderGraph = false;
        // 0 selects hardware concurrency; 1 records on the calling thread.
        uint32_t recordingThreadCount = 0;
		// Temporary CPU workers for preparing texture mipmaps during scene upload.
		uint32_t texturePreparationThreadCount = 1;
		// Stock Scene path only. Disable independently for output/performance comparisons.
		bool enableInstancing = true;
		bool enableFrustumCulling = true;
        bool enableIncrementalTransformUploads = true;
        bool enableResourceSetCache = true;
        bool enableBindlessMaterials = true;
        bool enableCanvasBatching = true;
        bool enableCanvasImageCache = true;
        bool enableCanvasVertexBufferReuse = true;
        // 지원 장치에서는 가시 인스턴스와 간접 명령을 GPU에서 생성한다.
        bool enableGpuCulling = false;
        // 현재 프레임의 깊이로 가려진 불투명 객체를 추가 제거한다.
        bool enableOcclusionCulling = false;
		// Shared by the stock Main, Surface and transparent passes. Shadow maps
		// retain level zero so camera LOD changes do not invalidate static shadows.
		bool enableLod = true;
		float lodPixelError = 1.0f;
		float lodHysteresis = 0.15f;
		bool debugLodColor = false;
		// Stock advanced HDR Scene diagnostic. Requires Feature::Wireframe;
		// opaque/masked Main uses LOD-colored lines over a solid depth prepass.
		bool debugWireframe = false;
	};
}

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
    // Custom shadow vertices may opt into the stock material coverage fragment.
    // They must preserve DrawConstants and output UV at location/TEXCOORD 0
    // and vertex alpha at 1 (Metal: position, float2 uv, float alpha).
    bool stockShadowCoverage = false;
};
// 직접 준비한 RHI 입력이다. Scene에는 GPU 자원을 넣지 않는다.
struct RendererDrawDesc
{
    // 생략하면 Scene의 Mesh를 사용한다. 위치 float3·법선 float3·UV float2·탄젠트 float4·선형 RGBA float4(64바이트).
    RHI::BufferHandle vertexBuffer = nullptr;
    std::vector<RHI::ResourceBinding> vertexResources;
    // 기본 draw 상수 192바이트 뒤에 전달하며 additionalConstantBytes와 크기를 맞춘다.
    std::vector<uint8_t> inlineConstants;
    // 표준 64바이트 정점 형식의 변형 결과는 고급 렌더 패스에서도 공유한다.
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
    [[nodiscard]] const RendererConfig& GetAppliedConfig() const;
    bool SetConfig(const RendererConfig&);
    // 선택적 셰이더 교체다. 입력 바이트와 진입점은 복사하며 다음 Render에서 적용한다.
    bool SetShaders(const RendererShaderDesc&);
    // 반환한 셰이더 바이트와 진입점은 다음 설정 적용까지 유효한 읽기 뷰다.
    [[nodiscard]] RendererShaderDesc GetShaders() const;

private:
    friend struct RHI::ProfilerAccess;
    [[nodiscard]] std::uint64_t GetLiveSourceId() const noexcept;
    friend class ModelRenderer;
    void InitializeLiveProfiling() noexcept;
    bool PrepareLiveFrame(Platform::LiveFrameKey) noexcept;
    void PublishLiveGeometryAvailability(bool) noexcept;
    struct Impl;
    std::unique_ptr<Impl> m_impl;
    Renderer(RHI::IDevice&,const void* window,const RendererConfig&);
};
}
