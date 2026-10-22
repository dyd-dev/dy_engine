#include "../Platform/Profiler/CollectorInternal.h"
#include "RendererInternal.h"
#include "DrawCommands.h"
#include "dyf/Camera.h"
#include "dyf/Scene.h"
#include "../RHI/ResourceSet.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
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

bool ShadowSortProjection(const Math::float4x4& matrix,const Math::float4& parameters,bool& perspective)
{
    perspective=false;
    for(float value:matrix.m)if(!std::isfinite(value) || std::abs(value)>1e10f)return false;
    // Canonical orthographic light views keep clip w exactly one in either
    // light*(world*position) or (light*world)*position evaluation order.
    if(matrix.m[3]==0 && matrix.m[7]==0 && matrix.m[11]==0 && matrix.m[15]==1)return true;
    // Engine-generated point/spot projections have z = -A*w + B, A <= -1,
    // B < 0. Verify that relation before using clip w as increasing view depth.
    const double a=parameters.z,b=parameters.w;
    if(!std::isfinite(a) || !std::isfinite(b) || a>-1 || b>=0)return false;
    double directionSquared=0;
    for(uint32_t column=0;column<4;++column)
    {
        const double z=matrix.m[column*4+2],w=matrix.m[column*4+3];
        const double constant=column==3?b:0;
        const double expected=-a*w+constant;
        const double scale=std::max({1.0,std::abs(z),std::abs(a*w),std::abs(constant)});
        if(std::abs(z-expected)>1e-5*scale)return false;
        if(column<3)directionSquared+=w*w;
    }
    if(std::abs(directionSquared-1.0)>1e-4)return false;
    perspective=true;
    return true;
}

