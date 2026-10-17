#include "ExposureMeter.h"
#include "dyf/RHI/IDevice.h"
#include "dyf/RHI/ICommandList.h"
#include "dyf/RHI/Pipeline.h"
#include "dyf/RHI/ResourceSet.h"
#include "dyf/RHI/Shader.h"
#include "dyf/RHI/Texture.h"
#include <algorithm>
#include <cmath>
#include <string>
#if defined(ENABLE_METAL) || defined(ENABLE_D3D12) || defined(ENABLE_VULKAN)
#include "ExposureMeterFragment.h"
#endif

namespace dyf
{
ExposureMeter::~ExposureMeter()
{
    ReleaseTargets();
    m_device.DestroyPipeline(m_pipeline);m_device.DestroyShader(m_shader);
}

void ExposureMeter::Invalidate()
{
    m_valid=false;m_pending=false;m_previousTime={};
}

void ExposureMeter::ReleaseTargets()
{
    for(const auto& set:m_sets) m_device.DestroyResourceSet(set.set);
    m_sets.clear();
    for(auto& target:m_levels) m_device.DestroyTexture(target.image);
    m_levels.clear();
    for(auto& target:m_history) {m_device.DestroyTexture(target.image);target={};}
    m_output=nullptr;m_outputGraph={};m_width=m_height=0;m_write=0;Invalidate();
}

bool ExposureMeter::CreateTarget(Target& target,uint32_t width,uint32_t height)
{
    RHI::TextureDesc desc;
    desc.width=width;desc.height=height;desc.format=RHI::Format::R16G16B16A16_FLOAT;
    desc.usage=RHI::TextureUsage::RenderTarget|RHI::TextureUsage::ShaderResource;
    target.image=m_device.CreateTexture(desc);target.state=RHI::ResourceState::Undefined;
    return target.image!=nullptr;
}

bool ExposureMeter::PreparePipeline(RHI::ShaderHandle vertex)
{
    using namespace RHI;
    if(m_pipeline) return true;
    if(!vertex) return false;
#if defined(ENABLE_METAL)
    constexpr const char* entry="fragmentMain";
#else
    constexpr const char* entry="main";
#endif
#if defined(ENABLE_METAL) || defined(ENABLE_D3D12) || defined(ENABLE_VULKAN)
    if(!m_shader) m_shader=m_device.CreateShader({ShaderStage::Fragment,entry,kExposureMeterFragment,kExposureMeterFragmentSize});
#endif
    if(!m_shader) return false;
    SamplerDesc sampler;
    sampler.minFilter=sampler.magFilter=sampler.mipFilter=SamplerFilter::Nearest;
    sampler.addressU=sampler.addressV=sampler.addressW=SamplerAddressMode::ClampToEdge;
    sampler.mipLodBias=sampler.minLod=sampler.maxLod=0;
    const ResourceBindingLayout bindings[]={{0,ResourceBindingType::SampledTexture,1,ShaderStageFlags::Fragment,{}},
        {1,ResourceBindingType::SampledTexture,1,ShaderStageFlags::Fragment,{}},
        {8,ResourceBindingType::StaticSampler,1,ShaderStageFlags::Fragment,sampler}};
    const ColorAttachmentDesc color{Format::R16G16B16A16_FLOAT,{},ColorWriteMask::All};
    GraphicsPipelineDesc desc;
    desc.vertexShader=vertex;desc.fragmentShader=m_shader;desc.topology=PrimitiveTopology::TriangleList;
    desc.raster={FillMode::Solid,CullMode::None,FrontFace::CounterClockwise,0,0,0};
    desc.colorAttachments=&color;desc.colorAttachmentCount=1;
    desc.layout={bindings,3,16,ShaderStageFlags::Fragment,15};
    m_pipeline=m_device.CreateGraphicsPipeline(desc);
    return m_pipeline!=nullptr;
}

bool ExposureMeter::Prepare(uint32_t width,uint32_t height,RHI::ShaderHandle vertex,float adaptationSpeed)
{
    if(!width || !height || !std::isfinite(adaptationSpeed) || adaptationSpeed<0) return false;
    if(width!=m_width || height!=m_height) ReleaseTargets();
    if(adaptationSpeed!=m_adaptationSpeed) Invalidate();
    m_adaptationSpeed=adaptationSpeed;m_pending=false;
    if(!PreparePipeline(vertex)) return false;
    if(m_levels.empty())
    {
        uint32_t w=width,h=height;
        do
        {
            w=w/4+(w%4!=0);h=h/4+(h%4!=0);
            m_levels.emplace_back();
            if(!CreateTarget(m_levels.back(),w,h)) {ReleaseTargets();return false;}
        } while(w>1 || h>1);
        for(auto& target:m_history)
            if(!CreateTarget(target,1,1)) {ReleaseTargets();return false;}
    }
    m_width=width;m_height=height;m_frameTime=Clock::now();
    ++m_frame;
    const auto retained=std::max(2u,m_device.GetDesc().maxFramesInFlight);
    m_sets.erase(std::remove_if(m_sets.begin(),m_sets.end(),[&](const auto& set) {
        if(m_frame-set.used<=retained) return false;
        m_device.DestroyResourceSet(set.set);return true;
    }),m_sets.end());
    return true;
}

bool ExposureMeter::AddDraw(RHI::RenderGraph& graph,const std::string& name,RHI::TextureHandle source,
    RHI::RGResourceHandle sourceGraph,RHI::TextureHandle previous,RHI::RGResourceHandle previousGraph,
    Target& output,RHI::RGResourceHandle outputGraph,const std::array<float,4>& operation)
{
    using namespace RHI;
    if(!source || !previous || !sourceGraph.IsValid() || !outputGraph.IsValid()) return false;
    const std::array<TextureHandle,2> images={source,previous};
    ResourceSetHandle resources=nullptr;
    for(auto& set:m_sets) if(set.images==images) {resources=set.set;set.used=m_frame;break;}
    if(!resources)
    {
        const ResourceBinding bindings[]={{0,0,nullptr,source,0,0,{}},{1,0,nullptr,previous,0,0,{}}};
        resources=m_device.CreateResourceSet({m_pipeline,bindings,2});
        if(!resources) return false;
        try {m_sets.push_back({images,resources,m_frame});}
        catch(...) {m_device.DestroyResourceSet(resources);throw;}
    }
    auto& pass=graph.AddPass(name, RHI::RGRecordingMode::Independent);
    pass.Read(sourceGraph,ResourceState::ShaderResource);
    if(previous!=source)
    {
        if(!previousGraph.IsValid()) return false;
        pass.Read(previousGraph,ResourceState::ShaderResource);
    }
    pass.Write(outputGraph,ResourceState::RenderTarget).SetPipeline(m_pipeline);
    const auto target=output.image;
    const auto pipeline=m_pipeline;
    pass.SetExecute([target,pipeline,resources,operation](ICommandList* commands) {
        ColorAttachment color;color.texture=target;color.loadOp=LoadOp::Discard;color.storeOp=StoreOp::Store;
        const auto& desc=target->GetDesc();
        commands->BeginRendering({&color,1,nullptr});commands->BindGraphicsPipeline(pipeline);commands->BindResourceSet(resources);
        commands->SetViewport({0,0,float(desc.width),float(desc.height),0,1});commands->SetScissor({0,0,desc.width,desc.height});
        commands->SetInlineConstants(0,sizeof(operation),operation.data());commands->DrawInstanced(3,1,0,0);commands->EndRendering();
    });
    return true;
}

bool ExposureMeter::AddPasses(RHI::RenderGraph& graph,RHI::TextureHandle source,RHI::RGResourceHandle sourceGraph)
{
    using namespace RHI;
    if(m_pending || !source || !sourceGraph.IsValid() || m_levels.empty() ||
        source->GetDesc().width!=m_width || source->GetDesc().height!=m_height) return false;
    for(uint32_t i=0;i<m_levels.size();++i)
    {
        auto& target=m_levels[i];
        const auto handle=graph.ImportTexture("Exposure luminance level "+std::to_string(i),target.image,target.state,ResourceState::ShaderResource);
        if(!AddDraw(graph,i?"Exposure luminance reduction "+std::to_string(i):"Exposure log luminance",
            source,sourceGraph,source,sourceGraph,target,handle,{i?1.f:0.f,0,0,0})) return false;
        source=target.image;sourceGraph=handle;
    }
    auto previous=source;auto previousGraph=sourceGraph;
    if(m_valid)
    {
        auto& target=m_history[1-m_write];previous=target.image;
        previousGraph=graph.ImportTexture("Exposure previous adaptation",target.image,target.state,ResourceState::ShaderResource);
        if(!previousGraph.IsValid()) return false;
    }
    auto& target=m_history[m_write];
    m_outputGraph=graph.ImportTexture("Exposure adapted luminance",target.image,target.state,ResourceState::ShaderResource);
    const double seconds=m_valid?std::chrono::duration<double>(m_frameTime-m_previousTime).count():0.0;
    const float alpha=float(-std::expm1(-double(m_adaptationSpeed)*std::max(0.0,seconds)));
    if(!AddDraw(graph,"Exposure adaptation",source,sourceGraph,previous,previousGraph,target,m_outputGraph,
        {2,m_valid?1.f:0.f,alpha,0})) return false;
    m_output=target.image;m_pending=true;return true;
}

void ExposureMeter::CommitFrame()
{
    if(!m_pending) return;
    for(auto& target:m_levels) target.state=RHI::ResourceState::ShaderResource;
    m_history[m_write].state=RHI::ResourceState::ShaderResource;
    m_write=1-m_write;m_valid=true;m_pending=false;m_previousTime=m_frameTime;
}
}
