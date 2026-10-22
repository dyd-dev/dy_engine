#include "../Platform/Profiler/CollectorInternal.h"
#include "RendererInternal.h"
#include "DrawCommands.h"
#include "GpuVisibility.h"
#include "SceneAlgorithms.h"
#include "dyf/Camera.h"
#include "dyf/Scene.h"
#include "dyf/RHI/Buffer.h"
#include "dyf/RHI/Texture.h"
#include "../RHI/ResourceSet.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace dyf
{
bool Renderer::Impl::RecordMainPass(const Scene& scene, const Camera& camera,
	RHI::ICommandList& commands, const std::vector<RendererDrawDesc>* draws,
	RHI::TextureHandle output)
{
#if defined(DY_LIVE_PROFILING_ENABLED) || defined(DY_PROFILER_TRACING_ENABLED)
    Platform::CpuScope cpuScope(Platform::LiveStage::Record,"RendererMainForward");
#endif
    Platform::FrameProfiler::GpuScope liveGpu(m_profiler,commands,Platform::LiveGpuScope::RendererMainForward,liveId);
	auto& materialSets=mainSetScratch;
    struct MaterialBufferTransaction {
        Impl& r;
        RHI::BufferHandle previous;
        bool ready,committed=false;
        explicit MaterialBufferTransaction(Impl& owner):r(owner),previous(r.materialIndexBuffer),ready(r.materialIndexReady) {}
        ~MaterialBufferTransaction() {
            if(committed) {if(previous!=r.materialIndexBuffer)r.device->DestroyBuffer(previous);}
            else {
                if(previous!=r.materialIndexBuffer)r.device->DestroyBuffer(r.materialIndexBuffer);
                r.materialIndexBuffer=previous;r.materialIndexReady=ready;
            }
        }
    } materialTransaction(*this);
	if(!CreateMaterialResourceSets(scene, commands, draws, materialSets)) return false;
    materialTransaction.committed=true;
	auto* target = config.enableHdrRendering ? hdrTarget : output;
	const auto viewProjection = camera.projection * camera.view;
	const CameraFrustum frustum(camera);
#ifdef DY_RENDERDOC_ENABLED
	commands.BeginDebugEvent("MainForward");
#endif

	RHI::ColorAttachment color;
	color.texture = target;
	color.loadOp = RHI::LoadOp::Clear;
	color.storeOp = RHI::StoreOp::Store;
	color.clearColor[0] = config.clearColor.x;
	color.clearColor[1] = config.clearColor.y;
	color.clearColor[2] = config.clearColor.z;
	color.clearColor[3] = config.clearColor.w;
    // clearColor는 창·Canvas와 같은 디스플레이 색이다.
    // HDR 저장 전에 출력 변환과 Reinhard·노출을 되돌려 배경과 경계 밝기를 유지한다.
    if(config.enableHdrRendering)
    {
        for(uint32_t channel=0;channel<3;++channel)
        {
            const double value=std::clamp(static_cast<double>(color.clearColor[channel]),0.0,1.0);
            const double linear=RHI::IsSrgbFormat(output->GetDesc().format)
                ? (value<=0.04045 ? value/12.92 : std::pow((value+0.055)/1.055,2.4))
                : std::pow(value,2.2);
            color.clearColor[channel]=lodDebugActive ? static_cast<float>(linear) : config.exposure<=0 ? 0.0f : static_cast<float>(
                linear>=1.0 ? 65504.0 : std::min(linear/((1.0-linear)*config.exposure),65504.0));
        }
    }
    else if(RHI::IsSrgbFormat(target->GetDesc().format))
    {
        for(uint32_t channel=0;channel<3;++channel)
        {
            const float value=color.clearColor[channel];
            color.clearColor[channel]=value<=0.04045f ? value/12.92f : std::pow((value+0.055f)/1.055f,2.4f);
        }
    }
	RHI::DepthStencilAttachment depth;
	depth.texture = depthStencilTarget;
	depth.state = RHI::ResourceState::DepthWrite;
	depth.depthLoadOp = frameOpaqueDepthComplete || NeedsSurfacePass() ? RHI::LoadOp::Load : RHI::LoadOp::Clear;
	depth.depthStoreOp = UsesAdvancedRendering() || m_environmentCapture ? RHI::StoreOp::Store : RHI::StoreOp::Discard;
	if(RHI::HasStencil(depthStencilTarget->GetDesc().format))
	{
		depth.stencilLoadOp = RHI::LoadOp::Discard;
		depth.stencilStoreOp = RHI::StoreOp::Discard;
	}
	commands.BeginRendering({&color, 1, &depth});
	commands.BindGraphicsPipeline(pipeline);
	commands.SetViewport({0, 0, static_cast<float>(target->GetDesc().width), static_cast<float>(target->GetDesc().height), 0, 1});
	commands.SetScissor({0, 0, target->GetDesc().width, target->GetDesc().height});
    const bool stock=stockDrawInputs;
    const bool instanced=UsesInstanceStream();
	const bool batchBindings = shaderSources.bytes[MeshFragment].empty();
	const bool separateTransparency=m_environmentCapture || wireframeActive || UsesSeparateTransparency();
	const auto blended=[&](uint32_t index) {
		const auto material=scene.GetEntityMaterial(static_cast<EntityID>(index));
		return IsValid(material) && ToIndex(material)<scene.Materials().size() &&
			scene.Materials()[ToIndex(material)].alphaMode==MaterialAlphaMode::Blend;
	};
	mainEntityOrder.clear();
	mainEntityOrder.reserve(scene.GetEntityCount());
	for(uint32_t i=0;i<scene.GetEntityCount();++i)
		if((!separateTransparency || !blended(i)) && ((!visibilityActive && !cpuBatchesActive) || blended(i))) mainEntityOrder.push_back(i);
	if(batchBindings && !separateTransparency)
	{
		const auto transparent=std::stable_partition(mainEntityOrder.begin(),mainEntityOrder.end(),[&](uint32_t i) {return !blended(i);});
		const auto distance=[&](uint32_t index) {
			const auto entity=static_cast<EntityID>(index);
			const auto mesh=scene.GetEntityMesh(entity);
			Math::float3 local={};
			if(IsValid(mesh) && ToIndex(mesh)<m_meshes.size() && m_meshes[ToIndex(mesh)].bounds.valid)
			{
				const auto& bounds=m_meshes[ToIndex(mesh)].bounds;
				local=(bounds.min+bounds.max)*0.5f;
			}
			const auto center=Math::TransformPoint(scene.GetTransform(entity),local);
			const float z=Math::TransformPoint(camera.view,center).z;
			return std::isfinite(z) ? z : 0.0f;
		};
		std::stable_sort(transparent,mainEntityOrder.end(),[&](uint32_t a,uint32_t b) {return distance(a)<distance(b);});
	}
	RHI::ResourceSetHandle boundMaterial = nullptr;
	RHI::PipelineHandle boundPipeline=pipeline;
	if(visibilityActive || cpuBatchesActive)
	{
		recordingStatistics.gpuVisibility=visibilityActive;
        if(cpuBatchesActive)recordingStatistics.culledEntityCount+=cpuCulledEntities;
        const auto& batches=visibilityActive ? visibilityDraws : cpuDraws;
		for(uint32_t batchIndex=0;batchIndex<batches.size();++batchIndex)
		{
			if(visibilityActive && !gpuVisibility->MayDrawBatch(batchIndex))continue;
			const auto& draw=batches[batchIndex];
			const auto entity=static_cast<EntityID>(draw.entity);
			const auto& mesh=m_meshes[draw.mesh];
			const auto& material=scene.Materials()[draw.material];
			const auto* input=draws ? &(*draws)[draw.entity] : nullptr;
            auto batchContext=Platform::GetLiveContext();
            if(Platform::IsLiveCollectorEnabled())
            {
                const auto sceneBatch=Platform::GetLiveBatchId(liveId,scene.m_liveId);
                const auto passBatch=Platform::GetLiveBatchId(sceneBatch,1);
                batchContext.batch=Platform::GetLiveBatchId(passBatch,(uint64_t(1)<<63)|batchIndex);
                Platform::LiveEvent batch;batch.id=batchContext.batch;batch.context=batchContext;
                batch.kind=Platform::LiveEventKind::Batch;batch.stage=Platform::LiveStage::Record;
                if(visibilityActive)
                {
                    batch.first=batchIndex;batch.stride=sizeof(RHI::DrawIndexedIndirectArguments);
                    batch.bytes=batch.stride;batch.operations=1;
                    batch.resource=Platform::FindLiveObject(gpuVisibility->GetArgumentsBuffer());
                    // GPU compaction determines the visible instance count; CPU capacity is not a measurement.
                    std::strncpy(batch.label,"GPU indirect batch: visible count unknown",sizeof(batch.label)-1);
                    Platform::RecordLiveLink({Platform::FindLiveObject(gpuVisibility->GetInstanceBuffer()),batch.id});
                }
                else
                {
                    batch.first=draw.instanceOffset;batch.elements=draw.instanceCount;
                    batch.stride=2*sizeof(Math::float4x4);batch.bytes=uint64_t(batch.elements)*batch.stride;batch.operations=1;
                    batch.resource=Platform::FindLiveObject(cpuInstanceBuffer);
                    std::strncpy(batch.label,"CPU compacted instance batch",sizeof(batch.label)-1);
                    for(uint32_t i=0;i<draw.instanceCount;++i)
                    {
                        const auto source=cpuInstanceEntities[draw.instanceOffset+i];
                        if(source<scene.m_liveEntities.size())Platform::RecordLiveLink({scene.m_liveEntities[source],batch.id});
                    }
                }
                Platform::RecordLiveEvent(batch);
            }
            Platform::LiveContextScope linkedBatch(batchContext);
			const auto selectedPipeline=MaterialPipeline(material,Math::Determinant3x3(scene.GetTransform(entity))<0);
			if(selectedPipeline!=boundPipeline)
			{
				commands.BindGraphicsPipeline(selectedPipeline);boundPipeline=selectedPipeline;boundMaterial=nullptr;
			}
			auto selectedSet=materialSets[draw.material];
			if(selectedSet && selectedSet->GetPipeline()!=selectedPipeline)
			{
				mainBindingScratch.assign(selectedSet->GetBindings(),selectedSet->GetBindings()+selectedSet->GetBindingCount());
				selectedSet=CachedResourceSet(selectedPipeline,mainBindingScratch,mainSets);
			}
			if(!selectedSet) return false;
			if(boundMaterial!=selectedSet)
			{
				boundMaterial=selectedSet;commands.BindResourceSet(boundMaterial);
			}
			commands.BindVertexBuffer(0,input && input->vertexBuffer ? input->vertexBuffer : mesh.vertexBuffer,0);
			commands.BindVertexBuffer(1,visibilityActive ? gpuVisibility->GetInstanceBuffer() : cpuInstanceBuffer,
				(visibilityActive ? gpuVisibility->GetBatch(batchIndex).instanceOffset : draw.instanceOffset)*sizeof(GpuVisibility::VisibleInstance));
			commands.BindIndexBuffer(MainIndexBuffer(mesh,draw.level,material,input),RHI::Format::R32_UINT,0);
			const uint32_t flags=materialStates[draw.material].textureFlags |
				(scene.GetEntityLighting(entity).receiveShadow ? 32u : 0u);
			auto constants=MakeDrawConstants(viewProjection,material,scene.GetTransform(entity),flags);
			if(lodDebugActive)
			{
				const float alpha=constants.baseColor.w;
				constants.baseColor=LodColor(draw.level);constants.baseColor.w=alpha;constants.textureFlags|=64u;
			}
			if(m_environmentCapture && !m_environmentCaptureEmitters)constants.emissiveColor={};
			constants.padding2=draw.material;
			commands.SetInlineConstants(0,sizeof(constants),&constants);
            if(visibilityActive)
            {
                commands.DrawIndexedIndirect(gpuVisibility->GetArgumentsBuffer(),batchIndex*sizeof(RHI::DrawIndexedIndirectArguments));
                ++recordingStatistics.indirectBatchCount;
            }
            else
            {
                const auto indexCount=draw.level ? mesh.lods[draw.level-1].count : mesh.indexCount;
                commands.DrawIndexedInstanced(indexCount,draw.instanceCount,0,0,0);
                recordingStatistics.mainInstanceCount+=draw.instanceCount;
                recordingStatistics.mainTriangleCount+=uint64_t(indexCount/3)*draw.instanceCount;
                recordingStatistics.mainOriginalTriangleCount+=uint64_t(mesh.indexCount/3)*draw.instanceCount;
                recordingStatistics.lodInstanceCounts[draw.level]+=draw.instanceCount;
            }
			++recordingStatistics.mainDrawCount;
		}
	}
	for(uint32_t orderIndex = 0; orderIndex < mainEntityOrder.size(); ++orderIndex)
	{
		const uint32_t entityIndex=mainEntityOrder[orderIndex];
		const auto entity = static_cast<EntityID>(entityIndex);
		const auto meshId = scene.GetEntityMesh(entity);
		const auto materialId = scene.GetEntityMaterial(entity);
		if(!IsValid(meshId) || !IsValid(materialId)) continue;
		const uint32_t meshIndex = ToIndex(meshId), materialIndex = ToIndex(materialId);
		const uint32_t setIndex = draws && !stockDrawInputs ? entityIndex : materialIndex;
		if(meshIndex >= m_meshes.size() || materialIndex >= materialStates.size() || setIndex >= materialSets.size()) continue;
		const auto& mesh = m_meshes[meshIndex];
		if(!mesh.prepared || !mesh.vertexBuffer || !mesh.indexBuffer || !mesh.indexCount) continue;
        const auto level=std::min<uint32_t>(EntityLod(entityIndex),mesh.lods.size());
        const auto* input=draws ? &(*draws)[entityIndex] : nullptr;
        auto indexBuffer=MainIndexBuffer(mesh,level,scene.Materials()[materialIndex],input);
        const auto indexCount=level ? mesh.lods[level-1].count : mesh.indexCount;
        const auto& localBounds=draws && (*draws)[entityIndex].hasLocalBounds ? (*draws)[entityIndex].localBounds : mesh.bounds;
        if(stock && config.enableFrustumCulling && frustum.Outside(localBounds,scene.GetTransform(entity))) {
            ++recordingStatistics.culledEntityCount;continue;
        }
        const bool mirrored=Math::Determinant3x3(scene.GetTransform(entity))<0;
        uint32_t count=1;
        if(stock && instanced && (!batchBindings || scene.Materials()[materialIndex].alphaMode!=MaterialAlphaMode::Blend)) {
            while(orderIndex+count<mainEntityOrder.size() && mainEntityOrder[orderIndex+count]==entityIndex+count) {
                const auto next=static_cast<EntityID>(entityIndex+count);
                if((Math::Determinant3x3(scene.GetTransform(next))<0)!=mirrored) break;
                if(draws && (*draws)[entityIndex+count].vertexBuffer!=(*draws)[entityIndex].vertexBuffer) break;
                if(EntityLod(entityIndex+count)!=level) break;
                const auto nextMaterial=scene.GetEntityMaterial(next);
                if(scene.GetEntityMesh(next)!=meshId || !IsValid(nextMaterial) || ToIndex(nextMaterial)>=materialStates.size())break;
                const auto index=ToIndex(nextMaterial);
                if(scene.GetEntityLighting(next).receiveShadow!=scene.GetEntityLighting(entity).receiveShadow ||
                    materialStates[index].textures!=materialStates[materialIndex].textures ||
                    materialStates[index].textureFlags!=materialStates[materialIndex].textureFlags ||
                    !EqualMaterial(scene.Materials()[index],scene.Materials()[materialIndex]) ||
                    (config.enableFrustumCulling && frustum.Outside(draws && (*draws)[entityIndex+count].hasLocalBounds ?
                        (*draws)[entityIndex+count].localBounds : mesh.bounds,scene.GetTransform(next))))break;
                ++count;
            }
        }
        // Legacy batches compare the current stream only. A previous-position
        // override on any member must still keep the entire batch on original IB.
        if(draws && indexBuffer==mesh.mainIndexBuffer)
            for(uint32_t i=1;i<count;++i)
                if(MainIndexBuffer(mesh,level,scene.Materials()[materialIndex],&(*draws)[entityIndex+i])!=indexBuffer)
                {indexBuffer=mesh.indexBuffer;break;}
        auto batchContext=Platform::GetLiveContext();
        if(Platform::IsLiveCollectorEnabled())
        {
            const auto sceneBatch=Platform::GetLiveBatchId(liveId,scene.m_liveId);
            const auto passBatch=Platform::GetLiveBatchId(sceneBatch,1);
            batchContext.batch=Platform::GetLiveBatchId(passBatch,(uint64_t(entityIndex)<<32)|count);
            batchContext.object=entityIndex<scene.m_liveEntities.size()?scene.m_liveEntities[entityIndex]:scene.m_liveId;
            Platform::LiveEvent batch;batch.id=batchContext.batch;batch.context=batchContext;
            batch.kind=Platform::LiveEventKind::Batch;batch.stage=Platform::LiveStage::Data;
            batch.first=entityIndex;batch.elements=count;batch.stride=2*sizeof(Math::float4x4);
            batch.bytes=uint64_t(count)*batch.stride;batch.resource=Platform::FindLiveObject(instanceBuffer);
            batch.reason=!stock?Platform::LiveSplitReason::CustomGeometry:!instanced?Platform::LiveSplitReason::InstancingDisabled:Platform::LiveSplitReason::None;
            Platform::RecordLiveEvent(batch);
            for(uint32_t i=0;i<count&&entityIndex+i<scene.m_liveEntities.size();++i)
                Platform::RecordLiveLink({scene.m_liveEntities[entityIndex+i],batch.id});
        }
        Platform::LiveContextScope linkedBatch(batchContext);
		const auto selectedPipeline=MaterialPipeline(scene.Materials()[materialIndex],mirrored);
		if(selectedPipeline!=boundPipeline)
		{
			commands.BindGraphicsPipeline(selectedPipeline);
			boundPipeline=selectedPipeline;
			boundMaterial=nullptr;
		}
        if(instanced)commands.BindVertexBuffer(1,instanceBuffer,entityIndex*2*sizeof(Math::float4x4));
		auto selectedSet=materialSets[setIndex];
		if(selectedSet && selectedSet->GetPipeline()!=selectedPipeline)
		{
			mainBindingScratch.assign(selectedSet->GetBindings(),selectedSet->GetBindings()+selectedSet->GetBindingCount());
			selectedSet=CachedResourceSet(selectedPipeline,mainBindingScratch,mainSets);
		}
		if(!selectedSet) return false;
		if(!batchBindings || boundMaterial != selectedSet)
		{
			boundMaterial = selectedSet;
			commands.BindResourceSet(boundMaterial);
		}
		commands.BindVertexBuffer(0, input && input->vertexBuffer ? input->vertexBuffer : mesh.vertexBuffer, 0);
		commands.BindIndexBuffer(indexBuffer, RHI::Format::R32_UINT, 0);
		const uint32_t flags = materialStates[materialIndex].textureFlags |
			(scene.GetEntityLighting(entity).receiveShadow ? 32u : 0u);
		auto constants = MakeDrawConstants(viewProjection, scene.Materials()[materialIndex], scene.GetTransform(entity), flags);
		if(lodDebugActive)
		{
			const float alpha=constants.baseColor.w;
			constants.baseColor=LodColor(level);constants.baseColor.w=alpha;
			// 비트 0~4는 텍스처, 5는 그림자 수신, 6은 렌더러의 LOD 시각화다.
			constants.textureFlags|=64u;
		}
		if(m_environmentCapture && !m_environmentCaptureEmitters) constants.emissiveColor={};
		constants.padding2 = materialIndex;
		commands.SetInlineConstants(0, sizeof(constants), &constants);
		if(input && !input->inlineConstants.empty())
			commands.SetInlineConstants(sizeof(constants), static_cast<uint32_t>(input->inlineConstants.size()), input->inlineConstants.data());
        commands.DrawIndexedInstanced(indexCount,count,0,0,0);
        ++recordingStatistics.mainDrawCount;recordingStatistics.mainInstanceCount+=count;
        recordingStatistics.mainTriangleCount+=uint64_t(indexCount/3)*count;
        recordingStatistics.mainOriginalTriangleCount+=uint64_t(mesh.indexCount/3)*count;
        recordingStatistics.lodInstanceCounts[level]+=count;
        orderIndex+=count-1;
	}
	commands.EndRendering();
#ifdef DY_RENDERDOC_ENABLED
	commands.EndDebugEvent();
#endif
	return true;
}
}
