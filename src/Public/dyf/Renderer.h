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
	// 표면 패스가 만든 중간 데이터를 어떤 의미로 표시할지 고른다. Motion은 이전 프레임과의 이동 정보다.
	enum class SurfaceDebugView : uint8_t { None, BaseColor, Normal, Roughness, Metallic, Depth, Motion };

	// 월드 공간의 한 지점에서 캡처한 정적 간접광·반사 정보다.
	// 한 위치에서 얻은 HDR 반사·확산 입력과 영향 상자를 묶는다. visibility가 없다는 것은 차폐 정보가 없다는 뜻이며, 완전히 보인다는 측정 결과가 아니다.
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
	// 특정 방향에서 들어오거나 나가는 빛의 세기를 나타내는 값(방사휘도)을 radiance에 담아 반사·배경에 사용한다.
	// 전역 하늘과 국소 프로브를 조명 입력으로 지정하며, diffuseRadiance는 선택적 확산광 입력이다. GPU 자원 준비는 렌더 모듈이 맡는다.
	struct EnvironmentDesc
	{
		Math::float3 diffuseColor = Math::float3(1.0f, 1.0f, 1.0f);
		float diffuseIntensity = 1.0f;
		Math::float3 specularColor = Math::float3(1.0f, 1.0f, 1.0f);
		float specularIntensity = 1.0f;
		// Equirectangular linear HDR sky. Z is up; rotation is around world Z.
		HdrImage radiance;
		// 선택적 확산광 전용 파노라마로, 방 캡처에서 직접 보이는 발광을 제외해도 반사용 radiance에는 발광체의 형태를 남길 수 있다.
		HdrImage diffuseRadiance;
		float intensity = 1.0f;
		float rotationRadians = 0.0f;
		bool diffuse = true;
		bool specular = true;
		bool background = true;
		// 월드 공간의 상자로 반사 시차를 보정하며, 확산 SH는 한 지점의 표본으로 남아 라이트맵이나 프로브 볼륨이 되지 않는다.
		bool localProbe = false;
		Math::float3 probePosition = Math::float3(0.0f, 0.0f, 0.0f);
		Math::float3 probeBoxMin = Math::float3(-1.0f, -1.0f, -1.0f);
		Math::float3 probeBoxMax = Math::float3(1.0f, 1.0f, 1.0f);
		// 영향 상자의 가장자리에서 전역 환경으로 전환하고 겹친 구역은 위치별로 혼합한다.
		std::vector<EnvironmentProbeDesc> probes;
	};

	// 조명 사용 여부·직접광·그림자·환경광을 요청해 Scene 광원과 함께 프레임 상수를 만들며, 광원 종류와 표면 반응은 Light·Material에서 정한다.
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
	// 밀도·높이 감쇠·산란 방향·거리·적분 횟수로 안개를 정한다. steps는 시선 따라 누적하는 표본 수, resolutionScale은 효과의 계산 해상도 비율이다.
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
	// 고급 렌더링 설정만 켜도 모든 구현이 생기지는 않는다. HDR·표준 지오메트리와 필요한 RenderExtension의 등록이 함께 충족되어야 한다.
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
		// 이전 프레임을 얼마나 섞을지 정한다. 카메라 컷·장면 교체·명시적 InvalidateHistory 후에는 누적 이력이 다시 시작된다.
		float historyWeight = 0.9f;
		// bloom 전 GPU 로그 평균 휘도로 자동 노출을 구하며, exposureKey는 선형 목표값이고 exposure는 추가 보정 배율이다.
		// 측정과 적용은 GPU 경로에서 진행하므로 CPU에서 결과가 즉시 완료되었다는 뜻은 아니다.
		bool autoExposure = false;
		float exposureKey = 0.18f;
		// Log-luminance adaptation rate in inverse seconds; 0 holds after reset.
		float exposureAdaptationSpeed = 1.5f;
		// 1은 불투명 배경만, 2는 가까운 유리 뒤의 투명층까지 굴절 배경에 반영한다.
		uint32_t transparencyLayers = 1;
	};

    // 출력 단계의 색보정이다. 기본값은 중립이며 장면의 HDR·모션 히스토리를 바꾸지 않는다.
    // 톤 매핑 후 표시 색을 조절하는 설정 묶음이다. temperature/tint는 상대 축이고 gamma는 채널별 보정 값이다.
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
	// 생성 때 입력 설정을 검증하며, SetConfig는 유효한 요청을 pendingConfig에 복사해 예약한다. 다음 Render의 설정 경계에서 필요한 출력·파이프라인 생성이 성공한 뒤에만 현재 적용 설정을 바꾼다.
	struct RendererConfig
	{
		bool vsync = true;
		// CPU Image 캡처를 허용하는 스왑체인 설정이다. readback 포인터를 전달하는 것만으로 이 권한이나 장치 지원이 생기지는 않는다.
		bool allowReadback = false;
		Math::float4 clearColor = Math::float4(0.08f, 0.10f, 0.14f, 1.0f);
		LightingConfig lighting = {};
		AdvancedRenderingConfig advanced = {};
		PostProcessConfig postProcess = {};
		bool enableHdrRendering = false;
		// 둘 다 0이면 창 크기를 따른다. 지정하면 장면 해상도를 고정하고 비율을 유지해 창에 표시한다.
		uint32_t renderWidth = 0, renderHeight = 0;
		// HDR 색·깊이를 출력 해상도의 1~2배로 렌더해 출력 크기로 톤 매핑하며, HDR을 끄면 출력 크기로 직접 렌더한다.
		float hdrRenderScale = 1;
		float exposure = 1;
		// Scene/Mesh 렌더링의 HUD다. Canvas만 Render하는 경로에는 표시하지 않는다.
		bool enableProfilerHud = true;
		bool profilerStartsExpanded = false;
        // The device owns recording workers. Disabled records the same graph serially.
        // 독립 패스의 CPU 명령 기록과 네이티브 변환을 작업자에 분배한다. 이 토글이 GPU 동시 실행이나 성능 향상을 보장하지는 않는다.
        bool enableParallelRenderGraph = false;
        // 0 selects hardware concurrency; 1 records on the calling thread.
        uint32_t recordingThreadCount = 0;
		// Temporary CPU workers for preparing texture mipmaps during scene upload.
		uint32_t texturePreparationThreadCount = 1;
		// Stock Scene path only. Disable independently for output/performance comparisons.
		bool enableInstancing = true;
		bool enableFrustumCulling = true;
        // 변환 변경 구간 업로드·자원 세트 캐시·재질 텍스처 배열 경로를 각각 제어한다. 실제 사용 조건은 Impl의 UsesInstanceStream/SupportsBindlessMaterials 등에서 다시 검사한다.
        bool enableIncrementalTransformUploads = true;
        bool enableResourceSetCache = true;
        bool enableBindlessMaterials = true;
        bool enableCanvasBatching = true;
        bool enableCanvasImageCache = true;
        bool enableCanvasVertexBufferReuse = true;
        // 지원 장치에서는 가시 인스턴스와 간접 명령을 GPU에서 생성한다.
        // Compute·간접 draw 지원과 표준 인스턴싱 경로가 필요하다. CPU가 마련한 버퍼 용량과 GPU가 골라낸 실제 가시 객체 수를 구분해야 한다.
        bool enableGpuCulling = false;
        // 현재 프레임의 깊이로 가려진 불투명 객체를 추가 제거한다.
        // GPU 가시성 경로와 표면 깊이 패스가 함께 준비된 프레임에서 사용한다. 설정 true만으로 차폐 제거가 수행되었다고 판단하지 않는다.
        bool enableOcclusionCulling = false;
		// 상태 전환 방향마다 서로 다른 기준을 적용해 잦은 전환을 막는 방식(히스테리시스)의 폭을 lodHysteresis로 지정한다.
		// 메인·표면·투명 패스는 선택한 LOD를 공유하며, 그림자는 원본 LOD를 유지해 카메라 LOD 변경만으로 정적 그림자가 무효화되지 않게 한다.
		bool enableLod = true;
		float lodPixelError = 1.0f;
		float lodHysteresis = 0.15f;
		bool debugLodColor = false;
		// Feature::Wireframe이 필요한 표준 고급 HDR 진단으로, 불투명·마스크 Main을 깊이 선행 패스 위에 LOD 색 선으로 표시한다.
		bool debugWireframe = false;
	};
}

