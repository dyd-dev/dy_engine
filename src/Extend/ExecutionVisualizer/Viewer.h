#pragma once
#include "Process.h"
#include "ViewerState.h"
#include "FlowGraph.h"
#include "LaunchOptions.h"
#include "FlowPlayback.h"
#include <array>
#include <future>
#include <map>
#include <filesystem>
#include <unordered_set>

namespace viewer
{
void Style();
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
    enum class Layout { Default,Columns,Tabs };
    Layout requestedLayout=Layout::Default;
    float optionalWindowY=0;
    bool resetLayout = false, onlyRelated = false;
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
    void Targets();
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
    enum Panel { OverviewPanel,FlowPanel,TargetsPanel,ControlsPanel,EventsPanel,ObjectsPanel,GpuPanel,HistoryPanel,RequestsPanel,DetailsPanel,PanelCount };
    std::array<bool,PanelCount> panels{true,true,false,false,false,false,false,false,false,false};
    FlowPlayback playback;
    StatisticsView statistics;
    PresentationLog presentationLog;
    std::optional<ComparisonPresentation> comparisonDrawn;
    bool statisticsDrawn=false,flowDrawn=false;
    uint64_t receivedSnapshots=0;
};
}
