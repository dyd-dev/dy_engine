#include "EmbeddedTracy.h"
#include "Process.h"
#include <imgui.h>
#include <stdexcept>
#include <utility>

#if DY_VIEWER_EMBED_TRACY
#include "TracyPlatform.h"
#include "Fonts.hpp"
#include "profiler/TracyConfig.hpp"
#include "profiler/TracyFileselector.hpp"
#include "profiler/TracyTexture.hpp"
#include "profiler/TracyView.hpp"
#include "TracyFileRead.hpp"
#include "TracyFileWrite.hpp"
#include "dyf/RHI/IDevice.h"
#include "imgui_internal.h"
#include <chrono>
#include <filesystem>
#include <mutex>
#include <thread>
#include <vector>
#include <windows.h>
#endif

namespace viewer
{
#if DY_VIEWER_EMBED_TRACY
struct EmbeddedTracy::Impl
{
    dyf::Gui& gui;
    dyf::RHI::IDevice& device;
    TracyPlatform graphics;
    std::unique_ptr<tracy::View> view;
    std::string error;
    uint32_t pid = 0;
    uint64_t creationTime = 0;
    uint16_t port = 0;
    bool stopped = false, closePending = false, closeApproved = false, closeCancelled = false;
    int mouseFrame = -1;
    std::thread::id owner = std::this_thread::get_id();
    std::mutex tasksMutex;
    std::vector<std::function<void()>> tasks;
    std::function<void()> afterClose;
    static Impl* active;

