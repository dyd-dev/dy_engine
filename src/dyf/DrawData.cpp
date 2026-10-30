#include "../Platform/Profiler/CollectorInternal.h"
#include "RendererInternal.h"
#include "SceneAlgorithms.h"
#include "dyf/Scene.h"
#include "dyf/RHI/Buffer.h"
#include "dyf/RHI/ICommandList.h"
#include <algorithm>

namespace dyf
{
// UpdateBuffer가 CPU 바이트를 복사한다. 같은 큐의 업로드와 draw 사이에는 CPU 대기가 필요 없다.
// 장면 생존 식별자·변환 revision·객체 수를 비교하여 변경 없는 업로드를 생략한다. 버퍼 교체의 확정/복구는 호출부의 FrameBuffers 범위 보호가 담당한다.
bool Renderer::Impl::UpdateInstanceBuffer(const Scene& scene,RHI::ICommandList& commands)
{
    pendingInstanceScene=scene.m_lifetime;
    pendingInstanceRevision=scene.m_transformRevision;
    pendingInstanceCount=scene.GetEntityCount();
    // 직접/그림자 경로가 사용할 때만 공용 인스턴스 버퍼를 준비한다. 압축 배치만 쓰는 프레임은 불필요한 업로드를 생략한다.
    const bool separateTransparency=m_environmentCapture || wireframeActive || UsesSeparateTransparency();
    const bool legacyDraws=(!visibilityActive && !cpuBatchesActive) || (!separateTransparency &&
        std::any_of(scene.Materials().begin(),scene.Materials().end(),[](const MaterialDesc& material) {
            return UsesBlendedRendering(material);
        }));
    const bool shadows=config.lighting.enabled && config.lighting.direct && config.lighting.shadows;
    if(!UsesInstanceStream() || !pendingInstanceCount || (!legacyDraws && !shadows)) {
        instanceBuffer=nullptr;instanceReady=false;return true;
    }
    // GPU 가시 목록과 같은 128바이트 간격을 사용한다. 기본 셰이더는 앞 행렬만 읽는다.
    constexpr uint32_t stride=2*sizeof(Math::float4x4);
    if(pendingInstanceCount>UINT32_MAX/stride)return false; // 32비트 버퍼 크기에 담을 수 없는 객체 수면 실패한다.
    const auto bytes=static_cast<uint32_t>(pendingInstanceCount*stride);
    if(!instanceBuffer || instanceBuffer->GetDesc().size!=bytes) { // 버퍼가 없거나 크기가 다르면 CopyDestination 버퍼로 교체하고 instanceReady를 끈다. 이전 버퍼의 해제 또는 실패 시 복구는 호출부의 FrameBuffers가 맡는다.
        instanceBuffer=device->CreateBuffer({bytes,stride,RHI::BufferUsage::Vertex,RHI::ResourceState::CopyDestination});
        instanceReady=false;
    }
    if(!instanceBuffer)return false; // 버퍼 생성 실패는 업로드 실패로 전달한다.
    // weak_ptr의 소유 제어 블록을 양방향으로 비교한다. 주소 재사용만으로 다른 Scene을 같은 장면으로 잘못 취급하지 않는다.
    const bool sameScene=!instanceScene.expired() &&
        !instanceScene.owner_before(pendingInstanceScene) && !pendingInstanceScene.owner_before(instanceScene);
    // 최적화 OFF·새 버퍼·다른 Scene·객체 수 변화 중 하나면 전체 업로드가 필요하다. 동일 Scene의 변경 revision만 부분 업로드 후보가 된다.
    bool full=!config.enableIncrementalTransformUploads || !instanceReady || !sameScene || instanceCount!=pendingInstanceCount;
    auto fullReason=!instanceReady?Platform::LiveSplitReason::NewResource:!sameScene?Platform::LiveSplitReason::SceneChanged:instanceCount!=pendingInstanceCount?Platform::LiveSplitReason::ElementCountChanged:Platform::LiveSplitReason::None;
    if(!full && instanceRevision==pendingInstanceRevision)return true; // 전체 업로드 조건이 없고 마지막 제출 성공 revision과 이번 revision이 같으면 업로드 기록을 생략한다.

    instanceScratch.resize(size_t(pendingInstanceCount)*2);
    instanceRanges.clear();
    if(!full) { // 전체 업로드가 필요 없을 때만 객체별 변경 revision을 훑어 구간을 만든다.
        for(uint32_t i=0;i<pendingInstanceCount;++i) {
            if(scene.m_entityTransformRevisions[i]<=instanceRevision)continue; // 마지막 제출 성공 때 저장한 instanceRevision보다 최신이 아닌 객체는 변경 구간에서 제외한다.
            if(!instanceRanges.empty() && instanceRanges.back().first+instanceRanges.back().count==i) // 직전 변경 구간 바로 다음 객체라면 구간을 늘려 연속 복사로 합친다.
                ++instanceRanges.back().count;
            else instanceRanges.push_back({i,1});
        }
    }
    if(full) { // 전체 업로드라면 모든 객체를 덮는 하나의 구간으로 바꾼다.
        instanceRanges.clear();instanceRanges.push_back({0,pendingInstanceCount});
    }
    // 선택한 구간의 현재 행렬을 채우고 두 번째 행렬 공간은 0으로 둔다. 표준 셰이더는 첫 행렬을 읽으며 GPU 가시 인스턴스와 같은 128바이트 간격을 유지한다.
    for(const auto& range:instanceRanges)
        for(uint32_t i=range.first;i<range.first+range.count;++i)
        {
            instanceScratch[size_t(i)*2]=scene.GetTransform(static_cast<EntityID>(i));
            instanceScratch[size_t(i)*2+1]={};
        }
    if(instanceRanges.empty())return true; // 변경 구간이 없으면 배리어나 업로드도 기록하지 않는다.
    if(instanceReady) { // 이전에 정점 읽기에 사용한 버퍼면 복사 목적 상태로 되돌린 뒤 갱신한다.
        const RHI::ResourceBarrierDesc barrier={instanceBuffer,nullptr,RHI::ResourceState::VertexBuffer,RHI::ResourceState::CopyDestination,{}};
        commands.ResourceBarrier(&barrier,1);
    }
    for(const auto& range:instanceRanges) {
        if(!device->UpdateBuffer(commands,instanceBuffer,range.first*stride, // 구간 업로드 기록이 실패하면 false를 반환한다. 호출부는 이 프레임의 Scene/revision을 제출 성공 기준으로 확정하지 않는다.
            instanceScratch.data()+size_t(range.first)*2,range.count*stride))return false;
        // 데이터 범위 이벤트에 Scene 출처·원본 버퍼·첫 객체·개수·바이트·간격·전체 업로드 이유를 함께 남긴다. 기록된 전송량이며 GPU 완료 시간을 뜻하지 않는다.
        Platform::LiveEvent event;event.context=Platform::GetLiveContext();event.context.object=scene.m_liveId;
        event.kind=Platform::LiveEventKind::DataRange;event.stage=Platform::LiveStage::Data;
        event.resource=Platform::FindLiveObject(instanceBuffer);event.first=range.first;event.elements=range.count;
        event.bytes=uint64_t(range.count)*stride;event.stride=stride;event.operations=1;event.fullUpload=full;event.reason=fullReason;
        Platform::RecordLiveEvent(event);
    }
    const RHI::ResourceBarrierDesc ready={instanceBuffer,nullptr,RHI::ResourceState::CopyDestination,RHI::ResourceState::VertexBuffer,{}};
    commands.ResourceBarrier(&ready,1);
    return true;
}

// 호출자가 업로드 명령 제출 성공을 확인한 뒤 부른다. 이번 Scene/revision/상수 바이트를 다음 프레임의 동일성 비교 기준으로 옮긴다.
void Renderer::Impl::CommitFrameData()
{
    instanceScene=pendingInstanceScene;instanceRevision=pendingInstanceRevision;instanceCount=pendingInstanceCount;
    if(cpuBatchesActive)cpuInstanceBytes.swap(cpuInstanceScratch);
    lightingBytes.swap(pendingLightingBytes);
    if(config.lighting.enabled && config.lighting.direct && config.lighting.shadows)shadowBytes.swap(pendingShadowBytes); // 그림자 기능이 실제 활성일 때만 이번 그림자 상수 바이트를 재사용 기준으로 확정한다.
}
}
