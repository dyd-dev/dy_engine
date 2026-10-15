#pragma once
#include "dyf/RHI/IDevice.h"
#include "dyf/RHI/ICommandList.h"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <thread>

namespace dyf::RHI
{
struct IDevice::LiveState
{
    static constexpr uint32_t QuerySlots=512;
    static constexpr size_t PendingLimit=1024,SubmissionIdLimit=4096;
    static constexpr uint64_t PendingBytesLimit=Diagnostics::LiveTransferLimit;
    Diagnostics::LiveId device=0,gpuClock=0;
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

struct LiveRhiScope
{
    Diagnostics::LiveEvent event;
    explicit LiveRhiScope(Diagnostics::LiveEventKind kind,Diagnostics::LiveStage stage,const char* label) noexcept
    {
#ifdef DY_LIVE_PROFILING_ENABLED
        if(!Diagnostics::IsLiveCollectorEnabled())return;
        event.id=Diagnostics::AllocateLiveId();event.context=Diagnostics::GetLiveContext();event.kind=kind;event.stage=stage;
        event.beginNs=Diagnostics::LiveNowNs();
        for(size_t i=0;label&&label[i]&&i+1<sizeof(event.label);++i)event.label[i]=label[i];
#else
        (void)kind;(void)stage;(void)label;
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
