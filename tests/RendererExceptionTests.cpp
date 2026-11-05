// CPU-only audit probe. Fake native calls; current public RHI validation and
// current Renderer::Impl::RenderScene execute unchanged. No shader/GPU execution.
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <functional>
#include <exception>
#include <iostream>
#include <limits>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
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
#include "dyf/RHI.h"
#undef private
#include "dyf/RendererInternal.h"
#include "dyf/Scene.h"
#include "dyf/Camera.h"
#include "Backends/Null/NullDevice.h"

namespace R = dyf::RHI;
namespace {
bool cleanupArmed=false,cleanupEntered=false;
void Check(bool value,const char* message)
{if(!value)throw std::runtime_error(message);}

struct Commands final : R::ICommandList
{
    bool hasDraw=false;
    ~Commands() override=default;
    void ResourceBarrierNative(const R::ResourceBarrierDesc*,uint32_t) override {}
    void GlobalBarrierNative() override {}
    void BeginRenderingNative(const R::RenderingDesc&) override {}
    void EndRenderingNative() override {}
    void BindGraphicsPipelineNative(R::PipelineHandle) override {}
    void BindComputePipelineNative(R::PipelineHandle) override {}
    void DispatchNative(uint32_t,uint32_t,uint32_t) override {}
    void BindResourceSetNative(R::ResourceSetHandle) override {}
    void BindVertexBufferNative(uint32_t,R::BufferHandle,uint32_t) override {}
    void BindIndexBufferNative(R::BufferHandle,R::Format,uint32_t) override {}
    void SetInlineConstantsNative(uint32_t,uint32_t,const void*) override {}
    void SetViewportNative(const R::Viewport&) override {}
    void SetScissorNative(const R::Rect&) override {}
    void SetStencilReferenceNative(uint32_t) override {}
    void DrawInstancedNative(uint32_t,uint32_t,uint32_t,uint32_t) override {hasDraw=true;}
    void DrawIndexedInstancedNative(uint32_t,uint32_t,uint32_t,int32_t,uint32_t) override {hasDraw=true;}
    void ResetTimestampsNative(R::TimestampQueryHandle,uint32_t,uint32_t) override {}
    void WriteTimestampNative(R::TimestampQueryHandle,uint32_t) override {}
    bool CloseNative() override {return true;}
};

struct Device final : dyf::Backends::NullDevice
{
    uint64_t serial=0,drawBatches=0,presentCalls=0;
    Device(){Check(Initialize(nullptr,{})==0,"Null initialization failed");}
    ~Device() override {ReleaseResources();}
    bool SupportsNative(R::Feature feature) const override
    {
        if(feature==R::Feature::TimestampQuery || feature==R::Feature::DescriptorIndexing ||
            feature==R::Feature::Compute || feature==R::Feature::IndirectDraw)return false;
        return NullDevice::SupportsNative(feature);
    }
    bool CreateSwapchainNative(const R::SwapchainDesc& desc) override
    {auto local=desc;local.initialWidth=local.initialHeight=16;return NullDevice::CreateSwapchainNative(local);}
    R::ICommandList* AcquireCommandListNative() override {return new Commands;}
    void DiscardCommandListNative(R::ICommandList* commands) override {delete static_cast<Commands*>(commands);}
    bool UpdateBufferNative(R::ICommandList&,R::BufferHandle,uint32_t,const void*,uint32_t) override {return true;}
    bool UpdateTextureNative(R::ICommandList&,R::TextureHandle,uint32_t,uint32_t,const void*,uint32_t,uint32_t,uint32_t) override {return true;}
    bool SubmitNative(R::ICommandList** lists,uint32_t count) override
    {
        bool draw=false;
        for(uint32_t i=0;i<count;++i)draw|=static_cast<Commands*>(lists[i])->hasDraw;
        ++serial;if(draw)++drawBatches;
        for(uint32_t i=0;i<count;++i)DiscardCommandListNative(lists[i]);
        return true;
    }
    uint64_t GetCompletedSubmissionNative() override {return serial;}
    uint64_t GetLastSubmissionNative() const override {return serial;}
    bool PresentNative() override {++presentCalls;return NullDevice::PresentNative();}
    R::ResourceState Tracked(R::TextureHandle texture) const
    {
        Check(texture!=nullptr,"Missing tracked texture");
        const auto key=std::make_tuple(reinterpret_cast<uintptr_t>(texture),0u,0u);
        const auto found=m_resourceStates.find(key);
        Check(found!=m_resourceStates.end(),"Texture not tracked by public RHI");
        return found->second;
    }
};

struct CommitProbe final : dyf::RenderExtension
{
    Device& device;
    bool throwNext,cleanupThrow;
    unsigned commits=0,invalidations=0,strongInvalidations=0;
    CommitProbe(Device& owner,bool fail,bool cleanup=false):device(owner),throwNext(fail),cleanupThrow(cleanup) {}
    const char* GetName() const override {return "Audit one-shot post-submit CommitFrame exception";}
    void CommitFrame(const dyf::RenderFrame&) override
    {
        ++commits;
        Check(device.drawBatches>0,"Commit callback ran before successful native draw submit");
        if(std::exchange(throwNext,false))
        {
            // Arm only after the accepted draw has reached CommitFrame. Initial
            // registration/Prepare invalidations must not trigger this probe.
            if(cleanupThrow)cleanupArmed=true;
            throw std::runtime_error("AUDIT injected CommitFrame after accepted draw");
        }
    }
    void InvalidateHistory(bool release=false) override
    {
        ++invalidations;if(release)++strongInvalidations;
        if(cleanupThrow&&release&&cleanupArmed)
        {
            cleanupEntered=true;
            throw std::runtime_error("AUDIT injected InvalidateHistory during failed-frame cleanup");
        }
    }
};

struct CleanupObserver final : dyf::RenderExtension
{
    unsigned releases=0;
    void InvalidateHistory(bool release=false) override {if(release)++releases;}
};

void Run(bool window,bool inject,bool cleanupThrow=false,bool shadowsEnabled=false)
{
    Device device;
    dyf::RendererConfig config;
    config.enableProfilerHud=false;
    config.enableHdrRendering=true;
    config.enableParallelRenderGraph=true;
    config.recordingThreadCount=2;
    config.lighting.shadows=shadowsEnabled;
    config.lighting.shadowQuality=dyf::ShadowQuality::Low;
    auto renderer=dyf::Renderer::Create(device,reinterpret_cast<void*>(1),config);
    Check(bool(renderer),"Renderer creation failed");
    auto& impl=*renderer->m_impl;
    static const uint8_t shader[]={1};
    impl.vertexShader=device.CreateShader({R::ShaderStage::Vertex,"main",shader,1});
    impl.fragmentShader=device.CreateShader({R::ShaderStage::Fragment,"main",shader,1});
    impl.toneVertexShader=device.CreateShader({R::ShaderStage::Vertex,"main",shader,1});
    impl.toneFragmentShader=device.CreateShader({R::ShaderStage::Fragment,"main",shader,1});
    if(shadowsEnabled)
    {
        impl.shadowVertexShader=device.CreateShader({R::ShaderStage::Vertex,"main",shader,1});
        impl.shadowFragmentShader=device.CreateShader({R::ShaderStage::Fragment,"main",shader,1});
    }
    Check(impl.vertexShader&&impl.fragmentShader&&impl.toneVertexShader&&impl.toneFragmentShader,"Fake shader setup failed");
    auto extension=std::make_unique<CommitProbe>(device,inject,cleanupThrow);
    auto* probe=extension.get();
    Check(renderer->AddExtension(std::move(extension)),"Extension registration failed");
    auto observer=std::make_unique<CleanupObserver>();auto* cleanup=observer.get();
    Check(renderer->AddExtension(std::move(observer)),"Cleanup observer registration failed");
    const auto releases=cleanup->releases;
    dyf::MeshData mesh;
    mesh.vertices.resize(3);mesh.indices={0,1,2};
    mesh.vertices[0].position={-.2f,-.2f,-2};
    mesh.vertices[1].position={.2f,-.2f,-2};
    mesh.vertices[2].position={0,.2f,-2};
    dyf::Scene scene;Check(bool(scene.Add(mesh)),"Scene setup failed");
    if(shadowsEnabled){dyf::DirectionalLight light;light.castShadow=true;Check(bool(scene.Add(light)),"Shadow light setup failed");}
    dyf::Camera camera;
    auto* target=device.CreateTexture({16,16,1,1,R::Format::R8G8B8A8_UNORM,R::TextureUsage::RenderTarget|R::TextureUsage::ShaderResource});
    Check(target!=nullptr,"RTT target creation failed");
    const auto render=[&](R::ResourceState before) {
        return impl.RenderScene(scene,&camera,nullptr,nullptr,nullptr,window?nullptr:target,
            window?R::ResourceState::Present:before);
    };
    const bool first=render(R::ResourceState::Undefined);
    Check(device.drawBatches==1 && probe->commits==1,"First frame never reached post-submit callback");
    const auto hdrActual=device.Tracked(impl.hdrTarget);
    const auto depthActual=device.Tracked(impl.depthStencilTarget);
    const bool staleHdr=impl.hdrState!=hdrActual;
    const bool staleDepth=impl.depthStencilState!=depthActual;
    const bool staleShadow=shadowsEnabled && (!impl.shadowDepthTarget || impl.shadowDepthState!=device.Tracked(impl.shadowDepthTarget));
    std::printf("CASE window=%d inject=%d first=%d drawBatches=%llu completion=%d hdr_cached=%u hdr_actual=%u depth_cached=%u depth_actual=%u invalidations=%u strong=%u presents=%llu\n",
        window,inject,first,static_cast<unsigned long long>(device.drawBatches),bool(renderer->GetLastRenderCompletion()),
        unsigned(impl.hdrState),unsigned(hdrActual),unsigned(impl.depthStencilState),unsigned(depthActual),
        probe->invalidations,probe->strongInvalidations,static_cast<unsigned long long>(device.presentCalls));
    Check(first==!inject,"Unexpected first-frame return");
    Check(bool(renderer->GetLastRenderCompletion()),"Accepted frame completion was not preserved");
    if(window)Check(device.presentCalls>=1 && !impl.framePending,"FrameEnd failed to close accepted window frame");
    const bool retry=render(device.Tracked(target));
    Check(!staleHdr&&!staleDepth&&!staleShadow,"Submitted resource boundary cache is stale");
    Check(retry&&device.drawBatches==2&&probe->commits==2,"Retry after callback exception failed");
    if(cleanupThrow)Check(cleanupEntered && cleanup->releases>releases,"Throwing cleanup prevented later module cleanup");
    device.DestroyTexture(target);
}
}

int main()
{
    try
    {
        Run(false,false);Run(false,true);Run(true,true);Run(false,true,true);Run(false,true,false,true);
        std::puts("PASS renderer callback exception recovery (CPU fake native).");
        return 0;
    }
    catch(const std::exception& error)
    {std::fprintf(stderr,"Renderer exception test failed: %s\n",error.what());return 1;}
}
