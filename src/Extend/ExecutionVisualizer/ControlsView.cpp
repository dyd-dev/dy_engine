#include "Viewer.h"
#include <imgui.h>
#include <algorithm>
#include <cmath>

namespace viewer
{
static const ImVec4 accent{.22f,.76f,.75f,1},warning{1,.69f,.3f,1};
struct FeatureInfo { const char* key; const char* title; const char* description; };
static const FeatureInfo* Feature(const std::string& key)
{
    static constexpr FeatureInfo values[]{
        {"culling","프러스텀 컬링","카메라 시야 밖 객체를 그리기 대상에서 제외합니다."},
        {"model.skinning","스키닝 처리 방식","정점 변형을 버텍스 셰이더 또는 컴퓨트 셰이더에서 처리합니다."},
        {"instancing","인스턴싱","같은 메시의 여러 인스턴스를 묶어 Draw 호출 수를 줄입니다."},
        {"example14.instancing","인스턴싱","256개 인스턴스의 개별 Draw와 묶음 Draw를 전환합니다."},
        {"example15.reuse","버퍼 재사용","인스턴스 버퍼를 매 프레임 새로 만들거나 기존 버퍼를 재사용합니다."},
        {"parallelRecording","명령 병렬 기록","등록된 작업 스레드로 렌더 명령을 병렬 기록합니다."},
        {"rhi.parallelRecording","RenderGraph 병렬 기록","RenderGraph의 CPU pass 기록과 네이티브 명령 변환을 직렬 또는 병렬로 수행합니다."},
        {"lighting","조명","실제 Renderer 조명 설정을 전환합니다."},
        {"shadows","그림자","그림자 렌더링을 전환합니다."},
        {"shadowQuality","그림자 품질","등록된 그림자 품질 단계를 선택합니다."},
        {"hdr","HDR 렌더링","HDR 렌더 타깃 사용을 전환합니다."},
        {"exposure","노출","실제 적용할 노출 값을 지정합니다."},
        {"vsync","수직 동기화","제작물의 실제 표시 동기화 설정을 변경합니다."},
        {"simd","SIMD","현재 빌드에서 선택한 SIMD 상태입니다."},
        {"optimization.transformUploads","변경 transform 업로드",""},
        {"optimization.resourceSetCache","ResourceSet 재사용",""},
        {"optimization.bindlessMaterials","바인드리스 재질",""},
        {"canvas.batching","Canvas 인접 draw 병합",""},
        {"canvas.imageCache","Canvas 이미지 캐시",""},
        {"canvas.vertexBufferReuse","Canvas 정점 버퍼 재사용",""},
        {"model.frameBufferReuse","Model frame-slot 버퍼 재사용",""},
        {"model.skipUnchangedUploads","동일 바이트 업로드 생략",""},
        {"model.staticMeshCache","정적 메시 재사용",""},
        {"model.influenceCache","스키닝 influence 재사용",""},
        {"rhi.uploadPageReuse","업로드 페이지 재사용",""},
        {"rhi.uploadSuballocation","업로드 페이지 내 서브할당",""}};
    for(const auto& value:values) if(key==value.key) return &value;
    return nullptr;
}
static const char* FeatureTitle(const D::ControlDesc& control)
{
    if(const auto* info=Feature(control.key)) return info->title;
    return control.label.empty() ? control.key.c_str() : control.label.c_str();
}
static const char* UnavailableReason(const std::string& reason)
{
    if(reason.rfind("Stock Scene geometry only",0)==0) return "이 제작물의 Model·사용자 지정 그리기 경로에는 적용되지 않습니다.";
    if(reason=="No caller-owned ThreadPool") return "병렬 기록에 필요한 작업 스레드가 등록되지 않았습니다.";
    if(reason=="SIMD is selected at compile time") return "SIMD는 빌드 시 결정되며 실행 중 변경할 수 없습니다.";
    return reason.c_str();
}
static std::string FeatureValue(const D::ControlDesc& control,const D::ControlValue& value)
{
    if(const auto* boolean=std::get_if<bool>(&value)) return *boolean ? "켜짐" : "꺼짐";
    if(const auto* index=std::get_if<int64_t>(&value))
    {
        if(control.kind==D::ControlKind::Enumeration)
        {
            if(control.key=="model.skinning" && (*index==0 || *index==1)) return *index==0 ? "버텍스 셰이더 (VS)" : "컴퓨트 셰이더 (Compute)";
            if(control.key=="shadowQuality" && *index>=0 && *index<=2) return *index==0 ? "낮음" : *index==1 ? "중간" : "높음";
            for(const auto& choice:control.choices) if(choice.value==*index) return choice.label;
        }
    }
    return ValueText(value);
}
static const char* Category(const D::ControlDesc& control)
{
    if(control.category==D::ControlCategory::Optimization) return "최적화";
    if(control.category==D::ControlCategory::OutputQuality) return "품질·표시 조건";
    if(control.category==D::ControlCategory::BuildOrStructural) return "빌드·고정 구조";
    return "제작물 등록 / 미분류";
}
void Viewer::Controls()
{
    if(!ImGui::Begin("Controls / 실제 적용")) { ImGui::End(); return; }
    const bool connected = client.GetState() == D::LiveClientState::Connected;
    std::vector<const D::ControlDesc*> controls;
    if(state.snapshot) for(const auto& control : state.snapshot->controls)
    {
        if(control.category==D::ControlCategory::Diagnostic) continue;
        if(control.device && control.device != state.device) continue;
        if(control.source && control.source != state.source) continue;
        controls.push_back(&control);
    }
    if(controls.empty())
    {
        ImGui::TextWrapped("선택 source에 조절 항목이 없습니다. 작업은 Overview·Flow에서 확인하세요.");
        if(ImGui::CollapsingHeader("적용 요청 내역")) Requests();
        ImGui::End(); return;
    }
    auto selected=std::find_if(controls.begin(),controls.end(),[&](const auto* value){return value->id==selectedControl;});
    if(selected==controls.end())
    {
        if(const auto* applied=state.Comparison()) selected=std::find_if(controls.begin(),controls.end(),[&](const auto* value){return value->id==applied->control;});
        if(selected==controls.end()) selected=controls.begin();
        selectedControl=(*selected)->id;
    }
    const bool compact=ImGui::GetContentRegionAvail().x<1000;
    const auto entries=[&] {
        for(const auto* control:controls)
        {
            ImGui::PushID(reinterpret_cast<void*>(static_cast<uintptr_t>(control->id)));
            if(ImGui::Selectable(FeatureTitle(*control),selectedControl==control->id)) selectedControl=control->id;
            if(ImGui::IsItemHovered()) ImGui::SetTooltip("%s · %s%s\n%s",Category(*control),Application(control->application),control->writable ? "" : " · 읽기 전용",control->key.c_str());
            ImGui::PopID();
        }
    };
    if(compact)
    {
        ImGui::Text("조절 항목 %zu개",controls.size()); ImGui::SameLine(); ImGui::SetNextItemWidth(-1);
        const auto* current=*std::find_if(controls.begin(),controls.end(),[&](const auto* control){return control->id==selectedControl;});
        if(ImGui::BeginCombo("##FeatureSelectionCombo",FeatureTitle(*current))) { entries(); ImGui::EndCombo(); }
    }
    else
    {
        const float listWidth=std::min(240.f,ImGui::GetContentRegionAvail().x*.23f);
        if(ImGui::BeginChild("##FeatureSelection",{listWidth,0},true))
        {
            ImGui::Text("조절 항목 %zu개",controls.size()); entries();
        }
        ImGui::EndChild(); ImGui::SameLine();
    }
    if(ImGui::BeginChild("##FeatureAnalysis",{0,0}))
    {
        const auto& control=**std::find_if(controls.begin(),controls.end(),[&](const auto* value){return value->id==selectedControl;});
        ImGui::PushID(reinterpret_cast<void*>(static_cast<uintptr_t>(control.id)));
        auto inserted = drafts.try_emplace(control.id, Draft{control.appliedValue, false}); auto& draft = inserted.first->second;
        if(draft.pending) for(const auto& status : state.requests) if(status.request == draft.pending
            && (status.state == D::ControlState::Applied || status.state == D::ControlState::Rejected
                || status.state == D::ControlState::Failed || status.state == D::ControlState::Cancelled)) { draft.pending = 0; break; }
        if(!draft.dirty && !draft.pending && !ImGui::IsAnyItemActive()) draft.value = control.appliedValue;
        ImGui::PushFont(nullptr,26); ImGui::TextWrapped("%s",FeatureTitle(control)); ImGui::PopFont();
        ImGui::TextDisabled("%s · %s",Category(control),Application(control.application));
        const bool runtime = control.application == D::ControlApplication::Immediate || control.application == D::ControlApplication::ResourceRebuild;
        if(control.writable && runtime) ImGui::TextColored(accent,"현재: %s",FeatureValue(control,control.appliedValue).c_str());
        else ImGui::TextDisabled("등록된 설정값: %s",FeatureValue(control,control.appliedValue).c_str());
        if(!control.writable || !runtime)
        {
            ImGui::TextDisabled("읽기 전용 / 런타임 변경 불가");
            if(!control.unavailableReason.empty()) ImGui::TextWrapped("%s",UnavailableReason(control.unavailableReason));
        }
        else
        {
        const bool editable = connected && ValidControlValue(control, draft.value);
        ImGui::BeginDisabled(!connected || !control.writable || !runtime);
        bool changed = false;
        if(control.kind == D::ControlKind::Boolean)
        {
            if(auto* value = std::get_if<bool>(&draft.value)) changed = ImGui::Checkbox("요청: 켜기", value);
        }
        else if(control.kind == D::ControlKind::Integer)
        {
            if(auto* value = std::get_if<int64_t>(&draft.value)) { ImGui::SetNextItemWidth(-1); changed = ImGui::InputScalar("##Value", ImGuiDataType_S64, value); }
            ImGui::TextDisabled("Range %.0f .. %.0f", control.minimum, control.maximum);
        }
        else if(control.kind == D::ControlKind::Real)
        {
            if(auto* value = std::get_if<double>(&draft.value)) { ImGui::SetNextItemWidth(-1); changed = ImGui::InputScalar("##Value", ImGuiDataType_Double, value, nullptr, nullptr, "%.4f"); }
            ImGui::TextDisabled("Range %g .. %g", control.minimum, control.maximum);
        }
        else if(auto* value = std::get_if<int64_t>(&draft.value))
        {
            std::string preview=FeatureValue(control,draft.value);
            ImGui::SetNextItemWidth(-1);
            if(ImGui::BeginCombo("##Value", preview.c_str()))
            {
                for(const auto& choice : control.choices) if(ImGui::Selectable(FeatureValue(control,choice.value).c_str(), *value == choice.value)) { *value = choice.value; changed = true; }
                ImGui::EndCombo();
            }
        }
        if(changed) draft.dirty = true;
        ImGui::BeginDisabled(!draft.dirty || !editable);
        if(ImGui::Button("적용 요청")) if(const auto request = Send(control, draft.value)) { draft.pending = request; draft.dirty = false; }
        ImGui::EndDisabled(); ImGui::EndDisabled();
        if(!control.unavailableReason.empty()) ImGui::TextWrapped("%s",UnavailableReason(control.unavailableReason));
        }
        for(auto it = state.requests.rbegin(); it != state.requests.rend(); ++it) if(it->control == control.id)
        {
            ImGui::TextColored(it->state==D::ControlState::Applied ? accent : warning,"%s",ControlState(it->state));
            if(ImGui::IsItemHovered()) ImGui::SetTooltip("이 조절 항목의 실제 적용 요청 상태입니다.");
            if(it->state == D::ControlState::Applied)
            {
                ImGui::Text("실제 결과: %s",FeatureValue(control,it->actual).c_str());
                if(ImGui::IsItemHovered()) ImGui::SetTooltip("프로그램의 실제 준비 경계에서 적용된 결과입니다.");
            }
            else if((it->state == D::ControlState::Requested || it->state == D::ControlState::Validated || it->state == D::ControlState::PendingApply)
                && D::LiveNowNs() > it->requestedNs + 2000000000) ImGui::TextColored(warning, "상태 갱신 지연 / 결과 미확인");
            if(!it->message.empty()) ImGui::TextWrapped("%s", it->message.c_str());
            break;
        }
        if(ImGui::CollapsingHeader("역할·처리 경로·적용 조건"))
        {
            if(!control.description.empty()) ImGui::TextWrapped("%s",control.description.c_str());
            else if(const auto* info=Feature(control.key); info && *info->description) ImGui::TextWrapped("%s",info->description);
            const auto text=[](const char* label,const std::string& value) {
                ImGui::TextColored(accent,"%s",label); ImGui::TextWrapped("%s",value.empty() ? "제작물이 설명을 등록하지 않았습니다." : value.c_str());
            };
            text("ON / 선택 경로",control.onPath); text("OFF / 대체 경로",control.offPath);
            text("적용 조건",control.conditions); text("추가 비용·교환 조건",control.tradeoffs);
        }
        ControlComparison();
        if(ImGui::CollapsingHeader("적용 요청 내역")) Requests();
        ImGui::PopID();
    }
    ImGui::EndChild();
    ImGui::End();
}
}
