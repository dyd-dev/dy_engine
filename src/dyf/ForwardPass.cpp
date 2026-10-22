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
// 재질 세트 준비→색/깊이 렌더 시작→CPU 순서/투명 정렬→GPU 간접 배치→CPU 직접 배치→렌더 종료 순서다.
bool Renderer::Impl::RecordMainPass(const Scene& scene, const Camera& camera,
	RHI::ICommandList& commands, const std::vector<RendererDrawDesc>* draws,
	RHI::TextureHandle output)
{
// 라이브/트레이싱 빌드에서 CPU Record를 계측하며, 아래 GPU scope는 별도의 타임스탬프 기록과 후속 수집을 연결한다.
#if defined(DY_LIVE_PROFILING_ENABLED) || defined(DY_PROFILER_TRACING_ENABLED)
    Platform::CpuScope cpuScope(Platform::LiveStage::Record,"RendererMainForward");
#endif
    Platform::FrameProfiler::GpuScope liveGpu(m_profiler,commands,Platform::LiveGpuScope::RendererMainForward,liveId);
	auto& materialSets=mainSetScratch;
    // 재질 세트 준비 중 교체한 인덱스 버퍼의 소유권을 보호한다. 준비 실패면 새 버퍼를 버리고 이전 핸들/ready를 복구한다.
    struct MaterialBufferTransaction {
        Impl& r;
        RHI::BufferHandle previous;
        bool ready,committed=false;
        explicit MaterialBufferTransaction(Impl& owner):r(owner),previous(r.materialIndexBuffer),ready(r.materialIndexReady) {}
        ~MaterialBufferTransaction() {
            if(committed) {if(previous!=r.materialIndexBuffer)r.device->DestroyBuffer(previous);} // 교체 준비가 확정되었으면 과거 버퍼를 해제하고, 실패 경로에서는 새 버퍼를 해제해 이전 상태로 복구한다.
            else {
                if(previous!=r.materialIndexBuffer)r.device->DestroyBuffer(r.materialIndexBuffer); // 실패 중 새 버퍼로 바뀌었을 때만 그 새 버퍼를 정리한다.
                r.materialIndexBuffer=previous;r.materialIndexReady=ready;
            }
        }
    } materialTransaction(*this);
	if(!CreateMaterialResourceSets(scene, commands, draws, materialSets)) return false; // 필수 재질 세트 준비 실패면 버퍼 보호 객체가 이전 소유권을 복원한다.
    // 재질 세트 준비 성공을 확정한다. 여기의 committed는 버퍼 교체 책임의 확정이며 전체 프레임 GPU 완료를 뜻하지 않는다.
    materialTransaction.committed=true;
	auto* target = config.enableHdrRendering ? hdrTarget : output;
	const auto viewProjection = camera.projection * camera.view;
#ifdef DY_RENDERDOC_ENABLED // RenderDoc 표식 지원을 포함한 빌드에서만 디버그 이벤트 시작을 기록한다.
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
    if(config.enableHdrRendering) // HDR 타깃에는 clearColor의 출력 변환을 되돌린 선형 값을 넣는다. LOD 진단이면 그 값을 그대로 쓰고, LOD 진단이 아닐 때 노출이 0 이하면 0을 쓴다.
    // 일반 경로는 linear/((1-linear)*exposure)로 Reinhard와 노출을 역산하되 linear=1의 나눗셈을 피하고 16비트 float 최대값 65504로 제한한다.
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
    else if(RHI::IsSrgbFormat(target->GetDesc().format)) // 직접 sRGB 출력은 저장 변환을 고려해 clear 색을 선형 값으로 바꾼다.
    {
        for(uint32_t channel=0;channel<3;++channel)
        {
            const float value=color.clearColor[channel];
            color.clearColor[channel]=value<=0.04045f ? value/12.92f : std::pow((value+0.055f)/1.055f,2.4f);
        }
    }
	// 표면 선행 패스가 있으면 기존 깊이를 Load하고 없으면 Clear한다. 고급 효과/환경 캡처가 뒤에서 읽을 깊이는 Store한다.
	RHI::DepthStencilAttachment depth;
	depth.texture = depthStencilTarget;
	depth.state = RHI::ResourceState::DepthWrite;
	depth.depthLoadOp = NeedsSurfacePass() ? RHI::LoadOp::Load : RHI::LoadOp::Clear;
	depth.depthStoreOp = UsesAdvancedRendering() || m_environmentCapture ? RHI::StoreOp::Store : RHI::StoreOp::Discard;
	if(RHI::HasStencil(depthStencilTarget->GetDesc().format)) // 깊이 형식에 stencil이 있으면 이 패스에서 쓰지 않는 stencil의 load/store를 Discard로 지정한다.
	{
		depth.stencilLoadOp = RHI::LoadOp::Discard;
		depth.stencilStoreOp = RHI::StoreOp::Discard;
	}
	commands.BeginRendering({&color, 1, &depth});
	commands.BindGraphicsPipeline(pipeline);
	commands.SetViewport({0, 0, static_cast<float>(target->GetDesc().width), static_cast<float>(target->GetDesc().height), 0, 1});
	commands.SetScissor({0, 0, target->GetDesc().width, target->GetDesc().height});
    // 표준 입력, 인스턴스 스트림, 기본 fragment 재질 묶기, 별도 투명 처리 여부를 분리한다. 하나의 최적화 토글이 모든 경로를 결정하지 않는다.
    const bool stock=stockDrawInputs;
    const bool instanced=UsesInstanceStream();
	const bool batchBindings = shaderSources.bytes[MeshFragment].empty();
	const bool separateTransparency=m_environmentCapture || wireframeActive || UsesSeparateTransparency();
	// 유효한 재질 ID를 통해 Blend 여부를 판단한다. 무효 재질을 배열에 바로 접근하지 않는다.
	const auto blended=[&](uint32_t index) {
		const auto material=scene.GetEntityMaterial(static_cast<EntityID>(index));
		return IsValid(material) && ToIndex(material)<scene.Materials().size() &&
			scene.Materials()[ToIndex(material)].alphaMode==MaterialAlphaMode::Blend;
	};
	// CPU가 직접 그릴 객체 순서를 만든다. GPU 가시성 경로의 불투명 객체는 간접 배치로 처리하므로 이 목록에서 제외한다.
	mainEntityOrder.clear();
	mainEntityOrder.reserve(scene.GetEntityCount());
	for(uint32_t i=0;i<scene.GetEntityCount();++i)
		if((!separateTransparency || !blended(i)) && (!visibilityActive || blended(i))) mainEntityOrder.push_back(i); // 별도 처리할 Blend와 GPU 간접 처리할 불투명을 CPU 직접 목록에서 제외한다.
	if(batchBindings && !separateTransparency) // 기본 재질을 이 패스에서 직접 합성할 때만 투명 정렬을 수행한다.
	{
		// 불투명을 먼저 두고 투명 부분만 뒤에서 앞으로 정렬한다. 같은 키의 상대 순서를 보존하여 불필요한 순서 변화를 줄인다.
		const auto transparent=std::stable_partition(mainEntityOrder.begin(),mainEntityOrder.end(),[&](uint32_t i) {return !blended(i);});
		// 객체의 로컬 경계 중심을 월드→카메라 공간으로 옮겨 z 정렬 키를 만든다. 경계가 없으면 원점을 쓰고 비유한 z는 0으로 처리한다.
		const auto distance=[&](uint32_t index) {
			const auto entity=static_cast<EntityID>(index);
			const auto mesh=scene.GetEntityMesh(entity);
			Math::float3 local={};
			if(IsValid(mesh) && ToIndex(mesh)<m_meshes.size() && m_meshes[ToIndex(mesh)].bounds.valid) // 유효한 메시 경계가 있을 때만 경계 중심을 정렬 기준으로 쓴다.
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
	if(visibilityActive) // GPU 가시성 준비가 성공한 프레임이면 간접 배치를 먼저 기록한다.
	{
		recordingStatistics.gpuVisibility=true;
		// GPU가 만든 가시 인스턴스/간접 인수를 배치별로 연결한다. CPU는 배치와 LOD를 알지만 실제 GPU 가시 개수는 여기서 읽지 않는다.
		for(uint32_t batchIndex=0;batchIndex<visibilityDraws.size();++batchIndex)
		{
			const auto& draw=visibilityDraws[batchIndex];
			const auto entity=static_cast<EntityID>(draw.entity);
			const auto& mesh=m_meshes[draw.mesh];
			const auto& material=scene.Materials()[draw.material];
			const auto* input=draws ? &(*draws)[draw.entity] : nullptr;
            auto batchContext=Platform::GetLiveContext();
            if(Platform::IsLiveCollectorEnabled()) // 수집기가 켜진 경우에만 간접 배치 출처/인수 버퍼 링크를 남긴다.
            {
                const auto sceneBatch=Platform::GetLiveBatchId(liveId,scene.m_liveId);
                const auto passBatch=Platform::GetLiveBatchId(sceneBatch,1);
                batchContext.batch=Platform::GetLiveBatchId(passBatch,(uint64_t(1)<<63)|batchIndex);
                Platform::LiveEvent batch;batch.id=batchContext.batch;batch.context=batchContext;
                batch.kind=Platform::LiveEventKind::Batch;batch.stage=Platform::LiveStage::Record;
                batch.first=batchIndex;batch.stride=sizeof(RHI::DrawIndexedIndirectArguments);
                batch.bytes=batch.stride;batch.operations=1;
                batch.resource=Platform::FindLiveObject(gpuVisibility->GetArgumentsBuffer());
                // 실제 가시 개수는 GPU가 결정하므로 간접 배치 이벤트에는 미확정으로 남기고 CPU capacity를 측정값으로 게시하지 않는다.
                std::strncpy(batch.label,"GPU indirect batch: visible count unknown",sizeof(batch.label)-1);
                Platform::RecordLiveEvent(batch);
                Platform::RecordLiveLink({Platform::FindLiveObject(gpuVisibility->GetInstanceBuffer()),batch.id});
            }
            Platform::LiveContextScope linkedBatch(batchContext);
			const auto selectedPipeline=MaterialPipeline(material,Math::Determinant3x3(scene.GetTransform(entity))<0);
			if(selectedPipeline!=boundPipeline) // 행렬 변환이 넓이·부피를 얼마나 바꾸고 방향을 뒤집는지 나타내는 값(행렬식)이 음수이면 MaterialPipeline에 반전 여부를 전달한다.
			// 재질과 반전 여부로 고른 파이프라인이 이전 것과 달라지면 새로 바인딩하고 재질 세트 기억도 비운다.
			{
				commands.BindGraphicsPipeline(selectedPipeline);boundPipeline=selectedPipeline;boundMaterial=nullptr;
			}
			auto selectedSet=materialSets[draw.material];
			if(selectedSet && selectedSet->GetPipeline()!=selectedPipeline) // 세트가 다른 파이프라인용이면 같은 바인딩으로 새 파이프라인 세트를 찾는다.
			{
				mainBindingScratch.assign(selectedSet->GetBindings(),selectedSet->GetBindings()+selectedSet->GetBindingCount());
				selectedSet=CachedResourceSet(selectedPipeline,mainBindingScratch,mainSets);
			}
			if(!selectedSet) return false; // 간접 draw에 필요한 세트를 얻지 못하면 기록을 실패 처리한다.
			if(boundMaterial!=selectedSet) // 현재 세트와 다를 때만 중복 없는 바인딩을 기록한다.
			{
				boundMaterial=selectedSet;commands.BindResourceSet(boundMaterial);
			}
			commands.BindVertexBuffer(0,input && input->vertexBuffer ? input->vertexBuffer : mesh.vertexBuffer,0);
			commands.BindVertexBuffer(1,gpuVisibility->GetInstanceBuffer(),
				gpuVisibility->GetBatch(batchIndex).instanceOffset*sizeof(GpuVisibility::VisibleInstance));
			commands.BindIndexBuffer(draw.level ? mesh.lods[draw.level-1].buffer : mesh.indexBuffer,RHI::Format::R32_UINT,0);
			const uint32_t flags=materialStates[draw.material].textureFlags |
				(scene.GetEntityLighting(entity).receiveShadow ? 32u : 0u);
			auto constants=MakeDrawConstants(viewProjection,material,scene.GetTransform(entity),flags);
			if(lodDebugActive) // 간접 배치의 LOD를 진단색으로 표시하고 원래 알파를 보존한다.
			{
				const float alpha=constants.baseColor.w;
				constants.baseColor=LodColor(draw.level);constants.baseColor.w=alpha;constants.textureFlags|=64u;
			}
			constants.padding2=draw.material;
			commands.SetInlineConstants(0,sizeof(constants),&constants);
			// GPU 인수 버퍼의 해당 배치 오프셋을 참조하는 명령을 기록한다. 통계는 draw/간접 배치 수만 늘린다.
			commands.DrawIndexedIndirect(gpuVisibility->GetArgumentsBuffer(),batchIndex*sizeof(RHI::DrawIndexedIndirectArguments));
			++recordingStatistics.mainDrawCount;++recordingStatistics.indirectBatchCount;
		}
	}
	// 직접 경로는 유효 메시/재질 검사 후 선택 LOD와 CPU 절두체 제거를 적용하고 연속 호환 객체를 한 draw로 묶는다.
	for(uint32_t orderIndex = 0; orderIndex < mainEntityOrder.size(); ++orderIndex)
	{
		const uint32_t entityIndex=mainEntityOrder[orderIndex];
		const auto entity = static_cast<EntityID>(entityIndex);
		const auto meshId = scene.GetEntityMesh(entity);
		const auto materialId = scene.GetEntityMaterial(entity);
		if(!IsValid(meshId) || !IsValid(materialId)) continue; // 유효하지 않은 메시/재질 객체는 직접 draw에서 건너뛴다.
		const uint32_t meshIndex = ToIndex(meshId), materialIndex = ToIndex(materialId);
		const uint32_t setIndex = draws && !stockDrawInputs ? entityIndex : materialIndex;
		if(meshIndex >= m_meshes.size() || materialIndex >= materialStates.size() || setIndex >= materialSets.size()) continue; // 메시·재질·세트 배열 범위를 모두 확인한 뒤 접근한다.
		const auto& mesh = m_meshes[meshIndex];
		if(!mesh.prepared || !mesh.vertexBuffer || !mesh.indexBuffer || !mesh.indexCount) continue; // 업로드 준비된 버퍼/인덱스가 없는 메시를 그리지 않는다.
        const auto level=std::min<uint32_t>(EntityLod(entityIndex),mesh.lods.size());
        const auto indexBuffer=level ? mesh.lods[level-1].buffer : mesh.indexBuffer;
        const auto indexCount=level ? mesh.lods[level-1].count : mesh.indexCount;
        const auto& localBounds=draws && (*draws)[entityIndex].hasLocalBounds ? (*draws)[entityIndex].localBounds : mesh.bounds;
        if(stock && config.enableFrustumCulling && OutsideCamera(localBounds,scene.GetTransform(entity),camera)) { // 표준 입력에서 CPU 제거가 켜졌고 경계가 카메라 밖이면 제거 수만 늘리고 draw를 생략한다.
            ++recordingStatistics.culledEntityCount;continue;
        }
        const bool mirrored=Math::Determinant3x3(scene.GetTransform(entity))<0;
        uint32_t count=1;
        // 표준 입력과 인스턴스 스트림에서만 묶기를 시도하며, Blend 제외 조건은 기본 fragment에만 적용한다.
        if(stock && instanced && (!batchBindings || scene.Materials()[materialIndex].alphaMode!=MaterialAlphaMode::Blend)) {
            // 렌더 순서의 다음 항목이 연속 객체 ID일 때만 묶는다. 이어지는 조건은 같은 면 방향·정점 버퍼·LOD·메시·재질·그림자 수신 상태를 요구한다.
            while(orderIndex+count<mainEntityOrder.size() && mainEntityOrder[orderIndex+count]==entityIndex+count) {
                const auto next=static_cast<EntityID>(entityIndex+count);
                if((Math::Determinant3x3(scene.GetTransform(next))<0)!=mirrored) break; // 변환에 따른 면 방향이 다르면 같은 파이프라인의 배치로 묶지 않는다.
                if(draws && (*draws)[entityIndex+count].vertexBuffer!=(*draws)[entityIndex].vertexBuffer) break; // 객체별 정점 버퍼가 다르면 현재 배치를 끝낸다.
                if(EntityLod(entityIndex+count)!=level) break; // 선택된 LOD가 다르면 다른 인덱스 구간을 써야 하므로 배치를 끝낸다.
                const auto nextMaterial=scene.GetEntityMaterial(next);
                if(scene.GetEntityMesh(next)!=meshId || !IsValid(nextMaterial) || ToIndex(nextMaterial)>=materialStates.size())break; // 메시가 다르거나 다음 재질 ID가 무효·범위 밖이면 합치기를 중단한다.
                const auto index=ToIndex(nextMaterial);
                if(scene.GetEntityLighting(next).receiveShadow!=scene.GetEntityLighting(entity).receiveShadow || // 그림자 수신·텍스처·재질 상수가 다르거나 다음 객체가 시야 밖이면 배치를 나눈다.
                    materialStates[index].textures!=materialStates[materialIndex].textures ||
                    materialStates[index].textureFlags!=materialStates[materialIndex].textureFlags ||
                    !EqualMaterial(scene.Materials()[index],scene.Materials()[materialIndex]) ||
                    (config.enableFrustumCulling && OutsideCamera(draws && (*draws)[entityIndex+count].hasLocalBounds ?
                        (*draws)[entityIndex+count].localBounds : mesh.bounds,scene.GetTransform(next),camera)))break;
                ++count;
            }
        }
        auto batchContext=Platform::GetLiveContext();
        if(Platform::IsLiveCollectorEnabled()) // 수집 중이면 직접 배치의 범위·바이트·제한된 이유 분류와 객체 링크를 남긴다.
        {
            const auto sceneBatch=Platform::GetLiveBatchId(liveId,scene.m_liveId);
            const auto passBatch=Platform::GetLiveBatchId(sceneBatch,1);
            batchContext.batch=Platform::GetLiveBatchId(passBatch,(uint64_t(entityIndex)<<32)|count);
            batchContext.object=entityIndex<scene.m_liveEntities.size()?scene.m_liveEntities[entityIndex]:scene.m_liveId;
            Platform::LiveEvent batch;batch.id=batchContext.batch;batch.context=batchContext;
            batch.kind=Platform::LiveEventKind::Batch;batch.stage=Platform::LiveStage::Data;
            batch.first=entityIndex;batch.elements=count;batch.stride=2*sizeof(Math::float4x4);
            batch.bytes=uint64_t(count)*batch.stride;batch.resource=Platform::FindLiveObject(instanceBuffer);
            // main의 관측 이유는 사용자 기하·인스턴싱 OFF·그 밖(None)만 구분한다. 위에서 실제로 배치를 끊은 개별 조건을 별도 이유로 저장하지는 않는다.
            batch.reason=!stock?Platform::LiveSplitReason::CustomGeometry:!instanced?Platform::LiveSplitReason::InstancingDisabled:Platform::LiveSplitReason::None;
            Platform::RecordLiveEvent(batch);
            for(uint32_t i=0;i<count&&entityIndex+i<scene.m_liveEntities.size();++i)
                Platform::RecordLiveLink({scene.m_liveEntities[entityIndex+i],batch.id});
        }
        // 이후 명령을 방금 만든 배치 출처에 연결한다. scope가 끝나면 바깥 frame/source 문맥으로 돌아간다.
        Platform::LiveContextScope linkedBatch(batchContext);
		const auto selectedPipeline=MaterialPipeline(scene.Materials()[materialIndex],mirrored);
		if(selectedPipeline!=boundPipeline) // 직접 draw의 파이프라인이 바뀌면 재질 세트도 다시 바인딩할 수 있게 기억을 비운다.
		{
			commands.BindGraphicsPipeline(selectedPipeline);
			boundPipeline=selectedPipeline;
			boundMaterial=nullptr;
		}
        if(instanced)commands.BindVertexBuffer(1,instanceBuffer,entityIndex*2*sizeof(Math::float4x4)); // 인스턴스 스트림은 객체당 행렬 두 개의 128바이트 간격으로 배치되어 있으므로 첫 객체의 entityIndex*128 오프셋부터 바인딩한다.
		auto selectedSet=materialSets[setIndex];
		if(selectedSet && selectedSet->GetPipeline()!=selectedPipeline) // 반전 등으로 파이프라인이 달라지면 그 파이프라인용 세트를 얻는다.
		{
			mainBindingScratch.assign(selectedSet->GetBindings(),selectedSet->GetBindings()+selectedSet->GetBindingCount());
			selectedSet=CachedResourceSet(selectedPipeline,mainBindingScratch,mainSets);
		}
		if(!selectedSet) return false; // 직접 draw에 필요한 세트가 없으면 실패한다.
		if(!batchBindings || boundMaterial != selectedSet) // 사용자 fragment는 매번 바인딩하고, 기본 경로는 세트가 달라질 때만 바인딩한다.
		{
			boundMaterial = selectedSet;
			commands.BindResourceSet(boundMaterial);
		}
		const auto* input = draws ? &(*draws)[entityIndex] : nullptr;
		commands.BindVertexBuffer(0, input && input->vertexBuffer ? input->vertexBuffer : mesh.vertexBuffer, 0);
		commands.BindIndexBuffer(indexBuffer, RHI::Format::R32_UINT, 0);
		const uint32_t flags = materialStates[materialIndex].textureFlags |
			(scene.GetEntityLighting(entity).receiveShadow ? 32u : 0u);
		auto constants = MakeDrawConstants(viewProjection, scene.Materials()[materialIndex], scene.GetTransform(entity), flags);
		if(lodDebugActive) // 직접 draw도 선택 LOD의 색을 넣고 조명 대신 진단 표시하도록 플래그를 켠다.
		{
			const float alpha=constants.baseColor.w;
			constants.baseColor=LodColor(level);constants.baseColor.w=alpha;
			// 비트 0~4는 텍스처, 5는 그림자 수신, 6은 렌더러의 LOD 시각화다.
			constants.textureFlags|=64u;
		}
		if(m_environmentCapture && !m_environmentCaptureEmitters) constants.emissiveColor={}; // 환경 캡처에서 발광체 포함을 요청하지 않았다면 emissiveColor를 0으로 만들어 재질 발광을 캡처 색에서 제외한다.
		constants.padding2 = materialIndex;
		commands.SetInlineConstants(0, sizeof(constants), &constants);
		if(input && !input->inlineConstants.empty()) // 사용자 추가 상수가 있을 때 기본 DrawConstants 뒤에 이어 기록한다.
			commands.SetInlineConstants(sizeof(constants), static_cast<uint32_t>(input->inlineConstants.size()), input->inlineConstants.data());
        // 선택 LOD의 인덱스와 묶은 객체 수로 명령을 기록한다. 통계는 실제 제출 후보 삼각형과 LOD 전 원본 삼각형을 별도로 누적한다.
        commands.DrawIndexedInstanced(indexCount,count,0,0,0);
        ++recordingStatistics.mainDrawCount;recordingStatistics.mainInstanceCount+=count;
        recordingStatistics.mainTriangleCount+=uint64_t(indexCount/3)*count;
        recordingStatistics.mainOriginalTriangleCount+=uint64_t(mesh.indexCount/3)*count;
        recordingStatistics.lodInstanceCounts[level]+=count;
        // 이미 묶어 기록한 객체를 다음 반복에서 다시 그리지 않도록 순회 위치를 넘긴다.
        orderIndex+=count-1;
	}
	// 렌더 영역 기록을 닫는다. 명령 목록 Close/Submit와 GPU 완료는 호출자 단계에 남아 있다.
	commands.EndRendering();
#ifdef DY_RENDERDOC_ENABLED // RenderDoc 빌드에서 열린 MainForward 디버그 이벤트를 닫는다.
	commands.EndDebugEvent();
#endif
	return true;
}
}
