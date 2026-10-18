#include "../Platform/Profiler/CollectorInternal.h"
#include "RendererInternal.h"
#include "DrawCommands.h"
#include "dyf/Camera.h"
#include "dyf/Scene.h"
#include "../RHI/ResourceSet.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace dyf
{
namespace {
Math::Bounds3 ShadowWorldBounds(const Math::Bounds3& local,const Math::float4x4& world,std::array<double,3>& magnitude)
{
    if(!local.valid || world.m[3]!=0 || world.m[7]!=0 || world.m[11]!=0 || world.m[15]!=1)return {};
    for(float value:world.m)if(!std::isfinite(value))return {};
    const double minimum[]={local.min.x,local.min.y,local.min.z};
    const double maximum[]={local.max.x,local.max.y,local.max.z};
    double center[3],extent[3];
    for(uint32_t axis=0;axis<3;++axis) {
        if(!std::isfinite(minimum[axis]) || !std::isfinite(maximum[axis]) || minimum[axis]>maximum[axis])return {};
        center[axis]=(minimum[axis]+maximum[axis])*.5;
        extent[axis]=(maximum[axis]-minimum[axis])*.5;
    }
    float lower[3],upper[3];
    for(uint32_t row=0;row<3;++row) {
        double position=world.m[12+row],radius=0;
        magnitude[row]=std::abs(position);
        for(uint32_t column=0;column<3;++column) {
            const double coefficient=world.m[column*4+row];
            position+=coefficient*center[column];radius+=std::abs(coefficient)*extent[column];
            magnitude[row]+=std::abs(coefficient)*std::max(std::abs(minimum[column]),std::abs(maximum[column]));
        }
        // 보수적인 변환 경계를 float로 저장할 때 바깥 방향으로 반올림한다.
        lower[row]=std::nextafter(static_cast<float>(position-radius),-std::numeric_limits<float>::infinity());
        upper[row]=std::nextafter(static_cast<float>(position+radius),std::numeric_limits<float>::infinity());
        if(!std::isfinite(lower[row]) || !std::isfinite(upper[row]))return {};
    }
    return {{lower[0],lower[1],lower[2]},{upper[0],upper[1],upper[2]},true};
}

struct ShadowFrustum
{
    std::array<std::array<double,4>,6> planes={},magnitudes={};
    bool valid=false;
    explicit ShadowFrustum(const Math::float4x4& matrix)
    {
        for(float value:matrix.m)if(!std::isfinite(value))return;
        for(uint32_t plane=0;plane<6;++plane) {
            const uint32_t row=plane/2;
            const double sign=plane%2?-1:1;
            for(uint32_t column=0;column<4;++column) {
                const double a=matrix.m[column*4+3],b=matrix.m[column*4+row];
                planes[plane][column]=plane==4?b:a+sign*b;
                magnitudes[plane][column]=plane==4?std::abs(b):std::abs(a)+std::abs(b);
            }
            if(planes[plane][0]==0 && planes[plane][1]==0 && planes[plane][2]==0)return;
        }
        valid=true;
    }
    bool Outside(const Math::Bounds3& bounds,const std::array<double,3>& worldMagnitude) const
    {
        if(!valid || !bounds.valid)return false;
        const double minimum[]={bounds.min.x,bounds.min.y,bounds.min.z};
        const double maximum[]={bounds.max.x,bounds.max.y,bounds.max.z};
        for(uint32_t plane=0;plane<6;++plane) {
            double distance=planes[plane][3],scale=magnitudes[plane][3];
            for(uint32_t axis=0;axis<3;++axis) {
                distance+=planes[plane][axis]*(planes[plane][axis]>=0?maximum[axis]:minimum[axis]);
                // 셰이더의 light*world 연산에서도 상쇄 전 월드 항의 크기를 유지한다.
                scale+=magnitudes[plane][axis]*worldMagnitude[axis];
            }
            if(distance < -1e-5*std::max(1.0,scale))return true;
        }
        return false;
    }
};

}

bool Renderer::Impl::RecordShadowPass(const Scene& scene, const Camera& camera, const ShadowData& shadows,
	RHI::ICommandList& commands, const std::vector<RendererDrawDesc>* draws)
{
#if defined(DY_LIVE_PROFILING_ENABLED) || defined(DY_PROFILER_TRACING_ENABLED)
    Platform::CpuScope cpuScope(Platform::LiveStage::Record,"RendererShadow");
#endif
    Platform::FrameProfiler::GpuScope liveGpu(m_profiler,commands,Platform::LiveGpuScope::RendererShadow,liveId);
	// 그림자 윤곽은 원본 인덱스를 사용하여 카메라 LOD 변경에도 정적 아틀라스를 재사용한다.
	const bool stock=stockDrawInputs;
	const bool instanced=UsesInstanceStream();
	const bool perEntity=draws && !stock;
	const bool cull=stock && config.enableFrustumCulling;
    const auto shadowPipelineFor=[&](const MaterialDesc& material,bool mirrored) {
        // 비표준 입력에는 기존 fragment의 재질별 면 판정을 유지한다.
        if(!stock) return UsesStockGeometry() ? shadowMaterialPipelines[2] : shadowPipeline;
        const uint32_t variant=(material.doubleSided?1u:0u) |
            (material.alphaMode==MaterialAlphaMode::Mask?2u:0u) | (mirrored?4u:0u);
        return variant ? shadowMaterialPipelines[variant-1] : shadowPipeline;
    };
	if(cull) {
        shadowBoundsScratch.resize(scene.GetEntityCount());
        for(uint32_t i=0;i<scene.GetEntityCount();++i) {
            auto& bounds=shadowBoundsScratch[i];bounds={};
            const auto entity=static_cast<EntityID>(i);
            const auto meshId=scene.GetEntityMesh(entity);
            if(scene.GetEntityLighting(entity).castShadow && IsValid(meshId) && ToIndex(meshId)<m_meshes.size())
                bounds.bounds=ShadowWorldBounds(draws && (*draws)[i].hasLocalBounds ? (*draws)[i].localBounds :
                    m_meshes[ToIndex(meshId)].bounds,scene.GetTransform(entity),bounds.magnitude);
        }
    }
	auto& shadowSets=shadowSetScratch;
	{
#ifdef DY_RENDERDOC_ENABLED
		commands.BeginDebugEvent("Shadow");
#endif
		shadowSets.assign(perEntity ? scene.GetEntityCount() : shadowFragmentShader ? materialStates.size() : 1, nullptr);
		for(uint32_t i = 0; i < shadowSets.size(); ++i)
		{
			const auto materialIndex=perEntity ? ToIndex(scene.GetEntityMaterial(static_cast<EntityID>(i))) : i;
			if(materialIndex>=materialStates.size()) continue;
			const auto& material=scene.Materials()[materialIndex];
			if(shadowFragmentShader && material.alphaMode==MaterialAlphaMode::Blend) continue;
			auto& bindings=shadowBindingScratch;bindings = {{3u,
				0, shadowMatrixBuffer, nullptr, 0, static_cast<uint32_t>(sizeof(RendererShadowConstants)), {}}};
			if(shadowFragmentShader && (!stock || material.alphaMode==MaterialAlphaMode::Mask))
			{
				bindings.push_back({0u,0,nullptr,materialStates[materialIndex].textures[0],0,0,{}});
			}
			if(perEntity)
			{
				const auto& extra = (*draws)[i].vertexResources;
				bindings.insert(bindings.end(), extra.begin(), extra.end());
			}
			shadowSets[i] = CachedResourceSet(shadowPipelineFor(material,false),bindings,this->shadowSets);
			if(!shadowSets[i])
			{
				return false;
			}
		}
	}
	const auto viewProjection = camera.projection * camera.view;
	{
		RHI::DepthStencilAttachment depth;
		depth.texture = shadowDepthTarget;
		depth.state = RHI::ResourceState::DepthWrite;
		depth.depthLoadOp = RHI::LoadOp::Clear;
		depth.depthStoreOp = RHI::StoreOp::Store;
		commands.BeginRendering({nullptr, 0, &depth});
		RHI::PipelineHandle boundPipeline=nullptr;
		RHI::ResourceSetHandle boundSet=nullptr;
		for(uint32_t view = 0; view < shadows.viewCount; ++view)
		{
			const ShadowFrustum frustum(shadows.constants.lightViewProjectionMatrix[view]);
			const auto outside=[&](uint32_t index) {
                const auto& bounds=shadowBoundsScratch[index];
                return frustum.Outside(bounds.bounds,bounds.magnitude);
            };
			const uint32_t x = (view % shadows.columns) * shadows.resolution;
			const uint32_t y = (view / shadows.columns) * shadows.resolution;
			commands.SetViewport({static_cast<float>(x), static_cast<float>(y),
				static_cast<float>(shadows.resolution), static_cast<float>(shadows.resolution), 0, 1});
			commands.SetScissor({static_cast<int32_t>(x), static_cast<int32_t>(y), shadows.resolution, shadows.resolution});
			for(uint32_t entityIndex = 0; entityIndex < scene.GetEntityCount(); ++entityIndex)
			{
				const auto entity = static_cast<EntityID>(entityIndex);
				const auto& lighting = scene.GetEntityLighting(entity);
				if(!lighting.castShadow) continue;
				const auto meshId = scene.GetEntityMesh(entity);
				const auto materialId = scene.GetEntityMaterial(entity);
				if(!IsValid(meshId) || !IsValid(materialId)) continue;
				const uint32_t meshIndex = ToIndex(meshId), materialIndex = ToIndex(materialId);
				if(meshIndex >= m_meshes.size() || materialIndex >= materialStates.size()) continue;
				const auto& mesh = m_meshes[meshIndex];
				if(!mesh.prepared || !mesh.vertexBuffer || !mesh.indexBuffer || !mesh.indexCount) continue;
				const auto& material=scene.Materials()[materialIndex];
				// 블렌드 표면의 투과율은 이진 깊이맵으로 표현할 수 없다.
				if(shadowFragmentShader && material.alphaMode==MaterialAlphaMode::Blend) continue;
				if(cull && outside(entityIndex))continue;
				const bool mirrored=Math::Determinant3x3(scene.GetTransform(entity))<0;
				uint32_t count=1;
				if(stock && instanced) {
					// 알파 검사 그림자는 기본색의 알파 커버리지도 같아야 묶는다.
                    while(entityIndex+count<scene.GetEntityCount()) {
                        const auto next=static_cast<EntityID>(entityIndex+count);
                        if(draws && (*draws)[entityIndex+count].vertexBuffer!=(*draws)[entityIndex].vertexBuffer) break;
                        const auto material=scene.GetEntityMaterial(next);
                        if(scene.GetEntityMesh(next)!=meshId || !scene.GetEntityLighting(next).castShadow ||
                            !IsValid(material) || ToIndex(material)>=materialStates.size() ||
                            (cull && outside(entityIndex+count)))break;
                        const auto nextIndex=ToIndex(material);
                        const auto& nextMaterial=scene.Materials()[nextIndex];
                        const auto& currentMaterial=scene.Materials()[materialIndex];
                        // 깊이와 알파 커버리지가 같으면 PBR 표면 색·거칠기 차이는 배치를 나누지 않는다.
                        if(nextMaterial.alphaMode==MaterialAlphaMode::Blend ||
                            nextMaterial.doubleSided!=currentMaterial.doubleSided ||
                            (Math::Determinant3x3(scene.GetTransform(next))<0)!=mirrored ||
                            (nextMaterial.alphaMode==MaterialAlphaMode::Mask)!=(currentMaterial.alphaMode==MaterialAlphaMode::Mask))break;
                        if(currentMaterial.alphaMode==MaterialAlphaMode::Mask &&
                            (materialStates[nextIndex].textures[0]!=materialStates[materialIndex].textures[0] ||
                            (materialStates[nextIndex].textureFlags&1u)!=(materialStates[materialIndex].textureFlags&1u) ||
                            nextMaterial.baseColor.w!=currentMaterial.baseColor.w || nextMaterial.alphaCutoff!=currentMaterial.alphaCutoff))break;
                        ++count;
                    }
                }
        auto batchContext=Platform::GetLiveContext();
        if(Platform::IsLiveCollectorEnabled())
        {
            const auto sceneBatch=Platform::GetLiveBatchId(liveId,scene.m_liveId);
            const auto passBatch=Platform::GetLiveBatchId(sceneBatch,100+view);
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
				const auto* input = draws ? &(*draws)[entityIndex] : nullptr;
				const auto selectedPipeline=shadowPipelineFor(material,mirrored);
				if(selectedPipeline!=boundPipeline)
				{
					commands.BindGraphicsPipeline(selectedPipeline);boundPipeline=selectedPipeline;boundSet=nullptr;
				}
				auto selectedSet=shadowSets[perEntity ? entityIndex : shadowFragmentShader ? materialIndex : 0];
				if(selectedSet && selectedSet->GetPipeline()!=selectedPipeline)
				{
					shadowBindingScratch.assign(selectedSet->GetBindings(),selectedSet->GetBindings()+selectedSet->GetBindingCount());
					selectedSet=CachedResourceSet(selectedPipeline,shadowBindingScratch,this->shadowSets);
				}
				if(!selectedSet) return false;
				if(selectedSet!=boundSet) {commands.BindResourceSet(selectedSet);boundSet=selectedSet;}
				commands.BindVertexBuffer(0, input && input->vertexBuffer ? input->vertexBuffer : mesh.vertexBuffer, 0);
                if(instanced)commands.BindVertexBuffer(1,instanceBuffer,entityIndex*2*sizeof(Math::float4x4));
				commands.BindIndexBuffer(mesh.indexBuffer, RHI::Format::R32_UINT, 0);
				const uint32_t flags = materialStates[materialIndex].textureFlags |
					(lighting.receiveShadow ? 32u : 0u);
				auto constants = MakeDrawConstants(viewProjection, scene.Materials()[materialIndex], scene.GetTransform(entity), flags);
				constants.padding2 = view;
				commands.SetInlineConstants(0, sizeof(constants), &constants);
				if(input && !input->inlineConstants.empty())
					commands.SetInlineConstants(sizeof(constants), static_cast<uint32_t>(input->inlineConstants.size()), input->inlineConstants.data());
				commands.DrawIndexedInstanced(mesh.indexCount,count,0,0,0);
				++recordingStatistics.shadowDrawCount;
				recordingStatistics.shadowInstanceCount+=count;
				recordingStatistics.shadowTriangleCount+=uint64_t(mesh.indexCount/3)*count;
				entityIndex+=count-1;
			}
		}
		commands.EndRendering();

	}
#ifdef DY_RENDERDOC_ENABLED
	commands.EndDebugEvent();
#endif
	return true;
}
}
