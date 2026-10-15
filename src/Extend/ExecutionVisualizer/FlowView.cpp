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
    if(centered) point.x-=ImGui::GetFont()->CalcTextSizeA(size,std::numeric_limits<float>::max(),0,text).x*.5f;
    draw->AddText(ImGui::GetFont(),size,point,color,text);
}
static std::string MetricText(double value,int decimals)
{
    char text[64];
    if(std::abs(value)>=100000) std::snprintf(text,sizeof(text),"%.3g",value);
    else std::snprintf(text,sizeof(text),"%.*f",decimals,value);
    return text;
}
static bool MetricCard(const char* name,double mean,D::LiveValueState state,const D::LiveMetricRange& range,int decimals)
{
    const auto origin=ImGui::GetCursorScreenPos(); const ImVec2 size{std::max(1.f,ImGui::GetContentRegionAvail().x),116};
    ImGui::InvisibleButton(name,size); const bool visible=ImGui::IsItemVisible(); auto* draw=ImGui::GetWindowDrawList();
    draw->AddRectFilled(origin,{origin.x+size.x,origin.y+size.y},IM_COL32(23,35,48,255),5);
    TextAt(draw,{origin.x+12,origin.y+8},22,muted,name);
    char text[160];
    if(state==D::LiveValueState::Ready && std::isfinite(mean)) std::snprintf(text,sizeof(text),"%s",MetricText(mean,decimals).c_str());
    else std::snprintf(text,sizeof(text),"-- (%s)",ValueState(state));
    TextAt(draw,{origin.x+12,origin.y+34},34,ink,text);
    if(range.state==D::LiveValueState::Ready && range.windows && std::isfinite(range.low) && std::isfinite(range.high))
        std::snprintf(text,sizeof(text),"Low %s   High %s",MetricText(range.low,decimals).c_str(),MetricText(range.high,decimals).c_str());
    else std::snprintf(text,sizeof(text),"Low --   High --");
    TextAt(draw,{origin.x+12,origin.y+87},20,muted,text);
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("완료된 1초 구간의 평균\nLow / High: 같은 세션·source에서 완료된 1초 평균의 최저 / 최고\n유효 구간: %llu",range.windows);
    return visible;
}
void Viewer::Overview()
{
    if(!ImGui::Begin("Overview / 현재 상태",&panels[OverviewPanel])) { ImGui::End(); return; }
    const auto* source=statistics.state.Source();
    ImGui::Text("%s · 완료된 1초 평균 · 초당 1회 갱신",UpdateStatus());
    if(ImGui::IsItemHovered() && source)
        ImGui::SetTooltip("원본 완료 구간 %llu..%llu ns\nDevice %llu / source %llu / sequence %llu\nRevision %llu%s\n수신 기반 age %.1f ms",
            source->recent.beginNs,source->recent.endNs,source->device,source->source,statistics.state.snapshot->sequence,
            source->recent.appliedRevision,source->recent.mixedRevision ? " (혼합 설정 구간)" : "",state.AgeNs(D::LiveNowNs(),D::GetLiveClockInfo())/1e6);
    if(source && source->recent.mixedRevision) { ImGui::SameLine(); ImGui::TextDisabled("설정 전환 포함"); }
    const D::LiveAggregate empty; const D::LiveRunRanges emptyRanges;
    const auto& a=source ? source->recent : empty; const auto& ranges=source ? source->runRanges : emptyRanges;
    const auto counter=a.frameCount ? D::LiveValueState::Ready : D::LiveValueState::Unavailable;
    const auto metricState=[](const auto& value){ return Ready(value) ? D::LiveValueState::Ready : value.state==D::LiveValueState::Ready ? D::LiveValueState::Unavailable : value.state; };
    const auto fps=Ready(a.frame) && a.frame.average>0 ? D::LiveValueState::Ready : D::LiveValueState::Unavailable;
    if(ImGui::BeginChild("##StatisticCards",{0,0},false,ImGuiWindowFlags_HorizontalScrollbar))
    {
        const int columns=std::clamp(int(ImGui::GetContentRegionAvail().x/300),1,5);
        const auto width=std::max(float(columns)*300,ImGui::GetContentRegionAvail().x);
        if(ImGui::BeginTable("##Metrics",columns,ImGuiTableFlags_SizingStretchSame,{width,0}))
        {
            const auto card=[&](const char* title,double value,D::LiveValueState availability,const auto& range,int decimals) {
                ImGui::TableNextColumn(); if(MetricCard(title,value,availability,range,decimals) && source) statisticsDrawn=true;
            };
            card("FPS",a.fps,fps,ranges.fps,1);
            card("프레임 (ms)",a.frame.average,metricState(a.frame),ranges.frame,3);
            card("CPU render (ms)",a.cpuRender.average,metricState(a.cpuRender),ranges.cpuRender,3);
            card("GPU measured (ms)",a.gpuMeasured.average,metricState(a.gpuMeasured),ranges.gpuMeasured,3);
            card("Draw / 프레임",a.recordedMean.draw+a.recordedMean.drawIndexed,counter,ranges.drawCalls,1);
            card("Instances / 프레임",a.recordedMean.instances,counter,ranges.instances,1);
            card("Batches / 프레임",a.recordedMean.batches,counter,ranges.batches,1);
            card("Upload / 프레임 (B)",a.recordedMean.uploadBytes,counter,ranges.uploadBytes,1);
            card("생존 의미 객체",a.objects.average,metricState(a.objects),ranges.objects,1);
            ImGui::EndTable();
        }
    }
    ImGui::EndChild(); ImGui::End();
}
void Viewer::SelectTrace(SelectionKind kind,D::LiveId id)
{
    if(!playback.snapshot) return;
    state.selection={kind,id,playback.frame,playback.snapshot,playback.startedNs};
    indexedSequence=UINT64_MAX; panels[DetailsPanel]=true;
}
void Viewer::Flow()
{
    if(!ImGui::Begin("Flow / 데이터 흐름",&panels[FlowPanel])) { ImGui::End(); return; }
    ImGui::PushFont(nullptr,24); ImGui::TextUnformatted("3초 흐름 재생 / 실제 실행속도 아님"); ImGui::PopFont();
    if(ImGui::IsItemHovered() && playback.snapshot)
        ImGui::SetTooltip("원본 frame %llu / revision %llu / sequence %llu\n같은 snapshot과 실제 연결을 3초간 유지합니다.",playback.frame.frame,playback.revision,playback.snapshot->sequence);
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
            if(ImGui::IsItemHovered()) ImGui::SetTooltip("원본 ID %llu -> %llu\n독립 경로의 목록 순서는 실행 순서가 아닙니다.",route.links.front().from,route.links.back().to);
        }
        ImGui::EndCombo();
    }
    const auto& route=flowRoutes[selectedRoute];
    if(layoutRoute!=selectedRoute) { routeLayout=LayoutFlowRoute(route); layoutRoute=selectedRoute; }
    if(routeLayout.nodes.empty()) { ImGui::TextDisabled("연속된 원본 경로 미수집"); return; }
    ImGui::TextDisabled("%s · 수치는 완료된 1초 평균",route.recording ? "선택한 기록 연결만 표시" : "참조·의존 관계 / 실행 순서 아님");
    const float childHeight=std::max(1.f,ImGui::GetContentRegionAvail().y-(expandGraph ? 190.f : 0.f));
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
        const auto* averages=statistics.state.Source();
        for(size_t i=first;i<last;++i)
        {
            const int column=i ? route.steps[i-1].to : route.steps.front().from;
            const auto center=point(routeLayout.nodes[i]);
            draw->AddCircleFilled(center,routeLayout.Radius,IM_COL32(24,47,61,255));
            draw->AddCircle(center,routeLayout.Radius,column==selectedStage ? IM_COL32(255,211,112,255) : teal,64,3);
            TextAt(draw,{center.x,origin.y+12},26,ink,names[column],true);
            if(i && route.steps[i-1].from==column)
            {
                const auto repeat=std::string("반복 연결 ")+std::to_string(route.steps[i-1].links)+"개";
                TextAt(draw,{center.x,origin.y+46},20,muted,repeat.c_str(),true);
            }
            char number[96]="--",unit[32]="대기";
            if(column==FlowReferenceColumn) { std::snprintf(number,sizeof(number),"참조"); unit[0]=0; }
            else if(averages && averages->recent.frameCount)
            {
                if(ImGui::IsRectVisible({center.x-routeLayout.Radius,center.y-routeLayout.Radius},{center.x+routeLayout.Radius,center.y+routeLayout.Radius})) statisticsDrawn=true;
                const auto& a=averages->recent;
                const auto& timing=column==int(D::LiveStage::Gpu) ? a.gpuMeasured : a.stages[size_t(column)].cpu;
                if(column==int(D::LiveStage::Data))
                { std::snprintf(number,sizeof(number),a.recordedMean.batches>=100000 ? "%.3g" : "%.1f",a.recordedMean.batches); std::snprintf(unit,sizeof(unit),"batch/f"); }
                else if(column==int(D::LiveStage::Upload))
                { std::snprintf(number,sizeof(number),a.recordedMean.uploadBytes>=100000 ? "%.3g" : "%.0f",a.recordedMean.uploadBytes); std::snprintf(unit,sizeof(unit),"B/f"); }
                else if(Ready(timing)) { std::snprintf(number,sizeof(number),timing.average>=1000 ? "%.1f" : "%.3f",timing.average); std::snprintf(unit,sizeof(unit),"ms"); }
                else std::snprintf(unit,sizeof(unit),"%s",ValueState(timing.state));
            }
            TextAt(draw,{center.x,center.y-26},34,ink,number,true);
            TextAt(draw,{center.x,center.y+22},20,muted,unit,true);
            if(hovered && std::hypot(ImGui::GetIO().MousePos.x-center.x,ImGui::GetIO().MousePos.y-center.y)<routeLayout.Radius)
            {
                ImGui::SetTooltip("%s · 원본 ID %llu\nFrame %llu / revision %llu\n같은 단계의 재등장을 별도 위치에 표시합니다.\n단계 수치는 동일한 생산자 1초 평균이며 합산하지 않습니다.",names[column],routeLayout.ids[i],playback.frame.frame,playback.revision);
                if(ImGui::IsMouseClicked(ImGuiMouseButton_Left))
                {
                    if(column<FlowReferenceColumn) { selectedStage=column; panels[EventsPanel]=true; indexedSequence=UINT64_MAX; }
                    else panels[ObjectsPanel]=true;
                    const auto node=std::find_if(graph.nodes.begin(),graph.nodes.end(),[&](const auto& value){return value.id==routeLayout.ids[i];});
                    if(node!=graph.nodes.end() && node->kind!=FlowNodeKind::Unknown)
                        SelectTrace(node->kind==FlowNodeKind::Gpu ? SelectionKind::Gpu : node->kind==FlowNodeKind::Object ? SelectionKind::Object : SelectionKind::Event,
                            node->kind==FlowNodeKind::BatchReference || node->kind==FlowNodeKind::SubmissionReference ? source->flow.events[node->index].id : node->id);
                }
            }
        }
        const auto at=LocateFlowRoute(route,playback.Phase(D::LiveNowNs()));
        const auto dot=PointOnFlowPath(routeLayout.paths[at.step],at.phase);
        draw->AddCircleFilled(point(dot),8,IM_COL32(255,211,112,255));
    }
    ImGui::EndChild();
    if(expandGraph)
    {
        if(ImGui::BeginChild("##FlowIDs",{0,180},true))
        {
            ImGui::TextDisabled("모든 원본 항목 · 연결 ID는 hover로 확인");
            ImGuiListClipper clipper; clipper.Begin(int(graph.nodes.size()));
            while(clipper.Step()) for(int row=clipper.DisplayStart;row<clipper.DisplayEnd;++row)
            {
                const auto& node=graph.nodes[size_t(row)];
                const auto label=std::to_string(node.id)+" · "+names[node.column]+(node.kind==FlowNodeKind::Unknown ? " (metadata 미수집)" : "");
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
                    for(const auto& link:graph.links) if(link.from==node.id || link.to==node.id) ImGui::Text("%llu -> %llu",link.from,link.to);
                    ImGui::EndTooltip();
                }
            }
        }
        ImGui::EndChild();
    }
}
}
