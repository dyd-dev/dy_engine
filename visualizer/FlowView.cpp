#include "Viewer.h"
#include <imgui.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

namespace viewer
{
static constexpr const char* names[]{"CPU 준비","데이터·배치","업로드","명령 기록","네이티브 변환","제출","GPU","출력","참조 자원"};
static constexpr ImU32 ink=IM_COL32(228,239,246,255),muted=IM_COL32(145,166,187,255),teal=IM_COL32(56,194,191,255);
static bool Ready(const D::LiveMetricAggregate& value)
{ return value.ready && value.state==D::LiveValueState::Ready && std::isfinite(value.average); }
static void TextAt(ImDrawList* draw,ImVec2 point,float size,ImU32 color,const char* text,bool centered=false)
{
    const float width=FlowRouteLayout::Spacing-8;
    const auto center=point;
    if(centered) point.x-=std::min(width,ImGui::GetFont()->CalcTextSizeA(size,std::numeric_limits<float>::max(),0,text).x)*.5f;
    const ImVec4 clip{center.x-width*.5f,point.y,center.x+width*.5f,point.y+size+2};
    draw->AddText(ImGui::GetFont(),size,point,color,text,nullptr,0,centered ? &clip : nullptr);
}
static std::string MetricText(double value,int decimals)
{
    return DisplayAmount(value,decimals);
}
static bool MetricCard(const char* name,double mean,D::LiveValueState state,const D::LiveMetricRange& range,int decimals,const char* footer=nullptr)
{
    const auto origin=ImGui::GetCursorScreenPos(); const ImVec2 size{std::max(1.f,ImGui::GetContentRegionAvail().x),82};
    ImGui::InvisibleButton(name,size); const bool visible=ImGui::IsItemVisible(); auto* draw=ImGui::GetWindowDrawList();
    draw->AddRectFilled(origin,{origin.x+size.x,origin.y+size.y},IM_COL32(23,35,48,255),5);
    TextAt(draw,{origin.x+6,origin.y+3},22,muted,name);
    char text[160];
    if(state==D::LiveValueState::Ready && std::isfinite(mean)) std::snprintf(text,sizeof(text),"%s",MetricText(mean,decimals).c_str());
    else std::snprintf(text,sizeof(text),"-- (%s)",ValueState(state));
    TextAt(draw,{origin.x+6,origin.y+26},30,ink,text);
    if(footer) std::snprintf(text,sizeof(text),"%s",footer);
    else if(range.state==D::LiveValueState::Ready && range.windows && std::isfinite(range.low) && std::isfinite(range.high))
        std::snprintf(text,sizeof(text),"Low %s   High %s",MetricText(range.low,decimals).c_str(),MetricText(range.high,decimals).c_str());
    else std::snprintf(text,sizeof(text),"Low --   High --");
    TextAt(draw,{origin.x+6,origin.y+59},20,muted,text);
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("완료된 원본 구간의 값입니다. Low / High는 해당 지표의 완료 1초 평균 최저·최고이며 개별 프레임 극값이 아닙니다.");
    return visible;
}
void Viewer::Overview()
{
    if(!ImGui::Begin("Overview / 현재 상태")) { ImGui::End(); return; }
    const auto* source=statistics.state.Source();
    ImGui::TextWrapped("%s · 1초 평균",UpdateStatus());
    if(ImGui::IsItemHovered())
    {
        ImGui::BeginTooltip(); ImGui::PushTextWrapPos(ImGui::GetCursorPosX()+450.f);
        ImGui::TextWrapped("선택 source의 완료된 1초 평균이며 초당 갱신합니다. 처리 구간/초는 실제 Present FPS와 다릅니다. 설정 변경이 섞이면 혼합 구간으로 표시합니다.");
        ImGui::TextWrapped("BeginFrame 전에 준비한 명령은 기록·제출·완료 source가 분리될 수 있습니다. 기록 Draw가 0이면 제출·완료 열과 연결·실행의 Source를 함께 확인합니다.");
        ImGui::TextWrapped("raw RHI에 의미 객체·배치 계측이 없을 수 있습니다. 관측 0만으로 배칭 OFF나 장면 객체 0을 판정하지 않습니다.");
        if(source && Ready(source->recent.frame)) ImGui::TextWrapped("평균 시간의 역수: %s/초 · Low %s / High %s. 실제 Present FPS와 구분합니다.",MetricText(source->recent.fps,1).c_str(),source->runRanges.fps.windows ? MetricText(source->runRanges.fps.low,1).c_str() : "--",source->runRanges.fps.windows ? MetricText(source->runRanges.fps.high,1).c_str() : "--");
        ImGui::PopTextWrapPos(); ImGui::EndTooltip();
    }
    if(source && source->recent.mixedRevision) { ImGui::SameLine(); ImGui::TextDisabled("설정 전환 포함"); }
    const D::LiveAggregate empty; const D::LiveRunRanges emptyRanges;
    const auto& a=source ? source->recent : empty; const auto& ranges=source ? source->runRanges : emptyRanges;
    const auto rate=WindowRate(a);
    if(rate) { ImGui::TextWrapped("최근 1초 처리 %s개 · %s구간/초",DisplayAmount(double(a.frameCount),0).c_str(),DisplayAmount(*rate,1).c_str()); if(ImGui::IsItemVisible()) statisticsDrawn=true; }
    else ImGui::TextDisabled("최근 1초 처리량 · 완료 구간 대기");
    const auto counter=a.frameCount ? D::LiveValueState::Ready : D::LiveValueState::Unavailable;
    const auto metricState=[](const auto& value){ return Ready(value) ? D::LiveValueState::Ready : value.state==D::LiveValueState::Ready ? D::LiveValueState::Unavailable : value.state; };
    if(ImGui::BeginChild("##StatisticCards",{0,0}))
    {
        const int columns=std::clamp(int(ImGui::GetContentRegionAvail().x/220),1,5);
        const auto width=ImGui::GetContentRegionAvail().x;
        if(ImGui::BeginTable("##Metrics",columns,ImGuiTableFlags_SizingStretchSame,{width,0}))
        {
            const auto card=[&](const char* title,double value,D::LiveValueState availability,const auto& range,int decimals) {
                ImGui::TableNextColumn(); if(MetricCard(title,value,availability,range,decimals) && source) statisticsDrawn=true;
            };
            ImGui::TableNextColumn();
            if(MetricCard("초당 처리 구간",rate.value_or(0),rate ? D::LiveValueState::Ready : D::LiveValueState::Unavailable,{},1,"완료 구간 수 / 구간 길이") && source) statisticsDrawn=true;
            card("평균 구간 (ms)",a.frame.average,metricState(a.frame),ranges.frame,3);
            card("평균 CPU (ms)",a.cpuRender.average,metricState(a.cpuRender),ranges.cpuRender,3);
            card("평균 GPU (ms)",a.gpuMeasured.average,metricState(a.gpuMeasured),ranges.gpuMeasured,3);
            card("기록 Draw / 구간",a.recordedMean.draw+a.recordedMean.drawIndexed,counter,ranges.drawCalls,1);
            card("인스턴스 / 구간",a.recordedMean.instances,counter,ranges.instances,1);
            card("기록 배치 / 구간",a.recordedMean.batches,counter,ranges.batches,1);
            card("Upload / 구간 (B)",a.recordedMean.uploadBytes,counter,ranges.uploadBytes,1);
            card("생존 의미 객체",a.objects.average,metricState(a.objects),ranges.objects,1);
            ImGui::EndTable();
        }
        ObservationStatistics();
    }
    ImGui::EndChild(); ImGui::End();
}
void Viewer::ObservationStatistics()
{
    observationStatisticsDrawn=ObserveStatistics(statistics);
    if(!observationStatisticsDrawn) { ImGui::TextDisabled("RHI 관측 · 완료된 1초 구간 대기"); return; }
    auto& shown=*observationStatisticsDrawn; const auto& a=shown.values.average;
    const float width=ImGui::GetContentRegionAvail().x;
    const bool countersOpen=ImGui::CollapsingHeader("RHI 작업 · 1초 평균",ImGuiTreeNodeFlags_DefaultOpen);
    if(ImGui::IsItemHovered()) { ImGui::BeginTooltip(); ImGui::PushTextWrapPos(ImGui::GetCursorPosX()+450.f); ImGui::TextUnformatted("조절 항목·적용 요청과 무관한 생산자 집계입니다. 기록·제출·완료를 구분하며 원본 표본의 이벤트 수를 평균에 섞지 않습니다. 0은 구간 관측 수이며 기능 미지원·비활성 판정이 아닙니다. 페이지 할당·재사용은 기록 전용이고 장치 풀의 누적 통계와 다릅니다."); ImGui::PopTextWrapPos(); ImGui::EndTooltip(); }
    if(countersOpen)
    {
        if(ImGui::BeginTable("##RhiCounters",4,ImGuiTableFlags_RowBg|ImGuiTableFlags_BordersInnerV|ImGuiTableFlags_SizingStretchProp|ImGuiTableFlags_ScrollY,{width,TableHeight(ComparisonCounterCount)}))
        {
            for(const auto* label:{"작업 / 구간","기록한 작업","제출한 작업","완료한 작업"}) ImGui::TableSetupColumn(label);
            ImGui::TableSetupScrollFreeze(0,1); ImGui::TableHeadersRow();
            for(size_t i=0;i<ComparisonCounters().size();++i)
            {
                const auto& field=ComparisonCounters()[i]; ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextWrapped("%s",field.label);
                for(size_t phase=0;phase<3;++phase)
                {
                    ImGui::TableNextColumn(); const auto value=CounterNumber(a,CounterPhase(phase),field);
                    if(value) ImGui::TextUnformatted(MetricText(*value,1).c_str());
                    else ImGui::TextDisabled("%s",field.recordedOnly && phase ? "-- / 기록 전용" : "-- / 미수집");
                    shown.counterVisibility[phase][i]=ImGui::IsItemVisible();
                }
            }
            ImGui::EndTable();
        }
    }
    const bool stagesOpen=ImGui::CollapsingHeader("단계 비용·작업량",ImGuiTreeNodeFlags_DefaultOpen);
    if(ImGui::IsItemHovered()) { ImGui::BeginTooltip(); ImGui::PushTextWrapPos(ImGui::GetCursorPosX()+450.f); ImGui::TextUnformatted("CPU는 측정된 scope 평균이며 단계 합계는 wall time이 아닙니다. 부분·대기 GPU는 --입니다. GPU 미지원은 타이밍 측정 상태이며 그리기·컴퓨트 기능의 지원 여부가 아닙니다."); ImGui::PopTextWrapPos(); ImGui::EndTooltip(); }
    if(stagesOpen)
    {
        if(ImGui::BeginTable("##RhiStages",6,ImGuiTableFlags_RowBg|ImGuiTableFlags_BordersInnerV|ImGuiTableFlags_SizingStretchProp|ImGuiTableFlags_ScrollY,{width,TableHeight(D::LiveStageCount)}))
        {
            for(const auto* label:{"단계","CPU ms","GPU ms","작업 / 구간","요소 / 구간","B / 구간"}) ImGui::TableSetupColumn(label);
            ImGui::TableSetupScrollFreeze(0,1); ImGui::TableHeadersRow();
            for(size_t stage=0;stage<D::LiveStageCount;++stage)
            {
                ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextWrapped("%s",StageName(D::LiveStage(stage)));
                for(size_t metric=0;metric<size_t(StageMetric::Count);++metric)
                {
                    ImGui::TableNextColumn(); const auto value=StageNumber(a,D::LiveStage(stage),StageMetric(metric));
                    if(value) ImGui::TextUnformatted(MetricText(*value,metric<2 ? 3 : 1).c_str()); else ImGui::TextDisabled("--");
                    shown.stageVisibility[stage][metric]=ImGui::IsItemVisible();
                    if(metric<2)
                    {
                        const auto& timing=metric==0 ? a.stages[stage].cpu : a.stages[stage].gpu;
                        ImGui::TextWrapped("%s · 측정 %llu/%llu",ValueState(timing.state),timing.ready,a.frameCount);
                        if(ImGui::IsItemHovered()) ImGui::SetTooltip("대기 %llu / 타이밍 미지원 %llu / 미수집 %llu",timing.pending,timing.unsupported,timing.unavailable);
                    }
                }
            }
            ImGui::EndTable();
        }
    }
}
void Viewer::ObservationFlow()
{
    const bool expanded=ImGui::CollapsingHeader("원본 명령·이벤트");
    if(ImGui::IsItemHovered()) { ImGui::BeginTooltip(); ImGui::PushTextWrapPos(ImGui::GetCursorPosX()+450.f); ImGui::TextUnformatted("보존된 이벤트 관측이며 1초 평균·기능 ON/OFF와 다릅니다. 없거나 유실된 명령을 미사용으로 판정하지 않습니다. 이벤트를 선택하면 실제 인자·자원·연결을 확인할 수 있습니다. readback·타임스탬프 등 실제 label도 유지합니다."); ImGui::PopTextWrapPos(); ImGui::EndTooltip(); }
    if(!expanded) return;
    const auto* source=playback.Source(); if(!source || !playback.snapshot) { ImGui::TextDisabled("원본 프레임 대기"); return; }
    if(!observedFrame || observedFrameGeneration!=playback.generation || observedFrame->values.session!=playback.snapshot->session
        || observedFrame->values.frame!=playback.frame)
    { observedFrame=ObserveReplay(playback); observedFrameGeneration=playback.generation; }
    if(!observedFrame) return;
    observationFlowDrawn=*observedFrame; auto& shown=*observationFlowDrawn;
    shown.values.cycleStartedNs=playback.startedNs;
    if(ImGui::BeginChild("##RhiOriginalObservation",{0,TableHeight(9,9)},true))
    {
        ImGui::TextUnformatted("고정 원본 표본");
        shown.headerVisible=ImGui::IsItemVisible();
        if(shown.incomplete || shown.omittedEvents || shown.omittedLinks || shown.loss.transferTruncated)
            ImGui::TextColored({1,.69f,.3f,1},"일부 상세 생략 · 이벤트 %llu / 연결 %llu",shown.omittedEvents,shown.omittedLinks);
        if(ImGui::BeginTable("##RhiObservedCommands",3,ImGuiTableFlags_RowBg|ImGuiTableFlags_BordersInnerV,{0,0}))
        {
            for(const auto* label:{"관측 명령 종류","이 원본의 관측 개수","실제 결과"}) ImGui::TableSetupColumn(label);
            ImGui::TableHeadersRow();
            for(size_t i=0;i<shown.observed.commandKinds.size();++i)
            {
                ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextUnformatted(i ? CommandKind(D::LiveCommandKind(i)) : "기타 상태·마커 명령");
                ImGui::TableNextColumn(); ImGui::Text("%llu",shown.observed.commandKinds[i]); shown.commandVisibility[i]=ImGui::IsItemVisible();
                ImGui::TableNextColumn();
                if(i==size_t(D::LiveCommandKind::Present))
                {
                    for(size_t outcome=0;outcome<shown.observed.presentOutcomes.size();++outcome) if(const auto count=shown.observed.presentOutcomes[outcome])
                    { ImGui::Text("%s %llu",Outcome(D::LiveOutcome(outcome)),count); shown.presentOutcomeVisibility[outcome]=ImGui::IsItemVisible(); }
                }
            }
            ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextWrapped("다중 인스턴스 Draw 기록 (instanceCount > 1)");
            ImGui::TableNextColumn(); ImGui::Text("%llu",shown.observed.multiInstanceDraws); shown.multiInstanceVisible=ImGui::IsItemVisible();
            ImGui::TableNextColumn(); ImGui::TextWrapped("최적화 ON·성능 향상 판정 아님");
            ImGui::EndTable();
        }
        if(ImGui::BeginTable("##RhiObservedEvents",4,ImGuiTableFlags_RowBg|ImGuiTableFlags_BordersInnerV|ImGuiTableFlags_ScrollY,{0,TableHeight(source->flow.events.size())}))
        {
            for(const auto* label:{"종류","실제 label","실제 결과","원본 인자"}) ImGui::TableSetupColumn(label);
            ImGui::TableSetupScrollFreeze(0,1); ImGui::TableHeadersRow(); ImGuiListClipper clipper; clipper.Begin(int(source->flow.events.size()));
            while(clipper.Step()) for(int row=clipper.DisplayStart;row<clipper.DisplayEnd;++row)
            {
                const auto& event=source->flow.events[size_t(row)]; unsigned char visible=0;
                ImGui::PushID(reinterpret_cast<void*>(uintptr_t(event.id))); ImGui::TableNextRow(); ImGui::TableNextColumn();
                const auto label=std::string(EventKind(event.kind))+" / "+CommandKind(event.commandKind);
                if(ImGui::Selectable(label.c_str()))
                {
                    SelectTrace(SelectionKind::Event,event.id); revealEvents=true; onlyRelated=false;
                    selectedStage=size_t(event.stage)<D::LiveStageCount ? int(event.stage) : 3;
                }
                if(ImGui::IsItemVisible()) visible|=1;
                ImGui::TableNextColumn(); ImGui::TextUnformatted(event.label,std::find(std::begin(event.label),std::end(event.label),'\0')); if(ImGui::IsItemVisible()) visible|=2;
                if(ImGui::IsItemHovered()) { ImGui::BeginTooltip(); ImGui::TextUnformatted(event.label,std::find(std::begin(event.label),std::end(event.label),'\0')); ImGui::EndTooltip(); }
                ImGui::TableNextColumn(); ImGui::TextUnformatted(Outcome(event.outcome)); if(ImGui::IsItemVisible()) visible|=4;
                const auto description=DescribeEvent(event);
                ImGui::TableNextColumn(); ImGui::TextUnformatted(description.arguments.c_str());
                if(ImGui::IsItemVisible()) visible|=8;
                if(ImGui::IsItemHovered()) ImGui::SetTooltip("%s",description.arguments.c_str());
                if(visible) shown.events.push_back({event,visible});
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
    }
    ImGui::EndChild();
}
void Viewer::SelectTrace(SelectionKind kind,D::LiveId id)
{
    if(!playback.snapshot) return;
    state.selection={kind,id,playback.frame,playback.snapshot,playback.startedNs};
    indexedSequence=UINT64_MAX; revealDetails=true;
}
void Viewer::Flow()
{
    if(!ImGui::Begin("Flow / 데이터 흐름")) { ImGui::End(); return; }
    ImGui::PushFont(nullptr,24); ImGui::TextUnformatted("3초 표본 재생"); ImGui::PopFont();
    if(ImGui::IsItemHovered() && playback.snapshot)
    { ImGui::BeginTooltip(); ImGui::PushTextWrapPos(ImGui::GetCursorPosX()+450.f); ImGui::TextUnformatted("같은 원본 표본과 실제 연결을 3초간 재생합니다. 현재 GPU의 실시간 실행 위치가 아닙니다. 원은 원본 명령 정보이며 반복 묶음은 대표 명령을 표시합니다."); ImGui::PopTextWrapPos(); ImGui::EndTooltip(); }
    ObservationFlow();
    const float right=ImGui::GetCursorScreenPos().x+ImGui::GetContentRegionAvail().x;
    ImGui::Checkbox("참조·의존 경로 펼치기",&showReferenceRoutes);
    if(ImGui::GetItemRectMax().x+ImGui::CalcTextSize("원본 항목").x+ImGui::GetFrameHeight()+ImGui::GetStyle().ItemSpacing.x+ImGui::GetStyle().ItemInnerSpacing.x<=right) ImGui::SameLine();
    ImGui::Checkbox("원본 항목",&expandGraph);
    if(state.selection.id)
    {
        if(ImGui::SmallButton("선택 항목의 연결 범위")) { graphFocus=state.selection.id; graphSequence=UINT64_MAX; }
        if(ImGui::GetItemRectMax().x+ImGui::CalcTextSize("범위 해제").x+ImGui::GetStyle().FramePadding.x*2+ImGui::GetStyle().ItemSpacing.x<=right) ImGui::SameLine();
        if(ImGui::SmallButton("범위 해제")) { graphFocus=0; graphSequence=UINT64_MAX; }
    }
    if(const auto* source=playback.Source())
    {
        if(source->latest.incomplete || source->flow.omittedEvents || source->flow.omittedLinks || playback.snapshot->loss.transferTruncated)
            ImGui::TextColored({1,.69f,.3f,1},"일부 상세 생략 · 보존된 실제 연결만 표시");
        Graph();
    }
    else ImGui::TextDisabled("대기 · 같은 frame/revision의 실제 흐름 미수집");
    const auto section=[&](const char* title,void (Viewer::*contents)(),bool* reveal=nullptr) {
        if(reveal && *reveal) { ImGui::SetNextItemOpen(true); *reveal=false; }
        if(ImGui::CollapsingHeader(title))
        {
            ImGui::PushID(title);
            if(ImGui::BeginChild("##InlineDiagnostic",{0,TableHeight(8,8)},true)) (this->*contents)();
            ImGui::EndChild(); ImGui::PopID();
        }
    };
    section("선택 원본 상세",&Viewer::Details,&revealDetails);
    section("원본 명령",&Viewer::Events,&revealEvents);
    section("생존 객체·자원",&Viewer::Inventory,&revealInventory);
    section("GPU 원본 측정",&Viewer::Gpu);
    section("최근 10초",&Viewer::History);
    ImGui::End();
}
void Viewer::Graph()
{
    const auto* source=playback.Source(); if(!source) return;
    if(graphSequence!=playback.generation)
    {
        graph=BuildFlowGraph(*playback.snapshot,*source,graphFocus); graphSequence=playback.generation;
        flowRoutes=BuildFlowRoutes(graph,*source); layoutRoute=SIZE_MAX;
        selectedRoute=std::min(selectedRoute,flowRoutes.empty() ? size_t(0) : flowRoutes.size()-1);
    }
    std::vector<size_t> options;
    for(size_t i=0;i<flowRoutes.size();++i) if(flowRoutes[i].recording || showReferenceRoutes) options.push_back(i);
    if(options.empty())
    {
        ImGui::TextDisabled("기록 경로 없음 · 참조 경로를 펼쳐 원본 관계를 확인하세요.");
        return;
    }
    if(std::find(options.begin(),options.end(),selectedRoute)==options.end()) { selectedRoute=options.front(); layoutRoute=SIZE_MAX; }
    ImGui::SetNextItemWidth(std::min(420.f,std::max(200.f,ImGui::GetContentRegionAvail().x)));
    const auto title=std::string(flowRoutes[selectedRoute].recording ? "기록 경로 " : "참조 경로 ")+std::to_string(selectedRoute+1);
    if(ImGui::BeginCombo("경로",title.c_str()))
    {
        for(const auto i:options)
        {
            const auto& route=flowRoutes[i];
            const auto label=std::string(route.recording ? "기록 " : "참조 ")+std::to_string(i+1)+" · "+std::to_string(route.links.size())+"개 원본 연결";
            if(ImGui::Selectable(label.c_str(),selectedRoute==i)) { selectedRoute=i; layoutRoute=SIZE_MAX; playback.startedNs=D::LiveNowNs(); }
            if(ImGui::IsItemHovered()) ImGui::SetTooltip("보존된 실제 연결 경로입니다. 경로 목록의 순서가 전체 실행 순서를 뜻하지 않습니다.");
        }
        ImGui::EndCombo();
    }
    const auto& route=flowRoutes[selectedRoute];
    if(layoutRoute!=selectedRoute) { routeLayout=LayoutFlowRoute(route); layoutRoute=selectedRoute; }
    if(routeLayout.nodes.empty()) { ImGui::TextDisabled("연속된 원본 경로 미수집"); return; }
    if(!route.recording) ImGui::TextDisabled("참조 관계 · 실행 순서 아님");
    const float childHeight=routeLayout.height+ImGui::GetStyle().WindowPadding.y*2+ImGui::GetStyle().ScrollbarSize;
    if(ImGui::BeginChild("##RecordedFlow",{0,childHeight},false,ImGuiWindowFlags_HorizontalScrollbar))
    {
        const auto origin=ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##FlowCanvas",{routeLayout.width,routeLayout.height});
        const bool hovered=ImGui::IsItemHovered(); flowDrawn=ImGui::IsItemVisible(); auto* draw=ImGui::GetWindowDrawList();
        const auto point=[&](FlowPoint value){ return ImVec2{origin.x+value.x,origin.y+value.y}; };
        const auto count=routeLayout.nodes.size();
        const size_t first=size_t(std::max(0,int(ImGui::GetScrollX()/routeLayout.Spacing)-2));
        const size_t last=std::min(count,size_t(std::max(0,int((ImGui::GetScrollX()+ImGui::GetWindowWidth())/routeLayout.Spacing)+3)));
        for(size_t i=first;i<last && i<routeLayout.paths.size();++i)
        {
            const auto& path=routeLayout.paths[i]; const auto color=route.recording ? teal : IM_COL32(142,175,214,255);
            for(size_t j=1;j<path.size();++j) draw->AddLine(point(path[j-1]),point(path[j]),color,3);
            auto tip=routeLayout.nodes[i+1]; ImVec2 direction{1,0};
            if(path.size()==2)
            {
                tip.y=path.back().y;
                const float offset=tip.y-routeLayout.nodes[i+1].y;
                tip.x-=std::sqrt(routeLayout.Radius*routeLayout.Radius-offset*offset);
            }
            else { const float port=routeLayout.Radius*.5f; tip.x-=port; tip.y+=std::sqrt(routeLayout.Radius*routeLayout.Radius-port*port); direction={0,-1}; }
            const auto end=point(tip);
            draw->AddTriangleFilled(end,{end.x-direction.x*12-direction.y*6,end.y-direction.y*12+direction.x*6},
                {end.x-direction.x*12+direction.y*6,end.y-direction.y*12-direction.x*6},color);
        }
        for(size_t i=first;i<last;++i)
        {
            const int column=i ? route.steps[i-1].to : route.steps.front().from;
            const auto center=point(routeLayout.nodes[i]);
            draw->AddCircleFilled(center,routeLayout.Radius,IM_COL32(24,47,61,255));
            draw->AddCircle(center,routeLayout.Radius,column==selectedStage ? IM_COL32(255,211,112,255) : teal,64,3);
            const auto* event=FlowEvent(graph,source->flow,routeLayout.ids[i]);
            auto description=event ? DescribeEvent(*event) : EventText{names[column],{},"참조 정보",{},"--","미수집"};
            if(!event && column==int(D::LiveStage::Gpu))
            {
                const auto samples=GpuSamples(*source);
                const auto span=std::find_if(samples.begin(),samples.end(),[&](const auto* value){return value->id==routeLayout.ids[i] && value->frame==playback.frame && value->appliedRevision==playback.revision;});
                if(span!=samples.end())
                {
                    const auto& duration=(*span)->duration; description.name="GPU 표본";
                    description.unit=duration.state==D::LiveValueState::Pending ? "대기" : duration.state==D::LiveValueState::Unsupported ? "미지원" : "미수집";
                    if(duration.state==D::LiveValueState::Ready && std::isfinite(duration.milliseconds)) { description.value=DisplayAmount(duration.milliseconds,3); description.unit="GPU ms"; }
                }
            }
            TextAt(draw,{center.x,origin.y+2},18,ink,description.name.c_str(),true);
            if((i ? route.steps[i-1] : route.steps.front()).links>1) TextAt(draw,{center.x,origin.y+24},18,muted,"대표 명령",true);
            TextAt(draw,{center.x,center.y-22},22,ink,description.value.c_str(),true);
            TextAt(draw,{center.x,center.y+2},18,muted,description.unit.c_str(),true);
            if(hovered && std::hypot(ImGui::GetIO().MousePos.x-center.x,ImGui::GetIO().MousePos.y-center.y)<routeLayout.Radius)
            {
                ImGui::BeginTooltip();
                if(event) EventSummary(*event,"원본 명령 표본"); else ImGui::TextWrapped("%s · 명령 상세 미수집 또는 참조 관계",names[column]);
                ImGui::EndTooltip();
                if(ImGui::IsMouseClicked(ImGuiMouseButton_Left))
                {
                    if(column<FlowReferenceColumn) { selectedStage=column; revealEvents=true; indexedSequence=UINT64_MAX; }
                    else revealInventory=true;
                    const auto node=std::find_if(graph.nodes.begin(),graph.nodes.end(),[&](const auto& value){return value.id==routeLayout.ids[i];});
                    if(node!=graph.nodes.end() && node->kind!=FlowNodeKind::Unknown)
                        SelectTrace(node->kind==FlowNodeKind::Gpu ? SelectionKind::Gpu : node->kind==FlowNodeKind::Object ? SelectionKind::Object : SelectionKind::Event,
                            node->kind==FlowNodeKind::BatchReference || node->kind==FlowNodeKind::SubmissionReference ? source->flow.events[node->index].id : node->id);
                }
            }
        }
        const auto at=LocateFlowRoute(route,playback.Phase(D::LiveNowNs()));
        const auto dot=PointOnFlowPath(routeLayout.paths[at.step],at.phase);
        draw->AddCircleFilled(point(dot),4,IM_COL32(255,211,112,255));
    }
    ImGui::EndChild();
    const auto current=LocateFlowRoute(route,playback.Phase(D::LiveNowNs()));
    const D::LiveEvent* representative=state.selection.source==playback.frame ? state.SelectedEvent() : nullptr;
    const bool selected=representative!=nullptr;
    if(!representative) representative=FlowEvent(graph,source->flow,routeLayout.ids[current.step+1]);
    if(!representative) representative=FlowEvent(graph,source->flow,routeLayout.ids[current.step]);
    if(representative)
    {
        const auto caption=selected ? std::string("선택 원본 명령") : "대표 명령 · 묶음 연결 "+std::to_string(route.steps[current.step].links)+"개";
        EventSummary(*representative,caption.c_str());
    }
    else ImGui::TextWrapped("원본 참조 구간 · 실제 명령 상세 미수집");
    if(expandGraph)
    {
        if(ImGui::BeginChild("##FlowIDs",{0,TableHeight(graph.nodes.size())},true))
        {
            ImGui::TextDisabled("보존된 원본 항목 · 선택하면 명령·자원 상세 확인");
            ImGuiListClipper clipper; clipper.Begin(int(graph.nodes.size()));
            while(clipper.Step()) for(int row=clipper.DisplayStart;row<clipper.DisplayEnd;++row)
            {
                const auto& node=graph.nodes[size_t(row)];
                const auto* event=FlowEvent(graph,source->flow,node.id);
                const auto label=event ? DescribeEvent(*event).name : std::string(names[node.column]);
                ImGui::PushID(reinterpret_cast<void*>(uintptr_t(node.id)));
                if(ImGui::Selectable(label.c_str(),state.selection.id==node.id))
                {
                    if(node.kind==FlowNodeKind::Event) SelectTrace(SelectionKind::Event,node.id);
                    else if(node.kind==FlowNodeKind::Gpu) SelectTrace(SelectionKind::Gpu,node.id);
                    else if(node.kind==FlowNodeKind::Object) SelectTrace(SelectionKind::Object,node.id);
                    else if(node.kind==FlowNodeKind::BatchReference || node.kind==FlowNodeKind::SubmissionReference) SelectTrace(SelectionKind::Event,source->flow.events[node.index].id);
                }
                if(ImGui::IsItemHovered())
                {
                    ImGui::BeginTooltip();
                    if(event) EventSummary(*event,"원본 항목"); else ImGui::TextWrapped("%s · 보존된 참조 관계",names[node.column]);
                    ImGui::EndTooltip();
                }
                ImGui::PopID();
            }
        }
        ImGui::EndChild();
    }
}
}