namespace dyf::RHI { class IDevice; }
namespace dyf
{
class Image;
struct Camera;
struct MeshData;
// SetShaders가 셰이더 바이트와 진입점 문자열을 복사해 사용하는 RHI 재정의 입력이며, 비어 있는 단계만 기본 셰이더를 사용한다.
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
// Scene 객체 ID 순서로 호출자가 준비한 RHI 입력을 전달하며, Scene 자체에는 GPU 자원의 소유권을 넣지 않는다.
struct RendererDrawDesc
{
    // 표면에 수직인 방향(법선)과 표면을 따라가는 방향(접선)을 정점 속성으로 사용한다.
    // 버퍼를 생략하면 Scene의 Mesh를 사용하며, 정점은 위치 float3 → 법선 float3 → UV float2 → 접선 float4 → 선형 RGBA float4 순서의 64바이트다.
    // 접선 float4의 xyz에는 접선, w에는 종법선 방향 부호를 저장한다.
    RHI::BufferHandle vertexBuffer = nullptr;
    std::vector<RHI::ResourceBinding> vertexResources;
    // 기본 draw 상수 192바이트 뒤에 전달하며 additionalConstantBytes와 크기를 맞춘다.
    std::vector<uint8_t> inlineConstants;
    // 표준 64바이트 정점 형식과 모션 입력을 유지하는 변형 결과를 고급 패스에서 공유하며, 사용자 vertexResources·추가 상수가 있으면 표준 묶음 최적화는 우회한다.
    bool stockGeometry = false;
    // 현재·이전 변형 정점과 이력 유효성은 모션 계산에 쓰인다. 유효한 localBounds가 있으면 원본 메시 대신 변형 결과의 경계로 가시성을 판단한다.
    RHI::BufferHandle previousVertexBuffer = nullptr;
    Math::Bounds3 localBounds;
    bool hasLocalBounds = false;
    bool deformationHistoryValid = true;
    uint64_t geometryRevision = 0;
};
class Scene;
class Canvas;
class RenderExtension;

// 렌더러가 기록한 드로 수·인스턴스 수·삼각형 수와 해당 프레임의 경로/출력 크기를 보관한다. 이 구조체는 수치만 보관하며 GPU 자원을 소유하지 않는다.
// 최근 Scene 제출에 연결된 CPU 관측 통계다. 간접 draw에서는 GPU가 정하는 가시 인스턴스 수를 제출 용량으로 채우지 않는다.
struct RendererStatistics
{
    uint32_t mainDrawCount=0,mainInstanceCount=0,culledEntityCount=0;
    // Submitted indexed-triangle work. Shadow counts include every atlas view.
    uint64_t mainTriangleCount=0,shadowTriangleCount=0;
    uint32_t shadowDrawCount=0,shadowInstanceCount=0;
    // CPU 직접 draw 경로에서 메인 패스의 객체 수를 선택한 LOD별로 센다. 0은 원본이며 GPU 간접 draw의 실제 가시 개수를 리드백한 값은 아니다.
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
// Renderer는 복사할 수 없는 자원 소유자다. public 함수는 RendererFacade.cpp를 거쳐 Impl의 기능별 구현 파일로 연결된다.
class Renderer
{
public:
    // 장치를 받지 않는 Create는 장치를 만들고 소유한다. IDevice&를 받는 Create는 장치를 빌리므로 호출자가 Renderer보다 오래 살려 두어야 한다.
    [[nodiscard]] static std::unique_ptr<Renderer> Create(const void* window);
    [[nodiscard]] static std::unique_ptr<Renderer> Create(const void* window, const RendererConfig&);
    [[nodiscard]] static std::unique_ptr<Renderer> Create(RHI::IDevice& device, const void* window);
    [[nodiscard]] static std::unique_ptr<Renderer> Create(RHI::IDevice& device, const void* window, const RendererConfig&);
    ~Renderer();
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    // false는 실패다. 창 프레임을 확보하지 못했어도 장치 손실이 아니면 그 프레임을 생략하고 true를 반환하며, 장치 손실은 실패로 반환한다.
    // 생략한 프레임의 readback은 비운다. Mesh overload는 기본 광원, Scene overload는 등록된 광원만 사용한다.
    [[nodiscard]] bool Render(const MeshData&, Image* readback = nullptr);
    [[nodiscard]] bool Render(const MeshData&, const Camera&, Image* readback = nullptr);
    [[nodiscard]] bool Render(const MeshData&, const MaterialDesc&, const Camera&, Image* readback = nullptr);
    [[nodiscard]] bool Render(const Scene&, Image* readback = nullptr);
    [[nodiscard]] bool Render(const Scene&, const Camera&, Image* readback = nullptr);
    [[nodiscard]] bool Render(const Canvas&, Image* readback = nullptr);
    [[nodiscard]] bool Render(const Scene&, const Canvas& overlay, Image* readback = nullptr);
    [[nodiscard]] bool Render(const Scene&, const Camera&, const Canvas& overlay, Image* readback = nullptr);