    Impl(dyf::Gui& g, dyf::RHI::IDevice& d) : gui(g), device(d), graphics(g, d)
    {
        if(active) throw std::logic_error("Only one embedded Tracy session is supported");
        tracy::s_config.memoryLimit = true;
        tracy::s_config.memoryLimitPercent = 10;
        tracy::LoadConfig();
        tracy::s_config.llm = false;
        tracy::Fileselector::Init();
        active = this;
    }
    ~Impl()
    {
        view.reset(); // Shuts down and joins Worker and original save/background jobs first.
        Drain();
        tracy::SaveConfig();
        tracy::Fileselector::Shutdown();
        active = nullptr;
        // graphics drains retired textures while Gui and device are still alive.
    }
    void CheckThread() const
    {
        if(std::this_thread::get_id() != owner) throw std::logic_error("Embedded Tracy must run on the Gui thread");
    }
    static void Run(const std::function<void()>& task, bool delay)
    {
        auto* self = active;
        if(!self) return;
        if(!delay && std::this_thread::get_id() == self->owner) { task(); return; }
        std::lock_guard<std::mutex> lock(self->tasksMutex);
        self->tasks.push_back(task);
    }
    void Drain()
    {
        std::vector<std::function<void()>> ready;
        { std::lock_guard<std::mutex> lock(tasksMutex); ready.swap(tasks); }
        for(auto& task : ready) task();
    }
    static void Attention() {}
    static void Scale(float)
    {
        if(active) active->error = "내장 보기의 실행 중 글꼴 배율 변경은 지원하지 않습니다. 타임라인 확대·축소를 사용하세요.";
    }
    bool Dirty() const
    {
        if(!view || !view->GetEmbeddedWorker().HasData()) return false;
        if(view->EmbeddedSaveFailed() || view->EmbeddedSavePending()) return true;
        auto& worker = view->GetEmbeddedWorker();
        if(!stopped && worker.IsConnected()) return true;
        std::unique_lock<std::mutex> lock(worker.GetDataLock(), std::try_to_lock);
        return !lock.owns_lock() || worker.GetLastTime() > view->EmbeddedSavedTime();
    }
};
EmbeddedTracy::Impl* EmbeddedTracy::Impl::active = nullptr;

bool EmbeddedTracy::PrepareFonts(std::string& error)
{
    if(!ImGui::GetCurrentContext()) { error = "Tracy 글꼴 준비에는 공용 ImGui 컨텍스트가 필요합니다."; return false; }
    if(ImGui::GetFrameCount() != 0 || ImGui::GetIO().Fonts->TexRef.GetTexID() != 0)
    { error = "Tracy 글꼴은 첫 GUI 프레임 전에 준비해야 합니다. 분석기를 다시 시작하세요."; return false; }
    for(auto* font : ImGui::GetIO().Fonts->Fonts)
        if(font == g_fonts.normal) { error.clear(); return true; }
    try { LoadFonts(1); error.clear(); return true; }
    catch(const std::exception& ex) { error = ex.what(); return false; }
}
EmbeddedTracy::EmbeddedTracy(dyf::Gui& gui, dyf::RHI::IDevice& device) : impl(std::make_unique<Impl>(gui, device)) {}
EmbeddedTracy::~EmbeddedTracy() = default;

bool EmbeddedTracy::Connect(uint32_t pid, uint64_t creationTime, uint16_t port, std::string& error)
{
    auto& s = *impl; s.CheckThread();
    if(!pid || !creationTime || !port || !SameRunningProcess(pid, creationTime))
    { error = s.error = "Tracy 연결 대상 PID·시작 시각이 현재 게임과 일치하지 않습니다."; return false; }
    if(!OwnsTcpListener(pid, port))
    { error = s.error = "선택한 게임이 Tracy 포트를 소유하지 않습니다. 계측 빌드·포트·다른 수신기 연결을 확인하세요."; return false; }
    if(s.view && s.pid == pid && s.creationTime == creationTime && s.port == port && !s.stopped && !s.closeApproved)
    { error.clear(); return true; }
    if(HasCapture() && !s.closeApproved && !RequestClose())
    {
        s.afterClose = [this, pid, creationTime, port] { std::string result; Connect(pid, creationTime, port, result); };
        error = s.error = "기존 Tracy 기록을 저장하거나 폐기한 뒤 대상을 변경할 수 있습니다."; return false;
    }
    try
    {
        s.view.reset(); s.Drain();
        s.pid = pid; s.creationTime = creationTime; s.port = port;
        s.stopped = s.closePending = s.closeApproved = s.closeCancelled = false;
        s.view = std::make_unique<tracy::View>(Impl::Run, "127.0.0.1", port, nullptr, Impl::Scale, Impl::Attention, nullptr);
        s.error.clear(); error.clear(); return true;
    }
    catch(const std::exception& ex) { error = s.error = ex.what(); return false; }
}
void EmbeddedTracy::Tick()
{
    auto& s = *impl; s.CheckThread(); s.Drain(); s.graphics.Tick();
    if(!s.view) return;
    s.view->EmbeddedProcessSave();
    auto& worker = s.view->GetEmbeddedWorker();
    // The original Draw path processes postponed work too. Keep it progressing while hidden.
    {
        std::unique_lock<std::mutex> lock(worker.GetDataLock(), std::try_to_lock);
        if(lock.owns_lock())
        {
            worker.DoPostponedWork();
            if(!s.view->EmbeddedStatic() && worker.HasData() && !s.stopped &&
               (worker.GetPid() != s.pid || !SameRunningProcess(s.pid, s.creationTime)))
            {
                s.view->EmbeddedStop(); s.stopped = true;
                s.error = "Tracy 대상 게임이 종료됐거나 연결 신원이 바뀌었습니다. 받은 기록은 유지됩니다.";
            }
        }
    }
    if(s.view->EmbeddedSaveFailed()) s.error = "Tracy 기록 저장에 실패했습니다. 받은 기록은 유지됩니다.";
    const auto status = worker.GetHandshakeStatus();
    if(status == tracy::HandshakeProtocolMismatch) s.error = "Tracy 버전이 일치하지 않습니다. 게임을 v0.13.1 계측으로 다시 빌드하세요.";
    else if(status == tracy::HandshakeNotAvailable) s.error = "Tracy 연결을 사용할 수 없습니다. 다른 수신기가 연결 중인지 확인하세요.";
    else if(status == tracy::HandshakeDropped) s.error = "Tracy 연결이 거절되거나 종료됐습니다. 기존 기록은 유지됩니다.";
}
void EmbeddedTracy::Draw(bool& visible)
{
    auto& s = *impl; s.CheckThread();
    if(!ImGui::GetCurrentContext() || ImGui::GetFrameCount() == 0) return;
    if(s.mouseFrame != ImGui::GetFrameCount())
    {
        s.graphics.MouseFrame(visible); s.mouseFrame = ImGui::GetFrameCount();
    }
    if(visible)
    {
        const auto style = ImGui::GetStyle();
        auto* defaultFont = ImGui::GetIO().FontDefault;
        ImGui::SetNextWindowSize(ImVec2(1000, 650), ImGuiCond_FirstUseEver);
        const bool content = ImGui::Begin("Tracy###Profiler", &visible);
        if(content)
        {
            if(ImGui::Button("기록 열기")) tracy::Fileselector::OpenFile("tracy", "Tracy Profiler trace file", [this](const char* fn)
                { std::string error; OpenCapture(fn, error); });
            if(s.view)
            {
                ImGui::SameLine(); ImGui::TextUnformatted(Connected() ? "수집 중" : HasCapture() ? "기록 보존 중" : "연결 대기 중");
            }
            if(!s.error.empty()) ImGui::TextWrapped("%s", s.error.c_str());
        }
        ImGui::End();
        if(visible && content && s.view)
        {
            s.graphics.BeginDraw();
            ImGui::PushFont(g_fonts.normal, FontNormal);
            ImGuiErrorRecoveryState stacks;
            ImGui::ErrorRecoveryStoreState(&stacks);
            try
            {
                tracy::InitTexture();
                if(!s.view->Draw())
                {
                    visible = false;
                    if(s.view->EmbeddedDiscardRequested())
                    {
                        const bool reconnect = s.view->ReconnectRequested();
                        Stop(); s.view.reset(); s.Drain();
                        if(reconnect) { std::string error; Connect(s.pid, s.creationTime, s.port, error); visible = true; }
                    }
                    else if(s.view->ReconnectRequested())
                    {
                        s.afterClose = [this] { std::string error; impl->closeApproved = true; Connect(impl->pid, impl->creationTime, impl->port, error); };
                        if(RequestClose()) s.closeApproved = true;
                    }
                }
            }
            catch(const std::exception& ex) { ImGui::ErrorRecoveryTryToRecoverState(&stacks); s.error = ex.what(); }
            ImGui::PopFont();
            s.graphics.EndDraw();
        }
        ImGui::GetStyle() = style;
        ImGui::GetIO().FontDefault = defaultFont;
    }
    if(s.closePending)
    {
        ImGui::OpenPopup("Tracy 기록 보존");
        if(ImGui::BeginPopupModal("Tracy 기록 보존", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        {
            ImGui::TextUnformatted("받은 Tracy 기록을 저장한 뒤 닫으시겠습니까?");
            if(!s.error.empty()) ImGui::TextWrapped("%s", s.error.c_str());
            if(ImGui::Button("저장"))
            {
                Stop();
                tracy::Fileselector::SaveFile("tracy", "Tracy Profiler trace file", [this](const char* fn)
                {
                    std::string error;
                    if(SaveCapture(fn, error)) { impl->closeApproved = true; impl->closePending = false; }
                });
                if(s.closeApproved) ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if(ImGui::Button("폐기")) { Stop(); s.closeApproved = true; s.closePending = false; ImGui::CloseCurrentPopup(); }
            ImGui::SameLine();
            if(ImGui::Button("취소")) { s.closePending = false; s.closeCancelled = true; s.afterClose = {}; ImGui::CloseCurrentPopup(); }
            ImGui::EndPopup();
        }
    }
    if(s.closeApproved && s.afterClose) { auto action = std::exchange(s.afterClose, {}); action(); }
}
void EmbeddedTracy::Stop()
{
    auto& s = *impl; s.CheckThread(); s.stopped = true;
    if(s.view)
    {
        auto& worker = s.view->GetEmbeddedWorker();
        std::lock_guard<std::mutex> lock(worker.GetDataLock());
        s.view->EmbeddedStop();
    }
}
bool EmbeddedTracy::Connected() const
{
    const auto& s = *impl;
    if(!s.view || s.stopped || s.view->EmbeddedStatic()) return false;
    auto& worker = s.view->GetEmbeddedWorker();
    if(!worker.HasData() || !worker.IsConnected() || worker.GetHandshakeStatus() != tracy::HandshakeWelcome) return false;
    std::unique_lock<std::mutex> lock(worker.GetDataLock(), std::try_to_lock);
    return lock.owns_lock() && !worker.WasDisconnectIssued() && worker.GetPid() == s.pid && SameRunningProcess(s.pid, s.creationTime);
}
bool EmbeddedTracy::HasCapture() const { return impl->view && impl->view->GetEmbeddedWorker().HasData(); }
bool EmbeddedTracy::RequestClose()
{
    auto& s = *impl; s.CheckThread();
    if(s.closeApproved || !s.Dirty()) { s.closePending = false; return true; }
    s.closeCancelled = false; s.closePending = true; return false;
}
bool EmbeddedTracy::ClosePending() const { return impl->closePending; }
bool EmbeddedTracy::CloseCancelled() const { return impl->closeCancelled; }
bool EmbeddedTracy::ReadWorker(const std::function<void(tracy::Worker&)>& read)
{
    auto& s = *impl; s.CheckThread();
    if(!read || !s.view || s.stopped || s.view->EmbeddedStatic()) return false;
    auto& worker = s.view->GetEmbeddedWorker();
    std::unique_lock<std::mutex> lock(worker.GetDataLock(), std::try_to_lock);
    if(!lock.owns_lock() || !worker.HasData() || !worker.IsConnected() || worker.WasDisconnectIssued() || worker.GetHandshakeStatus() != tracy::HandshakeWelcome ||
        worker.GetPid() != s.pid || !SameRunningProcess(s.pid, s.creationTime)) return false;
    read(worker); return true;
}
bool EmbeddedTracy::SaveCapture(const std::string& path, std::string& error)
{
    auto& s = *impl; s.CheckThread();
    if(!HasCapture()) { error = s.error = "저장할 Tracy 기록이 없습니다."; return false; }
    namespace fs = std::filesystem;
    fs::path temporary;
    bool temporaryOwned = false;
    try
    {
        const auto destination = fs::absolute(fs::path(ToWide(path)));
        temporary = fs::path(destination.wstring() + L".dy-tracy-" + std::to_wstring(GetCurrentProcessId()) +
            L"-" + std::to_wstring(GetTickCount64()) + L".partial");
        auto& worker = s.view->GetEmbeddedWorker();
        // A stopped Worker finishes its in-flight receive before a close/save snapshot.
        if(s.stopped)
        {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            while(worker.IsConnected() && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(5));
            if(worker.IsConnected()) throw std::runtime_error("Tracy 수집 종료 처리 중입니다. 잠시 후 다시 저장하세요.");
        }
        std::lock_guard<std::mutex> lock(worker.GetDataLock());
        const auto temporaryHandle = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if(temporaryHandle == INVALID_HANDLE_VALUE) throw std::runtime_error("Tracy 임시 저장 파일을 만들 수 없습니다. 받은 기록은 유지됩니다.");
        CloseHandle(temporaryHandle); temporaryOwned = true;
        std::unique_ptr<tracy::FileWrite> file(tracy::FileWrite::Open(ToUtf8(temporary.wstring()).c_str(), tracy::FileCompression::Fast, 1, 2));
        if(!file) throw std::runtime_error("Tracy 저장 파일을 열 수 없습니다. 경로와 쓰기 권한을 확인하세요.");
        worker.DoPostponedWorkAll();
        worker.Write(*file, false); file->Finish();
        if(file->HasWriteError()) throw std::runtime_error("Tracy 기록을 끝까지 쓰지 못했습니다. 받은 기록은 유지됩니다.");
        file.reset();
        if(!MoveFileExW(temporary.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            throw std::runtime_error("Tracy 저장 파일을 확정할 수 없습니다. 받은 기록은 유지됩니다.");
        s.view->EmbeddedStoreUserData(true);
        s.view->EmbeddedMarkSaved(worker.GetLastTime());
        error.clear(); s.error.clear(); return true;
    }
    catch(const std::exception& ex)
    {
        if(temporaryOwned) { std::error_code ignored; fs::remove(temporary, ignored); }
        s.view->EmbeddedMarkSaveFailed();
        error = s.error = ex.what(); return false;
    }
}
bool EmbeddedTracy::OpenCapture(const std::string& path, std::string& error)
{
    auto& s = *impl; s.CheckThread();
    try
    {
        std::unique_ptr<tracy::FileRead> file(tracy::FileRead::Open(path.c_str()));
        if(!file) { error = s.error = "Tracy 기록 파일을 열 수 없습니다. 현재 기록은 유지됩니다."; return false; }
        if(!s.closeApproved && !RequestClose())
        {
            s.afterClose = [this, path] { std::string result; OpenCapture(path, result); };
            error = s.error = "기존 기록을 저장하거나 폐기한 뒤 선택한 파일을 엽니다."; return false;
        }
        if(s.view) s.view->EmbeddedStoreUserData();
        auto replacement = std::make_unique<tracy::View>(Impl::Run, *file, nullptr, Impl::Scale, Impl::Attention, nullptr);
        s.view = std::move(replacement); s.Drain();
        s.stopped = true; s.pid = 0; s.creationTime = 0; s.port = 0;
        s.closeApproved = s.closePending = s.closeCancelled = false;
        error.clear(); s.error.clear(); return true;
    }
    catch(...) { error = s.error = "Tracy 기록 형식이나 버전을 읽을 수 없습니다. 현재 기록은 유지됩니다."; return false; }
}
const std::string& EmbeddedTracy::LastError() const { return impl->error; }

#else
struct EmbeddedTracy::Impl { std::string error = "이 분석기는 내장 Tracy를 제외하고 빌드됐습니다 (DY_VIEWER_EMBED_TRACY=OFF)."; };
bool EmbeddedTracy::PrepareFonts(std::string& error) { error.clear(); return true; }
EmbeddedTracy::EmbeddedTracy(dyf::Gui&, dyf::RHI::IDevice&) : impl(std::make_unique<Impl>()) {}
EmbeddedTracy::~EmbeddedTracy() = default;
bool EmbeddedTracy::Connect(uint32_t, uint64_t, uint16_t, std::string& error) { error = impl->error; return false; }
void EmbeddedTracy::Tick() {}
void EmbeddedTracy::Draw(bool& visible)
{
    if(visible) { ImGui::Begin("Tracy###Profiler", &visible); ImGui::TextWrapped("%s", impl->error.c_str()); ImGui::End(); }
}
void EmbeddedTracy::Stop() {}
bool EmbeddedTracy::Connected() const { return false; }
bool EmbeddedTracy::HasCapture() const { return false; }
bool EmbeddedTracy::RequestClose() { return true; }
bool EmbeddedTracy::ClosePending() const { return false; }
bool EmbeddedTracy::CloseCancelled() const { return false; }
bool EmbeddedTracy::ReadWorker(const std::function<void(tracy::Worker&)>&) { return false; }
bool EmbeddedTracy::OpenCapture(const std::string&, std::string& error) { error = impl->error; return false; }
bool EmbeddedTracy::SaveCapture(const std::string&, std::string& error) { error = impl->error; return false; }
const std::string& EmbeddedTracy::LastError() const { return impl->error; }
#endif
}
