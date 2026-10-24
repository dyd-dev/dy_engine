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
// 각 좌표축에 나란한 면으로 물체를 감싸는 상자(AABB)를 로컬 좌표에서 affine 월드 변환으로 옮겨 월드 AABB를 만든다.
// 중심은 행렬로 옮기고 반폭은 행렬 성분의 절댓값으로 합산해 회전·확대된 경계를 포함한다. double로 계산한 뒤 float 저장 경계를 바깥으로 한 단계 넓힌다.
Math::Bounds3 ShadowWorldBounds(const Math::Bounds3& local,const Math::float4x4& world,std::array<double,3>& magnitude)
{
    if(!local.valid || world.m[3]!=0 || world.m[7]!=0 || world.m[11]!=0 || world.m[15]!=1)return {}; // 로컬 경계가 없거나 행렬의 마지막 행이 (0,0,0,1)이 아니면 이 affine 계산을 적용하지 않고 무효 경계를 반환한다. 이후 Outside는 이 경계의 객체를 제외하지 않는다.
    for(float value:world.m)if(!std::isfinite(value))return {}; // 월드 행렬에 NaN/무한대가 있으면 경계 계산을 중단한다.
    const double minimum[]={local.min.x,local.min.y,local.min.z};
    const double maximum[]={local.max.x,local.max.y,local.max.z};
    double center[3],extent[3];
    for(uint32_t axis=0;axis<3;++axis) {
        if(!std::isfinite(minimum[axis]) || !std::isfinite(maximum[axis]) || minimum[axis]>maximum[axis])return {}; // 경계값이 비유한 값이거나 최소/최대 순서가 뒤집혔으면 무효 처리한다.
        center[axis]=(minimum[axis]+maximum[axis])*.5;
        extent[axis]=(maximum[axis]-minimum[axis])*.5;
    }
    float lower[3],upper[3];
    for(uint32_t row=0;row<3;++row) {
        double position=world.m[12+row],radius=0;
        // 월드 좌표의 큰 항이 서로 상쇄되는 경우에도 오차 크기 평가를 위해 각 항의 절댓값을 따로 누적한다.
        magnitude[row]=std::abs(position);
        for(uint32_t column=0;column<3;++column) {
            const double coefficient=world.m[column*4+row];
            position+=coefficient*center[column];radius+=std::abs(coefficient)*extent[column];
            magnitude[row]+=std::abs(coefficient)*std::max(std::abs(minimum[column]),std::abs(maximum[column]));
        }
        // float 변환 뒤 nextafter로 최소는 -무한대 쪽, 최대는 +무한대 쪽으로 한 단계 옮긴다. 저장 반올림 때문에 실제 경계가 안쪽으로 좁아지는 것을 막는다.
        lower[row]=std::nextafter(static_cast<float>(position-radius),-std::numeric_limits<float>::infinity());
        upper[row]=std::nextafter(static_cast<float>(position+radius),std::numeric_limits<float>::infinity());
        if(!std::isfinite(lower[row]) || !std::isfinite(upper[row]))return {}; // 확장한 float 경계도 유한하게 표현되지 않으면 무효 경계를 돌려준다.
    }
    return {{lower[0],lower[1],lower[2]},{upper[0],upper[1],upper[2]},true};
}

