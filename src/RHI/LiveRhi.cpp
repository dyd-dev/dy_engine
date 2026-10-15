#include "LiveRhi.h"
#include <algorithm>
#include <cmath>

namespace dyf::RHI
{
namespace D=Diagnostics;
namespace
{
const char* BackendName() noexcept
{
#if defined(ENABLE_D3D12)
    return "D3D12";
#elif defined(ENABLE_VULKAN)
    return "Vulkan";
#elif defined(ENABLE_METAL)
    return "Metal";
#else
    return "Null";
#endif
}
bool Nanoseconds(long double value,uint64_t& result) noexcept
{
    if(!std::isfinite(value)||value<0||value>static_cast<long double>(UINT64_MAX))return false;
    result=static_cast<uint64_t>(value);return true;
}
}

IDevice::LiveState* IDevice::GetLiveState() noexcept
{
#ifdef DY_LIVE_PROFILING_ENABLED
    if(!D::IsLiveCollectorEnabled())return nullptr;
    try
    {
        if(!m_live)
        {
            auto live=std::make_unique<LiveState>();
            live->device=D::FindLiveObject(this);
            if(!live->device)live->device=D::RegisterLiveObject(this,{D::LiveObjectKind::Device,0,0,0,0,BackendName()});
            live->gpuClock=live->device;
            m_live=std::move(live);
            PublishLiveResources();
        }
        return m_live.get();
    }
    catch(...){return nullptr;}
#else
    return nullptr;
#endif
}

void IDevice::PrepareLiveControls(D::LiveFrameKey key) noexcept
{
#ifdef DY_LIVE_PROFILING_ENABLED
    if(!D::IsLiveCollectorEnabled()||!key.device||!key.source||!key.frame||D::GetLiveContext().frame!=key)return;
    try
    {
        std::lock_guard<std::recursive_mutex> lock(m_resourceMutex);
        auto* live=GetLiveState();if(!live||live->stopping||live->device!=key.device)return;
        if(!live->uploadPolicyInitialized)
        {
            live->uploadPolicySupported=SetUploadMemoryPolicyNative(live->uploadPageReuse,live->uploadSuballocation);
            live->uploadPolicyInitialized=true;
        }
        auto found=live->uploadControls.find(key.source);
        if(found==live->uploadControls.end())
        {
            LiveState::UploadControls source;source.owner=D::AllocateLiveId();
            source.reusePages=live->uploadPageReuse;source.suballocate=live->uploadSuballocation;
            for(size_t i=0;i<source.controls.size();++i)
            {
                D::ControlDesc desc;desc.device=key.device;desc.source=key.source;desc.owner=source.owner;
                desc.key=i?"rhi.uploadSuballocation":"rhi.uploadPageReuse";
                desc.label=i?"Upload page suballocation":"Upload page reuse";
                desc.appliedValue=i?source.suballocate:source.reusePages;
                desc.writable=live->uploadPolicySupported;
                if(!desc.writable)desc.unavailableReason="Native upload policies are not implemented by this backend";
                desc.category=D::ControlCategory::Optimization;
                desc.description=i?"Share aligned upload slices within each native command list":"Reuse completed native upload pages across command lists";
                desc.onPath=i?"Bump-allocate aligned slices in the last leased page":"Acquire a fitting retired page before allocating";
                desc.offPath=i?"Lease a separate page for every upload request":"Allocate a new page for each lease; do not cache returned pages";
                desc.conditions="Device policy is adopted at each source's first preparation boundary; recorded lists retain their policy";
                desc.tradeoffs=i?"Fewer pages versus alignment gaps; requested bytes and GPU payload remain unchanged":"Less native allocation versus bounded cached memory; existing cached pages remain reported while disabled";
                source.controls[i]=D::RegisterControl(std::move(desc),[this,sourceId=key.source,i](const D::ControlRequest& request,D::LiveFrameKey frame)
                {
                    auto* state=m_live.get();
                    if(!state||frame.device!=state->device||frame.source!=sourceId)
                        return D::ControlApplyResult{D::ControlState::Failed,false,"Source frame mismatch"};
                    auto owner=state->uploadControls.find(sourceId);
                    if(owner==state->uploadControls.end()||!owner->second.applying||owner->second.frame!=frame)
                        return D::ControlApplyResult{D::ControlState::Failed,false,"Upload policies apply only at the source preparation boundary"};
                    const bool value=std::get<bool>(request.value);
                    const bool reuse=i?state->uploadPageReuse:value,suballocate=i?value:state->uploadSuballocation;
                    if(!SetUploadMemoryPolicyNative(reuse,suballocate))
                        return D::ControlApplyResult{D::ControlState::Failed,i?state->uploadSuballocation:state->uploadPageReuse,"Native upload policy rejected"};
                    state->uploadPageReuse=owner->second.reusePages=reuse;
                    state->uploadSuballocation=owner->second.suballocate=suballocate;
                    return D::ControlApplyResult{D::ControlState::Applied,value,{}};
                });
                if(!source.controls[i].GetId())return;
            }
            found=live->uploadControls.emplace(key.source,std::move(source)).first;
        }
        auto& source=found->second;if(source.frame==key)return;
        const bool changed=source.reusePages!=live->uploadPageReuse||source.suballocate!=live->uploadSuballocation;
        source.reusePages=live->uploadPageReuse;source.suballocate=live->uploadSuballocation;source.frame=key;
        D::NotifyLiveSettingsApplied(key.source,key,changed);
        D::PublishControlValue(source.controls[0].GetId(),source.reusePages);
        D::PublishControlValue(source.controls[1].GetId(),source.suballocate);
        source.applying=true;D::PollControls(source.owner,key);source.applying=false;
        D::PublishControlValue(source.controls[0].GetId(),source.reusePages);
        D::PublishControlValue(source.controls[1].GetId(),source.suballocate);
    }
    catch(...){}
#else
    (void)key;
#endif
}

void IDevice::ReleaseLiveControls(D::LiveId source) noexcept
{
#ifdef DY_LIVE_PROFILING_ENABLED
    try{std::lock_guard<std::recursive_mutex> lock(m_resourceMutex);if(m_live)m_live->uploadControls.erase(source);}catch(...){}
#else
    (void)source;
#endif
}

void IDevice::CaptureLiveUploadPolicy(ICommandList& commands) noexcept
{
#ifdef DY_LIVE_PROFILING_ENABLED
    if(!m_live)return;
    const auto source=m_live->uploadControls.find(commands.m_liveContext.frame.source);
    if(source!=m_live->uploadControls.end()&&source->second.frame==commands.m_liveContext.frame)
    {commands.m_uploadPageReuse=source->second.reusePages;commands.m_uploadSuballocation=source->second.suballocate;}
    else
    {commands.m_uploadPageReuse=m_live->uploadPageReuse;commands.m_uploadSuballocation=m_live->uploadSuballocation;}
#else
    (void)commands;
#endif
}

void IDevice::PublishLiveResources() noexcept
{
#ifdef DY_LIVE_PROFILING_ENABLED
    if(!m_live)return;
    try
    {
        D::LiveDeviceSnapshot snapshot;snapshot.device=m_live->device;snapshot.backend=BackendName();
        snapshot.observedNs=D::LiveNowNs();
        const auto upload=GetUploadMemoryStatisticsNative();
        snapshot.upload={upload.allocationCount,upload.reuseCount,upload.allocationBytes,upload.requestCount,
            upload.requestedBytes,upload.cachedBytes,upload.inUseBytes,upload.peakInUseBytes};
        for(const auto& item:std::array<std::pair<D::LiveObjectKind,ResourceCounter>,3>{{
            {D::LiveObjectKind::Buffer,m_allocationCounters.buffers},{D::LiveObjectKind::Texture,m_allocationCounters.textures},
            {D::LiveObjectKind::Pipeline,m_allocationCounters.pipelines}}})
            snapshot.resources.push_back({item.first,item.second.live,item.second.created,item.second.destroyed,0});
        D::PublishLiveDevice(snapshot);
    }
    catch(...){}
#endif
}

D::LiveId IDevice::GetLiveSubmissionId(FenceHandle completion) const noexcept
{
#ifdef DY_LIVE_PROFILING_ENABLED
    try
    {
        std::lock_guard<std::recursive_mutex> lock(m_resourceMutex);
        if(!m_live||completion.m_device!=this||!completion.m_value)return 0;
        const auto found=m_live->submissions.find(completion.m_value);
        if(found==m_live->submissions.end())return 0;
        if(found->second.completedNs&&D::LiveNowNs()-found->second.completedNs>D::LiveHistoryWindowNs)return 0;
        return found->second.id;
    }
    catch(...){return 0;}
#else
    (void)completion;return 0;
#endif
}

uint32_t IDevice::ReserveLiveQuery(ICommandList* source) noexcept
{
#ifdef DY_LIVE_PROFILING_ENABLED
    auto* live=GetLiveState();
    if(!live||live->stopping||!Supports(Feature::TimestampQuery))return UINT32_MAX;
    try
    {
        if(!live->query)live->query=CreateTimestampQuery({LiveState::QuerySlots*2});
        if(!live->query)return UINT32_MAX;
        const auto completed=GetCompletedSubmissionNative();
        for(uint32_t slot=0;slot<LiveState::QuerySlots;++slot)
            if(live->untrackedCompletions[slot]&&live->untrackedCompletions[slot]<=completed)
            {live->untrackedCompletions[slot]=0;live->reserved[slot]=false;}
        for(uint32_t slot=0;slot<LiveState::QuerySlots;++slot)if(!live->reserved[slot])
        {
            live->prepared.emplace(source,LiveState::Prepared{slot});
            live->reserved[slot]=true;return slot;
        }
    }
    catch(...){}
#else
    (void)source;
#endif
    return UINT32_MAX;
}

void IDevice::DiscardLivePrepared(ICommandList* source,D::LiveOutcome outcome) noexcept
{
#ifdef DY_LIVE_PROFILING_ENABLED
    if(!m_live)return;
    const auto found=m_live->prepared.find(source);
    if(found!=m_live->prepared.end()){m_live->reserved[found->second.slot]=false;m_live->prepared.erase(found);}
    if(source&&source->m_livePosition.commandList)
    {
        D::LiveEvent event;event.context=source->m_liveContext;event.command=source->m_livePosition;
        event.kind=D::LiveEventKind::NativeTranslation;event.stage=D::LiveStage::NativeTranslate;event.outcome=outcome;
        D::RecordLiveEvent(event);
    }
#else
    (void)source;(void)outcome;
#endif
}

void IDevice::SubmitLiveWork(ICommandList* const* commands,uint32_t count,bool success,uint64_t completion) noexcept
{
#ifdef DY_LIVE_PROFILING_ENABLED
    auto* live=GetLiveState();if(!live)return;
    LiveState::Submission pending;
    try
    {
        if(!success)
        {
            for(uint32_t i=0;i<count;++i)DiscardLivePrepared(commands[i],D::LiveOutcome::Failed);
            return;
        }
        pending.completion=completion;pending.submittedNs=D::LiveNowNs();
        for(auto it=live->submissions.begin();it!=live->submissions.end();)
            if(it->second.completedNs&&pending.submittedNs-it->second.completedNs>D::LiveHistoryWindowNs)it=live->submissions.erase(it);else ++it;
        while(live->submissions.size()>=LiveState::SubmissionIdLimit)
        {
            auto oldest=live->submissions.end();
            for(auto it=live->submissions.begin();it!=live->submissions.end();++it)
                if(it->second.completedNs&&(oldest==live->submissions.end()||it->second.completedNs<oldest->second.completedNs))oldest=it;
            if(oldest==live->submissions.end())break;
            live->submissions.erase(oldest);
        }
        const auto submission=D::AllocateLiveId();
        if(live->submissions.size()<LiveState::SubmissionIdLimit)live->submissions.emplace(completion,LiveState::SubmissionId{submission,0});
        pending.lists.reserve(count);pending.samples.reserve(count);
        const auto submissionContext=D::GetLiveContext();
        pending.bytes=sizeof(pending)+uint64_t(count)*(sizeof(D::LiveEvent)+sizeof(LiveState::Sample));
        const bool track=live->pending.size()<LiveState::PendingLimit&&pending.bytes<=LiveState::PendingBytesLimit-live->pendingBytes;
        std::vector<D::LiveFrameKey> frames;
        for(uint32_t i=0;i<count;++i)
        {
            auto* list=commands[i];if(!list->m_livePosition.commandList)continue;
            const auto context=list->m_liveContext.frame.frame?list->m_liveContext:submissionContext;
            D::LiveContextScope contextScope(context);
            if(std::find(frames.begin(),frames.end(),context.frame)==frames.end())
            {frames.push_back(context.frame);D::SetLiveGpuCoverage(context.frame,false);}
            list->FlushLiveDeferred(context);
            D::LiveEvent event;event.id=D::AllocateLiveId();event.context=context;event.command=list->m_livePosition;
            event.submission=submission;event.kind=D::LiveEventKind::Submission;event.stage=D::LiveStage::Submit;
            event.outcome=D::LiveOutcome::Submitted;event.operations=i==0?1:0;
            pending.lists.push_back(event);D::RecordLiveEvent(event);
            if(list->m_livePreviousEvent)D::RecordLiveLink({list->m_livePreviousEvent,event.id});
            if(list->m_imageGeneration)live->backBufferSubmission=submission;
            LiveState::Sample sample;sample.span.id=D::AllocateLiveId();sample.span.frame=context.frame;
            sample.span.appliedRevision=context.appliedRevision;sample.span.owner=context.owner;
            sample.span.aggregation=D::LiveGpuAggregation::FrameCoverage;sample.span.gpuClockDomain=live->gpuClock;
            sample.span.begin={{list->m_livePosition.commandList,list->m_livePosition.generation,list->m_livePosition.index?1u:0u},submission};
            sample.span.end={list->m_livePosition,submission};
            const auto prepared=live->prepared.find(list);
            if(prepared!=live->prepared.end())
            {sample.slot=prepared->second.slot;live->untrackedCompletions[sample.slot]=completion;
                live->prepared.erase(prepared);sample.span.duration.state=D::LiveValueState::Pending;}
            else sample.span.duration.state=Supports(Feature::TimestampQuery)?D::LiveValueState::Unavailable:D::LiveValueState::Unsupported;
            if(!track)
            {
                if(sample.slot!=UINT32_MAX)live->untrackedCompletions[sample.slot]=completion;
                sample.span.outcome=D::LiveOutcome::Unknown;sample.span.duration.state=D::LiveValueState::Unavailable;
                D::LiveEvent omitted;omitted.context=context;omitted.kind=D::LiveEventKind::CpuWork;
                omitted.stage=D::LiveStage::Gpu;omitted.outcome=D::LiveOutcome::Skipped;omitted.reason=D::LiveSplitReason::Capacity;
                omitted.command=list->m_livePosition;omitted.submission=submission;omitted.elements=1;
                const char label[]="Completion unavailable: live pending capacity";
                std::copy(std::begin(label),std::end(label),omitted.label);D::RecordLiveEvent(omitted);
            }
            D::RecordLiveGpuSpan(sample.span);D::RecordLiveLink({event.id,sample.span.id});pending.samples.push_back(sample);
        }
        for(const auto& frame:frames)D::SetLiveGpuCoverage(frame,true);
        TimestampCalibration calibration;
        if(GetTimestampCalibration(calibration))pending.calibration=calibration;
        if(track)
        {
            live->pending.push_back(std::move(pending));live->pendingBytes+=live->pending.back().bytes;
            for(const auto& sample:live->pending.back().samples)
                if(sample.slot!=UINT32_MAX)live->untrackedCompletions[sample.slot]=0;
        }
        live->workPending=true;
#if !defined(ENABLE_METAL)
        if(!live->worker.joinable())live->worker=std::thread([this,live]
        {
            while(!live->stopping)
            {
                std::unique_lock<std::mutex> lock(live->wakeMutex);
                if(!live->workPending)live->wake.wait(lock,[&]{return live->stopping||live->workPending;});
                else live->wake.wait_for(lock,std::chrono::milliseconds(10),[&]{return live->stopping.load();});
                lock.unlock();if(!live->stopping)PumpLiveWork();
            }
        });
        live->wake.notify_one();
#endif
        PublishLiveResources();
    }
    catch(...)
    {
        for(const auto& sample:pending.samples)
        {
            auto span=sample.span;span.outcome=D::LiveOutcome::Unknown;span.duration.state=D::LiveValueState::Unavailable;
            D::RecordLiveGpuSpan(span);
        }
        for(uint32_t i=0;i<count;++i)
        {
            const auto found=live->prepared.find(commands[i]);
            if(found!=live->prepared.end())
            {live->untrackedCompletions[found->second.slot]=completion;live->prepared.erase(found);}
        }
    }
#else
    (void)commands;(void)count;(void)success;(void)completion;
#endif
}

void IDevice::PumpLiveWork() noexcept
{
#ifdef DY_LIVE_PROFILING_ENABLED
    try
    {
        std::vector<D::LiveEvent> events;
        std::vector<D::LiveGpuSpan> spans;
        {
            std::lock_guard<std::recursive_mutex> lock(m_resourceMutex);
            if(!m_live||m_live->stopping)return;
            auto& live=*m_live;
            const auto completed=GetCompletedSubmissionNative();const bool lost=IsLostNative();
            const auto observedNs=D::LiveNowNs();
            for(auto& entry:live.submissions)
                if(!entry.second.completedNs&&(lost||entry.first<=completed))entry.second.completedNs=observedNs;
            for(uint32_t slot=0;slot<LiveState::QuerySlots;++slot)
                if(live.untrackedCompletions[slot]&&(lost||live.untrackedCompletions[slot]<=completed))
                {live.untrackedCompletions[slot]=0;live.reserved[slot]=false;}
            for(auto it=live.pending.begin();it!=live.pending.end();)
            {
                auto& pending=*it;
                if(!lost&&pending.completion>completed){++it;continue;}
                for(auto event:pending.lists)
                {event.id=D::AllocateLiveId();event.outcome=lost?D::LiveOutcome::Unknown:D::LiveOutcome::Completed;
                    event.beginNs=event.endNs=D::LiveNowNs();events.push_back(event);}
                for(auto& sample:pending.samples)
                {
                    auto& span=sample.span;span.outcome=lost?D::LiveOutcome::Unknown:D::LiveOutcome::Completed;
                    if(sample.slot!=UINT32_MAX)
                    {
                        uint64_t ticks[2]{};const auto bits=GetTimestampValidBits();const auto period=GetTimestampPeriodNanoseconds();
                        if(!lost&&bits&&bits<=64&&period>0&&ReadTimestamps(live.query,sample.slot*2,2,ticks))
                        {
                            const uint64_t mask=bits==64?UINT64_MAX:(uint64_t{1}<<bits)-1;
                            const auto begin=ticks[0]&mask;const auto delta=(ticks[1]-ticks[0])&mask;
                            const long double wrapNs=std::ldexp(static_cast<long double>(period),bits);
                            if(bits==64||D::LiveNowNs()-pending.submittedNs<wrapNs)
                            {
                                span.duration={D::LiveValueState::Ready,double(delta)*period/1e6};
                                if(bits<64&&live.hasGpuBegin&&begin<live.lastGpuBegin)live.gpuWrapBase+=uint64_t{1}<<bits;
                                live.lastGpuBegin=begin;live.hasGpuBegin=true;
                                const long double beginNs=(static_cast<long double>(live.gpuWrapBase)+begin)*period;
                                span.hasQueueInterval=Nanoseconds(beginNs,span.beginGpuNs)&&Nanoseconds(beginNs+delta*period,span.endGpuNs);
                                if(pending.calibration&&pending.calibration->cpuDomain==CpuTimestampDomain::QueryPerformanceCounter&&
                                    D::GetLiveClockInfo().cpu==D::LiveCpuClock::QueryPerformanceCounter)
                                {
                                    const auto& calibration=*pending.calibration;long double offset;
                                    if(bits<64)
                                    {const auto distance=(begin-calibration.gpuTicks)&mask;const auto modulus=uint64_t{1}<<bits;
                                        offset=distance<=modulus/2?static_cast<long double>(distance):-static_cast<long double>(modulus-distance);}
                                    else offset=begin>=calibration.gpuTicks?static_cast<long double>(begin-calibration.gpuTicks):-static_cast<long double>(calibration.gpuTicks-begin);
                                    const long double cpuNs=static_cast<long double>(calibration.cpuTicks)*1e9L/calibration.cpuFrequency+offset*period;
                                    if(bits==64||D::LiveNowNs()-pending.submittedNs<wrapNs/2)
                                        span.hasCpuInterval=Nanoseconds(cpuNs,span.beginCpuNs)&&Nanoseconds(cpuNs+delta*period,span.endCpuNs);
                                    span.cpuCalibrationErrorKnown=calibration.maximumDeviationNanoseconds.has_value();
                                    if(span.cpuCalibrationErrorKnown)span.cpuCalibrationErrorNs=*calibration.maximumDeviationNanoseconds;
                                }
                            }
                            else span.duration.state=D::LiveValueState::Unavailable;
                        }
                        else span.duration.state=D::LiveValueState::Unavailable;
                        live.reserved[sample.slot]=false;
                    }
                    spans.push_back(span);
                }
                live.pendingBytes-=pending.bytes;it=live.pending.erase(it);
            }
            live.workPending=!live.pending.empty()||std::any_of(live.submissions.begin(),live.submissions.end(),
                [](const auto& entry){return entry.second.completedNs==0;});
        }
        // Collector publication never holds the device lock used by the rendering thread.
        for(const auto& event:events)D::RecordLiveEvent(event);
        for(const auto& span:spans)D::RecordLiveGpuSpan(span);
    }
    catch(...){}
#endif
}

void IDevice::StopLiveProfiling() noexcept
{
#ifdef DY_LIVE_PROFILING_ENABLED
    if(!m_live)return;
    PumpLiveWork();m_live->stopping=true;m_live->wake.notify_all();
    if(m_live->worker.joinable())m_live->worker.join();
    try
    {
        std::lock_guard<std::recursive_mutex> lock(m_resourceMutex);
        m_live->uploadControls.clear();
        for(const auto& pending:m_live->pending)for(auto sample:pending.samples)
        {sample.span.outcome=D::LiveOutcome::Unknown;sample.span.duration.state=D::LiveValueState::Unavailable;D::RecordLiveGpuSpan(sample.span);}
        m_live->pending.clear();m_live->prepared.clear();
        if(m_live->query){DestroyTimestampQuery(m_live->query);m_live->query=nullptr;}
        if(m_live->hasFrame){D::EndLiveFrame(m_live->frame,D::LiveOutcome::Failed);m_live->hasFrame=false;}
    }
    catch(...){}
#endif
}
}
