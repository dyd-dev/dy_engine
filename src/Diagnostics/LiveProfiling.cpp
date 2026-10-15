#include "LiveInternal.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <deque>
#include <limits>
#include <map>
#include <mutex>
#include <new>
#include <random>
#include <set>
#include <thread>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <utility>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace dyf::Diagnostics
{
namespace
{
#ifdef DY_LIVE_PROFILING_ENABLED
std::atomic<bool> enabled{true};
#else
std::atomic<bool> enabled{false};
#endif
std::atomic<LiveId> nextId{1};
thread_local LiveContext context;
constexpr std::uint64_t DetailBudget=32u*1024u*1024u;
constexpr std::uint64_t OptionalDetailBudget=24u*1024u*1024u;
std::atomic<std::uint64_t> detailBytes{0};
#ifdef DY_LIVE_TEST_CLOCK
std::atomic<std::uint64_t> testNowNs{0};
std::atomic<std::int64_t> testAllocationFailure{-1};
#endif
void AggregationAllocationPoint()
{
#ifdef DY_LIVE_TEST_CLOCK
    auto remaining=testAllocationFailure.load();
    while(remaining>=0)if(testAllocationFailure.compare_exchange_weak(remaining,remaining?remaining-1:-1)){if(!remaining)throw std::bad_alloc();break;}
#endif
}
bool ReserveDetail(std::size_t bytes,std::uint64_t budget=DetailBudget){auto current=detailBytes.load();while(current<=budget&&bytes<=budget-current){if(detailBytes.compare_exchange_weak(current,current+bytes))return true;}return false;}
struct SourceRun;
struct EvidenceRecord {std::mutex mutex;LiveFrameEvidence value;std::size_t retainedBytes=0;~EvidenceRecord(){detailBytes-=retainedBytes;}};
struct FrameRecord { std::mutex mutex; LiveFrameSummary summary; LiveFlowSnapshot flow; std::shared_ptr<const LiveFlowSnapshot> publishedFlow; std::shared_ptr<SourceRun> run; std::weak_ptr<EvidenceRecord> evidence; bool ended=false,retry=false,acquireSkipped=false,settingsNotified=false,coverage=false,coverageLost=false; std::uint64_t attemptBeginNs=0,cpuNs=0,omittedGpuSpans=0;std::size_t retainedBytes=0;std::set<std::tuple<LiveId,LiveId,std::uint64_t,LiveOutcome>> counted;std::map<std::tuple<LiveId,LiveId,std::uint64_t>,LiveCounters<std::uint64_t>> submittedCounts;~FrameRecord(){detailBytes-=retainedBytes;} };
void RepairEvidence(FrameRecord& frame)
{
    if(auto record=frame.evidence.lock()){std::lock_guard<std::mutex> lock(record->mutex);auto& value=record->value;if(value.summary.key!=frame.summary.key)return;value.summary=frame.summary;value.flow.gpuSpans.assign(frame.flow.gpuSpans.begin(),frame.flow.gpuSpans.end());value.loss.gpuSpans=frame.omittedGpuSpans;if(value.available&&(value.windowRevision!=frame.summary.appliedRevision||std::any_of(value.flow.gpuSpans.begin(),value.flow.gpuSpans.end(),[&](const auto& span){return span.frame!=value.summary.key||span.appliedRevision!=value.summary.appliedRevision;}))){value.available=false;++value.loss.frames;}++value.flow.revision;}
}
void AppendEvidenceDetail(FrameRecord& frame,const LiveEvent* event,const LiveFlowLink* link)
{
    if(auto record=frame.evidence.lock()){std::lock_guard<std::mutex> lock(record->mutex);auto& value=record->value;
        if(event&&(event->context.frame!=value.summary.key||event->context.appliedRevision!=value.summary.appliedRevision)){if(value.available){value.available=false;++value.loss.frames;}return;}
        if(event){if(value.flow.events.size()<2048)value.flow.events.push_back(*event);else{++value.loss.events;++value.flow.omittedEvents;value.budgetLimited=true;}}
        if(link){if(value.flow.links.size()<4096)value.flow.links.push_back(*link);else{++value.loss.links;++value.flow.omittedLinks;value.budgetLimited=true;}}
        ++value.flow.revision;
    }
}
struct EvidenceCandidate {LiveFrameKey key;std::uint64_t beginNs=0,endNs=0;LiveId revision=0;bool budgetLimited=false;std::shared_ptr<EvidenceRecord> record;};
template<class T>bool PushDetail(FrameRecord& frame,std::vector<T>& values,const T& value,std::size_t limit)
{
    if(values.size()>=limit)return false;
    if(values.size()==values.capacity()){const auto capacity=(std::min)(limit,(std::max)(std::size_t(16),values.capacity()*2));const auto extra=(capacity-values.capacity())*sizeof(T);if(!ReserveDetail(extra,std::is_same_v<T,LiveGpuSpan>?DetailBudget:OptionalDetailBudget))return false;try{values.reserve(capacity);frame.retainedBytes+=extra;}catch(...){detailBytes-=extra;throw;}}
    values.push_back(value);return true;
}
bool AppendLateDetail(const std::shared_ptr<FrameRecord>& frame,const LiveEvent* event,const LiveFlowLink* link)
{
    for(unsigned attempt=0;attempt<4;++attempt){
        std::shared_ptr<const LiveFlowSnapshot> base;{std::lock_guard<std::mutex> lock(frame->mutex);base=frame->publishedFlow;}
        if(!base)break;
        auto copy=std::make_shared<LiveFlowSnapshot>(*base);
        if(event){if(copy->events.size()>=8192)break;copy->events.push_back(*event);}
        if(link){if(copy->links.size()>=16384)break;copy->links.push_back(*link);}
        ++copy->revision;
        const auto oldBytes=base->events.capacity()*sizeof(LiveEvent)+base->links.capacity()*sizeof(LiveFlowLink);
        const auto newBytes=copy->events.capacity()*sizeof(LiveEvent)+copy->links.capacity()*sizeof(LiveFlowLink);
        std::lock_guard<std::mutex> lock(frame->mutex);if(frame->publishedFlow!=base)continue;
        if(newBytes>oldBytes&&!ReserveDetail(newBytes-oldBytes,OptionalDetailBudget))break;
        if(newBytes<oldBytes){detailBytes-=oldBytes-newBytes;frame->retainedBytes-=oldBytes-newBytes;}else frame->retainedBytes+=newBytes-oldBytes;
        frame->publishedFlow=std::move(copy);return true;
    }
    std::lock_guard<std::mutex> lock(frame->mutex);if(event)++frame->flow.omittedEvents;if(link)++frame->flow.omittedLinks;frame->summary.incomplete=true;return false;
}
struct SourceRecord { LiveId device=0,revision=1; std::uint64_t retiredNs=0,lastEndedNs=0;std::deque<std::shared_ptr<FrameRecord>> frames; std::shared_ptr<SourceRun> run; std::weak_ptr<FrameRecord> lastEnded;LiveFrameKey lastEndedKey;LiveAggregate baseline;EvidenceCandidate candidate,previousCandidate,beforeEvidence,afterEvidence;LiveId evidenceRequest=0; };
struct BatchRecord { LiveId id=0;std::uint64_t lastNs=0; };
thread_local std::shared_ptr<FrameRecord> currentFrame;
LiveSessionId NewSession(){std::random_device random;return {(std::uint64_t(random())<<32)^random(),(std::uint64_t(random())<<32)^random()};}
struct ControlSlot
{
    ControlDesc desc;
    ControlHandler handler;
    std::thread::id ownerThread;
    std::mutex invocation;
    std::atomic<bool> active{true};
    bool pending=false;
    LiveId executing=0;
};
struct ControlInvocation;
thread_local ControlInvocation* activeInvocation=nullptr;
struct ControlInvocation
{
    ControlSlot& slot;
    ControlInvocation* previous=activeInvocation;
    explicit ControlInvocation(ControlSlot& value):slot(value){activeInvocation=this;}
    ~ControlInvocation(){activeInvocation=previous;if(!slot.active)slot.handler={};}
    static bool Contains(const ControlSlot* value) noexcept
    {for(auto* call=activeInvocation;call;call=call->previous)if(&call->slot==value)return true;return false;}
};
struct State
{
    std::mutex mutex;
    std::mutex commandMutex;
    LiveSessionId session=NewSession();
    std::uint64_t sequence=0;
    std::unordered_map<const void*,LiveId> identities;
    std::unordered_map<LiveId,std::shared_ptr<const LiveObjectInfo>> objects;
    std::map<LiveId,LiveDeviceSnapshot> devices;
    std::map<LiveId,SourceRecord> sources;
    std::map<std::pair<LiveId,std::uint64_t>,BatchRecord> batches;
    std::uint64_t nextBatchSweepNs=0;
    std::map<LiveId,std::shared_ptr<ControlSlot>> controls;
    std::deque<ControlStatus> requests;
    std::map<LiveId,ControlValue> expectedRequests;
    std::map<std::pair<LiveId,std::uint64_t>,LiveCounters<std::uint64_t>> commandCounts;
    std::atomic<std::uint64_t> omittedEvents{0},omittedLinks{0},omittedFrames{0},omittedSpans{0};
    LiveLoss loss;
    std::size_t frameCount=0;
};
State& S() { static State value;return value; }
bool Matches(const EvidenceCandidate& candidate,const LiveAggregate& aggregate)
{return aggregate.frameCount&&!aggregate.mixedRevision&&candidate.key.frame&&candidate.beginNs==aggregate.beginNs&&candidate.endNs==aggregate.endNs&&candidate.revision==aggregate.appliedRevision;}
EvidenceCandidate SelectEvidence(const SourceRecord& source,const LiveAggregate& aggregate)
{if(Matches(source.candidate,aggregate))return source.candidate;if(Matches(source.previousCandidate,aggregate))return source.previousCandidate;return {};}
void CaptureEvidence(State& state,SourceRecord& source,FrameRecord& frame,std::uint64_t origin) noexcept
{
    if(source.retiredNs)return;
    const auto begin=origin+(frame.summary.endNs-origin)/LiveAverageWindowNs*LiveAverageWindowNs;
    if(source.candidate.key.frame&&source.candidate.beginNs==begin)return;
    source.previousCandidate=std::move(source.candidate);source.candidate={frame.summary.key,begin,begin+LiveAverageWindowNs,frame.summary.appliedRevision,true,{}};
    // One independently owned prefix per source/second; this reservation includes all retained capacities.
    constexpr std::size_t reservation=1024u*1024u,eventLimit=2048,linkLimit=4096,objectLimit=128;
    static_assert(sizeof(EvidenceRecord)+eventLimit*sizeof(LiveEvent)+linkLimit*sizeof(LiveFlowLink)+128*sizeof(LiveGpuSpan)+objectLimit*(sizeof(LiveObjectInfo)+257)<reservation);
    try{
        auto record=std::make_shared<EvidenceRecord>();if(!ReserveDetail(reservation,OptionalDetailBudget))return;record->retainedBytes=reservation;
        auto& value=record->value;value.available=true;value.summary=frame.summary;value.windowBeginNs=begin;value.windowEndNs=begin+LiveAverageWindowNs;value.windowRevision=frame.summary.appliedRevision;value.flow.key=frame.summary.key;
        const auto& flow=*frame.publishedFlow;const auto events=(std::min)(eventLimit,flow.events.size()),links=(std::min)(linkLimit,flow.links.size());
        value.flow.events.reserve(eventLimit);value.flow.links.reserve(linkLimit);value.flow.events.assign(flow.events.begin(),flow.events.begin()+events);value.flow.links.assign(flow.links.begin(),flow.links.begin()+links);value.flow.revision=flow.revision;
        value.flow.omittedEvents=flow.omittedEvents+flow.events.size()-events;value.flow.omittedLinks=flow.omittedLinks+flow.links.size()-links;
        value.loss.events=value.flow.omittedEvents;value.loss.links=value.flow.omittedLinks;value.loss.gpuSpans=frame.omittedGpuSpans;value.budgetLimited=events!=flow.events.size()||links!=flow.links.size();
        value.flow.gpuSpans.reserve(128);value.flow.gpuSpans.assign(frame.flow.gpuSpans.begin(),frame.flow.gpuSpans.end());
        if(std::any_of(value.flow.events.begin(),value.flow.events.end(),[&](const auto& event){return event.context.frame!=frame.summary.key||event.context.appliedRevision!=frame.summary.appliedRevision;})||std::any_of(value.flow.gpuSpans.begin(),value.flow.gpuSpans.end(),[&](const auto& span){return span.frame!=frame.summary.key||span.appliedRevision!=frame.summary.appliedRevision;})){source.candidate.budgetLimited=false;return;}
        std::set<LiveId> references,required;const auto add=[&](LiveId id){if(id)references.insert(id);};const auto resource=[&](LiveId id){add(id);if(id)required.insert(id);};add(frame.summary.key.device);add(frame.summary.key.source);
        for(const auto& event:value.flow.events){add(event.context.owner);add(event.context.object);resource(event.resource);resource(event.command.commandList);}
        for(const auto& span:value.flow.gpuSpans){add(span.owner);add(span.begin.command.commandList);add(span.end.command.commandList);}
        value.objects.reserve(objectLimit);
        for(const auto id:references){const auto it=state.objects.find(id);if(it==state.objects.end()){if(required.count(id))++value.loss.inventory;continue;}if(value.objects.size()==objectLimit){++value.loss.inventory;value.budgetLimited=true;continue;}const auto& desc=it->second->desc;if(desc.label.size()>256){++value.loss.inventory;value.budgetLimited=true;}value.objects.push_back({id,{desc.kind,desc.device,desc.owner,desc.elements,desc.bytes,desc.label.substr(0,256)}});}
        auto retained=sizeof(EvidenceRecord)+value.flow.events.capacity()*sizeof(LiveEvent)+value.flow.links.capacity()*sizeof(LiveFlowLink)+value.flow.gpuSpans.capacity()*sizeof(LiveGpuSpan)+value.objects.capacity()*sizeof(LiveObjectInfo);for(const auto& object:value.objects)retained+=object.desc.label.capacity()+1;if(retained>reservation)return;
        source.candidate.budgetLimited=value.budgetLimited;source.candidate.record=record;frame.evidence=record;
    }catch(...){}
}
LiveFrameEvidence CopyEvidence(const EvidenceCandidate& candidate,const LiveAggregate& aggregate,std::uint64_t now)
{
    LiveFrameEvidence value;if(!Matches(candidate,aggregate))return value;
    if(candidate.record){std::lock_guard<std::mutex> lock(candidate.record->mutex);value=candidate.record->value;value.summary.sampleAgeNs=now>=value.summary.endNs?now-value.summary.endNs:0;}
    else{value.budgetLimited=candidate.budgetLimited;value.summary.key=value.flow.key=candidate.key;value.summary.appliedRevision=value.windowRevision=candidate.revision;value.windowBeginNs=candidate.beginNs;value.windowEndNs=candidate.endNs;value.loss.frames=1;}
    return value;
}
#define LIVE_COUNTER_FIELDS(X) X(draw) X(drawIndexed) X(dispatch) X(dispatchMesh) X(instances) X(barriers) X(bindings) X(submissions) X(waits) X(objectsCreated) X(objectsDestroyed) X(batches) X(elements) X(dirtyRanges) X(uploadCalls) X(uploadBytes) X(fullUploads) X(partialUploads) X(uploadPageAllocations) X(uploadPageReuses) X(uploadPageAllocationBytes)
void ClearNativeUploadCounts(LiveCounters<std::uint64_t>& counts)
{counts.uploadPageAllocations=counts.uploadPageReuses=counts.uploadPageAllocationBytes=0;}
void Add(LiveCounters<std::uint64_t>& a,const LiveCounters<std::uint64_t>& b)
{
#define ADD(n) a.n+=b.n;
    LIVE_COUNTER_FIELDS(ADD)
#undef ADD
}
LiveCounters<double> Mean(const LiveCounters<std::uint64_t>& value,std::uint64_t count)
{
    LiveCounters<double> result;
    if(!count)return result;
#define MEAN(n) result.n=double(value.n)/double(count);
    LIVE_COUNTER_FIELDS(MEAN)
#undef MEAN
    return result;
}
void Accumulate(LiveMetricAggregate& a,LiveSample sample,LiveFrameKey key)
{
    switch(sample.state) {
    case LiveValueState::Ready:
        if(!std::isfinite(sample.milliseconds)||sample.milliseconds<0){++a.unavailable;break;}
        a.average+=sample.milliseconds;a.maximum=(std::max)(a.maximum,sample.milliseconds);++a.ready;a.lastReadyFrame=key;break;
    case LiveValueState::Pending:++a.pending;break;
    case LiveValueState::Unsupported:++a.unsupported;break;
    default:++a.unavailable;break;
    }
}
void Finish(LiveMetricAggregate& a)
{
    if(a.ready){a.average/=double(a.ready);a.state=LiveValueState::Ready;}
    else if(a.pending)a.state=LiveValueState::Pending;
    else if(a.unsupported && !a.unavailable)a.state=LiveValueState::Unsupported;
}
LiveAggregate Finish(LiveAggregate a)
{
    Finish(a.frame);Finish(a.cpuRender);Finish(a.gpuMain);Finish(a.gpuShadow);Finish(a.gpuMeasured);Finish(a.objects);
    if(a.frame.ready&&a.frame.average>0)a.fps=1000.0/a.frame.average;
    for(auto& stage:a.stages){Finish(stage.cpu);Finish(stage.gpu);if(a.frameCount){stage.meanElements=double(stage.totalElements)/a.frameCount;stage.meanBytes=double(stage.totalBytes)/a.frameCount;stage.meanOperations=double(stage.totalOperations)/a.frameCount;}}
    a.recordedMean=Mean(a.recordedTotal,a.frameCount);a.submittedMean=Mean(a.submittedTotal,a.frameCount);a.completedMean=Mean(a.completedTotal,a.frameCount);
    return a;
}
constexpr std::size_t MetricCount=6+2*LiveStageCount;
auto Metrics(LiveAggregate& value)
{
    std::array<LiveMetricAggregate*,MetricCount> result{&value.frame,&value.cpuRender,&value.gpuMain,&value.gpuShadow,&value.gpuMeasured,&value.objects};
    for(std::size_t i=0;i<LiveStageCount;++i){result[6+2*i]=&value.stages[i].cpu;result[7+2*i]=&value.stages[i].gpu;}return result;
}
auto Samples(const LiveFrameSummary& value)
{
    std::array<LiveSample,MetricCount> result{value.frame,value.cpuRender,value.gpuMain,value.gpuShadow,value.gpuMeasured,LiveSample{value.objects.state,double(value.objects.count)}};
    for(std::size_t i=0;i<LiveStageCount;++i){result[6+2*i]=value.stages[i].cpu;result[7+2*i]=value.stages[i].gpu;}return result;
}
struct MetricValues
{
    std::map<LiveId,double> frames;
    std::multiset<double> maxima;
    double frozenMaximum=0;
    LiveFrameKey frozenLast;
    void Remove(LiveId frame) {auto it=frames.find(frame);if(it!=frames.end()){auto maximum=maxima.find(it->second);if(maximum!=maxima.end())maxima.erase(maximum);frames.erase(it);}}
};
struct SecondRecord {LiveAggregate total;std::array<MetricValues,MetricCount> values;};
struct SourceRun
{
    std::mutex mutex;
    LiveId device=0,source=0;
    std::uint64_t originNs=0;
    std::map<std::uint64_t,SecondRecord> seconds;
    // Allocation-free tombstones cover every mutable second, including a failed first insertion.
    std::array<std::uint64_t,LiveHistoryWindowNs/LiveAverageWindowNs+1> invalidSeconds{};
    LiveRunRanges frozen;
    LiveFrameSummary latest;
};
bool Invalid(const SourceRun& run,std::uint64_t index) {return run.invalidSeconds[index%run.invalidSeconds.size()]==index+1;}
void Range(LiveMetricRange& range,double value)
{
    if(!std::isfinite(value)||value<0)return;
    if(!range.windows)range.low=range.high=value;else{range.low=(std::min)(range.low,value);range.high=(std::max)(range.high,value);}
    ++range.windows;range.state=LiveValueState::Ready;
}
void Ranges(LiveRunRanges& ranges,const LiveAggregate& value)
{
    if(value.frame.state==LiveValueState::Ready){Range(ranges.frame,value.frame.average);if(value.frame.average>0)Range(ranges.fps,value.fps);}
    if(value.cpuRender.state==LiveValueState::Ready)Range(ranges.cpuRender,value.cpuRender.average);
    const auto& gpu=value.gpuMeasured;
    if(gpu.state==LiveValueState::Ready&&!gpu.pending&&!gpu.unavailable&&!gpu.unsupported)Range(ranges.gpuMeasured,gpu.average);
    if(value.objects.state==LiveValueState::Ready)Range(ranges.objects,value.objects.average);
    if(value.frameCount){Range(ranges.drawCalls,value.recordedMean.draw+value.recordedMean.drawIndexed);Range(ranges.instances,value.recordedMean.instances);Range(ranges.batches,value.recordedMean.batches);Range(ranges.uploadBytes,value.recordedMean.uploadBytes);}
}
void Advance(SourceRun& run,std::uint64_t now)
{
    const auto cutoff=now>LiveHistoryWindowNs?now-LiveHistoryWindowNs:0;
    while(!run.seconds.empty()&&run.seconds.begin()->second.total.endNs<=cutoff){if(!Invalid(run,run.seconds.begin()->first))Ranges(run.frozen,Finish(run.seconds.begin()->second.total));run.seconds.erase(run.seconds.begin());}
}
void Change(LiveMetricAggregate& aggregate,MetricValues& values,LiveSample sample,LiveFrameKey key,bool adding)
{
    if(sample.state==LiveValueState::Ready&&(!std::isfinite(sample.milliseconds)||sample.milliseconds<0))sample.state=LiveValueState::Unavailable;
    auto& count=sample.state==LiveValueState::Ready?aggregate.ready:sample.state==LiveValueState::Pending?aggregate.pending:sample.state==LiveValueState::Unsupported?aggregate.unsupported:aggregate.unavailable;
    if(adding)++count;else --count;
    if(sample.state==LiveValueState::Ready){
        aggregate.average+=adding?sample.milliseconds:-sample.milliseconds;
        if(adding){AggregationAllocationPoint();values.frames.emplace(key.frame,sample.milliseconds);AggregationAllocationPoint();values.maxima.insert(sample.milliseconds);}else values.Remove(key.frame);
        if(!aggregate.ready)aggregate.average=0;
    }
    aggregate.maximum=(std::max)(values.frozenMaximum,values.maxima.empty()?0:*values.maxima.rbegin());
    aggregate.lastReadyFrame=values.frozenLast;
    if(!values.frames.empty()&&values.frames.rbegin()->first>aggregate.lastReadyFrame.frame){aggregate.lastReadyFrame=key;aggregate.lastReadyFrame.frame=values.frames.rbegin()->first;}
}
void Change(LiveCounters<std::uint64_t>& aggregate,const LiveCounters<std::uint64_t>& value,bool adding)
{
#define CHANGE(n) if(adding)aggregate.n+=value.n;else aggregate.n-=value.n;
    LIVE_COUNTER_FIELDS(CHANGE)
#undef CHANGE
}
void Change(SecondRecord& second,const LiveFrameSummary& frame,bool adding)
{
    auto& total=second.total;if(adding)++total.frameCount;else --total.frameCount;
    const auto samples=Samples(frame);const auto metrics=Metrics(total);
    for(std::size_t i=0;i<MetricCount;++i)Change(*metrics[i],second.values[i],samples[i],frame.key,adding);
    Change(total.recordedTotal,frame.recorded,adding);Change(total.submittedTotal,frame.submitted,adding);Change(total.completedTotal,frame.completed,adding);
    for(std::size_t i=0;i<LiveStageCount;++i){auto& to=total.stages[i];const auto& from=frame.stages[i];if(adding){to.totalElements+=from.elements;to.totalBytes+=from.bytes;to.totalOperations+=from.operations;}else{to.totalElements-=from.elements;to.totalBytes-=from.bytes;to.totalOperations-=from.operations;}}
}
// Call with only the frame mutex held (or state -> frame); never acquire state from run.
void UpdateSecond(FrameRecord& frame,const LiveFrameSummary* before=nullptr)
{
    if(!frame.ended||!frame.run)return;
    auto& run=*frame.run;std::lock_guard<std::mutex> lock(run.mutex);
    const auto index=(frame.summary.endNs-run.originNs)/LiveAverageWindowNs;
    auto it=run.seconds.find(index);
    if(!before||run.latest.key==frame.summary.key)run.latest=frame.summary;
    if(Invalid(run,index))return;
    if(before&&it==run.seconds.end())return;
    try {
    if(it==run.seconds.end()){
        AggregationAllocationPoint();it=run.seconds.emplace(index,SecondRecord{}).first;auto& total=it->second.total;
        total.device=run.device;total.source=run.source;total.appliedRevision=frame.summary.appliedRevision;
        total.beginNs=run.originNs+index*LiveAverageWindowNs;total.endNs=total.beginNs+LiveAverageWindowNs;
    }
    auto& second=it->second;
    if(before)Change(second,*before,false);
    if(second.total.appliedRevision!=frame.summary.appliedRevision){second.total.appliedRevision=0;second.total.mixedRevision=true;}
    Change(second,frame.summary,true);
    Advance(run,LiveNowNs());
    } catch(...) {
        run.invalidSeconds[index%run.invalidSeconds.size()]=index+1;
        run.seconds.erase(index);++S().omittedFrames;
    }
}
struct LateSecondUpdate
{
    FrameRecord& frame;
    std::optional<LiveFrameSummary> before;
    explicit LateSecondUpdate(FrameRecord& value):frame(value){if(frame.ended&&frame.run)before=frame.summary;}
    ~LateSecondUpdate(){if(before){try{UpdateSecond(frame,&*before);}catch(...){++S().omittedFrames;}try{RepairEvidence(frame);}catch(...){++S().omittedSpans;}}}
};
void EvictFrame(const std::shared_ptr<FrameRecord>& frame)
{
    std::lock_guard<std::mutex> lock(frame->mutex);if(!frame->ended||!frame->run)return;
    const auto before=frame->summary;std::uint64_t lost=0;
    auto abandon=[&](LiveSample& sample){if(sample.state==LiveValueState::Pending){sample={};++lost;}};
    abandon(frame->summary.gpuMain);abandon(frame->summary.gpuShadow);abandon(frame->summary.gpuMeasured);
    for(auto& stage:frame->summary.stages)abandon(stage.gpu);
    if(lost){frame->summary.incomplete=true;frame->summary.gpuCoverageComplete=false;UpdateSecond(*frame,&before);++S().omittedSpans;}
    for(auto& span:frame->flow.gpuSpans)if(span.duration.state==LiveValueState::Pending){span.duration={};span.outcome=LiveOutcome::Unknown;++frame->omittedGpuSpans;}
    RepairEvidence(*frame);
    auto run=frame->run;std::lock_guard<std::mutex> guard(run->mutex);
    if(run->latest.key==frame->summary.key)run->latest=frame->summary;
    const auto index=(frame->summary.endNs-run->originNs)/LiveAverageWindowNs;
    const auto it=run->seconds.find(index);
    if(it!=run->seconds.end())for(auto& values:it->second.values){auto value=values.frames.find(frame->summary.key.frame);if(value!=values.frames.end()){values.frozenMaximum=(std::max)(values.frozenMaximum,value->second);if(frame->summary.key.frame>values.frozenLast.frame)values.frozenLast=frame->summary.key;values.Remove(frame->summary.key.frame);}}
    frame->run.reset();
}
void ReadRun(SourceRun& run,std::uint64_t now,LiveSourceSnapshot& out)
{
    std::lock_guard<std::mutex> lock(run.mutex);Advance(run,now);out.runRanges=run.frozen;out.latest=run.latest;
    const auto elapsed=now>=run.originNs?now-run.originNs:0;
    out.recent.device=run.device;out.recent.source=run.source;
    if(elapsed>=LiveAverageWindowNs){
        const auto index=elapsed/LiveAverageWindowNs-1;out.recent.beginNs=run.originNs+index*LiveAverageWindowNs;out.recent.endNs=out.recent.beginNs+LiveAverageWindowNs;
        const auto it=run.seconds.find(index);if(it!=run.seconds.end()&&!Invalid(run,index))out.recent=Finish(it->second.total);
    }
    for(const auto& second:run.seconds)if(second.second.total.endNs<=now&&!Invalid(run,second.first)){auto completed=Finish(second.second.total);Ranges(out.runRanges,completed);out.recentSeconds.push_back(std::move(completed));}
}
LiveAggregate UnavailableSecond(const LiveAggregate& value)
{
    LiveAggregate result;result.device=value.device;result.source=value.source;result.appliedRevision=value.appliedRevision;
    result.beginNs=value.beginNs;result.endNs=value.endNs;return result;
}
LiveAggregate CompletedSecond(SourceRun& run,LiveId revision,std::uint64_t now)
{
    LiveAggregate result;result.device=run.device;result.source=run.source;result.appliedRevision=revision;
    if(now<run.originNs||now-run.originNs<LiveAverageWindowNs)return result;
    const auto index=(now-run.originNs)/LiveAverageWindowNs-1;
    result.beginNs=run.originNs+index*LiveAverageWindowNs;result.endNs=result.beginNs+LiveAverageWindowNs;
    const auto it=run.seconds.find(index);
    if(it!=run.seconds.end()&&!Invalid(run,index)&&it->second.total.frameCount&&!it->second.total.mixedRevision&&it->second.total.appliedRevision==revision)return Finish(it->second.total);
    return result;
}
LiveAggregate CompletedSecond(const SourceRecord& source,LiveId revision,std::uint64_t now)
{
    if(!source.run)return {};
    std::lock_guard<std::mutex> lock(source.run->mutex);return CompletedSecond(*source.run,revision,now);
}
void RefreshComparison(SourceRun& run,LiveAggregate& value)
{
    if(!value.frameCount||value.beginNs<run.originNs||value.endNs-value.beginNs!=LiveAverageWindowNs)return;
    const auto index=(value.beginNs-run.originNs)/LiveAverageWindowNs;const auto it=run.seconds.find(index);
    if(Invalid(run,index))value=UnavailableSecond(value);
    else if(it!=run.seconds.end()&&!it->second.total.mixedRevision&&it->second.total.appliedRevision==value.appliedRevision)value=Finish(it->second.total);
}
// State -> run only; retained request DTOs preserve old revisions across reconnect.
void UpdateControlComparisons(State& state,LiveId id,SourceRecord& source,std::uint64_t now)
{
    if(!source.run)return;
    std::lock_guard<std::mutex> lock(source.run->mutex);auto& run=*source.run;
    const auto latest=CompletedSecond(run,source.revision,now);
    for(auto& request:state.requests)if(request.source==id&&request.state==ControlState::Applied){
        RefreshComparison(run,request.baseline);
        if(request.appliedRevision==source.revision)request.after=latest.beginNs>=request.appliedNs?latest:UnavailableSecond(latest);
        else RefreshComparison(run,request.after);
        if(request.request==source.evidenceRequest&&!Matches(source.afterEvidence,request.after))source.afterEvidence=SelectEvidence(source,request.after);
    }
}
LiveAggregate Aggregate(const SourceRecord& source,LiveId id,LiveId revision,std::uint64_t begin,std::uint64_t end)
{
    LiveAggregate a;a.device=source.device;a.source=id;a.appliedRevision=revision;a.beginNs=begin;a.endNs=end;
    for(const auto& record:source.frames) {
        std::lock_guard<std::mutex> lock(record->mutex);const auto& f=record->summary;
        if(!record->ended||f.appliedRevision!=revision||f.endNs<begin||f.endNs>=end)continue;
        ++a.frameCount;
        Accumulate(a.frame,f.frame,f.key);Accumulate(a.cpuRender,f.cpuRender,f.key);
        Accumulate(a.gpuMain,f.gpuMain,f.key);Accumulate(a.gpuShadow,f.gpuShadow,f.key);Accumulate(a.gpuMeasured,f.gpuMeasured,f.key);
        Accumulate(a.objects,{f.objects.state,double(f.objects.count)},f.key);
        Add(a.recordedTotal,f.recorded);Add(a.submittedTotal,f.submitted);Add(a.completedTotal,f.completed);
        for(std::size_t i=0;i<LiveStageCount;++i){auto& s=a.stages[i];const auto& v=f.stages[i];s.totalElements+=v.elements;s.totalBytes+=v.bytes;s.totalOperations+=v.operations;Accumulate(s.cpu,v.cpu,f.key);Accumulate(s.gpu,v.gpu,f.key);}
    }
    return Finish(std::move(a));
}
FrameRecord* FindFrame(State& s,LiveFrameKey key)
{
    auto source=s.sources.find(key.source);if(source==s.sources.end()||source->second.device!=key.device)return nullptr;
    for(auto it=source->second.frames.rbegin();it!=source->second.frames.rend();++it)if((*it)->summary.key==key)return it->get();
    return nullptr;
}
std::shared_ptr<FrameRecord> Frame(LiveFrameKey key)
{
    if(currentFrame&&currentFrame->summary.key==key)return currentFrame;
    auto& s=S();std::lock_guard<std::mutex> lock(s.mutex);auto it=s.sources.find(key.source);if(it==s.sources.end()||it->second.device!=key.device)return {};
    for(auto f=it->second.frames.rbegin();f!=it->second.frames.rend();++f)if((*f)->summary.key==key)return *f;
    return {};
}
ControlStatus* FindRequest(State& s,LiveId id)
{ for(auto& r:s.requests)if(r.request==id)return &r;return nullptr; }
bool Terminal(ControlState s) {return s==ControlState::Applied||s==ControlState::Rejected||s==ControlState::Failed||s==ControlState::Cancelled;}
void Unregister(LiveId id) noexcept
{
    if(!id)return;
    try {
        std::shared_ptr<ControlSlot> slot;
        {auto& s=S();std::lock_guard<std::mutex> lock(s.mutex);auto it=s.controls.find(id);if(it==s.controls.end())return;slot=it->second;slot->active=false;s.controls.erase(it);for(auto& r:s.requests)if(r.control==id&&!Terminal(r.state)){r.state=ControlState::Cancelled;r.message="Owner destroyed";s.expectedRequests.erase(r.request);}}
        // The active callback still owns its captures; clear them after it returns.
        if(ControlInvocation::Contains(slot.get()))return;
        std::lock_guard<std::mutex> invocation(slot->invocation);slot->handler={};
    }catch(...){}
}
bool Validate(const ControlDesc& d,const ControlValue& value,bool checkAvailability=true)
{
    if(checkAvailability&&(!d.writable||d.application==ControlApplication::RestartRequired||d.application==ControlApplication::RebuildRequired))return false;
    if(d.kind==ControlKind::Boolean)return std::holds_alternative<bool>(value);
    if(d.kind==ControlKind::Real){const auto* v=std::get_if<double>(&value);return v&&std::isfinite(*v)&&*v>=d.minimum&&*v<=d.maximum;}
    const auto* v=std::get_if<std::int64_t>(&value);if(!v)return false;
    if(d.kind==ControlKind::Enumeration)return std::any_of(d.choices.begin(),d.choices.end(),[&](const auto& c){return c.value==*v;});
    return double(*v)>=d.minimum&&double(*v)<=d.maximum;
}
void GpuSummary(FrameRecord& f)
{
    std::vector<std::pair<std::uint64_t,std::uint64_t>> intervals;
    LiveId domain=0;bool pending=false,unavailable=f.coverageLost,unsupported=false;
    f.summary.gpuMain={};f.summary.gpuShadow={};
    for(const auto& span:f.flow.gpuSpans) {
        if(span.appliedRevision!=f.summary.appliedRevision){if(span.aggregation==LiveGpuAggregation::FrameCoverage)unavailable=true;continue;}
        if(span.scope==LiveGpuScope::RendererMainForward)f.summary.gpuMain=span.duration;
        if(span.scope==LiveGpuScope::RendererShadow)f.summary.gpuShadow=span.duration;
        if(span.aggregation!=LiveGpuAggregation::FrameCoverage)continue;
        pending|=span.duration.state==LiveValueState::Pending;
        unsupported|=span.duration.state==LiveValueState::Unsupported;
        if(span.duration.state==LiveValueState::Unavailable)unavailable=true;
        if(span.duration.state==LiveValueState::Ready){
            if(!span.hasQueueInterval||!span.gpuClockDomain||span.endGpuNs<span.beginGpuNs||(domain&&domain!=span.gpuClockDomain))unavailable=true;
            else{domain=span.gpuClockDomain;intervals.emplace_back(span.beginGpuNs,span.endGpuNs);}
        }
    }
    f.summary.gpuCoverageComplete=f.coverage&&!pending&&!unavailable&&!unsupported&&!intervals.empty();
    f.summary.gpuMeasured={pending?LiveValueState::Pending:unsupported?LiveValueState::Unsupported:LiveValueState::Unavailable,0};
    if(f.summary.gpuCoverageComplete){
        std::sort(intervals.begin(),intervals.end());std::uint64_t total=0,begin=intervals[0].first,end=intervals[0].second;
        for(std::size_t i=1;i<intervals.size();++i){if(intervals[i].first<=end)end=(std::max)(end,intervals[i].second);else{total+=end-begin;begin=intervals[i].first;end=intervals[i].second;}}
        total+=end-begin;f.summary.gpuMeasured={LiveValueState::Ready,double(total)/1e6};
    }
    f.summary.stages[static_cast<std::size_t>(LiveStage::Gpu)].gpu=f.summary.gpuMeasured;
}
}

LiveClockInfo GetLiveClockInfo() noexcept
{
#ifdef _WIN32
    LARGE_INTEGER frequency{};QueryPerformanceFrequency(&frequency);return {LiveCpuClock::QueryPerformanceCounter,std::uint64_t(frequency.QuadPart),0,false};
#else
    return {LiveCpuClock::SteadyClock,1000000000,0,false};
#endif
}
std::uint64_t LiveNowNs() noexcept
{
#ifdef DY_LIVE_TEST_CLOCK
    if(const auto now=testNowNs.load())return now;
#endif
#ifdef _WIN32
    static const auto frequency=GetLiveClockInfo().frequency;LARGE_INTEGER value{};QueryPerformanceCounter(&value);
    const auto ticks=std::uint64_t(value.QuadPart);return (ticks/frequency)*1000000000ull+(ticks%frequency)*1000000000ull/frequency;
#else
    return std::uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
#endif
}
#ifdef DY_LIVE_TEST_CLOCK
void Detail::SetTestLiveNowNs(std::uint64_t now) noexcept {testNowNs=now;}
void Detail::FailTestLiveAggregationAllocationAfter(std::int64_t count) noexcept {testAllocationFailure=count;}
#endif
bool IsLiveCollectorEnabled() noexcept {return enabled.load(std::memory_order_relaxed);}
bool SetLiveCollectorEnabled(bool value) noexcept
{
#ifndef DY_LIVE_PROFILING_ENABLED
    if(value)return false;
#endif
    const bool wasEnabled=enabled.exchange(value);if(!value){Detail::StopServer();if(wasEnabled)try{std::vector<LiveFrameToken> retries;{auto& s=S();std::lock_guard<std::mutex> lock(s.mutex);for(const auto& source:s.sources)for(const auto& frame:source.second.frames){std::lock_guard<std::mutex> guard(frame->mutex);if(frame->retry&&!frame->ended)retries.push_back({frame->summary.key,true,context});}}for(const auto& token:retries)EndLiveFrame(token,LiveOutcome::Discarded);}catch(...) {}}return true;
}
LiveId AllocateLiveId() noexcept {return IsLiveCollectorEnabled()?nextId.fetch_add(1,std::memory_order_relaxed):0;}
LiveId RegisterLiveObject(const void* identity,const LiveObjectDesc& desc) noexcept
{
    if(!IsLiveCollectorEnabled())return 0;
    try {
        auto& s=S();LiveId id;
        {std::lock_guard<std::mutex> lock(s.mutex);if(identity){auto it=s.identities.find(identity);if(it!=s.identities.end())return it->second;}
            id=AllocateLiveId();auto copy=desc;if(copy.label.size()>1024)copy.label.resize(1024);
            s.objects.emplace(id,std::make_shared<LiveObjectInfo>(LiveObjectInfo{id,std::move(copy)}));if(identity)s.identities[identity]=id;
            if(auto* frame=FindFrame(s,context.frame)){std::lock_guard<std::mutex> guard(frame->mutex);LateSecondUpdate update(*frame);++frame->summary.recorded.objectsCreated;}
        }
        Detail::StartServer();return id;
    }catch(...){return 0;}
}
LiveId FindLiveObject(const void* identity) noexcept
{
    if(!IsLiveCollectorEnabled()||!identity)return 0;
    try{auto& s=S();std::lock_guard<std::mutex> lock(s.mutex);auto it=s.identities.find(identity);return it==s.identities.end()?0:it->second;}catch(...){return 0;}
}
void UpdateLiveObject(LiveId id,const LiveObjectDesc& desc) noexcept
{if(!id||!IsLiveCollectorEnabled())return;try{auto copy=std::make_shared<LiveObjectInfo>(LiveObjectInfo{id,desc});if(copy->desc.label.size()>1024)copy->desc.label.resize(1024);auto& s=S();std::lock_guard<std::mutex> lock(s.mutex);auto it=s.objects.find(id);if(it!=s.objects.end())it->second=std::move(copy);}catch(...) {}}
void RebindLiveObject(LiveId id,const void* identity) noexcept
{if(!id)return;try{auto& s=S();std::lock_guard<std::mutex> lock(s.mutex);for(auto it=s.identities.begin();it!=s.identities.end();)if(it->second==id)it=s.identities.erase(it);else++it;if(identity)s.identities[identity]=id;}catch(...) {}}
void RetireLiveObject(LiveId id) noexcept
{
    if(!id)return;try{auto& s=S();std::vector<LiveFrameToken> retries;{std::lock_guard<std::mutex> lock(s.mutex);const bool removed=s.objects.erase(id)!=0;s.devices.erase(id);for(auto it=s.identities.begin();it!=s.identities.end();)if(it->second==id)it=s.identities.erase(it);else++it;
        if(removed)if(auto* frame=FindFrame(s,context.frame)){std::lock_guard<std::mutex> guard(frame->mutex);LateSecondUpdate update(*frame);++frame->summary.recorded.objectsDestroyed;}
        std::vector<LiveId> retired{id};for(std::size_t cursor=0;cursor<retired.size();++cursor)for(auto it=s.batches.lower_bound({retired[cursor],0});it!=s.batches.end()&&it->first.first==retired[cursor];){retired.push_back(it->second.id);it=s.batches.erase(it);}
        const auto now=LiveNowNs();for(auto& pair:s.sources)if(pair.first==id||pair.second.device==id){pair.second.retiredNs=now;pair.second.candidate={};pair.second.previousCandidate={};for(const auto& frame:pair.second.frames){std::lock_guard<std::mutex> frameLock(frame->mutex);if(frame->retry&&!frame->ended)retries.push_back({frame->summary.key,true,context});}}
        {std::lock_guard<std::mutex> commands(s.commandMutex);for(auto it=s.commandCounts.begin();it!=s.commandCounts.end();)if(it->first.first==id)it=s.commandCounts.erase(it);else++it;}
    }for(const auto& token:retries)EndLiveFrame(token,LiveOutcome::Discarded);}catch(...) {}}
LiveId GetLiveBatchId(LiveId owner,std::uint64_t discriminator) noexcept
{if(!IsLiveCollectorEnabled()||!owner)return 0;try{auto& s=S();std::lock_guard<std::mutex> lock(s.mutex);const auto now=LiveNowNs();if(now>=s.nextBatchSweepNs){const auto cutoff=now>LiveHistoryWindowNs?now-LiveHistoryWindowNs:0;for(auto it=s.batches.begin();it!=s.batches.end();)if(it->second.lastNs<cutoff)it=s.batches.erase(it);else++it;s.nextBatchSweepNs=now+1000000000;}
    const auto key=std::make_pair(owner,discriminator);auto it=s.batches.find(key);if(it!=s.batches.end()){it->second.lastNs=now;return it->second.id;}if(s.batches.size()>=65536){++s.loss.inventory;return 0;}const auto id=AllocateLiveId();s.batches.emplace(key,BatchRecord{id,now});return id;}catch(...){return 0;}}
void PublishLiveDevice(const LiveDeviceSnapshot& value) noexcept
{if(!IsLiveCollectorEnabled())return;try{auto& s=S();std::lock_guard<std::mutex> lock(s.mutex);s.devices[value.device]=value;}catch(...) {}}
LiveContext GetLiveContext() noexcept {return context;}
void SetLiveContext(LiveContext value) noexcept {context=value;if(!value.frame.frame){currentFrame.reset();return;}try{currentFrame=Frame(value.frame);}catch(...){currentFrame.reset();}}
LiveFrameToken BeginLiveFrame(LiveId device,LiveId source) noexcept
{
    LiveFrameToken token;token.previous=context;if(!IsLiveCollectorEnabled()||!device||!source)return token;
    if(context.frame.frame&&context.frame.device==device){auto frame=Frame(context.frame);if(frame){std::lock_guard<std::mutex> lock(frame->mutex);if(!frame->ended){token.key=context.frame;return token;}}token.previous={};}
    try{auto& s=S();std::lock_guard<std::mutex> lock(s.mutex);auto& record=s.sources[source];record.device=device;
        if(!record.frames.empty()){auto frame=record.frames.back();std::lock_guard<std::mutex> frameLock(frame->mutex);if(!frame->ended&&frame->retry){frame->retry=false;frame->acquireSkipped=false;frame->attemptBeginNs=LiveNowNs();token.key=frame->summary.key;token.ownsFrame=true;currentFrame=frame;context={token.key,record.revision,source,0,0};return token;}}
        while(s.frameCount>=8192){bool removed=false;for(auto it=s.sources.begin();it!=s.sources.end();){if(it->first!=source&&it->second.frames.empty()&&it->second.retiredNs){it=s.sources.erase(it);continue;}auto ended=std::find_if(it->second.frames.begin(),it->second.frames.end(),[](const auto& f){return f->ended;});if(ended!=it->second.frames.end()){EvictFrame(*ended);it->second.frames.erase(ended);--s.frameCount;++s.omittedFrames;removed=true;break;}++it;}if(!removed){if(record.frames.empty()&&!record.run)s.sources.erase(source);++s.omittedFrames;return token;}}
        token.key={device,source,AllocateLiveId()};token.ownsFrame=true;
        auto frame=std::make_shared<FrameRecord>();frame->summary.key=token.key;frame->summary.beginNs=LiveNowNs();frame->attemptBeginNs=frame->summary.beginNs;frame->summary.appliedRevision=record.revision;frame->flow.key=token.key;
        if(!record.run){record.run=std::make_shared<SourceRun>();record.run->device=device;record.run->source=source;record.run->originNs=frame->summary.beginNs;}frame->run=record.run;
        record.frames.push_back(frame);++s.frameCount;currentFrame=frame;context={token.key,record.revision,source,0,0};
        while(s.frameCount>8192){bool removed=false;for(auto it=s.sources.begin();it!=s.sources.end();){if(it->second.frames.empty()&&it->second.retiredNs){it=s.sources.erase(it);continue;}if(!it->second.frames.empty()&&it->second.frames.front()->ended){EvictFrame(it->second.frames.front());it->second.frames.pop_front();--s.frameCount;++s.omittedFrames;removed=true;break;}++it;}if(!removed)break;}
    }catch(...){context={token.key,0,source,0,0};++S().omittedFrames;}
    return token;
}
void EndLiveFrame(LiveFrameToken token,LiveOutcome outcome) noexcept
{
    if(!token.key.frame){SetLiveContext(token.previous);return;}
    try{auto& s=S();std::lock_guard<std::mutex> lock(s.mutex);if(auto* f=FindFrame(s,token.key)){
        std::lock_guard<std::mutex> frameLock(f->mutex);
        if(f->ended){token.ownsFrame=false;}
        if(!IsLiveCollectorEnabled()){outcome=LiveOutcome::Discarded;f->summary.incomplete=true;}
        f->acquireSkipped|=outcome==LiveOutcome::Skipped;
        const auto& work=f->summary.recorded;
        const bool skipped=outcome==LiveOutcome::Skipped||(outcome==LiveOutcome::Completed&&f->acquireSkipped);
        const bool retry=skipped&&!f->settingsNotified&&!work.draw&&!work.drawIndexed&&!work.dispatch&&!work.dispatchMesh&&!work.uploadCalls&&!f->summary.submitted.submissions;
        if(token.ownsFrame&&f->attemptBeginNs){f->cpuNs+=LiveNowNs()-f->attemptBeginNs;f->attemptBeginNs=0;}
        if(token.ownsFrame&&retry)f->retry=true;
        if(token.ownsFrame&&!retry){
        if(skipped)outcome=LiveOutcome::Skipped;
        f->summary.endNs=LiveNowNs();f->summary.cpuRender={LiveValueState::Ready,double(f->cpuNs)/1e6};f->summary.outcome=outcome;f->ended=true;
        auto& source=s.sources[token.key.source];auto& frames=source.frames;
        if(source.lastEndedNs&&f->summary.endNs>=source.lastEndedNs)f->summary.frame={LiveValueState::Ready,double(f->summary.endNs-source.lastEndedNs)/1e6};
        if(auto previous=source.lastEnded.lock();previous&&previous.get()!=f){std::lock_guard<std::mutex> previousLock(previous->mutex);if(previous->publishedFlow){const auto bytes=previous->publishedFlow->events.capacity()*sizeof(LiveEvent)+previous->publishedFlow->links.capacity()*sizeof(LiveFlowLink);previous->retainedBytes-=bytes;detailBytes-=bytes;}previous->publishedFlow.reset();}
        if(currentFrame&&currentFrame.get()==f)source.lastEnded=currentFrame;else for(auto frame=frames.rbegin();frame!=frames.rend();++frame)if(frame->get()==f){source.lastEnded=*frame;break;}source.lastEndedKey=f->summary.key;source.lastEndedNs=f->summary.endNs;
        GpuSummary(*f);
        UpdateSecond(*f);
        auto detail=std::make_shared<LiveFlowSnapshot>();detail->key=f->flow.key;detail->omittedEvents=f->flow.omittedEvents;detail->omittedLinks=f->flow.omittedLinks;detail->events.swap(f->flow.events);detail->links.swap(f->flow.links);f->publishedFlow=std::move(detail);
        CaptureEvidence(s,source,*f,source.run->originNs);
        const auto cutoff=f->summary.endNs>LiveHistoryWindowNs?f->summary.endNs-LiveHistoryWindowNs:0;
        while(frames.size()>1&&frames.front()->ended&&frames.front()->summary.endNs<cutoff){EvictFrame(frames.front());frames.pop_front();--s.frameCount;}
    }}}catch(...){++S().omittedFrames;}
    if(token.previous.frame.frame){auto previous=Frame(token.previous.frame);if(!previous)token.previous={};else{std::lock_guard<std::mutex> lock(previous->mutex);if(previous->ended)token.previous={};else token.previous.appliedRevision=previous->summary.appliedRevision;}}
    SetLiveContext(token.previous);
}
void AccumulateLiveEventSummary(LiveEventSummary& summary,const LiveEvent& e) noexcept
{
    if(!IsLiveCollectorEnabled())return;
    if(e.kind==LiveEventKind::UploadMemory){if(e.operations!=1||!e.bytes||e.arguments[0]<0||e.arguments[0]>1){++summary.omittedEvents;return;}if(e.arguments[0])++summary.recorded.uploadPageReuses;else{++summary.recorded.uploadPageAllocations;summary.recorded.uploadPageAllocationBytes+=e.bytes;}return;}
    if(static_cast<std::size_t>(e.stage)>=LiveStageCount){++summary.omittedEvents;return;}
    auto& stage=summary.stages[static_cast<std::size_t>(e.stage)];stage.elements+=e.elements;stage.bytes+=e.bytes;stage.operations+=e.operations;
    if(e.endNs>=e.beginNs&&e.endNs){stage.cpu.state=LiveValueState::Ready;stage.cpu.milliseconds+=double(e.endNs-e.beginNs)/1e6;}
    auto& counts=summary.recorded;
    if(e.kind==LiveEventKind::Command){switch(e.commandKind){case LiveCommandKind::Draw:++counts.draw;counts.instances+=e.arguments[1];break;case LiveCommandKind::DrawIndexed:++counts.drawIndexed;counts.instances+=e.arguments[1];break;case LiveCommandKind::Dispatch:++counts.dispatch;break;case LiveCommandKind::DispatchMesh:++counts.dispatchMesh;break;case LiveCommandKind::Barrier:counts.barriers+=e.operations;break;case LiveCommandKind::Bind:counts.bindings+=e.operations;break;default:break;}}
    else if(e.kind==LiveEventKind::Upload){++counts.uploadCalls;counts.uploadBytes+=e.bytes;if(e.fullUpload)++counts.fullUploads;else ++counts.partialUploads;}
    else if(e.kind==LiveEventKind::Batch){++counts.batches;counts.elements+=e.elements;}
    else if(e.kind==LiveEventKind::DataRange)counts.dirtyRanges+=e.operations;
    else if(e.kind==LiveEventKind::Wait)counts.waits+=e.operations?e.operations:1;
}
// Keep room for submission accounting and GPU results when optional detail fills.
bool ReserveLiveDetailBytes(std::size_t bytes) noexcept{return IsLiveCollectorEnabled()&&ReserveDetail(bytes,OptionalDetailBudget);}
void ReleaseLiveDetailBytes(std::size_t bytes) noexcept{detailBytes-=bytes;}
void RecordLiveEventSummary(LiveCommandRef command,const LiveEventSummary& summary) noexcept
{
    if(!IsLiveCollectorEnabled())return;
    try{auto& s=S();auto f=Frame(context.frame);if(!f){s.omittedEvents+=summary.omittedEvents;s.omittedLinks+=summary.omittedLinks;return;}std::lock_guard<std::mutex> lock(f->mutex);LateSecondUpdate update(*f);Add(f->summary.recorded,summary.recorded);
        for(std::size_t i=0;i<LiveStageCount;++i){auto& to=f->summary.stages[i];const auto& from=summary.stages[i];to.elements+=from.elements;to.bytes+=from.bytes;to.operations+=from.operations;if(from.cpu.state==LiveValueState::Ready){to.cpu.state=LiveValueState::Ready;to.cpu.milliseconds+=from.cpu.milliseconds;}}
        f->flow.omittedEvents+=summary.omittedEvents;f->flow.omittedLinks+=summary.omittedLinks;s.omittedEvents+=summary.omittedEvents;s.omittedLinks+=summary.omittedLinks;f->summary.incomplete|=summary.omittedEvents||summary.omittedLinks;
        if(command.commandList){auto logical=summary.recorded;ClearNativeUploadCounts(logical);std::lock_guard<std::mutex> commands(s.commandMutex);const auto key=std::make_pair(command.commandList,command.generation);for(auto it=s.commandCounts.lower_bound({command.commandList,0});it!=s.commandCounts.end()&&it->first.first==command.commandList;)if(it->first!=key)it=s.commandCounts.erase(it);else++it;if(s.commandCounts.size()<4096||s.commandCounts.count(key))Add(s.commandCounts[key],logical);else f->summary.incomplete=true;}
    }catch(...){++S().omittedEvents;}
}
void RecordLiveEvent(const LiveEvent& input,bool includeSummary) noexcept
{
    if(!IsLiveCollectorEnabled())return;
    auto& s=S();
    try{
        auto e=input;if(!e.context.frame.frame)e.context=context;if(!e.id)e.id=AllocateLiveId();
        auto f=Frame(e.context.frame);if(!f){++s.omittedEvents;return;}
        std::unique_lock<std::mutex> lock(f->mutex);auto& summary=f->summary;
        std::optional<LiveFrameSummary> before;if(f->ended&&f->run)before=summary;
        if(static_cast<std::size_t>(e.stage)>=LiveStageCount){++s.omittedEvents;summary.incomplete=true;return;}
        if(includeSummary){LiveEventSummary eventSummary;AccumulateLiveEventSummary(eventSummary,e);auto counts=eventSummary.recorded;
        auto& stage=summary.stages[static_cast<std::size_t>(e.stage)];const auto& extra=eventSummary.stages[static_cast<std::size_t>(e.stage)];stage.elements+=extra.elements;stage.bytes+=extra.bytes;stage.operations+=extra.operations;if(extra.cpu.state==LiveValueState::Ready){stage.cpu.state=LiveValueState::Ready;stage.cpu.milliseconds+=extra.cpu.milliseconds;}
        if(e.kind==LiveEventKind::Submission){
            if(e.outcome==LiveOutcome::Submitted||e.outcome==LiveOutcome::Completed){
                const auto stamp=std::make_tuple(e.submission,e.command.commandList,e.command.generation,e.outcome);
                if(!f->counted.count(stamp)){
                    if(f->counted.size()>=8192||!ReserveDetail(128)){summary.incomplete=true;++s.omittedEvents;}
                    else{f->retainedBytes+=128;f->counted.insert(stamp);const auto submittedKey=std::make_tuple(e.submission,e.command.commandList,e.command.generation);
                        if(e.outcome==LiveOutcome::Completed){auto saved=f->submittedCounts.find(submittedKey);if(saved!=f->submittedCounts.end()){counts=saved->second;f->submittedCounts.erase(saved);f->retainedBytes-=512;detailBytes-=512;}else if(e.command.commandList)summary.incomplete=true;}
                        else{std::lock_guard<std::mutex> commands(s.commandMutex);auto it=s.commandCounts.find({e.command.commandList,e.command.generation});if(it!=s.commandCounts.end())counts=it->second;else if(e.command.commandList)summary.incomplete=true;
                            if(ReserveDetail(512)){f->retainedBytes+=512;f->submittedCounts[submittedKey]=counts;}else summary.incomplete=true;
                        }
                        ClearNativeUploadCounts(counts);counts.submissions=e.operations;Add(e.outcome==LiveOutcome::Submitted?summary.submitted:summary.completed,counts);
                    }
                }
            }
        }else{Add(summary.recorded,counts);if(e.command.commandList&&e.kind!=LiveEventKind::UploadMemory){std::lock_guard<std::mutex> commands(s.commandMutex);auto key=std::make_pair(e.command.commandList,e.command.generation);for(auto it=s.commandCounts.lower_bound({e.command.commandList,0});it!=s.commandCounts.end()&&it->first.first==e.command.commandList;)if(it->first!=key)it=s.commandCounts.erase(it);else++it;if(s.commandCounts.size()<4096||s.commandCounts.count(key))Add(s.commandCounts[key],counts);else summary.incomplete=true;}}
        }
        if(e.kind==LiveEventKind::Output&&e.commandKind==LiveCommandKind::Present&&e.outcome==LiveOutcome::Completed&&!f->ended)f->acquireSkipped=false;
        if(e.kind==LiveEventKind::Wait&&e.outcome==LiveOutcome::Skipped&&e.id==e.context.frame.frame){f->acquireSkipped=true;auto existing=std::find_if(f->flow.events.begin(),f->flow.events.end(),[&](const auto& v){return v.id==e.id&&v.kind==LiveEventKind::Wait&&v.outcome==LiveOutcome::Skipped;});if(existing!=f->flow.events.end()){existing->endNs=e.endNs;existing->operations+=e.operations;return;}}
        if(f->ended){if(before)UpdateSecond(*f,&*before);RepairEvidence(*f);AppendEvidenceDetail(*f,&e,nullptr);lock.unlock();if(!AppendLateDetail(f,&e,nullptr))++s.omittedEvents;}
        else if(!PushDetail(*f,f->flow.events,e,8192)){++f->flow.omittedEvents;++s.omittedEvents;summary.incomplete=true;}
    }catch(...){++s.omittedEvents;try{if(auto f=Frame(input.context.frame.frame?input.context.frame:context.frame)){std::lock_guard<std::mutex> lock(f->mutex);f->summary.incomplete=true;}}catch(...) {}}
}
void RecordLiveLink(const LiveFlowLink& link) noexcept
{if(!IsLiveCollectorEnabled())return;try{if(auto f=Frame(context.frame)){std::unique_lock<std::mutex> lock(f->mutex);if(f->ended){AppendEvidenceDetail(*f,nullptr,&link);lock.unlock();if(!AppendLateDetail(f,nullptr,&link))++S().omittedLinks;}else if(!PushDetail(*f,f->flow.links,link,16384)){++f->flow.omittedLinks;++S().omittedLinks;f->summary.incomplete=true;}}}catch(...){++S().omittedLinks;}}
void RecordLiveGpuSpan(const LiveGpuSpan& span) noexcept
{
    if(!IsLiveCollectorEnabled()||!span.id)return;
    try{auto& s=S();auto f=Frame(span.frame);if(!f){++s.omittedSpans;return;}std::lock_guard<std::mutex> lock(f->mutex);LateSecondUpdate update(*f);
        auto& spans=f->flow.gpuSpans;auto it=std::find_if(spans.begin(),spans.end(),[&](const auto& v){return v.id==span.id;});
        if(it!=spans.end()) {if(it->frame!=span.frame||it->appliedRevision!=span.appliedRevision){++s.omittedSpans;return;}*it=span;}
        else if(!PushDetail(*f,spans,span,128)){++s.omittedSpans;++f->omittedGpuSpans;f->summary.incomplete=true;f->coverageLost=true;GpuSummary(*f);return;}
        GpuSummary(*f);
    }catch(...){++S().omittedSpans;}
}
void SetLiveGpuCoverage(LiveFrameKey key,bool complete) noexcept
{if(!IsLiveCollectorEnabled())return;try{if(auto f=Frame(key)){std::lock_guard<std::mutex> lock(f->mutex);LateSecondUpdate update(*f);f->coverage=complete;GpuSummary(*f);}}catch(...) {}}
void SetLiveObjectGauge(LiveFrameKey key,LiveObjectGauge gauge) noexcept
{if(!IsLiveCollectorEnabled())return;try{if(auto f=Frame(key)){std::lock_guard<std::mutex> lock(f->mutex);LateSecondUpdate update(*f);f->summary.objects=gauge;}}catch(...) {}}
std::shared_ptr<const LiveSnapshot> ReadLiveSnapshot()
{
    if(!IsLiveCollectorEnabled())return {};
    auto result=std::make_shared<LiveSnapshot>();std::map<LiveId,SourceRecord> sources;std::vector<std::shared_ptr<const LiveObjectInfo>> inventory;
    auto& s=S();
    {std::lock_guard<std::mutex> lock(s.mutex);result->session=s.session;result->sequence=++s.sequence;result->producerTimeNs=LiveNowNs();result->clock=GetLiveClockInfo();result->loss=s.loss;
        const auto cutoff=result->producerTimeNs>LiveHistoryWindowNs?result->producerTimeNs-LiveHistoryWindowNs:0;
        for(auto it=s.sources.begin();it!=s.sources.end();){auto& source=it->second;for(auto frame=source.frames.begin();frame!=source.frames.end();)if((*frame)->ended&&(*frame)->summary.endNs<cutoff){EvictFrame(*frame);frame=source.frames.erase(frame);--s.frameCount;}else++frame;if(source.candidate.endNs<cutoff)source.candidate={};if(source.previousCandidate.endNs<cutoff)source.previousCandidate={};if(source.retiredNs&&source.frames.empty())it=s.sources.erase(it);else++it;}
        result->loss.events+=s.omittedEvents.load();result->loss.links+=s.omittedLinks.load();result->loss.frames+=s.omittedFrames.load();result->loss.gpuSpans+=s.omittedSpans.load();
        result->collector.retainedFrames=s.frameCount;result->collector.retainedDetailBytes=detailBytes.load();result->collector.batches=s.batches.size();{std::lock_guard<std::mutex> commands(s.commandMutex);result->collector.commandGenerations=s.commandCounts.size();}
        inventory.reserve(s.objects.size());for(const auto& pair:s.objects)inventory.push_back(pair.second);for(const auto& pair:s.devices)result->devices.push_back(pair.second);
        for(const auto& pair:s.controls)result->controls.push_back(pair.second->desc);
        for(auto& pair:s.sources)UpdateControlComparisons(s,pair.first,pair.second,result->producerTimeNs);
        result->controlStatus.assign(s.requests.begin(),s.requests.end());
        // Only shared frame ownership is copied here; no large flow copy under this lock.
        for(const auto& pair:s.sources){auto& copy=sources[pair.first];copy.device=pair.second.device;copy.revision=pair.second.revision;copy.frames=pair.second.frames;copy.run=pair.second.run;copy.lastEndedKey=pair.second.lastEndedKey;copy.evidenceRequest=pair.second.evidenceRequest;copy.beforeEvidence=pair.second.beforeEvidence;copy.afterEvidence=pair.second.afterEvidence;}
    }
    result->objects.reserve(inventory.size());for(const auto& object:inventory)result->objects.push_back(*object);
    const auto now=result->producerTimeNs;
    for(auto& pair:sources){auto& source=pair.second;if(!source.run||!source.lastEndedKey.frame)continue;
        LiveSourceSnapshot out;out.device=source.device;out.source=pair.first;ReadRun(*source.run,now,out);
        const auto compared=std::find_if(result->controlStatus.begin(),result->controlStatus.end(),[&](const auto& status){return status.request==source.evidenceRequest&&status.state==ControlState::Applied;});
        if(compared!=result->controlStatus.end()){out.comparisonEvidence.request=compared->request;out.comparisonEvidence.before=CopyEvidence(source.beforeEvidence,compared->baseline,now);out.comparisonEvidence.after=CopyEvidence(source.afterEvidence,compared->after,now);}
        std::shared_ptr<const LiveFlowSnapshot> detail;std::vector<LiveGpuSpan> recent;
        for(auto& ptr:source.frames){auto copy=std::make_shared<FrameRecord>();{std::lock_guard<std::mutex> lock(ptr->mutex);copy->summary=ptr->summary;copy->ended=ptr->ended;copy->flow.gpuSpans=ptr->flow.gpuSpans;copy->flow.omittedEvents=ptr->flow.omittedEvents;copy->flow.omittedLinks=ptr->flow.omittedLinks;if(ptr->ended&&ptr->publishedFlow)detail=ptr->publishedFlow;}ptr=std::move(copy);}
        auto latest=std::find_if(source.frames.rbegin(),source.frames.rend(),[&](const auto& f){return f->ended&&f->summary.key==source.lastEndedKey;});
        if(latest!=source.frames.rend()){out.latest=(*latest)->summary;if(detail)out.flow=*detail;out.flow.gpuSpans=(*latest)->flow.gpuSpans;out.flow.omittedEvents=(*latest)->flow.omittedEvents;out.flow.omittedLinks=(*latest)->flow.omittedLinks;}
        out.latest.sampleAgeNs=now>=out.latest.endNs?now-out.latest.endNs:0;
        for(auto it=source.frames.rbegin();it!=source.frames.rend()&&recent.size()<1024;++it)for(const auto& span:(*it)->flow.gpuSpans)if(span.duration.state!=LiveValueState::Pending&&recent.size()<1024)recent.push_back(span);
        out.recentGpuSpans=std::move(recent);
        std::set<std::pair<std::uint64_t,LiveId>> buckets;
        for(const auto& frame:source.frames)if(frame->ended){buckets.emplace(frame->summary.endNs/LivePublishIntervalNs,frame->summary.appliedRevision);if(frame->summary.cpuRender.milliseconds>=1000||frame->summary.endNs-frame->summary.beginNs>=1000000000ull||(frame->summary.frame.state==LiveValueState::Ready&&frame->summary.frame.milliseconds>=1000))out.longFrames.push_back(frame->summary);}
        for(const auto& bucket:buckets)out.history.push_back(Aggregate(source,pair.first,bucket.second,bucket.first*LivePublishIntervalNs,(bucket.first+1)*LivePublishIntervalNs));
        result->sources.push_back(std::move(out));
    }
    return result;
}
namespace
{
LiveId ApplyRevision(State& s,LiveId source,LiveFrameKey key,bool changed)
{
    auto& v=s.sources[source];v.device=key.device;auto* f=FindFrame(s,key);bool notified=false;std::uint64_t now=LiveNowNs();
    if(f){std::lock_guard<std::mutex> lock(f->mutex);notified=f->settingsNotified;}
    if(changed&&!notified){UpdateControlComparisons(s,source,v,now);v.baseline=CompletedSecond(v,v.revision,now);++v.revision;if(f){std::lock_guard<std::mutex> lock(f->mutex);f->settingsNotified=true;f->summary.appliedRevision=v.revision;}}
    if(context.frame==key)context.appliedRevision=v.revision;return v.revision;
}
}
LiveId NotifyLiveSettingsApplied(LiveId source,LiveFrameKey key,bool changed) noexcept
{
    if(!IsLiveCollectorEnabled())return 0;
    try{auto& s=S();std::lock_guard<std::mutex> lock(s.mutex);return ApplyRevision(s,source,key,changed);}catch(...){return 0;}
}
ControlRegistration RegisterControl(ControlDesc desc,ControlHandler handler)
{
    if(!IsLiveCollectorEnabled()||(!handler&&desc.writable)||!desc.owner||!desc.source||!desc.device)return {};
    if(desc.key.size()>128||desc.label.size()>256||desc.unavailableReason.size()>1024||desc.description.size()>1024||desc.onPath.size()>1024||desc.offPath.size()>1024||desc.conditions.size()>1024||desc.tradeoffs.size()>1024)return {};
    desc.id=AllocateLiveId();auto slot=std::make_shared<ControlSlot>();slot->desc=std::move(desc);slot->handler=std::move(handler);slot->ownerThread=std::this_thread::get_id();
    auto& s=S();std::lock_guard<std::mutex> lock(s.mutex);s.controls.emplace(slot->desc.id,slot);return ControlRegistration(slot->desc.id);
}
ControlRegistration::~ControlRegistration(){Unregister(id);}
ControlRegistration::ControlRegistration(ControlRegistration&& other) noexcept:id(std::exchange(other.id,0)){}
ControlRegistration& ControlRegistration::operator=(ControlRegistration&& other) noexcept{if(this!=&other){Unregister(id);id=std::exchange(other.id,0);}return *this;}
namespace Detail
{
LiveId EnqueueControl(ControlRequest request)
{
    if(!IsLiveCollectorEnabled()||!request.control)return 0;
    auto& s=S();std::lock_guard<std::mutex> lock(s.mutex);if(!request.request)request.request=AllocateLiveId();
    if(FindRequest(s,request.request))return 0;
    ControlStatus status;status.request=request.request;status.control=request.control;status.requested=request.value;status.requestedNs=LiveNowNs();
    const auto it=s.controls.find(request.control);
    if(it==s.controls.end()){status.state=ControlState::Rejected;status.message="Unknown control";}
    else{status.device=it->second->desc.device;status.source=it->second->desc.source;status.previousActual=status.actual=it->second->desc.appliedValue;
        for(auto& r:s.requests)if(r.control==request.control&&!Terminal(r.state)&&it->second->executing!=r.request){r.state=ControlState::Cancelled;r.message="Superseded";s.expectedRequests.erase(r.request);}
    }
    while(s.requests.size()>=256){if(!Terminal(s.requests.front().state))return 0;s.expectedRequests.erase(s.requests.front().request);s.requests.pop_front();}
    s.requests.push_back(std::move(status));return request.request;
}
}
LiveId RequestLiveControl(LiveId id,const ControlValue& value){return Detail::EnqueueControl({0,id,value});}
std::uint32_t PollControls(LiveId owner,LiveFrameKey key) noexcept
{
    if(!IsLiveCollectorEnabled())return 0;
    std::uint32_t count=0;
    try{
        std::vector<std::pair<ControlRequest,std::shared_ptr<ControlSlot>>> work;
        {auto& s=S();std::lock_guard<std::mutex> lock(s.mutex);for(auto& r:s.requests){if(r.state!=ControlState::Requested)continue;auto it=s.controls.find(r.control);if(it==s.controls.end()||it->second->desc.owner!=owner)continue;auto slot=it->second;
            if(ControlInvocation::Contains(slot.get()))continue;
            if(slot->ownerThread!=std::this_thread::get_id()||!slot->handler||!Validate(slot->desc,r.requested)){r.state=ControlState::Rejected;r.message="Invalid type/range, unsupported control or owner thread";continue;}
            r.state=ControlState::Validated;work.push_back({{r.request,r.control,r.requested},slot});if(work.size()==32)break;
        }}
        for(auto& item:work){auto& slot=*item.second;std::lock_guard<std::mutex> invoke(slot.invocation);if(!slot.active)continue;ControlInvocation invocation(slot);
            bool noop=false;{auto& s=S();std::lock_guard<std::mutex> lock(s.mutex);auto* r=FindRequest(s,item.first.request);if(!r||r->state!=ControlState::Validated||!slot.active)continue;noop=!slot.pending&&slot.desc.appliedValue==item.first.value;slot.executing=r->request;r->state=ControlState::PendingApply;}
            if(noop){CompleteControl(item.first.request,key,item.first.value);{auto& s=S();std::lock_guard<std::mutex> lock(s.mutex);slot.executing=0;}++count;continue;}
            ControlApplyResult result;try{result=slot.handler(item.first,key);}catch(const std::exception& e){result.message=e.what();}catch(...){result.message="Control callback failed";}
            ++count;
            if(result.normalizedExpected){auto& s=S();std::lock_guard<std::mutex> lock(s.mutex);auto* r=FindRequest(s,item.first.request);if(!Validate(slot.desc,*result.normalizedExpected,false)){result.state=ControlState::Failed;result.message="Invalid normalized control value";}else if(r&&!Terminal(r->state))s.expectedRequests[item.first.request]=*result.normalizedExpected;}
            if(result.state==ControlState::Applied)CompleteControl(item.first.request,key,result.actual);
            else{auto& s=S();std::lock_guard<std::mutex> lock(s.mutex);auto* r=FindRequest(s,item.first.request);if(r&&!Terminal(r->state)){r->state=result.state==ControlState::PendingApply?ControlState::PendingApply:ControlState::Failed;r->message=result.message;slot.pending=r->state==ControlState::PendingApply;for(const auto& newer:s.requests)if(newer.control==r->control&&newer.state==ControlState::Requested&&newer.request!=r->request){r->state=ControlState::Cancelled;r->message="Superseded";break;}if(Terminal(r->state))s.expectedRequests.erase(r->request);}}
            {auto& s=S();std::lock_guard<std::mutex> lock(s.mutex);slot.executing=0;}
        }
    }catch(...){}
    return count;
}
void PublishControlValue(LiveId id,const ControlValue& value) noexcept
{if(!id)return;try{auto& s=S();std::lock_guard<std::mutex> lock(s.mutex);auto it=s.controls.find(id);if(it!=s.controls.end())it->second->desc.appliedValue=value;}catch(...) {}}
void PublishControlAvailability(LiveId id,bool available,std::string_view reason) noexcept
{if(!id)return;try{auto& s=S();std::lock_guard<std::mutex> lock(s.mutex);auto it=s.controls.find(id);if(it!=s.controls.end()){it->second->desc.writable=available;it->second->desc.unavailableReason.assign(reason.substr(0,1024));}}catch(...) {}}
void CompleteControl(LiveId request,LiveFrameKey key,const ControlValue& value) noexcept
{
    if(!request)return;
    try{auto& s=S();std::lock_guard<std::mutex> lock(s.mutex);auto* r=FindRequest(s,request);if(!r||Terminal(r->state))return;const auto source=r->source;auto slot=s.controls.find(r->control);if(slot==s.controls.end())return;
        const auto normalized=s.expectedRequests.find(request);const auto& expected=normalized==s.expectedRequests.end()?r->requested:normalized->second;
        if(value!=expected){r->state=ControlState::Cancelled;r->actual=value;r->message="Superseded by application settings";slot->second->pending=false;s.expectedRequests.erase(request);return;}
        r->previousActual=slot->second->desc.appliedValue;const bool changed=r->previousActual!=value;const auto revision=ApplyRevision(s,source,key,changed);
        r->actual=value;r->appliedFrame=key;r->appliedRevision=revision;r->appliedNs=LiveNowNs();r->baseline=changed?s.sources[source].baseline:CompletedSecond(s.sources[source],revision,r->appliedNs);
        r->state=ControlState::Applied;s.expectedRequests.erase(request);
        auto& evidenceSource=s.sources[source];evidenceSource.evidenceRequest=request;evidenceSource.beforeEvidence=SelectEvidence(evidenceSource,r->baseline);evidenceSource.afterEvidence={};
        auto it=s.controls.find(r->control);if(it!=s.controls.end()){it->second->desc.appliedValue=value;it->second->pending=false;}
    }catch(...){}
}
void FailControl(LiveId request,std::string_view message) noexcept
{try{auto& s=S();std::lock_guard<std::mutex> lock(s.mutex);if(auto* r=FindRequest(s,request)){if(!Terminal(r->state)){r->state=ControlState::Failed;r->message.assign(message.substr(0,1024));s.expectedRequests.erase(request);}}}catch(...) {}}
LiveCpuScope::LiveCpuScope(LiveStage stage,const char* label) noexcept
{if(!IsLiveCollectorEnabled())return;event.context=GetLiveContext();event.stage=stage;event.beginNs=LiveNowNs();if(label){std::strncpy(event.label,label,sizeof(event.label)-1);}}
LiveCpuScope::~LiveCpuScope(){if(event.beginNs){event.endNs=LiveNowNs();RecordLiveEvent(event);}}
}
