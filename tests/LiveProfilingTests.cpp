#include "dyf/Diagnostics/LiveConnection.h"
#include "dyf/Diagnostics/LiveProfiling.h"
#include <chrono>
#include <cmath>
#include <iostream>
#include <thread>
#include <atomic>
#include <type_traits>

using namespace dyf::Diagnostics;
#define CHECK(x) do { if(!(x)) { std::cerr<<__LINE__<<": " #x "\n";return 1;} } while(false)
int CallbackLifetime()
{
    struct Payload {explicit Payload(std::atomic<bool>& flag):destroyed(flag){}std::atomic<bool>& destroyed;int value=42;~Payload(){destroyed=true;}};
    int identity=0;const auto device=RegisterLiveObject(&identity,{LiveObjectKind::Device,0,0,0,0,"callback lifecycle"}),source=AllocateLiveId();
    ControlDesc desc;desc.device=device;desc.source=source;desc.owner=source;desc.key="lifetime";desc.appliedValue=false;
    std::atomic<bool> selfDestroyed{false};bool continued=false;ControlRegistration self;
    auto payload=std::make_shared<Payload>(selfDestroyed);
    self=RegisterControl(desc,[&self,&continued,&selfDestroyed,held=payload](const ControlRequest&,LiveFrameKey){self=ControlRegistration{};continued=!selfDestroyed&&held->value==42;return ControlApplyResult{ControlState::Applied,true,{}};});
    payload.reset();const auto selfId=self.GetId(),request=RequestLiveControl(selfId,true);CHECK(request);
    bool clearedBeforeNext=false;auto next=RegisterControl(desc,[&](const ControlRequest&,LiveFrameKey){clearedBeforeNext=selfDestroyed;return ControlApplyResult{ControlState::Applied,true,{}};});CHECK(RequestLiveControl(next.GetId(),true));
    {LiveFrameScope frame(device,source);CHECK(PollControls(source,frame.GetKey())==2);frame.SetOutcome(LiveOutcome::Completed);}
    CHECK(continued&&selfDestroyed&&clearedBeforeNext&&!self.GetId());auto snapshot=ReadLiveSnapshot();bool cancelled=false;for(const auto& status:snapshot->controlStatus)if(status.request==request)cancelled=status.state==ControlState::Cancelled;CHECK(cancelled);for(const auto& control:snapshot->controls)CHECK(control.id!=selfId);
    std::atomic<bool> ancestorDestroyed{false};bool nestedSafe=false;ControlRegistration ancestor;auto childDesc=desc;childDesc.owner=AllocateLiveId();
    auto child=RegisterControl(childDesc,[&ancestor](const ControlRequest&,LiveFrameKey){ancestor=ControlRegistration{};return ControlApplyResult{ControlState::Applied,true,{}};});payload=std::make_shared<Payload>(ancestorDestroyed);
    ancestor=RegisterControl(desc,[&nestedSafe,&ancestorDestroyed,childId=child.GetId(),childOwner=childDesc.owner,held=payload](const ControlRequest&,LiveFrameKey key){const auto requested=RequestLiveControl(childId,true);const auto calls=PollControls(childOwner,key);nestedSafe=requested&&calls==1&&!ancestorDestroyed&&held->value==42;return ControlApplyResult{ControlState::Applied,true,{}};});payload.reset();CHECK(RequestLiveControl(ancestor.GetId(),true));
    {LiveFrameScope frame(device,source);CHECK(PollControls(source,frame.GetKey())==1);frame.SetOutcome(LiveOutcome::Completed);}CHECK(nestedSafe&&ancestorDestroyed&&!ancestor.GetId());
    std::atomic<bool> started{false},attempted{false},returned{false},foreignDestroyed{false};bool protectedUntilReturn=false;LiveId foreignId=0;
    payload=std::make_shared<Payload>(foreignDestroyed);
    auto registration=RegisterControl(desc,[&started,&attempted,&returned,&protectedUntilReturn,&foreignDestroyed,&foreignId,held=payload](const ControlRequest&,LiveFrameKey){started=true;const auto deadline=LiveNowNs()+1000000000ull;bool removed=false;
        while(LiveNowNs()<deadline&&!removed){if(attempted){removed=true;for(const auto& control:ReadLiveSnapshot()->controls)if(control.id==foreignId)removed=false;}if(!removed)std::this_thread::yield();}
        protectedUntilReturn=removed&&!returned&&!foreignDestroyed&&held->value==42;return ControlApplyResult{ControlState::Applied,true,{}};});
    payload.reset();foreignId=registration.GetId();CHECK(RequestLiveControl(foreignId,true));
    std::thread destroy([owned=std::move(registration),&started,&attempted,&returned]()mutable{while(!started)std::this_thread::yield();attempted=true;owned=ControlRegistration{};returned=true;});
    std::uint32_t calls=0;{LiveFrameScope frame(device,source);calls=PollControls(source,frame.GetKey());frame.SetOutcome(LiveOutcome::Completed);}destroy.join();CHECK(calls==1&&protectedUntilReturn&&returned&&foreignDestroyed);
    std::cout<<"Self-unregister defers capture destruction; foreign unregister waits for callback return\n";return 0;
}
int OptimizationContracts()
{
    const auto device=RegisterLiveObject(nullptr,{LiveObjectKind::Device,0,0,0,0,"optimization contracts"}),source=AllocateLiveId();
    ControlDesc info;info.device=device;info.source=info.owner=source;info.key="structure.test";info.writable=false;
    info.category=ControlCategory::BuildOrStructural;info.description="Fixed representation";info.unavailableReason="No alternate implementation";
    auto readonly=RegisterControl(info,{});CHECK(readonly.GetId());const auto rejected=RequestLiveControl(readonly.GetId(),true);CHECK(rejected);
    {LiveFrameScope frame(device,source);CHECK(PollControls(source,frame.GetKey())==0);frame.SetOutcome(LiveOutcome::Completed);}
    const auto rejectedSnapshot=ReadLiveSnapshot();bool rejectedFound=false;for(const auto& status:rejectedSnapshot->controlStatus)if(status.request==rejected)rejectedFound=status.state==ControlState::Rejected;CHECK(rejectedFound);
    info.writable=true;CHECK(!RegisterControl(info,{}).GetId());
    const auto list=RegisterLiveObject(nullptr,{LiveObjectKind::CommandList,device,source,0,0,"native memory accounting"});
    {
        LiveFrameScope frame(device,source);LiveEvent event;event.kind=LiveEventKind::UploadMemory;event.stage=LiveStage::Upload;
        event.command={list,1,0};event.operations=1;event.bytes=4096;event.beginNs=1;event.endNs=1000001;RecordLiveEvent(event);
        event.arguments[0]=1;RecordLiveEvent(event);
        event.kind=LiveEventKind::Upload;event.bytes=16;event.beginNs=event.endNs=0;event.fullUpload=true;RecordLiveEvent(event);
        event.kind=LiveEventKind::Submission;event.stage=LiveStage::Submit;event.bytes=0;event.submission=AllocateLiveId();event.outcome=LiveOutcome::Submitted;RecordLiveEvent(event);
        event.outcome=LiveOutcome::Completed;RecordLiveEvent(event);frame.SetOutcome(LiveOutcome::Completed);
    }
    const auto snapshot=ReadLiveSnapshot();bool found=false;
    for(const auto& item:snapshot->sources)if(item.source==source){found=true;const auto& frame=item.latest;
        CHECK(frame.recorded.uploadPageAllocations==1&&frame.recorded.uploadPageReuses==1&&frame.recorded.uploadPageAllocationBytes==4096);
        CHECK(frame.recorded.uploadCalls==1&&frame.recorded.uploadBytes==16&&frame.stages[std::size_t(LiveStage::Upload)].bytes==16);
        CHECK(frame.stages[std::size_t(LiveStage::Upload)].operations==1&&frame.stages[std::size_t(LiveStage::Upload)].cpu.state==LiveValueState::Unavailable);
        CHECK(frame.submitted.uploadBytes==16&&frame.completed.uploadBytes==16&&frame.submitted.uploadPageAllocations==0&&frame.completed.uploadPageReuses==0);
    }CHECK(found);std::cout<<"Read-only catalog and isolated native upload-memory accounting passed\n";return 0;
}
int main(int argc,char** argv)
{
    if(argc==2&&std::string(argv[1])=="--optimization-contracts")return OptimizationContracts();
    if(argc==2&&std::string(argv[1])=="--callback-lifecycle")return CallbackLifetime();
    if(argc==2&&std::string(argv[1])=="--cold-lifecycle"){
        CHECK(SetLiveCollectorEnabled(false)&&!ReadLiveSnapshot()&&SetLiveCollectorEnabled(true));int identity=0;const auto id=RegisterLiveObject(&identity,{LiveObjectKind::Device,0,0,0,0,"cold lifecycle"});CHECK(id);{LiveFrameScope frame(id,id);frame.SetOutcome(LiveOutcome::Completed);}
#ifdef _WIN32
        LiveConnectionScope connection;const auto session=ReadLiveSnapshot()->session;LiveClient client;const auto deadline=LiveNowNs()+3000000000ull;bool connected=false;
        while(LiveNowNs()<deadline&&!connected){if(client.GetState()==LiveClientState::Disconnected)for(const auto& target:LiveClient::Discover())if(target.session==session){CHECK(client.Connect(target));break;}if(client.Poll())connected=true;if(client.GetState()==LiveClientState::Failed){std::cerr<<client.GetLastError()<<"\n";return 1;}std::this_thread::sleep_for(std::chrono::milliseconds(5));}CHECK(connected);
#endif
        std::cout<<"Cold disable-enable-register and connected worker shutdown passed\n";return 0;
    }
    static_assert(!std::is_copy_assignable_v<LiveContextScope> && !std::is_copy_assignable_v<LiveFrameScope> && !std::is_copy_assignable_v<LiveCpuScope>);
    CHECK(IsLiveCollectorEnabled());
    int deviceIdentity=0,sourceIdentity=0;
    const auto device=RegisterLiveObject(&deviceIdentity,{LiveObjectKind::Device,0,0,0,0,"contracts"});
    const auto source=RegisterLiveObject(&sourceIdentity,{LiveObjectKind::Renderer,device,0,0,0,"source"});
    CHECK(device && source && device!=source);
    CHECK(FindLiveObject(&sourceIdentity)==source);
    CHECK(GetLiveContext().frame.frame==0);
    {
        LiveFrameScope outer(device,source);
        const auto key=outer.GetKey();
        {
            LiveFrameScope nested(device,device);
            CHECK(nested.GetKey()==key);
            auto context=GetLiveContext();context.object=42;
            { LiveContextScope saved(context);CHECK(GetLiveContext().object==42); }
            CHECK(GetLiveContext().object==0);
            nested.SetOutcome(LiveOutcome::Completed);
        }
        CHECK(GetLiveContext().frame==key);
        outer.SetOutcome(LiveOutcome::Completed);
    }
    CHECK(GetLiveContext().frame.frame==0);
    const auto source2=AllocateLiveId();
    for(int i=0;i<3;++i) {
        LiveFrameScope frame(device,source2);
        LiveEvent event;event.context=GetLiveContext();event.kind=LiveEventKind::CpuWork;
        event.stage=LiveStage::Data;event.beginNs=10;event.endNs=10+std::uint64_t(i==0?1:i==1?3:8)*1000000;
        RecordLiveEvent(event);frame.SetOutcome(LiveOutcome::Completed);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1050));
    auto snapshot=ReadLiveSnapshot();CHECK(snapshot);
    bool aggregate=false;
    for(const auto& s:snapshot->sources)if(s.source==source2) {
        CHECK(s.recent.frameCount==3);
        CHECK(std::abs(s.recent.stages[1].cpu.average-4.0)<0.001);aggregate=true;
    }
    CHECK(aggregate);
    const auto retrySource=AllocateLiveId();LiveFrameKey retryKey;
    {LiveFrameScope frame(device,retrySource);frame.SetOutcome(LiveOutcome::Completed);}
    const auto beforeRetry=ReadLiveSnapshot();
    for(unsigned i=0;i<10000;++i){LiveFrameScope frame(device,retrySource);if(i==0)retryKey=frame.GetKey();CHECK(frame.GetKey()==retryKey);LiveEvent wait;wait.id=retryKey.frame;wait.kind=LiveEventKind::Wait;wait.stage=LiveStage::Output;wait.outcome=LiveOutcome::Skipped;wait.beginNs=LiveNowNs();wait.endNs=wait.beginNs+1000;wait.operations=1;RecordLiveEvent(wait);frame.SetOutcome(LiveOutcome::Skipped);}
    CHECK(GetLiveContext().frame.frame==0);snapshot=ReadLiveSnapshot();CHECK(snapshot->collector.retainedFrames==beforeRetry->collector.retainedFrames+1&&snapshot->loss.frames==beforeRetry->loss.frames);
    for(const auto& s:snapshot->sources)if(s.source==retrySource)CHECK(s.latest.key!=retryKey&&s.recent.frameCount==0);
    std::this_thread::sleep_for(std::chrono::milliseconds(1050));
    {LiveFrameScope frame(device,retrySource);CHECK(frame.GetKey()==retryKey);LiveEvent draw;draw.kind=LiveEventKind::Command;draw.commandKind=LiveCommandKind::Draw;draw.arguments={3,1,0,0,0};RecordLiveEvent(draw);frame.SetOutcome(LiveOutcome::Completed);}
    snapshot=ReadLiveSnapshot();for(const auto& s:snapshot->sources)if(s.source==retrySource){CHECK(s.latest.key==retryKey&&s.latest.outcome==LiveOutcome::Completed&&s.latest.recorded.waits==10000&&s.latest.recorded.draw==1&&s.latest.cpuRender.milliseconds<1000);CHECK(s.flow.events.size()==2&&s.flow.events[0].operations==10000&&!s.longFrames.empty());}
    const auto nestedSkipSource=AllocateLiveId();LiveFrameKey computed;
    {LiveFrameScope outer(device,nestedSkipSource);computed=outer.GetKey();LiveEvent e;e.kind=LiveEventKind::Command;e.commandKind=LiveCommandKind::Dispatch;RecordLiveEvent(e);{LiveFrameScope inner(device,nestedSkipSource);inner.SetOutcome(LiveOutcome::Skipped);}outer.SetOutcome(LiveOutcome::Completed);}
    snapshot=ReadLiveSnapshot();for(const auto& s:snapshot->sources)if(s.source==nestedSkipSource)CHECK(s.latest.key==computed&&s.latest.outcome==LiveOutcome::Skipped&&s.latest.recorded.dispatch==1);
    {LiveFrameScope frame(device,nestedSkipSource);CHECK(frame.GetKey()!=computed);frame.SetOutcome(LiveOutcome::Completed);}
    {LiveFrameScope outer(device,nestedSkipSource);{LiveFrameScope failed(device,nestedSkipSource);failed.SetOutcome(LiveOutcome::Skipped);}LiveEvent presented;presented.kind=LiveEventKind::Output;presented.commandKind=LiveCommandKind::Present;presented.outcome=LiveOutcome::Completed;RecordLiveEvent(presented);outer.SetOutcome(LiveOutcome::Completed);}
    snapshot=ReadLiveSnapshot();for(const auto& s:snapshot->sources)if(s.source==nestedSkipSource)CHECK(s.latest.outcome==LiveOutcome::Completed);
    const auto overlapSource=AllocateLiveId();LiveFrameKey firstEndedLast,secondEndedFirst;
    {LiveFrameScope first(device,overlapSource);firstEndedLast=first.GetKey();std::thread later([&]{LiveFrameScope second(device,overlapSource);secondEndedFirst=second.GetKey();second.SetOutcome(LiveOutcome::Completed);});later.join();std::this_thread::sleep_for(std::chrono::milliseconds(2));first.SetOutcome(LiveOutcome::Completed);}
    snapshot=ReadLiveSnapshot();for(const auto& s:snapshot->sources)if(s.source==overlapSource)CHECK(s.latest.key==firstEndedLast&&s.latest.key!=secondEndedFirst&&s.latest.frame.state==LiveValueState::Ready&&s.latest.frame.milliseconds>0);
    {
        const auto outsideList=RegisterLiveObject(nullptr,{LiveObjectKind::CommandList,device,0,0,0,"deferred list"});LiveEventSummary deferred;LiveEvent draw;draw.kind=LiveEventKind::Command;draw.commandKind=LiveCommandKind::Draw;draw.command={outsideList,1,1};draw.arguments={3,2,0,0,0};
        for(unsigned i=0;i<100000;++i)AccumulateLiveEventSummary(deferred,draw);deferred.omittedEvents=99998;deferred.omittedLinks=99999;
        std::size_t reserved=0;while(ReserveLiveDetailBytes(1024u*1024u))reserved+=1024u*1024u;CHECK(reserved&&ReadLiveSnapshot()->collector.retainedDetailBytes<=32u*1024u*1024u);
        const auto deferredSource=AllocateLiveId();{LiveFrameScope frame(device,deferredSource);RecordLiveEventSummary(draw.command,deferred);RecordLiveEvent(draw,false);RecordLiveEvent(draw,false);LiveEvent submit;submit.kind=LiveEventKind::Submission;submit.command=draw.command;submit.submission=AllocateLiveId();submit.operations=1;submit.outcome=LiveOutcome::Submitted;RecordLiveEvent(submit);RetireLiveObject(outsideList);submit.outcome=LiveOutcome::Completed;RecordLiveEvent(submit);frame.SetOutcome(LiveOutcome::Completed);}
        ReleaseLiveDetailBytes(reserved);snapshot=ReadLiveSnapshot();for(const auto& s:snapshot->sources)if(s.source==deferredSource)CHECK(s.latest.recorded.draw==100000&&s.latest.submitted.draw==100000&&s.latest.completed.draw==100000&&s.latest.completed.instances==200000&&s.latest.incomplete&&s.flow.omittedEvents>=99998&&s.flow.omittedLinks==99999);
    }
    const auto cappedSource=AllocateLiveId();LiveFrameKey capped;LiveId cappedRevision=0;
    {
        LiveFrameScope frame(device,cappedSource);capped=frame.GetKey();cappedRevision=GetLiveContext().appliedRevision;
        for(unsigned i=0;i<8192;++i){LiveEvent e;e.kind=LiveEventKind::CpuWork;RecordLiveEvent(e);}
        for(unsigned i=0;i<16384;++i)RecordLiveLink({i+1,i+2});
        frame.SetOutcome(LiveOutcome::Completed);
    }
    auto heldAtCap=ReadLiveSnapshot();
    {LiveContextScope restore({capped,cappedRevision,cappedSource,0,0});LiveEvent e;e.kind=LiveEventKind::CpuWork;RecordLiveEvent(e);RecordLiveLink({1,2});}
    snapshot=ReadLiveSnapshot();CHECK(snapshot->loss.events>heldAtCap->loss.events&&snapshot->loss.links>heldAtCap->loss.links);
    for(const auto& s:snapshot->sources)if(s.source==cappedSource)CHECK(s.latest.incomplete&&s.flow.omittedEvents==1&&s.flow.omittedLinks==1&&s.flow.events.size()==8192&&s.flow.links.size()==16384);
    for(const auto& s:heldAtCap->sources)if(s.source==cappedSource)CHECK(!s.latest.incomplete&&s.flow.omittedEvents==0&&s.flow.omittedLinks==0);
    const auto countsSource=AllocateLiveId();
    {
        LiveFrameScope frame(device,countsSource);
        const auto texture=RegisterLiveObject(nullptr,{LiveObjectKind::Texture,device,0,1,4,"idempotent texture"});RetireLiveObject(texture);RetireLiveObject(texture);frame.SetOutcome(LiveOutcome::Completed);
    }
    snapshot=ReadLiveSnapshot();for(const auto& s:snapshot->sources)if(s.source==countsSource)CHECK(s.latest.recorded.objectsDestroyed==1);
    {
        const auto owner=AllocateLiveId();const auto before=ReadLiveSnapshot()->collector.batches;
        const auto sceneBatch=GetLiveBatchId(owner,1);const auto passBatch=GetLiveBatchId(sceneBatch,2);CHECK(GetLiveBatchId(passBatch,3)==GetLiveBatchId(passBatch,3));
        CHECK(ReadLiveSnapshot()->collector.batches==before+3);RetireLiveObject(owner);CHECK(ReadLiveSnapshot()->collector.batches==before);
    }
    const auto gpuSource=AllocateLiveId();LiveFrameKey original;LiveId oldRevision=0;
    {LiveFrameScope frame(device,gpuSource);original=frame.GetKey();oldRevision=GetLiveContext().appliedRevision;frame.SetOutcome(LiveOutcome::Completed);}
    auto heldBeforeLate=ReadLiveSnapshot();
    {LiveContextScope restore({original,oldRevision,gpuSource,0,0});LiveEvent late;late.id=AllocateLiveId();late.kind=LiveEventKind::Submission;late.outcome=LiveOutcome::Submitted;late.operations=1;late.submission=late.id;RecordLiveEvent(late);RecordLiveLink({late.id,12345});}
    snapshot=ReadLiveSnapshot();for(const auto& s:snapshot->sources)if(s.source==gpuSource)CHECK(!s.flow.events.empty()&&!s.flow.links.empty());for(const auto& s:heldBeforeLate->sources)if(s.source==gpuSource)CHECK(s.flow.events.empty()&&s.flow.links.empty());
    LiveGpuSpan a;a.id=AllocateLiveId();a.frame=original;a.appliedRevision=oldRevision;a.aggregation=LiveGpuAggregation::FrameCoverage;a.duration={LiveValueState::Pending,0};RecordLiveGpuSpan(a);
    auto b=a;b.id=AllocateLiveId();RecordLiveGpuSpan(b);SetLiveGpuCoverage(original,true);
    {LiveFrameScope newer(device,gpuSource);NotifyLiveSettingsApplied(gpuSource,newer.GetKey(),true);newer.SetOutcome(LiveOutcome::Completed);}
    a.duration={LiveValueState::Ready,2};a.hasQueueInterval=true;a.gpuClockDomain=device;a.beginGpuNs=0;a.endGpuNs=2000000;RecordLiveGpuSpan(a);
    b.duration={LiveValueState::Ready,2};b.hasQueueInterval=true;b.gpuClockDomain=device;b.beginGpuNs=1000000;b.endGpuNs=3000000;RecordLiveGpuSpan(b);
    snapshot=ReadLiveSnapshot();for(const auto& s:snapshot->sources)if(s.source==gpuSource){CHECK(s.latest.appliedRevision!=oldRevision);CHECK(s.recent.gpuMeasured.ready==0);CHECK(s.recentGpuSpans.size()==2);bool unionFound=false;for(const auto& h:s.history)if(h.appliedRevision==oldRevision){CHECK(h.gpuMeasured.ready==1&&std::abs(h.gpuMeasured.average-3)<0.001);unionFound=true;}CHECK(unionFound);}
    std::this_thread::sleep_for(std::chrono::milliseconds(1050));
    {LiveFrameScope frame(device,gpuSource);frame.SetOutcome(LiveOutcome::Completed);}
    snapshot=ReadLiveSnapshot();for(const auto& s:snapshot->sources)if(s.source==gpuSource)CHECK(!s.longFrames.empty()&&s.longFrames.back().frame.milliseconds>=1000);
    const auto concurrentSource=AllocateLiveId();
    {
        LiveFrameScope frame(device,concurrentSource);
        std::atomic<bool> running{true};
        std::thread reader([&]{while(running) (void)ReadLiveSnapshot();});
        for(unsigned i=0;i<4096;++i){LiveEvent e;e.kind=LiveEventKind::Command;e.commandKind=LiveCommandKind::Draw;e.arguments={3,1,0,0,0};RecordLiveEvent(e);}
        running=false;reader.join();frame.SetOutcome(LiveOutcome::Completed);
    }
    snapshot=ReadLiveSnapshot();
    for(const auto& s:snapshot->sources)if(s.source==concurrentSource)CHECK(s.latest.recorded.draw==4096);
    bool applied=false;
    unsigned calls=0;
    ControlDesc desc;desc.device=device;desc.source=source;desc.owner=source;
    desc.key="toggle";desc.label="toggle";desc.appliedValue=false;
    auto registration=RegisterControl(desc,[&](const ControlRequest& r,LiveFrameKey){
        ++calls;applied=std::get<bool>(r.value);
        return ControlApplyResult{ControlState::Applied,applied,{}};
    });
    CHECK(registration.GetId());
    const auto old=RequestLiveControl(registration.GetId(),false);
    const auto latest=RequestLiveControl(registration.GetId(),true);
    CHECK(old && latest && old!=latest && !applied);
    { LiveFrameScope frame(device,source);CHECK(PollControls(source,frame.GetKey())==1);frame.SetOutcome(LiveOutcome::Completed); }
    CHECK(applied && calls==1);
    snapshot=ReadLiveSnapshot();CHECK(snapshot);
    LiveId revision=0;
    bool cancelled=false,complete=false;
    for(const auto& status:snapshot->controlStatus) {
        if(status.request==old)cancelled=status.state==ControlState::Cancelled && status.message=="Superseded";
        if(status.request==latest){complete=status.state==ControlState::Applied && status.appliedFrame.frame!=0;revision=status.appliedRevision;}
    }
    CHECK(cancelled && complete);
    const auto noop=RequestLiveControl(registration.GetId(),true);
    {LiveFrameScope frame(device,source);PollControls(source,frame.GetKey());frame.SetOutcome(LiveOutcome::Completed);}
    snapshot=ReadLiveSnapshot();
    for(const auto& status:snapshot->controlStatus)if(status.request==noop)CHECK(status.appliedRevision==revision);
    CHECK(calls==1);
    const auto submittedSource=AllocateLiveId();
    const auto listId=RegisterLiveObject(nullptr,{LiveObjectKind::CommandList,device,0,0,0,"retained list"});
    {
        LiveFrameScope frame(device,submittedSource);
        LiveEvent draw;draw.kind=LiveEventKind::Command;draw.commandKind=LiveCommandKind::Draw;draw.command={listId,1,1};draw.arguments={3,4,0,0,0};RecordLiveEvent(draw);
        LiveEvent submit;submit.kind=LiveEventKind::Submission;submit.command={listId,1,0};submit.submission=AllocateLiveId();submit.operations=1;submit.outcome=LiveOutcome::Submitted;RecordLiveEvent(submit);
        RetireLiveObject(listId);submit.outcome=LiveOutcome::Completed;RecordLiveEvent(submit);
        frame.SetOutcome(LiveOutcome::Completed);
    }
    snapshot=ReadLiveSnapshot();for(const auto& s:snapshot->sources)if(s.source==submittedSource)CHECK(s.latest.completed.draw==1 && s.latest.completed.instances==4);
    unsigned secondCalls=0;
    std::atomic<bool> callbackStarted{false},replacementQueued{false};
    auto first=RegisterControl(desc,[&](const ControlRequest&,LiveFrameKey){callbackStarted=true;while(!replacementQueued)std::this_thread::yield();return ControlApplyResult{ControlState::Applied,true,{}};});
    auto second=RegisterControl(desc,[&](const ControlRequest&,LiveFrameKey){++secondCalls;return ControlApplyResult{ControlState::Applied,true,{}};});
    const auto firstRequest=RequestLiveControl(first.GetId(),true);(void)firstRequest;
    const auto secondRequest=RequestLiveControl(second.GetId(),true);
    std::thread replace([&]{while(!callbackStarted)std::this_thread::yield();RequestLiveControl(second.GetId(),false);replacementQueued=true;});
    {LiveFrameScope frame(device,source);PollControls(source,frame.GetKey());frame.SetOutcome(LiveOutcome::Completed);}
    replace.join();CHECK(secondCalls==0);
    snapshot=ReadLiveSnapshot();for(const auto& r:snapshot->controlStatus)if(r.request==secondRequest)CHECK(r.state==ControlState::Cancelled);
    {LiveFrameScope active(device,source);snapshot=ReadLiveSnapshot();for(const auto& s:snapshot->sources)if(s.source==source)CHECK(s.latest.key!=active.GetKey());active.SetOutcome(LiveOutcome::Completed);}
    for(unsigned i=0;i<8300;++i){const auto id=AllocateLiveId();{LiveFrameScope frame(device,id);frame.SetOutcome(LiveOutcome::Completed);}RetireLiveObject(id);}
    snapshot=ReadLiveSnapshot();CHECK(snapshot->collector.retainedFrames<=8192);CHECK(snapshot->collector.retainedDetailBytes<=32u*1024u*1024u);CHECK(snapshot->collector.commandGenerations<=4096);
    CHECK(RequestLiveControl(registration.GetId(),std::int64_t(1))!=0);
    {LiveFrameScope frame(device,source);PollControls(source,frame.GetKey());frame.SetOutcome(LiveOutcome::Completed);}
    CHECK(calls==1);
    auto held=snapshot;
    RetireLiveObject(source);CHECK(!FindLiveObject(&sourceIdentity));
    CHECK(held->objects.size()>=2);
    const auto expiringSource=AllocateLiveId(),batchOwner=AllocateLiveId();
    {LiveFrameScope frame(device,expiringSource);frame.SetOutcome(LiveOutcome::Skipped);}
    const auto expiringBatch=GetLiveBatchId(batchOwner,1);auto heldBeforeExpiry=ReadLiveSnapshot();RetireLiveObject(expiringSource);
    heldBeforeExpiry=ReadLiveSnapshot();bool discarded=false;for(const auto& s:heldBeforeExpiry->sources)if(s.source==expiringSource)discarded=s.latest.outcome==LiveOutcome::Discarded;CHECK(discarded);
    const auto disabledSource=AllocateLiveId();LiveFrameKey disabled;
    {LiveFrameScope frame(device,disabledSource);disabled=frame.GetKey();CHECK(SetLiveCollectorEnabled(false));frame.SetOutcome(LiveOutcome::Completed);}
    CHECK(GetLiveContext().frame.frame==0&&SetLiveCollectorEnabled(true));snapshot=ReadLiveSnapshot();for(const auto& s:snapshot->sources)if(s.source==disabledSource)CHECK(s.latest.key==disabled&&s.latest.outcome==LiveOutcome::Discarded&&s.latest.incomplete);
    const auto disabledRetrySource=AllocateLiveId();{LiveFrameScope frame(device,disabledRetrySource);frame.SetOutcome(LiveOutcome::Skipped);}CHECK(SetLiveCollectorEnabled(false)&&SetLiveCollectorEnabled(true));snapshot=ReadLiveSnapshot();for(const auto& s:snapshot->sources)if(s.source==disabledRetrySource)CHECK(s.latest.outcome==LiveOutcome::Discarded);
    std::this_thread::sleep_for(std::chrono::milliseconds(10100));
    CHECK(GetLiveBatchId(batchOwner,1)!=expiringBatch);snapshot=ReadLiveSnapshot();
    for(const auto& s:snapshot->sources)CHECK(s.source!=expiringSource);
    bool retainedInHeld=false;for(const auto& s:heldBeforeExpiry->sources)retainedInHeld|=s.source==expiringSource;CHECK(retainedInHeld);
    std::cout<<"Live collector contracts passed\n";
}
