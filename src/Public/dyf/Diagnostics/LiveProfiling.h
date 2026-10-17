#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace dyf::Diagnostics
{
using LiveId = std::uint64_t;
constexpr std::uint32_t LiveProtocolVersion = 3;
constexpr std::size_t LiveTransferLimit = 64u * 1024u * 1024u;
constexpr std::uint64_t LivePublishIntervalNs = 100000000;
constexpr std::uint64_t LiveAverageWindowNs = 1000000000;
constexpr std::uint64_t LiveHistoryWindowNs = 10000000000ull;

struct LiveSessionId { std::uint64_t high = 0, low = 0; };
inline bool operator==(LiveSessionId a, LiveSessionId b) noexcept { return a.high==b.high && a.low==b.low; }
inline bool operator!=(LiveSessionId a, LiveSessionId b) noexcept { return !(a==b); }
struct LiveFrameKey { LiveId device = 0, source = 0, frame = 0; };
inline bool operator==(LiveFrameKey a, LiveFrameKey b) noexcept { return a.device==b.device && a.source==b.source && a.frame==b.frame; }
inline bool operator!=(LiveFrameKey a, LiveFrameKey b) noexcept { return !(a==b); }
struct LiveCommandRef { LiveId commandList = 0; std::uint64_t generation = 0; std::uint32_t index = 0; };
struct LiveContext { LiveFrameKey frame; LiveId appliedRevision = 0, owner = 0, object = 0, batch = 0; };
enum class LiveValueState : std::uint8_t { Unavailable, Unsupported, Pending, Ready };
struct LiveSample { LiveValueState state = LiveValueState::Unavailable; double milliseconds = 0; };
struct LiveObjectGauge { LiveValueState state=LiveValueState::Unavailable; std::uint64_t count=0; };
enum class LiveStage : std::uint8_t { CpuPrepare, Data, Upload, Record, NativeTranslate, Submit, Gpu, Output, Count };
constexpr std::size_t LiveStageCount = static_cast<std::size_t>(LiveStage::Count);
struct LiveStageMetrics { std::uint64_t elements=0, bytes=0, operations=0; LiveSample cpu, gpu; };
template<class T> struct LiveCounters
{
    T draw=0,drawIndexed=0,dispatch=0,dispatchMesh=0,instances=0,barriers=0,bindings=0;
    T submissions=0,waits=0,objectsCreated=0,objectsDestroyed=0,batches=0,elements=0,dirtyRanges=0;
    T uploadCalls=0,uploadBytes=0,fullUploads=0,partialUploads=0;
    T uploadPageAllocations=0,uploadPageReuses=0,uploadPageAllocationBytes=0;
};
enum class LiveOutcome : std::uint8_t { Observed, Pending, Submitted, Completed, Skipped, Failed, Discarded, Unknown };
struct LiveFrameSummary
{
    LiveFrameKey key;
    LiveId appliedRevision=0;
    std::uint64_t beginNs=0,endNs=0,sampleAgeNs=0;
    LiveSample frame,cpuRender,gpuMain,gpuShadow,gpuMeasured;
    LiveObjectGauge objects;
    bool gpuCoverageComplete=false, incomplete=false;
    std::array<LiveStageMetrics,LiveStageCount> stages;
    LiveCounters<std::uint64_t> recorded,submitted,completed;
    LiveOutcome outcome=LiveOutcome::Observed;
};
struct LiveMetricAggregate
{
    LiveValueState state=LiveValueState::Unavailable;
    double average=0,maximum=0;
    std::uint64_t ready=0,pending=0,unsupported=0,unavailable=0;
    LiveFrameKey lastReadyFrame;
};
struct LiveStageAggregate
{
    std::uint64_t totalElements=0,totalBytes=0,totalOperations=0;
    double meanElements=0,meanBytes=0,meanOperations=0;
    LiveMetricAggregate cpu,gpu;
};
struct LiveAggregate
{
    LiveId device=0,source=0,appliedRevision=0;
    std::uint64_t beginNs=0,endNs=0,frameCount=0;
    double fps=0;
    bool mixedRevision=false;
    LiveMetricAggregate frame,cpuRender,gpuMain,gpuShadow,gpuMeasured;
    LiveMetricAggregate objects;
    std::array<LiveStageAggregate,LiveStageCount> stages;
    LiveCounters<std::uint64_t> recordedTotal,submittedTotal,completedTotal;
    LiveCounters<double> recordedMean,submittedMean,completedMean;
};
struct LiveMetricRange
{
    LiveValueState state=LiveValueState::Unavailable;
    double low=0,high=0;
    std::uint64_t windows=0;
};
struct LiveRunRanges
{
    LiveMetricRange fps,frame,cpuRender,gpuMeasured,drawCalls,instances,batches,uploadBytes,objects;
};
enum class LiveObjectKind : std::uint8_t { Device,Renderer,Scene,Entity,Canvas,Model,Mesh,Material,Buffer,Texture,Pipeline,Shader,ResourceSet,Query,CommandList,Batch,Count };
struct LiveObjectDesc
{
    LiveObjectKind kind=LiveObjectKind::Entity;
    LiveId device=0,owner=0;
    std::uint64_t elements=0,bytes=0;
    std::string label;
};
struct LiveObjectInfo { LiveId id=0; LiveObjectDesc desc; };
struct LiveResourceCount { LiveObjectKind kind=LiveObjectKind::Buffer; std::uint64_t live=0,created=0,destroyed=0,liveBytes=0; };
struct LiveUploadMemoryStatistics
{
    std::uint64_t allocationCount=0,reuseCount=0,allocationBytes=0,requestCount=0,requestedBytes=0;
    std::uint64_t cachedBytes=0,inUseBytes=0,peakInUseBytes=0;
};
enum class LiveCpuClock : std::uint8_t { QueryPerformanceCounter,SteadyClock };
struct LiveClockInfo { LiveCpuClock cpu=LiveCpuClock::SteadyClock; std::uint64_t frequency=0,originTicks=0; bool cpuGpuCorrelated=false; };
struct LiveDeviceSnapshot
{
    LiveId device=0;
    std::string backend;
    std::uint32_t queueCount=1;
    std::uint64_t observedNs=0;
    LiveUploadMemoryStatistics upload;
    std::vector<LiveResourceCount> resources;
};
enum class LiveCommandKind : std::uint8_t { None,Draw,DrawIndexed,Dispatch,DispatchMesh,Barrier,Bind,Upload,Wait,Present };
enum class LiveEventKind : std::uint8_t { CpuWork,DataRange,Batch,Upload,Command,NativeTranslation,Submission,Output,Wait,UploadMemory };
enum class LiveSplitReason : std::uint8_t { None,InstancingDisabled,CustomGeometry,Mesh,Material,Texture,TexturePageCapacity,ShadowFlags,Culled,Viewport,Scissor,Noncontiguous,Capacity,DirtyRangeLimit,NewResource,SceneChanged,ElementCountChanged,EndOfInput };
// Fixed-size hot-path record: no caller storage, data pointers or native handles cross IPC.
struct LiveEvent
{
    LiveId id=0;
    LiveContext context;
    LiveId resource=0,submission=0;
    LiveCommandRef command;
    LiveEventKind kind=LiveEventKind::CpuWork;
    LiveCommandKind commandKind=LiveCommandKind::None;
    LiveStage stage=LiveStage::CpuPrepare;
    LiveOutcome outcome=LiveOutcome::Observed;
    LiveSplitReason reason=LiveSplitReason::None;
    std::uint64_t beginNs=0,endNs=0,elements=0,bytes=0,operations=0,first=0,stride=0;
    bool fullUpload=false;
    std::array<std::int64_t,5> arguments{};
    char label[64]{};
};
struct LiveFlowLink { LiveId from=0,to=0; };
enum class LiveGpuScope : std::uint8_t { CommandList,RendererMainForward,RendererShadow,ModelCompute,LegacyRendererMainWindow };
enum class LiveGpuAggregation : std::uint8_t { FrameCoverage,SemanticDetail,LegacyDetail };
struct LiveGpuEndpoint { LiveCommandRef command; LiveId submission=0; };
struct LiveGpuSpan
{
    LiveId id=0;
    LiveFrameKey frame;
    LiveId appliedRevision=0,owner=0;
    LiveGpuScope scope=LiveGpuScope::CommandList;
    LiveGpuAggregation aggregation=LiveGpuAggregation::SemanticDetail;
    LiveGpuEndpoint begin,end;
    std::uint32_t queueIndex=0;
    LiveOutcome outcome=LiveOutcome::Pending;
    LiveSample duration;
    LiveId gpuClockDomain=0;
    bool hasQueueInterval=false;
    std::uint64_t beginGpuNs=0,endGpuNs=0;
    bool hasCpuInterval=false;
    std::uint64_t beginCpuNs=0,endCpuNs=0;
    bool cpuCalibrationErrorKnown=false;
    std::uint64_t cpuCalibrationErrorNs=0;
};
struct LiveFlowSnapshot
{
    LiveFrameKey key;
    std::uint64_t revision=0,omittedEvents=0,omittedLinks=0;
    std::vector<LiveEvent> events;
    std::vector<LiveFlowLink> links;
    std::vector<LiveGpuSpan> gpuSpans;
};
struct LiveLoss { std::uint64_t events=0,links=0,frames=0,inventory=0,gpuSpans=0; bool transferTruncated=false; };
struct LiveFrameEvidence
{
    bool available=false,budgetLimited=false;
    LiveFrameSummary summary;
    LiveFlowSnapshot flow;
    std::uint64_t windowBeginNs=0,windowEndNs=0;
    LiveId windowRevision=0;
    std::vector<LiveObjectInfo> objects;
    LiveLoss loss;
};
struct LiveComparisonEvidence
{
    LiveId request=0;
    LiveFrameEvidence before,after;
};
struct LiveSourceSnapshot
{
    LiveId device=0,source=0;
    LiveFrameSummary latest;
    // Fixed [beginNs,endNs) seconds start at this source's first BeginLiveFrame.
    // recent is the last completed second (unavailable before the first completes);
    // every ended frame belongs to its endNs second, including option transitions.
    // A mixed second has appliedRevision=0 and mixedRevision=true; frame IDs stay original.
    LiveAggregate recent;
    // Extrema of completed one-second averages since this session/source began.
    // Pending or partially measured GPU seconds cannot contribute a GPU range.
    // Same-session reconnects and revisions retain ranges; a new session starts fresh.
    LiveRunRanges runRanges;
    // Completed fixed seconds within the last 10s, at most 10; empty/lost gaps are absent.
    // Use these original aggregates after attach/reconnect; do not re-average history.
    std::vector<LiveAggregate> recentSeconds;
    // Each bucket is computed from every contributing frame, split by revision.
    std::vector<LiveAggregate> history;
    std::vector<LiveFrameSummary> longFrames;
    LiveFlowSnapshot flow;
    std::vector<LiveGpuSpan> recentGpuSpans;
    // One latest request per source. Each side is an original representative frame,
    // never an averaged/synthetic frame; window fields identify its aggregate.
    LiveComparisonEvidence comparisonEvidence;
};
using ControlValue=std::variant<bool,std::int64_t,double>;
enum class ControlKind : std::uint8_t { Boolean,Integer,Real,Enumeration };
enum class ControlApplication : std::uint8_t { Immediate,ResourceRebuild,RestartRequired,RebuildRequired };
enum class ControlState : std::uint8_t { Requested,Validated,PendingApply,Applied,Rejected,Failed,Cancelled };
enum class ControlCategory : std::uint8_t { Unspecified,Optimization,OutputQuality,BuildOrStructural };
struct ControlChoice { std::int64_t value=0; std::string label; };
struct ControlDesc
{
    LiveId id=0,device=0,source=0,owner=0;
    std::string key,label;
    ControlKind kind=ControlKind::Boolean;
    ControlApplication application=ControlApplication::Immediate;
    ControlValue appliedValue=false;
    bool writable=true;
    double minimum=0,maximum=0;
    std::vector<ControlChoice> choices;
    std::string unavailableReason;
    // Optional producer-owned catalog metadata. Empty text preserves legacy callers.
    ControlCategory category=ControlCategory::Unspecified;
    std::string description,onPath,offPath,conditions,tradeoffs;
};
struct ControlRequest { LiveId request=0,control=0; ControlValue value=false; };
struct ControlStatus
{
    LiveId request=0,control=0,device=0,source=0;
    ControlState state=ControlState::Requested;
    ControlValue requested=false,actual=false;
    LiveFrameKey appliedFrame;
    LiveId appliedRevision=0;
    std::uint64_t requestedNs=0,appliedNs=0;
    // Completed source-aligned seconds: baseline pins the interval before application;
    // after follows the latest same-revision interval starting at/after appliedNs.
    // Mixed/empty/lost intervals have frameCount=0. Retained late results repair the same interval.
    // GPU comparisons also require ready>0 and no pending/unsupported/unavailable samples.
    LiveAggregate baseline,after;
    std::string message;
    ControlValue previousActual=false;
};
struct ControlApplyResult { ControlState state=ControlState::Failed; ControlValue actual=false; std::string message; std::optional<ControlValue> normalizedExpected; };
using ControlHandler=std::function<ControlApplyResult(const ControlRequest&,LiveFrameKey)>;
class ControlRegistration
{
public:
    ControlRegistration() noexcept=default;
    ~ControlRegistration();
    ControlRegistration(ControlRegistration&&) noexcept;
    ControlRegistration& operator=(ControlRegistration&&) noexcept;
    ControlRegistration(const ControlRegistration&)=delete;
    ControlRegistration& operator=(const ControlRegistration&)=delete;
    LiveId GetId() const noexcept { return id; }
private:
    friend ControlRegistration RegisterControl(ControlDesc,ControlHandler);
    explicit ControlRegistration(LiveId value) noexcept:id(value) {}
    LiveId id=0;
};
struct LiveCollectorStatistics { std::uint64_t retainedFrames=0,retainedDetailBytes=0,commandGenerations=0,batches=0; };
struct LiveSnapshot
{
    LiveSessionId session;
    std::uint64_t sequence=0,producerTimeNs=0;
    LiveClockInfo clock;
    std::vector<LiveObjectInfo> objects;
    std::vector<LiveDeviceSnapshot> devices;
    std::vector<LiveSourceSnapshot> sources;
    std::vector<ControlDesc> controls;
    std::vector<ControlStatus> controlStatus;
    LiveLoss loss;
    LiveCollectorStatistics collector;
};

std::uint64_t LiveNowNs() noexcept;
LiveClockInfo GetLiveClockInfo() noexcept;
bool SetLiveCollectorEnabled(bool) noexcept;
bool IsLiveCollectorEnabled() noexcept;
LiveId AllocateLiveId() noexcept;
LiveId RegisterLiveObject(const void*,const LiveObjectDesc&) noexcept;
LiveId FindLiveObject(const void*) noexcept;
void UpdateLiveObject(LiveId,const LiveObjectDesc&) noexcept;
void RebindLiveObject(LiveId,const void*) noexcept;
void RetireLiveObject(LiveId) noexcept;
LiveId GetLiveBatchId(LiveId owner,std::uint64_t discriminator) noexcept;
void PublishLiveDevice(const LiveDeviceSnapshot&) noexcept;
LiveContext GetLiveContext() noexcept;
void SetLiveContext(LiveContext) noexcept;
struct LiveFrameToken { LiveFrameKey key; bool ownsFrame=false; LiveContext previous; };
LiveFrameToken BeginLiveFrame(LiveId device,LiveId source) noexcept;
void EndLiveFrame(LiveFrameToken,LiveOutcome) noexcept;
// Deferred command recording keeps exact fixed summaries while detail is bounded.
struct LiveEventSummary
{
    LiveCounters<std::uint64_t> recorded;
    std::array<LiveStageMetrics,LiveStageCount> stages;
    std::uint64_t omittedEvents=0,omittedLinks=0;
};
void AccumulateLiveEventSummary(LiveEventSummary&,const LiveEvent&) noexcept;
bool ReserveLiveDetailBytes(std::size_t bytes) noexcept;
void ReleaseLiveDetailBytes(std::size_t bytes) noexcept;
void RecordLiveEventSummary(LiveCommandRef,const LiveEventSummary&) noexcept;
void RecordLiveEvent(const LiveEvent&,bool includeSummary=true) noexcept;
void RecordLiveLink(const LiveFlowLink&) noexcept;
void RecordLiveGpuSpan(const LiveGpuSpan&) noexcept;
// Native GPU coverage is complete only after all submitted list spans are registered.
void SetLiveGpuCoverage(LiveFrameKey,bool complete) noexcept;
void SetLiveObjectGauge(LiveFrameKey,LiveObjectGauge) noexcept;
std::shared_ptr<const LiveSnapshot> ReadLiveSnapshot();
// Renderer controls use this once per actual settings change, before GPU recording.
LiveId NotifyLiveSettingsApplied(LiveId source,LiveFrameKey,bool changed) noexcept;
ControlRegistration RegisterControl(ControlDesc,ControlHandler);
std::uint32_t PollControls(LiveId owner,LiveFrameKey) noexcept;
void PublishControlValue(LiveId,const ControlValue&) noexcept;
void PublishControlAvailability(LiveId,bool,std::string_view reason={}) noexcept;
void CompleteControl(LiveId request,LiveFrameKey,const ControlValue&) noexcept;
void FailControl(LiveId request,std::string_view) noexcept;
LiveId RequestLiveControl(LiveId,const ControlValue&);

class LiveContextScope
{
public:
    explicit LiveContextScope(LiveContext context) noexcept:previous(GetLiveContext()) { SetLiveContext(context); }
    ~LiveContextScope() { SetLiveContext(previous); }
    LiveContextScope(const LiveContextScope&)=delete;
    LiveContextScope& operator=(const LiveContextScope&)=delete;
private: LiveContext previous;
};
class LiveFrameScope
{
public:
    LiveFrameScope(LiveId device,LiveId source) noexcept:token(BeginLiveFrame(device,source)) {}
    ~LiveFrameScope() { EndLiveFrame(token,outcome); }
    LiveFrameKey GetKey() const noexcept { return token.key; }
    void SetOutcome(LiveOutcome value) noexcept { outcome=value; }
    LiveFrameScope(const LiveFrameScope&)=delete;
    LiveFrameScope& operator=(const LiveFrameScope&)=delete;
private: LiveFrameToken token; LiveOutcome outcome=LiveOutcome::Failed;
};
class LiveCpuScope
{
public:
    explicit LiveCpuScope(LiveStage stage,const char* label=nullptr) noexcept;
    ~LiveCpuScope();
    LiveCpuScope(const LiveCpuScope&)=delete;
    LiveCpuScope& operator=(const LiveCpuScope&)=delete;
private: LiveEvent event;
};
}
