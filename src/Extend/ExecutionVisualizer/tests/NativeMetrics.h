#pragma once
#include "dyf/Diagnostics/LiveProfiling.h"
#include <windows.h>
#include <map>
#include <ostream>
#include <stdexcept>
#include <tuple>
#include <limits>
#include <cstring>

namespace native_metrics
{
inline uint64_t HistoryFrames(const dyf::Diagnostics::LiveSourceSnapshot& source)
{
    using Bucket=std::tuple<uint64_t,uint64_t,uint64_t>;
    std::map<Bucket,uint64_t> unique;
    for(const auto& bucket:source.history)
    {
        if(bucket.device!=source.device || bucket.source!=source.source || bucket.endNs<=bucket.beginNs)
            throw std::runtime_error("Invalid history source or interval");
        const auto inserted=unique.emplace(Bucket{bucket.appliedRevision,bucket.beginNs,bucket.endNs},bucket.frameCount);
        if(!inserted.second && inserted.first->second!=bucket.frameCount) throw std::runtime_error("Duplicate history bucket has conflicting frame counts");
    }
    uint64_t total=0,previousRevision=0,previousEnd=0; bool first=true;
    for(const auto& bucket:unique)
    {
        const auto [revision,begin,end]=bucket.first;
        if(!first && revision==previousRevision && begin<previousEnd) throw std::runtime_error("Overlapping same-revision history would double-count frames");
        if(bucket.second>std::numeric_limits<uint64_t>::max()-total) throw std::runtime_error("History count overflow");
        total+=bucket.second; previousRevision=revision; previousEnd=end; first=false;
    }
    // Producer partitions each frame by exactly one revision, even in the same time bucket.
    return total;
}
inline bool HasWork(const dyf::Diagnostics::LiveFrameSummary& frame)
{
    return frame.recorded.draw || frame.recorded.drawIndexed || frame.recorded.dispatch || frame.recorded.dispatchMesh
        || frame.recorded.uploadCalls || frame.submitted.submissions;
}
class RetryAudit
{
public:
    void Observe(const dyf::Diagnostics::LiveSourceSnapshot& source,bool transition=false)
    {
        namespace D=dyf::Diagnostics;
        const auto& frame=source.latest;
        if(transition || !frame.key.frame || frame.key.device!=source.device || frame.key.source!=source.source
            || frame.incomplete || frame.endNs<=frame.beginNs || source.flow.key!=frame.key
            || source.flow.omittedEvents || source.flow.omittedLinks || source.flow.events.empty()) return;
        bool acquireOnly=true,present=false;
        for(const auto& event:source.flow.events)
        {
            if(event.context.frame!=frame.key || event.context.appliedRevision!=frame.appliedRevision) return;
            acquireOnly &= event.stage==D::LiveStage::Output && (event.kind==D::LiveEventKind::Output || event.kind==D::LiveEventKind::Wait)
                && event.commandKind==D::LiveCommandKind::None && !event.command.commandList && !event.submission
                && (event.outcome==D::LiveOutcome::Failed || event.outcome==D::LiveOutcome::Skipped)
                && std::strncmp(event.label,"BeginFrame / acquire",sizeof(event.label))==0;
            present |= event.kind==D::LiveEventKind::Output && event.stage==D::LiveStage::Output
                && event.commandKind==D::LiveCommandKind::Present && event.outcome==D::LiveOutcome::Completed && event.submission;
        }
        if(!HasWork(frame) && acquireOnly && (frame.outcome==D::LiveOutcome::Failed || frame.outcome==D::LiveOutcome::Skipped))
            candidates.emplace(Key{frame.key.device,frame.key.source,frame.key.frame},Candidate{frame.endNs});
        if(frame.outcome!=D::LiveOutcome::Completed || !present || !frame.submitted.submissions
            || !(frame.recorded.draw || frame.recorded.drawIndexed || frame.recorded.dispatch || frame.recorded.dispatchMesh)) return;
        for(auto& [key,candidate]:candidates)
            if(!candidate.confirmed && std::get<0>(key)==source.device && std::get<1>(key)==source.source && candidate.endNs<=frame.beginNs)
            { candidate.confirmed=true; ++confirmed; }
    }
    uint64_t Candidates() const { return candidates.size(); }
    uint64_t Confirmed() const { return confirmed; }
private:
    using Key=std::tuple<uint64_t,uint64_t,uint64_t>;
    struct Candidate { uint64_t endNs; bool confirmed=false; };
    std::map<Key,Candidate> candidates;
    uint64_t confirmed=0;
};
struct Cost
{
    uint64_t creation=0,exit=0,kernel=0,user=0;
};
inline uint64_t Ticks(FILETIME value) { return (uint64_t(value.dwHighDateTime)<<32)|value.dwLowDateTime; }
class Meter
{
public:
    ~Meter() { if(handle) CloseHandle(handle); }
    Meter()=default;
    Meter(const Meter&)=delete;
    Meter& operator=(const Meter&)=delete;
    void Open(uint32_t pid)
    {
        auto replacement=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION|SYNCHRONIZE,FALSE,pid);
        if(!replacement) throw std::runtime_error("OpenProcess for timing failed: "+std::to_string(GetLastError()));
        if(handle) CloseHandle(handle); handle=replacement;
    }
    Cost Finished() const
    {
        if(!handle || WaitForSingleObject(handle,0)!=WAIT_OBJECT_0) throw std::runtime_error("Final process timing requires actual termination");
        FILETIME created{},exited{},kernel{},user{};
        if(!GetProcessTimes(handle,&created,&exited,&kernel,&user)) throw std::runtime_error("GetProcessTimes failed");
        return {Ticks(created),Ticks(exited),Ticks(kernel),Ticks(user)};
    }
private: HANDLE handle=nullptr;
};
inline Cost DriverCutoff()
{
    FILETIME created{},unusedExit{},kernel{},user{},cutoff{};
    if(!GetProcessTimes(GetCurrentProcess(),&created,&unusedExit,&kernel,&user)) throw std::runtime_error("Driver GetProcessTimes failed");
    GetSystemTimePreciseAsFileTime(&cutoff);
    return {Ticks(created),Ticks(cutoff),Ticks(kernel),Ticks(user)};
}
inline void Report(std::ostream& output,const char* prefix,const Cost& cost,bool terminated=true)
{
    if(!cost.creation || cost.exit<cost.creation) throw std::runtime_error("Invalid process creation/exit times");
    output<<prefix<<"_measurement_scope="<<(terminated ? "creation_to_exit" : "creation_to_report_cutoff")<<'\n'
        <<prefix<<"_measurement_complete="<<terminated<<'\n'<<prefix<<"_creation_100ns="<<cost.creation<<'\n'<<prefix<<(terminated ? "_exit_100ns=" : "_cutoff_100ns=")<<cost.exit<<'\n'
        <<prefix<<"_wall_ms="<<double(cost.exit-cost.creation)/10000.0<<'\n'<<prefix<<"_kernel_ms="<<double(cost.kernel)/10000.0<<'\n'
        <<prefix<<"_user_ms="<<double(cost.user)/10000.0<<'\n'<<prefix<<"_cpu_total_ms="<<double(cost.kernel+cost.user)/10000.0<<'\n';
}
}
