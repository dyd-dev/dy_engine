#include "GpuVisibility.h"
#include "dyf/RHI/IDevice.h"
#include "dyf/RHI/ICommandList.h"
#include "dyf/RHI/Pipeline.h"
#include "dyf/RHI/ResourceSet.h"
#include "dyf/RHI/Shader.h"
#include "dyf/RHI/Indirect.h"
#include "dyf/RHI/Texture.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#if defined(ENABLE_METAL) || defined(ENABLE_D3D12) || defined(ENABLE_VULKAN)
#include "GpuVisibilityCompute.h"
#include "GpuOcclusionCompute.h"
#include "GpuHiZCompute.h"
#endif

namespace dyf
{
GpuVisibility::~GpuVisibility()
{
    ReleaseBuffers();
    m_device.DestroyPipeline(m_hizPipeline);m_device.DestroyPipeline(m_occlusionPipeline);
    m_device.DestroyShader(m_hizShader);m_device.DestroyShader(m_occlusionShader);
    m_device.DestroyPipeline(m_pipeline);m_device.DestroyShader(m_shader);
}

void GpuVisibility::ReleaseBuffers()
{
    ReleaseOcclusion();
    m_device.DestroyResourceSet(m_resources);m_resources=nullptr;
    for(auto* buffer:{m_inputs,m_batchData,m_frame,m_instances,m_arguments,m_lodHistory}) m_device.DestroyBuffer(buffer);
    m_inputs=m_batchData=m_frame=m_instances=m_arguments=m_lodHistory=nullptr;
    m_uploadReady=false;m_ready=false;m_pending=false;m_resetHistory=true;m_capacity=0;
    m_instancesGraph={};m_argumentsGraph={};
}

bool GpuVisibility::CreateBuffer(RHI::BufferHandle& buffer,uint64_t bytes,uint32_t stride,
    RHI::BufferUsage usage,RHI::ResourceState initialState)
{
    if(!bytes || bytes>UINT32_MAX ||
        ((usage&RHI::BufferUsage::Storage)!=RHI::BufferUsage::None && bytes>m_device.GetLimit(RHI::Limit::StorageBufferBytes))) return false;
    buffer=m_device.CreateBuffer({static_cast<uint32_t>(bytes),stride,usage,initialState});
    return buffer!=nullptr;
}

bool GpuVisibility::PreparePipeline()
{
    using namespace RHI;
    if(m_pipeline) return true;
    if(!m_device.Supports(Feature::Compute) || !m_device.Supports(Feature::IndirectDraw)) return false;
#if defined(ENABLE_METAL)
    constexpr const char* entry="kernelMain";
#else
    constexpr const char* entry="main";
#endif
#if defined(ENABLE_METAL) || defined(ENABLE_D3D12) || defined(ENABLE_VULKAN)
    if(!m_shader) m_shader=m_device.CreateShader({ShaderStage::Compute,entry,kGpuVisibilityCompute,kGpuVisibilityComputeSize});
#endif
    if(!m_shader) return false;
    const ResourceBindingLayout bindings[]={{0,ResourceBindingType::ReadOnlyStorageBuffer,1,ShaderStageFlags::Compute,{}},
        {1,ResourceBindingType::ReadOnlyStorageBuffer,1,ShaderStageFlags::Compute,{}},
        {2,ResourceBindingType::ReadWriteStorageBuffer,1,ShaderStageFlags::Compute,{}},
        {3,ResourceBindingType::ReadWriteStorageBuffer,1,ShaderStageFlags::Compute,{}},
        {4,ResourceBindingType::ReadWriteStorageBuffer,1,ShaderStageFlags::Compute,{}},
        {5,ResourceBindingType::ConstantBuffer,1,ShaderStageFlags::Compute,{}}};
    ComputePipelineDesc desc;desc.computeShader=m_shader;
    desc.layout={bindings,6,16,ShaderStageFlags::Compute,15};desc.threadGroupSize[0]=64;
    m_pipeline=m_device.CreateComputePipeline(desc);return m_pipeline!=nullptr;
}

bool GpuVisibility::Prepare(const std::vector<Instance>& inputs,const std::vector<Batch>& batches,const Settings& settings)
{
    using namespace RHI;
    if(inputs.empty() || batches.empty() || inputs.size()>UINT32_MAX/sizeof(Instance) ||
        batches.size()>UINT32_MAX/sizeof(Batch) || !settings.width || !settings.height ||
        !std::isfinite(settings.pixelError) || settings.pixelError<=0 ||
        !std::isfinite(settings.hysteresis) || settings.hysteresis<0 || settings.hysteresis>=.5f) return false;
    uint64_t capacity=0;
    for(const auto& batch:batches)
    {
        if(!batch.capacity || !batch.indexCount || batch.instanceOffset<capacity) return false;
        capacity=uint64_t(batch.instanceOffset)+batch.capacity;
    }
    if(capacity>UINT32_MAX/sizeof(VisibleInstance)) return false;
    // 배치당 최악의 선택 개수까지 확인하여 원자적 append가 다음 구간을 덮지 않게 한다.
    std::vector<uint32_t> needed(batches.size(),0);
    for(const auto& input:inputs)
    {
        if(!input.lodCount || input.lodCount>4) return false;
        for(uint32_t level=0;level<input.lodCount;++level)
        {
            const auto batch=input.batches[level];
            if(batch>=batches.size()) return false;
            bool repeated=false;
            for(uint32_t earlier=0;earlier<level;++earlier) repeated=repeated || input.batches[earlier]==batch;
            if(!repeated && ++needed[batch]>batches[batch].capacity) return false;
        }
    }
    if(!PreparePipeline()) return false;
    if(!m_inputs || m_inputData.size()!=inputs.size() || m_batches.size()!=batches.size() || m_capacity!=capacity)
    {
        ReleaseBuffers();
        const auto count=uint64_t(inputs.size()),batchCount=uint64_t(batches.size());
        if(!CreateBuffer(m_inputs,count*sizeof(Instance),sizeof(Instance),BufferUsage::Storage,ResourceState::CopyDestination) ||
            !CreateBuffer(m_batchData,batchCount*sizeof(Batch),sizeof(Batch),BufferUsage::Storage,ResourceState::CopyDestination) ||
            !CreateBuffer(m_frame,sizeof(Frame),0,BufferUsage::Constant,ResourceState::CopyDestination) ||
            !CreateBuffer(m_instances,capacity*sizeof(VisibleInstance),sizeof(VisibleInstance),BufferUsage::Storage|BufferUsage::Vertex,ResourceState::UnorderedAccess) ||
            !CreateBuffer(m_arguments,batchCount*sizeof(DrawIndexedIndirectArguments),sizeof(uint32_t),BufferUsage::Storage|BufferUsage::Indirect,ResourceState::UnorderedAccess) ||
            !CreateBuffer(m_lodHistory,count*sizeof(uint32_t),sizeof(uint32_t),BufferUsage::Storage,ResourceState::UnorderedAccess))
            {ReleaseBuffers();return false;}
        const ResourceBinding bindings[]={{0,0,m_inputs,nullptr,0,m_inputs->GetDesc().size,{}},
            {1,0,m_batchData,nullptr,0,m_batchData->GetDesc().size,{}},
            {2,0,m_instances,nullptr,0,m_instances->GetDesc().size,{}},
            {3,0,m_arguments,nullptr,0,m_arguments->GetDesc().size,{}},
            {4,0,m_lodHistory,nullptr,0,m_lodHistory->GetDesc().size,{}},{5,0,m_frame,nullptr,0,sizeof(Frame),{}}};
        m_resources=m_device.CreateResourceSet({m_pipeline,bindings,6});
        if(!m_resources) {ReleaseBuffers();return false;}
        m_capacity=static_cast<uint32_t>(capacity);
    }
    m_inputData=inputs;m_batches=batches;m_pending=false;m_pendingOcclusion=false;
    m_resetHistory=m_resetHistory || settings.resetHistory;
    const auto vp=settings.camera.projection*settings.camera.view;
    const auto row=[&](uint32_t r) {return Math::float4{vp.m[r],vp.m[4+r],vp.m[8+r],vp.m[12+r]};};
    const auto x=row(0),y=row(1),z=row(2),w=row(3);
    const auto combine=[](Math::float4 a,Math::float4 b,float sign) {
        Math::float4 plane={a.x+sign*b.x,a.y+sign*b.y,a.z+sign*b.z,a.w+sign*b.w};
        const float length=std::sqrt(plane.x*plane.x+plane.y*plane.y+plane.z*plane.z);
        if(!std::isfinite(length) || length<=1e-20f || !std::isfinite(plane.w)) return Math::float4{0,0,0,0};
        return Math::float4{plane.x/length,plane.y/length,plane.z/length,plane.w/length};
    };
    // 지터로 화면 안에 들어오는 가장자리도 유지하도록 한 픽셀의 여유를 둔다.
    const auto expand=[&](float amount) {return Math::float4{w.x*amount,w.y*amount,w.z*amount,w.w*amount};};
    const auto horizontal=expand(1.f+2.f/settings.width),vertical=expand(1.f+2.f/settings.height);
    m_frameData.planes={combine(horizontal,x,1),combine(horizontal,x,-1),combine(vertical,y,1),combine(vertical,y,-1),
        combine(z,{0,0,0,0},1),combine(w,z,-1)};
    const auto& view=settings.camera.view;const auto& projection=settings.camera.projection;
    m_frameData.depthRow={-view.m[2],-view.m[6],-view.m[10],-view.m[14]};
    m_frameData.projection={std::max(std::abs(projection.m[0])*settings.width,std::abs(projection.m[5])*settings.height)*.5f,
        settings.pixelError,settings.hysteresis,std::abs(projection.m[15])<1e-6f ? 1.f : 0.f};
    m_frameData.counts={static_cast<uint32_t>(inputs.size()),static_cast<uint32_t>(batches.size()),settings.frustum?1u:0u,settings.lod?1u:0u};
    return true;
}

bool GpuVisibility::RecordUploads(RHI::ICommandList& commands)
{
    using namespace RHI;
    if(!m_resources || m_inputData.empty()) return false;
    const BufferHandle buffers[]={m_inputs,m_batchData,m_frame};
    const ResourceState states[]={ResourceState::ShaderResource,ResourceState::ShaderResource,ResourceState::ConstantBuffer};
    const void* data[]={m_inputData.data(),m_batches.data(),&m_frameData};
    const uint32_t sizes[]={static_cast<uint32_t>(m_inputData.size()*sizeof(Instance)),
        static_cast<uint32_t>(m_batches.size()*sizeof(Batch)),sizeof(Frame)};
    for(uint32_t i=0;i<3;++i)
    {
        if(m_uploadReady) {const ResourceBarrierDesc barrier={buffers[i],nullptr,states[i],ResourceState::CopyDestination,{}};commands.ResourceBarrier(&barrier,1);}
        if(!m_device.UpdateBuffer(commands,buffers[i],0,data[i],sizes[i])) return false;
        const ResourceBarrierDesc barrier={buffers[i],nullptr,ResourceState::CopyDestination,states[i],{}};
        commands.ResourceBarrier(&barrier,1);
    }
    return true;
}

bool GpuVisibility::AddPasses(RHI::RenderGraph& graph)
{
    using namespace RHI;
    if(m_pending || !m_resources || m_inputData.empty()) return false;
    const auto inputs=graph.ImportBuffer("GPU visibility inputs",m_inputs,ResourceState::ShaderResource,ResourceState::ShaderResource);
    const auto batches=graph.ImportBuffer("GPU visibility batches",m_batchData,ResourceState::ShaderResource,ResourceState::ShaderResource);
    const auto frame=graph.ImportBuffer("GPU visibility camera",m_frame,ResourceState::ConstantBuffer,ResourceState::ConstantBuffer);
    const auto history=graph.ImportBuffer("GPU LOD history",m_lodHistory,ResourceState::UnorderedAccess,ResourceState::UnorderedAccess);
    m_instancesGraph=graph.ImportBuffer("GPU visible instances",m_instances,m_ready?ResourceState::VertexBuffer:ResourceState::UnorderedAccess,ResourceState::VertexBuffer);
    m_argumentsGraph=graph.ImportBuffer("GPU indirect arguments",m_arguments,m_ready?ResourceState::IndirectArgument:ResourceState::UnorderedAccess,ResourceState::IndirectArgument);
    if(!inputs.IsValid() || !batches.IsValid() || !frame.IsValid() || !history.IsValid() ||
        !m_instancesGraph.IsValid() || !m_argumentsGraph.IsValid()) return false;
    const auto pipeline=m_pipeline;const auto resources=m_resources;
    for(uint32_t operation=0;operation<2;++operation)
    {
        auto& pass=graph.AddPass(operation?"GPU frustum and LOD selection":"GPU indirect reset", RHI::RGRecordingMode::Independent);
        pass.Read(inputs,ResourceState::ShaderResource).Read(batches,ResourceState::ShaderResource).Read(frame,ResourceState::ConstantBuffer)
            .Read(history,ResourceState::UnorderedAccess).Write(history,ResourceState::UnorderedAccess)
            .Write(m_instancesGraph,ResourceState::UnorderedAccess).Write(m_argumentsGraph,ResourceState::UnorderedAccess).SetPipeline(pipeline);
        if(operation) pass.Read(m_argumentsGraph,ResourceState::UnorderedAccess);
        const auto count=operation?m_frameData.counts[0]:std::max(m_frameData.counts[0],m_frameData.counts[1]);
        const uint32_t groups=count/64+(count%64!=0),groupsX=std::min(groups,65535u),groupsY=groups/groupsX+(groups%groupsX!=0);
        const std::array<uint32_t,4> push={operation,groupsX*64,m_resetHistory?1u:0u,0};
        pass.SetExecute([pipeline,resources,push,groupsX,groupsY](ICommandList* commands) {
            commands->BindComputePipeline(pipeline);commands->BindResourceSet(resources);
            commands->SetInlineConstants(0,sizeof(push),push.data());commands->Dispatch(groupsX,groupsY,1);
        });
    }
    m_pending=true;return true;
}

void GpuVisibility::ReleaseOcclusion()
{
    m_device.DestroyResourceSet(m_occlusionResources);m_occlusionResources=nullptr;
    for(auto resources:m_hizResources) m_device.DestroyResourceSet(resources);
    m_hizResources.clear();
    m_device.DestroyTexture(m_hiz);m_hiz=nullptr;m_hizDepth=nullptr;
    m_device.DestroyBuffer(m_occlusionFrame);m_occlusionFrame=nullptr;
    m_hizReady=false;m_occlusionReady=false;m_pendingOcclusion=false;
}

bool GpuVisibility::PrepareOcclusion(RHI::TextureHandle depth)
{
    using namespace RHI;
    if(!depth || !m_resources) return false;
#if defined(ENABLE_METAL)
    constexpr const char* entry="kernelMain";
#else
    constexpr const char* entry="main";
#endif
    SamplerDesc sampler;
    sampler.minFilter=sampler.magFilter=sampler.mipFilter=SamplerFilter::Nearest;
    sampler.addressU=sampler.addressV=sampler.addressW=SamplerAddressMode::ClampToEdge;
    sampler.mipLodBias=sampler.minLod=0;sampler.maxLod=32;
    if(!m_hizPipeline)
    {
#if defined(ENABLE_METAL) || defined(ENABLE_D3D12) || defined(ENABLE_VULKAN)
        if(!m_hizShader) m_hizShader=m_device.CreateShader({ShaderStage::Compute,entry,kGpuHiZCompute,kGpuHiZComputeSize});
#endif
        if(!m_hizShader) return false;
        const ResourceBindingLayout bindings[]={{0,ResourceBindingType::SampledTexture,1,ShaderStageFlags::Compute,{}},
            {1,ResourceBindingType::StorageTexture,1,ShaderStageFlags::Compute,{}},
            {2,ResourceBindingType::StorageTexture,1,ShaderStageFlags::Compute,{}},
            {8,ResourceBindingType::StaticSampler,1,ShaderStageFlags::Compute,sampler}};
        ComputePipelineDesc desc;desc.computeShader=m_hizShader;
        desc.layout={bindings,4,16,ShaderStageFlags::Compute,15};desc.threadGroupSize[0]=desc.threadGroupSize[1]=8;
        m_hizPipeline=m_device.CreateComputePipeline(desc);
        if(!m_hizPipeline) return false;
    }
    if(!m_occlusionPipeline)
    {
#if defined(ENABLE_METAL) || defined(ENABLE_D3D12) || defined(ENABLE_VULKAN)
        if(!m_occlusionShader) m_occlusionShader=m_device.CreateShader({ShaderStage::Compute,entry,kGpuOcclusionCompute,kGpuOcclusionComputeSize});
#endif
        if(!m_occlusionShader) return false;
        const ResourceBindingLayout bindings[]={{0,ResourceBindingType::ReadOnlyStorageBuffer,1,ShaderStageFlags::Compute,{}},
            {1,ResourceBindingType::ReadOnlyStorageBuffer,1,ShaderStageFlags::Compute,{}},
            {2,ResourceBindingType::ReadWriteStorageBuffer,1,ShaderStageFlags::Compute,{}},
            {3,ResourceBindingType::ReadWriteStorageBuffer,1,ShaderStageFlags::Compute,{}},
            {4,ResourceBindingType::ReadWriteStorageBuffer,1,ShaderStageFlags::Compute,{}},
            {5,ResourceBindingType::ConstantBuffer,1,ShaderStageFlags::Compute,{}},
            {6,ResourceBindingType::SampledTexture,1,ShaderStageFlags::Compute,{}},
            {7,ResourceBindingType::ConstantBuffer,1,ShaderStageFlags::Compute,{}},
            {8,ResourceBindingType::StaticSampler,1,ShaderStageFlags::Compute,sampler}};
        ComputePipelineDesc desc;desc.computeShader=m_occlusionShader;
        desc.layout={bindings,9,16,ShaderStageFlags::Compute,15};desc.threadGroupSize[0]=64;
        m_occlusionPipeline=m_device.CreateComputePipeline(desc);
        if(!m_occlusionPipeline) return false;
    }
    if(m_hizDepth==depth && m_hiz && m_occlusionResources) return true;
    ReleaseOcclusion();
    const auto& source=depth->GetDesc();
    uint32_t width=source.width/4+(source.width%4!=0),height=source.height/4+(source.height%4!=0);
    // 첫 4×4 레벨을 2의 거듭제곱으로 채워 native mip의 홀수 끝 픽셀 누락을 막는다.
    const auto padded=[](uint32_t value) {uint64_t result=1;while(result<value) result*=2;return result;};
    const auto paddedWidth=padded(width),paddedHeight=padded(height);
    if(paddedWidth>m_device.GetLimit(Limit::Texture2DDimension) || paddedHeight>m_device.GetLimit(Limit::Texture2DDimension)) return false;
    TextureDesc desc;desc.width=static_cast<uint32_t>(paddedWidth);desc.height=static_cast<uint32_t>(paddedHeight);
    for(uint32_t size=std::max(desc.width,desc.height);size>1;size/=2) ++desc.mipLevels;
    desc.format=Format::R32_FLOAT;desc.usage=TextureUsage::Storage|TextureUsage::ShaderResource;
    m_hiz=m_device.CreateTexture(desc);
    if(!m_hiz || !CreateBuffer(m_occlusionFrame,sizeof(OcclusionFrame),0,BufferUsage::Constant,ResourceState::CopyDestination))
        {ReleaseOcclusion();return false;}
    for(uint32_t level=0;level<desc.mipLevels;++level)
    {
        const TextureSubresourceRange previous={level?level-1:0,1,0,1},current={level,1,0,1};
        const ResourceBinding bindings[]={{0,0,nullptr,depth,0,0,{0,1,0,1}},
            {1,0,nullptr,m_hiz,0,0,previous},{2,0,nullptr,m_hiz,0,0,current}};
        auto resources=m_device.CreateResourceSet({m_hizPipeline,bindings,3});
        if(!resources) {ReleaseOcclusion();return false;}
        try {m_hizResources.push_back(resources);}
        catch(...) {m_device.DestroyResourceSet(resources);throw;}
    }
    const ResourceBinding bindings[]={{0,0,m_inputs,nullptr,0,m_inputs->GetDesc().size,{}},
        {1,0,m_batchData,nullptr,0,m_batchData->GetDesc().size,{}},
        {2,0,m_instances,nullptr,0,m_instances->GetDesc().size,{}},
        {3,0,m_arguments,nullptr,0,m_arguments->GetDesc().size,{}},
        {4,0,m_lodHistory,nullptr,0,m_lodHistory->GetDesc().size,{}},{5,0,m_frame,nullptr,0,sizeof(Frame),{}},
        {6,0,nullptr,m_hiz,0,0,{}},{7,0,m_occlusionFrame,nullptr,0,sizeof(OcclusionFrame),{}}};
    m_occlusionResources=m_device.CreateResourceSet({m_occlusionPipeline,bindings,8});
    if(!m_occlusionResources) {ReleaseOcclusion();return false;}
    m_hizDepth=depth;return true;
}

bool GpuVisibility::AddOcclusionPasses(RHI::RenderGraph& graph,RHI::TextureHandle depth,
    RHI::RGResourceHandle depthHandle,const Camera& jitteredCamera)
{
    using namespace RHI;
    if(!m_pending || m_pendingOcclusion || !depth || !depthHandle.IsValid() || !PrepareOcclusion(depth)) return false;
    const auto& depthDesc=depth->GetDesc();const auto& hizDesc=m_hiz->GetDesc();
    OcclusionFrame constants;
    constants.viewProjection=jitteredCamera.projection*jitteredCamera.view;
    constants.viewport={float(depthDesc.width),float(depthDesc.height),.0001f,2.f};
    constants.info={hizDesc.mipLevels,0,0,0};
    const auto camera=graph.ImportBuffer("GPU occlusion camera",m_occlusionFrame,
        m_occlusionReady?ResourceState::ConstantBuffer:ResourceState::CopyDestination,ResourceState::ConstantBuffer);
    const auto hiz=graph.ImportTexture("GPU current HiZ",m_hiz,
        m_hizReady?ResourceState::ShaderResource:ResourceState::Undefined,ResourceState::ShaderResource);
    const auto inputs=graph.ImportBuffer("GPU visibility inputs",m_inputs,ResourceState::ShaderResource,ResourceState::ShaderResource);
    const auto batches=graph.ImportBuffer("GPU visibility batches",m_batchData,ResourceState::ShaderResource,ResourceState::ShaderResource);
    const auto frame=graph.ImportBuffer("GPU visibility camera",m_frame,ResourceState::ConstantBuffer,ResourceState::ConstantBuffer);
    const auto history=graph.ImportBuffer("GPU LOD history",m_lodHistory,ResourceState::UnorderedAccess,ResourceState::UnorderedAccess);
    if(!camera.IsValid() || !hiz.IsValid() || !inputs.IsValid() || !batches.IsValid() || !frame.IsValid() || !history.IsValid()) return false;
    const auto constantBuffer=m_occlusionFrame;auto* device=&m_device;
    graph.AddPass("GPU occlusion camera upload", RHI::RGRecordingMode::Independent).Write(camera,ResourceState::CopyDestination)
        .SetExecute([device,constantBuffer,constants](ICommandList* commands) {
            if(!device->UpdateBuffer(*commands,constantBuffer,0,&constants,sizeof(constants)))
                throw std::runtime_error("GPU occlusion camera upload failed");
        });
    for(uint32_t level=0;level<hizDesc.mipLevels;++level)
    {
        auto& pass=graph.AddPass(level?"GPU HiZ max reduction":"GPU HiZ depth reduction", RHI::RGRecordingMode::Independent);
        pass.Read(depthHandle,ResourceState::ShaderResource).Write(hiz,ResourceState::UnorderedAccess).SetPipeline(m_hizPipeline);
        if(level) pass.Read(hiz,ResourceState::UnorderedAccess);
        const uint32_t width=std::max(1u,hizDesc.width>>level),height=std::max(1u,hizDesc.height>>level);
        const std::array<uint32_t,4> operation={level?2u:4u,level?1u:0u,0,0};
        const auto pipeline=m_hizPipeline;const auto resources=m_hizResources[level];
        pass.SetExecute([pipeline,resources,operation,width,height](ICommandList* commands) {
            commands->BindComputePipeline(pipeline);commands->BindResourceSet(resources);
            commands->SetInlineConstants(0,sizeof(operation),operation.data());commands->Dispatch(width/8+(width%8!=0),height/8+(height%8!=0),1);
        });
    }
    for(uint32_t operation=0;operation<2;++operation)
    {
        auto& pass=graph.AddPass(operation?"GPU HiZ occlusion selection":"GPU occlusion indirect reset", RHI::RGRecordingMode::Independent);
        pass.Read(inputs,ResourceState::ShaderResource).Read(batches,ResourceState::ShaderResource).Read(frame,ResourceState::ConstantBuffer)
            .Read(camera,ResourceState::ConstantBuffer).Read(hiz,ResourceState::ShaderResource)
            .Read(history,ResourceState::UnorderedAccess).Write(history,ResourceState::UnorderedAccess)
            .Write(m_instancesGraph,ResourceState::UnorderedAccess).Write(m_argumentsGraph,ResourceState::UnorderedAccess).SetPipeline(m_occlusionPipeline);
        if(operation) pass.Read(m_argumentsGraph,ResourceState::UnorderedAccess);
        const auto count=operation?m_frameData.counts[0]:std::max(m_frameData.counts[0],m_frameData.counts[1]);
        const uint32_t groups=count/64+(count%64!=0),groupsX=std::min(groups,65535u),groupsY=groups/groupsX+(groups%groupsX!=0);
        const std::array<uint32_t,4> push={operation,groupsX*64,0,0};
        const auto pipeline=m_occlusionPipeline;const auto resources=m_occlusionResources;
        pass.SetExecute([pipeline,resources,push,groupsX,groupsY](ICommandList* commands) {
            commands->BindComputePipeline(pipeline);commands->BindResourceSet(resources);
            commands->SetInlineConstants(0,sizeof(push),push.data());commands->Dispatch(groupsX,groupsY,1);
        });
    }
    m_pendingOcclusion=true;return true;
}

void GpuVisibility::CommitFrame()
{
    if(!m_pending) return;
    if(m_pendingOcclusion) {m_hizReady=true;m_occlusionReady=true;m_pendingOcclusion=false;}
    m_ready=true;m_resetHistory=false;m_pending=false;
}
}
