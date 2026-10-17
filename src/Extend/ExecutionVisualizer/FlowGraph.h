#pragma once
#include "ViewerState.h"

namespace viewer
{
enum class FlowNodeKind { Event, Object, Gpu, Unknown, BatchReference, SubmissionReference };
constexpr int FlowReferenceColumn=int(D::LiveStage::Count);
struct FlowNode { D::LiveId id=0; FlowNodeKind kind=FlowNodeKind::Unknown; size_t index=0; int column=FlowReferenceColumn; };
struct FlowGraph
{
    std::vector<FlowNode> nodes;
    std::vector<D::LiveFlowLink> links;
    std::array<bool,D::LiveStageCount> activeStages{};
    uint64_t rejectedLinks=0;
};
FlowGraph BuildFlowGraph(const D::LiveSnapshot&, const D::LiveSourceSnapshot&, D::LiveId focus=0);
FlowGraph BuildFlowGraph(const D::LiveFrameEvidence&);
const D::LiveEvent* FlowEvent(const FlowGraph&,const D::LiveFlowSnapshot&,D::LiveId);
std::vector<const D::LiveGpuSpan*> GpuSamples(const D::LiveSourceSnapshot&);
struct FlowBundle { int from=0,to=0; uint64_t links=0; };
std::vector<FlowBundle> GroupFlowLinks(const FlowGraph&);
struct FlowObservations
{
    std::array<uint64_t,D::LiveStageCount> stageEvents{};
    std::array<uint64_t,size_t(D::LiveCommandKind::Present)+1> commandKinds{};
    uint64_t multiInstanceDraws=0;
    std::array<uint64_t,size_t(D::LiveOutcome::Unknown)+1> presentOutcomes{};
    std::vector<FlowBundle> links;
};
FlowObservations ObserveFlow(const FlowGraph&,const D::LiveSourceSnapshot&);
FlowObservations ObserveFlow(const FlowGraph&,const D::LiveFrameEvidence&);
struct FlowRouteStep { int from=0,to=0; uint64_t links=0; };
struct FlowRoute
{
    bool recording=false;
    std::vector<D::LiveFlowLink> links;
    std::vector<FlowRouteStep> steps;
};
std::vector<FlowRoute> BuildFlowRoutes(const FlowGraph&,const D::LiveSourceSnapshot&);
std::vector<FlowRoute> BuildFlowRoutes(const FlowGraph&,const D::LiveFrameEvidence&);
}
