#include "TracyMetrics.h"
#include "../../Diagnostics/LiveTracy.h"
#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <tuple>
#include <unordered_map>
#if defined(DY_VIEWER_EMBED_TRACY) && DY_VIEWER_EMBED_TRACY
#include "TracyWorker.hpp"
#endif

namespace viewer
{
namespace
{
namespace T=D::Detail;
using FrameKey=std::tuple<uint64_t,uint64_t,uint64_t>;
using WindowKey=std::tuple<uint64_t,uint64_t,uint64_t,uint64_t,uint64_t>;
FrameKey Key(D::LiveFrameKey k) { return {k.device,k.source,k.frame}; }
WindowKey Key(const D::LiveAggregate& a) { return {a.device,a.source,a.appliedRevision,a.beginNs,a.endNs}; }
constexpr uint64_t RetainNs=D::LiveHistoryWindowNs+2*D::LiveAverageWindowNs;
constexpr size_t FrameLimit=32768,WindowLimit=2048,StageSpanLimit=8192,MessageLimit=131072;
struct Span
{
    std::optional<int64_t> begin,end;
    uint64_t connection=0,revision=0;
    bool invalid=false;
    bool Add(bool first,int64_t stamp,const T::LiveTraceEvent& e)
    {
        auto& target=first?begin:end;
        const bool changed=!target||*target!=stamp||connection!=e.connection||revision!=e.revision;
        if(connection && (connection!=e.connection || revision!=e.revision))invalid=true;
        connection=e.connection;revision=e.revision;
        if(target && *target!=stamp)invalid=true;target=stamp;
        return changed;
    }
    std::optional<double> Milliseconds(const T::LiveTraceEvent& e) const
    {
        if(invalid||!begin||!end||*end<*begin||connection!=e.connection||revision!=e.revision)return {};
        const double ms=double(static_cast<long double>(*end)-*begin)/1e6;
        return std::isfinite(ms)?std::optional<double>(ms):std::nullopt;
    }
};
struct Frame
{
    std::optional<T::LiveTraceEvent> seal,boundary;
    std::optional<int64_t> end;
    std::map<uint64_t,Span> attempts;
    std::array<std::map<uint64_t,Span>,D::LiveStageCount> stages;
    std::array<bool,D::LiveStageCount> limitedStages{};
    int64_t lastStamp=0;
    bool invalid=false;
};
struct Window
{
    D::LiveAggregate original;
    MetricEvidence evidence;
    std::optional<double> frame,cpu;
    std::array<std::optional<double>,D::LiveStageCount> stages;
    std::string frameReason="논리 프레임 경계 또는 이전 원본 프레임 미수집";
    std::string cpuReason="프레임 시도 구간 누락·진행 중 또는 revision 불일치";
    std::array<std::string,D::LiveStageCount> stageReason;
    bool engineDetailIncomplete=false;
};
bool SameBoundary(const T::LiveTraceEvent& a,const T::LiveTraceEvent& b)
{
    return a.frame==b.frame && a.connection==b.connection && a.revision==b.revision && a.id==b.id && a.aux==b.aux
        && a.originNs==b.originNs && a.endNs==b.endNs && a.previousFrame==b.previousFrame;
}
struct RunRange
{
    struct Values {std::optional<double> engine,tracy;};
    std::map<std::pair<uint64_t,uint64_t>,Values> windows;
    std::multiset<double> engine,tracy;
    void Update(const D::LiveAggregate& a,std::optional<double> own,std::optional<double> raw)
    {
        const auto key=std::make_pair(a.beginNs,a.endNs);const auto found=windows.find(key);
        if(found!=windows.end())
        {
            if(found->second.engine==own&&found->second.tracy==raw)return;
            if(found->second.engine)engine.erase(engine.find(*found->second.engine));
            if(found->second.tracy)tracy.erase(tracy.find(*found->second.tracy));
            windows.erase(found);
        }
        if(own)engine.insert(*own);if(raw)tracy.insert(*raw);
        if(own||raw)windows.emplace(key,Values{own,raw});
    }
    static D::LiveMetricRange Range(const std::multiset<double>& values)
    {
        D::LiveMetricRange r;if(!values.empty()){r.state=D::LiveValueState::Ready;r.low=*values.begin();r.high=*values.rbegin();r.windows=values.size();}return r;
    }
};
bool SameMetric(const D::LiveMetricAggregate& a,const D::LiveMetricAggregate& b)
{
    return a.state==b.state && a.ready==b.ready && a.pending==b.pending && a.unsupported==b.unsupported
        && a.unavailable==b.unavailable && a.lastReadyFrame==b.lastReadyFrame;
}
bool CompleteMetric(const D::LiveMetricAggregate& m,uint64_t frames)
{
    return frames && m.state==D::LiveValueState::Ready && m.ready==frames && !m.pending && !m.unsupported && !m.unavailable;
}
bool SameWindow(const D::LiveAggregate& a,const D::LiveAggregate& b)
{
    if(a.frameCount!=b.frameCount||!SameMetric(a.cpuRender,b.cpuRender)||!SameMetric(a.frame,b.frame))return false;
    for(size_t i=0;i<D::LiveStageCount;++i)if(!SameMetric(a.stages[i].cpu,b.stages[i].cpu))return false;
    return true;
}
bool ValidWindow(const D::LiveAggregate& a,uint64_t now)
{
    return a.frameCount && a.appliedRevision && !a.mixedRevision && a.endNs>a.beginNs
        && a.endNs-a.beginNs==D::LiveAverageWindowNs && a.endNs<=now;
}
MetricEvidence Evidence(D::LiveSessionId session,const D::LiveAggregate& a)
{
    MetricEvidence e;e.session=session;e.device=a.device;e.source=a.source;e.revision=a.appliedRevision;
    e.beginNs=a.beginNs;e.endNs=a.endNs;e.frames=a.frameCount;return e;
}
const char* Definition(ComparisonMetric metric)
{
    switch(metric)
    {
    case ComparisonMetric::Frame:return "동일 source의 논리 종료 간격 평균 (완료 1초 구간, endNs 소속)";
    case ComparisonMetric::Fps:return "1000 / 논리 종료 간격 평균 ms (완료 처리율·Present FPS와 구분)";
    case ComparisonMetric::Cpu:return "논리 프레임별 소유 시도 실행 시간의 합 평균 (재시도 대기 제외)";
    case ComparisonMetric::Gpu:return "완료된 프레임 GPU coverage (Tracy GPU 대응 계측 없음)";
    default:return "엔진 고유 의미 카운터";
    }
}
void AddRange(D::LiveMetricRange& r,double value)
{
    if(!r.windows)r.low=r.high=value;else{r.low=(std::min)(r.low,value);r.high=(std::max)(r.high,value);}
    ++r.windows;r.state=D::LiveValueState::Ready;
}
}
struct TracyMetrics::Impl
{
    D::LiveSessionId session;
    uint64_t now=0,lastReadTick=~uint64_t(0),observedSequence=~uint64_t(0),workerIdentity=0,captureTime=0;
    int64_t lastReadTraceTick=-1;
    size_t lastMessageCount=0;
    bool pendingStrings=false;
    std::unordered_map<uintptr_t,int64_t> seenMessages;
    int64_t latestStamp=0;
    std::map<FrameKey,Frame> frames;
    std::map<WindowKey,Window> windows;
    std::map<std::tuple<D::LiveId,D::LiveId,ComparisonMetric>,RunRange> runRanges;

