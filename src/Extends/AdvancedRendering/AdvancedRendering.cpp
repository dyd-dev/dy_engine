#include "dyf/Extends/AdvancedRendering.h"
#include "AdvancedRenderingInternal.h"

namespace dyf
{
std::unique_ptr<RenderExtension> CreateAdvancedRenderingModule(AdvancedRenderModule module)
{
    switch(module)
    {
    case AdvancedRenderModule::EnvironmentLighting: return Detail::CreateEnvironmentLightingModule();
    case AdvancedRenderModule::ClusteredLighting: return Detail::CreateClusteredLightingModule();
    case AdvancedRenderModule::SurfaceLighting: return Detail::CreateSurfaceLightingModule();
    case AdvancedRenderModule::VolumetricFog: return Detail::CreateFogModule();
    case AdvancedRenderModule::Transparency: return Detail::CreateTransparencyModule();
    case AdvancedRenderModule::PostProcess: return Detail::CreatePostProcessModule();
    }
    return nullptr;
}

namespace
{
std::array<std::unique_ptr<RenderExtension>,6> CreatePresetModules()
{
    auto proof=std::make_shared<Detail::EnvironmentFilteringProof>();
    return {Detail::CreateEnvironmentLightingModule(proof),Detail::CreateClusteredLightingModule(),
        Detail::CreateSurfaceLightingModule(),Detail::CreateFogModule(),Detail::CreateTransparencyModule(proof),
        Detail::CreatePostProcessModule()};
}
}

AdvancedRendering::AdvancedRendering():m_modules(CreatePresetModules()) {}

AdvancedRendering::~AdvancedRendering()=default;

RenderRequirements AdvancedRendering::GetRequirements(const Renderer& renderer) const
{
    RenderRequirements combined;
    for(const auto& module:m_modules)
    {
        const auto requirements=module->GetRequirements(renderer);
        combined.providesSurface|=requirements.providesSurface;
        combined.needsSurface|=requirements.needsSurface;
        combined.needsGeometry|=requirements.needsGeometry;
        combined.handlesTransparency|=requirements.handlesTransparency;
        combined.writesOutput|=requirements.writesOutput;
        combined.providesEnvironment|=requirements.providesEnvironment;
        combined.providesLocalLights|=requirements.providesLocalLights;
    }
    return combined;
}

bool AdvancedRendering::UsesSurfacePipeline(const Renderer& renderer) const
{
    return GetRequirements(renderer).providesSurface;
}

bool AdvancedRendering::NeedsSurface(const Renderer& renderer) const
{
    return GetRequirements(renderer).needsSurface;
}

bool AdvancedRendering::PrepareCamera(RenderFrame& frame)
{
    for(auto& module:m_modules) if(!module->PrepareCamera(frame)) return false;
    return true;
}

bool AdvancedRendering::Prepare(RenderFrame& frame,RHI::ICommandList& uploads)
{
    for(auto& module:m_modules) if(!module->Prepare(frame,uploads)) return false;
    return true;
}

void AdvancedRendering::CommitFrame(const RenderFrame& frame)
{
    for(auto& module:m_modules) module->CommitFrame(frame);
}

void AdvancedRendering::InvalidateHistory(bool release)
{
    for(auto& module:m_modules) module->InvalidateHistory(release);
}
}
