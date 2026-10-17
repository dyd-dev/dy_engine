#include "../FlowPlayback.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <sstream>
static void Check(bool result,const char* why) { if(!result) throw std::runtime_error(why); }
int main()
{
    try
    {
        namespace D=viewer::D;
        auto first=std::make_shared<D::LiveSnapshot>(); first->session={1,2}; first->sequence=1;
        D::LiveSourceSnapshot source; source.device=1; source.source=2; source.latest.key={1,2,30}; source.latest.appliedRevision=7;
        source.flow.key=source.latest.key;
        D::LiveEvent event; event.id=100; event.context.frame=source.latest.key; event.context.appliedRevision=7; event.stage=D::LiveStage::Record;
        source.flow.events={event}; source.flow.links={{100,200}}; first->sources={source};
        viewer::FlowPlayback replay;
        Check(replay.Update(first,1,2,100),"An actual same-frame flow can begin a presentation cycle");
        auto next=std::make_shared<D::LiveSnapshot>(*first); next->sequence=2;
        next->sources[0].latest.key.frame=90000; next->sources[0].flow.key=next->sources[0].latest.key;
        next->sources[0].flow.events[0].context.frame=next->sources[0].latest.key;
        Check(!replay.Update(next,1,2,2000000100) && replay.snapshot==first && replay.frame.frame==30,
            "New live snapshots cannot replace the pinned frame or topology during three seconds");
        Check(std::abs(replay.Phase(1500000100)-.5)<1e-9,"Presentation uses elapsed time, not frame ID distance");
        Check(replay.Update(next,1,2,3000000100) && replay.snapshot==next && replay.frame.frame==90000,
            "Next cycle adopts a fresh representative frame at the exact boundary");
        auto malformed=std::make_shared<D::LiveSnapshot>(*next); malformed->sources[0].flow.events[0].context.appliedRevision=8;
        Check(!replay.Update(malformed,1,2,6000000100) && replay.snapshot==next,"Wrong revision cannot relabel pinned trace");
        auto other=std::make_shared<D::LiveSnapshot>(*next); other->session={3,4}; other->sources[0].flow.events.clear(); other->sources[0].flow.links.clear();
        Check(!replay.Update(other,1,2,6000000200) && !replay.snapshot,"Session change clears old replay even when the new trace is empty");
        Check(replay.Update(next,1,2,7000000100),"Valid source can resume after reset");
        Check(!replay.Update(next,1,99,7000000200) && !replay.snapshot,"Source change cannot reuse the previous source's trace");
        viewer::FlowPlayback partialReplay;
        auto partial=std::make_shared<D::LiveSnapshot>(*first); partial->sources[0].latest.incomplete=true; partial->sources[0].flow.omittedEvents=1;
        Check(partialReplay.Update(partial,1,2,100),"Retained real flow remains viewable with an explicit partial-detail warning");
        auto mismatch=std::make_shared<D::LiveSnapshot>(*first); mismatch->sources[0].flow.key.frame=99;
        Check(!partialReplay.Update(mismatch,1,2,3000000100),"A different flow frame cannot be relabelled as the representative frame");
        viewer::StatisticsView statistics;
        auto averaged=std::make_shared<D::LiveSnapshot>(*next); averaged->producerTimeNs=2000000000;
        auto& aggregate=averaged->sources[0].recent; aggregate.beginNs=1000000000; aggregate.endNs=2000000000; aggregate.frameCount=99;
        aggregate.device=1; aggregate.source=2; aggregate.appliedRevision=7;
        aggregate.cpuRender={D::LiveValueState::Ready,12.5}; aggregate.cpuRender.ready=99;
        aggregate.cpuRender.maximum=9999;
        averaged->sources[0].runRanges.cpuRender={D::LiveValueState::Ready,4,40,2};
        auto previous=aggregate; previous.beginNs=0; previous.endNs=1000000000; previous.cpuRender.average=4;
        averaged->sources[0].recentSeconds={previous,aggregate};
        Check(statistics.Update(averaged,1,2,100),"A completed producer one-second window can be displayed");
        Check(statistics.history.size()==2 && statistics.history.front().cpuRender.average==4,
            "Late attach restores the producer's retained one-second history without GUI reconstruction");
        auto newerStats=std::make_shared<D::LiveSnapshot>(*averaged); ++newerStats->sequence; newerStats->sources[0].recent.cpuRender.average=31;
        newerStats->sources[0].recentSeconds.back().cpuRender.average=31;
        Check(!statistics.Update(newerStats,1,2,500000100) && statistics.state.Source()->recent.cpuRender.average==12.5,
            "Numeric view stays stable between one-Hz refreshes while live input changes");
        Check(statistics.Update(newerStats,1,2,1000000100) && statistics.state.Source()->recent.cpuRender.average==31
            && statistics.history.size()==2 && statistics.history.back().cpuRender.average==31,
            "Late correction copies the original window/history without snapshot re-averaging or duplication");
        Check(statistics.state.Source()->runRanges.cpuRender.low==4 && statistics.state.Source()->runRanges.cpuRender.high==40,
            "Run ranges are producer one-second mean extrema, never the individual-frame maximum");
        viewer::PresentationLog log; replay.Update(first,1,2,100);
        log.Presented(statistics,replay,false,false,1000000200);
        Check(!log.statistics && !log.flow,"Hidden panels cannot claim received data was displayed");
        log.Presented(statistics,replay,true,true,1000000300);
        Check(log.statistics->average.cpuRender.average==31 && log.flow->frame.frame==30,
            "Report keeps one-second displayed data separate from the older pinned replay frame");
        log.Presented(statistics,replay,false,false,1000000400);
        std::ostringstream report; log.Write(report);
        Check(!log.statisticsVisible && !log.flowVisible && report.str().find("statistics_metric=cpu_render_ms,3,31,3,4,40,2")!=std::string::npos
            && report.str().find("statistics_metric=fps,0,--,")!=std::string::npos
            && report.str().find("p95_ms=")==std::string::npos,"Report records actual presentation, preserves unknown FPS and retires the obsolete p95 gate");
        viewer::ComparisonPresentation shown; shown.sequence=2; shown.status.request=70; shown.status.control=80;
        shown.status.appliedRevision=7; shown.status.baseline=aggregate; shown.status.after=aggregate; shown.status.after.cpuRender.average=8;
        shown.currentRevision=8;
        shown.beforeReady=shown.afterReady=true;
        shown.metricVisibility[size_t(viewer::ComparisonMetric::Cpu)]=15;
        shown.status.baseline.stages[size_t(D::LiveStage::Upload)].meanBytes=8192;
        shown.status.after.stages[size_t(D::LiveStage::Upload)].meanBytes=2048;
        shown.stageVisibility[size_t(D::LiveStage::Upload)][size_t(viewer::StageMetric::Bytes)]=15;
        shown.status.baseline.submittedMean.uploadBytes=8192; shown.status.after.submittedMean.uploadBytes=4096;
        shown.counterVisibility[size_t(viewer::CounterPhase::Submitted)][15]=15;
        viewer::ComparisonFlowPresentation flowProof; flowProof.session={1,2}; flowProof.request=70; flowProof.sequence=12; flowProof.cycleStartedNs=100;
        flowProof.frames[0].frame={1,2,30}; flowProof.frames[0].revision=7; flowProof.frames[0].windowRevision=7;
        flowProof.frames[0].windowBeginNs=1000000000; flowProof.frames[0].windowEndNs=2000000000;
        flowProof.frames[0].available=flowProof.frames[0].valid=flowProof.frames[0].complete=flowProof.frames[0].canvasVisible=true;
        flowProof.frames[0].observed.stageEvents[3]=10; flowProof.frames[0].visibleOccurrences={100,200};
        flowProof.frames[1]=flowProof.frames[0]; flowProof.frames[1].frame.frame=90000; flowProof.frames[1].revision=8;
        flowProof.frames[1].observed.stageEvents[3]=2; flowProof.stageVisibility[3]=15; shown.flow=flowProof;
        log.PresentedComparison(nullptr,10); Check(!log.comparison,"An undisplayed comparison cannot be reported as shown");
        log.PresentedComparison(&shown,20); shown.status.after.cpuRender.average=9999;
        log.PresentedComparison(nullptr,30); std::ostringstream comparisonReport; log.Write(comparisonReport);
        Check(!log.comparisonVisible && log.comparison->status.after.cpuRender.average==8
            && comparisonReport.str().find("comparison_metric=cpu_ms,1,12.5,1,8,-4.5,-36")!=std::string::npos,
            "Presentation report retains only the comparison values actually drawn, not newer received status");
        Check(comparisonReport.str().find("comparison_revision_context=7,8,1")!=std::string::npos
            && comparisonReport.str().find("comparison_stage_revision_context=7,8,1")!=std::string::npos,
            "A preserved historical comparison reports its actual revision separately from the current frame revision");
        Check(comparisonReport.str().find("comparison_stage=2,4,1,8192,1,2048,-6144,-75,15")!=std::string::npos
            && comparisonReport.str().find("comparison_counter=1,upload_bytes,1,8192,1,4096,-4096,-50,15")!=std::string::npos
            && comparisonReport.str().find("comparison_counter=0,upload_bytes")==std::string::npos,
            "Report preserves only the stage and counter scopes actually visible, including their original producer means");
        Check(comparisonReport.str().find("comparison_flow_last_presented=12,70,100,20")!=std::string::npos
            && comparisonReport.str().find("comparison_flow_frame=0,1,2,30,7,1000000000,2000000000,7")!=std::string::npos
            && comparisonReport.str().find("comparison_flow_stage=3,1,10,1,2,-8,-80,15")!=std::string::npos,
            "Flow draw proof preserves the actual original pair independently of changing aggregate windows");
        shown.flow->frames[1].complete=false; shown.flow->frames[1].loss.links=1; shown.flow->frames[1].observed.stageEvents[3]=0;
        log.PresentedComparison(&shown,40); std::ostringstream partialFlowReport; log.Write(partialFlowReport);
        Check(partialFlowReport.str().find("comparison_flow_stage=3,1,10,1,0,--,--,15")!=std::string::npos,
            "Reported partial flow cannot turn missing retained detail into a measured disappearance");
        auto scrolled=shown; scrolled.sequence=99; scrolled.stageVisibility={}; scrolled.flow.reset(); scrolled.status.after.stages[size_t(D::LiveStage::Upload)].meanBytes=12345;
        log.PresentedComparison(&scrolled,50); std::ostringstream scrolledReport; log.Write(scrolledReport);
        Check(scrolledReport.str().find("comparison_stage=2,4,1,8192,1,2048,-6144,-75,15")!=std::string::npos
            && scrolledReport.str().find("comparison_flow_last_presented=12,70,100,40")!=std::string::npos,
            "Scrolling to another section cannot erase or relabel the last actually displayed stage and original-pair evidence");
        newerStats=std::make_shared<D::LiveSnapshot>(*newerStats); ++newerStats->sequence; newerStats->sources[0].recent.endNs-=500000000;
        Check(!statistics.Update(newerStats,1,2,2000000100),"An old half-second aggregate cannot be relabelled one second");
        Check(!statistics.Update(newerStats,1,99,2000000200) && !statistics.state.snapshot && statistics.history.empty(),"Statistics source change resets prior view/history");

        auto paired=std::make_shared<D::LiveSnapshot>(*first); paired->producerTimeNs=5000000000;
        auto& evidence=paired->sources[0].comparisonEvidence; evidence.request=70;
        evidence.before.available=evidence.after.available=true;
        evidence.before.summary=paired->sources[0].latest; evidence.before.summary.endNs=1500000000;
        evidence.before.flow=paired->sources[0].flow;
        evidence.before.windowBeginNs=1000000000; evidence.before.windowEndNs=2000000000; evidence.before.windowRevision=7;
        evidence.after=evidence.before; evidence.after.summary.key.frame=90000; evidence.after.summary.appliedRevision=8;
        evidence.after.summary.endNs=3500000000; evidence.after.windowBeginNs=3000000000; evidence.after.windowEndNs=4000000000; evidence.after.windowRevision=8;
        evidence.after.flow.key=evidence.after.summary.key;
        for(auto& value:evidence.after.flow.events) { value.context.frame=evidence.after.summary.key; value.context.appliedRevision=8; }
        Check(viewer::ValidFrameEvidence(evidence.before,1,2,paired->producerTimeNs) && viewer::CompleteFrameEvidence(evidence.before,1,2,paired->producerTimeNs),
            "A comparison representative retains original frame/revision and its own completed one-second window");
        auto corrupt=evidence.before; corrupt.windowRevision=99;
        Check(!viewer::ValidFrameEvidence(corrupt,1,2,paired->producerTimeNs),"Evidence cannot borrow a window from another revision");
        corrupt=evidence.before; corrupt.summary.endNs=corrupt.windowEndNs;
        Check(!viewer::ValidFrameEvidence(corrupt,1,2,paired->producerTimeNs),"Representative end time must belong to its half-open producer window");
        corrupt=evidence.before; corrupt.loss.links=1;
        Check(viewer::ValidFrameEvidence(corrupt,1,2,paired->producerTimeNs) && !viewer::CompleteFrameEvidence(corrupt,1,2,paired->producerTimeNs),
            "Partial actual evidence remains inspectable but cannot establish missing work or links");
        viewer::ComparisonPlayback pairReplay;
        Check(pairReplay.Update(paired,1,2,70,100),"The producer latest request can pin a bounded before/after pair");
        auto newPair=std::make_shared<D::LiveSnapshot>(*paired); ++newPair->sequence;
        auto& after=newPair->sources[0].comparisonEvidence.after;
        after.summary.key.frame=800000; after.flow.key=after.summary.key;
        for(auto& value:after.flow.events) value.context.frame=after.summary.key;
        Check(!pairReplay.Update(newPair,1,2,70,1000000100) && pairReplay.evidence->after.summary.key.frame==90000
            && std::abs(pairReplay.Phase(1500000100)-.5)<1e-9,
            "Both original sides and their annotations stay frozen together for three seconds");
        Check(pairReplay.Update(newPair,1,2,70,3000000100) && pairReplay.evidence->after.summary.key.frame==800000,
            "The next pair is adopted only at the presentation boundary");
        Check(!pairReplay.Update(newPair,1,2,71,3000000200) && !pairReplay.evidence,
            "A new latest request immediately clears the previous pair rather than mislabelling it");
        Check(pairReplay.Update(newPair,1,2,70,4000000100),"Existing producer evidence restores after reconnect");
        Check(!pairReplay.Update(newPair,1,99,70,4000000200) && !pairReplay.evidence,"Pair replay cannot cross a source change");
        Check(pairReplay.Update(newPair,1,2,70,5000000100),"Original source can resume");
        auto newSession=std::make_shared<D::LiveSnapshot>(*newPair); newSession->session={8,9}; newSession->sources[0].comparisonEvidence={};
        Check(!pairReplay.Update(newSession,1,2,70,5000000200) && !pairReplay.evidence,"Session replacement clears the old comparison even without new detail");

        auto graph=viewer::BuildFlowGraph(*first,first->sources[0]);
        const auto columns=viewer::ActiveFlowColumns(graph);
        Check(columns.size()==2 && columns[0]==3 && columns[1]==viewer::FlowReferenceColumn,"Only actual record/reference columns are shown; no empty placeholders");
        D::LiveObjectInfo buffer; buffer.id=200; buffer.desc.kind=D::LiveObjectKind::Buffer; first->objects={buffer};
        graph=viewer::BuildFlowGraph(*first,first->sources[0]);
        Check(!graph.activeStages[size_t(D::LiveStage::Upload)],"A referenced buffer does not invent an active Upload operation");
        auto oldGpu=D::LiveGpuSpan{}; oldGpu.id=700; oldGpu.frame={1,2,29}; oldGpu.appliedRevision=7;
        first->sources[0].recentGpuSpans={oldGpu}; first->sources[0].flow.links.push_back({100,700});
        graph=viewer::BuildFlowGraph(*first,first->sources[0]);
        Check(graph.links.size()==1 && std::none_of(graph.nodes.begin(),graph.nodes.end(),[](const auto& node){return node.id==700;}),
            "Older GPU frames cannot become current replay nodes or animated edges");
        Check(viewer::BuildFlowGraph(*first,first->sources[0],999).nodes.empty(),"Stale selection cannot invent a disconnected node");

        auto chained=std::make_shared<D::LiveSnapshot>(*first); auto& trace=chained->sources[0];
        auto command=trace.flow.events.front(); command.command={900,2,1}; command.kind=D::LiveEventKind::Command;
        auto second=command; second.id=101; second.command.index=2;
        auto third=second; third.id=102; third.command.index=3;
        auto submit=third; submit.id=103; submit.kind=D::LiveEventKind::Submission; submit.stage=D::LiveStage::Submit; submit.submission=901;
        auto span=D::LiveGpuSpan{}; span.id=104; span.frame=trace.flow.key; span.appliedRevision=7; span.begin.submission=span.end.submission=901;
        trace.flow.events={command,second,third,submit}; trace.flow.gpuSpans={span}; trace.recentGpuSpans.clear();
        trace.flow.links={{102,103},{103,104},{101,102},{100,101},{200,100},{200,101},{300,301}};
        const auto actual=viewer::BuildFlowGraph(*chained,trace); const auto routes=viewer::BuildFlowRoutes(actual,trace);
        Check(routes.size()==4 && routes.front().recording && routes.front().links.size()==4,
            "Actual command-list recording links form one route; resource branches and disconnected references remain separate");
        const auto& chain=routes.front();
        const auto layout=viewer::LayoutFlowRoute(chain);
        Check(layout.nodes.size()==4 && layout.paths.size()==3 && layout.ids.front()==100 && layout.ids.back()==104,
            "One actual route lays out its successive original node occurrences without merging stage positions");
        for(const auto& path:layout.paths) for(size_t i=1;i<path.size();++i)
            Check(path[i].x==path[i-1].x || path[i].y==path[i-1].y,"Selected route segments remain straight or orthogonal");
        for(const auto& path:layout.paths) for(int sample=0;sample<=20;++sample)
            Check(viewer::PointOnFlowPath(path,sample/20.0).y>=layout.CenterY+layout.Radius*.8f,
                "The single marker cannot enter the number and unit text region");
        for(size_t i=0;i+1<layout.paths.size();++i)
            Check(layout.paths[i].back().x==layout.paths[i+1].front().x && layout.paths[i].back().y==layout.paths[i+1].front().y,
                "Sequential marker geometry meets at the actual shared node without teleporting");
        Check(chain.steps.size()==3 && chain.steps[0].from==3 && chain.steps[0].to==3 && chain.steps[0].links==2
            && chain.steps[1].from==3 && chain.steps[1].to==5 && chain.steps[2].from==5 && chain.steps[2].to==6,
            "Repeated record edges collapse to a loop, followed only by recorded submission and its GPU relationship");
        for(const auto& route:routes) for(size_t i=0;i<route.links.size();++i)
        {
            const auto& link=route.links[i];
            Check(std::any_of(actual.links.begin(),actual.links.end(),[&](const auto& value){return value.from==link.from && value.to==link.to;}),
                "Every replay edge must exist in the original frame");
            if(i) Check(route.links[i-1].to==link.from,"A route cannot jump between disconnected data paths");
        }
        Check(viewer::LocateFlowRoute(chain,.1).step==0 && viewer::LocateFlowRoute(chain,.5).step==1
            && viewer::LocateFlowRoute(chain,.9).step==2 && viewer::LocateFlowRoute(chain,1).phase==1,
            "Three seconds advances one chosen route through its successive real steps, never all edges at once");
        trace.flow.events[1].stage=D::LiveStage::Upload;
        const auto alternating=viewer::BuildFlowRoutes(viewer::BuildFlowGraph(*chained,trace),trace);
        const auto unfolded=viewer::LayoutFlowRoute(alternating.front());
        viewer::FlowRoute returns; returns.recording=true; returns.links={{1,2},{2,3},{3,4}};
        returns.steps={{5,3,1},{3,2,1},{2,1,1}};
        const auto compact=viewer::LayoutFlowRoute(returns);
        Check(compact.height==320 && std::all_of(compact.paths.begin(),compact.paths.end(),[](const auto& path){return path.size()==6 && path[2].y==280;}),
            "Multiple independent return segments share a fixed height rather than growing below the initial viewport");
        Check(unfolded.nodes.size()==5 && unfolded.nodes[0].x<unfolded.nodes[2].x && unfolded.paths[0].size()>2,
            "Record to Upload is an orthogonal return, with a distinct occurrence when Record appears again");
        for(const auto* geometry:{&unfolded,&compact})
        for(size_t a=0;a<geometry->paths.size();++a) for(size_t b=a+1;b<geometry->paths.size();++b)
            for(size_t i=1;i<geometry->paths[a].size();++i) for(size_t j=1;j<geometry->paths[b].size();++j)
            {
                const auto p=geometry->paths[a][i-1],q=geometry->paths[a][i],r=geometry->paths[b][j-1],t=geometry->paths[b][j];
                if(p.x==q.x && r.x==t.x && p.x==r.x)
                    Check(std::max(std::min(p.y,q.y),std::min(r.y,t.y))>=std::min(std::max(p.y,q.y),std::max(r.y,t.y)),"Vertical ports cannot share a segment");
                if(p.y==q.y && r.y==t.y && p.y==r.y)
                    Check(std::max(std::min(p.x,q.x),std::min(r.x,t.x))>=std::min(std::max(p.x,q.x),std::max(r.x,t.x)),"Horizontal route segments cannot overlap");
                Check(std::min(p.y,q.y)>65 && std::min(r.y,t.y)>65,"Connections cannot enter the reserved title and repeat-label region");
            }
        Check(alternating.front().steps.size()==4 && alternating.front().steps[0].from==3 && alternating.front().steps[0].to==2
            && alternating.front().steps[1].from==2 && alternating.front().steps[1].to==3,
            "Record to Upload to Record keeps its real transitions instead of sorting or flattening by stage");
        trace.flow.links={{200,300},{300,200},{400,400}};
        const auto cyclic=viewer::BuildFlowRoutes(viewer::BuildFlowGraph(*chained,trace),trace);
        Check(std::count_if(cyclic.begin(),cyclic.end(),[](const auto& route){return !route.recording;})==2,
            "Reference cycles and self links are finite separate routes");

        const auto forward=viewer::RouteFlowEdge({100,100},{400,100},40,170);
        Check(forward.size()==2 && forward.front().x==140 && forward.back().x==360,"Forward actual links use one straight segment");
        const auto backward=viewer::RouteFlowEdge({400,100},{100,100},40,190);
        Check(backward.size()>=4,"Backward reference has a rectilinear return path");
        for(size_t i=1;i<backward.size();++i) Check(backward[i].x==backward[i-1].x || backward[i].y==backward[i-1].y,"Every return segment is axis aligned");
        const auto reference=viewer::RouteFlowEdge({100,100},{400,100},40,210,true);
        Check(reference.size()==4,"Forward resource references also use a rectilinear route");
        Check(viewer::FlowEdgeDetour({3,4,5,6},3,5) && !viewer::FlowEdgeDetour({3,5,6},3,5),
            "An actual Record to Submit link bypasses an intervening NativeTranslate circle; absent stages do not add detours");
        const auto bypass=viewer::RouteFlowEdge({100,100},{500,100},40,210,viewer::FlowEdgeDetour({3,4,5},3,5));
        for(int i=0;i<=100;++i)
        {
            const auto point=viewer::PointOnFlowPath(bypass,i/100.0);
            Check(std::hypot(point.x-300,point.y-100)>=40,"A marker must not enter an unconnected intermediate stage's circle");
        }
        const auto begin=viewer::PointOnFlowPath(backward,0),end=viewer::PointOnFlowPath(backward,1);
        Check(begin.x==backward.front().x && begin.y==backward.front().y && end.x==backward.back().x && end.y==backward.back().y,
            "Marker remains on the one real edge from its source to its own target");
        const auto middle=viewer::PointOnFlowPath(forward,.5);
        Check(middle.x==250 && middle.y==100,"Marker interpolation follows recorded-link geometry");
        std::puts("PASS immutable three-second flow, source/revision identity, actual active links, GPU frame isolation and straight/orthogonal marker geometry");
    }
    catch(const std::exception& error) { std::fprintf(stderr,"FAIL replay: %s\n",error.what()); return 1; }
    return 0;
}
