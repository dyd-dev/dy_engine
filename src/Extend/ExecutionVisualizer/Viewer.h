#pragma once
#include "Process.h"
#include "ViewerState.h"
#include "FlowGraph.h"
#include "LaunchOptions.h"
#include "FlowPlayback.h"
#include "AutoSession.h"
#include "EmbeddedTracy.h"
#include "TracyMetrics.h"
#include <array>
#include <future>
#include <map>
#include <filesystem>
#include <unordered_set>

namespace viewer
{
void Style();
float TableHeight(size_t rows,size_t visibleRows=6);
class Viewer
{
public:
    TargetProcess process;
    LaunchOptions launch;
    ~Viewer();
    void SetOwnerWindow(void* value) { ownerWindow=value; }
    void SetLaunchExecutable(const std::string&);
    std::string message;
    ViewerState state;
    void Launch();
    void InitializeAutomatic(bool start=true,std::optional<bool> tracyOverride={});
    void InitializeEmbedded(dyf::Gui&,dyf::RHI::IDevice&);
    void ShowTracy(bool visible,bool persist=true);
    void SetTracyPort(uint16_t port) {manualTracyPort=port;}
    bool RequestClose();
    bool CloseCancelled() const {return embedded&&embedded->CloseCancelled();}
    bool SaveTracy(const std::filesystem::path&);
    bool OpenTracy(const std::filesystem::path&);
    std::optional<int64_t> TracyMemoryLimit() const;
    void StartAutomatic();
    void Tick();
    void Draw();
    void Presented(uint64_t nowNs);
    bool WriteReport(const std::filesystem::path&, uint64_t frames) const;
    void AttachPid(uint32_t value) { wantedPid = value; }
private:
    D::LiveClient client;
    std::vector<D::LiveTarget> targets;
    std::optional<D::LiveTarget> reconnect;
    std::future<std::vector<D::LiveTarget>> discovery;
    uint64_t nextDiscoveryNs = 0;
    uint32_t wantedPid = 0;
    int selectedStage = 3, historyMetric = 0;
    bool resetLayout = false, onlyRelated = false;
    bool revealDetails=false,revealEvents=false,revealInventory=false;
    struct Draft { D::ControlValue value; bool dirty = false; D::LiveId pending = 0; };
    std::map<D::LiveId, Draft> drafts;
    std::vector<size_t> visibleEvents;
    uint64_t indexedSequence = UINT64_MAX;
    D::LiveId indexedSelection = 0;
    int indexedStage = -1;
    void Connect(const D::LiveTarget&);
    D::LiveId Send(const D::ControlDesc&, const D::ControlValue&);
    void Dock();
    void Overview();
    void ObservationStatistics();
    void ObservationFlow();
    void Targets();
    void ExternalTools();
    TargetProcess renderDocProcess;
    std::string renderDocExecutable,toolIssue;
    std::unique_ptr<EmbeddedTracy> embedded;
    TracyMetrics tracyMetrics;
    bool tracyVisible=false,tracyCollectRequested=false,tracyConnectPending=false,tracyReplaceWaiting=false;
    uint16_t manualTracyPort=8086;
    uint32_t embeddedPid=0;
    uint64_t embeddedCreation=0;
    D::LiveSessionId embeddedSession;
    std::string tracyIssue;
    void TickTracy();
    struct DisplayedMetric {MetricChoice choice;D::LiveId request=0;uint64_t presentedNs=0;D::LiveMetricRange range;};
    std::map<std::string,DisplayedMetric> drawnMetrics,presentedMetrics;
    void RecordMetric(const std::string& slot,const MetricChoice& value,D::LiveId request=0,D::LiveMetricRange range={}) {drawnMetrics[slot]={value,request,0,range};}
    D::LiveId toolRequest=0;
    AutoSession automatic{CurrentExecutable().parent_path()/L"execution-visualizer.session",CurrentExecutable()};
    bool automaticEnabled=true;
    uint32_t automaticAttachPid=0;
    D::LiveId automaticReplayRequest=0;
    void TickAutomatic(uint64_t now);
    void LaunchDialog();
    void StartExecutableScan();
    void PollExecutableScan();
    std::string executableFolder,manualExecutable,scanQuery,launchIssue;
    ExecutableScan executableCatalog;
    std::shared_ptr<std::atomic_bool> cancelExecutableScan;
    std::future<ExecutableScan> executableScan;
    bool openLaunchDialog=false;
    void* ownerWindow=nullptr;
    void Flow();
    void Controls();
    void ControlComparison();
    void ComparisonFlows(ComparisonPresentation&);
    D::LiveId selectedControl=0;
    int stageComparisonMetric=int(StageMetric::Operations),counterComparisonPhase=0;
    void Details();
    void EventSummary(const D::LiveEvent&,const char* caption);
    void History();
    void Graph();
    void Requests();
    void SelectTrace(SelectionKind,D::LiveId);
    const char* UpdateStatus() const;
    void Events();
    void Inventory();
    void Gpu();
    FlowGraph graph;
    std::vector<FlowRoute> flowRoutes;
    size_t selectedRoute=0;
    FlowRouteLayout routeLayout;
    size_t layoutRoute=SIZE_MAX;
    D::LiveId graphFocus=0;
    uint64_t graphSequence=UINT64_MAX;
    bool expandGraph=false,showReferenceRoutes=false;
    ComparisonPlayback comparisonPlayback;
    struct ComparisonSide
    {
        FlowGraph graph;
        FlowObservations observed;
        std::vector<FlowRoute> routes;
        FlowRouteLayout layout;
        size_t selected=0,layoutSelected=SIZE_MAX;
        bool valid=false,complete=false;
    };
    std::array<ComparisonSide,2> comparisonSides;
    uint64_t comparisonGraphGeneration=UINT64_MAX;
    bool comparisonReferences=false;
    FlowPlayback playback;
    StatisticsView statistics;
    PresentationLog presentationLog;
    std::optional<ComparisonPresentation> comparisonDrawn;
    std::optional<StatisticsObservation> observationStatisticsDrawn;
    std::optional<FlowObservationPresentation> observationFlowDrawn,observedFrame;
    uint64_t observedFrameGeneration=UINT64_MAX;
    bool statisticsDrawn=false,flowDrawn=false;
    uint64_t receivedSnapshots=0;
};
}
