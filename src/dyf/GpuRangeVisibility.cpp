#include "GpuVisibility.h"
#include "dyf/RHI/IDevice.h"
#include "dyf/RHI/ICommandList.h"
#include "dyf/RHI/Pipeline.h"
#include "dyf/RHI/Binding.h"
#include "dyf/RHI/Shader.h"
#include <algorithm>
#include <cstring>
#if defined(ENABLE_METAL) || defined(ENABLE_D3D12) || defined(ENABLE_VULKAN)
#include "GpuRangeCompute.h"
#include "GpuRangeOcclusionCompute.h"
#endif
namespace dyf {
namespace {
struct RangeLayouts {
    std::array<RHI::ResourceBindingLayout,11> bindings;
    RangeLayouts() {
        using namespace RHI;
        SamplerDesc sampler;
        sampler.minFilter=sampler.magFilter=sampler.mipFilter=SamplerFilter::Nearest;
        sampler.addressU=sampler.addressV=sampler.addressW=SamplerAddressMode::ClampToEdge;
        sampler.mipLodBias=sampler.minLod=0;sampler.maxLod=32;
        bindings={{{0,ResourceBindingType::ReadOnlyStorageBuffer,1,ShaderStageFlags::Compute,{}},
            {1,ResourceBindingType::ReadOnlyStorageBuffer,1,ShaderStageFlags::Compute,{}},
            {2,ResourceBindingType::ReadOnlyStorageBuffer,1,ShaderStageFlags::Compute,{}},
            {3,ResourceBindingType::ReadOnlyStorageBuffer,1,ShaderStageFlags::Compute,{}},
            {4,ResourceBindingType::ReadWriteStorageBuffer,1,ShaderStageFlags::Compute,{}},
            {5,ResourceBindingType::ReadWriteStorageBuffer,1,ShaderStageFlags::Compute,{}},
            {6,ResourceBindingType::ReadWriteStorageBuffer,1,ShaderStageFlags::Compute,{}},
            {7,ResourceBindingType::ConstantBuffer,1,ShaderStageFlags::Compute,{}},
            {8,ResourceBindingType::SampledTexture,1,ShaderStageFlags::Compute,{}},
            {9,ResourceBindingType::ConstantBuffer,1,ShaderStageFlags::Compute,{}},
            {10,ResourceBindingType::StaticSampler,1,ShaderStageFlags::Compute,sampler}}};
    }
    RHI::PipelineLayoutDesc Get(bool occlusion) const
    {return {bindings.data(),occlusion?11u:8u,16,RHI::ShaderStageFlags::Compute,15};}
};
}
void GpuVisibility::ReleaseRanges()
{
    m_device.DestroyResourceSet(m_rangeOcclusionResources);m_rangeOcclusionResources=nullptr;
    m_device.DestroyResourceSet(m_rangeResources);m_rangeResources=nullptr;
    for(auto buffer:{m_rangeData,m_rangeInstances,m_rangeArguments,m_rangeKeys})m_device.DestroyBuffer(buffer);
    m_rangeData=m_rangeInstances=m_rangeArguments=m_rangeKeys=nullptr;
    m_rangeInstancesGraph={};m_rangeArgumentsGraph={};m_rangeKeysGraph={};
    m_rangeReady=false;m_rangeUploaded=false;m_rangeDirty=true;m_ranges.clear();m_drawRanges.clear();m_rangeInputData.clear();
}

bool GpuVisibility::PrepareRanges(const std::vector<Range>& input,bool sameBatches)
{
    using namespace RHI;
    if(input.empty() || !m_device.Supports(Feature::IndirectFirstInstance)) {ReleaseRanges();return true;}
    if(m_rangeResources && sameBatches && m_rangeInputData.size()==input.size() &&
        std::memcmp(m_rangeInputData.data(),input.data(),input.size()*sizeof(Range))==0)return true;
    m_rangeInputData.clear(); // A failed validation must not certify the next retry against newer batches.
    auto& ranges=m_rangeScratch;auto& draws=m_rangeDrawScratch;
    ranges.clear();draws.clear();
    uint64_t capacity=0;size_t cursor=0;
    for(uint32_t b=0;b<m_batches.size();++b) {
        const auto& batch=m_batches[b];DrawRange draw{static_cast<uint32_t>(ranges.size()),0};
        uint64_t covered=0;
        while(cursor<input.size() && input[cursor].batch==b) {
            auto range=input[cursor++];
            if(!range.indexCount || range.firstIndex!=covered || range.firstIndex%3 || range.indexCount%3 ||
                uint64_t(range.firstIndex)+range.indexCount>batch.indexCount ||
                uint64_t(batch.firstIndex)+range.firstIndex+range.indexCount>UINT32_MAX)return false;
            range.instanceOffset=static_cast<uint32_t>(capacity);capacity+=batch.capacity;
            if(capacity>UINT32_MAX/sizeof(VisibleInstance)) {ReleaseRanges();return true;}
            covered+=range.indexCount;ranges.push_back(range);++draw.count;
        }
        if(draw.count && covered!=batch.indexCount)return false;
        if(!draw.count) {
            ranges.push_back({{},{},b,0,batch.indexCount,static_cast<uint32_t>(capacity)});
            capacity+=batch.capacity;draw.count=1;
            if(capacity>UINT32_MAX/sizeof(VisibleInstance)) {ReleaseRanges();return true;}
        }
        draws.push_back(draw);
    }
    if(cursor!=input.size())return false;
    if(ranges.size()>UINT32_MAX/sizeof(Range) || capacity*sizeof(VisibleInstance)>m_device.GetLimit(Limit::StorageBufferBytes))
        {ReleaseRanges();return true;}
    if(!m_rangePipeline) {
#if defined(ENABLE_METAL)
        constexpr const char* entry="kernelMain";
#else
        constexpr const char* entry="main";
#endif
#if defined(ENABLE_METAL) || defined(ENABLE_D3D12) || defined(ENABLE_VULKAN)
        if(!m_rangeShader)m_rangeShader=m_device.CreateShader({ShaderStage::Compute,entry,kGpuRangeCompute,kGpuRangeComputeSize});
#endif
        if(!m_rangeShader) {ReleaseRanges();return true;}
        const RangeLayouts layouts;
        ComputePipelineDesc desc;desc.computeShader=m_rangeShader;desc.layout=layouts.Get(false);desc.threadGroupSize[0]=64;
        if(!m_device.Supports(desc.layout)) {ReleaseRanges();return true;}
        m_rangePipeline=m_device.CreateComputePipeline(desc);
        if(!m_rangePipeline) {ReleaseRanges();return true;}
    }
    if(!m_rangeResources || m_ranges.size()!=ranges.size() || m_rangeInstances->GetDesc().size!=capacity*sizeof(VisibleInstance)) {
        ReleaseRanges();
        if(!CreateBuffer(m_rangeData,ranges.size()*sizeof(Range),sizeof(Range),BufferUsage::Storage,ResourceState::CopyDestination) ||
            !CreateBuffer(m_rangeInstances,capacity*sizeof(VisibleInstance),sizeof(VisibleInstance),BufferUsage::Storage|BufferUsage::Vertex,ResourceState::UnorderedAccess) ||
            !CreateBuffer(m_rangeArguments,ranges.size()*20,4,BufferUsage::Storage|BufferUsage::Indirect,ResourceState::UnorderedAccess) ||
            !CreateBuffer(m_rangeKeys,capacity*8,8,BufferUsage::Storage|BufferUsage::Vertex,ResourceState::UnorderedAccess))
            {ReleaseRanges();return true;} // Optional range expansion falls back to the original complete batches.
        const BufferHandle buffers[]={m_rangeData,m_instances,m_arguments,m_instanceKeys,m_rangeInstances,m_rangeArguments,m_rangeKeys,m_frame};
        ResourceBinding bindings[8]={};
        for(uint32_t i=0;i<8;++i)bindings[i]={i,0,buffers[i],nullptr,0,buffers[i]->GetDesc().size,{}};
        m_rangeResources=m_device.CreateResourceSet({m_rangePipeline,bindings,8});
        if(!m_rangeResources) {ReleaseRanges();return true;}
    }
    if(m_ranges.size()!=ranges.size() || std::memcmp(m_ranges.data(),ranges.data(),ranges.size()*sizeof(Range))!=0) {
        m_ranges.swap(ranges);m_rangeDirty=true;
    }
    m_drawRanges.swap(draws);m_rangeInputData=input;return true;
}

bool GpuVisibility::PrepareRangeOcclusion()
{
    using namespace RHI;
    if(!m_rangeResources || !m_hiz || !m_occlusionFrame)return false;
    if(m_rangeOcclusionResources)return true;
    if(!m_rangeOcclusionPipeline) {
        const RangeLayouts layouts;
        if(!m_device.Supports(layouts.Get(true)))return false;
#if defined(ENABLE_METAL)
        constexpr const char* entry="kernelMain";
#else
        constexpr const char* entry="main";
#endif
#if defined(ENABLE_METAL) || defined(ENABLE_D3D12) || defined(ENABLE_VULKAN)
        if(!m_rangeOcclusionShader)m_rangeOcclusionShader=m_device.CreateShader(
            {ShaderStage::Compute,entry,kGpuRangeOcclusionCompute,kGpuRangeOcclusionComputeSize});
#endif
        if(!m_rangeOcclusionShader)return false;
        ComputePipelineDesc desc;desc.computeShader=m_rangeOcclusionShader;
        desc.layout=layouts.Get(true);desc.threadGroupSize[0]=64;
        m_rangeOcclusionPipeline=m_device.CreateComputePipeline(desc);
        if(!m_rangeOcclusionPipeline)return false;
    }
    const BufferHandle buffers[]={m_rangeData,m_instances,m_arguments,m_instanceKeys,m_rangeInstances,m_rangeArguments,m_rangeKeys,m_frame};
    ResourceBinding bindings[10]={};
    for(uint32_t i=0;i<8;++i)bindings[i]={i,0,buffers[i],nullptr,0,buffers[i]->GetDesc().size,{}};
    bindings[8]={8,0,nullptr,m_hiz,0,0,{}};
    bindings[9]={9,0,m_occlusionFrame,nullptr,0,sizeof(OcclusionFrame),{}};
    m_rangeOcclusionResources=m_device.CreateResourceSet({m_rangeOcclusionPipeline,bindings,10});
    return m_rangeOcclusionResources!=nullptr;
}

bool GpuVisibility::AddRangePass(RHI::RenderGraph& graph,bool afterOcclusion)
{
    using namespace RHI;
    if(!m_rangeResources)return true;
    if(!afterOcclusion) {
        m_rangeInstancesGraph=graph.ImportBuffer("GPU range instances",m_rangeInstances,m_rangeReady?ResourceState::VertexBuffer:ResourceState::UnorderedAccess,ResourceState::VertexBuffer);
        m_rangeArgumentsGraph=graph.ImportBuffer("GPU range arguments",m_rangeArguments,m_rangeReady?ResourceState::IndirectArgument:ResourceState::UnorderedAccess,ResourceState::IndirectArgument);
        m_rangeKeysGraph=graph.ImportBuffer("GPU range keys",m_rangeKeys,m_rangeReady?ResourceState::VertexBuffer:ResourceState::UnorderedAccess,ResourceState::VertexBuffer);
    }
    const auto metadata=graph.ImportBuffer("GPU spatial ranges",m_rangeData,ResourceState::ShaderResource,ResourceState::ShaderResource);
    const auto frame=graph.ImportBuffer("GPU visibility camera",m_frame,ResourceState::ConstantBuffer,ResourceState::ConstantBuffer);
    if(!m_rangeInstancesGraph.IsValid() || !m_rangeArgumentsGraph.IsValid() || !m_rangeKeysGraph.IsValid() || !metadata.IsValid() || !frame.IsValid())return false;
    // Failure of this optional refinement retains the original complete range selection.
    const bool occlusion=afterOcclusion && PrepareRangeOcclusion();
    RGResourceHandle hiz,camera;
    if(occlusion) {
        hiz=graph.ImportTexture("GPU current HiZ",m_hiz,
            m_hizReady?ResourceState::ShaderResource:ResourceState::Undefined,ResourceState::ShaderResource);
        camera=graph.ImportBuffer("GPU occlusion camera",m_occlusionFrame,
            m_occlusionReady?ResourceState::ConstantBuffer:ResourceState::CopyDestination,ResourceState::ConstantBuffer);
        if(!hiz.IsValid() || !camera.IsValid())return false;
    }
    const uint32_t count=static_cast<uint32_t>(m_ranges.size()),x=std::min(count,65535u),y=(count+x-1)/x;
    const std::array<uint32_t,4> push={count,x,0,0};
    const auto pipeline=occlusion?m_rangeOcclusionPipeline:m_rangePipeline;
    const auto resources=occlusion?m_rangeOcclusionResources:m_rangeResources;
    auto& pass=graph.AddPass(afterOcclusion?"GPU ranges after HiZ":"GPU spatial range selection",RGRecordingMode::Independent)
        .Read(metadata,ResourceState::ShaderResource).Read(frame,ResourceState::ConstantBuffer)
        .Read(m_instancesGraph,ResourceState::ShaderResource).Read(m_argumentsGraph,ResourceState::ShaderResource).Read(m_instanceKeysGraph,ResourceState::ShaderResource)
        .Write(m_rangeInstancesGraph,ResourceState::UnorderedAccess).Write(m_rangeArgumentsGraph,ResourceState::UnorderedAccess).Write(m_rangeKeysGraph,ResourceState::UnorderedAccess)
        .SetPipeline(pipeline).SetExecute([pipeline,resources,push,x,y](ICommandList* commands) {
            commands->BindComputePipeline(pipeline);commands->BindResourceSet(resources);commands->SetInlineConstants(0,sizeof(push),push.data());commands->Dispatch(x,y,1);
        });
    if(occlusion)pass.Read(hiz,ResourceState::ShaderResource).Read(camera,ResourceState::ConstantBuffer);
    return true;
}
}
