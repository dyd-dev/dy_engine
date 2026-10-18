#include "VolumetricFog.h"
#include "ShaderLayout.h"
#include "dyf/RHI/IDevice.h"
#include "dyf/RHI/ICommandList.h"
#include "dyf/RHI/Buffer.h"
#include "dyf/RHI/Pipeline.h"
#include "dyf/RHI/Binding.h"
#include "dyf/RHI/Shader.h"
#include "dyf/RHI/Texture.h"
#include <algorithm>
#include <cmath>
#if defined(ENABLE_METAL) || defined(ENABLE_D3D12) || defined(ENABLE_VULKAN)
#include "TemporalVertex.h"
#include "VolumetricFogFragment.h"
#endif

namespace dyf
{
VolumetricFog::~VolumetricFog() {Reset();}

void VolumetricFog::ReleaseSets()
{
    for(const auto& set:m_sets) m_device->DestroyResourceSet(set.resources);
    m_sets.clear();
}

void VolumetricFog::ReleaseTargets()
{
    ReleaseSets();
    m_device->DestroyTexture(m_fog);m_device->DestroyTexture(m_color);
    m_fog=m_color=m_colorOutput=nullptr;m_colorHandle={};
    m_fogState=m_colorState=RHI::ResourceState::Undefined;
}

void VolumetricFog::Reset()
{
    if(m_device)
    {
        ReleaseTargets();
        m_device->DestroyPipeline(m_pipeline);
        m_device->DestroyShader(m_vertex);m_device->DestroyShader(m_fragment);
        m_device->DestroyBuffer(m_frame);m_device->DestroyBuffer(m_noShadow);
    }
    m_device=nullptr;m_vertex=m_fragment=nullptr;m_pipeline=nullptr;m_frame=m_noShadow=nullptr;
    m_uploadReady=m_uploadPending=m_pending=m_prepared=false;m_width=m_height=0;m_cacheFrame=0;
}

bool VolumetricFog::PreparePipeline()
{
    using namespace RHI;
    if(m_pipeline) return true;
#if defined(ENABLE_METAL)
    constexpr const char* vertexEntry="vertexMain";
    constexpr const char* fragmentEntry="fragmentMain";
#else
    constexpr const char* vertexEntry="main";
    constexpr const char* fragmentEntry="main";
#endif
#if defined(ENABLE_METAL) || defined(ENABLE_D3D12) || defined(ENABLE_VULKAN)
    if(!m_vertex) m_vertex=m_device->CreateShader({ShaderStage::Vertex,vertexEntry,kTemporalVertex,kTemporalVertexSize});
    if(!m_fragment) m_fragment=m_device->CreateShader({ShaderStage::Fragment,fragmentEntry,kVolumetricFogFragment,kVolumetricFogFragmentSize});
#endif
    if(!m_vertex || !m_fragment) return false;
    SamplerDesc sampler;
    sampler.minFilter=sampler.magFilter=sampler.mipFilter=SamplerFilter::Nearest;
    sampler.addressU=sampler.addressV=sampler.addressW=SamplerAddressMode::ClampToEdge;
    sampler.mipLodBias=sampler.minLod=sampler.maxLod=0;
    const ResourceBindingLayout bindings[]={{0,ResourceBindingType::SampledTexture,1,ShaderStageFlags::Fragment,{}},
        {1,ResourceBindingType::SampledTexture,1,ShaderStageFlags::Fragment,{}},
        {2,ResourceBindingType::SampledTexture,1,ShaderStageFlags::Fragment,{}},
        {3,ResourceBindingType::SampledTexture,1,ShaderStageFlags::Fragment,{}},
        {8,ResourceBindingType::StaticSampler,1,ShaderStageFlags::Fragment,sampler},
        {12,ResourceBindingType::ConstantBuffer,1,ShaderStageFlags::Fragment,{}},
        {13,ResourceBindingType::ConstantBuffer,1,ShaderStageFlags::Fragment,{}},
        {16,ResourceBindingType::ConstantBuffer,1,ShaderStageFlags::Fragment,{}}};
    const ColorAttachmentDesc color{Format::R16G16B16A16_FLOAT,{},ColorWriteMask::All};
    GraphicsPipelineDesc desc;desc.vertexShader=m_vertex;desc.fragmentShader=m_fragment;
    desc.topology=PrimitiveTopology::TriangleList;
    desc.raster={FillMode::Solid,CullMode::None,FrontFace::CounterClockwise,0,0,0};
    desc.colorAttachments=&color;desc.colorAttachmentCount=1;
    desc.layout={bindings,8,16,ShaderStageFlags::Fragment,15};
    m_pipeline=m_device->CreateGraphicsPipeline(desc);return m_pipeline!=nullptr;
}

bool VolumetricFog::Prepare(RHI::IDevice& device,RHI::ICommandList& uploads,const Camera& camera,
    uint32_t width,uint32_t height,const Settings& settings)
{
    using namespace RHI;
    if(m_device && m_device!=&device) Reset();
    m_device=&device;m_prepared=false;m_pending=false;m_uploadPending=false;
    m_colorOutput=nullptr;m_colorHandle={};m_settings=settings;
    if(!width || !height || !std::isfinite(settings.density) || settings.density<0 ||
        !std::isfinite(settings.heightFalloff) || settings.heightFalloff<0 || !std::isfinite(settings.baseHeight) ||
        !std::isfinite(settings.maxDistance) || settings.maxDistance<=0 || !settings.steps ||
        !std::isfinite(settings.anisotropy) || std::abs(settings.anisotropy)>=1 ||
        !std::isfinite(settings.resolutionScale) || settings.resolutionScale<=0 || settings.resolutionScale>1) return false;
    for(float value:{settings.albedo.x,settings.albedo.y,settings.albedo.z})
        if(!std::isfinite(value) || value<0 || value>1) return false;
    if(!settings.enabled || settings.density==0) {m_width=width;m_height=height;m_prepared=true;return true;}
    if(!PreparePipeline()) return false;
    const uint32_t lowWidth=static_cast<uint32_t>(std::ceil(double(width)*settings.resolutionScale));
    const uint32_t lowHeight=static_cast<uint32_t>(std::ceil(double(height)*settings.resolutionScale));
    if(!m_fog || !m_color || width!=m_width || height!=m_height ||
        m_color->GetDesc().width!=width || m_color->GetDesc().height!=height ||
        m_fog->GetDesc().width!=lowWidth || m_fog->GetDesc().height!=lowHeight)
    {
        ReleaseTargets();
        TextureDesc desc;desc.width=lowWidth;desc.height=lowHeight;desc.format=Format::R16G16B16A16_FLOAT;
        desc.usage=TextureUsage::RenderTarget|TextureUsage::ShaderResource;
        m_fog=device.CreateTexture(desc);desc.width=width;desc.height=height;m_color=device.CreateTexture(desc);
        if(!m_fog || !m_color) {ReleaseTargets();return false;}
    }
    if(!m_frame) m_frame=device.CreateBuffer({sizeof(Frame),0,BufferUsage::Constant,ResourceState::CopyDestination});
    if(!m_noShadow) m_noShadow=device.CreateBuffer({sizeof(RendererShadowConstants),0,BufferUsage::Constant,ResourceState::CopyDestination});
    if(!m_frame || !m_noShadow) return false;
    Frame frame;static_assert(sizeof(Frame)==208);
    if(!Math::Inverse(camera.projection*camera.view,frame.inverseViewProjection)) return false;
    frame.view=camera.view;frame.camera={camera.position.x,camera.position.y,camera.position.z,0};
    frame.size={float(width),float(height),1.f/width,1.f/height};
    frame.fog={settings.density,settings.heightFalloff,settings.baseHeight,settings.maxDistance};
    frame.scattering={settings.albedo.x,settings.albedo.y,settings.albedo.z,settings.anisotropy};
    frame.options={settings.steps,std::abs(camera.projection.m[15])<1e-6f?1u:0u,0,0};
    if(m_uploadReady)
    {
        const ResourceBarrierDesc barrier={m_frame,nullptr,ResourceState::ConstantBuffer,ResourceState::CopyDestination,{}};
        uploads.ResourceBarrier(&barrier,1);
    }
    if(!device.UpdateBuffer(uploads,m_frame,0,&frame,sizeof(frame))) return false;
    const ResourceBarrierDesc ready={m_frame,nullptr,ResourceState::CopyDestination,ResourceState::ConstantBuffer,{}};
    uploads.ResourceBarrier(&ready,1);
    if(!m_uploadReady)
    {
        // 뷰 개수가 모두 0이므로 비활성 그림자 경로는 바인딩된 깊이를 샘플하지 않는다.
        const RendererShadowConstants noShadows={};
        if(!device.UpdateBuffer(uploads,m_noShadow,0,&noShadows,sizeof(noShadows))) return false;
        const ResourceBarrierDesc barrier={m_noShadow,nullptr,ResourceState::CopyDestination,ResourceState::ConstantBuffer,{}};
        uploads.ResourceBarrier(&barrier,1);
    }
    m_width=width;m_height=height;m_uploadPending=true;m_prepared=true;++m_cacheFrame;
    const uint64_t retained=std::max(2u,device.GetDesc().maxFramesInFlight);
    m_sets.erase(std::remove_if(m_sets.begin(),m_sets.end(),[&](const CachedSet& set) {
        if(m_cacheFrame-set.used<=retained) return false;
        device.DestroyResourceSet(set.resources);return true;
    }),m_sets.end());
    return true;
}

RHI::ResourceSetHandle VolumetricFog::Resources(const std::array<RHI::TextureHandle,4>& images,
    RHI::BufferHandle lights,RHI::BufferHandle shadows)
{
    using namespace RHI;
    for(auto& set:m_sets) if(set.images==images && set.lights==lights && set.shadows==shadows)
        {set.used=m_cacheFrame;return set.resources;}
    const ResourceBinding bindings[]={{0,0,nullptr,images[0],0,0,{}},{1,0,nullptr,images[1],0,0,{}},
        {2,0,nullptr,images[2],0,0,{}},{3,0,nullptr,images[3],0,0,{}},
        {12,0,m_frame,nullptr,0,sizeof(Frame),{}},{13,0,lights,nullptr,0,sizeof(RendererLightingConstants),{}},
        {16,0,shadows,nullptr,0,sizeof(RendererShadowConstants),{}}};
    auto resources=m_device->CreateResourceSet({m_pipeline,bindings,7});
    if(!resources) return nullptr;
    try {m_sets.push_back({images,lights,shadows,resources,m_cacheFrame});}
    catch(...) {m_device->DestroyResourceSet(resources);throw;}
    return resources;
}

bool VolumetricFog::AddPasses(RHI::RenderGraph& graph,RHI::TextureHandle hdr,RHI::RGResourceHandle hdrHandle,
    RHI::TextureHandle depth,RHI::RGResourceHandle depthHandle,const LightingResources& lighting)
{
    using namespace RHI;
    if(!m_prepared || m_pending || !hdr || !hdrHandle.IsValid()) return false;
    if(!m_settings.enabled || m_settings.density==0) {m_colorOutput=hdr;m_colorHandle=hdrHandle;return true;}
    if(!depth || !depthHandle.IsValid() || !lighting.lights || hdr->GetDesc().width!=m_width || hdr->GetDesc().height!=m_height ||
        depth->GetDesc().width!=m_width || depth->GetDesc().height!=m_height) return false;
    const bool shadows=lighting.shadows && lighting.shadowDepth;
    if(shadows && !lighting.shadowDepthHandle.IsValid()) return false;
    const auto shadowConstants=shadows?lighting.shadows:m_noShadow;
    const auto shadowDepth=shadows?lighting.shadowDepth:depth;
    const auto shadowDepthHandle=shadows?lighting.shadowDepthHandle:depthHandle;
    const auto lightsHandle=lighting.lightsHandle.IsValid()?lighting.lightsHandle:
        graph.ImportBuffer("Volumetric fog lights",lighting.lights,ResourceState::ConstantBuffer,ResourceState::ConstantBuffer);
    const auto shadowsHandle=shadows && lighting.shadowsHandle.IsValid()?lighting.shadowsHandle:
        graph.ImportBuffer(shadows?"Volumetric fog shadows":"Volumetric fog disabled shadows",shadowConstants,ResourceState::ConstantBuffer,ResourceState::ConstantBuffer);
    const auto frameHandle=graph.ImportBuffer("Volumetric fog frame",m_frame,ResourceState::ConstantBuffer,ResourceState::ConstantBuffer);
    const auto fogHandle=graph.ImportTexture("Volumetric scattering transmission",m_fog,m_fogState,ResourceState::ShaderResource);
    m_colorHandle=graph.ImportTexture("Volumetric fog HDR",m_color,m_colorState,ResourceState::ShaderResource);
    if(!lightsHandle.IsValid() || !shadowsHandle.IsValid() || !frameHandle.IsValid() || !fogHandle.IsValid() || !m_colorHandle.IsValid()) return false;
    for(uint32_t operation=0;operation<2;++operation)
    {
        const std::array<TextureHandle,4> images={hdr,depth,operation?m_fog:depth,shadowDepth};
        auto resources=Resources(images,lighting.lights,shadowConstants);
        if(!resources) return false;
        auto& pass=graph.AddPass(operation?"Volumetric depth aware composite":"Volumetric single scattering", RHI::RGRecordingMode::Independent);
        pass.Read(hdrHandle,ResourceState::ShaderResource).Read(depthHandle,ResourceState::ShaderResource)
            .Read(lightsHandle,ResourceState::ConstantBuffer).Read(shadowsHandle,ResourceState::ConstantBuffer)
            .Read(frameHandle,ResourceState::ConstantBuffer).SetPipeline(m_pipeline);
        if(shadowDepthHandle!=depthHandle) pass.Read(shadowDepthHandle,ResourceState::ShaderResource);
        if(operation) pass.Read(fogHandle,ResourceState::ShaderResource);
        pass.Write(operation?m_colorHandle:fogHandle,ResourceState::RenderTarget);
        const auto destination=operation?m_color:m_fog;const auto pipeline=m_pipeline;
        const std::array<float,4> settings={float(operation),0,0,0};
        pass.SetExecute([pipeline,resources,destination,settings](ICommandList* commands) {
            ColorAttachment color;color.texture=destination;color.loadOp=LoadOp::Discard;color.storeOp=StoreOp::Store;
            const auto& desc=destination->GetDesc();
            commands->BeginRendering({&color,1,nullptr});commands->BindGraphicsPipeline(pipeline);commands->BindResourceSet(resources);
            commands->SetViewport({0,0,float(desc.width),float(desc.height),0,1});commands->SetScissor({0,0,desc.width,desc.height});
            commands->SetInlineConstants(0,sizeof(settings),settings.data());commands->DrawInstanced(3,1,0,0);commands->EndRendering();
        });
    }
    m_colorOutput=m_color;m_pending=true;return true;
}

void VolumetricFog::CommitUploads()
{
    if(m_uploadPending) {m_uploadReady=true;m_uploadPending=false;}
}

void VolumetricFog::CommitFrame()
{
    if(m_pending) {m_fogState=m_colorState=RHI::ResourceState::ShaderResource;m_pending=false;}
}
}
