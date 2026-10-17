#include "ViewerState.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

namespace viewer
{
bool CompleteComparisonWindow(const D::LiveAggregate& a,D::LiveId device,D::LiveId source,uint64_t now,D::LiveId revision)
{
    return a.device==device && a.source==source && a.frameCount && a.appliedRevision && !a.mixedRevision
        && (!revision || revision==a.appliedRevision) && a.endNs>=a.beginNs && a.endNs-a.beginNs==D::LiveAverageWindowNs && a.endNs<=now;
}
bool ComparisonAfterReady(const ViewerState& state,const D::ControlStatus& status,uint64_t producerNs)
{
    return state.Source() && status.state==D::ControlState::Applied && status.request && status.control && status.appliedRevision
        && status.device==state.device && status.source==state.source && status.appliedFrame.device==state.device
        && status.appliedFrame.source==state.source && status.appliedFrame.frame
        && CompleteComparisonWindow(status.after,state.device,state.source,producerNs,status.appliedRevision) && status.after.beginNs>=status.appliedNs;
}
std::optional<double> WindowRate(const D::LiveAggregate& a)
{
    if(!a.frameCount || a.endNs<=a.beginNs) return {};
    return double(a.frameCount)*1e9/double(a.endNs-a.beginNs);
}
std::string DisplayAmount(double value,int decimals)
{
    if(!std::isfinite(value)) return "--";
    const char* suffix="";
    for(const auto* next:{"k","M","G"}) { if(std::abs(value)<1000) break; value/=1000; suffix=next; }
    char text[64];
    if(*suffix) std::snprintf(text,sizeof(text),"%.3g%s",value,suffix);
    else std::snprintf(text,sizeof(text),"%.*f",decimals,value);
    return text;
}
EventText DescribeEvent(const D::LiveEvent& e)
{
    EventText text; text.label.assign(e.label,std::find(std::begin(e.label),std::end(e.label),'\0'));
    text.name=e.commandKind!=D::LiveCommandKind::None ? CommandKind(e.commandKind) : text.label.empty() ? EventKind(e.kind) : text.label;
    text.explanation="프로그램이 기록한 작업 또는 상태입니다.";
    text.arguments="주요 인자 의미 미수집";
    const auto amount=[](int64_t value) { return DisplayAmount(double(value),0); };
    const auto& a=e.arguments;
    if(e.kind==D::LiveEventKind::NativeTranslation) { text.name="Native translate"; text.explanation="기록된 논리 명령을 백엔드 명령으로 변환합니다."; }
    else if(e.kind==D::LiveEventKind::Submission) { text.name="Submit"; text.explanation="기록된 명령 목록의 제출·완료 관측입니다."; }
    else if(e.kind==D::LiveEventKind::Wait) { text.name="Wait"; text.explanation="동기화 조건을 기다린 관측입니다."; }
    else if(e.kind==D::LiveEventKind::Output && e.commandKind==D::LiveCommandKind::Present)
    { text.name="Present"; text.explanation="화면 출력 API의 요청·결과 관측입니다."; }
    else if(e.kind==D::LiveEventKind::Upload)
    {
        text.name="Upload"; text.explanation="CPU 데이터를 GPU 자원으로 업로드합니다.";
        text.arguments="bytes="+DisplayAmount(double(e.bytes),0)+" B · "+(e.fullUpload ? "전체 업로드" : "부분 업로드");
        text.value=DisplayAmount(double(e.bytes),0); text.unit="B";
    }
    else if(e.kind==D::LiveEventKind::Command)
    {
        switch(e.commandKind)
        {
        case D::LiveCommandKind::Draw:
            text.explanation="정점과 인스턴스로 그리기를 기록합니다.";
            text.arguments="vertices="+amount(a[0])+" · instances="+amount(a[1])+" · firstVertex="+amount(a[2])+" · firstInstance="+amount(a[3]);
            text.value=amount(a[1]); text.unit="inst"; break;
        case D::LiveCommandKind::DrawIndexed:
            text.explanation="인덱스로 정점을 선택해 그리기를 기록합니다.";
            text.arguments="indices="+amount(a[0])+" · instances="+amount(a[1])+" · firstIndex="+amount(a[2])+" · vertexOffset="+amount(a[3])+" · firstInstance="+amount(a[4]);
            text.value=amount(a[1]); text.unit="inst"; break;
        case D::LiveCommandKind::Dispatch: case D::LiveCommandKind::DispatchMesh:
            text.explanation=e.commandKind==D::LiveCommandKind::Dispatch ? "컴퓨트 작업 그룹을 기록합니다." : "메시 셰이더 작업 그룹을 기록합니다.";
            text.arguments="groups xyz="+amount(a[0])+", "+amount(a[1])+", "+amount(a[2]); text.value=amount(a[0]); text.unit="x grp"; break;
        case D::LiveCommandKind::Barrier:
            text.explanation="자원 사용 상태·접근 동기화를 기록합니다."; break;
        case D::LiveCommandKind::Bind:
            text.explanation="파이프라인·버퍼·자원을 명령 목록에 연결합니다.";
            if(text.label=="Vertex buffer") text.arguments="binding="+amount(a[0])+" · offset="+amount(a[1])+" B";
            else if(text.label=="Index buffer") text.arguments="format code="+amount(a[0])+" · offset="+amount(a[1])+" B";
            else if(text.label=="Inline constants") text.arguments="offset="+amount(a[0])+" B · bytes="+amount(a[1])+" B";
            break;
        default: break;
        }
    }
    if(text.arguments=="주요 인자 의미 미수집" && (e.operations || e.elements || e.bytes))
        text.arguments="관측 작업="+DisplayAmount(double(e.operations),0)+" · 요소="+DisplayAmount(double(e.elements),0)+" · bytes="+DisplayAmount(double(e.bytes),0)+" B";
    if(text.value=="--" && e.beginNs && e.endNs>=e.beginNs)
    { text.value=DisplayAmount(double(e.endNs-e.beginNs)/1e6,3); text.unit="CPU ms"; }
    return text;
}
std::optional<double> ComparisonNumber(const D::LiveAggregate& a,ComparisonMetric metric)
{
    if(!a.frameCount) return {};
    if(metric==ComparisonMetric::Frames) return double(a.frameCount);
    if(metric==ComparisonMetric::Draws) return a.recordedMean.draw+a.recordedMean.drawIndexed;
    if(metric==ComparisonMetric::Instances) return a.recordedMean.instances;
    if(metric==ComparisonMetric::Dispatch) return a.recordedMean.dispatch;
    if(metric==ComparisonMetric::Upload) return a.recordedMean.uploadBytes;
    const auto& value=metric==ComparisonMetric::Cpu ? a.cpuRender : metric==ComparisonMetric::Gpu ? a.gpuMeasured : a.frame;
    if(value.state!=D::LiveValueState::Ready || !value.ready || !std::isfinite(value.average)) return {};
    if(metric==ComparisonMetric::Gpu && (value.ready!=a.frameCount || value.pending || value.unsupported || value.unavailable)) return {};
    if(metric==ComparisonMetric::Fps) return value.average>0 && std::isfinite(a.fps) ? std::optional<double>(a.fps) : std::nullopt;
    return value.average;
}
std::optional<double> StageNumber(const D::LiveAggregate& a,D::LiveStage stage,StageMetric metric)
{
    if(!a.frameCount || size_t(stage)>=D::LiveStageCount) return {};
    const auto& value=a.stages[size_t(stage)];
    if(metric==StageMetric::Cpu || metric==StageMetric::Gpu)
    {
        const auto& timing=metric==StageMetric::Cpu ? value.cpu : value.gpu;
        if(timing.state!=D::LiveValueState::Ready || !timing.ready || timing.ready>a.frameCount || !std::isfinite(timing.average) || timing.average<0) return {};
        if(metric==StageMetric::Gpu && (timing.ready!=a.frameCount || timing.pending || timing.unsupported || timing.unavailable)) return {};
        return timing.average;
    }
    const auto mean=metric==StageMetric::Operations ? value.meanOperations : metric==StageMetric::Elements ? value.meanElements
        : metric==StageMetric::Bytes ? value.meanBytes : std::numeric_limits<double>::quiet_NaN();
    return std::isfinite(mean) && mean>=0 ? std::optional<double>(mean) : std::nullopt;
}
const std::array<CounterField,ComparisonCounterCount>& ComparisonCounters()
{
    using C=D::LiveCounters<double>;
    static constexpr std::array<CounterField,ComparisonCounterCount> fields{{
        {"draw","Draw",&C::draw},{"draw_indexed","DrawIndexed",&C::drawIndexed},{"dispatch","Dispatch",&C::dispatch},
        {"dispatch_mesh","DispatchMesh",&C::dispatchMesh},{"instances","인스턴스",&C::instances},{"barriers","Barrier",&C::barriers},
        {"bindings","Binding",&C::bindings},{"submissions","제출",&C::submissions},{"waits","대기",&C::waits},
        {"created","생성",&C::objectsCreated},{"destroyed","해제",&C::objectsDestroyed},{"batches","배치",&C::batches},
        {"elements","처리 요소",&C::elements},{"dirty_ranges","변경 구간",&C::dirtyRanges},{"upload_calls","Upload 호출",&C::uploadCalls},
        {"upload_bytes","Upload B",&C::uploadBytes},{"full_uploads","전체 Upload",&C::fullUploads},{"partial_uploads","부분 Upload",&C::partialUploads},
        {"upload_page_allocations","신규 업로드 페이지 할당",&C::uploadPageAllocations,true},{"upload_page_reuses","업로드 페이지 재사용",&C::uploadPageReuses,true},
        {"upload_page_allocation_bytes","신규 업로드 페이지 할당 B",&C::uploadPageAllocationBytes,true}}};
    return fields;
}
std::optional<double> CounterNumber(const D::LiveAggregate& a,CounterPhase phase,const CounterField& field)
{
    if(!a.frameCount || !field.member || (field.recordedOnly && phase!=CounterPhase::Recorded)) return {};
    const auto& counters=phase==CounterPhase::Recorded ? a.recordedMean : phase==CounterPhase::Submitted ? a.submittedMean : a.completedMean;
    const auto value=counters.*field.member;
    return std::isfinite(value) && value>=0 ? std::optional<double>(value) : std::nullopt;
}
NumberChange CompareNumbers(std::optional<double> before,std::optional<double> after)
{
    NumberChange result;
    if(before && std::isfinite(*before)) result.before=before;
    if(after && std::isfinite(*after)) result.after=after;
    if(result.before && result.after)
    {
        const auto delta=*result.after-*result.before;
        if(std::isfinite(delta)) result.delta=delta;
        if(result.delta && *result.before!=0)
        {
            const auto percent=delta/std::abs(*result.before)*100;
            if(std::isfinite(percent)) result.percent=percent;
        }
    }
    return result;
}
NumberChange CompareObservedCounts(std::optional<double> before,std::optional<double> after,bool beforeComplete,bool afterComplete)
{
    auto result=CompareNumbers(before,after);
    if(!beforeComplete || !afterComplete) { result.delta.reset(); result.percent.reset(); }
    return result;
}
void ViewerState::Clear()
{
    snapshot.reset(); receivedNs = 0; device = source = 0; selection = {};
    requests.clear(); omittedCompletedRequests = 0;
}

bool ViewerState::Accept(std::shared_ptr<const D::LiveSnapshot> incoming, uint64_t nowNs)
{
    if(!incoming || (snapshot && incoming->session == snapshot->session && incoming->sequence <= snapshot->sequence)) return false;
    if(snapshot && incoming->session != snapshot->session) Clear();
    snapshot = std::move(incoming); receivedNs = nowNs;
    if(!Source() && !snapshot->sources.empty())
    {
        const auto active=std::max_element(snapshot->sources.begin(),snapshot->sources.end(),[](const auto& a,const auto& b) { return a.latest.endNs<b.latest.endNs; });
        SelectSource(active->device,active->source);
    }
    for(const auto& status : snapshot->controlStatus)
    {
        const auto found = std::find_if(requests.begin(), requests.end(), [&](const auto& existing) { return existing.request == status.request; });
        if(found == requests.end()) requests.push_back(status);
        else
        {
            auto update=status;
            if(!update.device) update.device=found->device;
            if(!update.source) update.source=found->source;
            if(!update.requestedNs)
            {
                update.requestedNs=found->requestedNs;
                if(update.state!=D::ControlState::Applied) update.actual=found->actual;
            }
            *found=std::move(update);
        }
    }
    while(requests.size() > 512)
    {
        const auto finished = std::find_if(requests.begin(), requests.end(), [](const auto& status) {
            return status.state == D::ControlState::Applied || status.state == D::ControlState::Rejected
                || status.state == D::ControlState::Failed || status.state == D::ControlState::Cancelled;
        });
        if(finished == requests.end()) break;
        requests.erase(finished); ++omittedCompletedRequests;
    }
    return true;
}

void ViewerState::SelectSource(D::LiveId deviceId, D::LiveId sourceId)
{
    if(device == deviceId && source == sourceId) return;
    device = deviceId; source = sourceId; selection = {};
}

const D::LiveSourceSnapshot* ViewerState::Source() const
{
    if(snapshot) for(const auto& value : snapshot->sources) if(value.device == device && value.source == source) return &value;
    return nullptr;
}

const D::LiveDeviceSnapshot* ViewerState::Device() const
{
    if(snapshot) for(const auto& value : snapshot->devices) if(value.device == device) return &value;
    return nullptr;
}

const D::ControlStatus* ViewerState::Comparison() const
{
    const D::ControlStatus* found = nullptr;
    const auto* current = Source();
    for(const auto& status : requests)
        if(current && status.device == device && status.source == source && status.state == D::ControlState::Applied && status.appliedRevision
            && (!found || status.appliedNs > found->appliedNs || (status.appliedNs == found->appliedNs && status.request > found->request))) found = &status;
    return found;
}

bool ViewerState::ExactClock(const D::LiveClockInfo& local) const
{
    return snapshot && snapshot->clock.cpu == D::LiveCpuClock::QueryPerformanceCounter
        && local.cpu == D::LiveCpuClock::QueryPerformanceCounter && local.frequency
        && snapshot->clock.frequency == local.frequency && snapshot->clock.originTicks == local.originTicks;
}
uint64_t ViewerState::AgeNs(uint64_t nowNs, const D::LiveClockInfo& local) const
{
    const auto* current = Source();
    if(current && current->latest.endNs && ExactClock(local) && nowNs >= current->latest.endNs) return nowNs-current->latest.endNs;
    const auto elapsed = nowNs > receivedNs ? nowNs - receivedNs : 0;
    const auto age = current ? current->latest.sampleAgeNs : 0;
    return age > UINT64_MAX - elapsed ? UINT64_MAX : age + elapsed;
}

void ViewerState::Queued(D::LiveId request, const D::ControlDesc& desc, const D::ControlValue& value, uint64_t nowNs)
{
    if(!request) return;
    D::ControlStatus status;
    status.request = request; status.control = desc.id; status.device = desc.device; status.source = desc.source;
    status.requested = value; status.actual = desc.appliedValue; status.requestedNs = nowNs;
    status.message = "Client queued; awaiting producer validation.";
    requests.push_back(std::move(status));
}

void ViewerState::Select(SelectionKind kind, D::LiveId id)
{
    selection = {kind, id, Source() ? Source()->latest.key : D::LiveFrameKey{device,source,0}, snapshot, receivedNs};
}

static const D::LiveSourceSnapshot* PinnedSource(const Selection& selection)
{
    if(selection.snapshot) for(const auto& value : selection.snapshot->sources)
        if(value.device == selection.source.device && value.source == selection.source.source) return &value;
    return nullptr;
}

const D::LiveEvent* ViewerState::SelectedEvent() const
{
    if(const auto* current = Source()) if(selection.kind == SelectionKind::Event)
        for(const auto& event : current->flow.events) if(event.id == selection.id && event.context.frame==selection.source) return &event;
    const auto* pinned = PinnedSource(selection);
    if(pinned && selection.kind == SelectionKind::Event) for(const auto& event : pinned->flow.events)
        if(event.id == selection.id && event.context.frame==selection.source) return &event;
    return nullptr;
}
const D::LiveObjectInfo* ViewerState::SelectedObject() const
{
    if(selection.kind != SelectionKind::Object) return nullptr;
    if(snapshot) for(const auto& object:snapshot->objects) if(object.id==selection.id) return &object;
    if(selection.snapshot) for(const auto& object:selection.snapshot->objects) if(object.id==selection.id) return &object;
    return nullptr;
}

const D::LiveGpuSpan* ViewerState::SelectedGpu() const
{
    if(const auto* current = Source()) if(selection.kind == SelectionKind::Gpu)
    {
        for(const auto& span : current->recentGpuSpans) if(span.id == selection.id) return &span;
        for(const auto& span : current->flow.gpuSpans) if(span.id == selection.id) return &span;
    }
    const auto* pinned = PinnedSource(selection);
    if(pinned && selection.kind == SelectionKind::Gpu)
    {
        for(const auto& span : pinned->recentGpuSpans) if(span.id == selection.id) return &span;
        for(const auto& span : pinned->flow.gpuSpans) if(span.id == selection.id) return &span;
    }
    return nullptr;
}

bool ViewerState::Related(const D::LiveEvent& event) const
{
    if(selection.kind == SelectionKind::None) return true;
    const auto has = [&](D::LiveId id) {
        return id && (event.id == id || event.context.owner == id || event.context.object == id
            || event.context.batch == id || event.resource == id || event.submission == id || event.command.commandList == id);
    };
    if(has(selection.id)) return true;
    if(const auto* selected = SelectedEvent())
    {
        if(has(selected->context.object) || has(selected->context.batch) || has(selected->resource) || has(selected->submission)) return true;
        if(selected->command.commandList && event.command.commandList == selected->command.commandList
            && event.command.generation == selected->command.generation) return true;
    }
    if(const auto* span = SelectedGpu())
    {
        if(has(span->begin.submission) || has(span->end.submission)) return true;
        if(event.command.commandList && ((event.command.commandList == span->begin.command.commandList
            && event.command.generation == span->begin.command.generation) || (event.command.commandList == span->end.command.commandList
            && event.command.generation == span->end.command.generation))) return true;
    }
    const auto* current = Source();
    if(current) for(const auto& link : current->flow.links)
        if((link.from == selection.id && has(link.to)) || (link.to == selection.id && has(link.from))) return true;
    return false;
}

bool ValidControlValue(const D::ControlDesc& desc, const D::ControlValue& value)
{
    if(desc.kind == D::ControlKind::Boolean) return std::holds_alternative<bool>(value);
    if(desc.kind == D::ControlKind::Enumeration)
    {
        const auto* integer = std::get_if<int64_t>(&value);
        return integer && std::any_of(desc.choices.begin(), desc.choices.end(), [&](const auto& choice) { return choice.value == *integer; });
    }
    if(!std::isfinite(desc.minimum) || !std::isfinite(desc.maximum) || desc.minimum > desc.maximum) return false;
    if(desc.kind == D::ControlKind::Integer)
    {
        const auto* integer = std::get_if<int64_t>(&value);
        return integer && static_cast<long double>(*integer) >= desc.minimum && static_cast<long double>(*integer) <= desc.maximum;
    }
    if(desc.kind!=D::ControlKind::Real) return false;
    const auto* real = std::get_if<double>(&value);
    return real && std::isfinite(*real) && *real >= desc.minimum && *real <= desc.maximum;
}

std::string ValueText(const D::ControlValue& value)
{
    if(const auto* boolean = std::get_if<bool>(&value)) return *boolean ? "true" : "false";
    if(const auto* integer = std::get_if<int64_t>(&value)) return std::to_string(*integer);
    char text[64]; std::snprintf(text, sizeof(text), "%.6g", std::get<double>(value)); return text;
}

template<class E, size_t N> static const char* Name(E value, const char* const (&names)[N])
{
    const auto index = static_cast<size_t>(value); return index < N ? names[index] : "unknown";
}
const char* ValueState(D::LiveValueState value) { static const char* const names[] = {"미수집", "미지원", "대기", "ready"}; return Name(value, names); }
const char* Outcome(D::LiveOutcome value) { static const char* const names[] = {"observed", "pending", "submitted", "completed", "skipped", "failed", "discarded", "unknown"}; return Name(value, names); }
const char* ControlState(D::ControlState value) { static const char* const names[] = {"요청됨", "검증됨", "적용 대기", "실제 적용", "거절", "실패", "취소"}; return Name(value, names); }
const char* Application(D::ControlApplication value) { static const char* const names[] = {"즉시 적용", "자원 재구성", "재시작 필요", "재빌드 필요"}; return Name(value, names); }
const char* ObjectKind(D::LiveObjectKind value) { static const char* const names[] = {"Device", "Renderer", "Scene", "Entity", "Canvas", "Model", "Mesh", "Material", "Buffer", "Texture", "Pipeline", "Shader", "ResourceSet", "Query", "CommandList", "Batch"}; return Name(value, names); }
const char* CommandKind(D::LiveCommandKind value) { static const char* const names[] = {"None", "Draw", "DrawIndexed", "Dispatch", "DispatchMesh", "Barrier", "Bind", "Upload", "Wait", "Present"}; return Name(value, names); }
const char* EventKind(D::LiveEventKind value) { static const char* const names[] = {"CpuWork", "DataRange", "Batch", "Upload", "Command", "NativeTranslation", "Submission", "Output", "Wait", "UploadMemory"}; return Name(value, names); }
const char* SplitReason(D::LiveSplitReason value) { static const char* const names[] = {"None", "InstancingDisabled", "CustomGeometry", "Mesh", "Material", "Texture", "TexturePageCapacity", "ShadowFlags", "Culled", "Viewport", "Scissor", "Noncontiguous", "Capacity", "DirtyRangeLimit", "NewResource", "SceneChanged", "ElementCountChanged", "EndOfInput"}; return Name(value, names); }
const char* StageName(D::LiveStage value) { static const char* const names[] = {"CPU work", "Data / batch", "GPU upload", "Record", "Native translate", "Submit", "GPU execution", "Output"}; return Name(value, names); }
const char* GpuScope(D::LiveGpuScope value) { static const char* const names[] = {"Command list", "Main forward", "Shadow", "Model compute", "Legacy main window"}; return Name(value, names); }
}
