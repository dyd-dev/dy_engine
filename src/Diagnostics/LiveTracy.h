#pragma once
#include "dyf/Diagnostics/LiveProfiling.h"
#include <charconv>
#include <cstdio>

namespace dyf::Diagnostics::Detail
{
// v1 contains identities/counts only. Durations always come from Tracy's own clock.
enum class LiveTraceKind : unsigned { AttemptBegin=1, AttemptEnd, FrameEnd, StageBegin, StageEnd, FrameSeal };
struct LiveTraceEvent
{
    LiveTraceKind kind=LiveTraceKind::FrameEnd;
    LiveSessionId session;
    uint64_t connection=0;
    LiveFrameKey frame;
    LiveId revision=0, id=0;
    uint64_t aux=0, originNs=0, endNs=0, previousFrame=0, sequence=0, unsupportedStages=0;
    std::array<uint64_t,LiveStageCount> stageCount{},stageDigest{};
};
inline uint64_t LiveTraceDigest(uint64_t id) noexcept
{
    id^=id>>30;id*=0xbf58476d1ce4e5b9ull;id^=id>>27;id*=0x94d049bb133111ebull;return id^(id>>31);
}
inline std::string EncodeLiveTrace(const LiveTraceEvent& e)
{
    std::string out="dyf.live.v1";out.reserve(768);
    const auto add=[&](uint64_t n){char buf[24];const auto r=std::to_chars(buf,buf+sizeof(buf),n);out+=' ';out.append(buf,r.ptr);};
    add(unsigned(e.kind));add(e.session.high);add(e.session.low);add(e.connection);add(e.frame.device);add(e.frame.source);add(e.frame.frame);
    add(e.revision);add(e.id);add(e.aux);add(e.originNs);add(e.endNs);add(e.previousFrame);add(e.sequence);add(e.unsupportedStages);
    for(auto n:e.stageCount)add(n);for(auto n:e.stageDigest)add(n);return out;
}
inline bool DecodeLiveTrace(std::string_view text,LiveTraceEvent& out)
{
    constexpr std::string_view prefix="dyf.live.v1 ";
    if(text.size()>1024 || text.substr(0,prefix.size())!=prefix)return false;
    const char* p=text.data()+prefix.size();const char* end=text.data()+text.size();
    const auto get=[&](uint64_t& n){if(p==end||*p<'0'||*p>'9')return false;auto r=std::from_chars(p,end,n);if(r.ec!=std::errc{})return false;p=r.ptr;if(p<end){if(*p!=' ')return false;++p;}return true;};
    LiveTraceEvent e;uint64_t kind=0;
    if(!get(kind)||kind<1||kind>6||!get(e.session.high)||!get(e.session.low)||!get(e.connection)||!get(e.frame.device)||!get(e.frame.source)||!get(e.frame.frame)
        ||!get(e.revision)||!get(e.id)||!get(e.aux)||!get(e.originNs)||!get(e.endNs)||!get(e.previousFrame)||!get(e.sequence)||!get(e.unsupportedStages))return false;
    for(auto& n:e.stageCount)if(!get(n))return false;
    for(auto& n:e.stageDigest)if(!get(n))return false;
    if(p!=end || text.back()==' ' || (!e.session.high&&!e.session.low)||!e.connection||!e.frame.device||!e.frame.source||!e.frame.frame||!e.revision||!e.id)return false;
    e.kind=LiveTraceKind(kind);
    if((e.kind==LiveTraceKind::StageBegin||e.kind==LiveTraceKind::StageEnd)&&e.aux>=LiveStageCount)return false;
    if((e.kind==LiveTraceKind::FrameEnd||e.kind==LiveTraceKind::FrameSeal)&&(!e.sequence||e.endNs<e.originNs||e.aux>unsigned(LiveOutcome::Unknown)))return false;
    out=e;return true;
}
#ifdef DY_LIVE_TRACE_TEST
// Implemented by the producer contract executable, never in product builds.
void CaptureLiveTraceForTest(const LiveTraceEvent&,uint64_t) noexcept;
#endif
}
