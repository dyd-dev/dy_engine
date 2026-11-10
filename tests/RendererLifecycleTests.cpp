// CPU-only audit: reuse the existing fake commands, current public RHI and
// Renderer implementation. The included entry point is not called.
#define main CommitProbeEntryUnused
#include "RendererExceptionTests.cpp"
#undef main
#include "dyf/Canvas.h"

namespace {
struct LifecycleDevice final : dyf::Backends::NullDevice
{
    uint64_t serial=0, pipelineCalls=0, presentCalls=0;
    unsigned pipelineFailureCountdown=0;
    bool failPresent=false,failBackbuffer=false;
    LifecycleDevice(){Check(Initialize(nullptr,{})==0,"Lifecycle Null init failed");}
    ~LifecycleDevice() override {ReleaseResources();}
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
    bool SubmitNative(R::ICommandList** lists,uint32_t count) override
    {++serial;for(uint32_t i=0;i<count;++i)DiscardCommandListNative(lists[i]);return true;}
    uint64_t GetCompletedSubmissionNative() override {return serial;}
    uint64_t GetLastSubmissionNative() const override {return serial;}
    bool PresentNative() override {++presentCalls;const bool presented=NullDevice::PresentNative();return presented && !std::exchange(failPresent,false);}
    R::TextureHandle GetBackBufferNative() override {return std::exchange(failBackbuffer,false)?nullptr:NullDevice::GetBackBufferNative();}
    bool UpdateBufferNative(R::ICommandList&,R::BufferHandle,uint32_t,const void*,uint32_t) override {return true;}
    bool UpdateTextureNative(R::ICommandList&,R::TextureHandle,uint32_t,uint32_t,const void*,uint32_t,uint32_t,uint32_t) override {return true;}
    R::PipelineHandle CreateGraphicsPipelineNative(const R::GraphicsPipelineDesc& desc) override
    {
        ++pipelineCalls;
        if(pipelineFailureCountdown && --pipelineFailureCountdown==0)return nullptr;
        return NullDevice::CreateGraphicsPipelineNative(desc);
    }
};

void Transition(LifecycleDevice& device,R::TextureHandle texture,R::ResourceState before,R::ResourceState after)
{
    auto* commands=device.AcquireCommandList();Check(commands!=nullptr,"Acquire failed");
    const R::ResourceBarrierDesc barrier{nullptr,texture,before,after,{}};
    commands->ResourceBarrier(&barrier,1);
    Check(commands->Close(),"Close failed");
    Check(device.Submit(&commands,1),"Barrier submit failed");
    // Submit consumes the fake native list, not this public recorded list.
    // Release its retained backbuffer reference before swapchain replacement.
    device.DestroyCommandList(commands);
}

void TerminalPresentationProbe(bool frameEnd)
{
    LifecycleDevice device;
    dyf::RendererConfig config;config.enableProfilerHud=false;
    auto renderer=dyf::Renderer::Create(device,reinterpret_cast<void*>(1),config);
    Check(bool(renderer) && device.BeginFrame(),"Terminal Present setup failed");
    auto& state=*renderer->m_impl;
    state.framePending=true;device.failPresent=true;
    if(frameEnd){dyf::Renderer::Impl::FrameEnd end{device,state.framePending};}
    else Check(!state.ApplySettings(),"Native Present failure was hidden");
    Check(!state.framePending && device.presentCalls==1,"Terminal Present failure retained frame ownership");
    Check(renderer->SetVSync(!config.vsync) && state.ApplySettings(),"Terminal Present failure prevented output recreation");
}

void WsiRecoveryProbe(bool frameEnd)
{
    LifecycleDevice device;
    dyf::RendererConfig config;config.enableProfilerHud=false;
    auto renderer=dyf::Renderer::Create(device,reinterpret_cast<void*>(1),config);
    Check(bool(renderer),"Renderer create failed");auto& state=*renderer->m_impl;
    Check(device.BeginFrame(),"Begin failed");
    auto* backbuffer=device.GetBackBuffer();
    Transition(device,backbuffer,R::ResourceState::Present,R::ResourceState::RenderTarget);
    state.framePending=true;
    bool recovered=false;
    if(frameEnd){dyf::Renderer::Impl::FrameEnd end{device,state.framePending};}
    else recovered=state.ApplySettings();
    Check(state.framePending,"Retryable Present rejection lost frame ownership");
    Check(!frameEnd ? !recovered : true,"ApplySettings ignored Present failure");
    Check(device.presentCalls==0,"Present reached native despite invalid boundary state");
    // Current public RHI explicitly retains an active frame on this rejection.
    // A valid same-thread RHI transition induced this state; this is an error
    // recovery contract probe, not a normal RenderGraph occurrence.
    Check(device.BeginFrame() && device.GetBackBuffer()==backbuffer,"Expected same active frame");
    Check(renderer->SetVSync(!config.vsync),"Queue output change failed");
    Check(!state.ApplySettings(),"Expected output recreation to reject active frame");
    Transition(device,backbuffer,R::ResourceState::RenderTarget,R::ResourceState::Present);
    Check(state.ApplySettings(),"Owned frame recovery failed to restore output change");
    Check(!state.framePending && device.presentCalls==1,"Recovered frame ownership was not released");
    std::printf("PASS frame recovery=%s: retryable rejection retains ownership until successful recovery.\n",frameEnd?"FrameEnd":"ApplySettings");
}

void PipelineRetryProbe()
{
    LifecycleDevice device;
    dyf::RendererConfig config;config.enableProfilerHud=false;
    auto renderer=dyf::Renderer::Create(device,reinterpret_cast<void*>(1),config);
    Check(bool(renderer),"Renderer create failed");auto& state=*renderer->m_impl;
    static const uint8_t code[]={1};
    state.vertexShader=device.CreateShader({R::ShaderStage::Vertex,"main",code,1});
    state.fragmentShader=device.CreateShader({R::ShaderStage::Fragment,"main",code,1});
    Check(state.vertexShader&&state.fragmentShader,"Fake shaders failed");
    // Avoid unrelated default texture upload. This owned handle is released by
    // unchanged Renderer::Impl::Shutdown; no GPU operation occurs.
    state.defaultMaterialTextures[0]=device.CreateTexture({1,1,1,1,R::Format::R8G8B8A8_UNORM,R::TextureUsage::ShaderResource});
    Check(state.defaultMaterialTextures[0]!=nullptr,"Default texture failed");
    auto* a=device.CreateTexture({16,16,1,1,R::Format::R8G8B8A8_UNORM,R::TextureUsage::RenderTarget|R::TextureUsage::ShaderResource});
    auto* b=device.CreateTexture({16,16,1,1,R::Format::B8G8R8A8_UNORM,R::TextureUsage::RenderTarget|R::TextureUsage::ShaderResource});
    Check(a&&b,"Target creation failed");
    Check(state.InitializeMesh(a,true),"Initial A pipeline set failed");
    Check(state.pipeline && state.pipeline->m_colorFormats.at(0)==a->GetDesc().format,"A control wrong pipeline format");
    // Base B succeeds; first mandatory stock-material variant fails.
    device.pipelineFailureCountdown=2;
    Check(!state.InitializeMesh(b,true),"Injected B variant failure absent");
    Check(state.meshColorFormat==R::Format::Unknown,"Partial pipeline bundle retained a reusable cache key");
    const auto calls=device.pipelineCalls;
    Check(state.InitializeMesh(a,true),"A retry did not take cached success branch");
    Check(device.pipelineCalls>calls && state.meshColorFormat==a->GetDesc().format &&
        state.pipeline->m_colorFormats.at(0)==a->GetDesc().format,"A retry reused the partial B pipeline bundle");
    std::puts("PASS pipeline retry rebuilds a complete bundle after a failed format change.");
    // Retrying B instead invalidates the old A key and restores a full bundle.
    Check(state.InitializeMesh(b,true),"Same-B retry control failed");
    Check(state.InitializeMesh(a,true) && state.pipeline->m_colorFormats.at(0)==a->GetDesc().format,"Full-rebuild recovery control failed");
    device.DestroyTexture(a);device.DestroyTexture(b);
}

void MissingBackbufferProbe(bool fixed,bool canvas)
{
    LifecycleDevice device;dyf::RendererConfig config;config.enableProfilerHud=false;
    if(fixed)config.renderWidth=config.renderHeight=16;
    auto renderer=dyf::Renderer::Create(device,reinterpret_cast<void*>(1),config);
    Check(bool(renderer),"Missing output fixture");
    dyf::Scene scene;dyf::Camera camera;dyf::Canvas overlay(16,16);
    const auto render=[&] {return canvas?renderer->Render(overlay):renderer->Render(scene,camera);};
    Check(render(),"Missing output control frame");
    const auto serial=device.serial;device.failBackbuffer=true;
    Check(!render(),"Missing output was accepted");
    Check(device.serial==serial && !device.HasActiveFrame(),"Missing output submitted work or retained acquired frame");
    Check(render(),"Missing output retry failed");
    renderer.reset();Check(device.WaitIdle(),"Missing output teardown");
    const auto counters=device.GetResourceAllocationCounters();
    Check(!counters.buffers.live && !counters.textures.live && !counters.pipelines.live,"Missing output leaked resources");
}
}

int main()
{
    try
    {
        WsiRecoveryProbe(true);WsiRecoveryProbe(false);
        TerminalPresentationProbe(true);TerminalPresentationProbe(false);PipelineRetryProbe();
        MissingBackbufferProbe(false,false);MissingBackbufferProbe(true,false);MissingBackbufferProbe(false,true);
        std::puts("PASS renderer lifecycle regression tests: CPU fake native only.");
        return 0;
    }
    catch(const std::exception& error){std::fprintf(stderr,"AUDIT lifecycle probe failed: %s\n",error.what());return 1;}
}
