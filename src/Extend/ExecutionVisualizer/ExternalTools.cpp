#include "Viewer.h"
#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

namespace viewer
{
void Viewer::ExternalTools()
{
    ImGui::TextWrapped("%s",automatic.Status().c_str());
    ImGui::BeginDisabled(automatic.Busy()||process.Running()||launch.executable.empty());
    if(ImGui::Button("자동 연결 / 재시도"))StartAutomatic();
    ImGui::EndDisabled();ImGui::SameLine();
    if(ImGui::Button("자동 연결 중단"))automatic.Cancel();
    if(ImGui::Checkbox("다음 분석기 실행 시 자동 연결",&automaticEnabled))
        if(!automatic.SetEnabled(automaticEnabled)) {automaticEnabled=automatic.Profile().enabled;toolIssue=automatic.Status();}
    ImGui::Separator();
    const bool connected=client.GetState()==D::LiveClientState::Connected;
    const auto find=[&](const char* key)->const D::ControlDesc* {
        if(state.snapshot) for(const auto& control:state.snapshot->controls)
            if(control.category==D::ControlCategory::Diagnostic && control.device==state.device && control.key==key) return &control;
        return nullptr;
    };
    bool pending=false;
    for(const auto& request:state.requests) if(request.request==toolRequest && request.device==state.device)
        pending=request.state==D::ControlState::Requested||request.state==D::ControlState::Validated||request.state==D::ControlState::PendingApply;
    const auto action=[&](const char* label,const char* key) {
        const auto* control=find(key);
        ImGui::BeginDisabled(!connected||!control||!control->writable||pending);
        if(ImGui::Button(label))
        {
            toolRequest=Send(*control,true);
            toolIssue=toolRequest ? "" : message;
            pending=toolRequest!=0;
        }
        ImGui::EndDisabled();
        if(control&&!control->writable&&!control->unavailableReason.empty())
            if(ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s",control->unavailableReason.c_str());
    };
    if(connected&&reconnect) ImGui::Text("대상 PID: %u",reconnect->processId);
    else ImGui::TextDisabled("대상 프로그램을 먼저 연결하세요.");
    if(!connected) if(const auto error=client.GetLastError();!error.empty()) ImGui::TextWrapped("%s",error.c_str());
    ImGui::SeparatorText("Tracy");
    const auto* tracy=find("tools.tracy");
    const auto* status=tracy ? std::get_if<int64_t>(&tracy->appliedValue) : nullptr;
    ImGui::TextUnformatted(embedded&&embedded->Connected() ? "내장 Tracy 수집 중" : embedded&&embedded->HasCapture() ? "내장 기록 보존 중" : !connected ? "대상 연결 대기" : !status ? "도구 정보 없음 · 대상 재빌드 필요" : *status==2 ? "다른 수신기 연결 확인 필요" : *status==1 ? "Tracy 연결 가능" : "대상 빌드에서 Tracy 꺼짐");
    const bool exactAutoTarget=connected&&reconnect&&automatic.Target().Running()&&reconnect->processId==automatic.Target().Pid();
    bool visible=tracyVisible;if(ImGui::Checkbox("Tracy 내부 탭",&visible))ShowTracy(visible);
    if(!exactAutoTarget)
    {
        int port=manualTracyPort;ImGui::SetNextItemWidth(160);
        if(ImGui::InputInt("대상 Tracy 포트",&port)&&port>0&&port<=65535)manualTracyPort=uint16_t(port);
    }
    ImGui::BeginDisabled(!connected||!embedded||!DY_VIEWER_EMBED_TRACY);
    if(ImGui::Button("내장 연결 / 재시도")) {tracyCollectRequested=true;tracyConnectPending=true;tracyVisible=true;tracyIssue.clear();}
    ImGui::EndDisabled();ImGui::SameLine();
    ImGui::BeginDisabled(!embedded||!embedded->Connected());
    if(ImGui::Button("수집 종료")) {embedded->Stop();tracyCollectRequested=false;tracyConnectPending=false;}
    ImGui::EndDisabled();
    action("Tracy 표식 남기기","tools.tracy.bookmark");
    if(exactAutoTarget)ImGui::TextWrapped("자동 세션: 게임 PID %u의 전용 포트 %u로 연결합니다.",reconnect->processId,unsigned(automatic.TracyPort()));
    else if(connected&&reconnect) ImGui::TextWrapped("수동 세션: 게임 PID %u가 소유한 로컬 포트를 확인한 뒤 연결합니다.",reconnect->processId);
    if(automatic.LegacyTracyRunning())ImGui::TextWrapped("이전 외부 Tracy가 실행 중입니다. 받은 기록과 프로세스를 보존하며 자동 종료하지 않습니다.");
    ImGui::TextWrapped("탭을 숨겨도 수집은 계속됩니다. 기록 저장·열기는 Tracy 원래 화면에서 사용할 수 있습니다.");
    if(!tracyIssue.empty())ImGui::TextWrapped("%s",tracyIssue.c_str());
    if(embedded&&!embedded->LastError().empty())ImGui::TextWrapped("%s",embedded->LastError().c_str());
    ImGui::SeparatorText("RenderDoc");
    const auto* capture=find("tools.renderdoc.capture");
    const auto* replay=find("tools.renderdoc.connected");
    const auto* replayConnected=replay ? std::get_if<bool>(&replay->appliedValue) : nullptr;
    ImGui::TextUnformatted(!connected ? "연결 상태 미확인" : !state.snapshot ? "대상 스냅샷 대기" : !capture ? "도구 정보 없음 · 대상 재빌드 필요" : capture->writable ? "캡처 가능" : "캡처 불가");
    if(connected&&capture&&!capture->writable) ImGui::TextWrapped("%s",capture->unavailableReason.c_str());
    ImGui::SetNextItemWidth(-1);ImGui::InputTextWithHint("##RenderDocPath","qrenderdoc.exe 경로",&renderDocExecutable);
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("qrenderdoc.exe 실행 파일 경로");
    ImGui::BeginDisabled(renderDocExecutable.empty()||renderDocProcess.Running());
    if(ImGui::Button("RenderDoc 실행")) renderDocProcess.Launch(renderDocExecutable,"","",toolIssue);
    ImGui::EndDisabled();
    if(renderDocProcess.Running()) ImGui::TextDisabled("이 뷰어에서 실행한 RenderDoc PID: %u",renderDocProcess.Pid());
    action("이 게임에 RenderDoc 연결","tools.renderdoc.connect");
    if(connected&&replayConnected) {ImGui::SameLine();ImGui::TextDisabled(*replayConnected ? "대상 UI 연결됨" : "대상 UI 연결 대기");}
    action("다음 프레임 캡처","tools.renderdoc.capture");ImGui::SameLine();action("캡처 목록 열기","tools.renderdoc.open");
    if(connected) if(const auto* count=find("tools.renderdoc.captures"))
        if(const auto* value=std::get_if<int64_t>(&count->appliedValue)) ImGui::Text("완료된 캡처: %lld",static_cast<long long>(*value));
    ImGui::TextWrapped("자동 연결은 RenderDoc을 통해 게임을 시작하고 해당 게임에 도구 UI를 연결합니다. 수동 실행은 RenderDoc의 Launch Application도 사용할 수 있습니다.");
    ImGui::TextWrapped("캡처 목록 열기 후 기존 RenderDoc 창의 Capture List를 확인하고 캡처를 직접 선택하세요. 새 UI 실행 요청만으로 연결 완료를 보장하지 않습니다.");
    // Send() can reallocate requests, so resolve the result after the buttons.
    const D::ControlStatus* last=nullptr;
    for(const auto& request:state.requests) if(request.request==toolRequest && request.device==state.device) last=&request;
    if(last)
    {
        ImGui::Separator();
        if(!connected) ImGui::TextDisabled(pending ? "연결 종료 · 요청 결과 미확인" : "연결 종료 · 마지막 요청 결과");
        ImGui::TextUnformatted(last->state==D::ControlState::Applied ? "요청 처리 완료" : ControlState(last->state));
        if(!last->message.empty()) ImGui::TextWrapped("%s",last->message.c_str());
        if(connected&&last->state==D::ControlState::Applied&&capture&&last->control==capture->id)
            ImGui::TextDisabled("캡처 완료는 위 건수 증가와 RenderDoc에서 확인합니다.");
    }
    if(!toolIssue.empty()) ImGui::TextWrapped("%s",toolIssue.c_str());
}
}
