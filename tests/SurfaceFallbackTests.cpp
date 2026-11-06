// Returned-failure probes through the current renderer/Surface code, without a native GPU.
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#define private public
#include "dyf/Renderer.h"
#undef private
#include "dyf/RendererInternal.h"
#include "dyf/Camera.h"
#include "dyf/Scene.h"
#include "dyf/Extends/AdvancedRendering.h"
#include "dyf/RHI.h"
#include "Backends/Null/NullDevice.h"

namespace
{
using namespace dyf;
namespace R=dyf::RHI;
void Check(bool value,const char* message) {if(!value)throw std::runtime_error(message);}
enum class Failure {None,MainPipeline,OwnerLayout,OwnerPipeline,OwnerTexture,PackedTexture,SurfaceBuffer};
bool HasOwnerBinding(const R::PipelineLayoutDesc& layout)
{
    for(uint32_t i=0;i<layout.bindingCount;++i)if(layout.bindings[i].binding==25)return true;
    return false;
}
struct Device final:Backends::NullDevice
{
    Failure failure=Failure::None;
    unsigned cacheAttempts=0,ownerAttempts=0,textureFailures=0,bufferFailures=0;
    mutable unsigned layoutFailures=0;
    std::unordered_set<R::PipelineHandle> cachePipelines,ownerPipelines;
    Device() {Check(Initialize(nullptr,{})==0,"Null initialization failed");}
    bool CreateSwapchainNative(const R::SwapchainDesc& input) override
    {auto desc=input;desc.initialWidth=desc.initialHeight=16;return NullDevice::CreateSwapchainNative(desc);}
    bool SupportsNative(R::Feature feature) const override
    {return feature==R::Feature::FragmentPrimitiveId || feature==R::Feature::DescriptorIndexing || NullDevice::SupportsNative(feature);}
    bool SupportsPipelineLayoutNative(const R::PipelineLayoutDesc& layout) const override
    {
        if(failure==Failure::OwnerLayout && HasOwnerBinding(layout)) {++layoutFailures;return false;}
        return NullDevice::SupportsPipelineLayoutNative(layout);
    }
    R::PipelineHandle CreateGraphicsPipelineNative(const R::GraphicsPipelineDesc& desc) override
    {
        const bool owner=desc.colorAttachmentCount==4 && desc.colorAttachments[3].format==R::Format::R32G32_UINT;
        bool cache=false;
        for(uint32_t i=0;i<desc.vertexAttributeCount;++i)
            cache|=desc.vertexAttributes[i].location==9 && desc.vertexAttributes[i].format==R::Format::R32G32_UINT;
        if(cache && ++cacheAttempts==3 && failure==Failure::MainPipeline)return nullptr;
        if(owner && ++ownerAttempts==3 && failure==Failure::OwnerPipeline)return nullptr;
        auto* pipeline=NullDevice::CreateGraphicsPipelineNative(desc);
        if(cache && pipeline)cachePipelines.insert(pipeline);
        if(owner && pipeline)ownerPipelines.insert(pipeline);
        return pipeline;
    }
    void DestroyPipelineNative(R::PipelineHandle pipeline) override
    {
        cachePipelines.erase(pipeline);ownerPipelines.erase(pipeline);
        NullDevice::DestroyPipelineNative(pipeline);
    }
    R::TextureHandle CreateTextureNative(const R::TextureDesc& desc) override
    {
        if((failure==Failure::OwnerTexture && desc.format==R::Format::R32G32_UINT) ||
           (failure==Failure::PackedTexture && desc.format==R::Format::R32_UINT))
        {++textureFailures;return nullptr;}
        return NullDevice::CreateTextureNative(desc);
    }
    R::BufferHandle CreateBufferNative(const R::BufferDesc& desc) override
    {
        // Frame constants are allocated after the white AO texture upload was recorded.
        if(failure==Failure::SurfaceBuffer && desc.usage==R::BufferUsage::Constant)
        {++bufferFailures;return nullptr;}
        return NullDevice::CreateBufferNative(desc);
    }
};
RendererConfig Config(bool bindless=false)
{
    RendererConfig config;config.enableProfilerHud=false;config.texturePreparationThreadCount=1;
    config.enableHdrRendering=true;config.advanced.enabled=true;config.enableInstancing=true;
    config.advanced.reuseSurfaceMaterials=true;
    config.enableBindlessMaterials=bindless;config.lighting.shadows=false;
    config.advanced.ambientOcclusion=config.advanced.diffuseGI=config.advanced.reflections=false;
    config.advanced.temporalAA=config.advanced.bloom=false;
    return config;
}
void MainFallback(Failure failure,bool bindless)
{
    Device device;device.failure=failure;
    auto renderer=Renderer::Create(device,reinterpret_cast<void*>(1),Config(bindless));
    Check(bool(renderer),"Main fixture renderer creation failed");
    auto& impl=*renderer->m_impl;impl.stockDrawInputs=true;
    Check(impl.InitializeMesh(device.GetBackBuffer()),"Optional Main cache failure must retain the base pipeline");
    const bool cache=failure==Failure::None;
    Check(bool(impl.surfaceCachePipelines[0])==cache,"Unexpected Main cache pipeline selection");
    Check(HasOwnerBinding(impl.pipeline->GetLayout())==(failure!=Failure::OwnerLayout),
        "Owner layout refusal must use the base layout without descriptor 25");
    if(failure==Failure::MainPipeline)
    {
        Check(device.cacheAttempts==3,"Main failure did not exercise partially created cache pipelines");
        Check(device.cachePipelines.empty(),"Partial Main cache pipelines leaked");
        for(auto* pipeline:impl.surfaceCachePipelines)Check(!pipeline,"Partial Main cache pipeline survived fallback");
    }
    if(failure==Failure::OwnerLayout)Check(device.layoutFailures>0,"Owner layout refusal was not exercised");
    Check(impl.UsesBindlessMaterials()==bindless,"Optional owner layout changed the base material paging choice");
    renderer.reset();Check(device.WaitIdle(),"Main fixture drain failed");
    Check(device.cachePipelines.empty(),"Main cache pipeline cleanup failed");
}
void SurfaceFallback(Failure failure)
{
    Device device;device.failure=failure;
    auto renderer=Renderer::Create(device,reinterpret_cast<void*>(1),Config());
    Check(bool(renderer),"Surface fixture renderer creation failed");
    R::ResourceScope resources(device);
    R::TextureDesc desc;desc.width=desc.height=16;desc.format=R::Format::D32_FLOAT;
    desc.usage=R::TextureUsage::DepthStencil|R::TextureUsage::ShaderResource;
    auto* depth=resources.Keep(device.CreateTexture(desc));
    auto* keys=resources.Keep(device.CreateBuffer({8,8,R::BufferUsage::Vertex,R::ResourceState::VertexBuffer}));
    Check(depth && keys,"Surface fixture resources failed");
    Scene scene;Camera camera;
    MeshData triangle;triangle.vertices.resize(3);triangle.indices={0,1,2};
    Check(bool(scene.Add(triangle,{})),"Surface owner fixture needs one entity");
    auto surface=CreateAdvancedRenderingModule(AdvancedRenderModule::SurfaceLighting);
    Check(bool(surface),"Surface module creation failed");
    const auto prepare=[&](bool expectedOwner,bool packed=false) {
        RenderFrame frame{*renderer,scene,camera};
        if(packed)frame.requirements=surface->GetRequirements(*renderer);
        frame.requirements.needsSurface=true;
        frame.depth.texture=depth;frame.gpuVisibility=true;frame.visibleInstanceKeys.buffer=keys;
        auto* commands=device.AcquireCommandList();Check(commands!=nullptr,"Surface upload list failed");
        const bool prepared=surface->Prepare(frame,*commands);
        const bool closed=commands->Close();device.DestroyCommandList(commands);
        if(failure==Failure::SurfaceBuffer)
        {
            Check(!prepared && closed && device.bufferFailures==1 && device.ownerAttempts==8,
                "Post-upload buffer failure must propagate without replaying Surface preparation");
            return;
        }
        Check(prepared && closed,"Optional Surface owner failure must retain ordinary Surface preparation");
        R::RenderGraph graph;
        frame.depth.resource=graph.ImportTexture("depth",depth,R::ResourceState::Undefined,R::ResourceState::DepthWrite);
        frame.visibleInstanceKeys.resource=graph.ImportBuffer("keys",keys,R::ResourceState::VertexBuffer,R::ResourceState::VertexBuffer);
        Check(surface->AddPasses(RenderStage::BeforeOpaque,frame,graph),"Fallback Surface graph creation failed");
        Check(frame.normal.texture && frame.albedo.texture && frame.motion.texture && frame.opaqueDepthComplete,
            "Fallback lost required Surface/depth outputs");
        Check(bool(frame.materialOwner.texture)==expectedOwner && frame.surfaceMaterialComplete==expectedOwner,
            "Surface owner proof does not match the successful resource path");
        if(expectedOwner)
            Check(frame.materialOwner.texture->GetDesc().format==(packed && device.failure!=Failure::PackedTexture?R::Format::R32_UINT:R::Format::R32G32_UINT),
                "Surface owner must use R32 for compact consumers and UINT2 for legacy consumers");
    };
    prepare(failure==Failure::None);
    if(failure==Failure::None) {
        prepare(true,true);prepare(true);
        device.failure=Failure::PackedTexture;prepare(true,true);
        Check(device.textureFailures==1,"Failed compact allocation must fall back to the full owner pair");
        device.failure=Failure::None;prepare(true,true);
    }
    if(failure==Failure::OwnerPipeline)Check(device.ownerAttempts==3,"Surface pipeline failure was not injected after partial creation");
    if(failure==Failure::OwnerTexture)Check(device.textureFailures==1,"Owner texture failure was not injected");
    if(failure==Failure::OwnerPipeline || failure==Failure::OwnerTexture)
    {
        Check(device.ownerPipelines.empty(),"Failed Surface owner pipelines were not released");
        device.failure=Failure::None;prepare(true);
    }
    surface.reset();Check(device.WaitIdle(),"Surface fixture drain failed");
    Check(device.ownerPipelines.empty(),"Surface owner pipeline cleanup failed");
}
}
int main() try
{
    for(bool bindless:{false,true})for(auto failure:{Failure::None,Failure::MainPipeline,Failure::OwnerLayout})MainFallback(failure,bindless);
    for(auto failure:{Failure::None,Failure::OwnerPipeline,Failure::OwnerTexture,Failure::SurfaceBuffer})SurfaceFallback(failure);
    std::puts("PASS Surface fallback: partial Main/Surface pipelines, owner texture refusal, descriptor 25 refusal, cleanup and retry (Null CPU probe).");
    return 0;
}
catch(const std::exception& error) {std::fprintf(stderr,"FAIL Surface fallback: %s\n",error.what());return 1;}
