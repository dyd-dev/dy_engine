#include "AdvancedRenderingInternal.h"
#include "VolumetricFog.h"
#include "dyf/RHI/Texture.h"
#include <memory>

namespace dyf::Detail
{
namespace
{
class FogModule final : public RenderExtension
{
public:
    const char* GetName() const override { return "VolumetricFog"; }

    RenderRequirements GetRequirements(const Renderer&) const override { return {}; }

    bool Prepare(RenderFrame& frame,RHI::ICommandList& uploads) override
    {
        m_prepared=false;
        const auto& config=frame.renderer.GetAppliedConfig();
        if(!AdvancedModuleEnabled(frame.renderer) || !config.advanced.fog.enabled ||
            !config.lighting.enabled || frame.lodDebug) return true;
        if(!m_fog) m_fog=std::make_unique<VolumetricFog>();
        const auto& source=config.advanced.fog;
        VolumetricFog::Settings settings;
        settings.enabled=true;
        settings.density=source.density;
        settings.heightFalloff=source.heightFalloff;
        settings.baseHeight=source.baseHeight;
        settings.albedo=source.albedo;
        settings.anisotropy=source.anisotropy;
        settings.maxDistance=source.maxDistance;
        settings.resolutionScale=source.resolutionScale;
        settings.steps=source.steps;
        const auto& target=frame.color.texture->GetDesc();
        m_prepared=m_fog->Prepare(frame.renderer.GetDevice(),uploads,frame.camera,
            target.width,target.height,settings);
        return m_prepared;
    }

    bool AddPasses(RenderStage stage,RenderFrame& frame,RHI::RenderGraph& graph) override
    {
        if(stage!=RenderStage::Atmosphere || frame.outputWritten || !m_prepared) return true;
        VolumetricFog::LightingResources resources;
        resources.lights=frame.lighting.lights.buffer;
        resources.lightsHandle=frame.lighting.lights.resource;
        if(frame.hasShadows)
        {
            resources.shadows=frame.lighting.shadowConstants.buffer;
            resources.shadowsHandle=frame.lighting.shadowConstants.resource;
            resources.shadowDepth=frame.shadow.texture;
            resources.shadowDepthHandle=frame.shadow.resource;
        }
        if(!m_fog->AddPasses(graph,frame.color.texture,frame.color.resource,
            frame.depth.texture,frame.depth.resource,resources))
            return AdvancedPassFailure("Volumetric fog pass creation failed.");
        frame.color={m_fog->GetColor(),m_fog->GetHandle()};
        return true;
    }

    void CommitFrame(const RenderFrame&) override
    {
        if(!m_prepared) return;
        m_fog->CommitUploads();
        m_fog->CommitFrame();
        m_prepared=false;
    }

    void InvalidateHistory(bool release) override
    {
        if(release)
        {
            m_fog.reset();
            m_prepared=false;
        }
    }

private:
    std::unique_ptr<VolumetricFog> m_fog;
    bool m_prepared=false;
};
}

std::unique_ptr<RenderExtension> CreateFogModule()
{
    return std::make_unique<FogModule>();
}
}
