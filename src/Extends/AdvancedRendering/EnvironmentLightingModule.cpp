#include "AdvancedRenderingInternal.h"
#include "EnvironmentLighting.h"
#include <memory>

namespace dyf::Detail
{
namespace
{
class EnvironmentLightingModule final : public RenderExtension
{
public:
    explicit EnvironmentLightingModule(std::shared_ptr<EnvironmentFilteringProof> proof):m_proof(std::move(proof)) {}
    const char* GetName() const override { return "EnvironmentLighting"; }

    RenderPreparation GetPreparationStage() const override { return RenderPreparation::Lighting; }

    RenderRequirements GetRequirements(const Renderer& renderer) const override
    {
        RenderRequirements requirements;
        if(!AdvancedModuleEnabled(renderer)) return requirements;
        requirements.providesEnvironment=true;
        const auto& config=renderer.GetConfig();
        const auto& environment=config.lighting.environment;
        requirements.needsSurface=config.lighting.enabled && !config.debugLodColor && !config.debugWireframe &&
            (environment.radiance.IsValid() || !environment.probes.empty()) &&
            (environment.diffuse || environment.specular);
        requirements.needsGeometry=requirements.needsSurface;
        return requirements;
    }

    bool Prepare(RenderFrame& frame,RHI::ICommandList& uploads) override
    {
        m_prepared=false;
        if(m_proof) *m_proof={};
        const bool advanced=AdvancedModuleEnabled(frame.renderer);
        // 캡처는 고급 효과가 꺼진 상태에서도 원본 배경과 환경 입력을 준비한다.
        if(!advanced && !frame.environmentCapture) return true;
        if(!m_environment) m_environment=std::make_unique<EnvironmentLighting>();
        const auto& config=frame.renderer.GetConfig();
        const auto& target=frame.color.texture->GetDesc();
        if(!m_environment->Prepare(frame.renderer.GetDevice(),uploads,config.lighting.environment,
            frame.camera,target.width,target.height,advanced && config.advanced.ambientOcclusion && !frame.lodDebug,
            (advanced || frame.environmentCapture) && config.lighting.enabled && !frame.lodDebug,
            nullptr,config.texturePreparationThreadCount)) return false;
        auto& resources=frame.lighting.environment;
        resources.constants=m_environment->GetCPUConstants();
        resources.constantBuffer=m_environment->GetConstants();
        resources.prefilter=m_environment->GetPrefilter();
        resources.probeData=m_environment->GetProbeData();
        resources.probePrefilter=m_environment->GetProbePrefilter();
        resources.probeParameters=m_environment->GetProbeParameters();
        resources.diffuse={m_environment->GetDiffuse(),{}};
        resources.specular={m_environment->GetSpecular(),{}};
        if(m_proof) *m_proof={&frame,&frame.renderer.GetDevice(),resources,m_environment->UsesNativeFiltering()};
        m_prepared=true;
        return true;
    }

    bool AddPasses(RenderStage stage,RenderFrame& frame,RHI::RenderGraph& graph) override
    {
        if(frame.outputWritten || !m_prepared) return true;
        if(stage==RenderStage::BeforeOpaque)
        {
            if(!m_environment->Import(graph)) return AdvancedPassFailure("Environment resource import failed.");
            frame.lighting.environment.diffuse={m_environment->GetDiffuse(),m_environment->GetDiffuseHandle()};
            frame.lighting.environment.specular={m_environment->GetSpecular(),m_environment->GetSpecularHandle()};
        }
        else if(stage==RenderStage::OpaqueLighting)
        {
            if(m_environment->IsDiffuseEnabled() &&
                !m_environment->AddDiffuse(graph,frame.depth.resource,frame.depth.texture,
                    frame.normal.resource,frame.normal.texture,frame.ao.resource,frame.ao.texture))
                return AdvancedPassFailure("Environment diffuse pass creation failed.");
            if(m_environment->IsSpecularEnabled() &&
                !m_environment->AddSpecular(graph,frame.depth.resource,frame.depth.texture,
                    frame.normal.resource,frame.normal.texture,frame.albedo.resource,frame.albedo.texture,
                    frame.ao.resource,frame.ao.texture))
                return AdvancedPassFailure("Environment specular pass creation failed.");
        }
        else if(stage==RenderStage::Background)
        {
            const auto& config=frame.renderer.GetConfig();
            const bool surfaceDebug=AdvancedModuleEnabled(frame.renderer) &&
                config.advanced.surfaceDebugView!=SurfaceDebugView::None && !frame.wireframe;
            if(!surfaceDebug && m_environment->IsBackgroundEnabled() &&
                !m_environment->AddBackground(graph,frame.color.resource,frame.color.texture,
                    frame.depth.resource,frame.depth.texture))
                return AdvancedPassFailure("Environment background pass creation failed.");
        }
        return true;
    }

    void CommitFrame(const RenderFrame&) override
    {
        if(m_prepared) m_environment->CommitFrame();
        m_prepared=false;
        if(m_proof) *m_proof={};
    }

    void InvalidateHistory(bool release) override
    {
        if(m_proof) *m_proof={};
        if(release)
        {
            m_environment.reset();
            m_prepared=false;
        }
    }

private:
    std::shared_ptr<EnvironmentFilteringProof> m_proof;
    std::unique_ptr<EnvironmentLighting> m_environment;
    bool m_prepared=false;
};
}

std::unique_ptr<RenderExtension> CreateEnvironmentLightingModule(std::shared_ptr<EnvironmentFilteringProof> proof)
{
    return std::make_unique<EnvironmentLightingModule>(std::move(proof));
}
}
