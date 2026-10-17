#pragma once

#include "dyf/Renderer.h"
#include "dyf/RenderResources.h"

namespace dyf
{
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
