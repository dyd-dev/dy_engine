#include "ClusteredLighting.h"
#include "dyf/Camera.h"
#include "dyf/Scene.h"
#include "dyf/RHI/Buffer.h"
#include "dyf/RHI/IDevice.h"
#include "dyf/RHI/Shader.h"
#include "dyf/RHI/ICommandList.h"
#include "dyf/RHI/Pipeline.h"
#include "dyf/RHI/Binding.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#if defined(ENABLE_METAL) || defined(ENABLE_VULKAN) || defined(ENABLE_D3D12)
#include "ClusteredLightingCompute.h"
#endif

namespace dyf
{
using namespace RHI;

ClusteredLighting::~ClusteredLighting() { Reset(); }

void ClusteredLighting::Reset()
{
    if(m_device)
    {
        m_device->DestroyResourceSet(m_set);
        m_device->DestroyPipeline(m_pipeline);
        m_device->DestroyShader(m_shader);
        m_device->DestroyBuffer(m_lights);
        m_device->DestroyBuffer(m_list);
        m_device->DestroyBuffer(m_constants);
    }
    m_device=nullptr;m_set=nullptr;m_pipeline=nullptr;m_shader=nullptr;
    m_lights=m_list=m_constants=nullptr;m_setBuffers={};
    m_lightBytes.clear();m_constantBytes.clear();m_listBytes.clear();
    m_lightsReady=m_constantsReady=m_listReady=m_prepared=m_recorded=false;
    m_lightsHandle=m_listHandle=m_constantsHandle={};
}

bool ClusteredLighting::CreatePipeline()
{
    if(m_pipeline) return true;
#if defined(ENABLE_METAL) || defined(ENABLE_VULKAN) || defined(ENABLE_D3D12)
#if defined(ENABLE_METAL)
    const char* entry="clusteredLightingCompute";
#else
    const char* entry="main";
#endif
    if(!m_shader) m_shader=m_device->CreateShader({ShaderStage::Compute,entry,kClusteredLightingCompute,kClusteredLightingComputeSize});
#endif
    if(!m_shader) return false;
    const ResourceBindingLayout bindings[]={
        {19,ResourceBindingType::ReadOnlyStorageBuffer,1,ShaderStageFlags::Compute,{}},
        {20,ResourceBindingType::ReadWriteStorageBuffer,1,ShaderStageFlags::Compute,{}},
        {21,ResourceBindingType::ConstantBuffer,1,ShaderStageFlags::Compute,{}}};
    m_pipeline=m_device->CreateComputePipeline({m_shader,{bindings,3,0,ShaderStageFlags::None,0},{64,1,1}});
    return m_pipeline!=nullptr;
}

bool ClusteredLighting::Upload(ICommandList& commands,BufferHandle& buffer,bool& ready,std::vector<uint8_t>& cached,
    const void* data,uint32_t size,uint32_t stride,BufferUsage usage,ResourceState state)
{
    if(!buffer || buffer->GetDesc().size!=size)
    {
        m_device->DestroyResourceSet(m_set);m_set=nullptr;m_setBuffers={};
        m_device->DestroyBuffer(buffer);
        buffer=m_device->CreateBuffer({size,stride,usage,ResourceState::CopyDestination});
        ready=false;cached.clear();
    }
    if(!buffer) return false;
    if(ready && cached.size()==size && std::memcmp(cached.data(),data,size)==0) return true;
    if(ready)
    {
        const ResourceBarrierDesc before{buffer,nullptr,state,ResourceState::CopyDestination,{}};
        commands.ResourceBarrier(&before,1);
    }
    if(!m_device->UpdateBuffer(commands,buffer,0,data,size)) return false;
    const ResourceBarrierDesc after{buffer,nullptr,ResourceState::CopyDestination,state,{}};
    commands.ResourceBarrier(&after,1);
    const auto* first=static_cast<const uint8_t*>(data);cached.assign(first,first+size);ready=true;
    return true;
}

bool ClusteredLighting::Prepare(IDevice& device,ICommandList& uploads,const Scene& scene,const Camera& camera,bool enabled,bool active)
{
    if(m_device && m_device!=&device) Reset();
    m_device=&device;m_prepared=m_recorded=false;
    const uint64_t maximumBytes=std::min<uint64_t>(UINT32_MAX,device.GetLimit(Limit::StorageBufferBytes));
    if(!m_localLights.Prepare(device,scene,active)) return false;
    auto& lights=m_localLights.lights;
    m_data={};m_data.view=camera.view;m_data.projection=camera.projection;
    const auto& p=camera.projection;
    const bool perspective=p.m[11]==-1 && p.m[15]==0;
    const bool orthographic=p.m[11]==0 && p.m[15]==1;
    const float nearDepth=p.m[14]/p.m[10];
    const float farDepth=perspective?p.m[14]/(p.m[10]+1):(p.m[14]-1)/p.m[10];
    bool volume=enabled && !lights.empty() && device.Supports(Feature::Compute) &&
        (perspective || orthographic) && nearDepth>0 && farDepth>nearDepth && std::isfinite(farDepth) &&
        p.m[1]==0 && p.m[2]==0 && p.m[3]==0 && p.m[4]==0 && p.m[6]==0 && p.m[7]==0 &&
        p.m[0]!=0 && p.m[5]!=0 && p.m[10]!=0 &&
        (perspective?(p.m[12]==0 && p.m[13]==0):(p.m[8]==0 && p.m[9]==0)) &&
        Math::Inverse(p,m_data.inverseProjection);
    for(float value:camera.view.m) volume=volume && std::isfinite(value);
    for(float value:p.m) volume=volume && std::isfinite(value);
    float squaredScale=0;
    bool rigid=true;
    for(uint32_t a=0;a<3;++a)for(uint32_t b=0;b<3;++b)
    {
        float product=0;
        for(uint32_t k=0;k<3;++k) product+=camera.view.m[a*4+k]*camera.view.m[b*4+k];
        rigid=rigid && std::fabs(product-(a==b?1.f:0.f))<1e-4f;
        if(a==b) squaredScale+=product;
    }
    // 일반 카메라는 단위 회전이며, 직접 편집한 비직교 뷰는 Frobenius 상계로 반경을 넓힌다.
    const float radiusScale=rigid?1.001f:std::sqrt(squaredScale);
    volume=volume && camera.view.m[3]==0 && camera.view.m[7]==0 && camera.view.m[11]==0 && camera.view.m[15]==1;
    // The grid is a fixed storage/work budget, independent of scene light
    // count. Exceeding device capacity uses the complete light list instead
    // of switching grid size at an estimated performance break-even point.
    const uint64_t volumeListBytes=uint64_t{m_data.dimensions[0]}*m_data.dimensions[1]*m_data.dimensions[2]*
        (m_data.dimensions[3]+1)*sizeof(uint32_t);
    volume=volume && volumeListBytes<=maximumBytes;
    m_data.depth={volume?nearDepth:.1f,volume?farDepth:100.f,volume?std::log(farDepth/nearDepth):std::log(1000.f),radiusScale};
    m_data.counts={static_cast<uint32_t>(lights.size()),volume?1u:0u,perspective?1u:0u,0};
    if(lights.empty()) lights.emplace_back();
    if(!Upload(uploads,m_lights,m_lightsReady,m_lightBytes,lights.data(),static_cast<uint32_t>(lights.size()*sizeof(RendererClusterLight)),
        sizeof(RendererClusterLight),BufferUsage::Storage,ResourceState::ShaderResource) ||
        !Upload(uploads,m_constants,m_constantsReady,m_constantBytes,&m_data,sizeof(m_data),sizeof(m_data),
        BufferUsage::Constant,ResourceState::ConstantBuffer)) return false;
    const uint32_t listBytes=volume?static_cast<uint32_t>(volumeListBytes):sizeof(uint32_t);
    if(listBytes>maximumBytes) {std::fputs("dyf: Cluster light lists exceed the device storage-buffer limit.\n",stderr);return false;}
    if(!m_list || m_list->GetDesc().size!=listBytes)
    {
        std::vector<uint32_t> zero(listBytes/sizeof(uint32_t));
        if(!Upload(uploads,m_list,m_listReady,m_listBytes,zero.data(),listBytes,sizeof(uint32_t),
            BufferUsage::Storage,ResourceState::ShaderResource)) return false;
    }
    if(volume)
    {
        if(!CreatePipeline()) return false;
        const std::array<BufferHandle,3> buffers={m_lights,m_list,m_constants};
        if(!m_set || buffers!=m_setBuffers)
        {
            device.DestroyResourceSet(m_set);m_set=nullptr;
            const ResourceBinding bindings[]={{19,0,m_lights,nullptr,0,m_lights->GetDesc().size,{}},
                {20,0,m_list,nullptr,0,m_list->GetDesc().size,{}},{21,0,m_constants,nullptr,0,sizeof(m_data),{}}};
            m_set=device.CreateResourceSet({m_pipeline,bindings,3});
            if(!m_set) return false;
            m_setBuffers=buffers;
        }
    }
    m_prepared=true;return true;
}

bool ClusteredLighting::AddPasses(RenderGraph& graph)
{
    if(!m_prepared || m_recorded) return false;
    m_lightsHandle=graph.ImportBuffer("Cluster local lights",m_lights,ResourceState::ShaderResource,ResourceState::ShaderResource);
    m_listHandle=graph.ImportBuffer("Cluster light lists",m_list,ResourceState::ShaderResource,ResourceState::ShaderResource);
    m_constantsHandle=graph.ImportBuffer("Cluster camera",m_constants,ResourceState::ConstantBuffer,ResourceState::ConstantBuffer);
    if(!m_lightsHandle.IsValid() || !m_listHandle.IsValid() || !m_constantsHandle.IsValid()) return false;
    if(m_data.counts[1])
    {
        graph.AddPass("Cluster local light assignment", RHI::RGRecordingMode::Independent).Read(m_lightsHandle,ResourceState::ShaderResource)
            .Read(m_constantsHandle,ResourceState::ConstantBuffer).Write(m_listHandle,ResourceState::UnorderedAccess)
            .SetExecute([this](ICommandList* commands) {
                commands->BindComputePipeline(m_pipeline);commands->BindResourceSet(m_set);
                const uint32_t count=m_data.dimensions[0]*m_data.dimensions[1]*m_data.dimensions[2];
                commands->Dispatch((count+63)/64,1,1);
            });
    }
    m_recorded=true;return true;
}

void ClusteredLighting::CommitFrame() { m_prepared=m_recorded=false; }
}
