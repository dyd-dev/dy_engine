#define main RendererExceptionEntryUnused
#include "RendererExceptionTests.cpp"
#undef main

namespace { bool failAllocation=false; }
void* operator new(std::size_t bytes)
{
    if(std::exchange(failAllocation,false))throw std::bad_alloc();
    if(auto* result=std::malloc(bytes ? bytes : 1))return result;
    throw std::bad_alloc();
}
void operator delete(void* pointer) noexcept {std::free(pointer);}
void operator delete(void* pointer,std::size_t) noexcept {std::free(pointer);}

int main()
{
    try
    {
        Device device;
        dyf::RendererConfig config;config.enableProfilerHud=false;
        auto renderer=dyf::Renderer::Create(device,reinterpret_cast<void*>(1),config);
        Check(bool(renderer),"Renderer creation failed");
        auto& state=*renderer->m_impl;
        const auto liveId=state.liveId;
        state.liveId=std::numeric_limits<dyf::Platform::LiveId>::max();
        state.config.lighting.environment.probes.resize(1);
        state.config.exposure=2;
        failAllocation=true;
        state.FinishLiveSettings(true,{});
        Check(!failAllocation,"Diagnostic snapshot allocation failure was not injected");
        Check(state.config.lighting.environment.probes.size()==1 && state.config.exposure==2,
            "Diagnostic snapshot failure changed applied configuration");
        Check(state.liveAppliedConfig.lighting.environment.probes.empty() && state.liveAppliedConfig.exposure==1,
            "Failed diagnostic snapshot partially changed the previous snapshot");
        state.FinishLiveSettings(true,{});
        Check(state.liveAppliedConfig.lighting.environment.probes.size()==1 && state.liveAppliedConfig.exposure==2,
            "Diagnostic snapshot retry failed");
        state.liveId=liveId;
        std::puts("PASS renderer live snapshot allocation failure and retry.");
        return 0;
    }
    catch(const std::exception& error)
    {std::fprintf(stderr,"Renderer live test failed: %s\n",error.what());return 1;}
}
