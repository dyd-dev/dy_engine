#include "Viewer.h"
#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>
#include <windows.h>
#include <shobjidl.h>
#include <wrl/client.h>
#include <algorithm>
#include <climits>

namespace viewer
{
Viewer::~Viewer() { if(cancelExecutableScan) cancelExecutableScan->store(true); }
void Viewer::SetLaunchExecutable(const std::string& value)
{
    launch.SelectExecutable(value); manualExecutable=launch.executable; launchIssue.clear();
    if(executableFolder.empty()) executableFolder=launch.directory;
}
void Viewer::StartExecutableScan()
{
    if(executableScan.valid()) return;
    scanQuery=executableFolder;
    cancelExecutableScan=std::make_shared<std::atomic_bool>(false);
    executableCatalog={}; launchIssue.clear();
    const auto folder=scanQuery; const auto cancellation=cancelExecutableScan;
    try { executableScan=std::async(std::launch::async,[folder,cancellation] { return ScanExecutables(folder,cancellation.get()); }); }
    catch(const std::exception& error) { executableCatalog.error=error.what(); }
}
void Viewer::PollExecutableScan()
{
    if(!executableScan.valid() || executableScan.wait_for(std::chrono::seconds(0))!=std::future_status::ready) return;
    try
    {
        auto result=executableScan.get();
        if(scanQuery==executableFolder) executableCatalog=std::move(result);
    }
    catch(const std::exception& error) { executableCatalog.error=error.what(); }
}
static bool FolderPicker(void* owner,std::string& path,std::string& error)
{
    struct ComScope
    {
        HRESULT result=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED|COINIT_DISABLE_OLE1DDE);
        ~ComScope() { if(SUCCEEDED(result)) CoUninitialize(); }
    } com;
    if(FAILED(com.result)) { error="폴더 선택 COM 초기화 실패: "+std::to_string(com.result); return false; }
    Microsoft::WRL::ComPtr<IFileOpenDialog> dialog;
    auto result=CoCreateInstance(CLSID_FileOpenDialog,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(dialog.GetAddressOf()));
    if(SUCCEEDED(result)) result=dialog->SetOptions(FOS_PICKFOLDERS|FOS_FORCEFILESYSTEM|FOS_PATHMUSTEXIST|FOS_DONTADDTORECENT);
    if(SUCCEEDED(result)) result=dialog->Show(static_cast<HWND>(owner));
    if(result==HRESULT_FROM_WIN32(ERROR_CANCELLED)) return false;
    Microsoft::WRL::ComPtr<IShellItem> item;
    if(SUCCEEDED(result)) result=dialog->GetResult(item.GetAddressOf());
    PWSTR selected=nullptr;
    if(SUCCEEDED(result)) result=item->GetDisplayName(SIGDN_FILESYSPATH,&selected);
    if(FAILED(result)) { error="폴더 선택 실패: "+std::to_string(result); return false; }
    try { path=ToUtf8(selected); } catch(...) { CoTaskMemFree(selected); throw; }
    CoTaskMemFree(selected); return true;
}
void Viewer::LaunchDialog()
{
    if(openLaunchDialog) { ImGui::OpenPopup("Launch setup / 실행 설정"); openLaunchDialog=false; }
    const auto* viewport=ImGui::GetMainViewport();
    const ImVec2 maximum{std::max(320.f,viewport->WorkSize.x-24),std::max(240.f,viewport->WorkSize.y-24)};
    ImGui::SetNextWindowSize({std::min(1100.f,maximum.x),std::min(720.f,maximum.y)},ImGuiCond_Appearing);
    ImGui::SetNextWindowSizeConstraints({std::min(640.f,maximum.x),std::min(380.f,maximum.y)},maximum);
    ImGui::SetNextWindowPos({viewport->WorkPos.x+viewport->WorkSize.x*.5f,viewport->WorkPos.y+viewport->WorkSize.y*.5f},ImGuiCond_Appearing,{.5f,.5f});
    bool open=true;
    if(!ImGui::BeginPopupModal("Launch setup / 실행 설정",&open)) return;
    const auto footerHeight=ImGui::GetFrameHeightWithSpacing()+ImGui::GetStyle().ItemSpacing.y;
    ImGui::BeginChild("##LaunchSetupBody",{0,-footerHeight});
    const auto folderChanged=[&] {
        if(cancelExecutableScan) cancelExecutableScan->store(true);
        executableCatalog={}; launch.SelectExecutable(""); manualExecutable.clear(); launchIssue.clear();
    };
    ImGui::TextUnformatted("1. 폴더 지정 -> 2. 하위 .exe 검색 -> 3. 선택 -> 4. 인자 조정 -> Launch");
    ImGui::SetNextItemWidth(std::max(120.f,ImGui::GetContentRegionAvail().x-180));
    if(ImGui::InputText("##Folder",&executableFolder)) folderChanged();
    ImGui::SameLine();
    if(ImGui::Button("Browse / 폴더 선택"))
    {
        try { if(FolderPicker(ownerWindow,executableFolder,launchIssue)) folderChanged(); }
        catch(const std::exception& error) { launchIssue=error.what(); }
    }
    ImGui::BeginDisabled(executableScan.valid());
    if(ImGui::Button("Scan / 하위 폴더 재검색")) StartExecutableScan();
    ImGui::EndDisabled(); ImGui::SameLine();
    if(executableScan.valid())
    { if(ImGui::Button("Cancel scan / 검색 취소")) cancelExecutableScan->store(true); ImGui::SameLine(); ImGui::TextDisabled("검색 중..."); }
    else ImGui::Text(".exe %zu개 | 오류 %llu | 링크 폴더 생략 %llu%s",executableCatalog.files.size(),executableCatalog.errors,executableCatalog.skippedLinks,executableCatalog.cancelled ? " | 취소된 부분 결과" : "");
    if(!executableCatalog.error.empty()) ImGui::TextWrapped("%s",executableCatalog.error.c_str());
    if(!executableCatalog.issues.empty() && ImGui::CollapsingHeader("부분 검색 / 생략 상세"))
    {
        ImGui::BeginChild("##ScanIssues",{0,70},ImGuiChildFlags_Borders);
        for(const auto& issue:executableCatalog.issues) ImGui::TextWrapped("%s",issue.c_str());
        if(executableCatalog.errors+executableCatalog.skippedLinks>executableCatalog.issues.size()) ImGui::TextDisabled("상세는 처음 32건까지 표시합니다.");
        ImGui::EndChild();
    }
    const float bodyHeight=std::max(150.f,ImGui::GetContentRegionAvail().y-205.f);
    if(ImGui::BeginTable("##LaunchColumns",2,ImGuiTableFlags_Resizable|ImGuiTableFlags_SizingStretchProp))
    {
        ImGui::TableSetupColumn("실행 파일",ImGuiTableColumnFlags_WidthStretch,.43f);
        ImGui::TableSetupColumn("실행 인자",ImGuiTableColumnFlags_WidthStretch,.57f);
        ImGui::TableNextColumn(); ImGui::TextUnformatted("상대 경로 목록 (예제 필터 없음)");
        ImGui::BeginChild("##ExecutableCatalog",{0,bodyHeight},ImGuiChildFlags_Borders,ImGuiWindowFlags_HorizontalScrollbar);
        ImGuiListClipper clipper; clipper.Begin(int(std::min(executableCatalog.files.size(),size_t(INT_MAX))));
        while(clipper.Step()) for(int i=clipper.DisplayStart;i<clipper.DisplayEnd;++i)
        {
            const auto& file=executableCatalog.files[size_t(i)];
            ImGui::PushID(i);
            const auto width=std::max(ImGui::GetContentRegionAvail().x,ImGui::CalcTextSize(file.relativePath.c_str()).x+16);
            if(ImGui::Selectable(file.relativePath.c_str(),launch.executable==file.path,0,{width,0}))
            { try { SetLaunchExecutable(file.path); } catch(const std::exception& error) { launchIssue=error.what(); } }
            if(ImGui::IsItemHovered()) ImGui::SetTooltip("%s",file.path.c_str());
            ImGui::PopID();
        }
        ImGui::EndChild();
        ImGui::TableNextColumn(); ImGui::TextUnformatted("인자 이름 / 종류 / 값");
        ImGui::BeginChild("##LaunchArguments",{0,bodyHeight},ImGuiChildFlags_Borders);
        ImGui::PushID(launch.executable.c_str());
        ImGui::Checkbox("Raw command / 직접 문자열",&launch.useRaw);
        if(!launch.description.empty()) ImGui::TextWrapped("%s",launch.description.c_str());
        if(launch.useRaw) { ImGui::SetNextItemWidth(-1); ImGui::InputText("##RawArguments",&launch.rawArguments); }
        else
        {
            ImGui::TextWrapped("%s",launch.known ? "현재 소스 기준 프리셋. 동명 다른 프로그램은 직접 문자열을 사용하세요." : "미등록 exe: 이름·종류·값을 직접 추가하세요. 이름 끝 = 는 한 토큰으로 결합합니다.");
            if(!launch.source.empty() && ImGui::IsItemHovered()) ImGui::SetTooltip("%s",launch.source.c_str());
            int erase=-1;
            if(ImGui::BeginTable("##ArgumentRows",5,ImGuiTableFlags_SizingStretchProp))
            {
                ImGui::TableSetupColumn("On",ImGuiTableColumnFlags_WidthFixed,24);
                ImGui::TableSetupColumn("Name",ImGuiTableColumnFlags_WidthStretch,1.2f);
                ImGui::TableSetupColumn("Type",ImGuiTableColumnFlags_WidthFixed,65);
                ImGui::TableSetupColumn("Value",ImGuiTableColumnFlags_WidthStretch,1);
                ImGui::TableSetupColumn("",ImGuiTableColumnFlags_WidthFixed,22);
                ImGui::TableHeadersRow();
                for(size_t i=0;i<launch.arguments.size();++i)
                {
                    auto& argument=launch.arguments[i]; ImGui::PushID(int(i));
                    ImGui::TableNextColumn(); ImGui::Checkbox("##Enabled",&argument.enabled);
                    ImGui::TableNextColumn(); ImGui::SetNextItemWidth(-1);
                    if(launch.known) ImGui::TextUnformatted(argument.name.c_str()); else ImGui::InputText("##Name",&argument.name);
                    if(ImGui::IsItemHovered() && !argument.description.empty()) ImGui::SetTooltip("%s",argument.description.c_str());
                    ImGui::TableNextColumn();
                    static const char* kinds[]={"flag","text","integer","float","choice"};
                    if(launch.known) ImGui::TextUnformatted(kinds[size_t(argument.type)]);
                    else { int type=int(argument.type); ImGui::SetNextItemWidth(-1); if(ImGui::Combo("##Type",&type,"flag\0text\0integer\0float\0")) argument.type=ArgumentType(type); }
                    ImGui::TableNextColumn(); ImGui::SetNextItemWidth(-1); ImGui::BeginDisabled(!argument.enabled);
                    if(argument.type==ArgumentType::Choice)
                    {
                        if(ImGui::BeginCombo("##Value",argument.value.c_str()))
                        { for(const auto& choice:argument.choices) if(ImGui::Selectable(choice.c_str(),choice==argument.value)) argument.value=choice; ImGui::EndCombo(); }
                    }
                    else if(argument.type!=ArgumentType::Flag) ImGui::InputText("##Value",&argument.value);
                    else ImGui::TextDisabled("체크=포함");
                    ImGui::EndDisabled(); ImGui::TableNextColumn();
                    if(!launch.known && ImGui::SmallButton("X")) erase=int(i);
                    ImGui::PopID();
                }
                ImGui::EndTable();
            }
            if(erase>=0) launch.arguments.erase(launch.arguments.begin()+erase);
            if(!launch.known && ImGui::Button("Add argument / 인자 추가")) { LaunchArgument argument; argument.name="--option"; launch.arguments.push_back(std::move(argument)); }
            if(launch.known && launch.arguments.empty()) ImGui::TextWrapped("이 실행 파일은 원본 소스에 인자 파서가 없습니다.");
        }
        ImGui::PopID(); ImGui::EndChild(); ImGui::EndTable();
    }
    if(ImGui::CollapsingHeader("직접 실행 파일 경로"))
    {
        ImGui::SetNextItemWidth(std::max(120.f,ImGui::GetContentRegionAvail().x-160)); ImGui::InputText("##ManualExe",&manualExecutable); ImGui::SameLine();
        if(ImGui::Button("Select exe / 선택")) { try { SetLaunchExecutable(manualExecutable); } catch(const std::exception& error) { launchIssue=error.what(); } }
    }
    ImGui::TextUnformatted("Working directory / 작업 폴더 (빈 값 = 선택 exe의 폴더)");
    ImGui::SetNextItemWidth(-1); ImGui::InputText("##WorkingDirectory",&launch.directory);
    std::string arguments,error; const bool valid=launch.BuildArguments(arguments,error);
    std::string preview;
    try { if(!launch.executable.empty()) preview=ToUtf8(QuoteArgument(ToWide(launch.executable)))+(arguments.empty() ? "" : " "+arguments); }
    catch(const std::exception& exception) { error=exception.what(); }
    ImGui::TextUnformatted("전체 명령 미리보기");
    ImGui::BeginChild("##CommandPreview",{0,48},ImGuiChildFlags_Borders,ImGuiWindowFlags_HorizontalScrollbar); ImGui::TextUnformatted(preview.c_str()); ImGui::EndChild();
    ImGui::EndChild();
    ImGui::BeginDisabled(process.Running() || launch.executable.empty() || !valid || !error.empty());
    if(ImGui::Button("Launch / 선택 exe 실행")) Launch();
    ImGui::EndDisabled(); ImGui::SameLine();
    if(ImGui::Button("Close / 닫기")) ImGui::CloseCurrentPopup();
    const auto& footerError=error.empty() ? launchIssue : error;
    if(!footerError.empty())
    {
        ImGui::SameLine();
        ImGui::BeginChild("##LaunchError",{0,ImGui::GetFrameHeight()},ImGuiChildFlags_None,ImGuiWindowFlags_NoScrollbar|ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::AlignTextToFramePadding(); ImGui::TextColored({1,.69f,.3f,1},"%s",footerError.c_str());
        if(ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem))
            ImGui::SetTooltip("%s%s%s",error.c_str(),error.empty() || launchIssue.empty() ? "" : "\n",launchIssue.c_str());
        ImGui::EndChild();
    }
    else if(process.Running()) { ImGui::SameLine(); ImGui::TextDisabled("직접 실행한 PID %u 실행 중",process.Pid()); }
    ImGui::EndPopup();
}
}
