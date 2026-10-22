#include "TransparentRendering.h"
#include "RHI/FormatCapabilities.h"
#include "Platform/Profiler/RhiProfiler.h"
#include "SceneAlgorithms.h"
#include "ShaderLayout.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <string>
#include <unordered_set>
#if defined(ENABLE_METAL)
#include "TransparentLibrary.h"
#elif defined(ENABLE_VULKAN) || defined(ENABLE_D3D12)
#include "TransparentVertex.h"
#include "TransparentFragment.h"
#include "TransparentCopyVertex.h"
#include "TransparentCopyFragment.h"
#include "TransparentDepthCopyFragment.h"
#endif
#if defined(ENABLE_METAL) || defined(ENABLE_VULKAN) || defined(ENABLE_D3D12)
#include "TransparentNoAreaFragment.h"
#include "TransparentDepthFragment.h"
#endif

namespace dyf
{
using namespace RHI;
TransparentRendering::~TransparentRendering()
{
    if(!m_device) return;
    for(const auto& set:m_sets) m_device->DestroyResourceSet(set.set);
    for(auto* pipeline:m_pipelines) m_device->DestroyPipeline(pipeline);
    for(auto* pipeline:m_noAreaPipelines) m_device->DestroyPipeline(pipeline);
    for(auto* pipeline:m_depthPipelines) m_device->DestroyPipeline(pipeline);
    m_device->DestroyPipeline(m_copyPipeline);
    m_device->DestroyPipeline(m_depthCopyPipeline);
    for(auto* shader:m_shaders) m_device->DestroyShader(shader);
    m_device->DestroyShader(m_noAreaShader);
    m_device->DestroyShader(m_depthShader);
    m_device->DestroyBuffer(m_frameBuffer);m_device->DestroyBuffer(m_drawBuffer);
    m_device->DestroyBuffer(m_noShadowBuffer);
    m_device->DestroyTexture(m_color);
    m_device->DestroyTexture(m_reactive);
    m_device->DestroyTexture(m_rearColor);
    m_device->DestroyTexture(m_nearestDepth);
    m_device->DestroyTexture(m_opaqueDepth);
    m_device->DestroyTexture(m_fallbackDepth);
}

bool TransparentRendering::CreatePipelines()
{
    const bool depthReady=m_layers==1 || std::all_of(m_depthPipelines.begin(),m_depthPipelines.end(),
        [](PipelineHandle pipeline){return pipeline!=nullptr;});
    if(m_copyPipeline && m_depthCopyPipeline && m_pipelines[0] && m_pipelines[1] && m_pipelines[2] && m_pipelines[3] && depthReady) return true;
    std::array<ShaderDesc,5> shaders={};
#if defined(ENABLE_METAL)
    const char* names[]={"transparentVertex","transparentFragment","transparentCopyVertex","transparentCopyFragment","transparentDepthCopyFragment"};
    for(uint32_t i=0;i<shaders.size();++i) shaders[i]={i==0 || i==2?ShaderStage::Vertex:ShaderStage::Fragment,
        names[i],kTransparentLibrary,kTransparentLibrarySize};
#elif defined(ENABLE_VULKAN) || defined(ENABLE_D3D12)
    shaders={{{ShaderStage::Vertex,"main",kTransparentVertex,kTransparentVertexSize},
        {ShaderStage::Fragment,"main",kTransparentFragment,kTransparentFragmentSize},
        {ShaderStage::Vertex,"main",kTransparentCopyVertex,kTransparentCopyVertexSize},
        {ShaderStage::Fragment,"main",kTransparentCopyFragment,kTransparentCopyFragmentSize},
        {ShaderStage::Fragment,"main",kTransparentDepthCopyFragment,kTransparentDepthCopyFragmentSize}}};
#else
    return false;
#endif
    for(uint32_t i=0;i<shaders.size();++i)
        if(!m_shaders[i] && !(m_shaders[i]=m_device->CreateShader(shaders[i]))) return false;
    SamplerDesc material;
    material.minFilter=material.magFilter=material.mipFilter=SamplerFilter::Linear;
    material.addressU=material.addressV=material.addressW=SamplerAddressMode::Repeat;
    material.mipLodBias=material.minLod=0;
    material.maxLod=32;
    material.maxAnisotropy=static_cast<uint32_t>(std::clamp<uint64_t>(m_device->GetLimit(Limit::SamplerAnisotropy),1,8));
    auto linear=material;linear.maxAnisotropy=1;linear.maxLod=0;linear.mipFilter=SamplerFilter::Nearest;
    linear.addressU=linear.addressV=linear.addressW=SamplerAddressMode::ClampToEdge;
    auto nearest=linear;nearest.minFilter=nearest.magFilter=SamplerFilter::Nearest;nearest.maxLod=32;
    std::vector<ResourceBindingLayout> bindings;
    for(uint32_t i=0;i<5;++i) bindings.push_back({i,ResourceBindingType::SampledTexture,1,ShaderStageFlags::Fragment,{}});
    for(uint32_t i:{5u,6u,7u}) bindings.push_back({i,ResourceBindingType::SampledTexture,1,ShaderStageFlags::Fragment,{}});
    bindings.push_back({8,ResourceBindingType::StaticSampler,1,ShaderStageFlags::Fragment,material});
    bindings.push_back({9,ResourceBindingType::StaticSampler,1,ShaderStageFlags::Fragment,linear});
    bindings.push_back({10,ResourceBindingType::StaticSampler,1,ShaderStageFlags::Fragment,nearest});
    auto environmentLinear=nearest;environmentLinear.addressU=SamplerAddressMode::Repeat;
    if(RHI::Detail::SupportsLinearFiltering(*m_device,Format::R32G32B32A32_FLOAT))
        environmentLinear.minFilter=environmentLinear.magFilter=environmentLinear.mipFilter=SamplerFilter::Linear;
    bindings.push_back({15,ResourceBindingType::StaticSampler,1,ShaderStageFlags::Fragment,environmentLinear});
    bindings.push_back({11,ResourceBindingType::ReadOnlyStorageBuffer,1,ShaderStageFlags::Vertex|ShaderStageFlags::Fragment,{}});
    bindings.push_back({12,ResourceBindingType::ConstantBuffer,1,ShaderStageFlags::Vertex|ShaderStageFlags::Fragment,{}});
    bindings.push_back({13,ResourceBindingType::ConstantBuffer,1,ShaderStageFlags::Fragment,{}});
    bindings.push_back({14,ResourceBindingType::SampledTexture,1,ShaderStageFlags::Fragment,{}});
    bindings.push_back({16,ResourceBindingType::ConstantBuffer,1,ShaderStageFlags::Fragment,{}});
    bindings.push_back({17,ResourceBindingType::SampledTexture,1,ShaderStageFlags::Fragment,{}});
    bindings.push_back({19,ResourceBindingType::ReadOnlyStorageBuffer,1,ShaderStageFlags::Fragment,{}});
    bindings.push_back({20,ResourceBindingType::ReadOnlyStorageBuffer,1,ShaderStageFlags::Fragment,{}});
    bindings.push_back({21,ResourceBindingType::ConstantBuffer,1,ShaderStageFlags::Fragment,{}});
    bindings.push_back({26,ResourceBindingType::SampledTexture,1,ShaderStageFlags::Fragment,{}});
    bindings.push_back({27,ResourceBindingType::SampledTexture,1,ShaderStageFlags::Fragment,{}});
    bindings.push_back({28,ResourceBindingType::SampledTexture,1,ShaderStageFlags::Fragment,{}});
    bindings.push_back({22,ResourceBindingType::ReadOnlyStorageBuffer,1,ShaderStageFlags::Fragment,{}});
    bindings.push_back({23,ResourceBindingType::SampledTexture,1,ShaderStageFlags::Fragment,{}});
    bindings.push_back({24,ResourceBindingType::ConstantBuffer,1,ShaderStageFlags::Fragment,{}});
    const VertexBufferLayout vertex{0,64,VertexStepMode::Vertex};
    const VertexAttribute attributes[]={{0,0,Format::R32G32B32_FLOAT,0},{1,0,Format::R32G32B32_FLOAT,12},
        {2,0,Format::R32G32_FLOAT,24},{3,0,Format::R32G32B32A32_FLOAT,32},
        {8,0,Format::R32G32B32A32_FLOAT,48}};
    ColorAttachmentDesc color{Format::R16G16B16A16_FLOAT,
        {true,BlendFactor::One,BlendFactor::OneMinusSourceAlpha,BlendOp::Add,
            BlendFactor::One,BlendFactor::OneMinusSourceAlpha,BlendOp::Add},ColorWriteMask::All};
    ColorAttachmentDesc transparentColors[]={color,color};
    GraphicsPipelineDesc desc;
    desc.vertexShader=m_shaders[0];desc.fragmentShader=m_shaders[1];desc.topology=PrimitiveTopology::TriangleList;
    desc.vertexBuffers=&vertex;desc.vertexBufferCount=1;desc.vertexAttributes=attributes;desc.vertexAttributeCount=5;
    desc.raster={FillMode::Solid,CullMode::Back,FrontFace::CounterClockwise,0,0,0};
    // Sample a separate R32 copy while testing the original opaque D32 attachment.
    desc.depthStencil.format=Format::D32_FLOAT;
    desc.depthStencil.depthTestEnabled=true;desc.depthStencil.depthWriteEnabled=false;
    desc.depthStencil.depthCompareOp=CompareOp::LessEqual;
    desc.colorAttachments=transparentColors;desc.colorAttachmentCount=2;
    desc.layout={bindings.data(),static_cast<uint32_t>(bindings.size()),16,ShaderStageFlags::Vertex|ShaderStageFlags::Fragment,15};
    if(m_reactiveFormat==Format::Unknown)
    {
        transparentColors[1].format=Format::R16_FLOAT;
        transparentColors[1].writeMask=ColorWriteMask::Red;
        // This query also proves sampled-image support; unknown backends keep RGBA16F.
        // Supports(desc) checks attachment blending and independent write masks together.
        m_reactiveFormat=RHI::Detail::SupportsLinearFiltering(*m_device,Format::R16_FLOAT) && m_device->Supports(desc)
            ?Format::R16_FLOAT:Format::R16G16B16A16_FLOAT;
    }
    transparentColors[1].format=m_reactiveFormat;
    transparentColors[1].writeMask=m_reactiveFormat==Format::R16_FLOAT?ColorWriteMask::Red:ColorWriteMask::All;
    // OneMinusSourceAlpha uses the fragment output alpha, including for a red-only target.
    for(uint32_t i=0;i<m_pipelines.size();++i)
    {
        desc.raster.cullMode=(i&1u)?CullMode::None:CullMode::Back;
        desc.raster.frontFace=(i&2u)?FrontFace::Clockwise:FrontFace::CounterClockwise;
        if(!m_pipelines[i] && !(m_pipelines[i]=m_device->CreateGraphicsPipeline(desc))) return false;
    }
    if(!m_noAreaAttempted)
    {
        m_noAreaAttempted=true;
        try
        {
#if defined(ENABLE_METAL)
            const ShaderDesc noArea{ShaderStage::Fragment,"transparentNoAreaFragment",kTransparentNoAreaFragment,kTransparentNoAreaFragmentSize};
#elif defined(ENABLE_VULKAN) || defined(ENABLE_D3D12)
            const ShaderDesc noArea{ShaderStage::Fragment,"main",kTransparentNoAreaFragment,kTransparentNoAreaFragmentSize};
#endif
#if defined(ENABLE_METAL) || defined(ENABLE_VULKAN) || defined(ENABLE_D3D12)
            m_noAreaShader=m_device->CreateShader(noArea);
            if(m_noAreaShader)
            {
                auto noAreaDesc=desc;noAreaDesc.fragmentShader=m_noAreaShader;
                for(uint32_t i=0;i<m_noAreaPipelines.size();++i)
                {
                    noAreaDesc.raster.cullMode=(i&1u)?CullMode::None:CullMode::Back;
                    noAreaDesc.raster.frontFace=(i&2u)?FrontFace::Clockwise:FrontFace::CounterClockwise;
                    m_noAreaPipelines[i]=m_device->CreateGraphicsPipeline(noAreaDesc);
                    if(!m_noAreaPipelines[i]) break;
                }
            }
#endif
        }
        catch(...) { /* Optional specialization retains the original pipelines. */ }
        if(!std::all_of(m_noAreaPipelines.begin(),m_noAreaPipelines.end(),[](PipelineHandle value){return value!=nullptr;}))
        {
            for(auto& value:m_noAreaPipelines) {m_device->DestroyPipeline(value);value=nullptr;}
            m_device->DestroyShader(m_noAreaShader);m_noAreaShader=nullptr;
        }
    }
    if(m_layers==2)
    {
        if(!m_depthShader)
        {
#if defined(ENABLE_METAL)
            const ShaderDesc depthShader{ShaderStage::Fragment,"transparentDepthFragment",kTransparentDepthFragment,kTransparentDepthFragmentSize};
#elif defined(ENABLE_VULKAN) || defined(ENABLE_D3D12)
            const ShaderDesc depthShader{ShaderStage::Fragment,"main",kTransparentDepthFragment,kTransparentDepthFragmentSize};
#endif
#if defined(ENABLE_METAL) || defined(ENABLE_VULKAN) || defined(ENABLE_D3D12)
            m_depthShader=m_device->CreateShader(depthShader);
#endif
            if(!m_depthShader) return false;
        }
        const ResourceBindingLayout depthBindings[]={
            {0,ResourceBindingType::SampledTexture,1,ShaderStageFlags::Fragment,{}},
            {6,ResourceBindingType::SampledTexture,1,ShaderStageFlags::Fragment,{}},
            {8,ResourceBindingType::StaticSampler,1,ShaderStageFlags::Fragment,material},
            {10,ResourceBindingType::StaticSampler,1,ShaderStageFlags::Fragment,nearest},
            {11,ResourceBindingType::ReadOnlyStorageBuffer,1,ShaderStageFlags::Vertex|ShaderStageFlags::Fragment,{}},
            {12,ResourceBindingType::ConstantBuffer,1,ShaderStageFlags::Vertex,{}}};
        auto depthDesc=desc;
        depthDesc.fragmentShader=m_depthShader;
        depthDesc.colorAttachments=nullptr;depthDesc.colorAttachmentCount=0;
        depthDesc.depthStencil.format=Format::D32_FLOAT;
        depthDesc.depthStencil.depthTestEnabled=true;depthDesc.depthStencil.depthWriteEnabled=true;
        depthDesc.depthStencil.depthCompareOp=CompareOp::Less;
        depthDesc.layout={depthBindings,6,16,ShaderStageFlags::Vertex|ShaderStageFlags::Fragment,15};
        for(uint32_t i=0;i<m_depthPipelines.size();++i)
        {
            depthDesc.raster.cullMode=(i&1u)?CullMode::None:CullMode::Back;
            depthDesc.raster.frontFace=(i&2u)?FrontFace::Clockwise:FrontFace::CounterClockwise;
            if(!m_depthPipelines[i] && !(m_depthPipelines[i]=m_device->CreateGraphicsPipeline(depthDesc))) return false;
        }
    }
    const ResourceBindingLayout copyBindings[]={{5,ResourceBindingType::SampledTexture,1,ShaderStageFlags::Fragment,{}},
        {9,ResourceBindingType::StaticSampler,1,ShaderStageFlags::Fragment,linear}};
    color.blend.enabled=false;desc={};desc.vertexShader=m_shaders[2];desc.fragmentShader=m_shaders[3];
    desc.topology=PrimitiveTopology::TriangleList;desc.raster={FillMode::Solid,CullMode::None,FrontFace::CounterClockwise,0,0,0};
    desc.colorAttachments=&color;desc.colorAttachmentCount=1;desc.layout={copyBindings,2,0,ShaderStageFlags::None,0};
    if(!m_copyPipeline && !(m_copyPipeline=m_device->CreateGraphicsPipeline(desc))) return false;
    const ResourceBindingLayout depthCopyBindings[]={{5,ResourceBindingType::SampledTexture,1,ShaderStageFlags::Fragment,{}},
        {9,ResourceBindingType::StaticSampler,1,ShaderStageFlags::Fragment,nearest}};
    color.format=Format::R32_FLOAT;
    desc.fragmentShader=m_shaders[4];desc.layout={depthCopyBindings,2,0,ShaderStageFlags::None,0};
    if(!m_depthCopyPipeline && !(m_depthCopyPipeline=m_device->CreateGraphicsPipeline(desc))) return false;
    return true;
}

bool TransparentRendering::Upload(ICommandList& commands,BufferHandle& buffer,bool& ready,
    const void* data,uint32_t bytes,uint32_t stride,BufferUsage usage)
{
    const auto state=usage==BufferUsage::Constant?ResourceState::ConstantBuffer:ResourceState::ShaderResource;
    if(!buffer || buffer->GetDesc().size<bytes)
    {
        // Immutable descriptors retain old buffers through RHI retirement.
        m_device->DestroyBuffer(buffer);buffer=m_device->CreateBuffer({bytes,stride,usage,ResourceState::CopyDestination});ready=false;
    }
    if(!buffer) return false;
    if(ready) {const ResourceBarrierDesc barrier{buffer,nullptr,state,ResourceState::CopyDestination,{}};commands.ResourceBarrier(&barrier,1);}
    if(!m_device->UpdateBuffer(commands,buffer,0,data,bytes)) return false;
    const ResourceBarrierDesc barrier{buffer,nullptr,ResourceState::CopyDestination,state,{}};
    commands.ResourceBarrier(&barrier,1);ready=true;return true;
}

bool TransparentRendering::Prepare(IDevice& device,ICommandList& uploads,const Camera& camera,
    const std::vector<ScreenSpaceLighting::SurfaceDraw>& draws,const EnvironmentDesc& environment,
    const RenderEnvironmentResources& lighting,bool frustumCulling,uint32_t layers,bool trustedEnvironment)
{
    if(layers!=1 && layers!=2) return false;
    m_layers=layers;
    m_nativeFiltering=trustedEnvironment && RHI::Detail::SupportsLinearFiltering(device,Format::R32G32B32A32_FLOAT);
    if(m_recorded)
    {
        // An uncommitted frame may have stopped before restoring the copy's state.
        device.DestroyTexture(m_opaqueDepth);m_opaqueDepth=nullptr;
        m_opaqueDepthState=ResourceState::Undefined;
    }
    m_device=&device;m_draws.clear();m_data.clear();m_recorded=false;m_layeredRecorded=false;m_noShadowPending=false;
    m_fallbackDepthPending=false;
    const CameraFrustum frustum(camera);
    for(const auto& draw:draws)
    {
        if(!draw.material) return false;
        if(UsesBlendedRendering(*draw.material) && draw.indexCount &&
            (!frustumCulling || !frustum.Outside(draw.localBounds,draw.model))) m_draws.push_back(draw);
    }
    if(m_draws.empty()) return true;
    if(m_draws.size()>UINT32_MAX/sizeof(DrawGPU) || !CreatePipelines()) return false;
    if(!m_noShadowReady)
    {
        // Only CommitFrame marks this upload usable. A failed recording or
        // submission gets a fresh buffer on retry, with no guessed GPU state.
        m_device->DestroyBuffer(m_noShadowBuffer);m_noShadowBuffer=nullptr;
        const RendererShadowConstants noShadows={};
        bool uploaded=false;
        if(!Upload(uploads,m_noShadowBuffer,uploaded,&noShadows,sizeof(noShadows),0,BufferUsage::Constant)) return false;
        m_noShadowPending=true;
    }
    const auto distance=[&](const auto& draw) {
        const auto center=draw.localBounds.valid?(draw.localBounds.min+draw.localBounds.max)*0.5f:Math::float3{};
        return Math::TransformPoint(camera.view,Math::TransformPoint(draw.model,center)).z;
    };
    std::stable_sort(m_draws.begin(),m_draws.end(),[&](const auto& a,const auto& b){return distance(a)<distance(b);});
    for(const auto& draw:m_draws)
    {
        const auto& material=*draw.material;
        if(!std::isfinite(material.transmissionFactor+material.indexOfRefraction+material.thickness+material.attenuationDistance) ||
            !std::isfinite(material.attenuationColor.x+material.attenuationColor.y+material.attenuationColor.z+material.normalYSign) ||
            material.indexOfRefraction<1 || material.thickness<0 || material.attenuationDistance<0) return false;
        DrawGPU gpu={};gpu.model=draw.model;
        if(!Math::InverseTranspose(draw.model,gpu.normalMatrix)) gpu.normalMatrix=Math::float4x4::Identity();
        gpu.baseColor=draw.baseColor;gpu.emissive={material.emissiveColor.x,material.emissiveColor.y,material.emissiveColor.z,material.normalYSign};
        gpu.material={material.metallicFactor,material.roughnessFactor,material.normalScale,material.occlusionStrength};
        gpu.optics={material.transmissionFactor,material.indexOfRefraction,material.thickness,material.attenuationDistance};
        gpu.attenuation={material.attenuationColor.x,material.attenuationColor.y,material.attenuationColor.z,material.alphaCutoff};
        gpu.alphaMode=static_cast<uint32_t>(material.alphaMode);
        gpu.flags=draw.textureFlags|(material.unlit?128u:0u);gpu.doubleSided=material.doubleSided?1:0;
        gpu.pipelineIndex=gpu.doubleSided|(Math::Determinant3x3(draw.model)<0?2u:0u);
        m_data.push_back(gpu);
    }
    if(!m_drawReady || m_uploaded.size()!=m_data.size() || std::memcmp(m_uploaded.data(),m_data.data(),m_data.size()*sizeof(DrawGPU)))
    {
        if(!Upload(uploads,m_drawBuffer,m_drawReady,m_data.data(),static_cast<uint32_t>(m_data.size()*sizeof(DrawGPU)),sizeof(DrawGPU),BufferUsage::Storage)) return false;
        m_uploaded=m_data;
    }
    FrameGPU frame={};frame.viewProjection=camera.projection*camera.view;frame.view=camera.view;
    if(!Math::Inverse(frame.viewProjection,frame.inverseViewProjection)) return false;
    frame.camera={camera.position.x,camera.position.y,camera.position.z,0};frame.environment=lighting.constants;
    frame.specularColor={environment.specularColor.x*environment.specularIntensity,
        environment.specularColor.y*environment.specularIntensity,environment.specularColor.z*environment.specularIntensity,0};
    m_prefilter=lighting.prefilter;
    const bool specularIbl=(static_cast<uint32_t>(frame.environment.screen.w+.5f)&2u)!=0u;
    if(specularIbl && !lighting.brdf) return false;
    m_brdf=lighting.brdf?lighting.brdf:lighting.prefilter;
    m_probeData=lighting.probeData;m_probePrefilter=lighting.probePrefilter;m_probeParameters=lighting.probeParameters;
    return m_prefilter && m_brdf && m_probeData && m_probePrefilter && m_probeParameters &&
        Upload(uploads,m_frameBuffer,m_frameReady,&frame,sizeof(frame),0,BufferUsage::Constant);
}

ResourceSetHandle TransparentRendering::Resources(PipelineHandle pipeline,const std::vector<ResourceBinding>& bindings)
{
    for(auto& cached:m_sets)
    {
        if(cached.pipeline!=pipeline || cached.bindings.size()!=bindings.size()) continue;
        bool equal=true;
        for(size_t i=0;i<bindings.size();++i)
        {
            const auto& a=cached.bindings[i];const auto& b=bindings[i];
            equal=equal && a.binding==b.binding && a.arrayElement==b.arrayElement && a.buffer==b.buffer &&
                a.texture==b.texture && a.offset==b.offset && a.size==b.size &&
                a.subresources.firstMipLevel==b.subresources.firstMipLevel && a.subresources.mipLevelCount==b.subresources.mipLevelCount &&
                a.subresources.firstArrayLayer==b.subresources.firstArrayLayer && a.subresources.arrayLayerCount==b.subresources.arrayLayerCount;
        }
        if(equal) {cached.unusedFrames=0;return cached.set;}
    }
    auto* set=m_device->CreateResourceSet({pipeline,bindings.data(),static_cast<uint32_t>(bindings.size())});
    if(set)
    {
        try {m_sets.push_back({pipeline,bindings,set,0});}
        catch(...) {m_device->DestroyResourceSet(set);throw;}
    }
    return set;
}

bool TransparentRendering::AddPasses(RenderGraph& graph,TextureHandle opaque,RGResourceHandle opaqueHandle,
    TextureHandle depth,RGResourceHandle depthHandle,BufferHandle lights,
    TextureHandle shadowDepth,RGResourceHandle shadowDepthHandle,BufferHandle shadowConstants,
    const RenderLightingResources& lighting,const Renderer& renderer)
{
    if(m_draws.empty()) return true;
    if(!opaque || !depth || !lights || !opaqueHandle.IsValid() || !depthHandle.IsValid()) return false;
    if(!lighting.clusterLights.buffer || !lighting.clusterList.buffer || !lighting.clusterConstants.buffer) return false;
    if(opaque->GetDesc().width!=depth->GetDesc().width || opaque->GetDesc().height!=depth->GetDesc().height) return false;
    const bool shadows=shadowDepth && shadowDepthHandle.IsValid() && shadowConstants;
    const auto shadowBuffer=shadows?shadowConstants:m_noShadowBuffer;
    if(!shadowBuffer) return false;
    for(auto& cached:m_sets) ++cached.unusedFrames;
    const auto window=std::max(1u,m_device->GetDesc().maxFramesInFlight);
    m_sets.erase(std::remove_if(m_sets.begin(),m_sets.end(),[&](const auto& set) {
        if(set.unusedFrames<=window) return false;
        m_device->DestroyResourceSet(set.set);return true;
    }),m_sets.end());
    const auto resized=[&](TextureHandle texture) {
        return texture && (texture->GetDesc().width!=opaque->GetDesc().width ||
            texture->GetDesc().height!=opaque->GetDesc().height);
    };
    if(resized(m_color) || resized(m_reactive) || resized(m_rearColor) || resized(m_nearestDepth) || resized(m_opaqueDepth))
    {
        for(auto* texture:{m_color,m_reactive,m_rearColor,m_nearestDepth,m_opaqueDepth})m_device->DestroyTexture(texture);
        m_color=m_reactive=m_rearColor=m_nearestDepth=m_opaqueDepth=nullptr;
        m_state=m_rearState=m_nearestState=m_opaqueDepthState=ResourceState::Undefined;
    }
    if(!m_color)
    {
        auto desc=opaque->GetDesc();desc.format=Format::R16G16B16A16_FLOAT;
        desc.usage=TextureUsage::RenderTarget|TextureUsage::ShaderResource;
        if(!(m_color=m_device->CreateTexture(desc))) return false;
    }
    if(!m_reactive)
    {
        auto desc=m_color->GetDesc();desc.format=m_reactiveFormat;
        if(!(m_reactive=m_device->CreateTexture(desc))) return false;
    }
    if(!m_opaqueDepth)
    {
        auto desc=m_color->GetDesc();desc.format=Format::R32_FLOAT;
        if(!(m_opaqueDepth=m_device->CreateTexture(desc))) return false;
    }
    RGResourceHandle fallbackHandle;
    if(!shadows || m_layers==1)
    {
        if(!m_fallbackDepthReady)
        {
            // A failed frame never publishes this clear; retry with a fresh texture.
            m_device->DestroyTexture(m_fallbackDepth);m_fallbackDepth=nullptr;
            auto desc=depth->GetDesc();desc.width=desc.height=1;desc.depthOrArraySize=desc.mipLevels=1;
            desc.format=Format::D32_FLOAT;desc.usage=TextureUsage::DepthStencil|TextureUsage::ShaderResource;
            if(!(m_fallbackDepth=m_device->CreateTexture(desc))) return false;
        }
        fallbackHandle=graph.ImportTexture("Transparent fallback depth",m_fallbackDepth,
            m_fallbackDepthReady?ResourceState::ShaderResource:ResourceState::Undefined,ResourceState::ShaderResource);
        if(!fallbackHandle.IsValid()) return false;
        if(!m_fallbackDepthReady)
        {
            graph.AddPass("Transparency fallback depth clear",RGRecordingMode::Independent)
                .Write(fallbackHandle,ResourceState::DepthWrite).SetExecute([this](ICommandList* commands) {
                    DepthStencilAttachment attachment;attachment.texture=m_fallbackDepth;attachment.state=ResourceState::DepthWrite;
                    attachment.depthLoadOp=LoadOp::Clear;attachment.depthStoreOp=StoreOp::Store;attachment.clearDepth=1;
                    commands->BeginRendering({nullptr,0,&attachment});commands->EndRendering();
                });
            m_fallbackDepthPending=true;
        }
    }
    const auto shadowTexture=shadows?shadowDepth:m_fallbackDepth;
    const auto shadowHandle=shadows?shadowDepthHandle:fallbackHandle;
    if(m_layers==2)
    {
        if(!m_rearColor && !(m_rearColor=m_device->CreateTexture(m_color->GetDesc()))) return false;
        if(!m_nearestDepth)
        {
            auto desc=m_color->GetDesc();desc.format=Format::D32_FLOAT;
            desc.usage=TextureUsage::DepthStencil|TextureUsage::ShaderResource;
            if(!(m_nearestDepth=m_device->CreateTexture(desc))) return false;
        }
    }
    m_colorHandle=graph.ImportTexture("Transparent HDR",m_color,m_state,ResourceState::ShaderResource);
    m_reactiveHandle=graph.ImportTexture("Transparency reactive mask",m_reactive,m_state,ResourceState::ShaderResource);
    if(!m_colorHandle.IsValid() || !m_reactiveHandle.IsValid()) return false;
    const auto opaqueDepthHandle=graph.ImportTexture("Transparent opaque depth copy",m_opaqueDepth,
        m_opaqueDepthState,ResourceState::ShaderResource);
    if(!opaqueDepthHandle.IsValid()) return false;
    const auto frameHandle=graph.ImportBuffer("Transparent camera",m_frameBuffer,
        ResourceState::ConstantBuffer,ResourceState::ConstantBuffer);
    const auto drawsHandle=graph.ImportBuffer("Transparent draws",m_drawBuffer,
        ResourceState::ShaderResource,ResourceState::ShaderResource);
    const auto lightsHandle=graph.ImportBuffer("Transparent lights",lights,
        ResourceState::ConstantBuffer,ResourceState::ConstantBuffer);
    const auto shadowConstantsHandle=graph.ImportBuffer("Transparent shadow constants",shadowBuffer,
        ResourceState::ConstantBuffer,ResourceState::ConstantBuffer);
    const auto prefilterHandle=graph.ImportTexture("Transparent environment",m_prefilter,
        ResourceState::ShaderResource,ResourceState::ShaderResource);
    const auto brdfHandle=graph.ImportTexture("Transparent GGX split-sum BRDF",m_brdf,
        ResourceState::ShaderResource,ResourceState::ShaderResource);
    const auto probeDataHandle=graph.ImportBuffer("Spatial probe SH and bounds",m_probeData,
        ResourceState::ShaderResource,ResourceState::ShaderResource);
    const auto probeParametersHandle=graph.ImportBuffer("Spatial probe count",m_probeParameters,
        ResourceState::ConstantBuffer,ResourceState::ConstantBuffer);
    const auto probePrefilterHandle=graph.ImportTexture("Spatial probe GGX panoramas",m_probePrefilter,
        ResourceState::ShaderResource,ResourceState::ShaderResource);

    const auto geometryReads=[&](RenderGraphPass& pass) {
        pass.Read(frameHandle,ResourceState::ConstantBuffer).Read(drawsHandle,ResourceState::ShaderResource);
        std::unordered_set<BufferHandle> buffers;
        for(uint32_t i=0;i<m_draws.size();++i)
        {
            const auto& draw=m_draws[i];const auto label=std::to_string(i);
            if(buffers.insert(draw.vertexBuffer).second)
                pass.Read(graph.ImportBuffer("Transparent VB "+label,draw.vertexBuffer,
                    ResourceState::VertexBuffer,ResourceState::VertexBuffer),ResourceState::VertexBuffer);
            if(buffers.insert(draw.indexBuffer).second)
                pass.Read(graph.ImportBuffer("Transparent IB "+label,draw.indexBuffer,
                    ResourceState::IndexBuffer,ResourceState::IndexBuffer),ResourceState::IndexBuffer);
        }
    };
    const auto drawGeometry=[this](ICommandList* commands,const std::vector<ResourceSetHandle>& sets,
        uint32_t operation,bool depthOnly,bool noArea) {
        PipelineHandle boundPipeline=nullptr;
        ResourceSetHandle boundSet=nullptr;
        for(uint32_t i=0;i<m_draws.size();++i)
        {
            const auto& draw=m_draws[i];const auto variant=m_data[i].pipelineIndex;
            const auto pipeline=depthOnly?m_depthPipelines[variant]:noArea?m_noAreaPipelines[variant]:m_pipelines[variant];
            if(pipeline!=boundPipeline)
            {
                commands->BindGraphicsPipeline(pipeline);
                boundPipeline=pipeline;boundSet=nullptr;
            }
            const auto set=sets[i];
            if(set!=boundSet) {commands->BindResourceSet(set);boundSet=set;}
            commands->BindVertexBuffer(0,draw.vertexBuffer,0);
            commands->BindIndexBuffer(draw.indexBuffer,Format::R32_UINT,0);
            const uint32_t index[4]={i,operation,m_nativeFiltering?1u:0u,0};
            commands->SetInlineConstants(0,sizeof(index),index);
            commands->DrawIndexedInstanced(draw.indexCount,1,0,0,0);
        }
    };
    const auto addCopy=[&](const char* name,TextureHandle input,RGResourceHandle inputHandle,
        TextureHandle target,RGResourceHandle targetHandle,bool depthCopy=false) {
        const auto pipeline=depthCopy?m_depthCopyPipeline:m_copyPipeline;
        const auto copy=Resources(pipeline,{{5,0,nullptr,input,0,0,{}}});
        if(!copy) return false;
        graph.AddPass(name,RGRecordingMode::Independent).Read(inputHandle,ResourceState::ShaderResource)
            .Write(targetHandle,ResourceState::RenderTarget).SetExecute([target,copy,pipeline](ICommandList* commands) {
                ColorAttachment color;color.texture=target;color.loadOp=LoadOp::Discard;color.storeOp=StoreOp::Store;
                commands->BeginRendering({&color,1,nullptr});commands->BindGraphicsPipeline(pipeline);
                commands->BindResourceSet(copy);
                commands->SetViewport({0,0,float(target->GetDesc().width),float(target->GetDesc().height),0,1});
                commands->SetScissor({0,0,target->GetDesc().width,target->GetDesc().height});
                commands->DrawInstanced(3,1,0,0);commands->EndRendering();
            });
        return true;
    };
    const auto addShading=[&](const char* name,TextureHandle background,RGResourceHandle backgroundHandle,
        TextureHandle target,RGResourceHandle targetHandle,TextureHandle nearest,RGResourceHandle nearestHandle,
        uint32_t operation,bool clearReactive) {
        auto& pass=graph.AddPass(std::string(name)+(m_nativeFiltering?" Native filter":""),RGRecordingMode::Independent);
        pass.Read(backgroundHandle,ResourceState::ShaderResource).Read(opaqueDepthHandle,ResourceState::ShaderResource)
            // Store may write the attachment even though pixel depth writes are disabled.
            .Write(depthHandle,ResourceState::DepthWrite)
            .Write(targetHandle,ResourceState::RenderTarget).Write(m_reactiveHandle,ResourceState::RenderTarget);
        pass.Read(nearestHandle,ResourceState::ShaderResource).Read(shadowHandle,ResourceState::ShaderResource);
        if(lighting.areaMatrix.resource.IsValid())
            pass.Read(lighting.areaMatrix.resource,ResourceState::ShaderResource);
        if(lighting.areaAmplitude.resource.IsValid())
            pass.Read(lighting.areaAmplitude.resource,ResourceState::ShaderResource);
        pass.Read(lighting.clusterLights.resource,ResourceState::ShaderResource)
            .Read(lighting.clusterList.resource,ResourceState::ShaderResource)
            .Read(lighting.clusterConstants.resource,ResourceState::ConstantBuffer)
            .Read(lightsHandle,ResourceState::ConstantBuffer).Read(shadowConstantsHandle,ResourceState::ConstantBuffer)
            .Read(prefilterHandle,ResourceState::ShaderResource).Read(brdfHandle,ResourceState::ShaderResource)
            .Read(probeDataHandle,ResourceState::ShaderResource)
            .Read(probeParametersHandle,ResourceState::ConstantBuffer).Read(probePrefilterHandle,ResourceState::ShaderResource);
        geometryReads(pass);
        std::vector<ResourceSetHandle> sets;sets.reserve(m_draws.size());
        std::vector<ResourceSetHandle> noAreaSets;
        bool noAreaReady=std::all_of(m_noAreaPipelines.begin(),m_noAreaPipelines.end(),[](PipelineHandle value){return value!=nullptr;});
        if(noAreaReady)
            try {noAreaSets.reserve(m_draws.size());}catch(...) {noAreaReady=false;}
        std::unordered_set<TextureHandle> textures;
        for(uint32_t i=0;i<m_draws.size();++i)
        {
            const auto& draw=m_draws[i];std::vector<ResourceBinding> bindings;
            for(uint32_t slot=0;slot<5;++slot)
            {
                bindings.push_back({slot,0,nullptr,draw.textures[slot],0,0,{}});
                if(textures.insert(draw.textures[slot]).second)
                    pass.Read(graph.ImportTexture("Transparent material "+std::to_string(i)+" "+std::to_string(slot),
                        draw.textures[slot],ResourceState::ShaderResource,ResourceState::ShaderResource),ResourceState::ShaderResource);
            }
            bindings.push_back({5,0,nullptr,background,0,0,{}});
            bindings.push_back({6,0,nullptr,m_opaqueDepth,0,0,{}});
            bindings.push_back({7,0,nullptr,m_prefilter,0,0,{}});
            bindings.push_back({11,0,m_drawBuffer,nullptr,0,m_drawBuffer->GetDesc().size,{}});
            bindings.push_back({12,0,m_frameBuffer,nullptr,0,sizeof(FrameGPU),{}});
            bindings.push_back({13,0,lights,nullptr,0,sizeof(RendererLightingConstants),{}});
            bindings.push_back({14,0,nullptr,shadowTexture,0,0,{}});
            bindings.push_back({16,0,shadowBuffer,nullptr,0,sizeof(RendererShadowConstants),{}});
            bindings.push_back({17,0,nullptr,m_brdf,0,0,{}});
            bindings.push_back({19,0,lighting.clusterLights.buffer,nullptr,0,lighting.clusterLights.buffer->GetDesc().size,{}});
            bindings.push_back({20,0,lighting.clusterList.buffer,nullptr,0,lighting.clusterList.buffer->GetDesc().size,{}});
            bindings.push_back({21,0,lighting.clusterConstants.buffer,nullptr,0,sizeof(RendererClusterConstants),{}});
            bindings.push_back({26,0,nullptr,lighting.areaMatrix.texture ? lighting.areaMatrix.texture : background,0,0,{}});
            bindings.push_back({27,0,nullptr,lighting.areaAmplitude.texture ? lighting.areaAmplitude.texture : background,0,0,{}});
            bindings.push_back({28,0,nullptr,nearest,0,0,{}});
            bindings.push_back({22,0,m_probeData,nullptr,0,m_probeData->GetDesc().size,{}});
            bindings.push_back({23,0,nullptr,m_probePrefilter,0,0,{}});
            bindings.push_back({24,0,m_probeParameters,nullptr,0,16,{}});
            auto* set=Resources(m_pipelines[m_data[i].pipelineIndex],bindings);
            if(!set) return false;
            sets.push_back(set);
            if(noAreaReady)
            {
                try
                {
                    auto* specialized=Resources(m_noAreaPipelines[m_data[i].pipelineIndex],bindings);
                    if(specialized) noAreaSets.push_back(specialized);
                    else noAreaReady=false;
                }
                catch(...) {noAreaReady=false;}
                if(!noAreaReady) noAreaSets.clear();
            }
        }
        pass.SetExecute([this,target,depth,operation,clearReactive,drawGeometry,sets=std::move(sets),
            noAreaSets=std::move(noAreaSets),noAreaReady,renderer=&renderer,lights](ICommandList* commands) {
            // All extension hooks and the final graph writer/barrier checks have
            // completed before this callback records either shading layer.
            const bool noArea=noAreaReady && ProfilerAccess::GetCurrentNoAreaProof(*renderer,lights);
            if(noArea && !m_noAreaReported.exchange(true,std::memory_order_relaxed))
                std::fprintf(stderr,"[AdvancedRendering] Transparent no-area specialization selected: final owned-light proof, matched PSO/resource sets.\n");
            ColorAttachment colors[2];colors[0].texture=target;colors[0].loadOp=LoadOp::Load;colors[0].storeOp=StoreOp::Store;
            colors[1].texture=m_reactive;colors[1].loadOp=clearReactive?LoadOp::Clear:LoadOp::Load;colors[1].storeOp=StoreOp::Store;
            DepthStencilAttachment attachment;attachment.texture=depth;attachment.state=ResourceState::DepthWrite;
            attachment.depthLoadOp=LoadOp::Load;attachment.depthStoreOp=StoreOp::Store;
            commands->BeginRendering({colors,2,&attachment});
            commands->SetViewport({0,0,float(target->GetDesc().width),float(target->GetDesc().height),0,1});
            commands->SetScissor({0,0,target->GetDesc().width,target->GetDesc().height});
            drawGeometry(commands,noArea?noAreaSets:sets,operation,false,noArea);
            commands->EndRendering();
        });
        return true;
    };
    if(!addCopy("Transparency opaque depth copy",depth,depthHandle,m_opaqueDepth,opaqueDepthHandle,true)) return false;
    if(m_layers==2)
    {
        const auto rearHandle=graph.ImportTexture("Transparent rear HDR",m_rearColor,m_rearState,ResourceState::ShaderResource);
        const auto nearestHandle=graph.ImportTexture("Transparent nearest depth",m_nearestDepth,m_nearestState,ResourceState::ShaderResource);
        if(!rearHandle.IsValid() || !nearestHandle.IsValid()) return false;
        auto& nearest=graph.AddPass("Transparency nearest depth",RGRecordingMode::Independent);
        nearest.Write(nearestHandle,ResourceState::DepthWrite)
            .Read(opaqueDepthHandle,ResourceState::ShaderResource);
        geometryReads(nearest);
        std::vector<ResourceSetHandle> sets;
        sets.reserve(m_draws.size());
        std::unordered_set<TextureHandle> textures;
        for(uint32_t i=0;i<m_draws.size();++i)
        {
            const auto texture=m_draws[i].textures[0];
            if(textures.insert(texture).second)
                nearest.Read(graph.ImportTexture("Transparent depth alpha "+std::to_string(i),texture,
                    ResourceState::ShaderResource,ResourceState::ShaderResource),ResourceState::ShaderResource);
            const std::vector<ResourceBinding> bindings={{0,0,nullptr,texture,0,0,{}},
                {6,0,nullptr,m_opaqueDepth,0,0,{}},
                {11,0,m_drawBuffer,nullptr,0,m_drawBuffer->GetDesc().size,{}},
                {12,0,m_frameBuffer,nullptr,0,sizeof(FrameGPU),{}}};
            const auto pipeline=m_depthPipelines[m_data[i].pipelineIndex];
            auto* set=Resources(pipeline,bindings);if(!set) return false;
            sets.push_back(set);
        }
        nearest.SetExecute([this,drawGeometry,sets=std::move(sets)](ICommandList* commands) {
            DepthStencilAttachment depth;depth.texture=m_nearestDepth;
            depth.state=ResourceState::DepthWrite;
            depth.depthLoadOp=LoadOp::Clear;depth.depthStoreOp=StoreOp::Store;depth.clearDepth=1;
            commands->BeginRendering({nullptr,0,&depth});
            commands->SetViewport({0,0,float(m_nearestDepth->GetDesc().width),float(m_nearestDepth->GetDesc().height),0,1});
            commands->SetScissor({0,0,m_nearestDepth->GetDesc().width,m_nearestDepth->GetDesc().height});
            drawGeometry(commands,sets,0,true,false);
            commands->EndRendering();
        });
        // 뒤층은 원본 불투명 색을 읽고 앞층만 뒤층 합성을 굴절 배경으로 사용한다.
        if(!addCopy("Transparency rear background",opaque,opaqueHandle,m_rearColor,rearHandle) ||
            !addShading("Transparency rear layer",opaque,opaqueHandle,m_rearColor,rearHandle,
                m_nearestDepth,nearestHandle,1,true) ||
            !addCopy("Transparency front background",m_rearColor,rearHandle,m_color,m_colorHandle) ||
            !addShading("Transparency front layer",m_rearColor,rearHandle,m_color,m_colorHandle,
                m_nearestDepth,nearestHandle,2,false)) return false;
        m_layeredRecorded=true;
    }
    else if(!addCopy("Transparency opaque background",opaque,opaqueHandle,m_color,m_colorHandle) ||
        !addShading("Transparent Fresnel reflection refraction",opaque,opaqueHandle,m_color,m_colorHandle,
            m_fallbackDepth,fallbackHandle,0,true)) return false;
    m_recorded=true;return true;
}
}