    void Track(const D::LiveAggregate& a,std::optional<double> cpu={},std::optional<double> frame={})
    {
        if(!a.device||!a.source||a.endNs<=a.beginNs||a.endNs-a.beginNs!=D::LiveAverageWindowNs||a.endNs>now)return;
        for(const auto metric:{ComparisonMetric::Cpu,ComparisonMetric::Frame,ComparisonMetric::Fps})
        {
            auto raw=metric==ComparisonMetric::Cpu?cpu:frame;
            if(metric==ComparisonMetric::Fps)raw=raw&&*raw>0?std::optional<double>(1000.0/ *raw):std::nullopt;
            const auto own=ValidWindow(a,now)?ComparisonNumber(a,metric):std::nullopt;
            runRanges[{a.device,a.source,metric}].Update(a,own,raw);
        }
    }

    void Invalidate(const T::LiveTraceEvent& e)
    {
        for(auto w=windows.begin();w!=windows.end();)
            if(w->second.evidence.device==e.frame.device && w->second.evidence.source==e.frame.source && e.endNs>=w->second.evidence.beginNs && e.endNs<w->second.evidence.endNs)w=windows.erase(w);else ++w;
    }

    void Consume(const T::LiveTraceEvent& e,int64_t stamp)
    {
        if(e.session!=session)return;
        auto it=frames.find(Key(e.frame));
        if(it==frames.end()) { if(frames.size()>=FrameLimit)return;it=frames.emplace(Key(e.frame),Frame{}).first; }
        auto& f=it->second;f.lastStamp=(std::max)(f.lastStamp,stamp);latestStamp=(std::max)(latestStamp,stamp);
        if(e.kind==T::LiveTraceKind::FrameEnd||e.kind==T::LiveTraceKind::FrameSeal)
        {
            if(f.boundary&&!SameBoundary(*f.boundary,e))f.invalid=true;
            if(e.kind==T::LiveTraceKind::FrameEnd)
            {
                if(f.end && (*f.end!=stamp || f.boundary->connection!=e.connection))f.invalid=true;
                f.end=stamp;f.boundary=e;
            }
            if(!f.seal || e.sequence>f.seal->sequence)
            {
                Invalidate(e);
                f.seal=e;
            }
            else if(e.sequence==f.seal->sequence && (e.kind!=f.seal->kind || !SameBoundary(e,*f.seal)
                || e.stageCount!=f.seal->stageCount || e.stageDigest!=f.seal->stageDigest || e.unsupportedStages!=f.seal->unsupportedStages))f.invalid=true;
            if(f.invalid)Invalidate(e);
            return;
        }
        if(e.kind==T::LiveTraceKind::AttemptBegin||e.kind==T::LiveTraceKind::AttemptEnd)
        {
            if(e.id>8192){f.invalid=true;if(f.seal)Invalidate(*f.seal);return;}
            if(f.attempts[e.id].Add(e.kind==T::LiveTraceKind::AttemptBegin,stamp,e)&&f.seal)Invalidate(*f.seal);
        }
        else
        {
            const auto index=size_t(e.aux);auto& stage=f.stages[index];
            if(stage.size()>=StageSpanLimit&&!stage.count(e.id))
            {f.limitedStages[index]=true;if(f.seal)Invalidate(*f.seal);return;}
            if(stage[e.id].Add(e.kind==T::LiveTraceKind::StageBegin,stamp,e)&&f.seal)Invalidate(*f.seal);
        }
    }
    void Build(const D::LiveAggregate& a)
    {
        if(!ValidWindow(a,now)){Track(a);return;}
        const auto cached=windows.find(Key(a));
        if(cached!=windows.end()&&SameWindow(a,cached->second.original))
        {Track(a,cached->second.cpu,cached->second.frame);cached->second.original=a;return;}
        std::vector<const Frame*> originals;
        for(auto it=frames.lower_bound({a.device,a.source,0});it!=frames.end() && std::get<0>(it->first)==a.device && std::get<1>(it->first)==a.source;++it)
            if(it->second.seal && it->second.seal->endNs>=a.beginNs && it->second.seal->endNs<a.endNs)originals.push_back(&it->second);
        if(originals.empty())
        {
            const auto old=windows.find(Key(a));
            if(old==windows.end())Track(a);
            else Track(a,old->second.cpu,old->second.frame);
            return;
        }
        Window w;w.original=a;w.evidence=Evidence(session,a);
        for(auto& s:w.stageReason)s="단계별 원본 이벤트 집합 또는 측정 범위 불일치";
        auto store=[&]{Track(a,w.cpu,w.frame);if(windows.size()>=WindowLimit&&!windows.count(Key(a)))windows.erase(windows.begin());windows[Key(a)]=std::move(w);};
        if(originals.size()!=a.frameCount){w.cpuReason=w.frameReason="완료 구간의 원본 프레임 집합 불완전";store();return;}
        std::sort(originals.begin(),originals.end(),[](const Frame* x,const Frame* y){return std::tie(x->seal->endNs,x->seal->frame.frame)<std::tie(y->seal->endNs,y->seal->frame.frame);});
        const auto origin=originals.front()->seal->originNs;
        const auto connection=originals.front()->seal->connection;
        w.evidence.tracyConnection=connection;w.evidence.tracyFirstNs=originals.front()->end.value_or(0);w.evidence.tracyLastNs=originals.back()->end.value_or(0);
        for(size_t i=0;i<originals.size();++i)
        {
            const auto& f=*originals[i];const auto& e=*f.seal;
            if(f.invalid || !f.end || !f.boundary || f.boundary->connection!=e.connection || f.boundary->revision!=e.revision
                || e.revision!=a.appliedRevision || e.originNs!=origin || e.connection!=connection
                || a.beginNs<origin || (a.beginNs-origin)%D::LiveAverageWindowNs || (i && e.previousFrame!=originals[i-1]->seal->frame.frame))
            { w.cpuReason=w.frameReason="원본 프레임 연결·세션·revision·유실 검증 실패";store();return; }
            w.engineDetailIncomplete|=(e.unsupportedStages>>63)!=0;
            w.evidence.originalFrames.push_back(e.frame);w.evidence.frameDigest^=T::LiveTraceDigest(e.frame.frame);
        }
        bool cpu=CompleteMetric(a.cpuRender,a.frameCount),frame=CompleteMetric(a.frame,a.frameCount);
        double cpuSum=0,frameSum=0;
        D::LiveFrameKey lastCpu{},lastFrame{};
        std::array<double,D::LiveStageCount> sums{};
        std::array<uint64_t,D::LiveStageCount> ready{};
        std::array<D::LiveFrameKey,D::LiveStageCount> lastStage{};
        std::array<bool,D::LiveStageCount> stages;stages.fill(true);
        for(const auto* fp:originals)
        {
            const auto& f=*fp;const auto& e=*f.seal;double duration=0;
            if(f.attempts.size()!=e.id)cpu=false;
            std::optional<int64_t> previousAttemptEnd;
            for(uint64_t i=1;i<=e.id && i<=8192;++i)
            {
                const auto span=f.attempts.find(i);const auto ms=span==f.attempts.end()?std::nullopt:span->second.Milliseconds(e);
                if(ms)
                {
                    if(*span->second.end>*f.end || (previousAttemptEnd&&*span->second.begin<*previousAttemptEnd))cpu=false;
                    previousAttemptEnd=span->second.end;duration+=*ms;
                }
                else cpu=false;
            }
            cpuSum+=duration;if(e.frame.frame>lastCpu.frame)lastCpu=e.frame;
            const auto prev=frames.find({e.frame.device,e.frame.source,e.previousFrame});
            if(prev==frames.end() || !prev->second.end || !prev->second.seal || prev->second.invalid || prev->second.seal->connection!=e.connection
                || prev->second.seal->endNs>e.endNs || *prev->second.end>*f.end)frame=false;
            else {frameSum+=double(static_cast<long double>(*f.end)-*prev->second.end)/1e6;if(e.frame.frame>lastFrame.frame)lastFrame=e.frame;}
            for(size_t i=0;i<D::LiveStageCount;++i)
            {
                if(f.limitedStages[i]||e.stageCount[i]>StageSpanLimit)
                {stages[i]=false;w.stageReason[i]="단계 원본 이벤트 보존 한도 초과; 자체 값 유지";}
                if((e.unsupportedStages&(1ull<<i)) || f.stages[i].size()!=e.stageCount[i])stages[i]=false;
                uint64_t digest=0;double sum=0;
                for(const auto& item:f.stages[i]) {digest^=T::LiveTraceDigest(item.first);const auto ms=item.second.Milliseconds(e);if(ms)sum+=*ms;else stages[i]=false;}
                if(digest!=e.stageDigest[i])stages[i]=false;
                if(e.stageCount[i]){sums[i]+=sum;++ready[i];if(e.frame.frame>lastStage[i].frame)lastStage[i]=e.frame;}
            }
        }
        if(cpu && lastCpu==a.cpuRender.lastReadyFrame)w.cpu=cpuSum/a.frameCount;
        if(frame && lastFrame==a.frame.lastReadyFrame)w.frame=frameSum/a.frameCount;
        for(size_t i=0;i<D::LiveStageCount;++i)
        {
            const auto& own=a.stages[i].cpu;
            if(stages[i] && ready[i] && own.state==D::LiveValueState::Ready && own.ready==ready[i] && !own.pending && !own.unsupported
                && own.unavailable==a.frameCount-ready[i] && lastStage[i]==own.lastReadyFrame)w.stages[i]=sums[i]/ready[i];
        }
        store();
    }
    void BuildSnapshot(const D::LiveSnapshot& s)
    {
        for(const auto& source:s.sources){Build(source.recent);for(const auto& a:source.recentSeconds)Build(a);}
        for(const auto& status:s.controlStatus){Build(status.baseline);Build(status.after);}
        const auto cutoff=now>RetainNs?now-RetainNs:0;
        for(auto f=frames.begin();f!=frames.end();)
            if((f->second.seal&&f->second.seal->endNs<cutoff)||(!f->second.seal&&f->second.lastStamp<latestStamp-int64_t(RetainNs)))f=frames.erase(f);else ++f;
    }
    const Window* Find(const D::LiveAggregate& a) const
    {
        if(!ValidWindow(a,now))return nullptr;
        const auto it=windows.find(Key(a));return it!=windows.end() && it->second.original.frameCount==a.frameCount?&it->second:nullptr;
    }
    MetricChoice Engine(const D::LiveAggregate& a,ComparisonMetric m) const
    {
        MetricChoice c;c.value=ComparisonNumber(a,m);c.evidence=Evidence(session,a);c.evidence.definition=Definition(m);
        c.readyFrames=m==ComparisonMetric::Cpu?a.cpuRender.ready:m==ComparisonMetric::Gpu?a.gpuMeasured.ready:
            (m==ComparisonMetric::Frame||m==ComparisonMetric::Fps)?a.frame.ready:a.frameCount;
        c.reason=m==ComparisonMetric::Gpu?"동일 범위 Tracy GPU 계측 미제공":(m==ComparisonMetric::Cpu||m==ComparisonMetric::Frame||m==ComparisonMetric::Fps)?"해당 완료 구간의 호환되는 Tracy 원본 미수집":"엔진 고유 의미 카운터";
        return c;
    }
};
TracyMetrics::TracyMetrics():impl(std::make_unique<Impl>()) {}
TracyMetrics::~TracyMetrics()=default;
void TracyMetrics::Clear() {impl=std::make_unique<Impl>();}
void TracyMetrics::Observe(const D::LiveSnapshot& s)
{
    if(impl->session!=s.session)Clear();
    const bool changed=impl->now!=s.producerTimeNs||impl->observedSequence!=s.sequence;
    impl->session=s.session;impl->now=s.producerTimeNs;impl->observedSequence=s.sequence;
    // Live publication can complete a window after its originals have already arrived.
    if(changed&&!impl->frames.empty())impl->BuildSnapshot(s);
}
void TracyMetrics::ReadMessages(const std::vector<TracyRawMessage>& messages,const D::LiveSnapshot& snapshot)
{
    Observe(snapshot);for(const auto& msg:messages){T::LiveTraceEvent e;if(T::DecodeLiveTrace(msg.text,e))impl->Consume(e,msg.timeNs);}impl->BuildSnapshot(snapshot);
}
void TracyMetrics::Read(tracy::Worker& worker,const D::LiveSnapshot& snapshot)
{
    Observe(snapshot);
#if defined(DY_VIEWER_EMBED_TRACY) && DY_VIEWER_EMBED_TRACY
    const auto identity=uint64_t(reinterpret_cast<uintptr_t>(&worker));
    if(impl->workerIdentity && (impl->workerIdentity!=identity || impl->captureTime!=worker.GetCaptureTime())){Clear();Observe(snapshot);}
    impl->workerIdentity=identity;impl->captureTime=worker.GetCaptureTime();
    if(!worker.IsConnected())return;
    const auto count=worker.GetMessages().size();
    if(impl->lastMessageCount==count&&!impl->pendingStrings)return;
    const auto tick=snapshot.producerTimeNs/D::LivePublishIntervalNs;
    const auto traceTick=worker.GetLastTime()/int64_t(D::LivePublishIntervalNs);
    if(impl->lastReadTick==tick&&impl->lastReadTraceTick==traceTick)return;
    impl->lastReadTick=tick;impl->lastReadTraceTick=traceTick;impl->lastMessageCount=count;impl->pendingStrings=false;
    std::vector<TracyRawMessage> batch;
    // Numeric message identities expire with the bounded tail; unresolved strings remain retryable.
    const auto cutoff=worker.GetLastTime()-int64_t(RetainNs);
    for(auto it=impl->seenMessages.begin();it!=impl->seenMessages.end();)
        if(it->second<cutoff)it=impl->seenMessages.erase(it);else ++it;
    for(const auto* thread:worker.GetThreadData())
    {
        const auto& messages=thread->messages;if(messages.empty())continue;
        const auto start=std::lower_bound(messages.begin(),messages.end(),cutoff,[](const auto& m,int64_t t){return m->time<t;});
        for(auto it=start;it!=messages.end();++it)
        {
            const auto* message=&**it;const auto id=reinterpret_cast<uintptr_t>(message);
            if(impl->seenMessages.count(id))continue;
            const auto* text=worker.GetString(message->ref);
            if(!text||std::string_view(text)=="???"){impl->pendingStrings=true;continue;}
            if(impl->seenMessages.size()<MessageLimit)impl->seenMessages.emplace(id,message->time);else impl->pendingStrings=true;
            batch.push_back({message->time,text});
        }
    }
    if(!batch.empty())ReadMessages(batch,snapshot);
#else
    (void)worker;
#endif
}
const char* OriginName(MetricOrigin o) {return o==MetricOrigin::Tracy?"Tracy":"자체";}
MetricChoice TracyMetrics::Select(const D::LiveAggregate& a,ComparisonMetric metric) const
{
    auto c=impl->Engine(a,metric);const auto* w=impl->Find(a);if(!w)return c;
    const auto* own=metric==ComparisonMetric::Cpu?&a.cpuRender:(metric==ComparisonMetric::Frame||metric==ComparisonMetric::Fps)?&a.frame:nullptr;
    if(!own)return c;const auto& original=metric==ComparisonMetric::Cpu?w->original.cpuRender:w->original.frame;
    if(!SameMetric(*own,original)){c.reason="원본 구간의 자체 표본 집합이 변경되어 Tracy 재검증 대기";return c;}
    const auto value=metric==ComparisonMetric::Cpu?w->cpu:w->frame;c.reason=metric==ComparisonMetric::Cpu?w->cpuReason:w->frameReason;
    if(!value)return c;
    if(metric==ComparisonMetric::Fps && *value<=0){c.reason="프레임 간격 0의 역수는 정의되지 않음";return c;}
    c.value=metric==ComparisonMetric::Fps?1000.0/ *value:*value;c.origin=MetricOrigin::Tracy;c.reason="완료 구간의 동일 원본 프레임·측정 정의 검증 완료";
    if(w->engineDetailIncomplete)c.reason+="; 자체 상세 일부 생략";
    c.evidence=w->evidence;c.evidence.definition=Definition(metric);return c;
}
MetricChoice TracyMetrics::SelectStage(const D::LiveAggregate& a,D::LiveStage stage,StageMetric metric) const
{
    MetricChoice c;c.value=StageNumber(a,stage,metric);c.evidence=Evidence(impl->session,a);
    c.evidence.definition=metric==StageMetric::Cpu?"단계별 프레임 작업 시간 합 평균 (중첩·병렬 작업도 합산)":"엔진 단계별 측정·의미 카운터";
    c.reason=metric==StageMetric::Cpu?"해당 단계 전체의 Tracy 원본 대응 미수집":"동일 정의 Tracy 대응 계측 미제공";
    if(size_t(stage)>=D::LiveStageCount)return c;
    const auto& own=metric==StageMetric::Gpu?a.stages[size_t(stage)].gpu:a.stages[size_t(stage)].cpu;c.readyFrames=metric==StageMetric::Cpu||metric==StageMetric::Gpu?own.ready:a.frameCount;
    const auto* w=impl->Find(a);if(!w||metric!=StageMetric::Cpu)return c;c.reason=w->stageReason[size_t(stage)];
    if(!SameMetric(own,w->original.stages[size_t(stage)].cpu)||!w->stages[size_t(stage)])return c;
    c.value=w->stages[size_t(stage)];c.origin=MetricOrigin::Tracy;c.reason="단계 전체의 원본 이벤트 개수·식별·측정 정의 검증 완료";
    if(w->engineDetailIncomplete)c.reason+="; 자체 상세 일부 생략";
    const auto definition=c.evidence.definition;c.evidence=w->evidence;c.evidence.definition=definition;return c;
}
MetricPair TracyMetrics::Compare(const D::LiveAggregate& a,const D::LiveAggregate& b,ComparisonMetric metric) const
{
    MetricPair p{Select(a,metric),Select(b,metric),{}};
    if(p.before.origin!=MetricOrigin::Tracy||p.after.origin!=MetricOrigin::Tracy||a.device!=b.device||a.source!=b.source
        ||p.before.evidence.tracyConnection!=p.after.evidence.tracyConnection)
    {
        const auto reason="전후 양쪽의 동일 출처를 유지: "+p.before.reason+" / "+p.after.reason;
        p.before=impl->Engine(a,metric);p.after=impl->Engine(b,metric);p.before.reason=p.after.reason=reason;
    }
    p.change=CompareNumbers(p.before.value,p.after.value);return p;
}
MetricPair TracyMetrics::CompareStage(const D::LiveAggregate& a,const D::LiveAggregate& b,D::LiveStage stage,StageMetric metric) const
{
    MetricPair p{SelectStage(a,stage,metric),SelectStage(b,stage,metric),{}};
    if(p.before.origin!=MetricOrigin::Tracy||p.after.origin!=MetricOrigin::Tracy||a.device!=b.device||a.source!=b.source
        ||p.before.evidence.tracyConnection!=p.after.evidence.tracyConnection)
    {
        const auto reason="전후 양쪽의 동일 출처를 유지: "+p.before.reason+" / "+p.after.reason;
        p.before.value=StageNumber(a,stage,metric);p.after.value=StageNumber(b,stage,metric);p.before.origin=p.after.origin=MetricOrigin::Engine;p.before.reason=p.after.reason=reason;
        const auto definition=p.before.evidence.definition;p.before.evidence=Evidence(impl->session,a);p.after.evidence=Evidence(impl->session,b);
        p.before.evidence.definition=p.after.evidence.definition=definition;
    }
    p.change=CompareNumbers(p.before.value,p.after.value);return p;
}
std::vector<MetricHistoryPoint> TracyMetrics::History(const std::vector<D::LiveAggregate>& values,ComparisonMetric metric) const
{
    std::vector<MetricHistoryPoint> points;points.reserve(values.size());
    for(const auto& a:values)
    {
        MetricHistoryPoint p{Select(a,metric),false};
        if(!points.empty())
        {
            const auto& prev=points.back().choice;const auto& e=p.choice.evidence;
            p.connectPrevious=p.choice.value&&prev.value&&p.choice.origin==prev.origin&&e.session==prev.evidence.session&&e.device==prev.evidence.device
                &&e.source==prev.evidence.source&&e.revision&&e.revision==prev.evidence.revision&&e.beginNs==prev.evidence.endNs&&!a.mixedRevision
                &&(p.choice.origin==MetricOrigin::Engine||e.tracyConnection==prev.evidence.tracyConnection);
        }
        points.push_back(std::move(p));
    }
    return points;
}
MetricOriginRanges TracyMetrics::LowHigh(const std::vector<D::LiveAggregate>& values,ComparisonMetric metric) const
{
    MetricOriginRanges ranges;for(const auto& a:values){const auto c=Select(a,metric);if(c.value)AddRange(c.origin==MetricOrigin::Tracy?ranges.tracy:ranges.engine,*c.value);}return ranges;
}
MetricOriginRanges TracyMetrics::RunLowHigh(D::LiveId device,D::LiveId source,ComparisonMetric metric) const
{
    MetricOriginRanges r;const auto it=impl->runRanges.find({device,source,metric});
    if(it!=impl->runRanges.end()){r.engine=RunRange::Range(it->second.engine);r.tracy=RunRange::Range(it->second.tracy);}return r;
}
}
