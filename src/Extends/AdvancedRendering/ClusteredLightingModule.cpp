#include "AdvancedRenderingInternal.h"
#include "ClusteredLighting.h"
#include "AreaLightingLtc.h"

namespace dyf::Detail
{
namespace
{
class ClusteredLightingModule final : public RenderExtension
{
public:
    const char* GetName() const override {return "ClusteredLighting";}

    RenderPreparation GetPreparationStage() const override {return RenderPreparation::Lighting;}

    RenderRequirements GetRequirements(const Renderer&) const override
    {
        RenderRequirements requirements;
        requirements.providesLocalLights=true;
        return requirements;
    }

    bool Prepare(RenderFrame& frame,RHI::ICommandList& uploads) override
    {
        m_prepared=false;
        const auto& config=frame.renderer.GetConfig();
        if(!m_areaLighting.Prepare(frame.renderer.GetDevice(),uploads)) return false;
        const bool active=config.lighting.enabled && config.lighting.direct && !frame.lodDebug;
        if(!m_lighting.Prepare(frame.renderer.GetDevice(),uploads,frame.scene,frame.camera,
            config.lighting.clusteredLighting,active)) return false;
        frame.lighting.clusterLights={m_lighting.GetLights(),{}};
        frame.lighting.clusterList={m_lighting.GetList(),{}};
        frame.lighting.clusterConstants={m_lighting.GetConstants(),{}};
        frame.lighting.clustered=m_lighting.IsActive();
        frame.lighting.areaMatrix={m_areaLighting.GetMatrix(),{}};
        frame.lighting.areaAmplitude={m_areaLighting.GetAmplitude(),{}};
        m_prepared=true;
        return true;
    }

    bool AddPasses(RenderStage stage,RenderFrame& frame,RHI::RenderGraph& graph) override
    {
        if(stage!=RenderStage::BeforeOpaque || !m_prepared) return true;
        if(!m_lighting.AddPasses(graph))
            return AdvancedPassFailure("Clustered lighting pass creation failed.");
        if(!m_areaLighting.Import(graph))
            return AdvancedPassFailure("LTC area light resource import failed.");
        frame.lighting.clusterLights={m_lighting.GetLights(),m_lighting.GetLightsHandle()};
        frame.lighting.clusterList={m_lighting.GetList(),m_lighting.GetListHandle()};
        frame.lighting.clusterConstants={m_lighting.GetConstants(),m_lighting.GetConstantsHandle()};
        frame.lighting.clustered=m_lighting.IsActive();
        frame.lighting.areaMatrix={m_areaLighting.GetMatrix(),m_areaLighting.GetMatrixHandle()};
        frame.lighting.areaAmplitude={m_areaLighting.GetAmplitude(),m_areaLighting.GetAmplitudeHandle()};
        return true;
    }

    void CommitFrame(const RenderFrame&) override
    {
        if(m_prepared)
        {
            m_lighting.CommitFrame();
            m_areaLighting.CommitFrame();
        }
        m_prepared=false;
    }

    void InvalidateHistory(bool release) override
    {
        if(release)
        {
            m_lighting.Reset();
            m_areaLighting.Reset();
        }
        m_prepared=false;
    }

private:
    ClusteredLighting m_lighting;
    AreaLightingLtc m_areaLighting;
    bool m_prepared=false;
};
}

std::unique_ptr<RenderExtension> CreateClusteredLightingModule()
{
    return std::make_unique<ClusteredLightingModule>();
}
}
