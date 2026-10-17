#pragma once
#include "dyf/RHI/IDevice.h"
#include "dyf/RHI/ICommandList.h"
#include "Query.h"
#include "UploadMemory.h"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <thread>

namespace dyf::RHI
{
// Process-local 63-bit hash/token; not an OS TID and not guaranteed collision-free.
inline int64_t LiveRhiThreadToken() noexcept
{return static_cast<int64_t>(std::hash<std::thread::id>{}(std::this_thread::get_id())&INT64_MAX);}

struct IDevice::LiveState
{
    static constexpr uint32_t QuerySlots=512;
    static constexpr size_t PendingLimit=1024,SubmissionIdLimit=4096;
    static constexpr uint64_t PendingBytesLimit=Diagnostics::LiveTransferLimit;
    Diagnostics::LiveId device=0,gpuClock=0,recordingSource=0;
    Diagnostics::LiveId backBufferSubmission=0;
    Diagnostics::LiveFrameToken frame;
    bool hasFrame=false;
    struct UploadControls
    {
        Diagnostics::LiveId owner=0;
        std::array<Diagnostics::ControlRegistration,2> controls;
        Diagnostics::LiveFrameKey frame;
        bool reusePages=true,suballocate=true,applying=false;
    };
    std::unordered_map<Diagnostics::LiveId,UploadControls> uploadControls;
    struct GraphControls
    {
        Diagnostics::LiveId owner=0;
        Diagnostics::ControlRegistration control;
        Diagnostics::LiveFrameKey frame;
        bool parallel=true,applying=false;
    };
    std::unordered_map<Diagnostics::LiveId,GraphControls> graphControls;
    bool uploadPolicyInitialized=false,uploadPolicySupported=false;
    bool uploadPageReuse=true,uploadSuballocation=true;
    TimestampQueryHandle query=nullptr;
    std::array<bool,QuerySlots> reserved{};
    std::array<uint64_t,QuerySlots> untrackedCompletions{};
    struct Prepared {uint32_t slot=UINT32_MAX;};
    struct Sample {Diagnostics::LiveGpuSpan span;uint32_t slot=UINT32_MAX;};
    struct Submission
    {
        uint64_t completion=0,submittedNs=0,bytes=0;
        std::vector<Diagnostics::LiveEvent> lists;
        std::vector<Sample> samples;
        std::optional<TimestampCalibration> calibration;
    };
    std::unordered_map<ICommandList*,Prepared> prepared;
    struct SubmissionId {Diagnostics::LiveId id=0;uint64_t completedNs=0;};
    std::unordered_map<uint64_t,SubmissionId> submissions;
    std::deque<Submission> pending;
    uint64_t pendingBytes=0;
    uint64_t lastGpuBegin=0,gpuWrapBase=0;
    bool hasGpuBegin=false;
    std::atomic<bool> stopping{false},workPending{false};
    std::mutex wakeMutex;
    std::condition_variable wake;
    std::thread worker;
};

struct IDevice::LiveExecutionScope
{
    IDevice& device;
    std::vector<Diagnostics::LiveFrameToken> frames;
    std::vector<ICommandList*> activated;
    Diagnostics::LiveOutcome outcome=Diagnostics::LiveOutcome::Failed;
    LiveExecutionScope(IDevice& device,ICommandList* const* lists,uint32_t count) noexcept:device(device)
    {device.BeginLiveExecution(lists,count,*this);}
    ~LiveExecutionScope(){device.EndLiveExecution(*this);}
};

// A profiling token may end on a different recording worker; never restore its creator's TLS.
inline void EndLiveRhiFrame(Diagnostics::LiveFrameToken token,Diagnostics::LiveOutcome outcome) noexcept
{
    token.previous=Diagnostics::GetLiveContext();Diagnostics::EndLiveFrame(token,outcome);
}

inline void RecordLiveReadbackCompletion(Diagnostics::LiveId submission,Diagnostics::LiveId resource) noexcept
{
#ifdef DY_LIVE_PROFILING_ENABLED
    const auto context=Diagnostics::GetLiveContext();if(!submission||!context.frame.frame)return;
    // The native fence proves completion; this work has no GPU timestamp measurement.
    Diagnostics::LiveGpuSpan span;span.id=Diagnostics::AllocateLiveId();span.frame=context.frame;
    span.appliedRevision=context.appliedRevision;span.owner=context.owner;span.aggregation=Diagnostics::LiveGpuAggregation::FrameCoverage;
    span.begin.submission=span.end.submission=submission;span.outcome=Diagnostics::LiveOutcome::Completed;
    Diagnostics::RecordLiveGpuSpan(span);Diagnostics::RecordLiveLink({resource,span.id});
#else
    (void)submission;(void)resource;
#endif
}

struct LiveRhiScope
{
    Diagnostics::LiveEvent event;
    explicit LiveRhiScope(Diagnostics::LiveEventKind kind,Diagnostics::LiveStage stage,const char* label,bool enabled=true) noexcept
    {
#ifdef DY_LIVE_PROFILING_ENABLED
        if(!enabled||!Diagnostics::IsLiveCollectorEnabled())return;
        event.id=Diagnostics::AllocateLiveId();event.context=Diagnostics::GetLiveContext();event.kind=kind;event.stage=stage;
        if(kind==Diagnostics::LiveEventKind::NativeTranslation)
            event.arguments[4]=LiveRhiThreadToken();
        event.beginNs=Diagnostics::LiveNowNs();
        for(size_t i=0;label&&label[i]&&i+1<sizeof(event.label);++i)event.label[i]=label[i];
#else
        (void)kind;(void)stage;(void)label;(void)enabled;
#endif
    }
    void Finish() noexcept
    {
#ifdef DY_LIVE_PROFILING_ENABLED
        if(event.beginNs){event.endNs=Diagnostics::LiveNowNs();Diagnostics::RecordLiveEvent(event);event.beginNs=0;}
#endif
    }
    ~LiveRhiScope(){Finish();}
};
inline void RecordLiveRelease(const void* resource) noexcept
{
#ifdef DY_LIVE_PROFILING_ENABLED
    if(!Diagnostics::IsLiveCollectorEnabled())return;
    Diagnostics::LiveEvent event;event.context=Diagnostics::GetLiveContext();event.resource=Diagnostics::FindLiveObject(resource);
    event.kind=Diagnostics::LiveEventKind::CpuWork;event.stage=Diagnostics::LiveStage::Data;event.outcome=Diagnostics::LiveOutcome::Pending;
    const char label[]="Resource release requested";
    for(size_t i=0;i<sizeof(label);++i)event.label[i]=label[i];
    Diagnostics::RecordLiveEvent(event);
#else
    (void)resource;
#endif
}
}
