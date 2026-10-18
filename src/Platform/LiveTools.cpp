#include "dyf/Platform/LiveTools.h"
#include "dyf/Platform/Profiler.h"
#include "dyf/Platform/RenderDocCapture.h"
#include <cstdio>
#include <cstring>

namespace dyf::Platform
{
namespace D=Diagnostics;
namespace
{
std::int64_t TracyState()
{
#if defined(DY_TRACY_ENABLED)
    return TracyIsConnected ? 2 : 1;
#else
    return 0;
#endif
}

std::string Mark(const char* label,D::LiveId request,D::LiveFrameKey frame)
{
    char text[256];
    std::snprintf(text,sizeof(text),"%s request=%llu device=%llu source=%llu frame=%llu",label,
        static_cast<unsigned long long>(request),static_cast<unsigned long long>(frame.device),
        static_cast<unsigned long long>(frame.source),static_cast<unsigned long long>(frame.frame));
#if defined(DY_TRACY_ENABLED)
    if(TracyIsConnected) TracyMessage(text,std::strlen(text));
#endif
    D::LiveEvent event;event.context=D::GetLiveContext();event.context.frame=frame;
    event.kind=D::LiveEventKind::CpuWork;event.stage=D::LiveStage::CpuPrepare;
    event.beginNs=event.endNs=D::LiveNowNs();event.arguments[0]=static_cast<std::int64_t>(request);
    std::snprintf(event.label,sizeof(event.label),"%s",label);D::RecordLiveEvent(event);
    return text;
}
}

LiveTools::LiveTools(D::LiveId device,D::LiveId source):owner(D::AllocateLiveId())
{
    const char* keys[]{"tools.tracy","tools.tracy.bookmark","tools.renderdoc.capture","tools.renderdoc.open","tools.renderdoc.captures","tools.renderdoc.connected","tools.renderdoc.connect"};
    for(int i=0;i<Count;++i)
    {
        D::ControlDesc desc;desc.device=device;desc.source=source;desc.owner=owner;
        desc.key=desc.label=keys[i];desc.category=D::ControlCategory::Diagnostic;
        D::ControlHandler handler;
        if(i==Tracy)
        {
            desc.kind=D::ControlKind::Enumeration;desc.appliedValue=TracyState();desc.writable=false;
            desc.choices={{0,"Disabled in this build"},{1,"Waiting for Tracy"},{2,"Connected"}};
        }
        else if(i==Captures)
        {
            desc.kind=D::ControlKind::Integer;desc.appliedValue=std::int64_t(0);desc.maximum=UINT32_MAX;desc.writable=false;
        }
        else if(i==Connected) desc.writable=false;
        else handler=[i](const D::ControlRequest& request,D::LiveFrameKey frame) {
            const char* label=i==Bookmark ? "Tracy bookmark" : "RenderDoc capture requested";
            if(i==Open||i==Connect)
            {
                switch(i==Open ? RenderDocCapture::OpenReplayUI() : RenderDocCapture::ConnectReplayUI())
                {
                case RenderDocCapture::ReplayUIResult::Unavailable:
                    return D::ControlApplyResult{D::ControlState::Failed,false,"RenderDoc API is unavailable"};
                case RenderDocCapture::ReplayUIResult::NoCaptures:
                    return D::ControlApplyResult{D::ControlState::Failed,false,"No RenderDoc capture is available"};
                case RenderDocCapture::ReplayUIResult::LaunchFailed:
                    return D::ControlApplyResult{D::ControlState::Failed,false,"RenderDoc replay UI launch failed"};
                case RenderDocCapture::ReplayUIResult::AlreadyConnected:
                    label=i==Open ? "RenderDoc UI already connected; use its capture list" : "RenderDoc UI already connected";break;
                case RenderDocCapture::ReplayUIResult::Launched:
                    label="RenderDoc replay UI launched; wait for its target connection";break;
                }
            }
            else if(i==Bookmark ? TracyState()!=2 : !RenderDocCapture::TriggerNextFrame())
                return D::ControlApplyResult{D::ControlState::Failed,false,i==Bookmark ? "Tracy profiler is not connected" : "RenderDoc API is unavailable"};
            const auto message=Mark(label,request.request,frame);
            return D::ControlApplyResult{D::ControlState::Applied,false,message,D::ControlValue(false)};
        };
        controls[size_t(i)]=D::RegisterControl(std::move(desc),std::move(handler));
    }
    Refresh();
}

void LiveTools::Refresh()
{
    const auto tracy=TracyState();
    D::PublishControlValue(controls[Tracy].GetId(),tracy);
    D::PublishControlAvailability(controls[Bookmark].GetId(),tracy==2,tracy==0 ? "Tracy is disabled in the target build" : tracy==1 ? "Connect Tracy to this target first" : "");
    const bool available=RenderDocCapture::IsAvailable();
    const auto captures=RenderDocCapture::GetCaptureCount();
    const char* reason=available ? "" : RenderDocCapture::IsEnabled() ? "Launch the game through RenderDoc before attaching the analyzer" : "Rebuild the target with DY_ENABLE_RENDERDOC=ON";
    D::PublishControlAvailability(controls[Capture].GetId(),available,reason);
    D::PublishControlAvailability(controls[Connect].GetId(),available,reason);
    D::PublishControlAvailability(controls[Open].GetId(),available&&captures,available&&!captures ? "Capture a frame first" : reason);
    D::PublishControlValue(controls[Captures].GetId(),std::int64_t(captures));
    D::PublishControlValue(controls[Connected].GetId(),RenderDocCapture::IsReplayUIConnected());
}

void LiveTools::Poll(D::LiveFrameKey frame)
{
    const auto applied=D::Detail::PollControlsSerialized(owner,frame);
    const auto now=D::LiveNowNs();
    if(applied||now>=nextRefresh) { Refresh();nextRefresh=now+100000000; }
}
}
