#include "LiveInternal.h"
#include <cmath>
#include <iostream>
#include <stdexcept>

using namespace dyf::Diagnostics;
#define CHECK(x) do { if(!(x))throw std::runtime_error(std::to_string(__LINE__)+": " #x); } while(false)
namespace
{
constexpr std::uint64_t Second=1000000000;
void At(std::uint64_t now) {Detail::SetTestLiveNowNs(now);}
LiveSourceSnapshot Source(LiveId source)
{
    const auto snapshot=ReadLiveSnapshot();
    for(const auto& value:snapshot->sources)if(value.source==source)return value;
    throw std::runtime_error("missing source");
}
ControlStatus Status(LiveId request)
{const auto snapshot=ReadLiveSnapshot();for(const auto& value:snapshot->controlStatus)if(value.request==request)return value;throw std::runtime_error("missing request");}
LiveFrameKey Frame(LiveId device,LiveId source,std::uint64_t begin,std::uint64_t end,std::uint64_t draws=0,bool revise=false)
{
    At(begin);const auto token=BeginLiveFrame(device,source);CHECK(token.key.frame);
    if(revise)NotifyLiveSettingsApplied(source,token.key,true);
    LiveEventSummary counts;counts.recorded.draw=draws;counts.recorded.instances=draws*2;
    counts.recorded.batches=draws;counts.recorded.uploadBytes=draws*64;
    counts.stages[1].elements=draws;counts.stages[1].cpu={LiveValueState::Ready,double(draws)};
    RecordLiveEventSummary({},counts);SetLiveObjectGauge(token.key,{LiveValueState::Ready,draws});
    At(end);EndLiveFrame(token,LiveOutcome::Completed);return token.key;
}
LiveGpuSpan Gpu(LiveFrameKey key,LiveId revision,double ms,LiveValueState state=LiveValueState::Ready)
{
    LiveGpuSpan span;span.id=AllocateLiveId();span.frame=key;span.appliedRevision=revision;
    span.aggregation=LiveGpuAggregation::FrameCoverage;span.duration={state,ms};
    span.gpuClockDomain=key.device;span.hasQueueInterval=true;span.endGpuNs=std::uint64_t(ms*1e6);
    RecordLiveGpuSpan(span);SetLiveGpuCoverage(key,true);return span;
}
bool Equal(double a,double b) {return std::abs(a-b)<0.000001;}
}
int main()
{
    try {
        At(Second);const auto device=RegisterLiveObject(nullptr,{LiveObjectKind::Device,0,0,0,0,"aggregation contract"});
        const auto source=AllocateLiveId();
        const auto sourceFirst=Frame(device,source,Second,Second+100000000,2);
        CHECK(Source(source).recent.frameCount==0&&Source(source).runRanges.drawCalls.state==LiveValueState::Unavailable);
        Frame(device,source,Second+200000000,Second+300000000,6,true);
        At(2*Second-1);CHECK(Source(source).recent.frameCount==0);
        At(2*Second);auto value=Source(source);
        CHECK(value.recent.beginNs==Second&&value.recent.endNs==2*Second&&value.recent.frameCount==2);
        CHECK(value.recent.mixedRevision&&value.recent.appliedRevision==0);
        CHECK(value.recentSeconds.size()==1&&value.recentSeconds[0].mixedRevision&&value.recentSeconds[0].frameCount==2);
        CHECK(Equal(value.recent.recordedMean.draw,4)&&Equal(value.recent.cpuRender.average,100));
        CHECK(Equal(value.recent.stages[1].cpu.average,4)&&Equal(value.recent.stages[1].meanElements,4));
        CHECK(Equal(value.runRanges.drawCalls.low,4)&&Equal(value.runRanges.drawCalls.high,4)&&value.runRanges.drawCalls.windows==1);
        CHECK(Equal(value.runRanges.frame.low,200)&&Equal(value.runRanges.fps.low,5));
        {
            LiveContextScope original({sourceFirst,1,source,0,0});LiveEventSummary late;late.stages[1].cpu={LiveValueState::Ready,2};RecordLiveEventSummary({},late);
            LiveEvent submission;submission.kind=LiveEventKind::Submission;submission.submission=AllocateLiveId();submission.operations=1;submission.outcome=LiveOutcome::Submitted;RecordLiveEvent(submission);submission.outcome=LiveOutcome::Completed;RecordLiveEvent(submission);
            const auto object=RegisterLiveObject(nullptr,{LiveObjectKind::Buffer,device,source,0,0,"late object"});RetireLiveObject(object);
        }
        value=Source(source);CHECK(value.recent.frameCount==2&&value.recent.stages[1].cpu.ready==2&&Equal(value.recent.stages[1].cpu.average,5));
        CHECK(value.recent.submittedTotal.submissions==1&&value.recent.completedTotal.submissions==1&&value.recent.recordedTotal.objectsCreated==1&&value.recent.recordedTotal.objectsDestroyed==1);
        Frame(device,source,2*Second,2*Second+100000000,0);
        At(3*Second);value=Source(source);
        CHECK(value.recent.frameCount==1&&!value.recent.mixedRevision&&value.recent.appliedRevision==2);
        CHECK(value.runRanges.drawCalls.state==LiveValueState::Ready&&value.runRanges.drawCalls.low==0&&value.runRanges.drawCalls.high==4);
        CHECK(value.runRanges.objects.state==LiveValueState::Ready&&value.runRanges.objects.low==0);
        CHECK(value.recentSeconds.size()==2&&value.recentSeconds[0].recordedMean.draw==4&&value.recentSeconds[1].recordedMean.draw==0);
        At(4*Second);value=Source(source);CHECK(value.recent.frameCount==0&&value.recent.cpuRender.state==LiveValueState::Unavailable&&value.runRanges.drawCalls.windows==2);

        const auto gpuSource=AllocateLiveId();const auto first=Frame(device,gpuSource,4*Second,4*Second+100000000);
        auto firstGpu=Gpu(first,1,100);const auto second=Frame(device,gpuSource,4*Second+200000000,4*Second+300000000);
        auto secondGpu=Gpu(second,1,0,LiveValueState::Pending);
        At(5*Second);value=Source(gpuSource);
        CHECK(value.recent.gpuMeasured.ready==1&&value.recent.gpuMeasured.pending==1&&value.runRanges.gpuMeasured.state==LiveValueState::Unavailable);
        secondGpu.duration={LiveValueState::Ready,2};secondGpu.endGpuNs=2000000;RecordLiveGpuSpan(secondGpu);
        value=Source(gpuSource);CHECK(Equal(value.runRanges.gpuMeasured.high,51)&&value.runRanges.gpuMeasured.windows==1);
        firstGpu.duration={LiveValueState::Ready,4};firstGpu.endGpuNs=4000000;RecordLiveGpuSpan(firstGpu);
        value=Source(gpuSource);CHECK(Equal(value.runRanges.gpuMeasured.low,3)&&Equal(value.runRanges.gpuMeasured.high,3)&&Equal(value.recent.gpuMeasured.maximum,4));
        Frame(device,gpuSource,5*Second,5*Second+100000000);
        const auto third=Source(gpuSource).latest.key;auto abandoned=Gpu(third,1,0,LiveValueState::Pending);
        const auto zeroSource=AllocateLiveId();const auto zeroKey=Frame(device,zeroSource,6*Second,6*Second+100000000);
        Gpu(zeroKey,1,0);At(7*Second);value=Source(zeroSource);
        CHECK(value.runRanges.gpuMeasured.state==LiveValueState::Ready&&value.runRanges.gpuMeasured.low==0&&value.runRanges.gpuMeasured.high==0&&value.runRanges.gpuMeasured.windows==1);
        At(17*Second);value=Source(gpuSource);
        CHECK(value.recent.frameCount==0&&value.runRanges.gpuMeasured.windows==1&&Equal(value.runRanges.gpuMeasured.high,3));
        abandoned.duration={LiveValueState::Ready,999};abandoned.endGpuNs=999000000;RecordLiveGpuSpan(abandoned);
        value=Source(gpuSource);CHECK(value.runRanges.gpuMeasured.windows==1&&Equal(value.runRanges.gpuMeasured.high,3));

        // No snapshot/client between these original frames: exceed the raw-frame cap in one second.
        const auto highFps=AllocateLiveId();At(20*Second);std::size_t reserved=0;
        while(ReserveLiveDetailBytes(1024*1024))reserved+=1024*1024;
        for(std::uint64_t i=0;i<9000;++i)Frame(device,highFps,20*Second+i*10000,20*Second+i*10000+1000,i<4500?1:3);
        ReleaseLiveDetailBytes(reserved);
        At(21*Second);value=Source(highFps);
        CHECK(value.recent.frameCount==9000&&value.recent.recordedTotal.draw==18000&&Equal(value.recent.recordedMean.draw,2));
        CHECK(Equal(value.recent.cpuRender.average,0.001)&&Equal(value.recent.stages[1].cpu.average,2)&&Equal(value.recent.stages[1].meanElements,2));
        CHECK(Equal(value.runRanges.drawCalls.low,2)&&Equal(value.runRanges.instances.low,4)&&Equal(value.runRanges.uploadBytes.low,128));
        At(40*Second);Frame(device,highFps,40*Second,40*Second+1000000,4,true);
        At(41*Second);value=Source(highFps);
        CHECK(value.runRanges.drawCalls.windows==2&&Equal(value.runRanges.drawCalls.low,2)&&Equal(value.runRanges.drawCalls.high,4));
        CHECK(value.recent.appliedRevision==2&&!value.recent.mixedRevision);
        const auto failedSource=AllocateLiveId();const auto failedKey=Frame(device,failedSource,42*Second,42*Second+100000000,2);
        auto failedGpu=Gpu(failedKey,1,2);At(43*Second);value=Source(failedSource);
        CHECK(value.runRanges.gpuMeasured.state==LiveValueState::Ready&&Equal(value.runRanges.gpuMeasured.high,2));
        const auto lossBefore=ReadLiveSnapshot()->loss.frames;
        Detail::FailTestLiveAggregationAllocationAfter(1);failedGpu.endGpuNs=4000000;failedGpu.duration.milliseconds=4;RecordLiveGpuSpan(failedGpu);
        value=Source(failedSource);
        CHECK(value.latest.recorded.draw==2&&Equal(value.latest.gpuMeasured.milliseconds,4)&&value.recent.frameCount==0&&value.recent.cpuRender.state==LiveValueState::Unavailable);
        CHECK(value.runRanges.drawCalls.state==LiveValueState::Unavailable&&value.runRanges.gpuMeasured.state==LiveValueState::Unavailable&&value.recentSeconds.empty());
        failedGpu.endGpuNs=6000000;failedGpu.duration.milliseconds=6;RecordLiveGpuSpan(failedGpu);
        value=Source(failedSource);CHECK(value.recent.frameCount==0&&value.runRanges.drawCalls.windows==0&&ReadLiveSnapshot()->loss.frames>lossBefore);
        Frame(device,failedSource,43*Second,43*Second+100000000,4);At(44*Second);value=Source(failedSource);
        CHECK(value.recent.frameCount==1&&value.runRanges.drawCalls.windows==1&&Equal(value.runRanges.drawCalls.low,4)&&value.recentSeconds.size()==1);
        const auto bornFailed=AllocateLiveId();At(45*Second);const auto bornToken=BeginLiveFrame(device,bornFailed);
        Detail::FailTestLiveAggregationAllocationAfter(0);At(45*Second+100000000);EndLiveFrame(bornToken,LiveOutcome::Completed);
        Frame(device,bornFailed,45*Second+200000000,45*Second+300000000,999);At(46*Second);value=Source(bornFailed);
        CHECK(value.latest.recorded.draw==999&&value.recent.frameCount==0&&value.recentSeconds.empty()&&value.runRanges.drawCalls.windows==0);
        const auto compared=AllocateLiveId();ControlDesc desc;desc.device=device;desc.source=desc.owner=compared;desc.key="fixed-comparison";
        auto comparison=RegisterControl(desc,[](const ControlRequest& request,LiveFrameKey){return ControlApplyResult{ControlState::Applied,request.value,{}};});CHECK(comparison.GetId());
        Frame(device,compared,50*Second,50*Second+100000000,2);
        const auto change=[&](bool enabled,std::uint64_t begin,std::uint64_t end,std::uint64_t draws){
            At(begin);const auto token=BeginLiveFrame(device,compared);const auto request=RequestLiveControl(comparison.GetId(),enabled);CHECK(request&&PollControls(compared,token.key)==1);
            LiveEventSummary counts;counts.recorded.draw=draws;RecordLiveEventSummary({},counts);At(end);EndLiveFrame(token,LiveOutcome::Completed);return request;
        };
        const auto firstChange=change(true,50*Second+400000000,50*Second+500000000,4);
        CHECK(Status(firstChange).baseline.frameCount==0);
        const auto comparedGpuKey=Frame(device,compared,51*Second+100000000,51*Second+200000000,8);
        auto comparedGpu=Gpu(comparedGpuKey,2,0,LiveValueState::Pending);At(51*Second+500000000);
        CHECK(Status(firstChange).after.frameCount==0);
        At(52*Second);auto comparedStatus=Status(firstChange);
        CHECK(comparedStatus.after.beginNs==51*Second&&comparedStatus.after.endNs==52*Second&&comparedStatus.after.frameCount==1&&comparedStatus.after.recordedMean.draw==8);
        CHECK(comparedStatus.after.gpuMeasured.state==LiveValueState::Pending);
        auto comparedEvidence=Source(compared).comparisonEvidence;
        CHECK(comparedEvidence.request==firstChange&&!comparedEvidence.before.available&&comparedEvidence.after.available);
        CHECK(comparedEvidence.after.summary.key==comparedGpuKey&&comparedEvidence.after.flow.key==comparedGpuKey&&comparedEvidence.after.windowBeginNs==51*Second&&comparedEvidence.after.windowRevision==2);
        CHECK(comparedEvidence.after.summary.gpuMeasured.state==LiveValueState::Pending&&!std::get<bool>(comparedStatus.previousActual));
        comparedGpu.duration={LiveValueState::Ready,6};comparedGpu.endGpuNs=6000000;RecordLiveGpuSpan(comparedGpu);
        comparedStatus=Status(firstChange);CHECK(comparedStatus.after.gpuMeasured.state==LiveValueState::Ready&&Equal(comparedStatus.after.gpuMeasured.average,6)&&comparedStatus.after.gpuMeasured.lastReadyFrame==comparedGpuKey);
        comparedEvidence=Source(compared).comparisonEvidence;CHECK(comparedEvidence.after.summary.key==comparedGpuKey&&Equal(comparedEvidence.after.summary.gpuMeasured.milliseconds,6)&&comparedEvidence.after.flow.gpuSpans[0].frame==comparedGpuKey);
        LiveEvent completed;completed.id=AllocateLiveId();completed.context={comparedGpuKey,2,compared,0,0};completed.kind=LiveEventKind::Submission;completed.outcome=LiveOutcome::Completed;completed.operations=1;RecordLiveEvent(completed);
        {LiveContextScope original(completed.context);RecordLiveLink({device,completed.id});}
        comparedEvidence=Source(compared).comparisonEvidence;CHECK(comparedEvidence.after.flow.events.size()==1&&comparedEvidence.after.flow.events[0].id==completed.id&&comparedEvidence.after.flow.links.size()==1&&comparedEvidence.after.flow.links[0].to==completed.id);
        CHECK(comparedEvidence.after.summary.completed.submissions==1&&comparedEvidence.after.loss.events==0&&comparedEvidence.after.loss.links==0&&comparedEvidence.after.loss.inventory==0&&!comparedEvidence.after.budgetLimited);
        const auto secondChange=change(false,52*Second+100000000,52*Second+200000000,3);
        comparedStatus=Status(secondChange);CHECK(comparedStatus.baseline.beginNs==51*Second&&comparedStatus.baseline.endNs==52*Second&&comparedStatus.baseline.recordedMean.draw==8);
        comparedEvidence=Source(compared).comparisonEvidence;CHECK(comparedEvidence.request==secondChange&&comparedEvidence.before.available&&comparedEvidence.before.summary.key==comparedGpuKey&&!comparedEvidence.after.available&&std::get<bool>(comparedStatus.previousActual));
        const auto thirdChange=change(true,52*Second+300000000,52*Second+400000000,5);
        At(53*Second);CHECK(Source(compared).recent.mixedRevision&&Status(thirdChange).baseline.frameCount==0&&Status(thirdChange).after.frameCount==0&&Status(secondChange).after.frameCount==0);
        Frame(device,compared,53*Second,53*Second+100000000,10);At(54*Second);
        CHECK(Status(thirdChange).after.beginNs==53*Second&&Status(thirdChange).after.recordedMean.draw==10);
        Frame(device,compared,54*Second,54*Second+100000000,14);At(55*Second);
        CHECK(Status(thirdChange).after.beginNs==54*Second&&Status(thirdChange).after.recordedMean.draw==14);
        comparedStatus=Status(firstChange);CHECK(comparedStatus.after.beginNs==51*Second&&comparedStatus.after.recordedMean.draw==8);
        comparedGpu.duration.milliseconds=3;comparedGpu.endGpuNs=3000000;RecordLiveGpuSpan(comparedGpu);
        CHECK(Equal(Status(firstChange).after.gpuMeasured.average,3)&&Equal(Status(secondChange).baseline.gpuMeasured.average,3));
        At(57*Second);CHECK(Status(thirdChange).after.frameCount==0&&Status(thirdChange).after.cpuRender.state==LiveValueState::Unavailable);
        const auto boundedSource=AllocateLiveId();ControlDesc boundedDesc;boundedDesc.device=device;boundedDesc.source=boundedDesc.owner=boundedSource;boundedDesc.key="bounded-evidence";
        auto boundedControl=RegisterControl(boundedDesc,[](const ControlRequest& request,LiveFrameKey){return ControlApplyResult{ControlState::Applied,request.value,{}};});
        Frame(device,boundedSource,60*Second,60*Second+100000000);At(61*Second+100000000);const auto boundedToken=BeginLiveFrame(device,boundedSource);
        const auto boundedRequest=RequestLiveControl(boundedControl.GetId(),true);CHECK(boundedRequest&&PollControls(boundedSource,boundedToken.key)==1);At(61*Second+200000000);EndLiveFrame(boundedToken,LiveOutcome::Completed);
        const auto boundedKey=Frame(device,boundedSource,62*Second,62*Second+100000000);At(63*Second);auto bounded=Source(boundedSource).comparisonEvidence;CHECK(bounded.after.available&&bounded.after.summary.key==boundedKey);
        const auto followingKey=Frame(device,boundedSource,63*Second,63*Second+100000000);
        {LiveContextScope original({boundedKey,2,boundedSource,0,0});for(unsigned i=0;i<2049;++i){LiveEvent late;late.kind=LiveEventKind::NativeTranslation;late.stage=LiveStage::NativeTranslate;RecordLiveEvent(late);}for(unsigned i=0;i<4097;++i)RecordLiveLink({device,boundedKey.frame});}
        bounded=Source(boundedSource).comparisonEvidence;CHECK(bounded.after.available&&bounded.after.summary.key==boundedKey&&bounded.after.flow.events.size()==2048&&bounded.after.flow.links.size()==4096&&bounded.after.loss.events==1&&bounded.after.loss.links==1&&bounded.after.budgetLimited);
        for(unsigned i=0;i<129;++i){LiveGpuSpan missing;missing.id=AllocateLiveId();missing.frame=boundedKey;missing.appliedRevision=2;RecordLiveGpuSpan(missing);}
        bounded=Source(boundedSource).comparisonEvidence;CHECK(bounded.after.flow.gpuSpans.size()==128&&bounded.after.loss.gpuSpans==1&&bounded.after.summary.incomplete&&!bounded.after.summary.gpuCoverageComplete);
        Frame(device,boundedSource,64*Second,64*Second+100000000,0,true);At(65*Second);CHECK(Source(boundedSource).comparisonEvidence.after.summary.key==followingKey);
        Frame(device,boundedSource,65*Second,65*Second+100000000);At(66*Second);CHECK(Source(boundedSource).comparisonEvidence.after.summary.key==followingKey);
        CHECK(ReadLiveSnapshot()->collector.retainedDetailBytes<=32u*1024u*1024u);
        const auto pressureSource=AllocateLiveId();std::size_t held=0;while(ReserveLiveDetailBytes(1024u*1024u))held+=1024u*1024u;
        Frame(device,pressureSource,70*Second,70*Second+100000000);ReleaseLiveDetailBytes(held);
        ControlDesc pressureDesc;pressureDesc.device=device;pressureDesc.source=pressureDesc.owner=pressureSource;pressureDesc.key="evidence-pressure";
        auto pressureControl=RegisterControl(pressureDesc,[](const ControlRequest& request,LiveFrameKey){return ControlApplyResult{ControlState::Applied,request.value,{}};});
        At(71*Second+100000000);const auto pressureToken=BeginLiveFrame(device,pressureSource);const auto pressureRequest=RequestLiveControl(pressureControl.GetId(),true);CHECK(pressureRequest&&PollControls(pressureSource,pressureToken.key)==1);At(71*Second+200000000);EndLiveFrame(pressureToken,LiveOutcome::Completed);
        const auto pressure=Source(pressureSource).comparisonEvidence;CHECK(pressure.request==pressureRequest&&!pressure.before.available&&pressure.before.budgetLimited&&pressure.before.loss.frames==1&&Status(pressureRequest).baseline.frameCount==1);
        At(80*Second);const auto retainedBefore=ReadLiveSnapshot()->collector.retainedDetailBytes;const auto retiredSource=RegisterLiveObject(nullptr,{LiveObjectKind::Renderer,device,0,0,0,"retired evidence"});Frame(device,retiredSource,80*Second,80*Second+100000000);
        CHECK(ReadLiveSnapshot()->collector.retainedDetailBytes>retainedBefore);RetireLiveObject(retiredSource);CHECK(ReadLiveSnapshot()->collector.retainedDetailBytes<=retainedBefore);
        // Leave all slots active until this fixture exits: a failed new frame must not erase prior ranges.
        const auto activeSource=AllocateLiveId();
        for(unsigned i=0;i<8192;++i){CHECK(BeginLiveFrame(device,activeSource).key.frame);SetLiveContext({});}
        CHECK(!BeginLiveFrame(device,highFps).key.frame);
        value=Source(highFps);CHECK(value.runRanges.drawCalls.windows==2&&Equal(value.runRanges.drawCalls.low,2)&&Equal(value.runRanges.drawCalls.high,4));
        std::cout<<"Completed seconds, mixed revisions, retractable GPU ranges, no-client/cap retention, measured zero and allocation failure passed\n";
        return 0;
    } catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
