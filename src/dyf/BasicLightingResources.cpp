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

BasicLightingResources::~BasicLightingResources()
{
    Reset();
}

void BasicLightingResources::Reset()
{
    if(m_device)
        for(auto& buffer:m_buffers) m_device->DestroyBuffer(buffer.buffer);
    m_buffers={};
    m_device=nullptr;
    m_prepared=false;
}

bool BasicLightingResources::Upload(ICommandList& commands,uint32_t slot,const void* data,
    uint32_t bytes,uint32_t stride,BufferUsage usage,ResourceState state)
{
    auto& target=m_buffers[slot];
    if(!target.buffer || target.buffer->GetDesc().size!=bytes)
    {
        m_device->DestroyBuffer(target.buffer);
        target.buffer=m_device->CreateBuffer({bytes,stride,usage,ResourceState::CopyDestination});
        target.ready=false;
        target.bytes.clear();
    }
    if(!target.buffer) return false;
    if(target.ready && target.bytes.size()==bytes && std::memcmp(target.bytes.data(),data,bytes)==0) return true;
    if(target.ready)
    {
        const ResourceBarrierDesc before{target.buffer,nullptr,state,ResourceState::CopyDestination,{}};
        commands.ResourceBarrier(&before,1);
    }
    if(!m_device->UpdateBuffer(commands,target.buffer,0,data,bytes)) return false;
    const ResourceBarrierDesc after{target.buffer,nullptr,ResourceState::CopyDestination,state,{}};
    commands.ResourceBarrier(&after,1);
    const auto* first=static_cast<const uint8_t*>(data);
    target.bytes.assign(first,first+bytes);
    target.ready=true;
    return true;
}

bool BasicLightingResources::Prepare(IDevice& device,ICommandList& uploads,const Scene& scene,
    const Camera& camera,bool active)
{
    if(m_device && m_device!=&device) Reset();
    m_device=&device;
    m_prepared=false;
    if(!m_localLights.Prepare(device,scene,active)) return false;
    const auto& lights=m_localLights.lights;
    m_data={};
    m_data.view=camera.view;
    m_data.projection=camera.projection;
    m_data.counts={static_cast<uint32_t>(lights.size()),0,0,0};
    const RendererClusterLight empty={};
    const uint32_t lightBytes=static_cast<uint32_t>(std::max<size_t>(1,lights.size())*sizeof(RendererClusterLight));
    const uint32_t unusedList=0;
    if(!Upload(uploads,0,lights.empty()?&empty:lights.data(),lightBytes,sizeof(RendererClusterLight),
            BufferUsage::Storage,ResourceState::ShaderResource) ||
        !Upload(uploads,1,&unusedList,sizeof(unusedList),sizeof(unusedList),
            BufferUsage::Storage,ResourceState::ShaderResource) ||
        !Upload(uploads,2,&m_data,sizeof(m_data),sizeof(m_data),
            BufferUsage::Constant,ResourceState::ConstantBuffer)) return false;
    m_prepared=true;
    return true;
}

bool BasicLightingResources::Import(RenderGraph& graph,RenderLightingResources& lighting)
{
    if(!m_prepared) return false;
    const auto lights=graph.ImportBuffer("Flat local lights",GetLights(),ResourceState::ShaderResource,ResourceState::ShaderResource);
    const auto list=graph.ImportBuffer("Flat local light list",GetList(),ResourceState::ShaderResource,ResourceState::ShaderResource);
    const auto constants=graph.ImportBuffer("Flat local light parameters",GetConstants(),ResourceState::ConstantBuffer,ResourceState::ConstantBuffer);
    if(!lights.IsValid() || !list.IsValid() || !constants.IsValid()) return false;
    lighting.clusterLights={GetLights(),lights};
    lighting.clusterList={GetList(),list};
    lighting.clusterConstants={GetConstants(),constants};
    lighting.clustered=false;
    return true;
}
}
