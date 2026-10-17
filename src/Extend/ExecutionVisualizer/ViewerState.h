#pragma once
#include "dyf/Diagnostics/LiveProfiling.h"
#include <optional>

namespace viewer
{
namespace D = dyf::Diagnostics;
enum class SelectionKind { None, Object, Event, Gpu };
struct Selection
{
    SelectionKind kind = SelectionKind::None;
    D::LiveId id = 0;
    D::LiveFrameKey source;
    std::shared_ptr<const D::LiveSnapshot> snapshot;
    uint64_t receivedNs=0;
};

struct ViewerState
{
    std::shared_ptr<const D::LiveSnapshot> snapshot;
    uint64_t receivedNs = 0;
    D::LiveId device = 0, source = 0;
    Selection selection;
    std::vector<D::ControlStatus> requests;
    uint64_t omittedCompletedRequests = 0;

    bool Accept(std::shared_ptr<const D::LiveSnapshot>, uint64_t nowNs);
    void Clear();
    void SelectSource(D::LiveId deviceId, D::LiveId sourceId);
    const D::LiveSourceSnapshot* Source() const;
    const D::LiveDeviceSnapshot* Device() const;
    const D::ControlStatus* Comparison() const;
    bool ExactClock(const D::LiveClockInfo& local) const;
    uint64_t AgeNs(uint64_t nowNs, const D::LiveClockInfo& local={}) const;
    void Queued(D::LiveId request, const D::ControlDesc&, const D::ControlValue&, uint64_t nowNs);
    void Select(SelectionKind, D::LiveId);
    const D::LiveEvent* SelectedEvent() const;
    const D::LiveObjectInfo* SelectedObject() const;
    const D::LiveGpuSpan* SelectedGpu() const;
    bool Related(const D::LiveEvent&) const;
};

bool ValidControlValue(const D::ControlDesc&, const D::ControlValue&);
bool CompleteComparisonWindow(const D::LiveAggregate&,D::LiveId device,D::LiveId source,uint64_t producerNs,D::LiveId revision=0);
bool ComparisonAfterReady(const ViewerState&,const D::ControlStatus&,uint64_t producerNs);
enum class ComparisonMetric { Frames,Fps,Frame,Cpu,Gpu,Draws,Instances,Dispatch,Upload };
std::optional<double> ComparisonNumber(const D::LiveAggregate&,ComparisonMetric);
std::optional<double> WindowRate(const D::LiveAggregate&);
std::string DisplayAmount(double value,int decimals=1);
struct EventText { std::string name,label,explanation,arguments,value="--",unit="미수집"; };
EventText DescribeEvent(const D::LiveEvent&);
enum class StageMetric { Cpu,Gpu,Operations,Elements,Bytes,Count };
std::optional<double> StageNumber(const D::LiveAggregate&,D::LiveStage,StageMetric);
enum class CounterPhase { Recorded,Submitted,Completed };
struct CounterField { const char* key; const char* label; double D::LiveCounters<double>::*member; bool recordedOnly=false; };
constexpr size_t ComparisonCounterCount=21;
const std::array<CounterField,ComparisonCounterCount>& ComparisonCounters();
std::optional<double> CounterNumber(const D::LiveAggregate&,CounterPhase,const CounterField&);
struct NumberChange { std::optional<double> before,after,delta,percent; };
NumberChange CompareNumbers(std::optional<double> before,std::optional<double> after);
NumberChange CompareObservedCounts(std::optional<double> before,std::optional<double> after,bool beforeComplete,bool afterComplete);
std::string ValueText(const D::ControlValue&);
const char* ValueState(D::LiveValueState);
const char* Outcome(D::LiveOutcome);
const char* ControlState(D::ControlState);
const char* Application(D::ControlApplication);
const char* ObjectKind(D::LiveObjectKind);
const char* CommandKind(D::LiveCommandKind);
const char* EventKind(D::LiveEventKind);
const char* SplitReason(D::LiveSplitReason);
const char* StageName(D::LiveStage);
const char* GpuScope(D::LiveGpuScope);
}
