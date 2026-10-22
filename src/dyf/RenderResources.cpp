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
    {
        const bool previous=frame.opaqueDepthComplete;
        const auto oldDepth=frame.depth;
        const auto oldView=frame.camera.view,oldProjection=frame.camera.projection;
        depthPassRevisions.clear();
        if(previous || frameOpaqueDepthComplete)
            for(uint32_t i=0;const auto* pass=graph.GetPass(i);++i)depthPassRevisions.push_back(pass->GetRevision());
        frame.opaqueDepthComplete=false;
        if(!extension->AddPasses(stage,frame,graph))
        {
            std::fprintf(stderr,"dyf: Render module %s failed while adding stage %u.\n",
                extension->GetName(),static_cast<unsigned>(stage));
            return false;
        }
        const bool beforeMain=stage==RenderStage::BeforeOpaque || stage==RenderStage::OpaqueLighting;
        bool cpuChanged=!SameDepthCamera(oldView,oldProjection,frame.camera);
        bool changed=cpuChanged || oldDepth.texture!=frame.depth.texture || oldDepth.resource!=frame.depth.resource;
        if(previous || frameOpaqueDepthComplete)
        {
            for(uint32_t i=0;i<depthPassRevisions.size();++i)
            {
                const auto* pass=graph.GetPass(i);
                if(!pass || pass->GetRevision()!=depthPassRevisions[i])cpuChanged=changed=true;
            }
            for(uint32_t i=static_cast<uint32_t>(depthPassRevisions.size());const auto* pass=graph.GetPass(i);++i)
            {
                if(beforeMain && pass->HasGlobalBarrier())changed=true;
                for(const auto& write:pass->GetWrites())
                    if(write.handle==oldDepth.resource || std::binary_search(depthProtectedInputs.begin(),
                        depthProtectedInputs.end(),write.handle.id))changed=true;
            }
        }
        // New GPU passes registered after Main execute after it. Only CPU input
        // changes or edits of earlier passes can invalidate its captured prepass.
        if(cpuChanged || (beforeMain && changed))frameOpaqueDepthComplete=false;
        const bool explicitOptIn=frame.opaqueDepthComplete;
        if(!explicitOptIn)frame.opaqueDepthComplete=previous && !changed;
        if(explicitOptIn && beforeMain)
        {
            depthProtectedInputs.clear();
            const RHI::RenderGraphPass* writer=nullptr;
            for(uint32_t i=0;const auto* pass=graph.GetPass(i);++i)
                for(const auto& write:pass->GetWrites())if(write.handle==frame.depth.resource)writer=pass;
            if(writer)
            {
                for(const auto& read:writer->GetReads())depthProtectedInputs.push_back(read.handle.id);
                std::sort(depthProtectedInputs.begin(),depthProtectedInputs.end());
                depthProtectedInputs.erase(std::unique(depthProtectedInputs.begin(),depthProtectedInputs.end()),depthProtectedInputs.end());
            }
            else frame.opaqueDepthComplete=false;
        }
        if(depthGeometryUnchanged && (!depthGeometrySnapshot.Matches(frame.geometry) ||
            !SameDepthCamera(depthGeometryView,depthGeometryProjection,frame.camera)))depthGeometryUnchanged=false;
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
    const bool baseInstance=stockDrawInputs && UsesInstanceStream() && device->Supports(RHI::Feature::IndirectFirstInstance);
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
        const bool cpuBatch=cpuBatchesActive && !UsesBlendedRendering(scene.Materials()[materialIndex]);
        if(cpuBatch && cpuBatchIndices[i]==UINT32_MAX)continue;
        RenderGeometryDraw draw;
        const auto* input=frame.draws ? &(*frame.draws)[i] : nullptr;
        const auto level=std::min<uint32_t>(EntityLod(i),mesh.lods.size());
        draw.vertexBuffer=input && input->vertexBuffer ? input->vertexBuffer : mesh.vertexBuffer;
        draw.previousVertexBuffer=input ? input->previousVertexBuffer : nullptr;
        draw.deformationHistoryValid=!input || input->deformationHistoryValid;
        draw.indexBuffer=MainIndexBuffer(mesh,level,scene.Materials()[materialIndex],input);
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
        if(cpuBatch)
        {
            const auto& batch=cpuDraws[cpuBatchIndices[i]];
            draw.instanceBuffer=cpuInstanceBuffer;
            draw.instanceOffset=batch.instanceOffset*2*sizeof(Math::float4x4);
            draw.instanceCount=batch.instanceCount;
            frame.geometry.push_back(std::move(draw));
        }
        else if(visibilityActive && !UsesBlendedRendering(*draw.material))
        {
            while(visibilityIndex<visibilityDraws.size() && visibilityDraws[visibilityIndex].entity==i)
            {
                const auto batchIndex=visibilityIndex++;
                if(!gpuVisibility->MayDrawBatch(batchIndex))continue;
                const auto& selected=visibilityDraws[batchIndex];
                const auto& batch=gpuVisibility->GetBatch(batchIndex);
                auto indirect=draw;
                indirect.indexBuffer=MainIndexBuffer(mesh,selected.level,scene.Materials()[materialIndex],input);
                indirect.indexCount=batch.indexCount;
                indirect.instanceBuffer=gpuVisibility->GetInstanceBuffer();
                indirect.indirectArguments=gpuVisibility->GetArgumentsBuffer();
                // The shared indirect arguments already select the first row.
                indirect.instanceOffset=baseInstance?0:batch.instanceOffset*sizeof(GpuVisibility::VisibleInstance);
                indirect.indirectOffset=batchIndex*sizeof(RHI::DrawIndexedIndirectArguments);
                if(lodDebugActive)
                {
                    indirect.baseColor=LodColor(selected.level);
                    indirect.baseColor.w=draw.baseColor.w;
                }
                frame.geometry.push_back(std::move(indirect));
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
    else if(cpuBatchesActive)
        frame.visibleInstances={cpuInstanceBuffer,handle(frame.visibleInstances.resource)};
}
}
