#include "GpuShadowVisibility.h"
#include "dyf/RHI/IDevice.h"
#include "dyf/RHI/ICommandList.h"
#include "dyf/RHI/Pipeline.h"
#include "dyf/RHI/Shader.h"
#include "dyf/RHI/Indirect.h"
#include <algorithm>
#include <cstring>
#include <limits>
#include <type_traits>
#if defined(ENABLE_VULKAN) || defined(ENABLE_D3D12) || defined(ENABLE_METAL)
#include "GpuShadowCompute.h"
#endif
namespace dyf {
namespace {
RHI::PipelineLayoutDesc ShadowVisibilityLayout() {
    using namespace RHI;
    static const ResourceBindingLayout bindings[]={
        {0,ResourceBindingType::ReadOnlyStorageBuffer,1,ShaderStageFlags::Compute,{}},
        {1,ResourceBindingType::ReadOnlyStorageBuffer,1,ShaderStageFlags::Compute,{}},
        {2,ResourceBindingType::ReadOnlyStorageBuffer,1,ShaderStageFlags::Compute,{}},
        {3,ResourceBindingType::ReadOnlyStorageBuffer,1,ShaderStageFlags::Compute,{}},
        {4,ResourceBindingType::ReadWriteStorageBuffer,1,ShaderStageFlags::Compute,{}},
        {5,ResourceBindingType::ReadWriteStorageBuffer,1,ShaderStageFlags::Compute,{}}};
    return {bindings,6,32,ShaderStageFlags::Compute,15};
}
}
GpuShadowVisibility::~GpuShadowVisibility() {
    ReleaseBuffers();m_device.DestroyPipeline(m_pipeline);m_device.DestroyShader(m_shader);
}
bool GpuShadowVisibility::Supported(const RHI::IDevice& device) {
    return device.Supports(RHI::Feature::Compute) && device.Supports(RHI::Feature::IndirectDraw) &&
        device.Supports(RHI::Feature::IndirectFirstInstance) && device.Supports(ShadowVisibilityLayout());
}
void GpuShadowVisibility::ReleaseBuffers() {
    m_device.DestroyResourceSet(m_resources);m_resources=nullptr;
    for(auto* buffer:{m_inputs,m_batches,m_views,m_models,m_instances,m_arguments})m_device.DestroyBuffer(buffer);
    m_inputs=m_batches=m_views=m_models=m_instances=m_arguments=m_source=nullptr;
    m_uploadReady=m_outputReady=m_pending=false;m_dirty.fill(true);
    m_instancesGraph={};m_argumentsGraph={};
}
bool GpuShadowVisibility::PreparePipeline() {
    using namespace RHI;
    if(m_pipeline)return true;
    if(!Supported(m_device))return false;
#if defined(ENABLE_METAL)
    constexpr const char* entry="kernelMain";
#else
    constexpr const char* entry="main";
#endif
#if defined(ENABLE_VULKAN) || defined(ENABLE_D3D12) || defined(ENABLE_METAL)
    if(!m_shader)m_shader=m_device.CreateShader({ShaderStage::Compute,entry,kGpuShadowCompute,kGpuShadowComputeSize});
#endif
    if(!m_shader)return false;
    ComputePipelineDesc desc;desc.computeShader=m_shader;
    desc.layout=ShadowVisibilityLayout();desc.threadGroupSize[0]=64;
    m_pipeline=m_device.CreateComputePipeline(desc);return m_pipeline!=nullptr;
}
bool GpuShadowVisibility::Prepare(const std::vector<Input>& inputs,const std::vector<Batch>& batches,
    const std::vector<View>& views,RHI::BufferHandle transforms) {
    using namespace RHI;
    if(inputs.empty() || batches.empty() || views.empty() || views.size()>128 || !transforms ||
        transforms->GetDesc().size%sizeof(Instance) || inputs.size()>UINT32_MAX/sizeof(Input) ||
        batches.size()>UINT32_MAX/sizeof(Batch))return false;
    const auto entityCount=transforms->GetDesc().size/sizeof(Instance);
    uint64_t capacity=0;
    for(const auto& batch:batches) {
        if(!batch.indexCount || !batch.capacity || batch.instanceOffset!=capacity)return false;
        capacity+=batch.capacity;
    }
    const uint64_t argumentCount=uint64_t(batches.size())*views.size();
    const uint64_t invocationCount=uint64_t(inputs.size())*views.size();
    if(capacity>UINT32_MAX || argumentCount>UINT32_MAX/sizeof(DrawIndexedIndirectArguments) ||
        capacity*views.size()>UINT32_MAX/sizeof(Instance) || invocationCount>UINT32_MAX)return false;
    std::vector<uint32_t> populations(batches.size(),0);
    for(const auto& input:inputs) {
        if(input.ids[0]>=entityCount || input.ids[1]>=batches.size() ||
            ++populations[input.ids[1]]>batches[input.ids[1]].capacity)return false;
    }
    for(const auto& view:views)if(view.info[0]>view.receiverPlanes.size() || view.info[1]>1)return false;
    const uint64_t sizes[]={inputs.size()*sizeof(Input),batches.size()*sizeof(Batch),views.size()*sizeof(View),
        transforms->GetDesc().size,capacity*views.size()*sizeof(Instance),argumentCount*sizeof(DrawIndexedIndirectArguments)};
    for(uint64_t size:sizes)
        if(!size || size>UINT32_MAX || size>m_device.GetLimit(Limit::StorageBufferBytes))return false;
    if(!PreparePipeline())return false;
    const BufferHandle buffers[]={m_inputs,m_batches,m_views,m_models,m_instances,m_arguments};
    bool replace=!m_resources;
    for(size_t i=0;i<std::size(buffers);++i)replace=replace || !buffers[i] || buffers[i]->GetDesc().size<sizes[i];
    if(replace) {
        ReleaseBuffers();
        BufferHandle* slots[]={&m_inputs,&m_batches,&m_views,&m_models,&m_instances,&m_arguments};
        const uint32_t strides[]={sizeof(Input),sizeof(Batch),sizeof(View),sizeof(Instance),sizeof(Instance),sizeof(uint32_t)};
        for(size_t i=0;i<std::size(slots);++i) {
            auto usage=BufferUsage::Storage;
            if(i==4)usage|=BufferUsage::Vertex;
            if(i==5)usage|=BufferUsage::Indirect;
            *slots[i]=m_device.CreateBuffer({static_cast<uint32_t>(sizes[i]),strides[i],usage,
                i<4?ResourceState::CopyDestination:ResourceState::UnorderedAccess});
            if(!*slots[i]) {ReleaseBuffers();return false;}
        }
        const BufferHandle allocated[]={m_inputs,m_batches,m_views,m_models,m_instances,m_arguments};
        ResourceBinding bindings[6];
        for(uint32_t i=0;i<6;++i)bindings[i]={i,0,allocated[i],nullptr,0,allocated[i]->GetDesc().size,{}};
        m_resources=m_device.CreateResourceSet({m_pipeline,bindings,6});
        if(!m_resources) {ReleaseBuffers();return false;}
    }
    const auto update=[&](auto& stored,const auto& incoming,uint32_t slot) {
        using Value=typename std::decay_t<decltype(stored)>::value_type;
        if(stored.size()!=incoming.size() || std::memcmp(stored.data(),incoming.data(),incoming.size()*sizeof(Value))!=0) {
            stored=incoming;m_dirty[slot]=true;
        }
    };
    update(m_inputData,inputs,0);update(m_batchData,batches,1);update(m_viewData,views,2);
    m_source=transforms;m_capacity=static_cast<uint32_t>(capacity);m_pending=false;
    return true;
}
bool GpuShadowVisibility::RecordUploads(RHI::ICommandList& commands) {
    using namespace RHI;
    if(!m_resources || !m_source)return false;
    const BufferHandle buffers[]={m_inputs,m_batches,m_views};
    const void* bytes[]={m_inputData.data(),m_batchData.data(),m_viewData.data()};
    const uint32_t sizes[]={static_cast<uint32_t>(m_inputData.size()*sizeof(Input)),
        static_cast<uint32_t>(m_batchData.size()*sizeof(Batch)),static_cast<uint32_t>(m_viewData.size()*sizeof(View))};
    for(uint32_t i=0;i<3;++i) {
        if(m_uploadReady && !m_dirty[i])continue;
        if(m_uploadReady) {
            const ResourceBarrierDesc before={buffers[i],nullptr,ResourceState::ShaderResource,ResourceState::CopyDestination,{}};
            commands.ResourceBarrier(&before,1);
        }
        if(!m_device.UpdateBuffer(commands,buffers[i],0,bytes[i],sizes[i]))return false;
        const ResourceBarrierDesc after={buffers[i],nullptr,ResourceState::CopyDestination,ResourceState::ShaderResource,{}};
        commands.ResourceBarrier(&after,1);
    }
    // Reuse the canonical GPU transforms without CPU copies for every light view.
    ResourceBarrierDesc before[2]={{m_source,nullptr,ResourceState::VertexBuffer,ResourceState::CopySource,{}},
        {m_models,nullptr,ResourceState::ShaderResource,ResourceState::CopyDestination,{}}};
    commands.ResourceBarrier(before,m_uploadReady?2:1);
    commands.CopyBuffer(m_source,0,m_models,0,m_source->GetDesc().size);
    const ResourceBarrierDesc after[]={{m_source,nullptr,ResourceState::CopySource,ResourceState::VertexBuffer,{}},
        {m_models,nullptr,ResourceState::CopyDestination,ResourceState::ShaderResource,{}}};
    commands.ResourceBarrier(after,2);
    return true;
}
bool GpuShadowVisibility::AddPasses(RHI::RenderGraph& graph) {
    using namespace RHI;
    if(!m_resources || m_pending || m_inputData.empty())return false;
    const auto inputs=graph.ImportBuffer("GPU shadow bounds",m_inputs,ResourceState::ShaderResource,ResourceState::ShaderResource);
    const auto batches=graph.ImportBuffer("GPU shadow batches",m_batches,ResourceState::ShaderResource,ResourceState::ShaderResource);
    const auto views=graph.ImportBuffer("GPU shadow views",m_views,ResourceState::ShaderResource,ResourceState::ShaderResource);
    const auto models=graph.ImportBuffer("GPU shadow transforms",m_models,ResourceState::ShaderResource,ResourceState::ShaderResource);
    m_instancesGraph=graph.ImportBuffer("GPU shadow instances",m_instances,
        m_outputReady?ResourceState::VertexBuffer:ResourceState::UnorderedAccess,ResourceState::VertexBuffer);
    m_argumentsGraph=graph.ImportBuffer("GPU shadow arguments",m_arguments,
        m_outputReady?ResourceState::IndirectArgument:ResourceState::UnorderedAccess,ResourceState::IndirectArgument);
    if(!inputs.IsValid() || !batches.IsValid() || !views.IsValid() || !models.IsValid() ||
        !m_instancesGraph.IsValid() || !m_argumentsGraph.IsValid())return false;
    for(uint32_t operation=0;operation<2;++operation) {
        auto& pass=graph.AddPass(operation?"GPU shadow culling":"GPU shadow indirect reset",RGRecordingMode::Independent);
        pass.Read(inputs,ResourceState::ShaderResource).Read(batches,ResourceState::ShaderResource)
            .Read(views,ResourceState::ShaderResource).Read(models,ResourceState::ShaderResource)
            .Write(m_instancesGraph,ResourceState::UnorderedAccess).Write(m_argumentsGraph,ResourceState::UnorderedAccess)
            .SetPipeline(m_pipeline);
        if(operation)pass.Read(m_argumentsGraph,ResourceState::UnorderedAccess);
        const uint32_t count=static_cast<uint32_t>((operation?m_inputData.size():m_batchData.size())*m_viewData.size());
        const uint32_t groups=count/64+(count%64!=0),x=std::min(groups,65535u),y=(groups+x-1)/x;
        const std::array<uint32_t,8> values={operation,x*64,static_cast<uint32_t>(m_inputData.size()),
            static_cast<uint32_t>(m_batchData.size()),static_cast<uint32_t>(m_viewData.size()),m_capacity,0,0};
        const auto pipeline=m_pipeline;const auto resources=m_resources;
        pass.SetExecute([pipeline,resources,values,x,y](ICommandList* commands) {
            commands->BindComputePipeline(pipeline);commands->BindResourceSet(resources);
            commands->SetInlineConstants(0,sizeof(values),values.data());commands->Dispatch(x,y,1);
        });
    }
    m_pending=true;return true;
}
void GpuShadowVisibility::CommitFrame() {
    if(!m_pending)return;
    m_uploadReady=m_outputReady=true;m_pending=false;m_dirty.fill(false);
}
}
