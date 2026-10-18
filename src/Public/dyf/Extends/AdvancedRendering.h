#pragma once

#include "dyf/RenderExtension.h"
#include <array>
#include <memory>

namespace dyf
{
enum class AdvancedRenderModule
{
    EnvironmentLighting, ClusteredLighting, SurfaceLighting, VolumetricFog, Transparency, PostProcess
};

// 필요한 모듈만 선택해 렌더러의 확장으로 등록한다. LTC 면광원은 ClusteredLighting이 제공한다.
std::unique_ptr<RenderExtension> CreateAdvancedRenderingModule(AdvancedRenderModule);

// 기존 사용법을 유지하며 독립 모듈을 함께 소유하는 프리셋이다.
class AdvancedRendering final : public RenderExtension
{
public:
    AdvancedRendering();
    ~AdvancedRendering() override;
    const char* GetName() const override { return "AdvancedRendering"; }
    RenderPreparation GetPreparationStage() const override { return RenderPreparation::Lighting; }
    RenderRequirements GetRequirements(const Renderer&) const override;
    bool UsesSurfacePipeline(const Renderer&) const override;
    bool NeedsSurface(const Renderer&) const override;
    bool PrepareCamera(RenderFrame&) override;
    bool Prepare(RenderFrame&, RHI::ICommandList&) override;
    bool AddPasses(RenderStage, RenderFrame&, RHI::RenderGraph&) override;
    void CommitFrame(const RenderFrame&) override;
    void InvalidateHistory(bool release = false) override;
private:
    std::array<std::unique_ptr<RenderExtension>,6> m_modules;
};
}
