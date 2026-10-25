#pragma once

#include "dyf/Material.h"
#include "dyf/RHI/RenderGraph.h"
#include <array>
#include "dyf/Renderer.h"

namespace dyf
{
// 자원 소유권은 제공한 렌더러나 모듈에 있다. 그래프 핸들은 현재 프레임에서만 유효하다.
struct RenderTexture
{
    RHI::TextureHandle texture = nullptr;
    RHI::RGResourceHandle resource;
};

struct RenderBuffer
{
    RHI::BufferHandle buffer = nullptr;
    RHI::RGResourceHandle resource;
};

// 환경광 셰이더와 투명 모듈이 공유하는 GPU 상수 형식이다.
struct RenderEnvironmentConstants
{
    std::array<Math::float4,9> sh;
    Math::float4 settings, screen, probePosition, probeBoxMin, probeBoxMax;
};
static_assert(sizeof(RenderEnvironmentConstants)==224);

struct RenderEnvironmentResources
{
    RenderEnvironmentConstants constants = {};
    RHI::TextureHandle prefilter = nullptr, probePrefilter = nullptr;
    RHI::TextureHandle brdf = nullptr;
    RHI::BufferHandle probeData = nullptr, probeParameters = nullptr;
    RHI::BufferHandle constantBuffer = nullptr;
    RenderTexture diffuse = {}, specular = {};
};

// 모듈은 조명을 관리하는 구현 클래스 대신 준비된 입력만 빌린다.
struct RenderLightingResources
{
    RenderBuffer lights = {}, shadowConstants = {};
    RenderBuffer clusterLights = {}, clusterList = {}, clusterConstants = {};
    RenderTexture areaMatrix = {}, areaAmplitude = {};
    RenderEnvironmentResources environment;
    bool clustered = false;
};

struct RenderHistory
{
    Math::float4x4 previousCameraView = Math::float4x4::Identity();
    Math::float4x4 previousCameraProjection = Math::float4x4::Identity();
    uint32_t frameIndex = 0;
    bool valid = false;
};

// 기본 경로에서 선택한 지오메트리·LOD·재질을 모든 추가 패스가 함께 사용한다.
struct RenderGeometryDraw
{
    RHI::BufferHandle vertexBuffer = nullptr, indexBuffer = nullptr;
    RHI::BufferHandle previousVertexBuffer = nullptr, instanceBuffer = nullptr, indirectArguments = nullptr;
    uint32_t instanceOffset = 0, indirectOffset = 0;
    bool deformationHistoryValid = true;
    uint32_t indexCount = 0;
    Math::Bounds3 localBounds;
    Math::float4x4 model = Math::float4x4::Identity();
    Math::float4x4 previousMVP = Math::float4x4::Identity();
    // Scene 재질은 패스 기록이 끝날 때까지 유효하다. 모듈은 프레임 종료 때 참조를 비운다.
    const MaterialDesc* material = nullptr;
    Math::float4 baseColor = {1,1,1,1};
    std::array<RHI::TextureHandle,ToIndex(MaterialTextureKind::Count)> textures = {};
    uint32_t textureFlags = 0;
    // Direct instancing uses instanceBuffer's 128-byte model/previous-MVP rows.
    // The provider culls instances before batching; localBounds describes the representative.
    // Indirect draws obtain their instance count from indirectArguments instead.
    uint32_t instanceCount = 1;
};
// 등록 순서와 별개로 불투명 조명 뒤 안개, 투명체, 최종 출력 순서를 유지한다.
enum class RenderStage { BeforeOpaque, OpaqueLighting, Background, AfterOpaque, Atmosphere, Transparency, PostProcess };
enum class RenderPreparation { Lighting, Effects };

struct RenderRequirements
{
    bool providesSurface = false;
    bool needsSurface = false;
    bool needsGeometry = false;
    bool handlesTransparency = false;
    bool writesOutput = false;
    bool providesLocalLights = false;
    bool providesEnvironment = false;
};

// 확장은 프레임 자원을 빌려 패스를 추가한다. 자원과 제출의 수명은 렌더러가 관리한다.
struct RenderFrame
{
    Renderer& renderer;
    const Scene& scene;
    Camera& camera;
    const std::vector<RendererDrawDesc>* draws = nullptr;
    // AfterOpaque에서 color를 교체하면 다음 확장과 기본 톤 매핑의 입력이 된다.
    RenderTexture output={}, color={}, depth={}, shadow={};
    RenderTexture normal={}, albedo={}, ao={}, motion={};
    RenderTexture reactive={};
    RenderBuffer visibleInstances={}, indirectArguments={};
    RenderLightingResources lighting={};
    RenderHistory history={};
    RenderRequirements requirements={};
    std::vector<RenderGeometryDraw> geometry={};
    RHI::TextureHandle blackTexture = nullptr;
    bool lodDebug = false, wireframe = false, gpuVisibility = false;
    bool temporalAA = false;
    bool environmentCapture = false;
    bool hasShadows = false;
    bool outputWritten = false;
    // Opt-in: the current depth texture/resource contains complete nearest
    // opaque and alpha-mask coverage for Main with the same camera, geometry,
    // LOD, sidedness and alpha tests. Later changes must clear this flag.
    // The renderer may preserve this depth and use early fragment tests.
    bool opaqueDepthComplete = false;
};

class RenderExtension
{
public:
    virtual ~RenderExtension() = default;
    virtual const char* GetName() const { return "RenderExtension"; }
    virtual RenderPreparation GetPreparationStage() const { return RenderPreparation::Effects; }
    virtual bool UsesSurfacePipeline(const Renderer&) const { return false; }
    virtual bool NeedsSurface(const Renderer&) const { return false; }
    // 기존 확장의 두 질의도 같은 요구 사항으로 수용한다.
    virtual RenderRequirements GetRequirements(const Renderer& renderer) const
    {
        RenderRequirements requirements;
        requirements.providesSurface=UsesSurfacePipeline(renderer);
        requirements.needsSurface=NeedsSurface(renderer);
        requirements.needsGeometry=requirements.needsSurface;
        return requirements;
    }
    // 카메라 지터는 가시성·표면 데이터 준비 전에 모든 패스에 한 번만 적용한다.
    virtual bool PrepareCamera(RenderFrame&) { return true; }
    virtual bool Prepare(RenderFrame&, RHI::ICommandList&) { return true; }
    virtual bool AddPasses(RenderStage, RenderFrame&, RHI::RenderGraph&) { return true; }
    virtual void CommitFrame(const RenderFrame&) {}
    virtual void InvalidateHistory(bool release = false) { (void)release; }
};
}
