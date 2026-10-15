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
            const bool large=std::abs(*values[i])>=100000;
            if(large) ImGui::Text(i>=2 ? "%+.3g" : "%.3g",*values[i]);
            else ImGui::Text(i>=2 ? "%+.*f" : "%.*f",i==3 ? 1 : decimals,*values[i]);
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
        ImGui::TextWrapped("선택한 기능을 실제 적용하면 이전·이후 1초와 원본 흐름을 비교합니다. 상세 원본은 source의 최신 적용 요청 하나를 보존합니다.");
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
    ImGui::Separator(); ImGui::PushFont(nullptr,24); ImGui::TextUnformatted("선택 구간의 관측 변화"); ImGui::PopFont();
    if(shown.currentRevision>applied->appliedRevision)
        ImGui::TextColored({1,.69f,.3f,1},"과거 비교 rev %llu / 현재 rev %llu",applied->appliedRevision,shown.currentRevision);
    else if(shown.currentRevision && shown.currentRevision!=applied->appliedRevision)
        ImGui::TextWrapped("비교 rev %llu / 최근 프레임 rev %llu",applied->appliedRevision,shown.currentRevision);
    ImGui::TextWrapped("실제 설정값: %s → %s",ValueText(applied->previousActual).c_str(),ValueText(applied->actual).c_str());
    if(applied->previousActual==applied->actual) ImGui::TextDisabled("실제 설정값이 같은 요청입니다. 기능 전환 효과로 해석하지 않습니다.");
    std::string changes;
    for(const auto& value:state.requests) if(value.state==D::ControlState::Applied && value.device==applied->device && value.source==applied->source && value.appliedRevision==applied->appliedRevision)
    {
        shown.revisionChanges.push_back({value.request,value.control,value.previousActual,value.actual});
        const D::ControlDesc* desc=nullptr;
        if(state.snapshot) for(const auto& candidate:state.snapshot->controls) if(candidate.id==value.control) {desc=&candidate;break;}
        if(!changes.empty()) changes+="; ";
        changes+=(desc ? desc->label.empty() ? desc->key : desc->label : "control "+std::to_string(value.control))+": "+ValueText(value.previousActual)+" → "+ValueText(value.actual);
    }
    if(shown.revisionChanges.size()>1) ImGui::TextWrapped("같은 revision에서 처리된 알려진 요청 %zu개: %s",shown.revisionChanges.size(),changes.c_str());
    ImGui::TextWrapped("단일 설정 변경·동일 부하를 확인하지 않은 비교입니다. 수치 증감만으로 이 기능의 성능 효과를 단정하지 않습니다.");
    ImGui::TextDisabled("%s / %s · 수치는 초당 1회 갱신",shown.beforeReady ? "이전 1초 준비" : "이전 기준 부족",shown.afterReady ? "이후 1초 준비" : "새 설정 1초 대기");
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("Request %llu / control %llu\n적용 frame %llu / revision %llu",applied->request,applied->control,applied->appliedFrame.frame,applied->appliedRevision);
    for(int side=0;side<2;++side)
    {
        const auto& value=side ? shown.status.after : shown.status.baseline;
        ImGui::TextWrapped("%s 평균 구간: %.3f .. %.3f s · rev %llu · %llu frames",side ? "이후" : "이전",value.beginNs/1e9,value.endNs/1e9,value.appliedRevision,value.frameCount);
        if(ImGui::IsItemHovered()) ImGui::SetTooltip("원본 구간 %llu .. %llu ns\n전환·혼합·미완료 구간은 비교하지 않습니다.",value.beginNs,value.endNs);
    }
    const float width=std::max(1000.f,ImGui::GetContentRegionAvail().x);
    if(ImGui::CollapsingHeader("단계별 비용·작업량",ImGuiTreeNodeFlags_DefaultOpen))
    {
        const char* metrics[]{"CPU ms","GPU ms","작업 / frame","요소 / frame","B / frame"};
        ImGui::SetNextItemWidth(300);
        ImGui::Combo("측정 항목",&stageComparisonMetric,metrics,int(StageMetric::Count));
        const auto metric=StageMetric(stageComparisonMetric);
        if(ImGui::BeginTable("##Stages",5,ImGuiTableFlags_RowBg|ImGuiTableFlags_BordersInnerV|ImGuiTableFlags_SizingStretchProp|ImGuiTableFlags_ScrollY,{width,350}))
        {
            ComparisonColumns("단계");
            for(size_t i=0;i<D::LiveStageCount;++i)
            {
                ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextUnformatted(stages[i]);
                if(metric==StageMetric::Cpu || metric==StageMetric::Gpu)
                {
                    const auto& a=metric==StageMetric::Cpu ? shown.status.baseline.stages[i].cpu : shown.status.baseline.stages[i].gpu;
                    const auto& b=metric==StageMetric::Cpu ? shown.status.after.stages[i].cpu : shown.status.after.stages[i].gpu;
                    ImGui::TextDisabled("측정 %llu/%llu → %llu/%llu",a.ready,shown.status.baseline.frameCount,b.ready,shown.status.after.frameCount);
                    if(ImGui::IsItemHovered()) ImGui::SetTooltip("이전: pending %llu / unsupported %llu / unavailable %llu\n이후: pending %llu / unsupported %llu / unavailable %llu",
                        a.pending,a.unsupported,a.unavailable,b.pending,b.unsupported,b.unavailable);
                }
                shown.stageVisibility[i][size_t(metric)]=NumberCells(CompareNumbers(
                    shown.beforeReady ? StageNumber(shown.status.baseline,D::LiveStage(i),metric) : std::nullopt,
                    shown.afterReady ? StageNumber(shown.status.after,D::LiveStage(i),metric) : std::nullopt),stageComparisonMetric<2 ? 3 : 1);
            }
            ImGui::EndTable();
        }
        ImGui::TextWrapped("CPU는 측정된 프레임의 scope 평균이며 중첩·병렬 작업을 포함할 수 있습니다. 단계 합계는 프레임 wall time이 아닙니다. GPU 미측정·부분 대기는 --, 작업 0은 계측된 작업 수 0입니다.");
    }
    if(ImGui::CollapsingHeader("어떤 작업이 줄거나 늘었는지",ImGuiTreeNodeFlags_DefaultOpen))
    {
        const char* phases[]{"기록한 작업","제출한 작업","완료한 작업"};
        ImGui::SetNextItemWidth(300); ImGui::Combo("집계 범위",&counterComparisonPhase,phases,3);
        if(ImGui::BeginTable("##Counters",5,ImGuiTableFlags_RowBg|ImGuiTableFlags_BordersInnerV|ImGuiTableFlags_SizingStretchProp|ImGuiTableFlags_ScrollY,{width,350}))
        {
            ComparisonColumns("작업 / frame");
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
        ImGui::TextWrapped("생산자가 계산한 source별 1초 평균입니다. 장치 전체 업로드 풀의 누적 재사용률을 이 구간의 수치로 사용하지 않습니다.");
    }
    ComparisonFlows(shown);
    if(ImGui::CollapsingHeader("프레임 전체 요약"))
    {
        const char* names[]{"프레임 수","FPS","프레임 (ms)","CPU (ms)","GPU (ms)","Draw / frame","Instances / frame","Dispatch / frame","Upload B / frame"};
        if(ImGui::BeginTable("##Summary",5,ImGuiTableFlags_RowBg|ImGuiTableFlags_BordersInnerV|ImGuiTableFlags_SizingStretchProp,{width,0}))
        {
            ComparisonColumns("측정");
            for(size_t i=0;i<9;++i)
            {
                ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextUnformatted(names[i]);
                shown.metricVisibility[i]=NumberCells(CompareNumbers(
                    shown.beforeReady ? ComparisonNumber(shown.status.baseline,ComparisonMetric(i)) : std::nullopt,
                    shown.afterReady ? ComparisonNumber(shown.status.after,ComparisonMetric(i)) : std::nullopt),i==0 ? 0 : i>=size_t(ComparisonMetric::Draws) ? 1 : 3);
            }
            ImGui::EndTable();
        }
    }
    comparisonDrawn=std::move(shown);
}
static void FlowText(ImDrawList* draw,ImVec2 at,float size,ImU32 color,const char* text)
{
    at.x-=ImGui::GetFont()->CalcTextSizeA(size,std::numeric_limits<float>::max(),0,text).x*.5f;
    draw->AddText(ImGui::GetFont(),size,at,color,text);
}
void Viewer::ComparisonFlows(ComparisonPresentation& shown)
{
    if(!ImGui::CollapsingHeader("전후 원본 흐름 · 한 쌍을 3초 동안 재생",ImGuiTreeNodeFlags_DefaultOpen)) return;
    bool visible=false;
    ImGui::TextWrapped("각각 실제 한 프레임입니다. 같은 재생 위치는 실제 시간 정렬이나 역할 대응을 뜻하지 않습니다. 1초 평균 구간과 원본 프레임 구간을 따로 확인하세요. 같은 단계가 다시 나오면 같은 원본 단계 집계를 반복 표시하며 합산하지 않습니다.");
    if(!comparisonPlayback.evidence || comparisonPlayback.evidence->request!=shown.status.request)
    { ImGui::TextDisabled("최신 요청의 전후 원본 쌍 대기 / 다른 프레임으로 대신 그리지 않습니다."); return; }
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
    const float width=std::max(760.f,ImGui::GetContentRegionAvail().x);
    if(ImGui::BeginTable("##OriginalPair",2,ImGuiTableFlags_SizingStretchSame|ImGuiTableFlags_BordersInnerV,{width,0}))
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
            if(proof.frame.frame) ImGui::TextWrapped("Frame %llu · rev %llu",proof.frame.frame,proof.revision);
            else ImGui::TextDisabled("Frame -- / 원본 미보존");
            if(frame.windowEndNs>frame.windowBeginNs) ImGui::TextWrapped("원본 구간 %.3f .. %.3f s",frame.windowBeginNs/1e9,frame.windowEndNs/1e9);
            else ImGui::TextDisabled("원본 구간 --");
            ImGui::TextWrapped("%s",proof.matchesAverageWindow ? "현재 1초 평균과 같은 구간" : "현재 1초 평균과 별도 구간 / 원본 유지");
            if(ImGui::IsItemHovered()) ImGui::SetTooltip("원본 %llu .. %llu ns / window rev %llu\nDevice %llu / source %llu",frame.windowBeginNs,frame.windowEndNs,frame.windowRevision,proof.frame.device,proof.frame.source);
            if(!side.valid)
            {
                ImGui::TextWrapped("%s",frame.budgetLimited ? "상세 보존 한도 / 원본 사용 불가" : frame.available ? "원본 frame·revision·구간 정합성 없음" : "원본 프레임 미보존 또는 완료 구간 대기");
                visible|=ImGui::IsItemVisible();
                ImGui::PopID(); continue;
            }
            ImGui::TextWrapped("보존된 이벤트 %zu · 연결 %zu%s",frame.flow.events.size(),frame.flow.links.size(),side.complete ? "" : " · 일부 상세 생략");
            if(ImGui::IsItemHovered()) ImGui::SetTooltip("Omitted events %llu / links %llu\nLoss events %llu / links %llu / frames %llu / inventory %llu / GPU %llu\nTransfer truncated %d / budget limited %d",
                frame.flow.omittedEvents,frame.flow.omittedLinks,frame.loss.events,frame.loss.links,frame.loss.frames,frame.loss.inventory,frame.loss.gpuSpans,frame.loss.transferTruncated,frame.budgetLimited);
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
            ImGui::TextWrapped("%s · 원 안의 수치는 이 원본 프레임의 작업 수",route.recording ? "실제 기록 연결" : "참조·의존 관계 / 실행 순서 아님");
            if(ImGui::BeginChild("##OriginalCanvas",{0,360},true,ImGuiWindowFlags_HorizontalScrollbar))
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
                    FlowText(draw,{center.x,origin.y+12},26,IM_COL32(228,239,246,255),column<FlowReferenceColumn ? stages[column] : "참조·미분류");
                    if(j && route.steps[j-1].from==column)
                    {
                        const auto repeat=std::string("반복 연결 ")+std::to_string(route.steps[j-1].links)+"개";
                        FlowText(draw,{center.x,origin.y+46},20,IM_COL32(145,166,187,255),repeat.c_str());
                    }
                    char number[64]="관계"; const char* unit="원본 연결";
                    if(column<FlowReferenceColumn)
                    {
                        const auto count=column==int(D::LiveStage::Gpu) ? frame.flow.gpuSpans.size() : frame.summary.stages[size_t(column)].operations;
                        if(count>=100000) std::snprintf(number,sizeof(number),"%.3g",double(count)); else std::snprintf(number,sizeof(number),"%llu",static_cast<unsigned long long>(count));
                        unit=column==int(D::LiveStage::Gpu) ? "GPU spans" : "op / 원본";
                    }
                    FlowText(draw,{center.x,center.y-26},34,IM_COL32(228,239,246,255),number); FlowText(draw,{center.x,center.y+22},20,IM_COL32(145,166,187,255),unit);
                    if(ImGui::IsRectVisible({center.x-layout.Radius,center.y-layout.Radius},{center.x+layout.Radius,center.y+layout.Radius})) proof.visibleOccurrences.push_back(layout.ids[j]);
                    if(hovered && std::hypot(ImGui::GetIO().MousePos.x-center.x,ImGui::GetIO().MousePos.y-center.y)<layout.Radius)
                    {
                        ImGui::BeginTooltip(); ImGui::Text("원본 ID %llu / frame %llu / rev %llu",layout.ids[j],proof.frame.frame,proof.revision);
                        const auto found=std::find_if(side.graph.nodes.begin(),side.graph.nodes.end(),[&](const auto& node){return node.id==layout.ids[j];});
                        if(found!=side.graph.nodes.end() && found->kind==FlowNodeKind::Event)
                        { const auto& event=frame.flow.events[found->index]; ImGui::Text("%s / %s",EventKind(event.kind),CommandKind(event.commandKind)); ImGui::TextWrapped("%.*s",int(sizeof(event.label)),event.label); }
                        if(found!=side.graph.nodes.end() && found->kind==FlowNodeKind::Object) ImGui::TextWrapped("%s",frame.objects[found->index].desc.label.c_str());
                        if(found!=side.graph.nodes.end() && found->kind==FlowNodeKind::Unknown) ImGui::TextUnformatted("종류 metadata 미수집 / 역할 미분류");
                        ImGui::EndTooltip();
                    }
                }
                const auto at=LocateFlowRoute(route,phase);
                draw->AddCircleFilled(point(PointOnFlowPath(layout.paths[at.step],at.phase)),8,IM_COL32(255,211,112,255));
            }
            ImGui::EndChild(); ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::TextWrapped("아래는 보존된 원본 상세의 관측 개수입니다. ID 차집합이나 정확한 역할 대응을 계산하지 않습니다. 일부 상세가 유실되면 변화량·비율은 --이며, 보이지 않는 작업을 사라졌다고 판정하지 않습니다.");
    const auto change=[&](double before,double after) {
        return CompareObservedCounts(flow.frames[0].valid ? std::optional<double>(before) : std::nullopt,flow.frames[1].valid ? std::optional<double>(after) : std::nullopt,
            flow.frames[0].complete,flow.frames[1].complete);
    };
    const auto table=[&](const char* id) {
        const auto result=ImGui::BeginTable(id,5,ImGuiTableFlags_RowBg|ImGuiTableFlags_BordersInnerV|ImGuiTableFlags_SizingStretchProp,{std::max(1000.f,width),0});
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
        ImGui::TextWrapped("논리 명령·업로드·대기·Present의 관측 이벤트 수입니다. 네이티브 번역·페이지 획득·완료 표식을 중복 합산하지 않으며, 완료 횟수는 아닙니다.");
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
