#include "BasicEnvironmentResources.h"
#include "dyf/RHI/IDevice.h"
#include "dyf/RHI/ICommandList.h"
#include "dyf/RHI/Buffer.h"

namespace dyf
{
// 세 버퍼만 이 객체가 소유한다. 장치가 살아 있는 동안 소멸되어야 파괴 요청을 할 수 있다.
BasicEnvironmentResources::~BasicEnvironmentResources()
{
    if(m_device) for(auto buffer:m_buffers) m_device->DestroyBuffer(buffer); // 장치가 지정되었던 경우에만 보유 버퍼를 정리한다.
}

// 초기화 순서는 CPU의 0 값 준비 → 버퍼 생성 → 복사 기록 → 셰이더가 읽을 상태로 전환이다.
bool BasicEnvironmentResources::Prepare(RHI::IDevice& device,RHI::ICommandList& uploads)
{
    if(m_ready) return true; // 이전에 업로드 제출을 확정했으면 같은 중립 자료를 다시 올리지 않는다.
    m_device=&device;
    // 배열의 두 번째 항목만 구조화 저장 버퍼이고 나머지는 상수 버퍼다. 세 입력을 같은 반복문으로 준비한다.
    const RenderEnvironmentConstants environment={};
    const std::array<Math::float4,12> probe={};
    const std::array<uint32_t,4> count={};
    const void* data[]={&environment,probe.data(),count.data()};
    const uint32_t sizes[]={sizeof(environment),sizeof(probe),sizeof(count)};
    for(uint32_t i=0;i<m_buffers.size();++i)
    {
        const auto usage=i==1 ? RHI::BufferUsage::Storage : RHI::BufferUsage::Constant;
        const auto state=i==1 ? RHI::ResourceState::ShaderResource : RHI::ResourceState::ConstantBuffer;
        if(!m_buffers[i]) // 이미 생성한 슬롯은 재사용하고 없는 슬롯만 만든다.
            m_buffers[i]=device.CreateBuffer({sizes[i],i==1 ? sizes[i] : 0,usage,RHI::ResourceState::CopyDestination});
        if(!m_buffers[i] || !device.UpdateBuffer(uploads,m_buffers[i],0,data[i],sizes[i])) return false; // 할당이나 업로드 기록이 실패하면 즉시 false를 반환하여 호출부가 이번 준비를 중단하게 한다.
        // 복사용 상태로 만든 버퍼를 ConstantBuffer 또는 ShaderResource 상태로 바꿔 이후 셰이더 읽기에 연결한다.
        const RHI::ResourceBarrierDesc ready={m_buffers[i],nullptr,RHI::ResourceState::CopyDestination,state,{}};
        uploads.ResourceBarrier(&ready,1);
    }
    return true;
}

// 환경 확장의 실제 결과 대신 사용할 빈 자원 묶음이다. black의 생성·파괴는 이 클래스가 맡지 않는다.
RenderEnvironmentResources BasicEnvironmentResources::GetResources(RHI::TextureHandle black) const
{
    RenderEnvironmentResources resources;
    resources.constantBuffer=m_buffers[0];
    resources.probeData=m_buffers[1];resources.probeParameters=m_buffers[2];
    resources.prefilter=resources.probePrefilter=black; // 전역·지역 반사 사전 필터 입력을 모두 같은 검은 텍스처로 채운다.
    resources.diffuse.texture=resources.specular.texture=black;
    return resources;
}
}
