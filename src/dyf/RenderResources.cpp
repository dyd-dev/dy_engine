#include "RendererInternal.h"
#include "SceneAlgorithms.h"
#include "dyf/RenderExtension.h"
#include "dyf/Camera.h"
#include "dyf/Scene.h"
#include "dyf/RHI/Indirect.h"
#include "GpuVisibility.h"
#include <algorithm>
#include <cstdio>

namespace dyf
{
// 각 모듈이 공급하거나 필요로 하는 기능을 OR로 합친다. 가시성 차폐 제거가 요청되면 표면 깊이도 필요하다고 추가한다.
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
    if(UsesAdvancedRendering() && UsesGpuVisibility() && config.enableOcclusionCulling) // 고급 GPU 차폐 제거에는 surface 깊이가 필요하므로 요구를 추가한다.
        result.needsSurface=true;
    result.needsGeometry|=result.needsSurface;
    return result;
}

// surface/지역광/환경/투명/최종 출력 공급자를 기능마다 하나로 제한한다. 서로 다른 모듈이 같은 기능을 공급하거나 필수 surface가 없으면 명시적으로 실패한다.
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
            if(!supplied[i]) continue; // 현재 확장이 공급하지 않는 기능은 중복 공급 검사에서 건너뛴다.
            if(providers[i]) // 이미 공급자가 있는 기능을 또 공급하면 두 모듈 이름을 보고하고 실패한다.
            {
                std::fprintf(stderr,"dyf: Render modules %s and %s both provide %s.\n",
                    providers[i],extension->GetName(),names[i]);
                return false;
            }
            providers[i]=extension->GetName();
        }
    }
    if(combined.needsSurface && !providers[0]) // 표면 자원을 요구하면서 공급 모듈이 없으면 프레임을 시작하지 않는다.
    {
        std::fputs("dyf: Render modules require a surface provider; register SurfaceLighting.\n",stderr);
        return false;
    }
    return true;
}

// 각 확장에 같은 stage를 등록 순서대로 전달하여 그래프에 패스를 추가하게 한다. 하나라도 실패하면 중단하고, 모두 성공했을 때만 frame.lighting을 Impl에 복사한다. 이 함수는 추가한 패스를 GPU에 제출하지 않는다.
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
        if(!extension->AddPasses(stage,frame,graph)) // 확장이 이 단계의 패스를 추가하지 못하면 모듈 이름/단계와 함께 실패를 전파한다.
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

// 고급 경로이며 실제로 투명 처리를 맡는 모듈이 있는지 함께 확인한다. 이때 MainForward는 Blend를 중복으로 그리지 않는다.
bool Renderer::Impl::UsesSeparateTransparency() const
{
    return UsesAdvancedRendering() && CollectRequirements().handlesTransparency;
}

// 최종 출력을 쓰는 모듈이 등록되었는지 요구 계약으로 조회한다.
bool Renderer::Impl::HasOutputExtension() const
{
    return CollectRequirements().writesOutput;
}

// 표면/효과 확장이 공통으로 사용할 draw 목록을 만든다. 선택 LOD·현재/이전 정점·모델·모션·재질·경계를 한 항목에 연결한다.
void Renderer::Impl::PrepareRenderGeometry(RenderFrame& frame) const
{
    const auto& scene=frame.scene;
    const auto& camera=frame.camera;
    frame.geometry.clear();
    if(!frame.requirements.needsGeometry && !frame.requirements.needsSurface) return; // 지오메트리와 surface를 모두 필요로 하지 않으면 공유 draw 목록을 만들지 않는다.
    frame.geometry.reserve(scene.GetEntityCount());
    const auto viewProjection=camera.projection*camera.view;
    const bool baseInstance=stockDrawInputs && UsesInstanceStream() && device->Supports(RHI::Feature::IndirectFirstInstance);
    uint32_t visibilityIndex=0;
    for(uint32_t i=0;i<scene.GetEntityCount();++i)
    {
        const auto entity=static_cast<EntityID>(i);
        const auto meshId=scene.GetEntityMesh(entity);
        const auto materialId=scene.GetEntityMaterial(entity);
        if(!IsValid(meshId) || !IsValid(materialId)) continue; // 메시/재질 ID가 없는 객체는 생략한다.
        const auto meshIndex=ToIndex(meshId),materialIndex=ToIndex(materialId);
        if(meshIndex>=m_meshes.size() || materialIndex>=materialStates.size()) continue; // 캐시/재질 범위를 벗어난 ID는 접근하지 않는다.
        const auto& mesh=m_meshes[meshIndex];
        if(!mesh.prepared || !mesh.vertexBuffer || !mesh.indexBuffer || !mesh.indexCount) continue; // GPU 버퍼와 인덱스가 준비된 메시만 확장에 전달한다.
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
        // 확정된 이전 모델 이력이 있으면 이전 view-projection과 곱하고, 없으면 현재 MVP로 시작한다. 변형 이력 유효성은 별도 플래그로 전달한다.
        draw.previousMVP=frameHistoryValid && i<previousModels.size()
            ? previousViewProjection*previousModels[i] : viewProjection*draw.model;
        draw.material=&scene.Materials()[materialIndex];
        draw.baseColor=draw.material->baseColor;
        if(lodDebugActive) // LOD 진단 중이면 원래 알파를 보존하면서 단계별 색으로 바꾼다.
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
        else if(visibilityActive && !UsesBlendedRendering(*draw.material)) // GPU 가시성이 활성인 불투명/마스크 재질은 간접 draw 목록으로 전달한다.
        {
            // GPU 배치의 대표 객체에서 LOD별 간접 draw 항목을 펼친다. 같은 그룹의 다른 객체는 GPU 인스턴스 버퍼에 포함되므로 별도 직접 draw를 중복 추가하지 않는다.
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
                if(lodDebugActive) // 간접 draw도 해당 GPU LOD 단계의 진단색을 사용한다.
                {
                    indirect.baseColor=LodColor(selected.level);
                    indirect.baseColor.w=draw.baseColor.w;
                }
                frame.geometry.push_back(std::move(indirect));
            }
        }
        else frame.geometry.push_back(std::move(draw)); // GPU 불투명 간접 경로가 아니면 준비한 직접 draw 하나를 그대로 넣는다.
    }
}

// 조명·그림자 상수 버퍼와 GPU 가시성 출력을 RenderFrame에 연결한다. graphReady=false이면 가시 인스턴스/간접 인수의 그래프 핸들은 비우고 실제 버퍼 핸들만 전달하여 이전 그래프의 식별자를 섞지 않는다.
void Renderer::Impl::UpdateFrameLighting(RenderFrame& frame,bool graphReady) const
{
    const auto handle=[&](RHI::RGResourceHandle resource) {
        return graphReady ? resource : RHI::RGResourceHandle{};
    };
    auto& lighting=frame.lighting;
    lighting.lights.buffer=lightingBuffer;
    lighting.shadowConstants.buffer=shadowMatrixBuffer;
    frame.gpuVisibility=visibilityActive;
    if(visibilityActive) // GPU 가시성 경로를 사용하는 프레임만 가시 인스턴스/간접 인수 연결을 제공한다.
    {
        frame.visibleInstances={gpuVisibility->GetInstanceBuffer(),handle(gpuVisibility->GetInstancesHandle())};
        frame.indirectArguments={gpuVisibility->GetArgumentsBuffer(),handle(gpuVisibility->GetArgumentsHandle())};
    }
    else if(cpuBatchesActive)
        frame.visibleInstances={cpuInstanceBuffer,handle(frame.visibleInstances.resource)};
}
}
