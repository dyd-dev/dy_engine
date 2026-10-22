#include "BasicLightingResources.h"
#include "dyf/Camera.h"
#include "dyf/RHI/Buffer.h"
#include "dyf/RHI/IDevice.h"
#include "dyf/RHI/ICommandList.h"
#include <algorithm>
#include <cstring>

namespace dyf
{
using namespace RHI;

// 소멸과 명시적 초기화를 같은 Reset 경로로 모아 버퍼 소유권 정리가 한 곳에서 이루어지게 한다.
BasicLightingResources::~BasicLightingResources()
{
    Reset();
}

// 장치가 살아 있을 때 버퍼들을 해제하고 핸들·캐시·준비 상태를 모두 초기값으로 되돌린다.
void BasicLightingResources::Reset()
{
    if(m_device) // 이전에 연결한 장치가 있을 때만 그 장치에 버퍼 파괴를 요청한다.
        for(auto& buffer:m_buffers) m_device->DestroyBuffer(buffer.buffer);
    m_buffers={};
    m_device=nullptr;
    m_prepared=false;
}

// 한 슬롯의 크기와 CPU 내용을 비교한 뒤 필요한 경우에만 복사 명령 및 앞뒤 자원 전환을 기록한다.
bool BasicLightingResources::Upload(ICommandList& commands,uint32_t slot,const void* data,
    uint32_t bytes,uint32_t stride,BufferUsage usage,ResourceState state)
{
    auto& target=m_buffers[slot];
    if(!target.buffer || target.buffer->GetDesc().size!=bytes) // 버퍼가 없거나 바이트 크기가 달라졌으면 기존 버퍼를 버리고 정확한 크기로 만든다.
    {
        m_device->DestroyBuffer(target.buffer);
        target.buffer=m_device->CreateBuffer({bytes,stride,usage,ResourceState::CopyDestination});
        target.ready=false;
        target.bytes.clear();
    }
    if(!target.buffer) return false; // 생성 실패를 다음 단계로 숨기지 않고 호출부에 전달한다.
    if(target.ready && target.bytes.size()==bytes && std::memcmp(target.bytes.data(),data,bytes)==0) return true; // 이미 기록한 내용과 바이트 단위로 같으면 업로드와 장벽을 모두 생략한다.
    if(target.ready) // 이전 사용 상태에 있는 버퍼를 다시 쓸 때만 CopyDestination으로 되돌린다.
    {
        const ResourceBarrierDesc before{target.buffer,nullptr,state,ResourceState::CopyDestination,{}};
        commands.ResourceBarrier(&before,1);
    }
    if(!m_device->UpdateBuffer(commands,target.buffer,0,data,bytes)) return false; // 복사 기록 실패 시 캐시와 ready를 새 내용으로 확정하지 않는다.
    const ResourceBarrierDesc after{target.buffer,nullptr,ResourceState::CopyDestination,state,{}};
    commands.ResourceBarrier(&after,1);
    const auto* first=static_cast<const uint8_t*>(data);
    // 복사 기록과 읽기 상태 전환이 성공한 뒤 비교용 CPU 사본을 갱신한다. 제출 실패 시에는 상위 호출부가 Reset해야 한다.
    target.bytes.assign(first,first+bytes);
    target.ready=true;
    return true;
}

// 지역 광원 목록을 만든 뒤 광원 배열·더미 목록·카메라 상수를 같은 업로드 명령 목록에 기록한다.
bool BasicLightingResources::Prepare(IDevice& device,ICommandList& uploads,const Scene& scene,
    const Camera& camera,bool active)
{
    if(m_device && m_device!=&device) Reset(); // 장치가 바뀌면 이전 장치의 핸들을 새 장치에서 쓰지 않도록 먼저 정리한다.
    m_device=&device;
    m_prepared=false;
    if(!m_localLights.Prepare(device,scene,active)) return false; // 원본 인덱스가 32비트 범위를 벗어나거나 광원 한 개분도 저장할 수 없거나 활성 광원 배열이 장치 저장 버퍼 한도를 넘으면 false를 반환한다. m_prepared는 false로 남아 Import를 막는다.
    const auto& lights=m_localLights.lights;
    m_data={};
    // flat 경로에서도 셰이더와 동일한 상수 배치를 사용한다. counts.x만 실제 광원 수이고 나머지는 0이다.
    m_data.view=camera.view;
    m_data.projection=camera.projection;
    m_data.counts={static_cast<uint32_t>(lights.size()),0,0,0};
    // 광원이 0개여도 유효한 저장 버퍼 바인딩이 필요하므로 0으로 채운 한 항목을 올린다. 실제 평가는 counts.x=0으로 막는다.
    const RendererClusterLight empty={};
    const uint32_t lightBytes=static_cast<uint32_t>(std::max<size_t>(1,lights.size())*sizeof(RendererClusterLight));
    const uint32_t unusedList=0;
    if(!Upload(uploads,0,lights.empty()?&empty:lights.data(),lightBytes,sizeof(RendererClusterLight), // 세 슬롯 중 하나라도 기록하지 못하면 m_prepared를 true로 만들지 않는다.
            BufferUsage::Storage,ResourceState::ShaderResource) ||
        !Upload(uploads,1,&unusedList,sizeof(unusedList),sizeof(unusedList),
            BufferUsage::Storage,ResourceState::ShaderResource) ||
        !Upload(uploads,2,&m_data,sizeof(m_data),sizeof(m_data),
            BufferUsage::Constant,ResourceState::ConstantBuffer)) return false;
    m_prepared=true;
    return true;
}

// 외부 소유 버퍼를 그래프에 등록한다. Import는 메모리 업로드나 버퍼 소유권 이전을 수행하는 단계가 아니다.
bool BasicLightingResources::Import(RenderGraph& graph,RenderLightingResources& lighting)
{
    if(!m_prepared) return false; // Prepare 전체가 성공하지 않은 묶음은 프레임 조명 입력으로 공개하지 않는다.
    const auto lights=graph.ImportBuffer("Flat local lights",GetLights(),ResourceState::ShaderResource,ResourceState::ShaderResource);
    const auto list=graph.ImportBuffer("Flat local light list",GetList(),ResourceState::ShaderResource,ResourceState::ShaderResource);
    const auto constants=graph.ImportBuffer("Flat local light parameters",GetConstants(),ResourceState::ConstantBuffer,ResourceState::ConstantBuffer);
    if(!lights.IsValid() || !list.IsValid() || !constants.IsValid()) return false; // 세 그래프 자원 중 하나라도 등록 실패하면 조명 출력 묶음을 완성하지 않는다.
    lighting.clusterLights={GetLights(),lights};
    lighting.clusterList={GetList(),list};
    lighting.clusterConstants={GetConstants(),constants};
    lighting.clustered=false; // 공간 분류 목록이 아니라 전체 광원 배열을 순회하는 경로임을 셰이더 쪽에 알린다.
    return true;
}
}
