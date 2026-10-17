#include "BasicEnvironmentResources.h"
#include "dyf/RHI/IDevice.h"
#include "dyf/RHI/ICommandList.h"
#include "dyf/RHI/Buffer.h"

namespace dyf
{
BasicEnvironmentResources::~BasicEnvironmentResources()
{
    if(m_device) for(auto buffer:m_buffers) m_device->DestroyBuffer(buffer);
}

bool BasicEnvironmentResources::Prepare(RHI::IDevice& device,RHI::ICommandList& uploads)
{
    if(m_ready) return true;
    m_device=&device;
    const RenderEnvironmentConstants environment={};
    const std::array<Math::float4,12> probe={};
    const std::array<uint32_t,4> count={};
    const void* data[]={&environment,probe.data(),count.data()};
    const uint32_t sizes[]={sizeof(environment),sizeof(probe),sizeof(count)};
    for(uint32_t i=0;i<m_buffers.size();++i)
    {
        const auto usage=i==1 ? RHI::BufferUsage::Storage : RHI::BufferUsage::Constant;
        const auto state=i==1 ? RHI::ResourceState::ShaderResource : RHI::ResourceState::ConstantBuffer;
        if(!m_buffers[i])
            m_buffers[i]=device.CreateBuffer({sizes[i],i==1 ? sizes[i] : 0,usage,RHI::ResourceState::CopyDestination});
        if(!m_buffers[i] || !device.UpdateBuffer(uploads,m_buffers[i],0,data[i],sizes[i])) return false;
        const RHI::ResourceBarrierDesc ready={m_buffers[i],nullptr,RHI::ResourceState::CopyDestination,state,{}};
        uploads.ResourceBarrier(&ready,1);
    }
    return true;
}

RenderEnvironmentResources BasicEnvironmentResources::GetResources(RHI::TextureHandle black) const
{
    RenderEnvironmentResources resources;
    resources.constantBuffer=m_buffers[0];
    resources.probeData=m_buffers[1];resources.probeParameters=m_buffers[2];
    resources.prefilter=resources.probePrefilter=black;
    resources.diffuse.texture=resources.specular.texture=black;
    return resources;
}
}
