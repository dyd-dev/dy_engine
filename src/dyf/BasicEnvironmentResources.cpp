#include "BasicEnvironmentResources.h"
#include "dyf/RHI/IDevice.h"
#include "dyf/RHI/ICommandList.h"
#include "dyf/RHI/Buffer.h"

namespace dyf
{
BasicEnvironmentResources::~BasicEnvironmentResources()
{
    if(m_device)
    {
        for(auto buffer:m_buffers) m_device->DestroyBuffer(buffer);
        m_device->DestroyTexture(m_emptyProbeArray);
    }
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
    // Array shaders require an array view even when the probe count is zero.
    // A two-layer neutral texture has the matching dimensional contract on all backends.
    device.DestroyTexture(m_emptyProbeArray);
    RHI::TextureDesc texture;
    texture.width=texture.height=1;texture.depthOrArraySize=2;
    texture.format=RHI::Format::R8G8B8A8_UNORM;texture.usage=RHI::TextureUsage::ShaderResource;
    m_emptyProbeArray=device.CreateTexture(texture);
    if(!m_emptyProbeArray) return false;
    const RHI::ResourceBarrierDesc writable={nullptr,m_emptyProbeArray,
        RHI::ResourceState::Undefined,RHI::ResourceState::CopyDestination,{}};
    uploads.ResourceBarrier(&writable,1);
    const uint8_t black[4]={0,0,0,0};
    for(uint32_t layer=0;layer<2;++layer)
        if(!device.UpdateTexture(uploads,m_emptyProbeArray,0,layer,black,sizeof(black),4,4)) return false;
    const RHI::ResourceBarrierDesc readable={nullptr,m_emptyProbeArray,
        RHI::ResourceState::CopyDestination,RHI::ResourceState::ShaderResource,{}};
    uploads.ResourceBarrier(&readable,1);
    return true;
}

RenderEnvironmentResources BasicEnvironmentResources::GetResources(RHI::TextureHandle black) const
{
    RenderEnvironmentResources resources;
    resources.constantBuffer=m_buffers[0];
    resources.probeData=m_buffers[1];resources.probeParameters=m_buffers[2];
    resources.prefilter=black;resources.probePrefilter=m_emptyProbeArray;
    resources.diffuse.texture=resources.specular.texture=black;
    return resources;
}
}