    // 호출자 소유 RGBA/BGRA8 텍스처(RenderTarget | ShaderResource)에 렌더링을 제출하고 ShaderResource 상태로 남긴다.
    // before에는 새 텍스처면 Undefined, 이후에는 실제 시작 상태를 넘긴다. 텍스처 소유권은 호출자가 유지한다.
    // 창의 BeginFrame/Present는 수행하지 않으며, Profiler HUD·Canvas 오버레이는 창 렌더링 경로에서 처리한다.
    // Gui::RegisterTexture에는 SRGB 타깃을 권장한다. UNORM 출력에는 이미 표시용 감마가 적용되어 있다.
    [[nodiscard]] bool RenderToTexture(const Scene&, const Camera&, RHI::TextureHandle target,
        RHI::ResourceState before);

    // 시작 시 기존 기하·텍스처 캐시로 불투명·마스크 장면의 여섯 방향을 동기 캡처해 정적 선형 HDR 파노라마(4*faceSize × 2*faceSize)를 만든다.
    // 매 프레임 또는 Render와 동시에 호출하지 않는다. 유리·IBL·화면 GI는 캡처에서 제외한다.
    // includeEmitters=false는 확산 프로브용 반사 직접광만, true는 반사 프로브용 가시 발광도 담으며 Scene은 수정하지 않는다.
    // 렌더러 설정을 복원하며 실패하면 result를 보존한다.
    // visibility를 지정하면 같은 원본 깊이에서 월드 거리(R)·거리 제곱(G)의 파노라마도 얻는다.
    [[nodiscard]] bool CaptureEnvironment(const Scene&,Math::float3 position,uint32_t faceSize,
        float nearPlane,float farPlane,HdrImage& result,bool includeEmitters=true,HdrImage* visibility=nullptr);

