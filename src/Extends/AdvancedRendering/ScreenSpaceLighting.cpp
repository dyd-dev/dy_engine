#include "ScreenSpaceLighting.h"
#include "SceneAlgorithms.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <string>
#include <unordered_set>
#if defined(ENABLE_METAL) || defined(ENABLE_VULKAN) || defined(ENABLE_D3D12)
#include "ScreenDenoiseTemporal.h"
#include "ScreenDenoiseSpatial.h"
#endif

#if defined(ENABLE_METAL)
#include "ScreenSpaceMetalLibrary.h"
#elif defined(ENABLE_VULKAN) || defined(ENABLE_D3D12)
#include "SurfaceVertex.h"
#include "SurfaceFragment.h"
#include "ScreenSpaceVertex.h"
#include "ScreenSpaceFragment.h"
#include "ScreenSpaceLightingFragment.h"
#include "SurfaceDebugFragment.h"
#endif

namespace dyf
{
using namespace RHI;

ScreenSpaceLighting::~ScreenSpaceLighting() { Reset(); }

void ScreenSpaceLighting::Invalidate()
{
    m_historyValid=false;m_draws.clear();
    for(auto& effect:m_effectHistory) {effect.valid=false;effect.pending=false;}
}

void ScreenSpaceLighting::ReleaseDenoise(uint32_t effect)
{
    auto& history=m_effectHistory[effect];
    if(m_device)
    {
        DestroySets();
        for(auto& slot:history.frames) for(auto& target:slot) m_device->DestroyTexture(target.image);
        for(auto& target:history.spatial) m_device->DestroyTexture(target.image);
    }
    history={};
}

bool ScreenSpaceLighting::CreateDenoisePipelines()
{
    if(m_denoisePipelines[0] && m_denoisePipelines[1]) return true;
#if defined(ENABLE_METAL) || defined(ENABLE_VULKAN) || defined(ENABLE_D3D12)
#if defined(ENABLE_METAL)
    const char* entries[]={"screenDenoiseTemporal","screenDenoiseSpatial"};
#else
    const char* entries[]={"main","main"};
#endif
    const ShaderDesc shaders[]={{ShaderStage::Fragment,entries[0],kScreenDenoiseTemporal,kScreenDenoiseTemporalSize},
        {ShaderStage::Fragment,entries[1],kScreenDenoiseSpatial,kScreenDenoiseSpatialSize}};
    for(uint32_t i=0;i<2;++i)
        if(!m_denoiseShaders[i] && !(m_denoiseShaders[i]=m_device->CreateShader(shaders[i]))) return false;
#else
    return false;
#endif
    SamplerDesc sampler;
    sampler.minFilter=sampler.magFilter=SamplerFilter::Nearest;sampler.mipFilter=SamplerFilter::Nearest;
    sampler.addressU=sampler.addressV=sampler.addressW=SamplerAddressMode::ClampToEdge;
    sampler.mipLodBias=sampler.minLod=sampler.maxLod=0;
    std::vector<ResourceBindingLayout> bindings;
    for(uint32_t i=0;i<6;++i) bindings.push_back({i,ResourceBindingType::SampledTexture,1,ShaderStageFlags::Fragment,{}});
    bindings.push_back({7,ResourceBindingType::StaticSampler,1,ShaderStageFlags::Fragment,sampler});
    bindings.push_back({12,ResourceBindingType::ConstantBuffer,1,ShaderStageFlags::Fragment,{}});
    const ColorAttachmentDesc colors[]={{Format::R16G16B16A16_FLOAT,{},ColorWriteMask::All},
        {Format::R16G16B16A16_FLOAT,{},ColorWriteMask::All},{Format::R32G32B32A32_FLOAT,{},ColorWriteMask::All}};
    for(uint32_t i=0;i<2;++i)
    {
        if(m_denoisePipelines[i]) continue;
        GraphicsPipelineDesc desc;desc.vertexShader=m_shaders[2];desc.fragmentShader=m_denoiseShaders[i];
        desc.topology=PrimitiveTopology::TriangleList;
        desc.raster={FillMode::Solid,CullMode::None,FrontFace::CounterClockwise,0,0,0};
        desc.colorAttachments=colors;desc.colorAttachmentCount=i?1:3;
        desc.layout={bindings.data(),static_cast<uint32_t>(bindings.size()),16,ShaderStageFlags::Fragment,15};
        if(!(m_denoisePipelines[i]=m_device->CreateGraphicsPipeline(desc))) return false;
    }
    return true;
}

bool ScreenSpaceLighting::PrepareDenoise(uint32_t effect,bool enabled)
{
    auto& history=m_effectHistory[effect];history.pending=false;
    if(!enabled) {if(history.frames[0][0].image) ReleaseDenoise(effect);return true;}
    if(!CreateDenoisePipelines()) return false;
    const auto create=[&](DenoiseTarget& target,Format format) {
        if(target.image) return true;
        TextureDesc desc;desc.width=(m_width+1)/2;desc.height=(m_height+1)/2;desc.format=format;
        desc.usage=TextureUsage::RenderTarget|TextureUsage::ShaderResource;
        target.image=m_device->CreateTexture(desc);target.state=ResourceState::Undefined;
        return target.image!=nullptr;
    };
    for(auto& slot:history.frames) for(uint32_t channel=0;channel<3;++channel)
        if(!create(slot[channel],channel==2?Format::R32G32B32A32_FLOAT:Format::R16G16B16A16_FLOAT)) return false;
    for(auto& target:history.spatial) if(!create(target,Format::R16G16B16A16_FLOAT)) return false;
    return true;
}

ResourceSetHandle ScreenSpaceLighting::DenoiseResources(PipelineHandle pipeline,const std::array<TextureHandle,6>& images)
{
    for(auto& cached:m_denoiseSets)
        if(cached.pipeline==pipeline && cached.images==images) {cached.unusedFrames=0;return cached.set;}
    std::vector<ResourceBinding> bindings;
    for(uint32_t i=0;i<images.size();++i) bindings.push_back({i,0,nullptr,images[i],0,0,{}});
    bindings.push_back({12,0,m_frameBuffer,nullptr,0,sizeof(FrameGPU),{}});
    auto* set=m_device->CreateResourceSet({pipeline,bindings.data(),static_cast<uint32_t>(bindings.size())});
    if(set) m_denoiseSets.push_back({images,pipeline,set,0});
    return set;
}

bool ScreenSpaceLighting::AddDenoise(RenderGraph& graph,uint32_t effect,TextureHandle raw,RGResourceHandle rawGraph,
    TextureHandle& filtered,RGResourceHandle& filteredGraph)
{
    auto& history=m_effectHistory[effect];
    const std::string name=effect?"SSR":"SSGI";
    auto& current=history.frames[history.write];auto& previous=history.frames[1-history.write];
    std::array<RGResourceHandle,3> output,old;
    for(uint32_t i=0;i<3;++i)
    {
        output[i]=graph.ImportTexture(name+" history write "+std::to_string(i),current[i].image,current[i].state,ResourceState::ShaderResource);
        if(!output[i].IsValid()) return false;
        if(history.valid)
        {
            old[i]=graph.ImportTexture(name+" history read "+std::to_string(i),previous[i].image,previous[i].state,ResourceState::ShaderResource);
            if(!old[i].IsValid()) return false;
        }
    }
    const std::array<TextureHandle,6> temporalImages={m_textures[Normal],m_textures[Motion],raw,
        history.valid?previous[0].image:raw,history.valid?previous[1].image:raw,history.valid?previous[2].image:raw};
    auto* set=DenoiseResources(m_denoisePipelines[0],temporalImages);
    if(!set) return false;
    auto& temporal=graph.AddPass(name+" temporal reprojection moments", RHI::RGRecordingMode::Independent);
    temporal.Read(m_handles[Normal],ResourceState::ShaderResource).Read(m_handles[Motion],ResourceState::ShaderResource)
        .Read(rawGraph,ResourceState::ShaderResource).Read(m_frameHandle,ResourceState::ConstantBuffer)
        .SetPipeline(m_denoisePipelines[0]);
    for(uint32_t i=0;i<3;++i)
    {
        temporal.Write(output[i],ResourceState::RenderTarget);
        if(history.valid) temporal.Read(old[i],ResourceState::ShaderResource);
    }
    const std::array<TextureHandle,3> destinations={current[0].image,current[1].image,current[2].image};
    const std::array<float,4> operation={0,float(effect),history.valid?1.f:0.f,0};
    temporal.SetExecute([this,set,destinations,operation](ICommandList* commands) {
        ColorAttachment colors[3];
        for(uint32_t i=0;i<3;++i) {colors[i].texture=destinations[i];colors[i].loadOp=LoadOp::Discard;colors[i].storeOp=StoreOp::Store;}
        commands->BeginRendering({colors,3,nullptr});commands->BindGraphicsPipeline(m_denoisePipelines[0]);commands->BindResourceSet(set);
        const auto& desc=destinations[0]->GetDesc();
        commands->SetViewport({0,0,float(desc.width),float(desc.height),0,1});commands->SetScissor({0,0,desc.width,desc.height});
        commands->SetInlineConstants(0,sizeof(operation),operation.data());commands->DrawInstanced(3,1,0,0);commands->EndRendering();
    });
    auto source=current[0].image;auto sourceGraph=output[0];
    for(uint32_t stage=0;stage<2;++stage)
    {
        auto& target=history.spatial[stage];
        const auto destination=graph.ImportTexture(name+" spatial "+std::to_string(stage),target.image,target.state,ResourceState::ShaderResource);
        const std::array<TextureHandle,6> images={m_textures[Normal],m_textures[Motion],source,source,current[1].image,current[2].image};
        auto* resources=DenoiseResources(m_denoisePipelines[1],images);
        if(!resources || !destination.IsValid()) return false;
        auto& spatial=graph.AddPass(name+" variance spatial filter "+std::to_string(stage), RHI::RGRecordingMode::Independent);
        spatial.Read(sourceGraph,ResourceState::ShaderResource).Read(output[1],ResourceState::ShaderResource)
            .Read(output[2],ResourceState::ShaderResource).Read(m_handles[Normal],ResourceState::ShaderResource)
            .Read(m_handles[Motion],ResourceState::ShaderResource).Read(m_frameHandle,ResourceState::ConstantBuffer)
            .Write(destination,ResourceState::RenderTarget).SetPipeline(m_denoisePipelines[1]);
        spatial.SetExecute([this,resources,image=target.image,effect,stage](ICommandList* commands) {
            DrawScreen(*commands,m_denoisePipelines[1],resources,image,{1,float(effect),0,float(1u<<stage)});
        });
        source=target.image;sourceGraph=destination;
    }
    filtered=source;filteredGraph=sourceGraph;history.pending=true;
    return true;
}

void ScreenSpaceLighting::DestroySets()
{
    if(!m_device) return;
    for(const auto& cached:m_surfaceSets) m_device->DestroyResourceSet(cached.second);
    for(const auto& cached:m_screenSets) m_device->DestroyResourceSet(cached.set);
    for(const auto& cached:m_denoiseSets) m_device->DestroyResourceSet(cached.set);
    m_denoiseSets.clear();
    m_surfaceSets.clear();m_screenSets.clear();m_drawSets.clear();
}

void ScreenSpaceLighting::Reset()
{
    if(m_device)
    {
        DestroySets();
        for(uint32_t effect=0;effect<2;++effect) ReleaseDenoise(effect);
        for(auto* image:m_textures) m_device->DestroyTexture(image);
        m_device->DestroyTexture(m_whiteAO);
        for(auto* pipeline:m_surfacePipelines) m_device->DestroyPipeline(pipeline);
        m_device->DestroyPipeline(m_screenPipeline);m_device->DestroyPipeline(m_lightingPipeline);
        m_device->DestroyPipeline(m_debugPipeline);
        for(auto* shader:m_shaders) m_device->DestroyShader(shader);
        for(auto* shader:m_denoiseShaders) m_device->DestroyShader(shader);
        for(auto* pipeline:m_denoisePipelines) m_device->DestroyPipeline(pipeline);
        m_device->DestroyBuffer(m_frameBuffer);m_device->DestroyBuffer(m_drawBuffer);
    }
    m_textures={};m_states={};m_handles={};m_imported={};m_shaders={};m_surfacePipelines={};
    m_screenPipeline=m_lightingPipeline=nullptr;m_frameBuffer=m_drawBuffer=nullptr;
    m_debugPipeline=nullptr;m_debugFormat=Format::Unknown;
    m_depth=m_whiteAO=m_colorOutput=nullptr;m_device=nullptr;m_width=m_height=0;
    m_frameReady=m_drawReady=m_historyValid=m_prepared=false;m_draws.clear();m_graph={};
    m_gpuDraws.clear();m_gpuScratch.clear();
    m_effectHistory={};m_denoiseShaders={};m_denoisePipelines={};
}

bool ScreenSpaceLighting::CreatePipelines()
{
    if(m_surfacePipelines[0] && m_surfacePipelines[1] && m_surfacePipelines[2] && m_surfacePipelines[3] &&
        m_screenPipeline && m_lightingPipeline) return true;
    std::array<ShaderDesc,6> shaders={};
#if defined(ENABLE_METAL)
    const char* entries[]={"surfaceVertex","surfaceFragment","screenSpaceVertex",
        "screenSpaceFragment","screenSpaceLightingFragment","surfaceDebugFragment"};
    for(uint32_t i=0;i<shaders.size();++i)
        shaders[i]={i==0 || i==2?ShaderStage::Vertex:ShaderStage::Fragment,
            entries[i],kScreenSpaceMetalLibrary,kScreenSpaceMetalLibrarySize};
#elif defined(ENABLE_VULKAN) || defined(ENABLE_D3D12)
    shaders={{{ShaderStage::Vertex,"main",kSurfaceVertex,kSurfaceVertexSize},
        {ShaderStage::Fragment,"main",kSurfaceFragment,kSurfaceFragmentSize},
        {ShaderStage::Vertex,"main",kScreenSpaceVertex,kScreenSpaceVertexSize},
        {ShaderStage::Fragment,"main",kScreenSpaceFragment,kScreenSpaceFragmentSize},
        {ShaderStage::Fragment,"main",kScreenSpaceLightingFragment,kScreenSpaceLightingFragmentSize},
        {ShaderStage::Fragment,"main",kSurfaceDebugFragment,kSurfaceDebugFragmentSize}}};
#else
    return false;
#endif
    for(uint32_t i=0;i<m_shaders.size();++i)
        if(!m_shaders[i] && !(m_shaders[i]=m_device->CreateShader(shaders[i]))) return false;
    SamplerDesc sampler;
    sampler.minFilter=sampler.magFilter=SamplerFilter::Linear;
    sampler.mipFilter=SamplerFilter::Linear;
    sampler.addressU=sampler.addressV=sampler.addressW=SamplerAddressMode::Repeat;
    sampler.mipLodBias=sampler.minLod=0;sampler.maxLod=32;
    sampler.maxAnisotropy=static_cast<uint32_t>(std::clamp<uint64_t>(m_device->GetLimit(Limit::SamplerAnisotropy),1,8));
    std::vector<ResourceBindingLayout> surfaceBindings;
    for(uint32_t i=0;i<ToIndex(MaterialTextureKind::Count);++i)
        surfaceBindings.push_back({i,ResourceBindingType::SampledTexture,1,ShaderStageFlags::Fragment,{}});
    surfaceBindings.push_back({8,ResourceBindingType::StaticSampler,1,ShaderStageFlags::Fragment,sampler});
    surfaceBindings.push_back({11,ResourceBindingType::ReadOnlyStorageBuffer,1,
        ShaderStageFlags::Vertex|ShaderStageFlags::Fragment,{}});
    surfaceBindings.push_back({12,ResourceBindingType::ConstantBuffer,1,
        ShaderStageFlags::Vertex|ShaderStageFlags::Fragment,{}});
    const VertexBufferLayout vertices[]={{0,48,VertexStepMode::Vertex},{1,48,VertexStepMode::Vertex},
        {2,128,VertexStepMode::Instance}};
    const VertexAttribute attributes[]={{0,0,Format::R32G32B32_FLOAT,0},
        {1,0,Format::R32G32B32_FLOAT,12},{2,0,Format::R32G32_FLOAT,24},
        {3,0,Format::R32G32B32A32_FLOAT,32},{4,1,Format::R32G32B32_FLOAT,0},
        {5,2,Format::R32G32B32A32_FLOAT,0},{6,2,Format::R32G32B32A32_FLOAT,16},
        {7,2,Format::R32G32B32A32_FLOAT,32},{8,2,Format::R32G32B32A32_FLOAT,48},
        {9,2,Format::R32G32B32A32_FLOAT,64},{10,2,Format::R32G32B32A32_FLOAT,80},
        {11,2,Format::R32G32B32A32_FLOAT,96},{12,2,Format::R32G32B32A32_FLOAT,112}};
    const ColorAttachmentDesc colors[]={{Format::R16G16B16A16_FLOAT,{},ColorWriteMask::All},
        {Format::R16G16B16A16_FLOAT,{},ColorWriteMask::All},
        {Format::R16G16B16A16_FLOAT,{},ColorWriteMask::All}};
    GraphicsPipelineDesc desc;
    desc.vertexShader=m_shaders[0];desc.fragmentShader=m_shaders[1];
    desc.topology=PrimitiveTopology::TriangleList;desc.vertexBuffers=vertices;desc.vertexBufferCount=3;
    desc.vertexAttributes=attributes;desc.vertexAttributeCount=13;
    desc.raster={FillMode::Solid,CullMode::Back,FrontFace::CounterClockwise,0,0,0};
    desc.depthStencil.format=Format::D32_FLOAT;desc.depthStencil.depthTestEnabled=true;
    desc.depthStencil.depthWriteEnabled=true;desc.depthStencil.depthCompareOp=CompareOp::Less;
    desc.colorAttachments=colors;desc.colorAttachmentCount=3;
    desc.layout={surfaceBindings.data(),static_cast<uint32_t>(surfaceBindings.size()),16,
        ShaderStageFlags::Vertex|ShaderStageFlags::Fragment,15};
    for(uint32_t i=0;i<m_surfacePipelines.size();++i)
    {
        desc.raster.cullMode=(i&1u)?CullMode::None:CullMode::Back;
        desc.raster.frontFace=(i&2u)?FrontFace::Clockwise:FrontFace::CounterClockwise;
        if(!m_surfacePipelines[i] && !(m_surfacePipelines[i]=m_device->CreateGraphicsPipeline(desc))) return false;
    }
    std::vector<ResourceBindingLayout> screenBindings;
    sampler.mipFilter=SamplerFilter::Nearest;sampler.maxLod=0;sampler.maxAnisotropy=1;
    sampler.addressU=sampler.addressV=sampler.addressW=SamplerAddressMode::ClampToEdge;
    for(uint32_t i=0;i<7;++i)
        screenBindings.push_back({i,ResourceBindingType::SampledTexture,1,ShaderStageFlags::Fragment,{}});
    screenBindings.push_back({7,ResourceBindingType::StaticSampler,1,ShaderStageFlags::Fragment,sampler});
    screenBindings.push_back({12,ResourceBindingType::ConstantBuffer,1,ShaderStageFlags::Fragment,{}});
    desc={};desc.vertexShader=m_shaders[2];desc.fragmentShader=m_shaders[3];
    desc.topology=PrimitiveTopology::TriangleList;
    desc.raster={FillMode::Solid,CullMode::None,FrontFace::CounterClockwise,0,0,0};
    desc.colorAttachments=colors;desc.colorAttachmentCount=1;
    desc.layout={screenBindings.data(),static_cast<uint32_t>(screenBindings.size()),16,ShaderStageFlags::Fragment,15};
    if(!m_screenPipeline && !(m_screenPipeline=m_device->CreateGraphicsPipeline(desc))) return false;
    desc.fragmentShader=m_shaders[4];
    if(!m_lightingPipeline && !(m_lightingPipeline=m_device->CreateGraphicsPipeline(desc))) return false;
    return true;
}

bool ScreenSpaceLighting::Upload(ICommandList& commands,BufferHandle& buffer,bool& ready,
    const void* data,uint32_t bytes,uint32_t stride,BufferUsage usage,ResourceState state,uint32_t capacityBytes)
{
    const uint32_t allocationBytes=std::max(bytes,capacityBytes);
    if(!buffer || buffer->GetDesc().size<allocationBytes)
    {
        DestroySets();m_device->DestroyBuffer(buffer);
        buffer=m_device->CreateBuffer({allocationBytes,stride,usage,ResourceState::CopyDestination});ready=false;
    }
    if(!buffer) return false;
    if(ready)
    {
        const ResourceBarrierDesc write{buffer,nullptr,state,ResourceState::CopyDestination,{}};
        commands.ResourceBarrier(&write,1);
    }
    if(!m_device->UpdateBuffer(commands,buffer,0,data,bytes)) return false;
    const ResourceBarrierDesc read{buffer,nullptr,ResourceState::CopyDestination,state,{}};
    commands.ResourceBarrier(&read,1);ready=true;
    return true;
}

bool ScreenSpaceLighting::Prepare(IDevice& device,ICommandList& uploads,TextureHandle depth,
    const Camera& camera,const std::vector<SurfaceDraw>& draws,const Settings& settings)
{
    const auto fail=[this]() {Invalidate();return false;};
    m_prepared=false;
    if(m_device && m_device!=&device) Reset();
    m_device=&device;
    if(!depth || depth->GetDesc().format!=Format::D32_FLOAT) return fail();
    const uint32_t width=depth->GetDesc().width,height=depth->GetDesc().height;
    if(!width || !height || !std::isfinite(settings.aoRadius+settings.rayDistance+settings.thickness) ||
        settings.aoRadius<=0 || settings.rayDistance<=0 || settings.thickness<=0) return fail();
    if(draws.size()>UINT32_MAX/sizeof(DrawGPU)) return fail();
    if(!CreatePipelines()) return fail();
    for(auto& cached:m_screenSets)
        if(cached.unusedFrames<UINT32_MAX) ++cached.unusedFrames;
    for(auto& cached:m_denoiseSets)
        if(cached.unusedFrames<UINT32_MAX) ++cached.unusedFrames;
    if(settings.diffuseGI!=m_settings.diffuseGI || settings.reflections!=m_settings.reflections ||
        settings.ambientOcclusion!=m_settings.ambientOcclusion || settings.aoRadius!=m_settings.aoRadius ||
        settings.aoStrength!=m_settings.aoStrength || settings.rayDistance!=m_settings.rayDistance ||
        settings.giStrength!=m_settings.giStrength || settings.reflectionStrength!=m_settings.reflectionStrength ||
        settings.thickness!=m_settings.thickness) Invalidate();
    m_drawSets.clear();
    if(width!=m_width || height!=m_height)
    {
        DestroySets();
        for(uint32_t i=0;i<ImageCount;++i)
        {
            device.DestroyTexture(m_textures[i]);m_textures[i]=nullptr;m_states[i]=ResourceState::Undefined;
        }
        for(uint32_t effect=0;effect<2;++effect) ReleaseDenoise(effect);
        m_width=width;m_height=height;Invalidate();
    }
    for(uint32_t i=0;i<ImageCount;++i)
    {
        const bool enabled=i<=Motion ||
            (i>=AORaw && i<=AO && settings.ambientOcclusion) ||
            (i==GI && settings.diffuseGI) || (i==Reflection && settings.reflections) ||
            (i==Color && (settings.diffuseGI || settings.reflections));
        if(!enabled)
        {
            if(i>=GI && m_textures[i])
            {
                DestroySets();device.DestroyTexture(m_textures[i]);m_textures[i]=nullptr;m_states[i]=ResourceState::Undefined;
            }
            continue;
        }
        if(m_textures[i]) continue;
        TextureDesc desc;
        const bool full=i<=Motion || i==Color;
        desc.width=full?width:(width+1)/2;desc.height=full?height:(height+1)/2;
        desc.format=Format::R16G16B16A16_FLOAT;
        desc.usage=TextureUsage::RenderTarget|TextureUsage::ShaderResource;
        if(!(m_textures[i]=device.CreateTexture(desc))) return fail();
    }
    if(!PrepareDenoise(0,settings.diffuseGI) || !PrepareDenoise(1,settings.reflections)) return fail();
    if(!settings.ambientOcclusion && !m_whiteAO)
    {
        TextureDesc desc;
        desc.width=desc.height=1;desc.format=Format::R8G8B8A8_UNORM;desc.usage=TextureUsage::ShaderResource;
        if(!(m_whiteAO=device.CreateTexture(desc))) return fail();
        const ResourceBarrierDesc writable{nullptr,m_whiteAO,ResourceState::Undefined,ResourceState::CopyDestination,{}};
        uploads.ResourceBarrier(&writable,1);
        const uint8_t white[]={255,255,255,255};
        if(!device.UpdateTexture(uploads,m_whiteAO,0,0,white,sizeof(white),4,4)) return fail();
        const ResourceBarrierDesc readable{nullptr,m_whiteAO,ResourceState::CopyDestination,ResourceState::ShaderResource,{}};
        uploads.ResourceBarrier(&readable,1);
    }
    m_colorOutput=m_textures[Color];
    m_depth=depth;m_settings=settings;m_draws.clear();
    m_gpuScratch.clear();m_gpuScratch.reserve(draws.size());
    m_draws.reserve(draws.size());m_drawSets.reserve(draws.size());
    const CameraFrustum frustum(camera);
    for(const auto& draw:draws)
    {
        if(!draw.material) return fail();
        const auto& material=*draw.material;
        if(material.alphaMode==MaterialAlphaMode::Blend || !draw.indexCount) continue;
        // Instance providers already cull each row; representative bounds cannot reject the whole batch.
        if(settings.frustumCulling && !draw.instanceBuffer && !draw.indirectArguments && frustum.Outside(draw.localBounds,draw.model)) continue;
        if(!draw.vertexBuffer || !draw.indexBuffer) return fail();
        for(auto* texture:draw.textures) if(!texture) return fail();
        DrawGPU gpu;
        gpu.model=draw.model;
        gpu.previousMVP=m_historyValid?draw.previousMVP:camera.projection*camera.view*draw.model;
        const size_t drawIndex=m_gpuScratch.size();
        if(drawIndex<m_gpuDraws.size() &&
            std::memcmp(&m_gpuDraws[drawIndex].model,&draw.model,sizeof(draw.model))==0)
            gpu.normalMatrix=m_gpuDraws[drawIndex].normalMatrix;
        else if(!Math::InverseTranspose(draw.model,gpu.normalMatrix)) gpu.normalMatrix=Math::float4x4::Identity();
        gpu.baseColor=draw.baseColor;
        gpu.materialParams={material.metallicFactor,material.roughnessFactor,
            material.normalScale,material.occlusionStrength};
        gpu.flags=draw.textureFlags;gpu.alphaMode=static_cast<uint32_t>(material.alphaMode);
        gpu.doubleSided=material.doubleSided?1:0;
        gpu.pipelineIndex=gpu.doubleSided|(Math::Determinant3x3(draw.model)<0?2u:0u);
        gpu.alpha={material.alphaCutoff,material.normalYSign,material.unlit?1.f:0.f,0};
        m_gpuScratch.push_back(gpu);m_draws.push_back(draw);
    }
    if(m_gpuScratch.empty()) m_gpuScratch.push_back({});
    if(m_gpuScratch.size()>UINT32_MAX/sizeof(DrawGPU)) return fail();
    FrameGPU frame;
    frame.view=camera.view;frame.projection=camera.projection;
    frame.viewProjection=camera.projection*camera.view;
    if(!Math::Inverse(camera.projection,frame.inverseProjection)) return fail();
    frame.size={float(width),float(height),1.f/width,1.f/height};
    frame.ao={settings.aoRadius,settings.aoStrength,0.025f,float(settings.frameIndex%1024)};
    frame.lighting={settings.rayDistance,settings.giStrength,settings.reflectionStrength,settings.thickness};
    frame.options={settings.ambientOcclusion?1.f:0.f,settings.diffuseGI?1.f:0.f,
        settings.reflections?1.f:0.f,m_historyValid?1.f:0.f};
    if(!m_frameReady || std::memcmp(&frame,&m_uploadedFrame,sizeof(frame))!=0)
    {
        if(!Upload(uploads,m_frameBuffer,m_frameReady,&frame,sizeof(frame),0,
            BufferUsage::Constant,ResourceState::ConstantBuffer)) return fail();
        m_uploadedFrame=frame;
    }
    const uint32_t drawBytes=static_cast<uint32_t>(m_gpuScratch.size()*sizeof(DrawGPU));
    const uint32_t drawCapacity=static_cast<uint32_t>(std::max<size_t>(1,draws.size())*sizeof(DrawGPU));
    const bool sameDraws=m_drawReady && m_drawBuffer->GetDesc().size>=drawCapacity && m_gpuScratch.size()==m_gpuDraws.size() &&
        std::memcmp(m_gpuScratch.data(),m_gpuDraws.data(),drawBytes)==0;
    if(!sameDraws && !Upload(uploads,m_drawBuffer,m_drawReady,m_gpuScratch.data(),drawBytes,
        sizeof(DrawGPU),BufferUsage::Storage,ResourceState::ShaderResource,drawCapacity)) return fail();
    m_gpuDraws.swap(m_gpuScratch);
    for(size_t i=0;i<m_draws.size();++i)
    {
        auto* set=SurfaceResources(m_draws[i],m_surfacePipelines[m_gpuDraws[i].pipelineIndex]);
        if(!set) return fail();
        m_drawSets.push_back(set);
    }
    m_prepared=true;return true;
}

ResourceSetHandle ScreenSpaceLighting::SurfaceResources(const SurfaceDraw& draw,PipelineHandle pipeline)
{
    const SurfaceSetKey key{draw.textures,pipeline};
    const auto cached=m_surfaceSets.find(key);
    if(cached!=m_surfaceSets.end()) return cached->second;
    std::vector<ResourceBinding> bindings;
    for(uint32_t i=0;i<draw.textures.size();++i) bindings.push_back({i,0,nullptr,draw.textures[i],0,0,{}});
    bindings.push_back({11,0,m_drawBuffer,nullptr,0,m_drawBuffer->GetDesc().size,{}});
    bindings.push_back({12,0,m_frameBuffer,nullptr,0,sizeof(FrameGPU),{}});
    auto* set=m_device->CreateResourceSet({pipeline,bindings.data(),static_cast<uint32_t>(bindings.size())});
    if(set)
    {
        try {m_surfaceSets.emplace(key,set);}
        catch(...) {m_device->DestroyResourceSet(set);throw;}
    }
    return set;
}

ResourceSetHandle ScreenSpaceLighting::ScreenResources(PipelineHandle pipeline,TextureHandle source,
    TextureHandle hdr,TextureHandle environment,TextureHandle reflection)
{
    const auto fallback=m_textures[Albedo];
    const std::array<TextureHandle,7> images={m_textures[Normal],m_textures[Motion],m_textures[Albedo],
        hdr?hdr:fallback,environment?environment:fallback,source?source:fallback,reflection?reflection:fallback};
    for(auto& cached:m_screenSets)
        if(cached.pipeline==pipeline && cached.textures==images)
        {cached.unusedFrames=0;return cached.set;}
    std::vector<ResourceBinding> bindings;
    for(uint32_t i=0;i<7;++i) bindings.push_back({i,0,nullptr,images[i],0,0,{}});
    bindings.push_back({12,0,m_frameBuffer,nullptr,0,sizeof(FrameGPU),{}});
    auto* set=m_device->CreateResourceSet({pipeline,bindings.data(),static_cast<uint32_t>(bindings.size())});
    if(set) m_screenSets.push_back({images,pipeline,set,0});
    return set;
}

bool ScreenSpaceLighting::Import(RenderGraph& graph)
{
    if(!m_prepared) return false;
    m_handles={};m_imported={};
    const char* names[]={"Surface normal roughness AO","Surface albedo metal","Surface motion depth",
        "AO raw","AO horizontal","AO filtered","Screen diffuse GI","Screen reflection","Screen lit HDR"};
    for(uint32_t i=0;i<ImageCount;++i)
    {
        const bool enabled=i<=Motion ||
            (i>=AORaw && i<=AO && m_settings.ambientOcclusion) ||
            (i==GI && m_settings.diffuseGI) || (i==Reflection && m_settings.reflections) ||
            (i==Color && (m_settings.diffuseGI || m_settings.reflections));
        if(!enabled) continue;
        m_handles[i]=graph.ImportTexture(names[i],m_textures[i],m_states[i],ResourceState::ShaderResource);
        if(!m_handles[i].IsValid()) return false;
        m_imported[i]=true;
    }
    if(!m_settings.ambientOcclusion)
    {
        m_handles[AO]=graph.ImportTexture("AO disabled white",m_whiteAO,
            ResourceState::ShaderResource,ResourceState::ShaderResource);
        if(!m_handles[AO].IsValid()) return false;
    }
    m_frameHandle=graph.ImportBuffer("Screen camera",m_frameBuffer,ResourceState::ConstantBuffer,ResourceState::ConstantBuffer);
    m_drawHandle=graph.ImportBuffer("Surface draws",m_drawBuffer,ResourceState::ShaderResource,ResourceState::ShaderResource);
    m_graph={m_handles[Normal],m_handles[Albedo],m_handles[Motion],m_handles[AO],m_handles[Color]};
    return m_frameHandle.IsValid() && m_drawHandle.IsValid();
}

bool ScreenSpaceLighting::AddSurface(RenderGraph& graph,RGResourceHandle depth,
    RGResourceHandle instances,RGResourceHandle arguments)
{
    if(!depth.IsValid() || !m_prepared) return false;
    bool usesInstances=false,usesArguments=false;
    for(const auto& draw:m_draws)
    {
        if((draw.instanceBuffer && !instances.IsValid()) || (draw.indirectArguments && !arguments.IsValid()))
        {Invalidate();return false;}
        usesInstances|=draw.instanceBuffer!=nullptr;usesArguments|=draw.indirectArguments!=nullptr;
    }
    m_depthHandle=depth;
    auto& pass=graph.AddPass("Surface normal albedo motion depth", RHI::RGRecordingMode::Independent);
    for(uint32_t i=Normal;i<=Motion;++i) pass.Write(m_handles[i],ResourceState::RenderTarget);
    pass.Write(depth,ResourceState::DepthWrite).Read(m_frameHandle,ResourceState::ConstantBuffer)
        .Read(m_drawHandle,ResourceState::ShaderResource);
    if(usesInstances) pass.Read(instances,ResourceState::VertexBuffer);
    if(usesArguments) pass.Read(arguments,ResourceState::IndirectArgument);
    std::unordered_set<BufferHandle> vertexImports,indexImports;
    std::unordered_set<TextureHandle> textureImports;
    vertexImports.reserve(m_draws.size());indexImports.reserve(m_draws.size());
    for(uint32_t i=0;i<m_draws.size();++i)
    {
        const auto& draw=m_draws[i];
        if(vertexImports.insert(draw.vertexBuffer).second)
            pass.Read(graph.ImportBuffer("Surface VB "+std::to_string(i),draw.vertexBuffer,
                ResourceState::VertexBuffer,ResourceState::VertexBuffer),ResourceState::VertexBuffer);
        if(draw.previousVertexBuffer && vertexImports.insert(draw.previousVertexBuffer).second)
            pass.Read(graph.ImportBuffer("Surface previous VB "+std::to_string(i),draw.previousVertexBuffer,
                ResourceState::VertexBuffer,ResourceState::VertexBuffer),ResourceState::VertexBuffer);
        if(indexImports.insert(draw.indexBuffer).second)
            pass.Read(graph.ImportBuffer("Surface IB "+std::to_string(i),draw.indexBuffer,
                ResourceState::IndexBuffer,ResourceState::IndexBuffer),ResourceState::IndexBuffer);
        for(uint32_t t=0;t<draw.textures.size();++t)
            if(textureImports.insert(draw.textures[t]).second)
                pass.Read(graph.ImportTexture("Surface texture "+std::to_string(i)+" "+std::to_string(t),draw.textures[t],
                    ResourceState::ShaderResource,ResourceState::ShaderResource),ResourceState::ShaderResource);
    }
    pass.SetExecute([this](ICommandList* commands) {
        ColorAttachment colors[3];
        for(uint32_t i=0;i<3;++i)
        {colors[i].texture=m_textures[i];colors[i].loadOp=LoadOp::Clear;colors[i].storeOp=StoreOp::Store;}
        DepthStencilAttachment depth;
        depth.texture=m_depth;depth.state=ResourceState::DepthWrite;
        depth.depthLoadOp=LoadOp::Clear;depth.depthStoreOp=StoreOp::Store;depth.clearDepth=1;
        commands->BeginRendering({colors,3,&depth});
        commands->SetViewport({0,0,float(m_width),float(m_height),0,1});
        commands->SetScissor({0,0,m_width,m_height});
        PipelineHandle boundPipeline=nullptr;
        ResourceSetHandle boundSet=nullptr;
        for(uint32_t i=0;i<m_draws.size();++i)
        {
            const auto& draw=m_draws[i];
            auto* pipeline=m_surfacePipelines[m_gpuDraws[i].pipelineIndex];
            if(pipeline!=boundPipeline)
            {commands->BindGraphicsPipeline(pipeline);boundPipeline=pipeline;boundSet=nullptr;}
            if(m_drawSets[i]!=boundSet)
            {commands->BindResourceSet(m_drawSets[i]);boundSet=m_drawSets[i];}
            commands->BindVertexBuffer(0,draw.vertexBuffer,0);
            commands->BindVertexBuffer(1,draw.previousVertexBuffer?draw.previousVertexBuffer:draw.vertexBuffer,0);
            commands->BindVertexBuffer(2,draw.instanceBuffer?draw.instanceBuffer:draw.vertexBuffer,draw.instanceOffset);
            commands->BindIndexBuffer(draw.indexBuffer,Format::R32_UINT,0);
            const uint32_t index[4]={i,draw.instanceBuffer?1u:0u,draw.deformationHistoryValid?1u:0u,0};
            commands->SetInlineConstants(0,sizeof(index),index);
            if(draw.indirectArguments) commands->DrawIndexedIndirect(draw.indirectArguments,draw.indirectOffset);
            else commands->DrawIndexedInstanced(draw.indexCount,draw.instanceCount,0,0,0);
        }
        commands->EndRendering();
    });
    return true;
}

void ScreenSpaceLighting::DrawScreen(ICommandList& commands,PipelineHandle pipeline,ResourceSetHandle set,
    TextureHandle output,const std::array<float,4>& values)
{
    ColorAttachment color;
    color.texture=output;color.loadOp=LoadOp::Discard;color.storeOp=StoreOp::Store;
    commands.BeginRendering({&color,1,nullptr});commands.BindGraphicsPipeline(pipeline);
    commands.BindResourceSet(set);
    commands.SetViewport({0,0,float(output->GetDesc().width),float(output->GetDesc().height),0,1});
    commands.SetScissor({0,0,output->GetDesc().width,output->GetDesc().height});
    commands.SetInlineConstants(0,16,values.data());commands.DrawInstanced(3,1,0,0);commands.EndRendering();
}

bool ScreenSpaceLighting::AddAO(RenderGraph& graph)
{
    if(!m_prepared) return false;
    if(!m_settings.ambientOcclusion) return true;
    const ImageIndex sources[]={Albedo,AORaw,AOHorizontal},outputs[]={AORaw,AOHorizontal,AO};
    const char* names[]={"View-space hemisphere AO","AO bilateral horizontal","AO bilateral vertical"};
    for(uint32_t stage=0;stage<3;++stage)
    {
        auto* set=ScreenResources(m_screenPipeline,m_textures[sources[stage]],nullptr,nullptr,nullptr);
        if(!set) return false;
        const std::array<float,4> constants={stage?1.f:0.f,
            stage==1?1.f/m_textures[AO]->GetDesc().width:0.f,
            stage==2?1.f/m_textures[AO]->GetDesc().height:0.f,float(m_settings.aoFrameIndex%1024)};
        auto& pass=graph.AddPass(names[stage], RHI::RGRecordingMode::Independent);
        pass.Read(m_handles[Normal],ResourceState::ShaderResource).Read(m_handles[Motion],ResourceState::ShaderResource)
            .Read(m_handles[Albedo],ResourceState::ShaderResource).Read(m_handles[sources[stage]],ResourceState::ShaderResource)
            .Read(m_frameHandle,ResourceState::ConstantBuffer).Write(m_handles[outputs[stage]],ResourceState::RenderTarget)
            .SetExecute([this,set,constants,target=m_textures[outputs[stage]]](ICommandList* commands) {
                DrawScreen(*commands,m_screenPipeline,set,target,constants);
            });
    }
    return true;
}

bool ScreenSpaceLighting::AddLighting(RenderGraph& graph,RGResourceHandle hdr,TextureHandle hdrTexture,
    RGResourceHandle environment,TextureHandle environmentTexture)
{
    const auto fail=[this]() {Invalidate();return false;};
    if(!m_prepared || !hdr.IsValid() || !hdrTexture) return fail();
    if(!m_settings.diffuseGI && !m_settings.reflections)
    {
        m_colorOutput=hdrTexture;m_graph.color=hdr;
        return true;
    }
    if(m_settings.reflections && (!environment.IsValid() || !environmentTexture)) return fail();
    const auto envTexture=m_settings.reflections?environmentTexture:nullptr;
    const bool enabled[]={m_settings.diffuseGI,m_settings.reflections};
    const ImageIndex targets[]={GI,Reflection};
    const char* names[]={"Screen diffuse GI ray march","Screen specular reflection ray march"};
    std::array<TextureHandle,2> filtered={};
    std::array<RGResourceHandle,2> filteredGraph;
    for(uint32_t effect=0;effect<2;++effect)
    {
        if(!enabled[effect]) continue;
        auto* rays=ScreenResources(m_lightingPipeline,GetAO(),hdrTexture,envTexture,nullptr);
        if(!rays) return fail();
        auto& pass=graph.AddPass(names[effect], RHI::RGRecordingMode::Independent);
        pass.Read(hdr,ResourceState::ShaderResource).Read(m_handles[Normal],ResourceState::ShaderResource)
            .Read(m_handles[Motion],ResourceState::ShaderResource).Read(m_handles[Albedo],ResourceState::ShaderResource)
            .Read(m_handles[AO],ResourceState::ShaderResource).Read(m_frameHandle,ResourceState::ConstantBuffer)
            .Write(m_handles[targets[effect]],ResourceState::RenderTarget);
        if(envTexture) pass.Read(environment,ResourceState::ShaderResource);
        pass.SetExecute([this,rays,target=m_textures[targets[effect]],effect](ICommandList* commands) {
            DrawScreen(*commands,m_lightingPipeline,rays,target,{2,0,0,float(effect)});
        });
        if(!AddDenoise(graph,effect,m_textures[targets[effect]],m_handles[targets[effect]],filtered[effect],filteredGraph[effect]))
            return fail();
    }
    auto* composite=ScreenResources(m_screenPipeline,filtered[0],hdrTexture,envTexture,filtered[1]);
    if(!composite) return fail();
    auto& resolve=graph.AddPass("Screen lighting resolve", RHI::RGRecordingMode::Independent);
    resolve.Read(hdr,ResourceState::ShaderResource).Read(m_handles[Normal],ResourceState::ShaderResource)
        .Read(m_handles[Motion],ResourceState::ShaderResource).Read(m_handles[Albedo],ResourceState::ShaderResource)
        .Read(m_frameHandle,ResourceState::ConstantBuffer).Write(m_handles[Color],ResourceState::RenderTarget);
    if(m_settings.diffuseGI) resolve.Read(filteredGraph[0],ResourceState::ShaderResource);
    if(m_settings.reflections) resolve.Read(filteredGraph[1],ResourceState::ShaderResource)
        .Read(environment,ResourceState::ShaderResource);
    resolve.SetExecute([this,composite](ICommandList* commands) {
        DrawScreen(*commands,m_screenPipeline,composite,m_textures[Color],{3,0,0,0});
    });
    m_colorOutput=m_textures[Color];m_graph.color=m_handles[Color];
    return true;
}

bool ScreenSpaceLighting::AddDebugView(RenderGraph& graph,TextureHandle output,RGResourceHandle outputGraph,
    SurfaceDebugView view,float farPlane)
{
    if(view==SurfaceDebugView::None) return true;
    if(!m_prepared || !output || !outputGraph.IsValid() || !std::isfinite(farPlane) || farPlane<=0 ||
        static_cast<uint32_t>(view)>static_cast<uint32_t>(SurfaceDebugView::Motion)) return false;
    for(uint32_t i=Normal;i<=Motion;++i)
        if(!m_handles[i].IsValid() || output==m_textures[i]) return false;
    const auto format=output->GetDesc().format;
    if(!m_debugPipeline || format!=m_debugFormat)
    {
        // Resource sets belong to their exact pipeline, including its output
        // format. Leave the AO/GI/SSR descriptor caches intact on format changes.
        m_screenSets.erase(std::remove_if(m_screenSets.begin(),m_screenSets.end(),[&](const ScreenSet& cached) {
            if(cached.pipeline!=m_debugPipeline) return false;
            m_device->DestroyResourceSet(cached.set);return true;
        }),m_screenSets.end());
        m_device->DestroyPipeline(m_debugPipeline);m_debugPipeline=nullptr;
        std::vector<ResourceBindingLayout> bindings;
        for(uint32_t i=0;i<7;++i)
            bindings.push_back({i,ResourceBindingType::SampledTexture,1,ShaderStageFlags::Fragment,{}});
        SamplerDesc sampler;
        sampler.minFilter=sampler.magFilter=sampler.mipFilter=SamplerFilter::Nearest;
        sampler.addressU=sampler.addressV=sampler.addressW=SamplerAddressMode::ClampToEdge;
        sampler.mipLodBias=sampler.minLod=sampler.maxLod=0;sampler.maxAnisotropy=1;
        bindings.push_back({7,ResourceBindingType::StaticSampler,1,ShaderStageFlags::Fragment,sampler});
        bindings.push_back({12,ResourceBindingType::ConstantBuffer,1,ShaderStageFlags::Fragment,{}});
        const ColorAttachmentDesc color{format,{},ColorWriteMask::All};
        GraphicsPipelineDesc desc;
        desc.vertexShader=m_shaders[2];desc.fragmentShader=m_shaders[5];
        desc.topology=PrimitiveTopology::TriangleList;
        desc.raster={FillMode::Solid,CullMode::None,FrontFace::CounterClockwise,0,0,0};
        desc.colorAttachments=&color;desc.colorAttachmentCount=1;
        desc.layout={bindings.data(),static_cast<uint32_t>(bindings.size()),16,ShaderStageFlags::Fragment,15};
        if(!(m_debugPipeline=m_device->CreateGraphicsPipeline(desc))) return false;
        m_debugFormat=format;
    }
    auto* set=ScreenResources(m_debugPipeline,nullptr,nullptr,nullptr,nullptr);
    if(!set) return false;
    const std::array<float,4> constants={float(static_cast<uint32_t>(view)),
        IsSrgbFormat(format)?1.f:0.f,farPlane,32.f};
    auto& pass=graph.AddPass("Surface attachment debug view", RHI::RGRecordingMode::Independent);
    pass.Read(m_handles[Normal],ResourceState::ShaderResource)
        .Read(m_handles[Albedo],ResourceState::ShaderResource)
        .Read(m_handles[Motion],ResourceState::ShaderResource)
        .Read(m_frameHandle,ResourceState::ConstantBuffer)
        .Write(outputGraph,ResourceState::RenderTarget)
        .SetExecute([this,set,output,constants](ICommandList* commands) {
            DrawScreen(*commands,m_debugPipeline,set,output,constants);
        });
    return true;
}

void ScreenSpaceLighting::CommitFrame()
{
    m_draws.clear();
    for(uint32_t i=0;i<ImageCount;++i)
        if(m_imported[i]) m_states[i]=ResourceState::ShaderResource;
    m_historyValid=true;
    for(auto& effect:m_effectHistory)
    {
        if(!effect.pending) continue;
        for(auto& target:effect.frames[effect.write]) target.state=ResourceState::ShaderResource;
        for(auto& target:effect.spatial) target.state=ResourceState::ShaderResource;
        effect.write=1-effect.write;effect.valid=true;effect.pending=false;
    }
    const auto frameWindow=std::max(1u,m_device->GetDesc().maxFramesInFlight);
    m_screenSets.erase(std::remove_if(m_screenSets.begin(),m_screenSets.end(),[&](const ScreenSet& cached) {
        if(cached.unusedFrames<frameWindow) return false;
        m_device->DestroyResourceSet(cached.set);return true;
    }),m_screenSets.end());
    m_denoiseSets.erase(std::remove_if(m_denoiseSets.begin(),m_denoiseSets.end(),[&](const DenoiseSet& cached) {
        if(cached.unusedFrames<frameWindow) return false;
        m_device->DestroyResourceSet(cached.set);return true;
    }),m_denoiseSets.end());
}
}
