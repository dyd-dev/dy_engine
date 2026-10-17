#include "RendererInternal.h"
#include "dyf/Scene.h"
#include "dyf/RHI/Buffer.h"
#include "dyf/RHI/ICommandList.h"

namespace dyf
{
// UpdateBuffer가 CPU 바이트를 복사한다. 같은 큐의 업로드와 draw 사이에는 CPU 대기가 필요 없다.
bool Renderer::Impl::UpdateInstanceBuffer(const Scene& scene,RHI::ICommandList& commands)
{
    pendingInstanceScene=scene.m_lifetime;
    pendingInstanceRevision=scene.m_transformRevision;
    pendingInstanceCount=scene.GetEntityCount();
    if(!UsesInstanceStream() || !pendingInstanceCount) {
        instanceBuffer=nullptr;instanceReady=false;return true;
    }
    // GPU 가시 목록과 같은 128바이트 간격을 사용한다. 기본 셰이더는 앞 행렬만 읽는다.
    constexpr uint32_t stride=2*sizeof(Math::float4x4);
    if(pendingInstanceCount>UINT32_MAX/stride)return false;
    const auto bytes=static_cast<uint32_t>(pendingInstanceCount*stride);
    if(!instanceBuffer || instanceBuffer->GetDesc().size!=bytes) {
        instanceBuffer=device->CreateBuffer({bytes,stride,RHI::BufferUsage::Vertex,RHI::ResourceState::CopyDestination});
        instanceReady=false;
    }
    if(!instanceBuffer)return false;
    const bool sameScene=!instanceScene.expired() &&
        !instanceScene.owner_before(pendingInstanceScene) && !pendingInstanceScene.owner_before(instanceScene);
    bool full=!instanceReady || !sameScene || instanceCount!=pendingInstanceCount;
    if(!full && instanceRevision==pendingInstanceRevision)return true;

    instanceScratch.resize(size_t(pendingInstanceCount)*2);
    instanceRanges.clear();
    if(!full) {
        for(uint32_t i=0;i<pendingInstanceCount;++i) {
            if(scene.m_entityTransformRevisions[i]<=instanceRevision)continue;
            if(!instanceRanges.empty() && instanceRanges.back().first+instanceRanges.back().count==i)
                ++instanceRanges.back().count;
            else instanceRanges.push_back({i,1});
            // 잘게 나뉜 복사 구간이 많아지면 전체 업로드로 합친다.
            if(instanceRanges.size()>32) {full=true;break;}
        }
    }
    if(full) {
        instanceRanges.clear();instanceRanges.push_back({0,pendingInstanceCount});
    }
    for(const auto& range:instanceRanges)
        for(uint32_t i=range.first;i<range.first+range.count;++i)
        {
            instanceScratch[size_t(i)*2]=scene.GetTransform(static_cast<EntityID>(i));
            instanceScratch[size_t(i)*2+1]={};
        }
    if(instanceRanges.empty())return true;
    if(instanceReady) {
        const RHI::ResourceBarrierDesc barrier={instanceBuffer,nullptr,RHI::ResourceState::VertexBuffer,RHI::ResourceState::CopyDestination,{}};
        commands.ResourceBarrier(&barrier,1);
    }
    for(const auto& range:instanceRanges)
        if(!device->UpdateBuffer(commands,instanceBuffer,range.first*stride,
            instanceScratch.data()+size_t(range.first)*2,range.count*stride))return false;
    const RHI::ResourceBarrierDesc ready={instanceBuffer,nullptr,RHI::ResourceState::CopyDestination,RHI::ResourceState::VertexBuffer,{}};
    commands.ResourceBarrier(&ready,1);
    return true;
}

void Renderer::Impl::CommitFrameData()
{
    instanceScene=pendingInstanceScene;instanceRevision=pendingInstanceRevision;instanceCount=pendingInstanceCount;
    lightingBytes.swap(pendingLightingBytes);
    if(config.lighting.enabled && config.lighting.direct && config.lighting.shadows)shadowBytes.swap(pendingShadowBytes);
}
}
