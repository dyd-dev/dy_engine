#pragma once
#include "FlowGraph.h"
#include <ostream>

namespace viewer
{
class FlowPlayback
{
public:
    static constexpr uint64_t DurationNs=3000000000ull;
    std::shared_ptr<const D::LiveSnapshot> snapshot;
    D::LiveFrameKey frame;
    D::LiveId revision=0;
    uint64_t startedNs=0,generation=0;
    bool Update(std::shared_ptr<const D::LiveSnapshot>,D::LiveId device,D::LiveId source,uint64_t nowNs);
    const D::LiveSourceSnapshot* Source() const;
    double Phase(uint64_t nowNs) const;
    void Clear();
private:
    D::LiveSessionId session;
    D::LiveId selectedDevice=0,selectedSource=0;
};
bool ValidFrameEvidence(const D::LiveFrameEvidence&,D::LiveId device,D::LiveId source,uint64_t producerNs);
bool CompleteFrameEvidence(const D::LiveFrameEvidence&,D::LiveId device,D::LiveId source,uint64_t producerNs);
class ComparisonPlayback
{
public:
    std::optional<D::LiveComparisonEvidence> evidence;
    D::LiveSessionId session;
    uint64_t sequence=0,producerNs=0,startedNs=0,generation=0;
    bool Update(std::shared_ptr<const D::LiveSnapshot>,D::LiveId device,D::LiveId source,D::LiveId request,uint64_t nowNs);
    double Phase(uint64_t nowNs) const;
    void Clear();
private:
    D::LiveId selectedDevice=0,selectedSource=0,selectedRequest=0;
};
struct FlowPoint { float x=0,y=0; };
struct FlowRouteLayout
{
    static constexpr float Radius=32,Spacing=114,CenterY=78;
    std::vector<FlowPoint> nodes;
    std::vector<D::LiveId> ids;
    std::vector<std::vector<FlowPoint>> paths;
    float width=0,height=0;
};
FlowRouteLayout LayoutFlowRoute(const FlowRoute&);
struct FlowRoutePosition { size_t step=0; double phase=0; };
FlowRoutePosition LocateFlowRoute(const FlowRoute&,double phase);
class StatisticsView
{
public:
    static constexpr uint64_t IntervalNs=1000000000ull;
    ViewerState state;
    std::vector<D::LiveAggregate> history;
    uint64_t updatedNs=0;
    bool Update(std::shared_ptr<const D::LiveSnapshot>,D::LiveId device,D::LiveId source,uint64_t nowNs);
    void Clear();
private:
    uint64_t nextUpdateNs=0;
};
struct StatisticsPresentation
{
    D::LiveSessionId session;
    uint64_t sequence=0,presentedNs=0;
    D::LiveAggregate average;
    D::LiveRunRanges ranges;
};
struct FlowPresentation
{
    D::LiveSessionId session;
    D::LiveFrameKey frame;
    D::LiveId revision=0;
    uint64_t sequence=0,cycleStartedNs=0,presentedNs=0;
};
struct StatisticsObservation
{
    StatisticsPresentation values;
    std::array<std::array<unsigned char,ComparisonCounterCount>,3> counterVisibility{};
    std::array<std::array<unsigned char,size_t(StageMetric::Count)>,D::LiveStageCount> stageVisibility{};
};
std::optional<StatisticsObservation> ObserveStatistics(const StatisticsView&);
struct ObservedEventPresentation { D::LiveEvent event; unsigned char visibility=0; };
struct FlowObservationPresentation
{
    FlowPresentation values;
    FlowObservations observed;
    D::LiveLoss loss;
    uint64_t omittedEvents=0,omittedLinks=0;
    bool incomplete=false,headerVisible=false,multiInstanceVisible=false;
    std::array<unsigned char,size_t(D::LiveOutcome::Unknown)+1> presentOutcomeVisibility{};
    std::array<unsigned char,size_t(D::LiveCommandKind::Present)+1> commandVisibility{};
    std::vector<ObservedEventPresentation> events;
};
std::optional<FlowObservationPresentation> ObserveReplay(const FlowPlayback&);
struct ComparisonFramePresentation
{
    D::LiveFrameKey frame;
    D::LiveId revision=0,windowRevision=0;
    uint64_t windowBeginNs=0,windowEndNs=0;
    bool available=false,valid=false,complete=false,budgetLimited=false,incomplete=false,matchesAverageWindow=false,canvasVisible=false;
    uint64_t omittedEvents=0,omittedLinks=0,routeLinks=0,routeSteps=0;
    D::LiveLoss loss;
    FlowObservations observed;
    std::vector<D::LiveId> visibleOccurrences;
};
struct ComparisonFlowPresentation
{
    D::LiveSessionId session;
    D::LiveId request=0;
    uint64_t sequence=0,cycleStartedNs=0,presentedNs=0;
    std::array<ComparisonFramePresentation,2> frames;
    std::array<unsigned char,D::LiveStageCount> stageVisibility{};
    std::array<unsigned char,size_t(D::LiveCommandKind::Present)+1> commandVisibility{};
    std::array<unsigned char,(FlowReferenceColumn+1)*(FlowReferenceColumn+1)> linkVisibility{};
};
struct ComparisonPresentation
{
    struct RevisionChange { D::LiveId request=0,control=0; D::ControlValue before=false,after=false; };
    D::LiveSessionId session;
    uint64_t sequence=0,presentedNs=0;
    D::LiveId currentRevision=0;
    D::ControlStatus status;
    bool beforeReady=false,afterReady=false;
    std::array<unsigned char,9> metricVisibility{};
    std::array<std::array<unsigned char,size_t(StageMetric::Count)>,D::LiveStageCount> stageVisibility{};
    std::array<std::array<unsigned char,ComparisonCounterCount>,3> counterVisibility{};
    std::optional<ComparisonFlowPresentation> flow;
    std::vector<RevisionChange> revisionChanges;
};
struct PresentationLog
{
    std::optional<StatisticsPresentation> statistics;
    std::optional<FlowPresentation> flow;
    std::optional<ComparisonPresentation> comparison;
    std::optional<ComparisonPresentation> stageComparison,counterComparison;
    std::optional<ComparisonFlowPresentation> comparisonFlow;
    std::optional<StatisticsObservation> observationCounters,observationStages;
    std::optional<FlowObservationPresentation> observationFlow;
    bool statisticsVisible=false,flowVisible=false;
    bool comparisonVisible=false;
    bool comparisonStagesVisible=false,comparisonCountersVisible=false,comparisonFlowVisible=false;
    bool observationCountersVisible=false,observationStagesVisible=false,observationFlowVisible=false;
    uint64_t statisticsUpdates=0,flowCycles=0;
    void Presented(const StatisticsView&,const FlowPlayback&,bool statisticsDrawn,bool flowDrawn,uint64_t nowNs);
    void PresentedComparison(const ComparisonPresentation*,uint64_t nowNs);
    void PresentedObservations(const StatisticsObservation*,const FlowObservationPresentation*,uint64_t nowNs);
    void Write(std::ostream&) const;
};
std::vector<int> ActiveFlowColumns(const FlowGraph&);
bool FlowEdgeDetour(const std::vector<int>& columns,int from,int to);
std::vector<FlowPoint> RouteFlowEdge(FlowPoint from,FlowPoint to,float radius,float lane,bool detour=false);
FlowPoint PointOnFlowPath(const std::vector<FlowPoint>&,double phase);
}
