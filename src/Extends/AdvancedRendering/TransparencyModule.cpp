#include "AdvancedRenderingInternal.h"
#include "TransparentRendering.h"
#include <memory>
#include <cstring>

namespace dyf::Detail
{
namespace
{
class TransparencyModule final : public RenderExtension
{
public:
    explicit TransparencyModule(std::shared_ptr<EnvironmentFilteringProof> proof):m_proof(std::move(proof)) {}
    const char* GetName() const override { return "Transparency"; }

    RenderRequirements GetRequirements(const Renderer& renderer) const override
    {
        RenderRequirements requirements;
        const auto& config=renderer.GetAppliedConfig();
        if(AdvancedModuleEnabled(renderer) && config.advanced.transparency && !config.debugWireframe)
        {
            requirements.needsGeometry=true;
            requirements.handlesTransparency=true;
        }
        return requirements;
    }

    bool Prepare(RenderFrame& frame,RHI::ICommandList& uploads) override
    {
        m_prepared=false;
        if(!GetRequirements(frame.renderer).handlesTransparency || frame.wireframe)
        {
            if(m_transparency) m_transparency->Invalidate();
            return true;
        }
        if(!m_transparency) m_transparency=std::make_unique<TransparentRendering>();
        const auto& config=frame.renderer.GetAppliedConfig();
        const auto& resources=frame.lighting.environment;
        const bool trusted=m_proof && m_proof->nativeFiltering && m_proof->frame==&frame &&
            m_proof->device==&frame.renderer.GetDevice() &&
            m_proof->resources.prefilter==resources.prefilter && m_proof->resources.probeData==resources.probeData &&
            m_proof->resources.probePrefilter==resources.probePrefilter &&
            m_proof->resources.probeParameters==resources.probeParameters &&
            m_proof->resources.constantBuffer==resources.constantBuffer &&
            std::memcmp(&m_proof->resources.constants,&resources.constants,sizeof(resources.constants))==0;
        m_prepared=m_transparency->Prepare(frame.renderer.GetDevice(),uploads,frame.camera,
            frame.geometry,config.lighting.environment,frame.lighting.environment,config.enableFrustumCulling,
            config.advanced.transparencyLayers,trusted);
        return m_prepared;
    }

    bool AddPasses(RenderStage stage,RenderFrame& frame,RHI::RenderGraph& graph) override
    {
        if(stage!=RenderStage::Transparency || frame.outputWritten || !m_prepared ||
            !m_transparency->HasDraws()) return true;
        if(!m_transparency->AddPasses(graph,frame.color.texture,frame.color.resource,
            frame.depth.texture,frame.depth.resource,frame.lighting.lights.buffer,
            frame.hasShadows?frame.shadow.texture:nullptr,
            frame.hasShadows?frame.shadow.resource:RHI::RGResourceHandle{},
            frame.hasShadows?frame.lighting.shadowConstants.buffer:nullptr,frame.lighting,frame.renderer))
            return AdvancedPassFailure("Transparent rendering pass creation failed.");
        frame.color={m_transparency->GetColor(),m_transparency->GetColorHandle()};
        frame.reactive={m_transparency->GetReactive(),m_transparency->GetReactiveHandle()};
        return true;
    }

    void CommitFrame(const RenderFrame&) override
    {
        if(!m_prepared) return;
        m_transparency->CommitFrame();
        m_prepared=false;
    }

    void InvalidateHistory(bool release) override
    {
        if(m_transparency) m_transparency->Invalidate();
        if(release) m_transparency.reset();
        m_prepared=false;
    }

private:
    std::shared_ptr<EnvironmentFilteringProof> m_proof;
    std::unique_ptr<TransparentRendering> m_transparency;
    bool m_prepared=false;
};
}

std::unique_ptr<RenderExtension> CreateTransparencyModule(std::shared_ptr<EnvironmentFilteringProof> proof)
{
    return std::make_unique<TransparencyModule>(std::move(proof));
}
}