    // RHI를 직접 조합하는 확장만 사용하는 입력이다. 배열 순서는 Scene의 객체 ID 순서다.
    [[nodiscard]] bool Render(const Scene&, const Camera&, const std::vector<RendererDrawDesc>&,
        const Canvas* overlay = nullptr, Image* readback = nullptr);
    [[nodiscard]] bool Render(const Scene&, const std::vector<RendererDrawDesc>&,
        const Canvas* overlay = nullptr, Image* readback = nullptr);
    [[nodiscard]] RHI::IDevice& GetDevice() const;

    // 창·텍스처 Scene의 시간 표본을 약 200ms마다 게시하며, 조회는 기다리지 않는다. 시작 시 환경 캡처는 제외한다.
    // CPU 실제 경과 시간과 완료된 비동기 GPU 프레임·패스 시간은 별개이며, 아직 오지 않은 GPU 값은 0으로 해석하지 않는다.
    // 뷰포트 CPU 구간은 Model 준비·GUI·Present를 제외하고, 프레임 간격에는 Scene 호출 사이의 시간이 포함된다.
    // GPU 구간은 Model compute를 제외하며 제출 사이의 유휴 시간을 포함할 수 있다.
    [[nodiscard]] const Platform::ProfilerTimingSnapshot& GetProfilerTimingSnapshot() const;

    // 최근 Scene의 메인·그림자 제출 수이며 제출 전에는 0이고, 별도 Surface·투명 패스와 Canvas·HUD는 제외한다.
    [[nodiscard]] const RendererStatistics& GetLastRenderStatistics() const;
    // Published at draw submission, even when later readback/Present fails. Survives setting swaps.
    // 그리기 제출이 성공한 시점에 받은 펜스다. 핸들이 있다는 사실만으로 GPU가 이미 끝났다고 판단할 수 없으며 완료 여부는 RHI에서 조회하거나 기다려야 한다.
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
    // 확장은 창 프레임 사이에 등록하며 렌더러가 소유하고 등록 순서대로 패스를 추가한다. unique_ptr를 넘기면 등록이 실패해도 호출자에게 소유권을 돌려주지 않는다.
    bool AddExtension(std::unique_ptr<RenderExtension>);
    // GetConfig는 아직 적용되지 않은 최신 요청을 우선 반환한다. GetAppliedConfig는 현재 Impl::config를 반환하므로 요청 실패·지연 상태를 구별할 수 있다.
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
    // 구현과 자원 배치를 공개 헤더 밖에 숨기는 PImpl 경계다. unique_ptr가 Impl을 단독 소유하고 소멸자는 구현 파일에서 실제 정리 코드를 호출한다.
    struct Impl;
    std::unique_ptr<Impl> m_impl;
    Renderer(RHI::IDevice&,const void* window,const RendererConfig&);
};
}
