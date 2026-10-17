#include "FlowPlayback.h"
#include <algorithm>
#include <cmath>
#include <set>
#include <iomanip>
namespace viewer
{
bool ValidFrameEvidence(const D::LiveFrameEvidence& value,D::LiveId device,D::LiveId source,uint64_t producerNs)
{
    const auto& key=value.summary.key;
    return value.available && key.frame && key.device==device && key.source==source && value.flow.key==key
        && value.summary.appliedRevision && value.windowRevision==value.summary.appliedRevision
        && value.windowEndNs>=value.windowBeginNs && value.windowEndNs-value.windowBeginNs==D::LiveAverageWindowNs
        && value.windowEndNs<=producerNs && value.summary.endNs>=value.windowBeginNs && value.summary.endNs<value.windowEndNs
        && std::all_of(value.flow.events.begin(),value.flow.events.end(),[&](const auto& event){return event.context.frame==key && event.context.appliedRevision==value.summary.appliedRevision;})
        && std::all_of(value.flow.gpuSpans.begin(),value.flow.gpuSpans.end(),[&](const auto& span){return span.frame==key && span.appliedRevision==value.summary.appliedRevision;});
}
bool CompleteFrameEvidence(const D::LiveFrameEvidence& value,D::LiveId device,D::LiveId source,uint64_t producerNs)
{
    const auto& loss=value.loss;
    return ValidFrameEvidence(value,device,source,producerNs) && !value.budgetLimited && !value.summary.incomplete
        && !value.flow.omittedEvents && !value.flow.omittedLinks && !loss.events && !loss.links && !loss.frames
        && !loss.inventory && !loss.gpuSpans && !loss.transferTruncated;
}
void ComparisonPlayback::Clear()
{ evidence.reset(); session={}; sequence=producerNs=startedNs=generation=0; selectedDevice=selectedSource=selectedRequest=0; }
bool ComparisonPlayback::Update(std::shared_ptr<const D::LiveSnapshot> incoming,D::LiveId device,D::LiveId source,D::LiveId request,uint64_t nowNs)
{
    if(device!=selectedDevice || source!=selectedSource || request!=selectedRequest || (incoming && incoming->session!=session)) Clear();
    selectedDevice=device; selectedSource=source; selectedRequest=request;
    if(!incoming || !request) return false;
    session=incoming->session;
    if(evidence && nowNs>=startedNs && nowNs-startedNs<FlowPlayback::DurationNs) return false;
    for(const auto& value:incoming->sources) if(value.device==device && value.source==source && value.comparisonEvidence.request==request)
    {
        evidence=value.comparisonEvidence; sequence=incoming->sequence; producerNs=incoming->producerTimeNs;
        startedNs=nowNs; ++generation; return true;
    }
    return false;
}
double ComparisonPlayback::Phase(uint64_t nowNs) const
{ return !evidence || nowNs<startedNs ? 0 : std::min(1.0,double(nowNs-startedNs)/double(FlowPlayback::DurationNs)); }
void StatisticsView::Clear() { state.Clear(); history.clear(); updatedNs=nextUpdateNs=0; }
bool StatisticsView::Update(std::shared_ptr<const D::LiveSnapshot> incoming,D::LiveId device,D::LiveId source,uint64_t nowNs)
{
    if(state.device!=device || state.source!=source || (incoming && state.snapshot && incoming->session!=state.snapshot->session)) Clear();
    state.SelectSource(device,source);
    if(!incoming || nowNs<nextUpdateNs) return false;
    for(const auto& value:incoming->sources) if(value.device==device && value.source==source)
    {
        if(value.recent.endNs<value.recent.beginNs || value.recent.endNs-value.recent.beginNs!=IntervalNs
            || value.recent.endNs>incoming->producerTimeNs || !state.Accept(incoming,nowNs)) return false;
        state.SelectSource(device,source); updatedNs=nowNs; nextUpdateNs=nowNs+IntervalNs;
        history=value.recentSeconds;
        return true;
    }
    return false;
}
std::optional<StatisticsObservation> ObserveStatistics(const StatisticsView& values)
{
    const auto* source=values.state.Source();
    if(!source || !values.state.snapshot) return {};
    StatisticsObservation shown;
    shown.values={values.state.snapshot->session,values.state.snapshot->sequence,0,source->recent,source->runRanges};
    return shown;
}
std::optional<FlowObservationPresentation> ObserveReplay(const FlowPlayback& replay)
{
    const auto* source=replay.Source();
    if(!source || !replay.snapshot || source->flow.key!=replay.frame) return {};
    FlowObservationPresentation shown;
    shown.values={replay.snapshot->session,replay.frame,replay.revision,replay.snapshot->sequence,replay.startedNs,0};
    shown.observed=ObserveFlow(BuildFlowGraph(*replay.snapshot,*source),*source);
    shown.loss=replay.snapshot->loss; shown.incomplete=source->latest.incomplete;
    shown.omittedEvents=source->flow.omittedEvents; shown.omittedLinks=source->flow.omittedLinks;
    return shown;
}
void PresentationLog::PresentedObservations(const StatisticsObservation* shownStatistics,const FlowObservationPresentation* shownFlow,uint64_t nowNs)
{
    const auto any=[](const auto& rows) { for(const auto& row:rows) if(std::any_of(row.begin(),row.end(),[](auto value){return value!=0;})) return true; return false; };
    observationCountersVisible=shownStatistics && any(shownStatistics->counterVisibility);
    observationStagesVisible=shownStatistics && any(shownStatistics->stageVisibility);
    observationFlowVisible=shownFlow && (shownFlow->headerVisible || shownFlow->multiInstanceVisible || !shownFlow->events.empty()
        || std::any_of(shownFlow->presentOutcomeVisibility.begin(),shownFlow->presentOutcomeVisibility.end(),[](auto value){return value!=0;})
        || std::any_of(shownFlow->commandVisibility.begin(),shownFlow->commandVisibility.end(),[](auto value){return value!=0;}));
    if(observationCountersVisible) { observationCounters=*shownStatistics; observationCounters->values.presentedNs=nowNs; }
    if(observationStagesVisible) { observationStages=*shownStatistics; observationStages->values.presentedNs=nowNs; }
    if(observationFlowVisible) { observationFlow=*shownFlow; observationFlow->values.presentedNs=nowNs; }
}
void PresentationLog::Presented(const StatisticsView& values,const FlowPlayback& replay,bool statisticsDrawn,bool flowDrawn,uint64_t nowNs)
{
    const auto* source=values.state.Source();
    statisticsVisible=statisticsDrawn && source && values.state.snapshot;
    flowVisible=flowDrawn && replay.snapshot;
    if(statisticsVisible && (!statistics || statistics->sequence!=values.state.snapshot->sequence || statistics->session!=values.state.snapshot->session
        || statistics->average.device!=source->device || statistics->average.source!=source->source))
    {
        statistics=StatisticsPresentation{values.state.snapshot->session,values.state.snapshot->sequence,nowNs,source->recent,source->runRanges};
        ++statisticsUpdates;
    }
    if(flowVisible && (!flow || flow->cycleStartedNs!=replay.startedNs || flow->frame!=replay.frame || flow->session!=replay.snapshot->session))
    {
        flow=FlowPresentation{replay.snapshot->session,replay.frame,replay.revision,replay.snapshot->sequence,replay.startedNs,nowNs}; ++flowCycles;
    }
}
void PresentationLog::Write(std::ostream& out) const
{
    out<<"display_policy=producer_completed_1s_average_and_3s_recorded_relationship_replay\nlegacy_250ms_gate=retired_by_user\n"
        <<"statistics_visible_at_last_present="<<statisticsVisible<<"\nflow_visible_at_last_present="<<flowVisible
        <<"\nstatistics_presented_updates="<<statisticsUpdates<<"\nflow_presented_cycles="<<flowCycles<<'\n';
    out<<"observation_measurement_scope=instrumented_frame_or_work_interval_not_display_fps\nobservation_counters_visible_at_last_present="<<observationCountersVisible
        <<"\nobservation_stages_visible_at_last_present="<<observationStagesVisible<<"\nobservation_flow_visible_at_last_present="<<observationFlowVisible<<'\n';
    const auto observationWindow=[&](const char* name,const StatisticsObservation& shown) {
        const auto& v=shown.values; const auto& a=v.average;
        out<<"observation_"<<name<<"_last_presented="<<v.session.high<<','<<v.session.low<<','<<v.sequence<<','<<a.device<<','<<a.source<<','
            <<a.beginNs<<','<<a.endNs<<','<<a.appliedRevision<<','<<a.mixedRevision<<','<<a.frameCount<<','<<v.presentedNs<<'\n';
    };
    const auto number=[&](std::optional<double> value) { out<<','<<bool(value)<<','; if(value) out<<std::setprecision(12)<<*value; else out<<"--"; out<<'\n'; };
    if(observationCounters)
    {
        const auto& shown=*observationCounters; observationWindow("counter",shown);
        for(size_t phase=0;phase<3;++phase) for(size_t i=0;i<ComparisonCounters().size();++i) if(shown.counterVisibility[phase][i])
        { const auto& field=ComparisonCounters()[i]; out<<"observation_counter="<<phase<<','<<field.key; number(CounterNumber(shown.values.average,CounterPhase(phase),field)); }
    }
    if(observationStages)
    {
        const auto& shown=*observationStages; observationWindow("stage",shown);
        for(size_t stage=0;stage<D::LiveStageCount;++stage) for(size_t metric=0;metric<size_t(StageMetric::Count);++metric) if(shown.stageVisibility[stage][metric])
        {
            out<<"observation_stage="<<stage<<','<<metric; number(StageNumber(shown.values.average,D::LiveStage(stage),StageMetric(metric)));
            if(metric<2)
            {
                const auto& timing=metric==0 ? shown.values.average.stages[stage].cpu : shown.values.average.stages[stage].gpu;
                out<<"observation_stage_coverage="<<stage<<','<<metric<<','<<int(timing.state)<<','<<timing.ready<<','<<shown.values.average.frameCount<<','
                    <<timing.pending<<','<<timing.unsupported<<','<<timing.unavailable<<'\n';
            }
        }
    }
    if(observationFlow)
    {
        const auto& shown=*observationFlow; const auto& v=shown.values;
        out<<"observation_flow_last_presented="<<v.session.high<<','<<v.session.low<<','<<v.sequence<<','<<v.frame.device<<','<<v.frame.source<<','<<v.frame.frame<<','
            <<v.revision<<','<<v.cycleStartedNs<<','<<v.presentedNs<<'\n';
        out<<"observation_flow_loss="<<shown.incomplete<<','<<shown.omittedEvents<<','<<shown.omittedLinks<<','<<shown.loss.events<<','<<shown.loss.links<<','
            <<shown.loss.frames<<','<<shown.loss.inventory<<','<<shown.loss.gpuSpans<<','<<shown.loss.transferTruncated<<'\n';
        for(size_t i=0;i<shown.commandVisibility.size();++i) if(shown.commandVisibility[i]) out<<"observation_command="<<i<<','<<shown.observed.commandKinds[i]<<'\n';
        if(shown.multiInstanceVisible) out<<"observation_multi_instance_draws="<<shown.observed.multiInstanceDraws<<'\n';
        for(size_t i=0;i<shown.observed.presentOutcomes.size();++i) if(shown.presentOutcomeVisibility[i])
            out<<"observation_present_outcome="<<i<<','<<shown.observed.presentOutcomes[i]<<'\n';
        for(const auto& item:shown.events)
        {
            const auto& event=item.event; const auto mask=item.visibility;
            out<<"observation_event="<<event.id<<','<<unsigned(mask)<<',';
            if(mask&1) out<<int(event.kind)<<','<<int(event.commandKind); else out<<"--,--";
            out<<','; if(mask&4) out<<int(event.outcome); else out<<"--";
            for(const auto argument:event.arguments) { out<<','; if(mask&8) out<<argument; else out<<"--"; }
            out<<','; if(mask&2) out<<std::quoted(std::string(event.label,std::find(std::begin(event.label),std::end(event.label),'\0'))); else out<<"--";
            out<<'\n';
        }
    }
    if(statistics)
    {
        const auto& value=*statistics; const auto& a=value.average;
        const auto rate=WindowRate(a);
        out<<"statistics_window_rate="<<a.frameCount<<','<<(a.endNs>=a.beginNs ? a.endNs-a.beginNs : 0)<<','<<bool(rate)<<',';
        if(rate) out<<std::setprecision(12)<<*rate; else out<<"--"; out<<'\n';
        out<<"statistics_session="<<value.session.high<<','<<value.session.low<<'\n';
        out<<"statistics_last_presented="<<value.sequence<<','<<a.device<<','<<a.source<<','<<a.beginNs<<','<<a.endNs<<','<<a.appliedRevision<<','<<a.mixedRevision<<','<<value.presentedNs<<'\n';
        const auto metric=[&](const char* name,double mean,D::LiveValueState state,const D::LiveMetricRange& range) {
            out<<"statistics_metric="<<name<<','<<int(state)<<',';
            if(state==D::LiveValueState::Ready && std::isfinite(mean)) out<<std::setprecision(12)<<mean; else out<<"--";
            out<<','<<int(range.state)<<',';
            if(range.state==D::LiveValueState::Ready && range.windows && std::isfinite(range.low) && std::isfinite(range.high)) out<<range.low<<','<<range.high; else out<<"--,--";
            out<<','<<range.windows<<'\n';
        };
        const auto counterState=a.frameCount ? D::LiveValueState::Ready : D::LiveValueState::Unavailable;
        const auto metricState=[](const auto& value) {
            return value.state==D::LiveValueState::Ready && (!value.ready || !std::isfinite(value.average)) ? D::LiveValueState::Unavailable : value.state;
        };
        const auto fpsState=a.frame.state==D::LiveValueState::Ready && a.frame.ready && a.frame.average>0
            ? D::LiveValueState::Ready : D::LiveValueState::Unavailable;
        metric("fps",a.fps,fpsState,value.ranges.fps);
        metric("frame_ms",a.frame.average,metricState(a.frame),value.ranges.frame);
        metric("cpu_render_ms",a.cpuRender.average,metricState(a.cpuRender),value.ranges.cpuRender);
        metric("gpu_measured_ms",a.gpuMeasured.average,metricState(a.gpuMeasured),value.ranges.gpuMeasured);
        metric("draws_per_frame",a.recordedMean.draw+a.recordedMean.drawIndexed,counterState,value.ranges.drawCalls);
        metric("instances_per_frame",a.recordedMean.instances,counterState,value.ranges.instances);
        metric("batches_per_frame",a.recordedMean.batches,counterState,value.ranges.batches);
        metric("upload_bytes_per_frame",a.recordedMean.uploadBytes,counterState,value.ranges.uploadBytes);
        metric("objects",a.objects.average,metricState(a.objects),value.ranges.objects);
    }
    if(flow)
    {
        const auto& value=*flow;
        out<<"flow_session="<<value.session.high<<','<<value.session.low<<'\n';
        out<<"flow_last_presented="<<value.sequence<<','<<value.frame.device<<','<<value.frame.source<<','<<value.frame.frame<<','<<value.revision<<','<<value.cycleStartedNs<<','<<value.presentedNs<<'\n';
    }
    out<<"comparison_visible_at_last_present="<<comparisonVisible<<"\ncomparison_stages_visible_at_last_present="<<comparisonStagesVisible
        <<"\ncomparison_counters_visible_at_last_present="<<comparisonCountersVisible<<"\ncomparison_flow_visible_at_last_present="<<comparisonFlowVisible<<'\n';
    if(comparison)
    {
        const auto& value=*comparison; const auto& status=value.status;
        out<<"comparison_session="<<value.session.high<<','<<value.session.low<<'\n';
        out<<"comparison_last_presented="<<value.sequence<<','<<status.request<<','<<status.control<<','<<status.device<<','<<status.source<<','
            <<status.appliedFrame.frame<<','<<status.appliedRevision<<','<<value.presentedNs<<'\n';
        out<<"comparison_requested="<<ValueText(status.requested)<<"\ncomparison_actual="<<ValueText(status.actual)
            <<"\ncomparison_previous_actual="<<ValueText(status.previousActual)<<'\n';
        out<<"comparison_revision_context="<<status.appliedRevision<<','<<value.currentRevision<<','<<(value.currentRevision>status.appliedRevision)<<'\n';
        for(const auto& change:value.revisionChanges) out<<"comparison_known_revision_change="<<change.request<<','<<change.control<<','<<ValueText(change.before)<<','<<ValueText(change.after)<<'\n';
        const auto window=[&](const char* name,const auto& a,bool ready) {
            out<<"comparison_"<<name<<"="<<a.beginNs<<','<<a.endNs<<','<<a.appliedRevision<<','<<a.frameCount<<','<<ready<<','<<a.mixedRevision<<'\n';
        };
        window("before",status.baseline,value.beforeReady); window("after",status.after,value.afterReady);
        for(int side=0;side<2;++side)
        {
            const auto& a=side ? status.after : status.baseline;
            const auto rate=(side ? value.afterReady : value.beforeReady) ? WindowRate(a) : std::nullopt;
            out<<"comparison_window_rate="<<side<<','<<a.frameCount<<','<<(a.endNs>=a.beginNs ? a.endNs-a.beginNs : 0)<<','<<bool(rate)<<',';
            if(rate) out<<*rate; else out<<"--"; out<<'\n';
        }
        const char* names[]{"frames","fps","frame_ms","cpu_ms","gpu_ms","draws","instances","dispatch","upload_bytes"};
        for(size_t i=0;i<9;++i)
        {
            const auto visible=value.metricVisibility[i]; if(!visible) continue;
            const auto metric=ComparisonMetric(i);
            const auto before=value.beforeReady ? ComparisonNumber(status.baseline,metric) : std::nullopt;
            const auto after=value.afterReady ? ComparisonNumber(status.after,metric) : std::nullopt;
            out<<"comparison_metric="<<names[i]<<','<<bool(before && (visible&1))<<',';
            if(before && (visible&1)) out<<*before; else out<<"--";
            out<<','<<bool(after && (visible&2))<<','; if(after && (visible&2)) out<<*after; else out<<"--";
            out<<','; if(before && after && (visible&4)) out<<*after-*before; else out<<"--";
            out<<','; if(before && after && *before!=0 && (visible&8)) out<<(*after-*before)/std::abs(*before)*100; else out<<"--";
            out<<','<<unsigned(visible)<<'\n';
        }
        const auto cells=[&](const NumberChange& change,unsigned char visible) {
            out<<','<<bool(change.before && (visible&1))<<',';
            if(change.before && (visible&1)) out<<*change.before; else out<<"--";
            out<<','<<bool(change.after && (visible&2))<<',';
            if(change.after && (visible&2)) out<<*change.after; else out<<"--";
            out<<','; if(change.delta && (visible&4)) out<<*change.delta; else out<<"--";
            out<<','; if(change.percent && (visible&8)) out<<*change.percent; else out<<"--";
            out<<','<<unsigned(visible)<<'\n';
        };
        const auto provenance=[&](const char* name,const ComparisonPresentation& sample) {
            const auto& s=sample.status;
            out<<"comparison_"<<name<<"_last_presented="<<sample.sequence<<','<<s.request<<','<<s.control<<','<<s.device<<','<<s.source<<','<<sample.presentedNs<<','
                <<s.baseline.beginNs<<','<<s.baseline.endNs<<','<<s.baseline.appliedRevision<<','<<sample.beforeReady<<','
                <<s.after.beginNs<<','<<s.after.endNs<<','<<s.after.appliedRevision<<','<<sample.afterReady<<'\n';
            out<<"comparison_"<<name<<"_revision_context="<<s.appliedRevision<<','<<sample.currentRevision<<','<<(sample.currentRevision>s.appliedRevision)<<'\n';
        };
        const auto& stages=stageComparison ? *stageComparison : value; const auto& stageStatus=stages.status;
        if(stageComparison) provenance("stage",stages);
        for(size_t stage=0;stage<D::LiveStageCount;++stage) for(size_t metric=0;metric<size_t(StageMetric::Count);++metric)
        {
            const auto visible=stages.stageVisibility[stage][metric]; if(!visible) continue;
            out<<"comparison_stage="<<stage<<','<<metric;
            cells(CompareNumbers(stages.beforeReady ? StageNumber(stageStatus.baseline,D::LiveStage(stage),StageMetric(metric)) : std::nullopt,
                stages.afterReady ? StageNumber(stageStatus.after,D::LiveStage(stage),StageMetric(metric)) : std::nullopt),visible);
            if(metric<=size_t(StageMetric::Gpu))
            {
                const auto& a=metric==size_t(StageMetric::Cpu) ? stageStatus.baseline.stages[stage].cpu : stageStatus.baseline.stages[stage].gpu;
                const auto& b=metric==size_t(StageMetric::Cpu) ? stageStatus.after.stages[stage].cpu : stageStatus.after.stages[stage].gpu;
                out<<"comparison_stage_coverage="<<stage<<','<<metric<<','<<a.ready<<','<<stageStatus.baseline.frameCount<<','
                    <<a.pending<<','<<a.unsupported<<','<<a.unavailable<<','<<b.ready<<','<<stageStatus.after.frameCount<<','<<b.pending<<','<<b.unsupported<<','<<b.unavailable<<'\n';
            }
        }
        const auto& counters=counterComparison ? *counterComparison : value; const auto& counterStatus=counters.status;
        if(counterComparison) provenance("counter",counters);
        for(size_t phase=0;phase<3;++phase) for(size_t metric=0;metric<ComparisonCounters().size();++metric)
        {
            const auto visible=counters.counterVisibility[phase][metric]; if(!visible) continue;
            const auto& field=ComparisonCounters()[metric]; out<<"comparison_counter="<<phase<<','<<field.key;
            cells(CompareNumbers(counters.beforeReady ? CounterNumber(counterStatus.baseline,CounterPhase(phase),field) : std::nullopt,
                counters.afterReady ? CounterNumber(counterStatus.after,CounterPhase(phase),field) : std::nullopt),visible);
        }
    }
    if(comparisonFlow)
    {
        const auto& value=*comparisonFlow;
        out<<"comparison_flow_session="<<value.session.high<<','<<value.session.low<<'\n';
        out<<"comparison_flow_last_presented="<<value.sequence<<','<<value.request<<','<<value.cycleStartedNs<<','<<value.presentedNs<<'\n';
        for(size_t i=0;i<2;++i)
        {
            const auto& frame=value.frames[i];
            out<<"comparison_flow_frame="<<i<<','<<frame.frame.device<<','<<frame.frame.source<<','<<frame.frame.frame<<','<<frame.revision<<','
                <<frame.windowBeginNs<<','<<frame.windowEndNs<<','<<frame.windowRevision<<','<<frame.available<<','<<frame.valid<<','<<frame.complete<<','
                <<frame.matchesAverageWindow<<','<<frame.canvasVisible<<','<<frame.budgetLimited<<','<<frame.incomplete<<','<<frame.omittedEvents<<','<<frame.omittedLinks<<'\n';
            const auto& loss=frame.loss;
            out<<"comparison_flow_loss="<<i<<','<<loss.events<<','<<loss.links<<','<<loss.frames<<','<<loss.inventory<<','<<loss.gpuSpans<<','<<loss.transferTruncated<<'\n';
            out<<"comparison_flow_route="<<i<<','<<frame.routeLinks<<','<<frame.routeSteps;
            for(const auto id:frame.visibleOccurrences) out<<','<<id;
            out<<'\n';
        }
        const auto cells=[&](double before,double after,unsigned char visible) {
            const auto& a=value.frames[0]; const auto& b=value.frames[1];
            const auto change=CompareObservedCounts(a.valid ? std::optional<double>(before) : std::nullopt,b.valid ? std::optional<double>(after) : std::nullopt,a.complete,b.complete);
            out<<','<<bool(change.before && (visible&1))<<','; if(change.before && (visible&1)) out<<*change.before; else out<<"--";
            out<<','<<bool(change.after && (visible&2))<<','; if(change.after && (visible&2)) out<<*change.after; else out<<"--";
            out<<','; if(change.delta && (visible&4)) out<<*change.delta; else out<<"--";
            out<<','; if(change.percent && (visible&8)) out<<*change.percent; else out<<"--";
            out<<','<<unsigned(visible)<<'\n';
        };
        for(size_t i=0;i<value.stageVisibility.size();++i) if(const auto visible=value.stageVisibility[i])
        { out<<"comparison_flow_stage="<<i; cells(double(value.frames[0].observed.stageEvents[i]),double(value.frames[1].observed.stageEvents[i]),visible); }
        for(size_t i=0;i<value.commandVisibility.size();++i) if(const auto visible=value.commandVisibility[i])
        { out<<"comparison_flow_command="<<i; cells(double(value.frames[0].observed.commandKinds[i]),double(value.frames[1].observed.commandKinds[i]),visible); }
        const auto count=[](const FlowObservations& observed,int from,int to) { for(const auto& link:observed.links) if(link.from==from && link.to==to) return link.links; return uint64_t(0); };
        for(int from=0;from<=FlowReferenceColumn;++from) for(int to=0;to<=FlowReferenceColumn;++to)
            if(const auto visible=value.linkVisibility[size_t(from*(FlowReferenceColumn+1)+to)])
            { out<<"comparison_flow_link="<<from<<','<<to; cells(double(count(value.frames[0].observed,from,to)),double(count(value.frames[1].observed,from,to)),visible); }
    }
}
void PresentationLog::PresentedComparison(const ComparisonPresentation* shown,uint64_t nowNs)
{
    comparisonVisible=shown!=nullptr;
    const auto any=[](const auto& rows) { for(const auto& row:rows) if(std::any_of(row.begin(),row.end(),[](auto flag){return flag!=0;})) return true; return false; };
    comparisonStagesVisible=shown && any(shown->stageVisibility); comparisonCountersVisible=shown && any(shown->counterVisibility);
    comparisonFlowVisible=shown && shown->flow.has_value();
    if(shown)
    {
        comparison=*shown; comparison->presentedNs=nowNs;
        if(comparisonStagesVisible) { stageComparison=*shown; stageComparison->flow.reset(); stageComparison->presentedNs=nowNs; }
        if(comparisonCountersVisible) { counterComparison=*shown; counterComparison->flow.reset(); counterComparison->presentedNs=nowNs; }
        if(shown->flow) { comparisonFlow=shown->flow; comparisonFlow->presentedNs=nowNs; }
    }
}
void FlowPlayback::Clear()
{ snapshot.reset(); frame={}; revision=startedNs=generation=0; session={}; selectedDevice=selectedSource=0; }
bool FlowPlayback::Update(std::shared_ptr<const D::LiveSnapshot> incoming,D::LiveId device,D::LiveId source,uint64_t nowNs)
{
    if(device!=selectedDevice || source!=selectedSource || (incoming && incoming->session!=session)) Clear();
    selectedDevice=device; selectedSource=source;
    if(!incoming) return false;
    session=incoming->session;
    if(snapshot && nowNs>=startedNs && nowNs-startedNs<DurationNs) return false;
    for(const auto& value:incoming->sources) if(value.device==device && value.source==source)
    {
        if(!value.flow.key.frame || value.flow.key!=value.latest.key || value.flow.key.device!=device || value.flow.key.source!=source
            || (value.flow.events.empty() && value.flow.links.empty() && value.flow.gpuSpans.empty())
            || !std::all_of(value.flow.events.begin(),value.flow.events.end(),[&](const auto& event) {
                return event.context.frame==value.flow.key && event.context.appliedRevision==value.latest.appliedRevision;
            })) return false;
        snapshot=std::move(incoming); frame=value.flow.key; revision=value.latest.appliedRevision; startedNs=nowNs; ++generation; return true;
    }
    return false;
}
const D::LiveSourceSnapshot* FlowPlayback::Source() const
{
    if(snapshot) for(const auto& value:snapshot->sources) if(value.device==frame.device && value.source==frame.source) return &value;
    return nullptr;
}
double FlowPlayback::Phase(uint64_t nowNs) const
{ return !snapshot || nowNs<startedNs ? 0 : std::min(1.0,double(nowNs-startedNs)/double(DurationNs)); }
FlowRoutePosition LocateFlowRoute(const FlowRoute& route,double phase)
{
    if(route.steps.empty()) return {};
    const auto scaled=std::clamp(std::isfinite(phase) ? phase : 0.0,0.0,1.0)*double(route.steps.size());
    const auto step=std::min(size_t(scaled),route.steps.size()-1);
    return {step,scaled-double(step)};
}
FlowRouteLayout LayoutFlowRoute(const FlowRoute& route)
{
    FlowRouteLayout result;
    if(route.links.empty() || route.steps.empty()) return result;
    for(size_t i=1;i<route.links.size();++i) if(route.links[i-1].to!=route.links[i].from) return {};
    result.ids.push_back(route.links.front().from); size_t offset=0;
    for(const auto& step:route.steps)
    {
        if(!step.links || step.links>route.links.size()-offset) return {};
        offset+=size_t(step.links); result.ids.push_back(route.links[offset-1].to);
    }
    if(offset!=route.links.size()) return {};
    for(size_t i=0;i<result.ids.size();++i) result.nodes.push_back({result.Spacing*.5f+4+float(i)*result.Spacing,result.CenterY});
    result.height=result.CenterY+result.Radius+36;
    for(size_t i=0;i<route.steps.size();++i)
    {
        auto from=result.nodes[i],to=result.nodes[i+1]; const auto& step=route.steps[i];
        // Keep the moving marker below the value/unit text, including inside a node.
        from.y+=result.Radius*.9f; to.y=from.y;
        if(route.recording && step.from<step.to) result.paths.push_back({from,to});
        else
        {
            const float port=result.Radius*.5f,bottom=result.CenterY+result.Radius+16;
            result.paths.push_back({from,{from.x+port,from.y},{from.x+port,bottom},{to.x-port,bottom},{to.x-port,to.y},to});
        }
    }
    result.width=result.nodes.back().x+result.Spacing*.5f+4;
    return result;
}
std::vector<int> ActiveFlowColumns(const FlowGraph& graph)
{
    std::set<int> active;
    for(size_t i=0;i<graph.activeStages.size();++i) if(graph.activeStages[i]) active.insert(int(i));
    for(const auto& node:graph.nodes) if(node.column==FlowReferenceColumn) {active.insert(FlowReferenceColumn);break;}
    return {active.begin(),active.end()};
}
bool FlowEdgeDetour(const std::vector<int>& columns,int from,int to)
{
    if(from>=to || from==FlowReferenceColumn || to==FlowReferenceColumn) return true;
    const auto position=std::find(columns.begin(),columns.end(),from);
    return position==columns.end() || position+1==columns.end() || *(position+1)!=to;
}
std::vector<FlowPoint> RouteFlowEdge(FlowPoint from,FlowPoint to,float radius,float lane,bool detour)
{
    if(from.x<to.x && !detour) return {{from.x+radius,from.y},{to.x-radius,to.y}};
    const float bottom=std::max(lane,std::max(from.y,to.y)+radius+20);
    if(from.x==to.x && from.y==to.y)
        return {{from.x+radius,from.y},{from.x+radius+24,from.y},{from.x+radius+24,bottom},{to.x,bottom},{to.x,to.y+radius}};
    return {{from.x,from.y+radius},{from.x,bottom},{to.x,bottom},{to.x,to.y+radius}};
}
FlowPoint PointOnFlowPath(const std::vector<FlowPoint>& path,double phase)
{
    if(path.empty()) return {};
    double length=0;
    for(size_t i=1;i<path.size();++i) length+=std::hypot(double(path[i].x-path[i-1].x),double(path[i].y-path[i-1].y));
    double remaining=length*std::clamp(std::isfinite(phase) ? phase : 0.0,0.0,1.0);
    for(size_t i=1;i<path.size();++i)
    {
        const auto distance=std::hypot(double(path[i].x-path[i-1].x),double(path[i].y-path[i-1].y));
        if(distance>0 && remaining<distance)
        { const auto t=float(remaining/distance); return {path[i-1].x+(path[i].x-path[i-1].x)*t,path[i-1].y+(path[i].y-path[i-1].y)*t}; }
        remaining-=distance;
    }
    return path.back();
}
}