// 광원 view-projection에서 여섯 평면과 각 계수의 크기를 보관한다. 유효하지 않은 평면은 제거 판정에 사용하지 않는다.
struct ShadowFrustum
{
    std::array<std::array<double,4>,6> planes={},magnitudes={};
    bool valid=false;
    // 클립 공간 평면을 추출한다. near 평면(4번)은 깊이 0 경계를 쓰고 나머지는 w±x/y/z 조합이다.
    explicit ShadowFrustum(const Math::float4x4& matrix)
    {
        for(float value:matrix.m)if(!std::isfinite(value))return; // 광원 행렬이 비유한 값이면 frustum을 valid로 만들지 않는다.
        for(uint32_t plane=0;plane<6;++plane) {
            const uint32_t row=plane/2;
            const double sign=plane%2?-1:1;
            for(uint32_t column=0;column<4;++column) {
                const double a=matrix.m[column*4+3],b=matrix.m[column*4+row];
                planes[plane][column]=plane==4?b:a+sign*b;
                magnitudes[plane][column]=plane==4?std::abs(b):std::abs(a)+std::abs(b);
            }
            if(planes[plane][0]==0 && planes[plane][1]==0 && planes[plane][2]==0)return; // 표면에 수직인 방향(법선)을 나타내는 평면의 앞 세 계수가 모두 0이면 유효한 절두체로 사용하지 않는다.
        }
        valid=true;
    }
    // 각 평면의 계수 부호에 따라 AABB의 min/max를 골라 평면식 값이 가장 큰 점을 검사한다. 이 값마저 -1e-5*max(1,scale)보다 작을 때만 상자 전체가 오차 여유를 넘어 그 평면 밖이라고 판정한다.
    bool Outside(const Math::Bounds3& bounds,const std::array<double,3>& worldMagnitude) const
    {
        if(!valid || !bounds.valid)return false; // 절두체나 경계가 불확실하면 밖이라고 판단하지 않아 잘못된 제거를 피한다.
        const double minimum[]={bounds.min.x,bounds.min.y,bounds.min.z};
        const double maximum[]={bounds.max.x,bounds.max.y,bounds.max.z};
        for(uint32_t plane=0;plane<6;++plane) {
            double distance=planes[plane][3],scale=magnitudes[plane][3];
            for(uint32_t axis=0;axis<3;++axis) {
                distance+=planes[plane][axis]*(planes[plane][axis]>=0?maximum[axis]:minimum[axis]);
                // 셰이더의 light*world 연산에서도 상쇄 전 월드 항의 크기를 유지한다.
                scale+=magnitudes[plane][axis]*worldMagnitude[axis];
            }
            if(distance < -1e-5*std::max(1.0,scale))return true; // 오차 크기에 따른 여유보다 확실하게 평면 밖일 때만 제거한다.
        }
        return false;
    }
};

}

