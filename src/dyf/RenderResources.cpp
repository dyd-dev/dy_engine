#include "RendererInternal.h"
#include "dyf/RenderExtension.h"
#include "dyf/Camera.h"
#include "dyf/Scene.h"
#include "dyf/RHI/Indirect.h"
#include "GpuVisibility.h"
#include <algorithm>
#include <cstdio>

namespace dyf
{
RenderRequirements Renderer::Impl::CollectRequirements() const
{
    RenderRequirements result;
    for(const auto& extension:extensions)
    {
        const auto requirements=extension->GetRequirements(owner);
        result.providesSurface|=requirements.providesSurface;
        result.needsSurface|=requirements.needsSurface;
        result.needsGeometry|=requirements.needsGeometry;
        result.handlesTransparency|=requirements.handlesTransparency;
        result.writesOutput|=requirements.writesOutput;
        result.providesLocalLights|=requirements.providesLocalLights;
        result.providesEnvironment|=requirements.providesEnvironment;
    }
    if(UsesAdvancedRendering() && UsesGpuVisibility() && config.enableOcclusionCulling)
        result.needsSurface=true;
    result.needsGeometry|=result.needsSurface;
    return result;
}

bool Renderer::Impl::ValidateModules(const RenderRequirements& combined) const
{
    const char* providers[5]={};
    const char* names[]={"surface", "local lights", "environment", "transparency", "output"};
    for(const auto& extension:extensions)
    {
        const auto requirements=extension->GetRequirements(owner);
        const bool supplied[]={requirements.providesSurface,requirements.providesLocalLights,
            requirements.providesEnvironment,requirements.handlesTransparency,requirements.writesOutput};
        for(uint32_t i=0;i<5;++i)
        {
            if(!supplied[i]) continue;
            if(providers[i])
            {
                std::fprintf(stderr,"dyf: Render modules %s and %s both provide %s.\n",
                    providers[i],extension->GetName(),names[i]);
                return false;
            }
            providers[i]=extension->GetName();
        }
    }
    if(combined.needsSurface && !providers[0])
    {
        std::fputs("dyf: Render modules require a surface provider; register SurfaceLighting.\n",stderr);
        return false;
    }
    return true;
}

bool Renderer::Impl::AddExtensionPasses(RenderStage stage,RenderFrame& frame,RHI::RenderGraph& graph)
{
    for(auto& extension:extensions)
        if(!extension->AddPasses(stage,frame,graph))
        {
            std::fprintf(stderr,"dyf: Render module %s failed while adding stage %u.\n",
                extension->GetName(),static_cast<unsigned>(stage));
            return false;
        }
    frameLighting=frame.lighting;
    return true;
}

bool Renderer::Impl::UsesSeparateTransparency() const
{
    return UsesAdvancedRendering() && CollectRequirements().handlesTransparency;
}

bool Renderer::Impl::HasOutputExtension() const
{
    return CollectRequirements().writesOutput;
}

void Renderer::Impl::PrepareRenderGeometry(RenderFrame& frame) const
{
    const auto& scene=frame.scene;
    const auto& camera=frame.camera;
    frame.geometry.clear();
    if(!frame.requirements.needsGeometry && !frame.requirements.needsSurface) return;
    frame.geometry.reserve(scene.GetEntityCount());
    const auto viewProjection=camera.projection*camera.view;
    uint32_t visibilityIndex=0;
    for(uint32_t i=0;i<scene.GetEntityCount();++i)
    {
        const auto entity=static_cast<EntityID>(i);
        const auto meshId=scene.GetEntityMesh(entity);
        const auto materialId=scene.GetEntityMaterial(entity);
        if(!IsValid(meshId) || !IsValid(materialId)) continue;
        const auto meshIndex=ToIndex(meshId),materialIndex=ToIndex(materialId);
        if(meshIndex>=m_meshes.size() || materialIndex>=materialStates.size()) continue;
        const auto& mesh=m_meshes[meshIndex];
        if(!mesh.prepared || !mesh.vertexBuffer || !mesh.indexBuffer || !mesh.indexCount) continue;
        RenderGeometryDraw draw;
        const auto* input=frame.draws ? &(*frame.draws)[i] : nullptr;
        const auto level=std::min<uint32_t>(EntityLod(i),mesh.lods.size());
        draw.vertexBuffer=input && input->vertexBuffer ? input->vertexBuffer : mesh.vertexBuffer;
        draw.previousVertexBuffer=input ? input->previousVertexBuffer : nullptr;
        draw.deformationHistoryValid=!input || input->deformationHistoryValid;
        draw.indexBuffer=level ? mesh.lods[level-1].buffer : mesh.indexBuffer;
        draw.indexCount=level ? mesh.lods[level-1].count : mesh.indexCount;
        draw.localBounds=input && input->hasLocalBounds ? input->localBounds : mesh.bounds;
        draw.model=scene.GetTransform(entity);
        draw.previousMVP=frameHistoryValid && i<previousModels.size()
            ? previousViewProjection*previousModels[i] : viewProjection*draw.model;
        draw.material=&scene.Materials()[materialIndex];
        draw.baseColor=draw.material->baseColor;
        if(lodDebugActive)
        {
            const auto alpha=draw.baseColor.w;
            draw.baseColor=LodColor(level);draw.baseColor.w=alpha;
        }
        draw.textures=materialStates[materialIndex].textures;
        draw.textureFlags=materialStates[materialIndex].textureFlags |
            (scene.GetEntityLighting(entity).receiveShadow ? 32u : 0u) | (lodDebugActive ? 64u : 0u);
        if(visibilityActive && draw.material->alphaMode!=MaterialAlphaMode::Blend)
        {
            while(visibilityIndex<visibilityDraws.size() && visibilityDraws[visibilityIndex].entity==i)
            {
                const auto& selected=visibilityDraws[visibilityIndex];
                const auto& batch=gpuVisibility->GetBatch(visibilityIndex);
                auto indirect=draw;
                indirect.indexBuffer=selected.level ? mesh.lods[selected.level-1].buffer : mesh.indexBuffer;
                indirect.indexCount=batch.indexCount;
                indirect.instanceBuffer=gpuVisibility->GetInstanceBuffer();
                indirect.indirectArguments=gpuVisibility->GetArgumentsBuffer();
                indirect.instanceOffset=batch.instanceOffset*sizeof(GpuVisibility::VisibleInstance);
                indirect.indirectOffset=visibilityIndex*sizeof(RHI::DrawIndexedIndirectArguments);
                if(lodDebugActive)
                {
                    indirect.baseColor=LodColor(selected.level);
                    indirect.baseColor.w=draw.baseColor.w;
                }
                frame.geometry.push_back(std::move(indirect));++visibilityIndex;
            }
        }
        else frame.geometry.push_back(std::move(draw));
    }
}

void Renderer::Impl::UpdateFrameLighting(RenderFrame& frame,bool graphReady) const
{
    const auto handle=[&](RHI::RGResourceHandle resource) {
        return graphReady ? resource : RHI::RGResourceHandle{};
    };
    auto& lighting=frame.lighting;
    lighting.lights.buffer=lightingBuffer;
    lighting.shadowConstants.buffer=shadowMatrixBuffer;
    frame.gpuVisibility=visibilityActive;
    if(visibilityActive)
    {
        frame.visibleInstances={gpuVisibility->GetInstanceBuffer(),handle(gpuVisibility->GetInstancesHandle())};
        frame.indirectArguments={gpuVisibility->GetArgumentsBuffer(),handle(gpuVisibility->GetArgumentsHandle())};
    }
}
}
