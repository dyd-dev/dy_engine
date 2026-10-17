#include "Viewer.h"
#include <imgui.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

namespace viewer
{
static constexpr const char* stages[]{"CPU 준비","데이터·배치","업로드","명령 기록","네이티브 변환","제출·대기","GPU","출력"};
static void ComparisonColumns(const char* first)
{
    ImGui::TableSetupColumn(first,ImGuiTableColumnFlags_WidthStretch,1.4f);
    for(const auto* label:{"이전 1초","이후 1초","변화량","변화율 (%)"}) ImGui::TableSetupColumn(label);
    ImGui::TableSetupScrollFreeze(0,1); ImGui::TableHeadersRow();
}
static unsigned char NumberCells(const NumberChange& change,int decimals)
{
    unsigned char visible=0;
    const std::optional<double> values[]{change.before,change.after,change.delta,change.percent};
    for(size_t i=0;i<4;++i)
    {
        if(!ImGui::TableNextColumn()) continue;
        if(values[i])
        {
            char text[64];
            const bool large=std::abs(*values[i])>=100000;
            if(large) { const auto formatted=DisplayAmount(*values[i],decimals); std::snprintf(text,sizeof(text),"%s%s",i>=2 && *values[i]>0 ? "+" : "",formatted.c_str()); }
            else std::snprintf(text,sizeof(text),i>=2 ? "%+.*f" : "%.*f",i==3 ? 1 : decimals,*values[i]);
            if(ImGui::CalcTextSize(text).x>ImGui::GetContentRegionAvail().x) std::snprintf(text,sizeof(text),i>=2 ? "%+.2g" : "%.2g",*values[i]);
            ImGui::TextUnformatted(text);
            if(ImGui::IsItemHovered()) ImGui::SetTooltip("%.17g",*values[i]);
        }
        else ImGui::TextDisabled("--");
        if(ImGui::IsItemVisible()) visible|=static_cast<unsigned char>(1u<<i);
    }
    return visible;
}
void Viewer::ControlComparison()
{
    const auto* applied=state.Comparison();
    if(!applied || applied->control!=selectedControl)
    {
        ImGui::TextDisabled("적용 후 전후 비교 표시");
        return;
    }
    ComparisonPresentation shown; shown.status=*applied; shown.status.baseline={}; shown.status.after={};
    if(const auto* current=state.Source()) shown.currentRevision=current->latest.appliedRevision;
    if(state.snapshot) { shown.session=state.snapshot->session; shown.sequence=state.snapshot->sequence; }
    if(statistics.state.snapshot && state.snapshot && statistics.state.snapshot->session==state.snapshot->session)
        for(const auto& value:statistics.state.requests) if(value.request==applied->request && value.state==D::ControlState::Applied)
        {
            shown.status=value; shown.sequence=statistics.state.snapshot->sequence;
            const auto now=statistics.state.snapshot->producerTimeNs;
            shown.beforeReady=CompleteComparisonWindow(value.baseline,state.device,state.source,now)
                && value.baseline.appliedRevision<=value.appliedRevision && value.baseline.endNs<=value.appliedNs;
            shown.afterReady=ComparisonAfterReady(state,value,now);
            break;
        }
    ImGui::Separator(); ImGui::PushFont(nullptr,24); ImGui::TextUnformatted("전후 비교"); ImGui::PopFont();
    if(ImGui::IsItemHovered()) { ImGui::BeginTooltip(); ImGui::PushTextWrapPos(ImGui::GetCursorPosX()+450.f); ImGui::TextUnformatted("실제 적용 전후의 완료된 1초 구간입니다. 동일 부하·단일 설정 변경을 검증한 성능 판정은 아니므로 수치 증감만으로 효과를 단정하지 않습니다. CPU는 측정 scope 평균이며 중첩·병렬 작업을 포함해 단계 합계가 프레임 wall time과 다를 수 있습니다. GPU 미측정·부분 대기는 --, 작업 0은 관측 수 0입니다. 카운터는 source별 구간 평균이며 장치 업로드 풀의 누적 통계와 구분합니다."); ImGui::PopTextWrapPos(); ImGui::EndTooltip(); }
    if(shown.currentRevision>applied->appliedRevision)
        ImGui::TextColored({1,.69f,.3f,1},"이전 설정의 비교 · 현재 설정은 이후에 변경됨");
    else if(shown.currentRevision && shown.currentRevision!=applied->appliedRevision)
        ImGui::TextWrapped("비교 표본과 현재 설정 구간이 다릅니다.");
    ImGui::TextWrapped("실제 설정값: %s → %s",ValueText(applied->previousActual).c_str(),ValueText(applied->actual).c_str());
    if(applied->previousActual==applied->actual) ImGui::TextDisabled("설정값 동일 · 전환 없음");
    std::string changes;
    for(const auto& value:state.requests) if(value.state==D::ControlState::Applied && value.device==applied->device && value.source==applied->source && value.appliedRevision==applied->appliedRevision)
    {
        shown.revisionChanges.push_back({value.request,value.control,value.previousActual,value.actual});
        const D::ControlDesc* desc=nullptr;
        if(state.snapshot) for(const auto& candidate:state.snapshot->controls) if(candidate.id==value.control) {desc=&candidate;break;}
        if(!changes.empty()) changes+="; ";
        changes+=(desc ? desc->label.empty() ? desc->key : desc->label : "미등록 조절 항목")+": "+ValueText(value.previousActual)+" → "+ValueText(value.actual);
    }
    if(shown.revisionChanges.size()>1) ImGui::TextWrapped("같은 설정 변경 구간의 요청 %zu개: %s",shown.revisionChanges.size(),changes.c_str());
    ImGui::TextDisabled("%s / %s · 수치는 초당 1회 갱신",shown.beforeReady ? "이전 1초 준비" : "이전 기준 부족",shown.afterReady ? "이후 1초 준비" : "새 설정 1초 대기");
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("실제 적용 경계 전후의 완료된 1초 구간을 비교합니다.");
    for(int side=0;side<2;++side)
    {
        const auto& value=side ? shown.status.after : shown.status.baseline;
        const auto rate=(side ? shown.afterReady : shown.beforeReady) ? WindowRate(value) : std::nullopt;
        if(rate) ImGui::TextWrapped("%s 1초 · 처리 %s개 · %s구간/초",side ? "이후" : "이전",DisplayAmount(double(value.frameCount),0).c_str(),DisplayAmount(*rate,1).c_str());
        else ImGui::TextDisabled("%s 1초 · 완료 구간 대기",side ? "이후" : "이전");
        if(ImGui::IsItemHovered()) ImGui::SetTooltip("선택 source의 프레임·작업 구간 처리량입니다. 실제 Present FPS와 다를 수 있습니다.");
    }
    const float width=ImGui::GetContentRegionAvail().x;
    if(ImGui::CollapsingHeader("단계별 비용·작업량",ImGuiTreeNodeFlags_DefaultOpen))
    {
        const char* metrics[]{"CPU ms","GPU ms","작업 / frame","요소 / frame","B / frame"};
        ImGui::SetNextItemWidth(std::min(260.f,width*.55f));
        ImGui::Combo("측정 항목",&stageComparisonMetric,metrics,int(StageMetric::Count));
        const auto metric=StageMetric(stageComparisonMetric);
        if(ImGui::BeginTable("##Stages",5,ImGuiTableFlags_RowBg|ImGuiTableFlags_BordersInnerV|ImGuiTableFlags_SizingStretchProp|ImGuiTableFlags_ScrollY,{width,TableHeight(D::LiveStageCount)}))
        {
            ComparisonColumns("단계");
            for(size_t i=0;i<D::LiveStageCount;++i)
            {
                ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextWrapped("%s",stages[i]);
                if(metric==StageMetric::Cpu || metric==StageMetric::Gpu)
                {
                    const auto& a=metric==StageMetric::Cpu ? shown.status.baseline.stages[i].cpu : shown.status.baseline.stages[i].gpu;
                    const auto& b=metric==StageMetric::Cpu ? shown.status.after.stages[i].cpu : shown.status.after.stages[i].gpu;
                    ImGui::TextDisabled("측정 %llu/%llu → %llu/%llu",a.ready,shown.status.baseline.frameCount,b.ready,shown.status.after.frameCount);
                    if(ImGui::IsItemHovered()) ImGui::SetTooltip("이전: pending %llu / unsupported %llu / unavailable %llu\n이후: pending %llu / unsupported %llu / unavailable %llu",
                        a.pending,a.unsupported,a.unavailable,b.pending,b.unsupported,b.unavailable);
                }
                const auto pair=tracyMetrics.CompareStage(shown.beforeReady ? shown.status.baseline : D::LiveAggregate{},
                    shown.afterReady ? shown.status.after : D::LiveAggregate{},D::LiveStage(i),metric);
                ImGui::TextDisabled("%s",OriginName(pair.before.origin));
                if(ImGui::IsItemHovered())ImGui::SetTooltip("이전: %s\n이후: %s",pair.before.reason.c_str(),pair.after.reason.c_str());
                shown.stageVisibility[i][size_t(metric)]=NumberCells(pair.change,stageComparisonMetric<2 ? 3 : 1);
                if(shown.stageVisibility[i][size_t(metric)])
                {
                    const auto slot="comparison.stage."+std::to_string(i)+"."+std::to_string(size_t(metric));
                    RecordMetric(slot+".before",pair.before,shown.status.request);RecordMetric(slot+".after",pair.after,shown.status.request);
                }
            }
            ImGui::EndTable();
        }
    }
    if(ImGui::CollapsingHeader("어떤 작업이 줄거나 늘었는지",ImGuiTreeNodeFlags_DefaultOpen))
    {
        const char* phases[]{"기록한 작업","제출한 작업","완료한 작업"};
        ImGui::SetNextItemWidth(std::min(260.f,width*.55f)); ImGui::Combo("집계 범위",&counterComparisonPhase,phases,3);
        if(ImGui::BeginTable("##Counters",5,ImGuiTableFlags_RowBg|ImGuiTableFlags_BordersInnerV|ImGuiTableFlags_SizingStretchProp|ImGuiTableFlags_ScrollY,{width,TableHeight(ComparisonCounterCount)}))
        {
            ComparisonColumns("작업 / frame · 자체");
            for(size_t i=0;i<ComparisonCounters().size();++i)
            {
                const auto& field=ComparisonCounters()[i]; ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextWrapped("%s",field.label);
                if(field.recordedOnly && counterComparisonPhase!=int(CounterPhase::Recorded)) ImGui::TextDisabled("기록 전용 / 해당 없음");
                shown.counterVisibility[size_t(counterComparisonPhase)][i]=NumberCells(CompareNumbers(
                    shown.beforeReady ? CounterNumber(shown.status.baseline,CounterPhase(counterComparisonPhase),field) : std::nullopt,
                    shown.afterReady ? CounterNumber(shown.status.after,CounterPhase(counterComparisonPhase),field) : std::nullopt),1);
            }
            ImGui::EndTable();
        }
    }
    ComparisonFlows(shown);
    if(ImGui::CollapsingHeader("프레임 전체 요약"))
    {
        const char* names[]{"1초 처리 구간 수","구간율(평균 시간 역수)","평균 구간 (ms)","평균 CPU (ms)","평균 GPU (ms)","Draw / 구간","Instances / 구간","Dispatch / 구간","Upload B / 구간"};
        if(ImGui::BeginTable("##Summary",5,ImGuiTableFlags_RowBg|ImGuiTableFlags_BordersInnerV|ImGuiTableFlags_SizingStretchProp,{width,0}))
        {
            ComparisonColumns("측정");
            for(size_t i=0;i<9;++i)
            {
                ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextUnformatted(names[i]);
                const auto pair=tracyMetrics.Compare(shown.beforeReady ? shown.status.baseline : D::LiveAggregate{},
                    shown.afterReady ? shown.status.after : D::LiveAggregate{},ComparisonMetric(i));
                ImGui::TextDisabled("%s",OriginName(pair.before.origin));
                if(ImGui::IsItemHovered())ImGui::SetTooltip("이전: %s\n이후: %s",pair.before.reason.c_str(),pair.after.reason.c_str());
                shown.metricVisibility[i]=NumberCells(pair.change,i==0 ? 0 : i>=size_t(ComparisonMetric::Draws) ? 1 : 3);
                if(shown.metricVisibility[i])
                {
                    const auto slot="comparison.metric."+std::to_string(i);
                    RecordMetric(slot+".before",pair.before,shown.status.request);RecordMetric(slot+".after",pair.after,shown.status.request);
                }
            }
            ImGui::EndTable();
        }
    }
    comparisonDrawn=std::move(shown);
}
static void FlowText(ImDrawList* draw,ImVec2 at,float size,ImU32 color,const char* text)
{
    const float width=FlowRouteLayout::Spacing-8; const auto center=at;
    at.x-=std::min(width,ImGui::GetFont()->CalcTextSizeA(size,std::numeric_limits<float>::max(),0,text).x)*.5f;
    const ImVec4 clip{center.x-width*.5f,at.y,center.x+width*.5f,at.y+size+2};
    draw->AddText(ImGui::GetFont(),size,at,color,text,nullptr,0,&clip);
}
void Viewer::ComparisonFlows(ComparisonPresentation& shown)
{
    const bool expanded=ImGui::CollapsingHeader("전후 원본 · 3초 표본",ImGuiTreeNodeFlags_DefaultOpen);
    if(ImGui::IsItemHovered()) { ImGui::BeginTooltip(); ImGui::PushTextWrapPos(ImGui::GetCursorPosX()+450.f); ImGui::TextUnformatted("각 열은 실제 한 프레임의 원본이며 1초 평균과 별도 구간일 수 있습니다. 같은 재생 위치가 실제 시간 정렬이나 명령의 일대일 대응을 뜻하지 않습니다. 반복 묶음은 대표 명령만 표시합니다. 관측 개수는 완료 횟수가 아니며 논리 명령·Upload·Wait·Present를 세고 변환·페이지 획득·완료 표식은 중복 합산하지 않습니다. 상세 유실 시 없는 작업을 사라졌다고 판정하지 않습니다."); ImGui::PopTextWrapPos(); ImGui::EndTooltip(); }
    if(!expanded) return;
    bool visible=false;
    if(!comparisonPlayback.evidence || comparisonPlayback.evidence->request!=shown.status.request)
    { ImGui::TextDisabled("전후 원본 표본 대기"); return; }
    const auto& pair=*comparisonPlayback.evidence;
    const D::LiveFrameEvidence* frames[]{&pair.before,&pair.after};
    if(comparisonGraphGeneration!=comparisonPlayback.generation)
    {
        for(size_t i=0;i<2;++i)
        {
            auto& side=comparisonSides[i]; const auto selected=side.selected; side={};
            side.valid=ValidFrameEvidence(*frames[i],state.device,state.source,comparisonPlayback.producerNs);
            side.complete=CompleteFrameEvidence(*frames[i],state.device,state.source,comparisonPlayback.producerNs);
            if(side.valid)
            {
                side.graph=BuildFlowGraph(*frames[i]); side.routes=BuildFlowRoutes(side.graph,*frames[i]); side.observed=ObserveFlow(side.graph,*frames[i]);
                side.selected=std::min(selected,side.routes.empty() ? size_t(0) : side.routes.size()-1);
            }
        }
        comparisonGraphGeneration=comparisonPlayback.generation;
    }
    ComparisonFlowPresentation flow;
    flow.session=comparisonPlayback.session; flow.request=pair.request; flow.sequence=comparisonPlayback.sequence; flow.cycleStartedNs=comparisonPlayback.startedNs;
    const auto phase=comparisonPlayback.Phase(D::LiveNowNs());
    ImGui::Checkbox("전후 흐름의 참조·의존 경로 포함",&comparisonReferences);
    const float width=ImGui::GetContentRegionAvail().x;
    if(ImGui::BeginTable("##OriginalPair",width<800 ? 1 : 2,ImGuiTableFlags_SizingStretchSame|ImGuiTableFlags_BordersInnerV,{width,0}))
    {
        for(size_t i=0;i<2;++i)
        {
            ImGui::TableNextColumn(); ImGui::PushID(int(i));
            const auto& frame=*frames[i]; auto& side=comparisonSides[i]; auto& proof=flow.frames[i];
            proof.frame=frame.summary.key; proof.revision=frame.summary.appliedRevision; proof.windowRevision=frame.windowRevision;
            proof.windowBeginNs=frame.windowBeginNs; proof.windowEndNs=frame.windowEndNs; proof.available=frame.available;
            proof.valid=side.valid; proof.complete=side.complete; proof.budgetLimited=frame.budgetLimited; proof.incomplete=frame.summary.incomplete;
            proof.loss=frame.loss; proof.omittedEvents=frame.flow.omittedEvents; proof.omittedLinks=frame.flow.omittedLinks; proof.observed=side.observed;
            const auto& average=i ? shown.status.after : shown.status.baseline;
            proof.matchesAverageWindow=(i ? shown.afterReady : shown.beforeReady) && frame.windowBeginNs==average.beginNs && frame.windowEndNs==average.endNs && frame.windowRevision==average.appliedRevision;
            ImGui::PushFont(nullptr,24); ImGui::TextUnformatted(i ? "이후 원본" : "이전 원본"); ImGui::PopFont();
            if(proof.frame.frame) ImGui::TextUnformatted(i ? "설정 변경 후 표본" : "설정 변경 전 표본");
            else ImGui::TextDisabled("원본 표본 미보존");
            if(frame.windowEndNs>frame.windowBeginNs) ImGui::TextWrapped("보존 구간 %.1f초",(frame.windowEndNs-frame.windowBeginNs)/1e9);
            else ImGui::TextDisabled("구간 미수집");
            ImGui::TextWrapped("%s",proof.matchesAverageWindow ? "1초 평균과 같은 구간" : "평균과 별도 구간");
            if(ImGui::IsItemHovered()) ImGui::SetTooltip("원본 표본과 1초 평균의 구간 일치 여부입니다. 내부 원본 연결은 보고서에 보존합니다.");
            if(!side.valid)
            {
                ImGui::TextWrapped("%s",frame.budgetLimited ? "상세 보존 한도 / 원본 사용 불가" : frame.available ? "표본과 설정 구간의 정합성 없음" : "원본 표본 미보존 또는 완료 구간 대기");
                visible|=ImGui::IsItemVisible();
                ImGui::PopID(); continue;
            }
            ImGui::TextWrapped("보존된 이벤트 %zu · 연결 %zu%s",frame.flow.events.size(),frame.flow.links.size(),side.complete ? "" : " · 일부 상세 생략");
            if(ImGui::IsItemHovered()) ImGui::SetTooltip("보존된 상세만 표시합니다. 상세가 유실·생략되면 없는 명령을 미사용으로 판정하지 않습니다.");
            std::vector<size_t> choices;
            for(size_t j=0;j<side.routes.size();++j) if(side.routes[j].recording || comparisonReferences) choices.push_back(j);
            if(choices.empty()) { ImGui::TextWrapped("연속된 기록 연결 미수집. 참조·의존 경로를 포함해 원본 관계를 확인할 수 있습니다."); visible|=ImGui::IsItemVisible(); ImGui::PopID(); continue; }
            if(std::find(choices.begin(),choices.end(),side.selected)==choices.end()) { side.selected=choices.front(); side.layoutSelected=SIZE_MAX; }
            const auto title=std::string(side.routes[side.selected].recording ? "기록 경로 " : "참조 경로 ")+std::to_string(side.selected+1);
            ImGui::SetNextItemWidth(-1);
            if(ImGui::BeginCombo("##Route",title.c_str()))
            {
                for(const auto j:choices)
                {
                    const auto label=std::string(side.routes[j].recording ? "기록 " : "참조 ")+std::to_string(j+1)+" · "+std::to_string(side.routes[j].links.size())+"개 연결";
                    if(ImGui::Selectable(label.c_str(),side.selected==j)) { side.selected=j; side.layoutSelected=SIZE_MAX; comparisonPlayback.startedNs=D::LiveNowNs(); flow.cycleStartedNs=comparisonPlayback.startedNs; }
                }
                ImGui::EndCombo();
            }
            const auto& route=side.routes[side.selected];
            if(side.layoutSelected!=side.selected) { side.layout=LayoutFlowRoute(route); side.layoutSelected=side.selected; }
            const auto& layout=side.layout; proof.routeLinks=route.links.size(); proof.routeSteps=route.steps.size();
            if(layout.nodes.empty()) { ImGui::TextDisabled("연속 경로 정합성 없음"); ImGui::PopID(); continue; }
            if(!route.recording) ImGui::TextDisabled("참조 관계 · 실행 순서 아님");
            if(ImGui::BeginChild("##OriginalCanvas",{0,layout.height+ImGui::GetStyle().WindowPadding.y*2+ImGui::GetStyle().ScrollbarSize},true,ImGuiWindowFlags_HorizontalScrollbar))
            {
                const auto origin=ImGui::GetCursorScreenPos(); ImGui::InvisibleButton("##Canvas",{layout.width,layout.height});
                proof.canvasVisible=ImGui::IsItemVisible(); visible|=proof.canvasVisible;
                const bool hovered=ImGui::IsItemHovered(); auto* draw=ImGui::GetWindowDrawList();
                const auto point=[&](FlowPoint p){return ImVec2{origin.x+p.x,origin.y+p.y};};
                const size_t first=size_t(std::max(0,int(ImGui::GetScrollX()/layout.Spacing)-2));
                const size_t last=std::min(layout.nodes.size(),size_t(std::max(0,int((ImGui::GetScrollX()+ImGui::GetWindowWidth())/layout.Spacing)+3)));
                const ImU32 color=route.recording ? IM_COL32(56,194,191,255) : IM_COL32(142,175,214,255);
                for(size_t j=first;j<last && j<layout.paths.size();++j)
                {
                    const auto& path=layout.paths[j];
                    for(size_t k=1;k<path.size();++k) draw->AddLine(point(path[k-1]),point(path[k]),color,3);
                    auto tip=layout.nodes[j+1]; ImVec2 direction{1,0};
                    if(path.size()==2) { tip.y=path.back().y; const auto dy=tip.y-layout.nodes[j+1].y; tip.x-=std::sqrt(layout.Radius*layout.Radius-dy*dy); }
                    else { const auto port=layout.Radius*.5f; tip.x-=port; tip.y+=std::sqrt(layout.Radius*layout.Radius-port*port); direction={0,-1}; }
                    const auto end=point(tip);
                    draw->AddTriangleFilled(end,{end.x-direction.x*12-direction.y*6,end.y-direction.y*12+direction.x*6},{end.x-direction.x*12+direction.y*6,end.y-direction.y*12-direction.x*6},color);
                }
                for(size_t j=first;j<last;++j)
                {
                    const int column=j ? route.steps[j-1].to : route.steps.front().from;
                    const auto center=point(layout.nodes[j]);
                    draw->AddCircleFilled(center,layout.Radius,IM_COL32(24,47,61,255)); draw->AddCircle(center,layout.Radius,color,64,3);
                    const auto* event=FlowEvent(side.graph,frame.flow,layout.ids[j]);
                    auto description=event ? DescribeEvent(*event) : EventText{column<FlowReferenceColumn ? stages[column] : "참조",{},"참조 정보",{},"--","미수집"};
                    if(!event && column==int(D::LiveStage::Gpu))
                    {
                        const auto span=std::find_if(frame.flow.gpuSpans.begin(),frame.flow.gpuSpans.end(),[&](const auto& value){return value.id==layout.ids[j] && value.frame==proof.frame;});
                        if(span!=frame.flow.gpuSpans.end())
                        {
                            description.name="GPU 표본"; const auto& duration=span->duration;
                            description.unit=duration.state==D::LiveValueState::Pending ? "대기" : duration.state==D::LiveValueState::Unsupported ? "미지원" : "미수집";
                            if(duration.state==D::LiveValueState::Ready && std::isfinite(duration.milliseconds)) { description.value=DisplayAmount(duration.milliseconds,3); description.unit="GPU ms"; }
                        }
                    }
                    FlowText(draw,{center.x,origin.y+2},18,IM_COL32(228,239,246,255),description.name.c_str());
                    if((j ? route.steps[j-1] : route.steps.front()).links>1) FlowText(draw,{center.x,origin.y+24},18,IM_COL32(145,166,187,255),"대표 명령");
                    FlowText(draw,{center.x,center.y-22},22,IM_COL32(228,239,246,255),description.value.c_str()); FlowText(draw,{center.x,center.y+2},18,IM_COL32(145,166,187,255),description.unit.c_str());
                    if(ImGui::IsRectVisible({center.x-layout.Radius,center.y-layout.Radius},{center.x+layout.Radius,center.y+layout.Radius})) proof.visibleOccurrences.push_back(layout.ids[j]);
                    if(hovered && std::hypot(ImGui::GetIO().MousePos.x-center.x,ImGui::GetIO().MousePos.y-center.y)<layout.Radius)
                    {
                        ImGui::BeginTooltip();
                        const auto found=std::find_if(side.graph.nodes.begin(),side.graph.nodes.end(),[&](const auto& node){return node.id==layout.ids[j];});
                        if(found!=side.graph.nodes.end() && found->kind==FlowNodeKind::Event)
                        { EventSummary(frame.flow.events[found->index],"원본 명령 표본"); }
                        if(found!=side.graph.nodes.end() && found->kind==FlowNodeKind::Object) ImGui::TextWrapped("%s",frame.objects[found->index].desc.label.c_str());
                        if(found!=side.graph.nodes.end() && found->kind==FlowNodeKind::Unknown) ImGui::TextUnformatted("종류 metadata 미수집 / 역할 미분류");
                        ImGui::EndTooltip();
                    }
                }
                const auto at=LocateFlowRoute(route,phase);
                draw->AddCircleFilled(point(PointOnFlowPath(layout.paths[at.step],at.phase)),4,IM_COL32(255,211,112,255));
            }
            ImGui::EndChild();
            const auto current=LocateFlowRoute(route,phase);
            const auto* representative=FlowEvent(side.graph,frame.flow,layout.ids[current.step+1]);
            if(!representative) representative=FlowEvent(side.graph,frame.flow,layout.ids[current.step]);
            if(representative) { const auto caption="대표 명령 · 묶음 연결 "+std::to_string(route.steps[current.step].links)+"개"; EventSummary(*representative,caption.c_str()); }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    const auto change=[&](double before,double after) {
        return CompareObservedCounts(flow.frames[0].valid ? std::optional<double>(before) : std::nullopt,flow.frames[1].valid ? std::optional<double>(after) : std::nullopt,
            flow.frames[0].complete,flow.frames[1].complete);
    };
    const auto table=[&](const char* id) {
        const auto result=ImGui::BeginTable(id,5,ImGuiTableFlags_RowBg|ImGuiTableFlags_BordersInnerV|ImGuiTableFlags_SizingStretchProp,{width,0});
        if(result) { ImGui::TableSetupColumn("관측 종류",ImGuiTableColumnFlags_WidthStretch,1.6f); for(const auto* name:{"이전 원본","이후 원본","관측 개수 차이","비율 (%)"}) ImGui::TableSetupColumn(name); ImGui::TableHeadersRow(); }
        return result;
    };
    if(ImGui::CollapsingHeader("원본 단계 관측 개수",ImGuiTreeNodeFlags_DefaultOpen) && table("##ObservedStages"))
    {
        for(size_t i=0;i<D::LiveStageCount;++i)
        { ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextUnformatted(stages[i]); flow.stageVisibility[i]=NumberCells(change(double(flow.frames[0].observed.stageEvents[i]),double(flow.frames[1].observed.stageEvents[i])),0); visible|=flow.stageVisibility[i]!=0; }
        ImGui::EndTable();
    }
    if(ImGui::CollapsingHeader("원본 명령·업로드·대기·출력 관측") && table("##ObservedCommands"))
    {
        for(size_t i=1;i<flow.commandVisibility.size();++i)
        { ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextUnformatted(CommandKind(D::LiveCommandKind(i))); flow.commandVisibility[i]=NumberCells(change(double(flow.frames[0].observed.commandKinds[i]),double(flow.frames[1].observed.commandKinds[i])),0); visible|=flow.commandVisibility[i]!=0; }
        ImGui::EndTable();
    }
    if(ImGui::CollapsingHeader("원본 단계 사이 관측 연결") && table("##ObservedLinks"))
    {
        const auto count=[](const FlowObservations& observed,int from,int to) { for(const auto& link:observed.links) if(link.from==from && link.to==to) return link.links; return uint64_t(0); };
        for(int from=0;from<=FlowReferenceColumn;++from) for(int to=0;to<=FlowReferenceColumn;++to)
        {
            const auto before=count(flow.frames[0].observed,from,to),after=count(flow.frames[1].observed,from,to); if(!before && !after) continue;
            ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextWrapped("%s → %s",from<FlowReferenceColumn ? stages[from] : "참조·미분류",to<FlowReferenceColumn ? stages[to] : "참조·미분류");
            auto& flag=flow.linkVisibility[size_t(from*(FlowReferenceColumn+1)+to)]; flag=NumberCells(change(double(before),double(after)),0); visible|=flag!=0;
        }
        ImGui::EndTable();
    }
    if(visible) shown.flow=std::move(flow);
}
}
