#include "FlowGraph.h"
#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <map>

namespace viewer
{
static FlowObservations ObserveEvents(const FlowGraph& graph,const D::LiveFlowSnapshot& flow)
{
    FlowObservations result; result.links=GroupFlowLinks(graph);
    for(const auto& node:graph.nodes)
    {
        if(node.column>=0 && node.column<int(D::LiveStageCount) && (node.kind==FlowNodeKind::Event || node.kind==FlowNodeKind::Gpu))
            ++result.stageEvents[size_t(node.column)];
        if(node.kind==FlowNodeKind::Event && node.index<flow.events.size())
        {
            const auto& event=flow.events[node.index];
            auto kind=event.commandKind;
            const bool command=event.kind==D::LiveEventKind::Command || (event.kind==D::LiveEventKind::Upload && kind==D::LiveCommandKind::Upload)
                || (event.kind==D::LiveEventKind::Output && kind==D::LiveCommandKind::Present);
            const bool wait=event.kind==D::LiveEventKind::Wait && event.outcome!=D::LiveOutcome::Skipped;
            if(wait) kind=D::LiveCommandKind::Wait;
            if((command || wait) && kind!=D::LiveCommandKind::None && size_t(kind)<result.commandKinds.size()) ++result.commandKinds[size_t(kind)];
        }
    }
    return result;
}
std::vector<FlowBundle> GroupFlowLinks(const FlowGraph& graph)
{
    std::unordered_map<D::LiveId,int> columns;
    for(const auto& node:graph.nodes) columns[node.id]=node.column;
    std::map<std::pair<int,int>,uint64_t> counts;
    for(const auto& link:graph.links)
    {
        const auto from=columns.find(link.from),to=columns.find(link.to);
        if(from!=columns.end() && to!=columns.end()) ++counts[{from->second,to->second}];
    }
    std::vector<FlowBundle> result;
    for(const auto& count:counts) result.push_back({count.first.first,count.first.second,count.second});
    return result;
}
std::vector<const D::LiveGpuSpan*> GpuSamples(const D::LiveSourceSnapshot& source)
{
    std::vector<const D::LiveGpuSpan*> result;
    std::unordered_map<D::LiveId,size_t> positions;
    const auto add=[&](const auto& spans) {
        for(const auto& span:spans)
        {
            const auto inserted=positions.emplace(span.id,result.size());
            if(inserted.second) result.push_back(&span); else result[inserted.first->second]=&span;
        }
    };
    add(source.flow.gpuSpans); add(source.recentGpuSpans);
    return result;
}
static std::vector<FlowRoute> BuildRoutes(const FlowGraph& graph,const std::vector<D::LiveEvent>& records,const std::vector<const D::LiveGpuSpan*>& spans)
{
    std::unordered_map<D::LiveId,int> columns;
    std::unordered_map<D::LiveId,const D::LiveEvent*> events;
    std::unordered_map<D::LiveId,const D::LiveGpuSpan*> gpu;
    for(const auto& node:graph.nodes) columns.emplace(node.id,node.column);
    for(const auto& event:records) events.emplace(event.id,&event);
    for(const auto* span:spans) gpu.emplace(span->id,span);
    std::array<std::vector<D::LiveFlowLink>,2> groups;
    for(const auto& link:graph.links)
    {
        bool recording=false;
        const auto from=events.find(link.from),to=events.find(link.to);
        if(from!=events.end() && to!=events.end())
        {
            const auto& a=*from->second; const auto& b=*to->second;
            recording=a.command.commandList && a.command.commandList==b.command.commandList && a.command.generation==b.command.generation
                && a.kind!=D::LiveEventKind::Submission
                && (a.command.index<b.command.index || (b.kind==D::LiveEventKind::Submission && a.command.index==b.command.index));
        }
        const auto span=gpu.find(link.to);
        if(from!=events.end() && from->second->kind==D::LiveEventKind::Submission && span!=gpu.end())
            recording=from->second->submission && (from->second->submission==span->second->begin.submission || from->second->submission==span->second->end.submission);
        groups[recording ? 0 : 1].push_back(link);
    }
    std::vector<FlowRoute> result;
    for(size_t group=0;group<groups.size();++group)
    {
        const auto& links=groups[group];
        std::unordered_map<D::LiveId,std::vector<size_t>> outgoing;
        std::unordered_map<D::LiveId,size_t> incoming;
        for(size_t i=0;i<links.size();++i) { outgoing[links[i].from].push_back(i); ++incoming[links[i].to]; }
        std::vector<bool> used(links.size(),false);
        const auto walk=[&](size_t start) {
            FlowRoute route; route.recording=group==0;
            for(size_t i=start;!used[i];)
            {
                used[i]=true; const auto& link=links[i]; route.links.push_back(link);
                const auto from=columns.at(link.from),to=columns.at(link.to);
                if(!route.steps.empty() && from==to && route.steps.back().from==from && route.steps.back().to==to) ++route.steps.back().links;
                else route.steps.push_back({from,to,1});
                const auto next=outgoing.find(link.to);
                // Branches end a route; edge membership does not reveal which branch executed next.
                if(incoming[link.to]!=1 || next==outgoing.end() || next->second.size()!=1) break;
                i=next->second.front();
            }
            result.push_back(std::move(route));
        };
        for(size_t i=0;i<links.size();++i)
            if(!used[i] && (incoming[links[i].from]!=1 || outgoing[links[i].from].size()!=1)) walk(i);
        for(size_t i=0;i<links.size();++i) if(!used[i]) walk(i);
    }
    // Pick a useful visible chain, without imposing an execution order on independent routes.
    std::stable_sort(result.begin(),result.end(),[](const auto& a,const auto& b) {
        return a.recording!=b.recording ? a.recording : a.links.size()>b.links.size();
    });
    return result;
}
static int Column(D::LiveStage stage)
{
    return size_t(stage)<D::LiveStageCount ? int(stage) : FlowReferenceColumn;
}

static FlowGraph BuildGraph(const std::vector<D::LiveObjectInfo>& objects,const D::LiveFrameSummary& summary,const D::LiveFlowSnapshot& flow,const std::vector<const D::LiveGpuSpan*>& gpu,D::LiveId focus)
{
    FlowGraph result;
    if(!flow.key.frame || flow.key!=summary.key) return result;
    for(size_t i=0;i<D::LiveStageCount;++i)
    {
        const auto& stage=summary.stages[i];
        result.activeStages[i]=stage.operations || stage.elements || stage.bytes || stage.cpu.state==D::LiveValueState::Ready;
    }
    std::unordered_map<D::LiveId, FlowNode> records;
    for(size_t i=0; i<objects.size(); ++i) records.emplace(objects[i].id, FlowNode{objects[i].id,FlowNodeKind::Object,i,FlowReferenceColumn});
    const auto currentEvent=[&](const auto& event){return event.context.frame==flow.key && event.context.appliedRevision==summary.appliedRevision;};
    for(size_t i=0; i<flow.events.size(); ++i) if(currentEvent(flow.events[i]))
    {
        const auto column=Column(flow.events[i].stage);
        records[flow.events[i].id]={flow.events[i].id,FlowNodeKind::Event,i,column};
        if(column<FlowReferenceColumn) result.activeStages[size_t(column)]=true;
    }
    for(size_t i=0; i<flow.events.size(); ++i)
    {
        const auto& event=flow.events[i];
        if(!currentEvent(event)) continue;
        if(event.context.batch) records.emplace(event.context.batch,FlowNode{event.context.batch,FlowNodeKind::BatchReference,i,FlowReferenceColumn});
        if(event.kind==D::LiveEventKind::Submission && event.submission) records.emplace(event.submission,FlowNode{event.submission,FlowNodeKind::SubmissionReference,i,FlowReferenceColumn});
    }
    std::unordered_set<D::LiveId> foreign;
    for(const auto& event:flow.events) if(!currentEvent(event)) foreign.insert(event.id);
    for(size_t i=0; i<gpu.size(); ++i)
    {
        if(gpu[i]->frame!=flow.key || gpu[i]->appliedRevision!=summary.appliedRevision) { foreign.insert(gpu[i]->id); continue; }
        records[gpu[i]->id]={gpu[i]->id,FlowNodeKind::Gpu,i,int(D::LiveStage::Gpu)};
        result.activeStages[size_t(D::LiveStage::Gpu)]=true;
    }
    std::vector<D::LiveFlowLink> links;
    for(const auto& link:flow.links)
        if(link.from && link.to && !foreign.count(link.from) && !foreign.count(link.to)) links.push_back(link);
        else ++result.rejectedLinks;
    std::unordered_set<D::LiveId> included;
    if(focus)
    {
        std::unordered_map<D::LiveId,std::vector<D::LiveId>> adjacent;
        for(const auto& link:links) { adjacent[link.from].push_back(link.to); adjacent[link.to].push_back(link.from); }
        if(!records.count(focus) && !adjacent.count(focus)) return {};
        std::vector<D::LiveId> queue{focus}; included.insert(focus);
        for(size_t i=0; i<queue.size(); ++i) for(const auto id:adjacent[queue[i]]) if(included.insert(id).second) queue.push_back(id);
    }
    else
    {
        for(const auto& event:flow.events) if(event.id && currentEvent(event)) included.insert(event.id);
        for(const auto* span:gpu) if(span->id && !foreign.count(span->id)) included.insert(span->id);
        for(const auto& link:links) { included.insert(link.from); included.insert(link.to); }
    }
    for(const auto id:included)
    {
        const auto found=records.find(id);
        result.nodes.push_back(found==records.end() ? FlowNode{id,FlowNodeKind::Unknown,0,FlowReferenceColumn} : found->second);
    }
    std::sort(result.nodes.begin(),result.nodes.end(),[](const auto& a,const auto& b) { return a.column==b.column ? a.id<b.id : a.column<b.column; });
    for(const auto& link:links) if(included.count(link.from) && included.count(link.to)) result.links.push_back(link);
    return result;
}
static std::vector<const D::LiveGpuSpan*> EvidenceSpans(const D::LiveFrameEvidence& evidence)
{
    std::vector<const D::LiveGpuSpan*> spans;
    for(const auto& span:evidence.flow.gpuSpans) spans.push_back(&span);
    return spans;
}
FlowGraph BuildFlowGraph(const D::LiveSnapshot& snapshot,const D::LiveSourceSnapshot& source,D::LiveId focus)
{ return BuildGraph(snapshot.objects,source.latest,source.flow,GpuSamples(source),focus); }
FlowGraph BuildFlowGraph(const D::LiveFrameEvidence& evidence)
{ return evidence.available ? BuildGraph(evidence.objects,evidence.summary,evidence.flow,EvidenceSpans(evidence),0) : FlowGraph{}; }
std::vector<FlowRoute> BuildFlowRoutes(const FlowGraph& graph,const D::LiveSourceSnapshot& source)
{ return BuildRoutes(graph,source.flow.events,GpuSamples(source)); }
std::vector<FlowRoute> BuildFlowRoutes(const FlowGraph& graph,const D::LiveFrameEvidence& evidence)
{ return BuildRoutes(graph,evidence.flow.events,EvidenceSpans(evidence)); }
FlowObservations ObserveFlow(const FlowGraph& graph,const D::LiveSourceSnapshot& source)
{ return ObserveEvents(graph,source.flow); }
FlowObservations ObserveFlow(const FlowGraph& graph,const D::LiveFrameEvidence& evidence)
{ return ObserveEvents(graph,evidence.flow); }
}
