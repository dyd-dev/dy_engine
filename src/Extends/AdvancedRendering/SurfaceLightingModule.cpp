#include "AdvancedRenderingInternal.h"
#include "ScreenSpaceLighting.h"
#include "dyf/Camera.h"
#include <cmath>

namespace dyf::Detail
{
namespace
{
class SurfaceLightingModule final : public RenderExtension
{
public:
    const char* GetName() const override { return "SurfaceLighting"; }

    RenderRequirements GetRequirements(const Renderer& renderer) const override
    {
        RenderRequirements requirements;
        if(!AdvancedModuleEnabled(renderer)) return requirements;
        const auto& config=renderer.GetConfig();
        const auto& quality=config.advanced;
        const bool diagnostic=config.debugLodColor || config.debugWireframe;
        const bool surfaceDebug=quality.surfaceDebugView!=SurfaceDebugView::None;
        requirements.providesSurface=true;
        requirements.writesOutput=surfaceDebug && !config.debugWireframe;
        requirements.needsSurface=config.debugWireframe || surfaceDebug;
        if(!diagnostic)
        {
            const auto& device=renderer.GetDevice();
            const bool occlusion=config.enableGpuCulling && config.enableInstancing && config.enableOcclusionCulling &&
                device.Supports(RHI::Feature::Compute) && device.Supports(RHI::Feature::IndirectDraw);
            requirements.needsSurface|=occlusion || (config.lighting.enabled &&
                (quality.ambientOcclusion || quality.diffuseGI || quality.reflections));
        }
        requirements.needsGeometry=requirements.needsSurface;
        return requirements;
    }

    bool Prepare(RenderFrame& frame,RHI::ICommandList& uploads) override
    {
        m_prepared=m_recorded=false;
        if(!AdvancedModuleEnabled(frame.renderer)) return true;
        if(!frame.requirements.needsSurface)
        {
            if(m_lighting) m_lighting->Invalidate();
            return true;
        }
        const auto& config=frame.renderer.GetConfig();
        const auto& quality=config.advanced;
        ScreenSpaceLighting::Settings settings;
        settings.frustumCulling=config.enableFrustumCulling;
        settings.ambientOcclusion=config.lighting.enabled && quality.ambientOcclusion && !frame.lodDebug;
        settings.diffuseGI=config.lighting.enabled && quality.diffuseGI && !frame.lodDebug;
        settings.reflections=config.lighting.enabled && quality.reflections && !frame.lodDebug;
        settings.aoRadius=quality.aoRadius;settings.aoStrength=quality.aoStrength;
        settings.rayDistance=quality.rayDistance;settings.giStrength=quality.giStrength;
        settings.reflectionStrength=quality.reflectionStrength;settings.thickness=quality.rayThickness;
        settings.frameIndex=frame.lodDebug?0:frame.history.frameIndex;
        settings.aoFrameIndex=frame.temporalAA?frame.history.frameIndex:0;
        if(!m_lighting) m_lighting=std::make_unique<ScreenSpaceLighting>();
        if(!m_lighting->Prepare(frame.renderer.GetDevice(),uploads,frame.depth.texture,
            frame.camera,frame.geometry,settings)) return false;
        frame.ao.texture=m_lighting->GetAO();
        m_prepared=true;
        return true;
    }

    bool AddPasses(RenderStage stage,RenderFrame& frame,RHI::RenderGraph& graph) override
    {
        if(!AdvancedModuleEnabled(frame.renderer) || frame.outputWritten || !frame.requirements.needsSurface) return true;
        if(stage!=RenderStage::BeforeOpaque && stage!=RenderStage::AfterOpaque) return true;
        if(!m_prepared || !m_lighting) return AdvancedPassFailure("Surface lighting resources were not prepared.");
        if(stage==RenderStage::BeforeOpaque)
        {
            if(!m_lighting->Import(graph) || !m_lighting->AddSurface(graph,frame.depth.resource,
                frame.visibleInstances.resource,frame.indirectArguments.resource) ||
                !m_lighting->AddAO(graph)) return AdvancedPassFailure("Surface/AO pass creation failed.");
            const auto& surface=m_lighting->GetGraphResources();
            frame.normal={m_lighting->GetNormal(),surface.normal};
            frame.albedo={m_lighting->GetAlbedoMetal(),surface.albedo};
            frame.ao={m_lighting->GetAO(),surface.ao};
            frame.motion={m_lighting->GetMotion(),surface.motion};
            m_recorded=true;
            return true;
        }
        const auto& quality=frame.renderer.GetConfig().advanced;
        if(quality.surfaceDebugView!=SurfaceDebugView::None && !frame.wireframe)
        {
            const auto& projection=frame.camera.projection;
            const float farDepth=std::fabs(projection.m[11])>.5f
                ?projection.m[14]/(projection.m[10]+1):(projection.m[14]-1)/projection.m[10];
            if(!m_lighting->AddDebugView(graph,frame.output.texture,frame.output.resource,
                quality.surfaceDebugView,std::fabs(farDepth))) return AdvancedPassFailure("Surface debug pass creation failed.");
            frame.outputWritten=true;
            return true;
        }
        const auto& specular=frame.lighting.environment.specular;
        if(!m_lighting->AddLighting(graph,frame.color.resource,frame.color.texture,specular.resource,specular.texture))
            return AdvancedPassFailure("Screen lighting pass creation failed.");
        frame.color={m_lighting->GetColor(),m_lighting->GetGraphResources().color};
        return true;
    }

    void CommitFrame(const RenderFrame&) override
    {
        if(m_recorded && m_lighting) m_lighting->CommitFrame();
        m_recorded=false;
    }

    void InvalidateHistory(bool release) override
    {
        if(m_lighting) m_lighting->Invalidate();
        if(release) m_lighting.reset();
        m_prepared=m_recorded=false;
    }

private:
    std::unique_ptr<ScreenSpaceLighting> m_lighting;
    bool m_prepared=false,m_recorded=false;
};
}

std::unique_ptr<RenderExtension> CreateSurfaceLightingModule()
{
    return std::make_unique<SurfaceLightingModule>();
}
}
