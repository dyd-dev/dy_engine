#include "LiveInternal.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <type_traits>

namespace dyf::Diagnostics::Detail
{
namespace
{
#define FIELDS(type, ...) template<class A> void Fields(A& a,type& v) { a(__VA_ARGS__); }
FIELDS(LiveSessionId,v.high,v.low)
FIELDS(LiveFrameKey,v.device,v.source,v.frame)
FIELDS(LiveCommandRef,v.commandList,v.generation,v.index)
FIELDS(LiveContext,v.frame,v.appliedRevision,v.owner,v.object,v.batch)
FIELDS(LiveSample,v.state,v.milliseconds)
FIELDS(LiveObjectGauge,v.state,v.count)
FIELDS(LiveStageMetrics,v.elements,v.bytes,v.operations,v.cpu,v.gpu)
template<class A,class T>void Fields(A& a,LiveCounters<T>& v){a(v.draw,v.drawIndexed,v.dispatch,v.dispatchMesh,v.instances,v.barriers,v.bindings,v.submissions,v.waits,v.objectsCreated,v.objectsDestroyed,v.batches,v.elements,v.dirtyRanges,v.uploadCalls,v.uploadBytes,v.fullUploads,v.partialUploads,v.uploadPageAllocations,v.uploadPageReuses,v.uploadPageAllocationBytes);}
FIELDS(LiveFrameSummary,v.key,v.appliedRevision,v.beginNs,v.endNs,v.sampleAgeNs,v.frame,v.cpuRender,v.gpuMain,v.gpuShadow,v.gpuMeasured,v.objects,v.gpuCoverageComplete,v.incomplete,v.stages,v.recorded,v.submitted,v.completed,v.outcome)
FIELDS(LiveMetricAggregate,v.state,v.average,v.maximum,v.ready,v.pending,v.unsupported,v.unavailable,v.lastReadyFrame)
FIELDS(LiveStageAggregate,v.totalElements,v.totalBytes,v.totalOperations,v.meanElements,v.meanBytes,v.meanOperations,v.cpu,v.gpu)
FIELDS(LiveAggregate,v.device,v.source,v.appliedRevision,v.beginNs,v.endNs,v.frameCount,v.fps,v.mixedRevision,v.frame,v.cpuRender,v.gpuMain,v.gpuShadow,v.gpuMeasured,v.objects,v.stages,v.recordedTotal,v.submittedTotal,v.completedTotal,v.recordedMean,v.submittedMean,v.completedMean)
FIELDS(LiveMetricRange,v.state,v.low,v.high,v.windows)
FIELDS(LiveRunRanges,v.fps,v.frame,v.cpuRender,v.gpuMeasured,v.drawCalls,v.instances,v.batches,v.uploadBytes,v.objects)
FIELDS(LiveObjectDesc,v.kind,v.device,v.owner,v.elements,v.bytes,v.label)
FIELDS(LiveObjectInfo,v.id,v.desc)
FIELDS(LiveResourceCount,v.kind,v.live,v.created,v.destroyed,v.liveBytes)
FIELDS(LiveUploadMemoryStatistics,v.allocationCount,v.reuseCount,v.allocationBytes,v.requestCount,v.requestedBytes,v.cachedBytes,v.inUseBytes,v.peakInUseBytes)
FIELDS(LiveClockInfo,v.cpu,v.frequency,v.originTicks,v.cpuGpuCorrelated)
FIELDS(LiveDeviceSnapshot,v.device,v.backend,v.queueCount,v.observedNs,v.upload,v.resources)
FIELDS(LiveEvent,v.id,v.context,v.resource,v.submission,v.command,v.kind,v.commandKind,v.stage,v.outcome,v.reason,v.beginNs,v.endNs,v.elements,v.bytes,v.operations,v.first,v.stride,v.fullUpload,v.arguments,v.label)
FIELDS(LiveFlowLink,v.from,v.to)
FIELDS(LiveGpuEndpoint,v.command,v.submission)
FIELDS(LiveGpuSpan,v.id,v.frame,v.appliedRevision,v.owner,v.scope,v.aggregation,v.begin,v.end,v.queueIndex,v.outcome,v.duration,v.gpuClockDomain,v.hasQueueInterval,v.beginGpuNs,v.endGpuNs,v.hasCpuInterval,v.beginCpuNs,v.endCpuNs,v.cpuCalibrationErrorKnown,v.cpuCalibrationErrorNs)
FIELDS(LiveFlowSnapshot,v.key,v.revision,v.omittedEvents,v.omittedLinks,v.events,v.links,v.gpuSpans)
FIELDS(LiveLoss,v.events,v.links,v.frames,v.inventory,v.gpuSpans,v.transferTruncated)
FIELDS(LiveFrameEvidence,v.available,v.budgetLimited,v.summary,v.flow,v.windowBeginNs,v.windowEndNs,v.windowRevision,v.objects,v.loss)
FIELDS(LiveComparisonEvidence,v.request,v.before,v.after)
FIELDS(LiveSourceSnapshot,v.device,v.source,v.latest,v.recent,v.runRanges,v.recentSeconds,v.history,v.longFrames,v.flow,v.recentGpuSpans,v.comparisonEvidence)
FIELDS(ControlChoice,v.value,v.label)
FIELDS(ControlDesc,v.id,v.device,v.source,v.owner,v.key,v.label,v.kind,v.application,v.appliedValue,v.writable,v.minimum,v.maximum,v.choices,v.unavailableReason,v.category,v.description,v.onPath,v.offPath,v.conditions,v.tradeoffs)
FIELDS(ControlRequest,v.request,v.control,v.value)
FIELDS(ControlAck,v.request,v.accepted,v.message)
FIELDS(ControlStatus,v.request,v.control,v.device,v.source,v.state,v.requested,v.actual,v.appliedFrame,v.appliedRevision,v.requestedNs,v.appliedNs,v.baseline,v.after,v.message,v.previousActual)
FIELDS(LiveCollectorStatistics,v.retainedFrames,v.retainedDetailBytes,v.commandGenerations,v.batches)
FIELDS(LiveSnapshot,v.session,v.sequence,v.producerTimeNs,v.clock,v.objects,v.devices,v.sources,v.controls,v.controlStatus,v.loss,v.collector)
#undef FIELDS
template<class T>constexpr unsigned EnumLimit();
#define LIMIT(type,last) template<>constexpr unsigned EnumLimit<type>(){return unsigned(type::last);}
LIMIT(LiveValueState,Ready) LIMIT(LiveStage,Output) LIMIT(LiveOutcome,Unknown)
LIMIT(LiveObjectKind,Batch) LIMIT(LiveCpuClock,SteadyClock) LIMIT(LiveCommandKind,Present)
LIMIT(LiveEventKind,UploadMemory) LIMIT(LiveSplitReason,EndOfInput) LIMIT(LiveGpuScope,LegacyRendererMainWindow)
LIMIT(LiveGpuAggregation,LegacyDetail) LIMIT(ControlKind,Enumeration)
LIMIT(ControlApplication,RebuildRequired) LIMIT(ControlState,Cancelled)
LIMIT(ControlCategory,Diagnostic)
#undef LIMIT
struct Writer
{
    std::vector<std::uint8_t> data;
    std::size_t budget=LiveTransferLimit;
    void Allocate(std::size_t bytes){if(bytes>budget)throw std::length_error("Live decoded allocation budget exceeded");budget-=bytes;}
    template<class... T>void operator()(const T&... values){(Value(values),...);}
    void Byte(std::uint8_t value){if(data.size()==LiveTransferLimit-sizeof(PacketHeader))throw std::length_error("Live payload exceeds 64 MiB");data.push_back(value);}
    template<class T>void Value(const T& v){
        if constexpr(std::is_same_v<T,bool>)Byte(v?1:0);
        else if constexpr(std::is_enum_v<T>)Value(static_cast<std::underlying_type_t<T>>(v));
        else if constexpr(std::is_integral_v<T>){using U=std::make_unsigned_t<T>;auto bits=static_cast<U>(v);for(std::size_t i=0;i<sizeof(T);++i)Byte(std::uint8_t(bits>>(i*8)));}
        else if constexpr(std::is_floating_point_v<T>){static_assert(sizeof(T)==8);std::uint64_t bits;std::memcpy(&bits,&v,8);Value(bits);}
        else Fields(*this,const_cast<T&>(v));
    }
    void Value(const std::string& v){Allocate(v.size());Value(std::uint32_t(v.size()));for(unsigned char c:v)Byte(c);}
    template<class T>void Value(const std::vector<T>& v){if(v.size()>1000000||v.size()>LiveTransferLimit/sizeof(T))throw std::length_error("Live vector too large");Allocate(v.size()*sizeof(T));Value(std::uint32_t(v.size()));for(const auto& x:v)Value(x);}
    template<class T,std::size_t N>void Value(const std::array<T,N>& v){for(const auto& x:v)Value(x);}
    template<std::size_t N>void Value(const char(&v)[N]){for(char x:v)Byte(std::uint8_t(x));}
    void Value(const ControlValue& v){Value(std::uint8_t(v.index()));std::visit([&](const auto& x){Value(x);},v);}
};
struct Reader
{
    const std::vector<std::uint8_t>& data;
    std::size_t position=0,budget=LiveTransferLimit;
    template<class... T>void operator()(T&... values){(Value(values),...);}
    std::uint8_t Byte(){if(position>=data.size())throw std::runtime_error("Truncated live payload");return data[position++];}
    void Allocate(std::size_t n){if(n>budget)throw std::runtime_error("Live allocation budget exceeded");budget-=n;}
    template<class T>void Value(T& v){
        if constexpr(std::is_same_v<T,bool>){const auto x=Byte();if(x>1)throw std::runtime_error("Invalid live boolean");v=x!=0;}
        else if constexpr(std::is_enum_v<T>){std::underlying_type_t<T> x{};Value(x);if(unsigned(x)>EnumLimit<T>())throw std::runtime_error("Invalid live enum");v=static_cast<T>(x);}
        else if constexpr(std::is_integral_v<T>){using U=std::make_unsigned_t<T>;U bits=0;for(std::size_t i=0;i<sizeof(T);++i)bits|=U(Byte())<<(i*8);v=static_cast<T>(bits);}
        else if constexpr(std::is_floating_point_v<T>){std::uint64_t bits;Value(bits);std::memcpy(&v,&bits,8);if(!std::isfinite(v))throw std::runtime_error("Nonfinite live value");}
        else Fields(*this,v);
    }
    void Value(std::string& v){std::uint32_t size;Value(size);if(size>data.size()-position)throw std::runtime_error("Invalid live string length");Allocate(size);v.assign(reinterpret_cast<const char*>(data.data()+position),size);position+=size;}
    template<class T>void Value(std::vector<T>& v){std::uint32_t size;Value(size);if(size>1000000||size>data.size()-position||size>LiveTransferLimit/sizeof(T))throw std::runtime_error("Invalid live vector length");Allocate(std::size_t(size)*sizeof(T));v.resize(size);for(auto& x:v)Value(x);}
    template<class T,std::size_t N>void Value(std::array<T,N>& v){for(auto& x:v)Value(x);}
    template<std::size_t N>void Value(char(&v)[N]){for(char& x:v)x=char(Byte());v[N-1]=0;}
    void Value(ControlValue& v){const auto tag=Byte();if(tag==0){bool x;Value(x);v=x;}else if(tag==1){std::int64_t x;Value(x);v=x;}else if(tag==2){double x;Value(x);v=x;}else throw std::runtime_error("Invalid live control type");}
};
template<class T>bool Decode(const std::vector<std::uint8_t>& data,T& out,std::string& error)
{
    try{if(data.size()>LiveTransferLimit)throw std::runtime_error("Live payload exceeds 64 MiB");Reader reader{data};T value;reader.Value(value);if(reader.position!=data.size())throw std::runtime_error("Trailing live bytes");out=std::move(value);return true;}
    catch(const std::exception& e){error=e.what();return false;}
}
}
std::vector<std::uint8_t> EncodeSnapshot(const LiveSnapshot& s){Writer writer;writer.Value(s);return std::move(writer.data);}
std::vector<std::uint8_t> EncodeBoundedSnapshot(LiveSnapshot& snapshot)
{
    for(unsigned step=0;step<10;++step){
        try{return EncodeSnapshot(snapshot);}catch(const std::length_error&){snapshot.loss.transferTruncated=true;}
        if(step==0)for(auto& source:snapshot.sources){snapshot.loss.events+=source.flow.events.size();snapshot.loss.links+=source.flow.links.size();source.flow.omittedEvents+=source.flow.events.size();source.flow.omittedLinks+=source.flow.links.size();source.flow.events.clear();source.flow.links.clear();for(auto* evidence:{&source.comparisonEvidence.before,&source.comparisonEvidence.after}){snapshot.loss.events+=evidence->flow.events.size();snapshot.loss.links+=evidence->flow.links.size();evidence->loss.events+=evidence->flow.events.size();evidence->loss.links+=evidence->flow.links.size();evidence->flow.omittedEvents+=evidence->flow.events.size();evidence->flow.omittedLinks+=evidence->flow.links.size();evidence->flow.events.clear();evidence->flow.links.clear();evidence->budgetLimited=true;evidence->loss.transferTruncated=true;}}
        else if(step==1)for(auto& source:snapshot.sources){snapshot.loss.gpuSpans+=source.flow.gpuSpans.size();source.flow.gpuSpans.clear();for(auto* evidence:{&source.comparisonEvidence.before,&source.comparisonEvidence.after}){snapshot.loss.gpuSpans+=evidence->flow.gpuSpans.size();evidence->loss.gpuSpans+=evidence->flow.gpuSpans.size();evidence->flow.gpuSpans.clear();evidence->budgetLimited=true;evidence->loss.transferTruncated=true;}}
        else if(step==2)for(auto& source:snapshot.sources){snapshot.loss.gpuSpans+=source.recentGpuSpans.size();source.recentGpuSpans.clear();}
        else if(step==3)for(auto& source:snapshot.sources){snapshot.loss.frames+=source.longFrames.size();source.longFrames.clear();}
        else if(step==4)for(auto& source:snapshot.sources){for(const auto& bucket:source.history)snapshot.loss.frames+=bucket.frameCount;source.history.clear();for(const auto& second:source.recentSeconds)snapshot.loss.frames+=second.frameCount;source.recentSeconds.clear();}
        else if(step==5){snapshot.loss.inventory+=snapshot.objects.size();snapshot.objects.clear();for(auto& source:snapshot.sources)for(auto* evidence:{&source.comparisonEvidence.before,&source.comparisonEvidence.after}){snapshot.loss.inventory+=evidence->objects.size();evidence->loss.inventory+=evidence->objects.size();evidence->objects.clear();evidence->budgetLimited=true;evidence->loss.transferTruncated=true;}}
        else if(step==6)for(auto& control:snapshot.controls){if(control.choices.size()>256){snapshot.loss.inventory+=control.choices.size()-256;control.choices.resize(256);control.writable=false;control.unavailableReason="Control metadata omitted by transfer budget";}if(control.key.size()>128)control.key.resize(128);if(control.label.size()>256)control.label.resize(256);for(auto* text:{&control.unavailableReason,&control.description,&control.onPath,&control.offPath,&control.conditions,&control.tradeoffs})if(text->size()>1024)text->resize(1024);for(auto& choice:control.choices)if(choice.label.size()>128)choice.label.resize(128);}
        else if(step==7){if(snapshot.sources.size()>1024){snapshot.loss.frames+=snapshot.sources.size()-1024;std::sort(snapshot.sources.begin(),snapshot.sources.end(),[](const auto& a,const auto& b){return a.latest.endNs>b.latest.endNs;});snapshot.sources.resize(1024);}if(snapshot.devices.size()>1024){snapshot.loss.inventory+=snapshot.devices.size()-1024;snapshot.devices.resize(1024);}if(snapshot.controls.size()>1024){snapshot.loss.inventory+=snapshot.controls.size()-1024;snapshot.controls.resize(1024);}}
        else if(step==8){for(auto& device:snapshot.devices){if(device.backend.size()>128)device.backend.resize(128);if(device.resources.size()>32)device.resources.resize(32);}if(snapshot.controlStatus.size()>256)snapshot.controlStatus.erase(snapshot.controlStatus.begin(),snapshot.controlStatus.end()-256);for(auto& status:snapshot.controlStatus)if(status.message.size()>1024)status.message.resize(1024);}
        else{snapshot.loss.frames+=snapshot.sources.size();snapshot.loss.inventory+=snapshot.devices.size()+snapshot.controls.size();snapshot.sources.clear();snapshot.devices.clear();snapshot.controls.clear();snapshot.controlStatus.clear();}
    }
    return EncodeSnapshot(snapshot);
}
bool DecodeSnapshot(const std::vector<std::uint8_t>& data,LiveSnapshot& s,std::string& error)
{
    LiveSnapshot value;if(!Decode(data,value,error))return false;
    for(const auto& source:value.sources){if(source.latest.key.device!=source.device||source.latest.key.source!=source.source){error="Mismatched live source";return false;}
        const auto& ranges=source.runRanges;
        for(const auto* range:{&ranges.fps,&ranges.frame,&ranges.cpuRender,&ranges.gpuMeasured,&ranges.drawCalls,&ranges.instances,&ranges.batches,&ranges.uploadBytes,&ranges.objects})
            if(range->low<0||range->high<range->low||(range->state==LiveValueState::Ready?range->windows==0:range->windows!=0||range->low!=0||range->high!=0)){error="Invalid live run range";return false;}
        if(source.recentSeconds.size()>10){error="Too many completed live seconds";return false;}
        const LiveAggregate* previous=nullptr;
        for(const auto& second:source.recentSeconds){if(second.device!=source.device||second.source!=source.source||second.endNs<second.beginNs||second.endNs-second.beginNs!=LiveAverageWindowNs||second.endNs>value.producerTimeNs||(previous&&previous->endNs>second.beginNs)||(second.mixedRevision&&second.appliedRevision!=0)){error="Invalid completed live second";return false;}previous=&second;}
        if(source.recent.mixedRevision&&source.recent.appliedRevision!=0){error="Invalid mixed live revision";return false;}
        for(const auto* spans:{&source.flow.gpuSpans,&source.recentGpuSpans})for(const auto& span:*spans){if(span.frame.device!=source.device||span.frame.source!=source.source||(span.hasQueueInterval&&(!span.gpuClockDomain||span.endGpuNs<span.beginGpuNs))||(span.hasCpuInterval&&(value.clock.cpu!=LiveCpuClock::QueryPerformanceCounter||!value.clock.frequency||span.endCpuNs<span.beginCpuNs))){error="Invalid live GPU interval/domain";return false;}}
        const auto& comparison=source.comparisonEvidence;
        for(const auto* evidence:{&comparison.before,&comparison.after})if(evidence->available){
            const auto status=std::find_if(value.controlStatus.begin(),value.controlStatus.end(),[&](const auto& request){return request.request==comparison.request&&request.source==source.source&&request.device==source.device&&request.state==ControlState::Applied;});
            if(status==value.controlStatus.end()){error="Missing live comparison request";return false;}
            const auto& aggregate=evidence==&comparison.before?status->baseline:status->after;const auto& summary=evidence->summary;
            if(!aggregate.frameCount||aggregate.mixedRevision||summary.key.device!=source.device||summary.key.source!=source.source||!summary.key.frame||summary.endNs<summary.beginNs||summary.endNs<evidence->windowBeginNs||summary.endNs>=evidence->windowEndNs||evidence->windowEndNs-evidence->windowBeginNs!=LiveAverageWindowNs||evidence->windowEndNs>value.producerTimeNs||evidence->windowRevision!=summary.appliedRevision||evidence->windowRevision!=aggregate.appliedRevision||evidence->windowBeginNs!=aggregate.beginNs||evidence->windowEndNs!=aggregate.endNs||evidence->flow.key!=summary.key||evidence->objects.size()>128||evidence->flow.gpuSpans.size()>128){error="Invalid live comparison evidence";return false;}
            for(const auto& event:evidence->flow.events)if(event.context.frame!=summary.key||event.context.appliedRevision!=summary.appliedRevision){error="Mismatched live evidence event/revision";return false;}
            for(const auto& span:evidence->flow.gpuSpans)if(span.frame!=summary.key||span.appliedRevision!=summary.appliedRevision||(span.hasQueueInterval&&(!span.gpuClockDomain||span.endGpuNs<span.beginGpuNs))||(span.hasCpuInterval&&(value.clock.cpu!=LiveCpuClock::QueryPerformanceCounter||!value.clock.frequency||span.endCpuNs<span.beginCpuNs))){error="Invalid live evidence GPU interval/domain/revision";return false;}
        }
        for(const auto* flow:{&source.flow,&comparison.before.flow,&comparison.after.flow})for(const auto& event:flow->events)if(event.kind==LiveEventKind::UploadMemory&&(event.operations!=1||!event.bytes||event.arguments[0]<0||event.arguments[0]>1)){error="Invalid native upload memory event";return false;}
    }
    s=std::move(value);return true;
}
std::vector<std::uint8_t> EncodeControl(const ControlRequest& r){Writer writer;writer.Value(r);return std::move(writer.data);}
bool DecodeControl(const std::vector<std::uint8_t>& data,ControlRequest& r,std::string& error){return Decode(data,r,error)&&r.request&&r.control;}
std::vector<std::uint8_t> EncodeAck(const ControlAck& r){Writer writer;writer.Value(r);return std::move(writer.data);}
bool DecodeAck(const std::vector<std::uint8_t>& data,ControlAck& r,std::string& error){return Decode(data,r,error)&&r.request;}
bool ValidateHeader(const PacketHeader& h) noexcept{return h.magic==0x564c5944&&h.version==LiveProtocolVersion&&(h.kind>=1&&h.kind<=3)&&h.size>0&&h.size<=LiveTransferLimit-sizeof(PacketHeader);}
}