bool ShadowBatchNearDepth(const Math::Bounds3& bounds,const Math::float4x4& matrix,bool perspective,double& depth)
{
    const double minimum[]={bounds.min.x,bounds.min.y,bounds.min.z};
    const double maximum[]={bounds.max.x,bounds.max.y,bounds.max.z};
    const uint32_t row=perspective?3u:2u;
    depth=matrix.m[12+row];
    for(uint32_t axis=0;axis<3;++axis)
    {
        const double coefficient=matrix.m[axis*4+row];
        depth+=coefficient*(coefficient>=0?minimum[axis]:maximum[axis]);
    }
    return std::isfinite(depth) && std::abs(depth)<=1e32;
}

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
    const bool baseInstance=stock && instanced;
	const bool perEntity=draws && !stock;
	const bool cull=stock && config.enableFrustumCulling;
    const auto shadowPipelineFor=[&](const MaterialDesc& material,bool mirrored,bool compact=false) {
        // 비표준 입력에는 기존 fragment의 재질별 면 판정을 유지한다.
        if(!stock) return UsesStockGeometry() ? shadowMaterialPipelines[2] : shadowPipeline;
        const uint32_t variant=(material.doubleSided?1u:0u) |
            (material.alphaMode==MaterialAlphaMode::Mask?2u:0u) | (mirrored?4u:0u);
        if(compact)return compactShadowPipelines[variant];
        return variant ? shadowMaterialPipelines[variant-1] : shadowPipeline;
    };
    auto& casters=shadowCasterScratch;
    casters.clear();
    casters.reserve(scene.GetEntityCount());
    // Validate and classify casters once; all atlas views share these inputs.
    for(uint32_t i=0;i<scene.GetEntityCount();++i) {
        const auto entity=static_cast<EntityID>(i);
        if(!scene.GetEntityLighting(entity).castShadow)continue;
        const auto meshId=scene.GetEntityMesh(entity);
        const auto materialId=scene.GetEntityMaterial(entity);
        if(!IsValid(meshId) || !IsValid(materialId))continue;
        const uint32_t meshIndex=ToIndex(meshId),materialIndex=ToIndex(materialId);
        if(meshIndex>=m_meshes.size() || materialIndex>=materialStates.size())continue;
        const auto& mesh=m_meshes[meshIndex];
        if(!mesh.prepared || !mesh.vertexBuffer || !mesh.indexBuffer || !mesh.indexCount)continue;
        const auto& material=scene.Materials()[materialIndex];
        // A binary shadow map cannot represent translucent coverage.
        if(shadowFragmentShader && material.alphaMode==MaterialAlphaMode::Blend)continue;
        ShadowCaster caster;
        caster.entity=i;caster.mesh=meshIndex;caster.material=materialIndex;
        const auto vertexBuffer=draws && (*draws)[i].vertexBuffer ? (*draws)[i].vertexBuffer : mesh.vertexBuffer;
        const bool originalVertices=stock && vertexBuffer==mesh.vertexBuffer;
        caster.arena=originalVertices && shadowArenaVertices && shadowArenaIndices && shadowArenaGeneration &&
            mesh.shadowArena.generation==shadowArenaGeneration && mesh.shadowVertexReady && mesh.shadowIndexReady;
        caster.compact=originalVertices && (caster.arena || (mesh.shadowVertexBuffer && mesh.shadowVertexReady &&
            mesh.shadowIndexBuffer && mesh.shadowIndexReady));
        caster.batch=static_cast<uint32_t>(casters.size());
        const auto& transform=scene.GetTransform(entity);
        caster.mirrored=Math::Determinant3x3(transform)<0;
        if(cull)
            caster.worldBounds.bounds=ShadowWorldBounds(draws && (*draws)[i].hasLocalBounds ? (*draws)[i].localBounds :
                mesh.bounds,transform,caster.worldBounds.magnitude);
        caster.sortSafe=caster.arena && mesh.shadowArena.sortSafeInputs && caster.worldBounds.bounds.valid;
        if(caster.sortSafe)
        {
            // Bound raw matrix inputs as well as transformed magnitudes: GLSL/Metal
            // can multiply light*world before position, so cancellation alone is insufficient.
            for(float value:transform.m)
                if(!std::isfinite(value) || std::abs(value)>1e10f)caster.sortSafe=false;
            for(double value:caster.worldBounds.magnitude)
                if(!std::isfinite(value) || value>1e21)caster.sortSafe=false;
        }
        if(stock && instanced && !casters.empty()) {
            const auto& previous=casters.back();
            const auto& previousMaterial=scene.Materials()[previous.material];
            // The canonical instance stream still requires consecutive entity indices.
            bool same=previous.entity+1==i && previous.mesh==meshIndex && previous.compact==caster.compact &&
                previous.arena==caster.arena &&
                (!draws || (*draws)[previous.entity].vertexBuffer==(*draws)[i].vertexBuffer) &&
                material.alphaMode!=MaterialAlphaMode::Blend &&
                previousMaterial.doubleSided==material.doubleSided && previous.mirrored==caster.mirrored &&
                (previousMaterial.alphaMode==MaterialAlphaMode::Mask)==(material.alphaMode==MaterialAlphaMode::Mask);
            if(same && material.alphaMode==MaterialAlphaMode::Mask)
                same=materialStates[previous.material].textures[0]==materialStates[materialIndex].textures[0] &&
                    (materialStates[previous.material].textureFlags&1u)==(materialStates[materialIndex].textureFlags&1u) &&
                    previousMaterial.baseColor.w==material.baseColor.w && previousMaterial.alphaCutoff==material.alphaCutoff;
            if(same)caster.batch=previous.batch;
        }
        casters.push_back(caster);
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
			shadowSets[i] = CachedResourceSet(shadowPipelineFor(material,false,stock),bindings,this->shadowSets);
			if(!shadowSets[i])
			{
				return false;
			}
		}
        // Resolve immutable bindings once rather than repeating the mirrored
        // pipeline lookup and binding copy in every atlas view.
        for(auto& caster:casters)
        {
            const auto& material=scene.Materials()[caster.material];
            caster.pipeline=shadowPipelineFor(material,caster.mirrored,caster.compact);
            caster.set=shadowSets[perEntity ? caster.entity : shadowFragmentShader ? caster.material : 0];
            if(caster.set && caster.set->GetPipeline()!=caster.pipeline)
            {
                shadowBindingScratch.assign(caster.set->GetBindings(),caster.set->GetBindings()+caster.set->GetBindingCount());
                caster.set=CachedResourceSet(caster.pipeline,shadowBindingScratch,this->shadowSets);
            }
            if(!caster.set)return false;
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
        RHI::BufferHandle boundVertices=nullptr,boundIndices=nullptr;
        RHI::Format boundIndexFormat=RHI::Format::Unknown;
		for(uint32_t view = 0; view < shadows.viewCount; ++view)
		{
			bool inlineConstantsValid=false;
            bool instanceStreamBound=false;
			const ShadowFrustum frustum(shadows.constants.lightViewProjectionMatrix[view]);
            auto& visible=shadowViewScratch;
            if(cull) {
                visible.clear();
                visible.reserve(casters.size());
                for(uint32_t i=0;i<casters.size();++i) {
                    const auto& bounds=casters[i].worldBounds;
                    if(!frustum.Outside(bounds.bounds,bounds.magnitude))visible.push_back(i);
                }
            }
            const auto casterAt=[&](uint32_t index) -> const ShadowCaster& {
                return casters[cull ? visible[index] : index];
            };
            const auto visibleCount=cull ? visible.size() : casters.size();
            bool perspectiveSort=false;
            if(stock && cull && visibleCount>1 && UsesStockGeometry() &&
                ShadowSortProjection(shadows.constants.lightViewProjectionMatrix[view],
                    shadows.constants.viewProjectionParams[view],perspectiveSort))
            {
                auto& depths=shadowBatchDepthScratch;
                depths.assign(casters.size(),std::numeric_limits<double>::infinity());
                bool safe=true;
                for(uint32_t index:visible)
                {
                    const auto& caster=casters[index];double depth=0;
                    if(!caster.sortSafe || !ShadowBatchNearDepth(caster.worldBounds.bounds,
                        shadows.constants.lightViewProjectionMatrix[view],perspectiveSort,depth))
                    {
                        safe=false;break;
                    }
                    depths[caster.batch]=std::min(depths[caster.batch],depth);
                }
                if(safe)
                {
                    // All members keep the same batch key. Original caster indices
                    // provide a stable tie break without a temporary sort allocation.
                    // Contiguous entity runs and every draw's primitive order survive.
                    std::sort(visible.begin(),visible.end(),[&](uint32_t a,uint32_t b) {
                        const double first=depths[casters[a].batch],second=depths[casters[b].batch];
                        return first<second || (first==second && a<b);
                    });
                    if(!shadowBatchSortReported && !std::is_sorted(visible.begin(),visible.end()))
                    {
                        std::fprintf(stderr,"[Renderer] Shadow batch depth ordering selected: %s view=%u casters=%zu; finite bounded inputs, unchanged instance runs.\n",
                            perspectiveSort?"perspective":"orthographic",view,visible.size());
                        shadowBatchSortReported=true;
                    }
                }
            }
			const uint32_t x = (view % shadows.columns) * shadows.resolution;
			const uint32_t y = (view / shadows.columns) * shadows.resolution;
			commands.SetViewport({static_cast<float>(x), static_cast<float>(y),
				static_cast<float>(shadows.resolution), static_cast<float>(shadows.resolution), 0, 1});
			commands.SetScissor({static_cast<int32_t>(x), static_cast<int32_t>(y), shadows.resolution, shadows.resolution});
			for(uint32_t drawIndex=0;drawIndex<visibleCount;)
			{
				const auto& caster=casterAt(drawIndex);
				const uint32_t entityIndex=caster.entity;
				const auto entity = static_cast<EntityID>(entityIndex);
				const auto& lighting = scene.GetEntityLighting(entity);
				const uint32_t materialIndex=caster.material;
				const auto& mesh = m_meshes[caster.mesh];
				const auto& material=scene.Materials()[materialIndex];
				uint32_t count=1;
                while(drawIndex+count<visibleCount) {
                    const auto& next=casterAt(drawIndex+count);
                    if(next.batch!=caster.batch || next.entity!=entityIndex+count)break;
                    ++count;
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
				const auto selectedPipeline=caster.pipeline;
				if(selectedPipeline!=boundPipeline)
				{
					commands.BindGraphicsPipeline(selectedPipeline);boundPipeline=selectedPipeline;boundSet=nullptr;
                    inlineConstantsValid=false;
				}
				const auto selectedSet=caster.set;
				if(selectedSet!=boundSet) {commands.BindResourceSet(selectedSet);boundSet=selectedSet;}
				const auto vertexBuffer=input && input->vertexBuffer ? input->vertexBuffer : mesh.vertexBuffer;
                const auto selectedVertices=caster.arena ? shadowArenaVertices :
                    caster.compact ? mesh.shadowVertexBuffer : vertexBuffer;
                if(!caster.arena || selectedVertices!=boundVertices)
                {
                    commands.BindVertexBuffer(0,selectedVertices,0);
                    boundVertices=selectedVertices;
                }
                if(instanced)
                {
                    // Stock shaders consume only instance attributes: offset zero
                    // plus firstInstance selects the same consecutive entity rows.
                    if(!baseInstance)commands.BindVertexBuffer(1,instanceBuffer,entityIndex*2*sizeof(Math::float4x4));
                    else if(!instanceStreamBound)
                    {
                        commands.BindVertexBuffer(1,instanceBuffer,0);
                        instanceStreamBound=true;
                    }
                }
                const auto selectedIndices=caster.arena ? shadowArenaIndices :
                    caster.compact ? mesh.shadowIndexBuffer : mesh.indexBuffer;
                const auto indexFormat=caster.arena ? mesh.shadowArena.indexFormat :
                    caster.compact ? mesh.shadowIndexFormat : RHI::Format::R32_UINT;
                if(!caster.arena || selectedIndices!=boundIndices || indexFormat!=boundIndexFormat)
                {
                    commands.BindIndexBuffer(selectedIndices,indexFormat,0);
                    boundIndices=selectedIndices;boundIndexFormat=indexFormat;
                }
                // Instanced opaque stock shadows only read the view index here;
                // their model comes from the instance stream and they have no FS.
                const bool reuseConstants=stock && instanced && material.alphaMode==MaterialAlphaMode::Opaque;
                if(!reuseConstants || !inlineConstantsValid)
                {
                    const uint32_t flags=materialStates[materialIndex].textureFlags | (lighting.receiveShadow ? 32u : 0u);
                    auto constants=MakeDrawConstants(viewProjection,material,scene.GetTransform(entity),flags);
                    constants.padding2=view;
                    commands.SetInlineConstants(0,sizeof(constants),&constants);
                    if(input && !input->inlineConstants.empty())
                        commands.SetInlineConstants(sizeof(constants),static_cast<uint32_t>(input->inlineConstants.size()),input->inlineConstants.data());
                    inlineConstantsValid=true;
                }
                const uint32_t firstIndex=caster.arena ? mesh.shadowArena.indexByteOffset/
                    (indexFormat==RHI::Format::R16_UINT?2u:4u) : 0;
                const int32_t baseVertex=caster.arena ? static_cast<int32_t>(mesh.shadowArena.baseVertex) : 0;
				commands.DrawIndexedInstanced(mesh.indexCount,count,firstIndex,baseVertex,baseInstance?entityIndex:0);
                if(caster.arena && !shadowArenaReported)
                {
                    std::fprintf(stderr,"[Renderer] Shared shadow arena selected: generation=%llu mixed R16/R32 index ranges; original draw/triangle order.\n",
                        static_cast<unsigned long long>(shadowArenaGeneration));
                    shadowArenaReported=true;
                }
				++recordingStatistics.shadowDrawCount;
				recordingStatistics.shadowInstanceCount+=count;
				recordingStatistics.shadowTriangleCount+=uint64_t(mesh.indexCount/3)*count;
				drawIndex+=count;
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
