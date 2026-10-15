#include "../ViewerState.h"
#include "../FlowGraph.h"
#include <cstdio>
#include <algorithm>
#include <limits>
#include <stdexcept>

static void Check(bool condition, const char* message)
{
    if(!condition) throw std::runtime_error(message);
}

int main()
{
    try
    {
        auto snapshot = std::make_shared<viewer::D::LiveSnapshot>();
        snapshot->session = {3, 5}; snapshot->sequence = 2;
        viewer::D::LiveSourceSnapshot source;
        source.device = 10; source.source = 20; source.latest.key = {10, 20, 8}; source.latest.sampleAgeNs = 50;
        source.latest.appliedRevision = 3; source.recent.frameCount = 17; source.recent.cpuRender = {viewer::D::LiveValueState::Ready, 12.5};
        viewer::D::LiveEvent event; event.id = 71; event.context.frame = source.latest.key; event.context.appliedRevision=3; event.context.batch = 101; event.resource = 102;
        source.flow.key=source.latest.key; source.flow.events.push_back(event); snapshot->sources.push_back(source);
        viewer::ViewerState state;
        Check(state.Accept(snapshot, 100), "Accept a new owning snapshot");
        Check(state.snapshot == snapshot, "Keep the actual producer snapshot");
        Check(!state.Accept(snapshot, 200), "Ignore an already delivered sequence");
        Check(state.Source() && state.Source()->recent.frameCount == 17 && state.Source()->recent.cpuRender.average == 12.5, "Preserve producer all-frame aggregate");
        Check(state.AgeNs(200) == 150 && state.receivedNs == 100, "Age increases without a new packet");
        state.Select(viewer::SelectionKind::Event, 71);
        viewer::D::LiveEvent related; related.context.batch = 101;
        Check(state.Related(related), "Batch links a selected command to other events");
        related.context.batch = 999; Check(!state.Related(related), "Unrelated work stays unrelated");
        viewer::D::ControlDesc control; control.id = 501; control.device = 10; control.source = 20;
        state.Queued(601, control, true, 200);
        Check(state.requests.back().state == viewer::D::ControlState::Requested && state.requests.back().actual == viewer::D::ControlValue(false), "Queued does not mean applied");
        viewer::ViewerState pending; Check(pending.Accept(snapshot,100),"Receive pre-request snapshot"); pending.Queued(602,control,true,200);
        auto localPacket=std::make_shared<viewer::D::LiveSnapshot>(*snapshot); localPacket->sequence=3;
        viewer::D::ControlStatus localRequest; localRequest.request=602; localRequest.control=control.id; localRequest.requested=true; localPacket->controlStatus.push_back(localRequest);
        Check(pending.Accept(localPacket,300) && pending.requests.back().requestedNs==200 && pending.requests.back().device==10
            && pending.requests.back().source==20,"Client-local pending placeholder preserves known request time and source");
        auto next = std::make_shared<viewer::D::LiveSnapshot>(*snapshot); next->sequence = 3; next->sources[0].flow.events.clear();
        viewer::D::ControlStatus applied = state.requests.back(); applied.state = viewer::D::ControlState::Applied;
        applied.actual = true; applied.appliedRevision = 3; applied.appliedFrame = {10, 20, 8}; applied.baseline.frameCount = 21;
        applied.baseline.cpuRender = {viewer::D::LiveValueState::Ready, 40}; next->controlStatus.push_back(applied);
        Check(state.Accept(next, 300), "Receive later actual application");
        Check(state.SelectedEvent() && state.SelectedEvent()->context.frame.frame == 8, "Selection retains original owning snapshot");
        Check(state.Comparison() && state.Comparison()->baseline.cpuRender.average == 40, "Use producer frozen baseline");
        viewer::D::LiveAggregate second; second.device=10; second.source=20; second.appliedRevision=3;
        second.beginNs=1000000000; second.endNs=2000000000; second.frameCount=60;
        second.cpuRender={viewer::D::LiveValueState::Ready,4}; second.cpuRender.ready=60;
        Check(viewer::CompleteComparisonWindow(second,10,20,2000000000,3),"A completed full second of the actual revision can be compared");
        auto invalid=second; invalid.endNs+=1;
        Check(!viewer::CompleteComparisonWindow(invalid,10,20,3000000000,3),"Rolling 1s plus one ns cannot masquerade as a completed fixed second");
        invalid=second; invalid.mixedRevision=true;
        Check(!viewer::CompleteComparisonWindow(invalid,10,20,3000000000,3) && !viewer::CompleteComparisonWindow(second,10,20,1999999999,3)
            && !viewer::CompleteComparisonWindow(second,10,20,3000000000,4),"Mixed, unfinished and wrong-revision comparison windows stay pending");
        Check(viewer::ComparisonNumber(second,viewer::ComparisonMetric::Cpu)==4,"Comparison preserves the producer mean");
        second.gpuMeasured={viewer::D::LiveValueState::Ready,2}; second.gpuMeasured.ready=59; second.gpuMeasured.pending=1;
        Check(!viewer::ComparisonNumber(second,viewer::ComparisonMetric::Gpu),"Partly pending GPU means cannot be advertised as a complete before/after result");
        auto& stage=second.stages[size_t(viewer::D::LiveStage::Record)];
        stage.cpu={viewer::D::LiveValueState::Ready,9}; stage.cpu.ready=30; stage.cpu.unavailable=30;
        stage.gpu=second.gpuMeasured; stage.meanOperations=123; stage.meanElements=456; stage.meanBytes=789;
        Check(viewer::StageNumber(second,viewer::D::LiveStage::Record,viewer::StageMetric::Cpu)==9
            && viewer::StageNumber(second,viewer::D::LiveStage::Record,viewer::StageMetric::Operations)==123
            && viewer::StageNumber(second,viewer::D::LiveStage::Record,viewer::StageMetric::Elements)==456
            && viewer::StageNumber(second,viewer::D::LiveStage::Record,viewer::StageMetric::Bytes)==789,
            "Stage comparison preserves producer means and measured CPU coverage rather than dividing by all frames");
        Check(!viewer::StageNumber(second,viewer::D::LiveStage::Record,viewer::StageMetric::Gpu)
            && !viewer::StageNumber(second,viewer::D::LiveStage::Data,viewer::StageMetric::Cpu),
            "A partly pending GPU stage and an unmeasured CPU stage remain unavailable, never zero");
        second.recordedMean.uploadBytes=8192; second.submittedMean.uploadBytes=4096; second.completedMean.uploadBytes=2048;
        const auto upload=std::find_if(viewer::ComparisonCounters().begin(),viewer::ComparisonCounters().end(),[](const auto& value){return std::string(value.key)=="upload_bytes";});
        Check(upload!=viewer::ComparisonCounters().end()
            && viewer::CounterNumber(second,viewer::CounterPhase::Recorded,*upload)==8192
            && viewer::CounterNumber(second,viewer::CounterPhase::Submitted,*upload)==4096
            && viewer::CounterNumber(second,viewer::CounterPhase::Completed,*upload)==2048,
            "Counter comparison keeps recorded, submitted and completed producer scopes separate");
        second.recordedMean.uploadPageAllocations=1; second.recordedMean.uploadPageReuses=2; second.recordedMean.uploadPageAllocationBytes=65536;
        Check(viewer::ComparisonCounters().size()==21 && viewer::CounterNumber(second,viewer::CounterPhase::Recorded,viewer::ComparisonCounters()[18])==1
            && viewer::CounterNumber(second,viewer::CounterPhase::Recorded,viewer::ComparisonCounters()[19])==2
            && viewer::CounterNumber(second,viewer::CounterPhase::Recorded,viewer::ComparisonCounters()[20])==65536,
            "Actual native page allocation, reuse and allocation bytes stay independent of logical upload bytes and device gauges");
        Check(!viewer::CounterNumber(second,viewer::CounterPhase::Submitted,viewer::ComparisonCounters()[18])
            && !viewer::CounterNumber(second,viewer::CounterPhase::Completed,viewer::ComparisonCounters()[19])
            && !viewer::CounterNumber(second,viewer::CounterPhase::Submitted,viewer::ComparisonCounters()[20]),
            "Native upload page acquisition is recorded-only; submitted/completed zero is not a measurement");
        const auto change=viewer::CompareNumbers(8.0,2.0);
        Check(change.delta==-6 && change.percent==-75 && !viewer::CompareNumbers(0.0,2.0).percent
            && !viewer::CompareNumbers(std::nullopt,2.0).delta,
            "Deltas require two measured values; a zero baseline cannot produce a percentage");
        stage.meanBytes=std::numeric_limits<double>::quiet_NaN();
        Check(!viewer::StageNumber(second,viewer::D::LiveStage::Record,viewer::StageMetric::Bytes)
            && !viewer::StageNumber(second,viewer::D::LiveStage::Count,viewer::StageMetric::Cpu),
            "Invalid numbers and stage indices cannot become comparison cells");
        auto emptyBaseline=std::make_shared<viewer::D::LiveSnapshot>(*next); emptyBaseline->sequence=4; emptyBaseline->controlStatus.back().baseline={};
        viewer::ViewerState waiting; waiting.Accept(emptyBaseline,400);
        Check(waiting.Comparison()!=nullptr,"Last actual application remains visible while its full-second baseline is unavailable");
        next=std::make_shared<viewer::D::LiveSnapshot>(*next);
        viewer::D::LiveClockInfo local{viewer::D::LiveCpuClock::QueryPerformanceCounter, 10000000, 0};
        next->clock = local; next->sequence=4; next->sources[0].latest.endNs=150; next->sources[0].latest.sampleAgeNs=50;
        Check(state.Accept(next, 1000) && state.AgeNs(1100,local)==950, "Age includes deliberately delayed transport on matching QPC");
        local.frequency=999; Check(state.AgeNs(1100,local)==150, "Unknown clock domain uses explicitly partial receipt age");
        next=std::make_shared<viewer::D::LiveSnapshot>(*next); next->sequence=5;
        viewer::D::LiveGpuSpan gpu; gpu.id=700; gpu.frame={10,20,8}; gpu.appliedRevision=3; gpu.duration.state=viewer::D::LiveValueState::Pending;
        next->sources[0].flow.gpuSpans.push_back(gpu); Check(state.Accept(next,1100),"Receive current flow pending GPU sample"); state.Select(viewer::SelectionKind::Gpu,700);
        auto pendingGraph=viewer::BuildFlowGraph(*next,next->sources[0]);
        Check(std::any_of(pendingGraph.nodes.begin(),pendingGraph.nodes.end(),[](const auto& node) { return node.id==700 && node.kind==viewer::FlowNodeKind::Gpu; }),"Current Pending GPU is a real graph node");
        next=std::make_shared<viewer::D::LiveSnapshot>(*next); next->sequence=6;
        gpu.duration={viewer::D::LiveValueState::Ready,2.0}; next->sources[0].recentGpuSpans.push_back(gpu);
        Check(state.Accept(next,1200) && state.SelectedGpu() && state.SelectedGpu()->duration.state==viewer::D::LiveValueState::Ready
            && state.SelectedGpu()->frame.frame==8, "Stable selected GPU sample follows delayed readiness on original frame");
        Check(viewer::GpuSamples(next->sources[0]).size()==1 && viewer::GpuSamples(next->sources[0])[0]->duration.state==viewer::D::LiveValueState::Ready,"Current Pending and recent Ready are deduplicated by sample ID");
        viewer::ViewerState inventoryState;
        auto inventory=std::make_shared<viewer::D::LiveSnapshot>(*next);
        viewer::D::LiveObjectInfo object; object.id=901; object.desc.label="before"; inventory->objects.push_back(object);
        Check(inventoryState.Accept(inventory,1200),"Receive object inventory"); inventoryState.Select(viewer::SelectionKind::Object,901);
        inventory=std::make_shared<viewer::D::LiveSnapshot>(*inventory); ++inventory->sequence; inventory->objects[0].desc.label="after";
        Check(inventoryState.Accept(inventory,1300) && inventoryState.SelectedObject()->desc.label=="after","Stable object selection follows updated inventory");
        next->sources[0].flow.events={event}; next->sources[0].flow.links={{71,101},{101,102},{800,801}};
        const auto graph=viewer::BuildFlowGraph(*next,next->sources[0],101);
        Check(graph.nodes.size()==3 && graph.links.size()==2, "Graph follows only actual connected links");
        Check(graph.nodes[2].kind==viewer::FlowNodeKind::Unknown, "Unknown endpoint receives no invented object meaning");
        const auto bundles=viewer::GroupFlowLinks(graph);
        uint64_t bundledLinks=0; for(const auto& bundle:bundles) bundledLinks+=bundle.links;
        Check(bundledLinks==2,"Collapsed groups retain every actual link even when individual nodes are hidden");
        const auto observed=viewer::ObserveFlow(graph,next->sources[0]);
        Check(observed.stageEvents[size_t(event.stage)]==1 && observed.links.size()==2,
            "Observed flow groups actual retained events and links without using cross-frame ID differences");
        auto renamed=*next; auto& renamedSource=renamed.sources[0];
        for(auto& value:renamedSource.flow.events) { value.id+=1000; if(value.context.batch) value.context.batch+=1000; if(value.resource) value.resource+=1000; }
        for(auto& value:renamedSource.flow.links) {value.from+=1000; value.to+=1000;}
        const auto renamedObserved=viewer::ObserveFlow(viewer::BuildFlowGraph(renamed,renamedSource,1101),renamedSource);
        Check(observed.stageEvents==renamedObserved.stageEvents && observed.commandKinds==renamedObserved.commandKinds
            && observed.links.size()==renamedObserved.links.size()
            && std::equal(observed.links.begin(),observed.links.end(),renamedObserved.links.begin(),[](const auto& a,const auto& b){return a.from==b.from && a.to==b.to && a.links==b.links;}),
            "Fresh original IDs cannot invent a semantic flow change");
        const auto partialCounts=viewer::CompareObservedCounts(10.0,0.0,true,false);
        Check(partialCounts.before==10 && partialCounts.after==0 && !partialCounts.delta && !partialCounts.percent
            && viewer::CompareObservedCounts(10.0,0.0,true,true).delta==-10,
            "Missing retained detail cannot be presented as work or an edge disappearing");
        auto batches=std::make_shared<viewer::D::LiveSnapshot>(*next); batches->sequence=7;
        viewer::D::LiveEvent batch; batch.id=905; batch.kind=viewer::D::LiveEventKind::Batch; batch.stage=viewer::D::LiveStage::Data;
        batch.context.frame={10,20,8}; batch.context.appliedRevision=3; batch.context.batch=905; batch.elements=256; batch.first=4; batch.stride=64; batch.reason=viewer::D::LiveSplitReason::Material;
        batches->sources[0].flow.events.push_back(batch); Check(state.Accept(batches,1300),"Receive actual producer Batch event"); state.Select(viewer::SelectionKind::Event,905);
        Check(state.SelectedEvent()->kind==viewer::D::LiveEventKind::Batch && state.SelectedEvent()->elements==256
            && state.SelectedEvent()->stride==64 && state.SelectedEvent()->reason==viewer::D::LiveSplitReason::Material,"Batch event selection exposes actual elements, range and split reason without invented inventory");
        const auto batchGraph=viewer::BuildFlowGraph(*batches,batches->sources[0]);
        Check(std::any_of(batchGraph.nodes.begin(),batchGraph.nodes.end(),[](const auto& node) { return node.id==905 && node.kind==viewer::FlowNodeKind::Event && node.column==1; }),"Producer batch event appears in Data group");
        auto reused=std::make_shared<viewer::D::LiveSnapshot>(*batches); reused->sequence=8;
        reused->sources[0].latest.key.frame=800; reused->sources[0].latest.appliedRevision=4; reused->sources[0].flow.key=reused->sources[0].latest.key;
        for(auto& value:reused->sources[0].flow.events) { value.context.frame=reused->sources[0].latest.key; value.context.appliedRevision=4; }
        reused->sources[0].flow.events.back().elements=7;
        Check(state.Accept(reused,1350) && state.SelectedEvent()->context.frame.frame==8 && state.SelectedEvent()->context.appliedRevision==3
            && state.SelectedEvent()->elements==256,"A stable batch ID in a later frame cannot replace the selected original frame/revision/elements");
        state.Select(viewer::SelectionKind::Event,905);
        Check(state.SelectedEvent()->context.frame.frame==800 && state.SelectedEvent()->elements==7,"An explicit new selection can inspect the newer instance of a stable batch ID");
        control.kind = viewer::D::ControlKind::Integer; control.minimum = 1; control.maximum = 8;
        Check(viewer::ValidControlValue(control, int64_t(8)) && !viewer::ValidControlValue(control, int64_t(9))
            && !viewer::ValidControlValue(control, double(2)), "Integer type and range");
        control.kind = viewer::D::ControlKind::Real;
        Check(!viewer::ValidControlValue(control, std::numeric_limits<double>::quiet_NaN()), "Reject NaN");
        control.kind = viewer::D::ControlKind::Enumeration; control.choices = {{4, "four"}};
        Check(viewer::ValidControlValue(control, int64_t(4)) && !viewer::ValidControlValue(control, int64_t(5)), "Only registered enum choices");
        next = std::make_shared<viewer::D::LiveSnapshot>(*snapshot); next->session = {7, 9}; next->sequence = 1;
        Check(state.Accept(next, 400) && state.requests.empty() && state.selection.kind == viewer::SelectionKind::None, "New session cannot inherit old requests or selection");
        viewer::ViewerState activity;
        auto active=std::make_shared<viewer::D::LiveSnapshot>(*next);
        auto newer=active->sources[0]; newer.source=21; newer.latest.key.source=21; newer.latest.endNs=2000; active->sources.push_back(newer);
        Check(activity.Accept(active,2100) && activity.source==21,"Initial selection follows the active source rather than an old startup upload");
        auto retained=std::make_shared<viewer::D::LiveSnapshot>(); retained->session={50,60}; retained->sequence=1;
        viewer::D::LiveSourceSnapshot current; current.device=10; current.source=20; current.latest.key={10,20,900}; current.latest.appliedRevision=3;
        current.flow.key=current.latest.key;
        viewer::D::LiveEvent logical; logical.id=1001; logical.context.frame=current.latest.key; logical.context.appliedRevision=3;
        logical.kind=viewer::D::LiveEventKind::Upload; logical.commandKind=viewer::D::LiveCommandKind::Upload; logical.stage=viewer::D::LiveStage::Upload;
        auto textureUpload=logical; textureUpload.id=1002;
        auto nativeUpload=logical; nativeUpload.id=1003; nativeUpload.kind=viewer::D::LiveEventKind::NativeTranslation;
        auto pageAcquire=logical; pageAcquire.id=1004; pageAcquire.kind=viewer::D::LiveEventKind::UploadMemory;
        auto drawCommand=logical; drawCommand.id=1005; drawCommand.kind=viewer::D::LiveEventKind::Command; drawCommand.commandKind=viewer::D::LiveCommandKind::Draw;
        auto wait=logical; wait.id=1006; wait.kind=viewer::D::LiveEventKind::Wait; wait.commandKind=viewer::D::LiveCommandKind::None; wait.outcome=viewer::D::LiveOutcome::Completed;
        auto present=logical; present.id=1007; present.kind=viewer::D::LiveEventKind::Output; present.commandKind=viewer::D::LiveCommandKind::Present; present.outcome=viewer::D::LiveOutcome::Completed;
        auto acquire=present; acquire.id=1008; acquire.commandKind=viewer::D::LiveCommandKind::None;
        auto nativeWait=wait; nativeWait.id=1009; nativeWait.kind=viewer::D::LiveEventKind::NativeTranslation; nativeWait.commandKind=viewer::D::LiveCommandKind::Wait;
        auto completion=present; completion.id=1010; completion.kind=viewer::D::LiveEventKind::Submission;
        auto skippedAcquire=wait; skippedAcquire.id=1011; skippedAcquire.outcome=viewer::D::LiveOutcome::Skipped;
        current.flow.events={logical,textureUpload,nativeUpload,pageAcquire,drawCommand,wait,present,acquire,nativeWait,completion,skippedAcquire}; retained->sources={current};
        viewer::D::ControlStatus oldRequest; oldRequest.request=40; oldRequest.control=50; oldRequest.device=10; oldRequest.source=20;
        oldRequest.state=viewer::D::ControlState::Applied; oldRequest.appliedFrame={10,20,80}; oldRequest.appliedRevision=2; oldRequest.appliedNs=500000000;
        oldRequest.after.device=10; oldRequest.after.source=20; oldRequest.after.appliedRevision=2; oldRequest.after.frameCount=30;
        oldRequest.after.beginNs=1000000000; oldRequest.after.endNs=2000000000; retained->controlStatus={oldRequest};
        viewer::ViewerState retainedState; retainedState.Accept(retained,3000000000);
        const auto uploadKinds=viewer::ObserveFlow(viewer::BuildFlowGraph(*retained,current),current);
        const auto logicalUploadCount=uploadKinds.commandKinds[size_t(viewer::D::LiveCommandKind::Upload)];
        const auto historicalReady=viewer::ComparisonAfterReady(retainedState,*retainedState.Comparison(),3000000000);
        if(logicalUploadCount!=2 || !historicalReady || uploadKinds.commandKinds[size_t(viewer::D::LiveCommandKind::Wait)]!=1 || uploadKinds.commandKinds[size_t(viewer::D::LiveCommandKind::Present)]!=1)
            std::fprintf(stderr,"regressions: logical_uploads=%llu (expected 2), wait=%llu (expected 1), present=%llu (expected 1), retained_after=%d (expected 1)\n",logicalUploadCount,
                uploadKinds.commandKinds[size_t(viewer::D::LiveCommandKind::Wait)],uploadKinds.commandKinds[size_t(viewer::D::LiveCommandKind::Present)],historicalReady);
        Check(logicalUploadCount==2 && uploadKinds.commandKinds[size_t(viewer::D::LiveCommandKind::Draw)]==1,
            "Logical buffer/texture Uploads count once, excluding NativeTranslation and UploadMemory duplicates");
        Check(uploadKinds.commandKinds[size_t(viewer::D::LiveCommandKind::Wait)]==1 && uploadKinds.commandKinds[size_t(viewer::D::LiveCommandKind::Present)]==1
            && uploadKinds.commandKinds[size_t(viewer::D::LiveCommandKind::None)]==0,
            "Wait and Present observations count once; acquire, skipped acquire, native translation and completion markers cannot duplicate them");
        Check(historicalReady && retainedState.Source()->latest.appliedRevision==3 && retainedState.Comparison()->after.appliedRevision==2,
            "A valid revision-2 after remains comparable after a programmatic revision-3 frame without relabelling either");
        auto wrongAfter=oldRequest; wrongAfter.after.source=21;
        Check(!viewer::ComparisonAfterReady(retainedState,wrongAfter,3000000000),"A retained after cannot borrow another source's aggregate");
        wrongAfter=oldRequest; wrongAfter.after.appliedRevision=3;
        Check(!viewer::ComparisonAfterReady(retainedState,wrongAfter,3000000000),"A retained after must match the request's actual revision");
        wrongAfter=oldRequest; wrongAfter.after.endNs-=1;
        Check(!viewer::ComparisonAfterReady(retainedState,wrongAfter,3000000000),"A retained after still requires a completed exact one-second window");
        wrongAfter=oldRequest; wrongAfter.appliedFrame.source=21;
        Check(!viewer::ComparisonAfterReady(retainedState,wrongAfter,3000000000),"The actual application key must belong to the selected source");
        std::puts("PASS owning latest snapshot, all-frame means, stale age, linked selection, actual application, frozen baseline, typed controls and session separation");
    }
    catch(const std::exception& error) { std::fprintf(stderr, "FAIL %s\n", error.what()); return 1; }
    return 0;
}
