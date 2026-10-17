#include "Viewer.h"
#include <imgui.h>
#include <imgui_internal.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <map>
#include <fstream>
#include <iomanip>

namespace viewer
{
static const ImVec4 accent{.22f, .76f, .75f, 1}, warning{1, .69f, .3f, 1};
static constexpr const char* stages[] = {"CPU 준비", "데이터·배치", "업로드", "명령 기록", "네이티브 변환", "제출", "GPU", "출력"};

void Style()
{
    ImGui::StyleColorsDark(); auto& style = ImGui::GetStyle();
    style.WindowPadding = {8, 6}; style.FramePadding = {5, 4}; style.ItemSpacing = {6, 5}; style.CellPadding={5,4};
    style.WindowRounding = 0; style.FrameRounding = 3; style.ChildRounding = 4;
    style.Colors[ImGuiCol_WindowBg] = {.055f, .068f, .09f, 1};
    style.Colors[ImGuiCol_ChildBg] = {.07f, .09f, .12f, 1};
    style.Colors[ImGuiCol_Header] = {.1f, .26f, .36f, 1};
    style.Colors[ImGuiCol_Button] = {.1f, .25f, .34f, 1};
    style.Colors[ImGuiCol_ButtonHovered] = {.12f, .38f, .49f, 1};
    style.Colors[ImGuiCol_CheckMark] = accent; style.Colors[ImGuiCol_PlotLines] = accent;
}

float TableHeight(size_t rows,size_t visibleRows)
{
    const float row=ImGui::GetTextLineHeight()+ImGui::GetStyle().CellPadding.y*2;
    return std::min((float(std::min(rows,visibleRows))+1)*row+2,std::max(row*3,ImGui::GetWindowHeight()*.4f));
}

static bool Ready(const D::LiveSample& value) { return value.state == D::LiveValueState::Ready && std::isfinite(value.milliseconds); }
static bool Ready(const D::LiveMetricAggregate& value) { return value.ready && value.state == D::LiveValueState::Ready && std::isfinite(value.average); }
static void Sample(const char* label, const D::LiveSample& value)
{
    if(Ready(value)) ImGui::Text("%s %.3f ms", label, value.milliseconds);
    else ImGui::TextDisabled("%s -- (%s)", label, ValueState(value.state));
}
static bool StageMatches(int selected, D::LiveStage stage)
{ return selected>=0 && selected<int(D::LiveStageCount) && selected==int(stage); }
const char* Viewer::UpdateStatus() const
{
    if(client.GetState()==D::LiveClientState::Connected)
        return state.snapshot && D::LiveNowNs()>=state.receivedNs && D::LiveNowNs()-state.receivedNs<2000000000ull ? "갱신중" : "대기";
    if(client.GetState()==D::LiveClientState::Connecting) return "대기";
    return state.snapshot || client.GetState()==D::LiveClientState::Failed || client.GetState()==D::LiveClientState::Unsupported ? "연결종료" : "대기";
}

void Viewer::Launch()
{
    std::string arguments;
    if(!launch.BuildArguments(arguments,launchIssue)) return;
    if(process.Launch(launch.executable, arguments, launch.directory, launchIssue))
    { wantedPid = process.Pid(); nextDiscoveryNs = 0; message = "대상 실행됨. 계측 세션을 찾는 중입니다."; }
}
void Viewer::Connect(const D::LiveTarget& target)
{
    if(!client.Connect(target)) { message = client.GetLastError(); return; }
    reconnect = target; state.Clear(); drafts.clear(); indexedSequence = graphSequence = UINT64_MAX;
    receivedSnapshots=0;
    statistics.Clear(); playback.Clear(); comparisonPlayback.Clear(); presentationLog={}; graphFocus=0; selectedControl=0;
    comparisonGraphGeneration=UINT64_MAX;
    message = "연결 요청됨. 실제 스냅샷을 기다립니다.";
}
void Viewer::Tick()
{
    PollExecutableScan();
    const auto now = D::LiveNowNs();
    if(discovery.valid() && discovery.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
    {
        try { targets = discovery.get(); }
        catch(const std::exception& error) { message = std::string("Session discovery: ") + error.what(); }
    }
    if(!discovery.valid() && now >= nextDiscoveryNs)
    {
        discovery = std::async(std::launch::async, [] { return D::LiveClient::Discover(); });
        nextDiscoveryNs = now + 750000000;
    }
    if(wantedPid)
    {
        for(const auto& target : targets) if(target.processId == wantedPid) { Connect(target); wantedPid = 0; break; }
        if(process.Pid() == wantedPid && process.ExitCode()) { wantedPid = 0; message = "대상이 계측 세션 연결 전에 종료되었습니다."; }
    }
    if(auto incoming = client.Poll())
    {
        if(state.snapshot && incoming->session!=state.snapshot->session)
        { statistics.Clear(); playback.Clear(); comparisonPlayback.Clear(); presentationLog={}; graphFocus=0; drafts.clear(); selectedControl=0; comparisonGraphGeneration=UINT64_MAX; }
        if(state.Accept(std::move(incoming), now)) { message.clear(); ++receivedSnapshots; }
    }
    if(client.GetState()==D::LiveClientState::Connected)
    {
        statistics.Update(state.snapshot,state.device,state.source,now);
        if(playback.Update(state.snapshot,state.device,state.source,now)) indexedSequence=graphSequence=UINT64_MAX;
        if(comparisonPlayback.Update(state.snapshot,state.device,state.source,state.Comparison() ? state.Comparison()->request : 0,now)) comparisonGraphGeneration=UINT64_MAX;
    }
}
D::LiveId Viewer::Send(const D::ControlDesc& control, const D::ControlValue& value)
{
    if(!ValidControlValue(control, value)) { message = "옵션 값의 형식 또는 범위가 잘못되었습니다."; return 0; }
    const auto request = client.RequestControl(control.id, value);
    if(!request) { message = "옵션 요청을 보내지 못했습니다. " + client.GetLastError(); return 0; }
    state.Queued(request, control, value, D::LiveNowNs());
    return request;
}

void Viewer::Dock()
{
    const auto* viewport=ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos); ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::Begin("##DockHost",nullptr,ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove
        | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus);
    const float right=ImGui::GetCursorScreenPos().x+ImGui::GetContentRegionAvail().x;
    const auto sameLineIfFits=[&](const char* text,float padding) {
        if(ImGui::GetItemRectMax().x+ImGui::GetStyle().ItemSpacing.x+ImGui::CalcTextSize(text).x+padding<=right) ImGui::SameLine();
    };
    ImGui::PushFont(nullptr,26); ImGui::TextColored(accent,"DY ENGINE / EXECUTION VISUALIZER"); ImGui::PopFont();
    sameLineIfFits(UpdateStatus(),0); ImGui::TextDisabled("%s",UpdateStatus());
    sameLineIfFits("연결·실행",ImGui::GetStyle().FramePadding.x*2);
    if(ImGui::SmallButton("연결·실행")) ImGui::OpenPopup("##ConnectionSetup");
    ImGui::SetNextWindowSizeConstraints({280,0},{std::min(560.f,viewport->WorkSize.x-16),viewport->WorkSize.y*.8f});
    if(ImGui::BeginPopup("##ConnectionSetup",ImGuiWindowFlags_AlwaysAutoResize)) { Targets(); ImGui::EndPopup(); }
    sameLineIfFits("배치 초기화",ImGui::GetStyle().FramePadding.x*2);
    if(ImGui::SmallButton("배치 초기화")) resetLayout=true;
    ImGui::Separator();
    const auto dock=ImGui::GetID("ExecutionVisualizerReadableFlowDock");
    if(!ImGui::DockBuilderGetNode(dock) || resetLayout)
    {
        resetLayout=false; ImGui::DockBuilderRemoveNode(dock); ImGui::DockBuilderAddNode(dock,ImGuiDockNodeFlags_DockSpace);
        ImGui::DockBuilderSetNodeSize(dock,ImGui::GetContentRegionAvail());
        ImGuiID rightSide=dock;
        const auto controls=ImGui::DockBuilderSplitNode(rightSide,ImGuiDir_Left,.44f,nullptr,&rightSide);
        const auto& style=ImGui::GetStyle();
        const float cardWidth=ImGui::GetContentRegionAvail().x*.56f-style.WindowPadding.x*2-style.ScrollbarSize;
        const int cardColumns=std::clamp(int(cardWidth/220),1,5),cardRows=(9+cardColumns-1)/cardColumns;
        const float overviewHeight=float(cardRows)*(82+style.CellPadding.y*2)+ImGui::GetFrameHeightWithSpacing()
            +ImGui::GetTextLineHeightWithSpacing()*2+style.WindowPadding.y*2+style.CellPadding.y*2+2;
        const float overviewRatio=std::clamp(overviewHeight/std::max(1.f,ImGui::GetContentRegionAvail().y),.2f,.5f);
        const auto overview=ImGui::DockBuilderSplitNode(rightSide,ImGuiDir_Up,overviewRatio,nullptr,&rightSide);
        ImGui::DockBuilderDockWindow("Controls / 실제 적용",controls);
        ImGui::DockBuilderDockWindow("Overview / 현재 상태",overview);
        ImGui::DockBuilderDockWindow("Flow / 데이터 흐름",rightSide);
        ImGui::DockBuilderFinish(dock);
    }
    ImGui::DockSpace(dock,{0,0}); ImGui::End();
}



void Viewer::Targets()
{
    ImGui::TextUnformatted("계측 제작물 / instrumented sessions");
    if(ImGui::Button("Refresh / 새로 고침")) nextDiscoveryNs = 0;
    if(targets.empty()) ImGui::TextDisabled("현재 세션 없음");
    for(const auto& target : targets)
    {
        const auto title = std::to_string(target.processId) + "  " + target.executable + "##" + target.endpoint;
        if(ImGui::Selectable(title.c_str(), reconnect && target.session == reconnect->session)) Connect(target);
        if(ImGui::IsItemHovered()) ImGui::SetTooltip("%s\nPID %u\n%s", target.executable.c_str(), target.processId, target.endpoint.c_str());
    }
    ImGui::BeginDisabled(client.GetState() == D::LiveClientState::Disconnected);
    if(ImGui::Button("Disconnect / 연결 해제")) { client.Disconnect(); message = "연결 해제됨. 마지막 측정값은 과거 값으로 보존합니다."; }
    ImGui::EndDisabled(); ImGui::SameLine(); ImGui::BeginDisabled(!reconnect.has_value());
    if(ImGui::Button("Reconnect")) Connect(*reconnect);
    ImGui::EndDisabled();
    if(state.snapshot && !state.snapshot->sources.empty())
    {
        const auto current = std::to_string(state.device) + "/" + std::to_string(state.source);
        if(ImGui::BeginCombo("Source", current.c_str()))
        {
            for(const auto& source : state.snapshot->sources)
            {
                std::string label = std::to_string(source.device) + "/" + std::to_string(source.source);
                for(const auto& object : state.snapshot->objects) if(object.id == source.source) { label += " " + object.desc.label; break; }
                if(ImGui::Selectable(label.c_str(), state.device == source.device && state.source == source.source))
                { state.SelectSource(source.device, source.source); statistics.Clear(); playback.Clear(); presentationLog={};
                    graphFocus=0; indexedSequence=graphSequence=UINT64_MAX; }
            }
            ImGui::EndCombo();
        }
    }
    ImGui::Separator(); ImGui::TextUnformatted("폴더에서 실행 파일 선택");
    ImGui::TextWrapped("폴더: %s",executableFolder.empty() ? "미지정" : executableFolder.c_str());
    ImGui::TextWrapped("선택: %s",launch.executable.empty() ? "없음" : launch.executable.c_str());
    if(ImGui::Button("Launch setup / 실행 설정",{-1,0})) { openLaunchDialog=true; ImGui::CloseCurrentPopup(); }
    if(process.Pid())
    {
        ImGui::Text("Target PID: %u", process.Pid());
        if(const auto code = process.ExitCode()) ImGui::Text("종료 코드: %u", *code); else ImGui::TextDisabled("실행 중");
    }
    if(wantedPid) ImGui::TextDisabled("Attach PID %u: 세션 기다리는 중", wantedPid);
    const auto error = client.GetLastError(); if(!error.empty()) ImGui::TextWrapped("%s", error.c_str());
    if(!message.empty()) ImGui::TextWrapped("%s", message.c_str());
    ImGui::Separator(); if(ImGui::Button("Reset layout / 배치 초기화", {-1, 0})) resetLayout = true;
}



void Viewer::Events()
{
    const auto* source=playback.Source(); if(!source) { ImGui::TextDisabled("실제 이벤트 미수집"); return; }
    ImGui::SetNextItemWidth(220); ImGui::Combo("단계",&selectedStage,"CPU 준비\0데이터·배치\0업로드\0명령 기록\0네이티브 변환\0제출\0GPU\0출력\0");
    if(ImGui::Checkbox("선택과 연결된 항목만",&onlyRelated)) indexedSequence=UINT64_MAX;
    if(indexedSequence != playback.generation || indexedSelection != state.selection.id || indexedStage != selectedStage)
    {
        visibleEvents.clear(); std::unordered_set<D::LiveId> related;
        if(onlyRelated && state.selection.id)
            for(const auto& node:BuildFlowGraph(*playback.snapshot,*source,state.selection.id).nodes) related.insert(node.id);
        for(size_t i=0;i<source->flow.events.size();++i)
            if(StageMatches(selectedStage,source->flow.events[i].stage) && (!onlyRelated || !state.selection.id || related.count(source->flow.events[i].id))) visibleEvents.push_back(i);
        indexedSequence = playback.generation; indexedSelection = state.selection.id; indexedStage = selectedStage;
    }
    ImGui::TextDisabled("대표 프레임의 원본 상세 · 실제 이벤트 %zu개",visibleEvents.size());
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("현재 고정된 원본 표본의 이벤트입니다. 누적 식별값은 보고서에 보존합니다.");
    if(ImGui::BeginTable("Events",5,ImGuiTableFlags_ScrollY|ImGuiTableFlags_RowBg|ImGuiTableFlags_Resizable,{0,ImGui::GetContentRegionAvail().y}))
    {
        for(const auto* label:{"종류 / 명령","요소","Bytes","실제 결과","이유 / 설명"}) ImGui::TableSetupColumn(label);
        ImGui::TableSetupScrollFreeze(0,1); ImGui::TableHeadersRow(); ImGuiListClipper clipper; clipper.Begin(int(visibleEvents.size()));
        while(clipper.Step()) for(int row=clipper.DisplayStart;row<clipper.DisplayEnd;++row)
        {
            const auto& event=source->flow.events[visibleEvents[size_t(row)]];
            ImGui::PushID(reinterpret_cast<void*>(uintptr_t(event.id))); ImGui::TableNextRow(); ImGui::TableNextColumn();
            const auto label=std::string(EventKind(event.kind))+" / "+CommandKind(event.commandKind);
            if(ImGui::Selectable(label.c_str(),state.selection.kind==SelectionKind::Event && state.selection.id==event.id,ImGuiSelectableFlags_SpanAllColumns))
            { SelectTrace(SelectionKind::Event,event.id); onlyRelated=true; indexedSequence=UINT64_MAX; }
            if(ImGui::IsItemHovered()) { ImGui::BeginTooltip(); EventSummary(event,"원본 명령 표본"); ImGui::EndTooltip(); }
            ImGui::TableNextColumn(); ImGui::TextUnformatted(DisplayAmount(double(event.elements),0).c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(DisplayAmount(double(event.bytes),0).c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(Outcome(event.outcome));
            ImGui::TableNextColumn(); ImGui::Text("%s / %.*s",SplitReason(event.reason),64,event.label);
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
}

void Viewer::Inventory()
{
    const auto& values=statistics.state;
    if(!values.snapshot) { ImGui::TextDisabled("생존 객체·자원 미수집"); return; }
    if(const auto* device=values.Device()) if(ImGui::TreeNode("자원 풀 원본 통계"))
    {
        const auto& upload=device->upload;
        ImGui::Text("현재 사용 %s B / 캐시 %s B",DisplayAmount(double(upload.inUseBytes),0).c_str(),DisplayAmount(double(upload.cachedBytes),0).c_str());
        for(const auto& resource:device->resources) ImGui::Text("%s 생존 %llu",ObjectKind(resource.kind),resource.live);
        ImGui::TreePop();
    }
    if(ImGui::BeginTable("Inventory",4,ImGuiTableFlags_ScrollY|ImGuiTableFlags_RowBg|ImGuiTableFlags_Resizable,{0,ImGui::GetContentRegionAvail().y}))
    {
        for(const auto* label:{"종류","실제 이름","요소","Bytes"}) ImGui::TableSetupColumn(label);
        ImGui::TableSetupScrollFreeze(0,1); ImGui::TableHeadersRow(); ImGuiListClipper clipper; clipper.Begin(int(values.snapshot->objects.size()));
        while(clipper.Step()) for(int row=clipper.DisplayStart;row<clipper.DisplayEnd;++row)
        {
            const auto& object=values.snapshot->objects[size_t(row)]; ImGui::PushID(reinterpret_cast<void*>(uintptr_t(object.id)));
            ImGui::TableNextRow(); ImGui::TableNextColumn();
            if(ImGui::Selectable(ObjectKind(object.desc.kind),state.selection.kind==SelectionKind::Object && state.selection.id==object.id,ImGuiSelectableFlags_SpanAllColumns))
            {
                state.selection={SelectionKind::Object,object.id,values.Source() ? values.Source()->latest.key : D::LiveFrameKey{},values.snapshot,statistics.updatedNs};
                revealDetails=true; onlyRelated=true; indexedSequence=UINT64_MAX;
            }
            if(ImGui::IsItemHovered()) ImGui::SetTooltip("%s · 현재 생존 자원",object.desc.label.c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(object.desc.label.c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(DisplayAmount(double(object.desc.elements),0).c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(DisplayAmount(double(object.desc.bytes),0).c_str()); ImGui::PopID();
        }
        ImGui::EndTable();
    }
}

void Viewer::Gpu()
{
    const auto* source=statistics.state.Source(); if(!source) { ImGui::TextDisabled("GPU 결과 미수집"); return; }
    const auto samples=GpuSamples(*source);
    if(const auto* device=statistics.state.Device())
    {
        ImGui::Text("%s / %u개 queue",device->backend.c_str(),device->queueCount);
        if(ImGui::IsItemHovered()) { ImGui::BeginTooltip(); ImGui::PushTextWrapPos(ImGui::GetCursorPosX()+450.f); ImGui::TextUnformatted("미지원은 GPU 타이밍 측정 상태이며 개별 렌더링·컴퓨트 기능의 미지원 판정이 아닙니다."); ImGui::PopTextWrapPos(); ImGui::EndTooltip(); }
    }
    if(samples.empty()) ImGui::TextDisabled("보존된 GPU 타이밍 원본 없음 · 1초 집계 상태: %s",ValueState(source->recent.gpuMeasured.state));
    if(ImGui::BeginTable("Gpu",4,ImGuiTableFlags_ScrollY|ImGuiTableFlags_RowBg|ImGuiTableFlags_Resizable,{0,ImGui::GetContentRegionAvail().y}))
    {
        for(const auto* label:{"측정 범위","측정 상태 / 시간","Queue","CPU 보정"}) ImGui::TableSetupColumn(label);
        ImGui::TableSetupScrollFreeze(0,1); ImGui::TableHeadersRow(); ImGuiListClipper clipper; clipper.Begin(int(samples.size()));
        while(clipper.Step()) for(int row=clipper.DisplayStart;row<clipper.DisplayEnd;++row)
        {
            const auto& span=*samples[size_t(row)]; ImGui::PushID(reinterpret_cast<void*>(uintptr_t(span.id)));
            ImGui::TableNextRow(); ImGui::TableNextColumn();
            if(ImGui::Selectable(GpuScope(span.scope),state.selection.kind==SelectionKind::Gpu && state.selection.id==span.id,ImGuiSelectableFlags_SpanAllColumns))
            { state.selection={SelectionKind::Gpu,span.id,span.frame,statistics.state.snapshot,statistics.updatedNs}; revealDetails=true; onlyRelated=true; indexedSequence=UINT64_MAX; }
            if(ImGui::IsItemHovered())
            {
                ImGui::BeginTooltip();
                ImGui::Text("%s · 원본 표본",GpuScope(span.scope));
                Sample("GPU 측정",span.duration);
                ImGui::TextUnformatted(span.hasCpuInterval ? "CPU/GPU 시계 보정 있음" : "CPU/GPU 시계 미보정");
                if(span.cpuCalibrationErrorKnown) ImGui::Text("보정 오차 %llu ns",span.cpuCalibrationErrorNs);
                ImGui::EndTooltip();
            }
            ImGui::TableNextColumn(); Sample(Outcome(span.outcome),span.duration);
            ImGui::TableNextColumn(); ImGui::Text("%u",span.queueIndex);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(span.hasCpuInterval ? "보정됨" : "미보정"); ImGui::PopID();
        }
        ImGui::EndTable();
    }
}

void Viewer::EventSummary(const D::LiveEvent& event,const char* caption)
{
    const bool tooltip=(ImGui::GetCurrentWindow()->Flags & ImGuiWindowFlags_Tooltip)!=0;
    if(tooltip) ImGui::PushTextWrapPos(ImGui::GetCursorPosX()+450.f);
    const auto text=DescribeEvent(event);
    ImGui::TextWrapped("%s · %s · 결과 %s",caption,text.name.c_str(),Outcome(event.outcome));
    if(!tooltip && ImGui::IsItemHovered()) { ImGui::BeginTooltip(); ImGui::PushTextWrapPos(ImGui::GetCursorPosX()+450.f); ImGui::TextUnformatted(text.explanation.c_str()); ImGui::PopTextWrapPos(); ImGui::EndTooltip(); }
    if(!text.label.empty() && text.label!=text.name) ImGui::TextWrapped("%s",text.label.c_str());
    ImGui::TextWrapped("%s",text.arguments.c_str());
    if(tooltip) ImGui::TextWrapped("%s",text.explanation.c_str());
    if(tooltip) ImGui::PopTextWrapPos();
}
void Viewer::Details()
{
    ImGui::PushTextWrapPos();
    ImGui::Text("Stage: %s", stages[selectedStage]); ImGui::SameLine();
    if(ImGui::SmallButton("Clear selection")) { state.selection = {}; onlyRelated = false; indexedSequence = UINT64_MAX; }
    ImGui::SameLine(); if(ImGui::Checkbox("Linked only", &onlyRelated)) indexedSequence = UINT64_MAX;
    if(const auto* event = state.SelectedEvent())
    {
        EventSummary(*event,"선택 원본 표본");
        ImGui::Text("Range first %llu / stride %llu", event->first, event->stride);
        ImGui::Text("원본 요소 %llu / %llu B / operations %llu / %s",event->elements,event->bytes,event->operations,SplitReason(event->reason));
        if(event->kind == D::LiveEventKind::Upload) { ImGui::SameLine(); ImGui::Text("%s upload", event->fullUpload ? "full" : "partial"); }
        if(event->beginNs && event->endNs >= event->beginNs) { ImGui::SameLine(); ImGui::Text("%.3f ms CPU", (event->endNs - event->beginNs) / 1e6); }
        else { ImGui::SameLine(); ImGui::TextDisabled("CPU time -- (미수집)"); }
    }
    if(const auto* span = state.SelectedGpu())
    {
        Sample("선택 원본 GPU",span->duration);
        const auto now=D::LiveNowNs();
        if(span->hasCpuInterval && state.ExactClock(D::GetLiveClockInfo()) && span->frame.device==state.device && span->frame.source==state.source
            && now >= span->endCpuNs) ImGui::TextDisabled("GPU end age %.1f ms (calibrated)", (now-span->endCpuNs)/1e6);
        else ImGui::TextDisabled("GPU end age: -- (CPU 시계 보정 없음)");
    }
    if(const auto* object=state.SelectedObject())
    {
        ImGui::TextColored(accent,"%s · %s · 요소 %s · %s B",ObjectKind(object->desc.kind),object->desc.label.c_str(),
            DisplayAmount(double(object->desc.elements),0).c_str(),DisplayAmount(double(object->desc.bytes),0).c_str());
        const bool live=state.snapshot && std::any_of(state.snapshot->objects.begin(),state.snapshot->objects.end(),[&](const auto& value) { return value.id==object->id; });
        if(!live) ImGui::TextColored(warning,"과거 선택: 현재 생존 목록에 없음 (해제 또는 상세 생략)");
    }
    if(state.selection.kind==SelectionKind::None) ImGui::TextDisabled("원형 노드 또는 상세 목록에서 실제 항목을 선택하세요.");
    ImGui::PopTextWrapPos();
}

void Viewer::History()
{
    ImGui::SetNextItemWidth(220);
    ImGui::Combo("Metric",&historyMetric,"Frame ms\0CPU render ms\0GPU measured ms\0Upload bytes/frame\0Draw calls/frame\0Semantic live objects\0");
    if(statistics.history.empty()) { ImGui::TextDisabled("완료된 생산자 1초 구간 대기"); return; }
    const auto point=[&](const D::LiveAggregate& bucket)->std::optional<double> {
        if(!bucket.frameCount) return {};
        if(historyMetric==3) return bucket.recordedMean.uploadBytes;
        if(historyMetric==4) return bucket.recordedMean.draw+bucket.recordedMean.drawIndexed;
        const auto& metric=historyMetric==5 ? bucket.objects : historyMetric==0 ? bucket.frame : historyMetric==1 ? bucket.cpuRender : bucket.gpuMeasured;
        if(Ready(metric)) return metric.average;
        return {};
    };
    uint64_t end=0; double maximum=0; bool havePoint=false;
    for(const auto& bucket:statistics.history)
    { end=std::max(end,bucket.endNs); if(const auto value=point(bucket)) { maximum=std::max(maximum,*value); havePoint=true; } }
    if(!havePoint) { ImGui::TextDisabled("해당 측정값은 대기 · 미지원 · 미수집입니다."); return; }
    maximum=std::max(1.0,maximum*1.1); const auto begin=end>D::LiveHistoryWindowNs ? end-D::LiveHistoryWindowNs : 0;
    ImGui::TextDisabled("생산자 완료 1초 평균 · 초당 1회 갱신 · 세로축 0..%.3f",maximum);
    const auto origin=ImGui::GetCursorScreenPos();
    const ImVec2 size{std::max(1.f,ImGui::GetContentRegionAvail().x),std::max(50.f,ImGui::GetContentRegionAvail().y-28)};
    ImGui::InvisibleButton("##HistoryPlot",size); auto* draw=ImGui::GetWindowDrawList();
    draw->AddRectFilled(origin,{origin.x+size.x,origin.y+size.y},IM_COL32(18,25,34,255));
    for(int i=0;i<=4;++i) draw->AddLine({origin.x,origin.y+size.y*i/4.f},{origin.x+size.x,origin.y+size.y*i/4.f},IM_COL32(90,105,130,45));
    std::optional<ImVec2> previous; D::LiveId revision=0; uint64_t previousEnd=0; bool previousMixed=false;
    for(const auto& bucket:statistics.history)
    {
        if(bucket.endNs<begin) continue;
        const float x=origin.x+float(double(bucket.endNs-begin)/double(D::LiveHistoryWindowNs))*size.x;
        if(previousEnd && (revision!=bucket.appliedRevision || bucket.mixedRevision)) draw->AddLine({x,origin.y},{x,origin.y+size.y},IM_COL32(255,175,76,180));
        const auto value=point(bucket);
        if(value)
        {
            const ImVec2 next{x,origin.y+size.y*(1.f-float(*value/maximum))};
            if(previous && previousEnd==bucket.beginNs && revision==bucket.appliedRevision && !previousMixed && !bucket.mixedRevision)
                draw->AddLine(*previous,next,IM_COL32(56,194,191,255),2);
            draw->AddCircleFilled(next,4,IM_COL32(56,194,191,255)); previous=next;
        }
        else previous.reset();
        revision=bucket.appliedRevision; previousEnd=bucket.endNs; previousMixed=bucket.mixedRevision;
        if(ImGui::IsItemHovered() && std::abs(ImGui::GetIO().MousePos.x-x)<8)
            ImGui::SetTooltip("완료 %.1f초 구간%s · 처리 %s개 · %s\nCPU 측정 %llu 대기 %llu / GPU 측정 %llu 대기 %llu",
                (bucket.endNs-bucket.beginNs)/1e9,bucket.mixedRevision ? " (설정 전환 포함)" : "",DisplayAmount(double(bucket.frameCount),0).c_str(),value ? std::to_string(*value).c_str() : "미수집",
                bucket.cpuRender.ready,bucket.cpuRender.pending,bucket.gpuMeasured.ready,bucket.gpuMeasured.pending);
    }
    ImGui::TextDisabled("-10초                          생산자 원본 시간 / 누락·미측정 구간은 연결하지 않음");
}

void Viewer::Requests()
{
    if(state.requests.empty()) ImGui::TextDisabled("요청 없음");
    for(auto it=state.requests.rbegin();it!=state.requests.rend();++it)
    {
        ImGui::TextWrapped("%s / 요청 %s / 실제 %s / %s",ControlState(it->state),ValueText(it->requested).c_str(),ValueText(it->actual).c_str(),it->message.c_str());
        if(ImGui::IsItemHovered()) ImGui::SetTooltip("프로그램이 보고한 실제 적용 상태입니다. 요청값과 실제 결과를 구분합니다.");
    }
    if(state.omittedCompletedRequests) ImGui::TextDisabled("이전 완료 요청 상세 일부 생략");
}
void Viewer::Draw()
{
    statisticsDrawn=flowDrawn=false; comparisonDrawn.reset(); observationStatisticsDrawn.reset(); observationFlowDrawn.reset(); Dock();
    Controls(); Overview(); Flow();
    LaunchDialog();
}
void Viewer::Presented(uint64_t nowNs)
{
    presentationLog.Presented(statistics,playback,statisticsDrawn,flowDrawn,nowNs);
    presentationLog.PresentedComparison(comparisonDrawn ? &*comparisonDrawn : nullptr,nowNs);
    presentationLog.PresentedObservations(observationStatisticsDrawn ? &*observationStatisticsDrawn : nullptr,
        observationFlowDrawn ? &*observationFlowDrawn : nullptr,nowNs);
}
bool Viewer::WriteReport(const std::filesystem::path& path,uint64_t frames) const
{
    std::ofstream output(path); if(!output) return false;
    output<<"native_presented_frames="<<frames<<"\nsnapshots_received="<<receivedSnapshots<<"\nclient_state="<<int(client.GetState())<<'\n';
    if(const auto* source=state.Source())
        output<<"received_device="<<state.device<<"\nreceived_source="<<state.source<<"\nreceived_frame_id="<<source->latest.key.frame
            <<"\nreceived_revision="<<source->latest.appliedRevision<<"\nreceived_draw="<<source->latest.recorded.draw<<"\nreceived_draw_indexed="<<source->latest.recorded.drawIndexed
            <<"\nreceived_upload_bytes="<<source->latest.recorded.uploadBytes<<"\nqpc_clock_matches="<<state.ExactClock(D::GetLiveClockInfo())<<'\n';
    presentationLog.Write(output);
    return output.good();
}
}