// 공통 월드 경계 준비→재질/객체별 세트 준비→아틀라스 전체 깊이 Clear→각 뷰 타일의 객체 배치 기록 순서다.
bool Renderer::Impl::RecordShadowPass(const Scene& scene, const Camera& camera, const ShadowData& shadows,
	RHI::ICommandList& commands, const std::vector<RendererDrawDesc>* draws)
{
// 라이브/트레이싱 빌드에서 CPU 기록을 계측하고, GPU scope는 명령 타임스탬프와 후속 수집에 연결한다.
#if defined(DY_LIVE_PROFILING_ENABLED) || defined(DY_PROFILER_TRACING_ENABLED)
    Platform::CpuScope cpuScope(Platform::LiveStage::Record,"RendererShadow");
#endif
    Platform::FrameProfiler::GpuScope liveGpu(m_profiler,commands,Platform::LiveGpuScope::RendererShadow,liveId);
	// 그림자 윤곽은 원본 인덱스를 사용하여 카메라 LOD 변경에도 정적 아틀라스를 재사용한다.
	const bool stock=stockDrawInputs;
	const bool instanced=UsesInstanceStream();
	const bool perEntity=draws && !stock;
	const bool cull=stock && config.enableFrustumCulling;
    // 기본 경로는 양면·알파 Mask·면 반전 조합으로 파이프라인을 고른다.
    // 면 반전은 행렬 변환이 넓이·부피를 얼마나 바꾸고 방향을 뒤집는지 나타내는 값(행렬식)이 음수인지로 정한다. 사용자 입력은 아래의 별도 분기로 파이프라인을 선택한다.
    const auto shadowPipelineFor=[&](const MaterialDesc& material,bool mirrored) {
        if(!stock) return UsesStockGeometry() ? shadowMaterialPipelines[2] : shadowPipeline; // 비표준 draw 입력은 기존 fragment 커버리지를 유지하는 파이프라인을 사용한다.
        const uint32_t variant=(material.doubleSided?1u:0u) |
            (material.alphaMode==MaterialAlphaMode::Mask?2u:0u) | (mirrored?4u:0u);
        return variant ? shadowMaterialPipelines[variant-1] : shadowPipeline;
    };
	if(cull) { // 표준 입력이고 CPU 제거가 켜진 경우만 월드 경계 배열을 준비한다.
        // 객체별 월드 경계는 한 번 계산해 모든 광원/캐스케이드 뷰에서 재사용한다.
        shadowBoundsScratch.resize(scene.GetEntityCount());
        for(uint32_t i=0;i<scene.GetEntityCount();++i) {
            auto& bounds=shadowBoundsScratch[i];bounds={};
            const auto entity=static_cast<EntityID>(i);
            const auto meshId=scene.GetEntityMesh(entity);
            if(scene.GetEntityLighting(entity).castShadow && IsValid(meshId) && ToIndex(meshId)<m_meshes.size()) // 그림자 생성 대상이며 메시 ID가 유효할 때만 월드 경계를 계산한다.
                bounds.bounds=ShadowWorldBounds(draws && (*draws)[i].hasLocalBounds ? (*draws)[i].localBounds :
                    m_meshes[ToIndex(meshId)].bounds,scene.GetTransform(entity),bounds.magnitude);
        }
    }
	auto& shadowSets=shadowSetScratch;
	{
#ifdef DY_RENDERDOC_ENABLED // RenderDoc 빌드에서만 Shadow 디버그 구간 시작을 기록한다.
		commands.BeginDebugEvent("Shadow");
#endif
		// 사용자 자원 입력이면 객체별, fragment가 있으면 재질별, 깊이 정점 전용이면 공통 한 세트로 구성한다.
		shadowSets.assign(perEntity ? scene.GetEntityCount() : shadowFragmentShader ? materialStates.size() : 1, nullptr);
		for(uint32_t i = 0; i < shadowSets.size(); ++i)
		{
			const auto materialIndex=perEntity ? ToIndex(scene.GetEntityMaterial(static_cast<EntityID>(i))) : i;
			if(materialIndex>=materialStates.size()) continue; // 범위 밖 재질에 대해 세트를 만들지 않는다.
			const auto& material=scene.Materials()[materialIndex];
			if(shadowFragmentShader && material.alphaMode==MaterialAlphaMode::Blend) continue; // Blend의 연속 투과율을 이진 깊이 그림자로 표현하지 않으므로 세트 준비에서 제외한다.
			auto& bindings=shadowBindingScratch;bindings = {{3u,
				0, shadowMatrixBuffer, nullptr, 0, static_cast<uint32_t>(sizeof(RendererShadowConstants)), {}}};
			if(shadowFragmentShader && (!stock || material.alphaMode==MaterialAlphaMode::Mask)) // 알파 Mask 또는 비표준 커버리지 경로에만 기본색 텍스처를 연결한다.
			{
				bindings.push_back({0u,0,nullptr,materialStates[materialIndex].textures[0],0,0,{}});
			}
			if(perEntity) // 객체별 사용자 정점 자원을 그림자 기본 바인딩 뒤에 추가한다.
			{
				const auto& extra = (*draws)[i].vertexResources;
				bindings.insert(bindings.end(), extra.begin(), extra.end());
			}
			shadowSets[i] = CachedResourceSet(shadowPipelineFor(material,false),bindings,this->shadowSets);
			if(!shadowSets[i]) // 그림자 자원 세트를 만들지 못하면 패스 기록을 실패 처리한다.
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
		// 그림자 뷰마다 아틀라스 타일 viewport/scissor를 설정한다. 같은 객체가 여러 뷰에 그려지면 통계에도 각 뷰 작업이 따로 누적된다.
		for(uint32_t view = 0; view < shadows.viewCount; ++view)
		{
			const ShadowFrustum frustum(shadows.constants.lightViewProjectionMatrix[view]);
			// 현재 광원 뷰의 절두체와 미리 만든 객체 월드 경계를 비교한다. cull이 켜진 경로에서만 이 함수를 호출한다.
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
				if(!lighting.castShadow) continue; // 그림자를 만들지 않도록 설정한 객체는 어느 뷰에서도 기록하지 않는다.
				const auto meshId = scene.GetEntityMesh(entity);
				const auto materialId = scene.GetEntityMaterial(entity);
				if(!IsValid(meshId) || !IsValid(materialId)) continue; // 유효한 메시/재질 ID가 없는 객체는 생략한다.
				const uint32_t meshIndex = ToIndex(meshId), materialIndex = ToIndex(materialId);
				if(meshIndex >= m_meshes.size() || materialIndex >= materialStates.size()) continue; // GPU 메시/재질 배열 범위 밖이면 접근하지 않는다.
				const auto& mesh = m_meshes[meshIndex];
				if(!mesh.prepared || !mesh.vertexBuffer || !mesh.indexBuffer || !mesh.indexCount) continue; // 필수 정점·원본 인덱스가 준비되지 않은 객체는 그리지 않는다.
				const auto& material=scene.Materials()[materialIndex];
				// 블렌드 표면의 투과율은 이진 깊이맵으로 표현할 수 없다.
				if(shadowFragmentShader && material.alphaMode==MaterialAlphaMode::Blend) continue; // 그림자 fragment 셰이더가 있을 때 Blend 재질을 draw 목록에서 제외한다. 깊이 정점 전용 경로에는 이 조건을 적용하지 않는다.
				if(cull && outside(entityIndex))continue; // 현재 광원 절두체 밖임을 보수적으로 확인한 객체만 제거한다.
				const bool mirrored=Math::Determinant3x3(scene.GetTransform(entity))<0;
				uint32_t count=1;
				if(stock && instanced) { // 표준 인스턴스 경로에서만 연속 그림자 객체 묶기를 시도한다.
                    // 연속 객체의 메시·정점 버퍼·그림자 생성·면 방향·알파 커버리지가 같을 때 묶는다. 그림자에 영향 없는 PBR 색/거칠기 차이는 배치를 나눌 이유가 아니다.
                    while(entityIndex+count<scene.GetEntityCount()) {
                        const auto next=static_cast<EntityID>(entityIndex+count);
                        if(draws && (*draws)[entityIndex+count].vertexBuffer!=(*draws)[entityIndex].vertexBuffer) break; // 다음 객체의 정점 버퍼가 다르면 현재 그림자 배치에 합치지 않는다.
                        const auto material=scene.GetEntityMaterial(next);
                        if(scene.GetEntityMesh(next)!=meshId || !scene.GetEntityLighting(next).castShadow || // 메시가 다르거나 그림자 비활성·재질 무효·광원 시야 밖인 객체를 만나면 합치기를 중단한다.
                            !IsValid(material) || ToIndex(material)>=materialStates.size() ||
                            (cull && outside(entityIndex+count)))break;
                        const auto nextIndex=ToIndex(material);
                        const auto& nextMaterial=scene.Materials()[nextIndex];
                        const auto& currentMaterial=scene.Materials()[materialIndex];
                        if(nextMaterial.alphaMode==MaterialAlphaMode::Blend || // 다음 재질이 Blend이면 묶기를 끝낸다. 그 외에도 양면 여부·면 반전·Mask 여부가 현재 재질과 다르면 파이프라인을 공유할 수 없어 배치를 나눈다.
                            nextMaterial.doubleSided!=currentMaterial.doubleSided ||
                            (Math::Determinant3x3(scene.GetTransform(next))<0)!=mirrored ||
                            (nextMaterial.alphaMode==MaterialAlphaMode::Mask)!=(currentMaterial.alphaMode==MaterialAlphaMode::Mask))break;
                        if(currentMaterial.alphaMode==MaterialAlphaMode::Mask && // Mask 재질에서는 알파 텍스처·사용 비트·기본 알파·임곗값까지 같아야 같은 커버리지로 묶는다.
                            (materialStates[nextIndex].textures[0]!=materialStates[materialIndex].textures[0] ||
                            (materialStates[nextIndex].textureFlags&1u)!=(materialStates[materialIndex].textureFlags&1u) ||
                            nextMaterial.baseColor.w!=currentMaterial.baseColor.w || nextMaterial.alphaCutoff!=currentMaterial.alphaCutoff))break;
                        ++count;
                    }
                }
        // 배치 ID에 Renderer→Scene→그림자 view→객체 범위를 연결한다. 메인 패스와 다른 pass ID를 사용해 같은 객체의 작업을 섞지 않는다.
        auto batchContext=Platform::GetLiveContext();
        if(Platform::IsLiveCollectorEnabled()) // 수집 중이면 그림자 배치의 범위·제한된 이유 분류와 원본 객체 링크를 남긴다.
        {
            batchContext.batch=Platform::GetLiveBatchPath(liveId,{scene.m_liveId,100+view,(uint64_t(entityIndex)<<32)|count});
            batchContext.object=entityIndex<scene.m_liveEntities.size()?scene.m_liveEntities[entityIndex]:scene.m_liveId;
            Platform::LiveEvent batch;batch.id=batchContext.batch;batch.context=batchContext;
            batch.kind=Platform::LiveEventKind::Batch;batch.stage=Platform::LiveStage::Data;
            batch.first=entityIndex;batch.elements=count;batch.stride=2*sizeof(Math::float4x4);
            batch.bytes=uint64_t(count)*batch.stride;batch.resource=Platform::FindLiveObject(instanceBuffer);
            // 이 그림자 배치의 reason은 사용자 기하·인스턴싱 OFF·그 밖(None)만 구분한다. 위에서 실제로 묶기를 끝낸 메시·재질·절두체 조건은 각각 기록하지 않는다.
            batch.reason=!stock?Platform::LiveSplitReason::CustomGeometry:!instanced?Platform::LiveSplitReason::InstancingDisabled:Platform::LiveSplitReason::None;
            Platform::RecordLiveEvent(batch);
            for(uint32_t i=0;i<count&&entityIndex+i<scene.m_liveEntities.size();++i)
                Platform::RecordLiveLink({scene.m_liveEntities[entityIndex+i],batch.id});
        }
        Platform::LiveContextScope linkedBatch(batchContext);
				const auto* input = draws ? &(*draws)[entityIndex] : nullptr;
				const auto selectedPipeline=shadowPipelineFor(material,mirrored);
				if(selectedPipeline!=boundPipeline) // 재질 또는 반전으로 파이프라인이 바뀌면 다시 바인딩하고 세트 기억을 비운다.
				{
					commands.BindGraphicsPipeline(selectedPipeline);boundPipeline=selectedPipeline;boundSet=nullptr;
				}
				auto selectedSet=shadowSets[perEntity ? entityIndex : shadowFragmentShader ? materialIndex : 0];
				if(selectedSet && selectedSet->GetPipeline()!=selectedPipeline) // 선택 파이프라인과 세트가 다르면 같은 자원 목록으로 호환 세트를 구한다.
				{
					shadowBindingScratch.assign(selectedSet->GetBindings(),selectedSet->GetBindings()+selectedSet->GetBindingCount());
					selectedSet=CachedResourceSet(selectedPipeline,shadowBindingScratch,this->shadowSets);
				}
				if(!selectedSet) return false; // 필수 세트가 없으면 draw를 기록하지 않고 실패한다.
				if(selectedSet!=boundSet) {commands.BindResourceSet(selectedSet);boundSet=selectedSet;} // 같은 자원 세트의 반복 바인딩을 생략한다.
				commands.BindVertexBuffer(0, input && input->vertexBuffer ? input->vertexBuffer : mesh.vertexBuffer, 0);
                if(instanced)commands.BindVertexBuffer(1,instanceBuffer,entityIndex*2*sizeof(Math::float4x4)); // 인스턴스 스트림에서 객체당 행렬 두 개의 128바이트 간격을 사용하므로 첫 객체의 entityIndex*128 오프셋을 바인딩한다.
				commands.BindIndexBuffer(mesh.indexBuffer, RHI::Format::R32_UINT, 0);
				const uint32_t flags = materialStates[materialIndex].textureFlags |
					(lighting.receiveShadow ? 32u : 0u);
				auto constants = MakeDrawConstants(viewProjection, scene.Materials()[materialIndex], scene.GetTransform(entity), flags);
				// 메인 패스에서는 재질 인덱스로 쓰던 상수 슬롯에 여기서는 그림자 view 번호를 넣는다. 그림자 셰이더가 해당 광원 행렬을 선택한다.
				constants.padding2 = view;
				commands.SetInlineConstants(0, sizeof(constants), &constants);
				if(input && !input->inlineConstants.empty()) // 사용자 추가 상수가 있으면 기본 draw 상수 뒤에 이어 기록한다.
					commands.SetInlineConstants(sizeof(constants), static_cast<uint32_t>(input->inlineConstants.size()), input->inlineConstants.data());
				// 항상 원본 메시 인덱스로 그림자 깊이를 기록하고 현재 뷰의 draw·인스턴스·삼각형 수를 누적한다.
				commands.DrawIndexedInstanced(mesh.indexCount,count,0,0,0);
				++recordingStatistics.shadowDrawCount;
				recordingStatistics.shadowInstanceCount+=count;
				recordingStatistics.shadowTriangleCount+=uint64_t(mesh.indexCount/3)*count;
				// 이미 인스턴스 배치에 포함한 연속 객체는 다음 반복에서 다시 기록하지 않는다.
				entityIndex+=count-1;
			}
		}
		commands.EndRendering();

	}
#ifdef DY_RENDERDOC_ENABLED // RenderDoc 빌드에서 열린 Shadow 디버그 이벤트를 닫는다.
	commands.EndDebugEvent();
#endif
	return true;
}
}
