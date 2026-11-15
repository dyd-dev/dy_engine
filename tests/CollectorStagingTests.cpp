#define DY_LIVE_PROFILING_ENABLED 1
#include "../src/Platform/Profiler/Collector.cpp"
#include <cstdlib>
#include <cstdio>
#include <stdexcept>

namespace { int failAfter=-1;unsigned injectedFailures=0,skippedDebugProxies=0; }
void* operator new(std::size_t bytes)
{
    bool eligible=true;
#if defined(_MSC_VER) && _ITERATOR_DEBUG_LEVEL != 0
    // MSVC's noexcept container constructors allocate iterator proxies in Debug.
    if(bytes==sizeof(std::_Container_proxy)) {eligible=false;if(failAfter>=0)++skippedDebugProxies;}
#endif
    if(eligible && failAfter>=0 && failAfter--==0){failAfter=-1;++injectedFailures;throw std::bad_alloc();}
    if(auto* result=std::malloc(bytes ? bytes : 1))return result;
    throw std::bad_alloc();
}
void operator delete(void* value) noexcept {std::free(value);}
void operator delete(void* value,std::size_t) noexcept {std::free(value);}

namespace P=dyf::Platform;
static void Check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
static P::ControlState Status(P::LiveId request)
{
    const auto snapshot=P::ReadLiveSnapshot();
    for(const auto& status:snapshot->controlStatus)if(status.request==request)return status.state;
    throw std::runtime_error("Missing control status");
}
int main()
{
    std::set_terminate([]{std::fputs("Unexpected termination during collector allocation test.\n",stderr);std::_Exit(2);});
    try
    {
        Check(P::SetLiveCollectorEnabled(true),"Live collector unavailable");
        for(int point=0;point<4;++point)
        {
            unsigned callbacks=0;
            P::ControlDesc desc;desc.device=1;desc.source=2;desc.owner=2;desc.key="valid";
            auto valid=P::RegisterControl(desc,[&](const auto& request,auto) {
                ++callbacks;return P::ControlApplyResult{P::ControlState::PendingApply,request.value,{}};
            });
            desc.key="invalid";
            auto invalid=P::RegisterControl(desc,[](const auto&,auto) {
                throw std::runtime_error("Invalid type reached handler");return P::ControlApplyResult{};
            });
            const auto good=P::RequestLiveControl(valid.GetId(),true);
            const auto bad=P::RequestLiveControl(invalid.GetId(),std::int64_t(3));
            Check(good && bad,"Control requests missing");
            failAfter=point;
            (void)P::PollControls(2,{1,2,1});
            failAfter=-1;
            (void)P::PollControls(2,{1,2,2});
            Check(callbacks==1 && Status(good)==P::ControlState::PendingApply,
                "Staging allocation failure stranded or repeated a deferred callback");
            Check(Status(bad)==P::ControlState::Rejected,"Invalid request was not terminal");
            P::CompleteControl(good,{1,2,3},true);
            Check(Status(good)==P::ControlState::Applied,"Deferred control completion failed");
        }
        Check(injectedFailures>=2,"Staging and rejection-message allocation failures were not both exercised");
        std::printf("PASS control staging allocation failures, invalid request diagnostics and deferred completion; injected=%u skipped_debug_proxies=%u.\n",injectedFailures,skippedDebugProxies);
        return 0;
    }
    catch(const std::exception& error)
    {failAfter=-1;std::fprintf(stderr,"Collector staging test failed: %s\n",error.what());return 1;}
}
