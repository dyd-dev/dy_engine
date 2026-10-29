#include "AdvancedRenderingInternal.h"
#include "TemporalPostProcess.h"
#include "dyf/RHI/Texture.h"

namespace dyf::Detail
{
namespace
{
class PostProcessModule final : public RenderExtension
{
public:
    const char* GetName() const override { return "PostProcess"; }

    RenderRequirements GetRequirements(const Renderer& renderer) const override
    {
        RenderRequirements requirements;
        if(!AdvancedModuleEnabled(renderer)) return requirements;
        const auto& config=renderer.GetConfig();
        const bool diagnostic=config.debugLodColor || config.debugWireframe;
        requirements.needsMotion=config.advanced.temporalAA &&
            config.advanced.surfaceDebugView==SurfaceDebugView::None && !diagnostic;
        requirements.needsGeometry=requirements.RequiresDepth();
        requirements.writesOutput=config.advanced.surfaceDebugView==SurfaceDebugView::None || config.debugWireframe;
        return requirements;
    }

    bool PrepareCamera(RenderFrame& frame) override
    {
        m_recorded=false;
        if(!AdvancedModuleEnabled(frame.renderer)) return true;
        const auto& config=frame.renderer.GetConfig();
        const auto& quality=config.advanced;
        TemporalPostProcess::Settings settings;
        settings.output=config.postProcess;
        settings.debugColor=frame.lodDebug;
        settings.taa=quality.temporalAA && quality.surfaceDebugView==SurfaceDebugView::None && !frame.lodDebug;
        settings.bloom=quality.bloom && !frame.lodDebug;
        settings.autoExposure=quality.autoExposure && quality.surfaceDebugView==SurfaceDebugView::None && !frame.lodDebug;
        settings.exposureKey=quality.exposureKey;settings.exposureAdaptationSpeed=quality.exposureAdaptationSpeed;
        settings.bloomStrength=quality.bloomStrength;settings.bloomThreshold=quality.bloomThreshold;
        settings.bloomKnee=quality.bloomKnee;settings.historyWeight=quality.historyWeight;
        if(!m_post) m_post=std::make_unique<TemporalPostProcess>(frame.renderer.GetDevice());
        const auto& target=frame.color.texture->GetDesc();
        if(!m_post->Prepare(target.width,target.height,frame.output.texture->GetDesc().format,settings)) return false;
        m_post->SetSkyReprojection(frame.camera,frame.history.previousCameraView,
            frame.history.previousCameraProjection,frame.history.valid);
        TemporalPostProcess::ApplyJitter(frame.camera,m_post->JitterPixels(),target.width,target.height);
        frame.temporalAA=settings.taa;
        return true;
    }

    bool AddPasses(RenderStage stage,RenderFrame& frame,RHI::RenderGraph& graph) override
    {
        if(stage!=RenderStage::PostProcess || frame.outputWritten || !AdvancedModuleEnabled(frame.renderer)) return true;
        if(!m_post) return AdvancedPassFailure("Post-processing resources were not prepared.");
        auto reactive=frame.reactive;
        if(!reactive.texture && frame.blackTexture)
        {
            reactive.texture=frame.blackTexture;
            reactive.resource=graph.ImportTexture("TAA no transparency",reactive.texture,
                RHI::ResourceState::ShaderResource,RHI::ResourceState::ShaderResource);
            if(!reactive.resource.IsValid()) return AdvancedPassFailure("TAA reactive resource import failed.");
        }
        if(!m_post->AddPasses(graph,frame.color.texture,frame.color.resource,frame.motion.texture,frame.motion.resource,
            frame.output.texture,frame.output.resource,frame.renderer.GetConfig().exposure,reactive.texture,reactive.resource))
            return AdvancedPassFailure("Temporal post-processing pass creation failed.");
        frame.outputWritten=true;m_recorded=true;
        return true;
    }

    void CommitFrame(const RenderFrame&) override
    {
        if(m_recorded && m_post) m_post->CommitFrame();
        m_recorded=false;
    }

    void InvalidateHistory(bool release) override
    {
        if(m_post) m_post->InvalidateHistory();
        if(release) m_post.reset();
        m_recorded=false;
    }

private:
    std::unique_ptr<TemporalPostProcess> m_post;
    bool m_recorded=false;
};
}

std::unique_ptr<RenderExtension> CreatePostProcessModule()
{
    return std::make_unique<PostProcessModule>();
}
}
