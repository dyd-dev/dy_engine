#include "AutoSession.h"
#include <windows.h>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace viewer
{
namespace fs=std::filesystem;
namespace
{
fs::path Sidecar(const fs::path& path,const wchar_t* suffix) {auto result=path;result+=suffix;return result;}
bool ReadSmall(const fs::path& path,std::string& data,std::string& error)
{
    error.clear();std::error_code code;
    const auto size=fs::file_size(path,code);
    if(code) {if(!fs::exists(path,code))return false;error="Cannot read session file.";return false;}
    if(size>131072) {error="Session file exceeds 128 KiB.";return false;}
    std::ifstream input(path,std::ios::binary);data.assign(std::istreambuf_iterator<char>(input),{});
    if(!input||data.size()!=size) {error="Session file could not be read completely.";return false;}return true;
}
bool AtomicWrite(const fs::path& path,const std::string& data,std::string& error)
{
    error.clear();auto temporary=Sidecar(path,(L".tmp-"+std::to_wstring(GetCurrentProcessId())).c_str());
    try
    {
        std::ofstream output(temporary,std::ios::binary|std::ios::trunc);output.write(data.data(),std::streamsize(data.size()));output.close();
        if(!output)throw std::runtime_error("Cannot write the local session file.");
        if(!MoveFileExW(temporary.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))
            throw std::runtime_error("Cannot atomically replace the local session file (Win32 "+std::to_string(GetLastError())+").");
        return true;
    }
    catch(const std::exception& exception) {std::error_code code;fs::remove(temporary,code);error=exception.what();return false;}
}
void Validate(const SessionProfile& profile)
{
    if(profile.executable.size()>32760||profile.arguments.size()>32760||profile.directory.size()>32760)
        throw std::invalid_argument("Session target, arguments or working directory is too long.");
    const auto executable=ToWide(profile.executable),directory=ToWide(profile.directory);(void)ToWide(profile.arguments);
    if(!executable.empty()&&!fs::path(executable).is_absolute())throw std::invalid_argument("Remembered target must have an absolute path.");
    if(!directory.empty()&&!fs::path(directory).is_absolute())throw std::invalid_argument("Remembered working directory must have an absolute path.");
}
void ProfileOut(std::ostream& output,const SessionProfile& profile)
{output<<int(profile.enabled)<<'\n'<<std::quoted(profile.executable)<<'\n'<<std::quoted(profile.arguments)<<'\n'<<std::quoted(profile.directory)<<'\n';}
bool ProfileIn(std::istream& input,SessionProfile& profile)
{
    int enabled=-1;
    if(!(input>>enabled>>std::quoted(profile.executable)>>std::quoted(profile.arguments)>>std::quoted(profile.directory))||enabled<0||enabled>1)return false;
    profile.enabled=enabled!=0;Validate(profile);return true;
}
bool End(std::istream& input) {input>>std::ws;return input.eof();}
bool SamePath(const std::string& a,const std::string& b)
{
    if(a.empty()||b.empty())return a==b;
    const auto left=fs::weakly_canonical(fs::path(ToWide(a))),right=fs::weakly_canonical(fs::path(ToWide(b)));
    return CompareStringOrdinal(left.c_str(),-1,right.c_str(),-1,TRUE)==CSTR_EQUAL;
}
struct Receipt {uint64_t nonce=0,creation=0;uint32_t pid=0;std::string error;};
bool ReadReceipt(const fs::path& path,Receipt& receipt,std::string& error)
{
    std::string data;if(!ReadSmall(path,data,error))return false;std::istringstream input(data);
    std::string magic;int version=0;
    if(!(input>>magic>>version>>receipt.nonce>>receipt.pid>>receipt.creation>>std::quoted(receipt.error))
        ||magic!="DY_AUTO_RECEIPT"||version!=1||!End(input)) {error="Invalid automatic launch receipt.";return false;}return true;
}
}

fs::path CurrentExecutable()
{
    wchar_t path[32768]{};const auto size=GetModuleFileNameW(nullptr,path,32768);
    if(!size||size>=32768)throw std::runtime_error("Cannot find the analyzer executable.");return fs::path(path);
}
bool LoadSessionProfile(const fs::path& path,SessionProfile& profile,std::string& error)
{
    std::string data;if(!ReadSmall(path,data,error))return false;
    try
    {
        std::istringstream input(data);std::string magic;int version=0;SessionProfile parsed;
        int visible=0;
        if(!(input>>magic>>version)||magic!="DY_AUTO_SESSION"||(version!=1&&version!=2)||!ProfileIn(input,parsed)
            ||(version==2&&(!(input>>visible)||visible<0||visible>1))||!End(input))
            throw std::runtime_error("Invalid automatic session profile.");
        parsed.tracyVisible=visible!=0;
        profile=std::move(parsed);return true;
    }
    catch(const std::exception& exception) {error=exception.what();return false;}
}
bool SaveSessionProfile(const fs::path& path,const SessionProfile& profile,std::string& error)
{
    try {Validate(profile);std::ostringstream output;output<<"DY_AUTO_SESSION 2\n";ProfileOut(output,profile);output<<int(profile.tracyVisible)<<'\n';return AtomicWrite(path,output.str(),error);}
    catch(const std::exception& exception) {error=exception.what();return false;}
}

int RunAutoSessionHelper(const fs::path& requestPath)
{
    Receipt receipt;std::string error;bool launched=false;
    try
    {
        std::string data;if(!ReadSmall(requestPath,data,error))throw std::runtime_error(error.empty() ? "Automatic launch request not found." : error);
        std::istringstream input(data);std::string magic;int version=0,port=0;SessionProfile profile;
        if(!(input>>magic>>version>>receipt.nonce>>port)||magic!="DY_AUTO_REQUEST"||version!=1||!receipt.nonce||port<1||port>65535
            ||!ProfileIn(input,profile)||!End(input))throw std::runtime_error("Invalid automatic launch request.");
        if(profile.executable.empty()||!fs::is_regular_file(fs::path(ToWide(profile.executable))))throw std::runtime_error("Automatic target executable is missing.");
        if(fs::equivalent(fs::path(ToWide(profile.executable)),CurrentExecutable()))throw std::runtime_error("The analyzer cannot profile itself recursively.");
        TargetProcess child;
        if(!child.Launch(profile.executable,profile.arguments,profile.directory,error,
            {{"TRACY_PORT",std::to_string(port)},{"TRACY_ONLY_IPV4","1"},{"TRACY_ONLY_LOCALHOST","1"}}))throw std::runtime_error(error);
        launched=true;receipt.pid=child.Pid();receipt.creation=child.CreationTime();
        std::ostringstream output;output<<"DY_AUTO_RECEIPT 1\n"<<receipt.nonce<<' '<<receipt.pid<<' '<<receipt.creation<<' '<<std::quoted("")<<'\n';
        // Keep the real game alive if receipt persistence fails; the analyzer reports partial startup.
        const bool saved=AtomicWrite(Sidecar(requestPath,L".receipt"),output.str(),error);
        while(child.Running())Sleep(100);
        return saved ? int(child.ExitCode().value_or(1)) : 2;
    }
    catch(const std::exception& exception)
    {
        if(!launched)
        {
            std::ostringstream output;output<<"DY_AUTO_RECEIPT 1\n"<<receipt.nonce<<" 0 0 "<<std::quoted(exception.what())<<'\n';
            AtomicWrite(Sidecar(requestPath,L".receipt"),output.str(),error);
        }
        return 1;
    }
}

AutoSession::AutoSession(const fs::path& profilePath,const fs::path& analyzerPath,Prepare prepare)
    :profilePath(fs::absolute(profilePath)),analyzerPath(fs::absolute(analyzerPath)),prepare(std::move(prepare)) {}
AutoSession::~AutoSession() {Cancel();if(preparing.valid())preparing.wait();if(lock)CloseHandle(lock);}
void AutoSession::Set(AutoStage value,std::string text) {stage=value;status=std::move(text);}
std::string AutoSession::Status() const
{
    if(stage==AutoStage::Preparing) {std::lock_guard<std::mutex> guard(progress->mutex);return status+(progress->text.empty() ? "" : " · "+progress->text);}
    return status;
}
bool AutoSession::Load(std::string& error)
{
    if(!LoadSessionProfile(profilePath,profile,error))
    {Set(error.empty() ? AutoStage::NeedsTarget : AutoStage::Failed,error.empty() ? "자동 연결할 게임을 한 번 선택하세요." : error);return error.empty();}
    Set(AutoStage::Idle,profile.enabled ? "저장한 자동 연결 설정을 불러왔습니다." : "자동 연결 꺼짐");return true;
}
bool AutoSession::SetEnabled(bool value)
{return SetPreference(value,false);}
bool AutoSession::SetTracyVisible(bool value)
{return SetPreference(value,true);}
bool AutoSession::SetPreference(bool value,bool tracyView)
{
    if(!lock)
    {
        const auto opened=CreateFileW(Sidecar(profilePath,L".lock").c_str(),GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ,nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(opened==INVALID_HANDLE_VALUE) {Set(AutoStage::Failed,"다른 분석기가 이 자동 세션 설정을 사용 중입니다.");return false;}lock=opened;
    }
    SessionProfile next;std::string error;
    if(!LoadSessionProfile(profilePath,next,error))
    {if(!error.empty()) {Set(AutoStage::Failed,error);return false;}next=profile;}
    if(tracyView)next.tracyVisible=value;else next.enabled=value;
    if(!SaveSessionProfile(profilePath,next,error)) {Set(AutoStage::Failed,error);return false;}
    profile=std::move(next);if(!tracyView&&!value)Cancel();return true;
}
bool AutoSession::ReadRuntime(std::string& error)
{
    std::string data;if(!ReadSmall(Sidecar(profilePath,L".runtime"),data,error))return error.empty();
    std::istringstream input(data);std::string magic;int version=0,port=0;Runtime parsed;
    if(!(input>>magic>>version>>parsed.nonce>>port>>parsed.launcherPid>>parsed.launcherTime>>parsed.targetPid>>parsed.targetTime
        >>parsed.tracyPid>>parsed.tracyTime)||magic!="DY_AUTO_RUNTIME"||version!=1||port<0||port>65535||!End(input))
    {error="Invalid automatic session ownership record; existing processes were left untouched.";return false;}
    parsed.port=uint16_t(port);runtime=parsed;return true;
}
bool AutoSession::SaveRuntime(std::string& error)
{
    runtime.launcherPid=launcher.Pid();runtime.launcherTime=launcher.CreationTime();runtime.targetPid=target.Pid();runtime.targetTime=target.CreationTime();
    std::ostringstream output;output<<"DY_AUTO_RUNTIME 1\n"<<runtime.nonce<<' '<<runtime.port<<' '
        <<runtime.launcherPid<<' '<<runtime.launcherTime<<' '<<runtime.targetPid<<' '<<runtime.targetTime<<' '
        <<runtime.tracyPid<<' '<<runtime.tracyTime<<'\n';
    return AtomicWrite(Sidecar(profilePath,L".runtime"),output.str(),error);
}
bool AutoSession::Start(const SessionProfile& requested)
{
    if(preparing.valid()||stage==AutoStage::Starting)return false;
    std::string error;
    if(!lock)
    {
        const auto opened=CreateFileW(Sidecar(profilePath,L".lock").c_str(),GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ,nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(opened==INVALID_HANDLE_VALUE) {Set(AutoStage::Failed,"다른 분석기가 이 자동 세션을 사용 중이거나 설정 폴더에 쓸 수 없습니다.");return false;}lock=opened;
    }
    try
    {
        SessionProfile previous;const bool saved=LoadSessionProfile(profilePath,previous,error);
        if(!saved&&!error.empty())throw std::runtime_error(error);
        if(!ReadRuntime(error))throw std::runtime_error(error);
        if(saved&&!target.Running()&&runtime.targetPid)target.Observe(runtime.targetPid,runtime.targetTime,previous.executable,error);
        const bool existing=target.Running()||launcher.Running()||SameRunningProcess(runtime.launcherPid,runtime.launcherTime);
        if(existing&&saved&&(!SamePath(requested.executable,previous.executable)||requested.arguments!=previous.arguments||!SamePath(requested.directory,previous.directory)))
            throw std::runtime_error("자동 실행한 게임 또는 실행기가 남아 있습니다. 종료한 뒤 게임·인자·작업 폴더를 바꾸세요.");
        if(requested.enabled&&!requested.executable.empty())
        {
            if(!fs::is_regular_file(fs::path(ToWide(requested.executable))))throw std::runtime_error("저장한 대상 실행 파일이 없습니다. 실행 설정에서 다시 선택하세요.");
            if(!requested.directory.empty()&&!fs::is_directory(fs::path(ToWide(requested.directory))))throw std::runtime_error("저장한 작업 폴더가 없습니다.");
        }
        if(!SaveSessionProfile(profilePath,requested,error))throw std::runtime_error(error);
        profile=requested;
        if(!profile.enabled) {Set(AutoStage::Idle,"자동 연결 꺼짐");return true;}
        if(profile.executable.empty()) {Set(AutoStage::NeedsTarget,"자동 연결할 게임을 한 번 선택하세요.");return false;}
        if(!launcher.Running()&&runtime.launcherPid&&!paths.renderdocCommand.empty())launcher.Observe(runtime.launcherPid,runtime.launcherTime,ToUtf8(paths.renderdocCommand.native()),error);
        cancel->store(false);requestReplay=false;deadline=GetTickCount64()+30000;
        if(!paths.renderdocCommand.empty()&&!paths.renderdocUi.empty())
        {
            if(target.Running())Set(AutoStage::WaitingLive,"기존 게임의 정확한 PID에 계측 연결 중입니다.");
            else if(launcher.Running())Set(AutoStage::Starting,"진행 중인 게임 실행 결과를 확인합니다.");
            else BeginLaunch();
            return stage!=AutoStage::Failed;
        }
        {std::lock_guard<std::mutex> guard(progress->mutex);progress->text.clear();}
        Set(AutoStage::Preparing,"공식 휴대용 프로파일러 준비 중");
        const auto preparation=prepare;const auto root=analyzerPath.parent_path()/L"profiler-tools";const auto cancellation=cancel;const auto shared=progress;
        preparing=std::async(std::launch::async,[preparation,root,cancellation,shared] {
            return preparation(root,[shared](std::string_view text) {std::lock_guard<std::mutex> guard(shared->mutex);shared->text=std::string(text);},*cancellation);
        });return true;
    }
    catch(const std::exception& exception) {Set(AutoStage::Failed,exception.what());return false;}
}
void AutoSession::Cancel()
{
    cancel->store(true);
    if(stage!=AutoStage::Idle&&stage!=AutoStage::NeedsTarget)
        Set(AutoStage::Cancelled,target.Running()||launcher.Running() ? "자동 연결 중단 · 시작한 게임과 외부 도구는 계속 실행됩니다." : "자동 연결 취소됨");
}
void AutoSession::BeginLaunch()
{
    try
    {
        if(cancel->load())return;
        // Previous profiler windows may contain unsaved work; release only our observation handles.
        launcher.Forget();helper.Forget();target.Forget();
        const auto previous=runtime;runtime={};runtime.tracyPid=previous.tracyPid;runtime.tracyTime=previous.tracyTime;
        runtime.nonce=(GetTickCount64()<<20)^GetCurrentProcessId();runtime.port=FreeTcpPort();
        const auto request=Sidecar(profilePath,L".request");std::ostringstream output;
        output<<"DY_AUTO_REQUEST 1\n"<<runtime.nonce<<' '<<runtime.port<<'\n';ProfileOut(output,profile);std::string error;
        if(!AtomicWrite(request,output.str(),error))throw std::runtime_error(error);
        if(cancel->load())return;
        // RenderDoc's CLI re-quotes arguments; pass only this immutable request, and let our helper preserve game arguments.
        const auto arguments=L"capture --wait-for-exit --opt-hook-children --working-dir "+QuoteArgument(analyzerPath.parent_path().native())
            +L" "+QuoteArgument(analyzerPath.native())+L" --session-helper "+QuoteArgument(request.native());
        if(!launcher.Launch(ToUtf8(paths.renderdocCommand.native()),ToUtf8(arguments),ToUtf8(paths.renderdocCommand.parent_path().native()),error,{},true))throw std::runtime_error(error);
        deadline=GetTickCount64()+30000;nextOwnershipPoll=0;
        Set(AutoStage::Starting,SaveRuntime(error) ? "RenderDoc을 통해 게임을 시작하고 소유한 PID를 확인 중입니다." : "게임 실행 중 · 소유 기록 저장 실패: "+error);
    }
    catch(const std::exception& exception) {Set(AutoStage::Failed,exception.what());}
}
void AutoSession::PollLaunch()
{
    const auto now=GetTickCount64();if(now<nextOwnershipPoll)return;nextOwnershipPoll=now+100;
    std::string error;
    if(!helper.Running())helper.ObserveChild(launcher,ToUtf8(analyzerPath.native()),error);
    Receipt receipt;
    if(ReadReceipt(Sidecar(profilePath,L".request.receipt"),receipt,error)&&receipt.nonce==runtime.nonce)
    {
        if(!receipt.error.empty()) {Set(AutoStage::Failed,"게임 실행 실패: "+receipt.error);return;}
        if(helper.Running())
        {
            if(!target.ObserveChild(helper,profile.executable,error,receipt.pid,receipt.creation))
            {Set(AutoStage::Failed,"실행 결과의 PID·시작 시각·경로·부모 관계 검증 실패: "+error);return;}
            if(!SaveRuntime(error)) {Set(AutoStage::Failed,"게임 실행됨 · 소유 기록 저장 실패: "+error);return;}
            deadline=now+30000;Set(AutoStage::WaitingLive,"정확한 게임 PID "+std::to_string(target.Pid())+"의 계측 세션을 연결 중입니다.");return;
        }
    }
    if(!launcher.Running()) {Set(AutoStage::Failed,"RenderDoc 실행기가 게임 연결 전에 종료되었습니다. 종료 코드 "+std::to_string(launcher.ExitCode().value_or(1))+". 기존 프로세스는 보존됩니다.");return;}
    if(now>=deadline)Set(AutoStage::Failed,"실행기 또는 게임이 남아 있을 수 있으나 검증된 실행 결과가 없습니다. 실행 설정을 확인하고 재시도하세요.");
}
void AutoSession::Tick(const AutoEvidence& evidence)
{
    try
    {
        if(preparing.valid()&&preparing.wait_for(std::chrono::seconds(0))==std::future_status::ready)
        {
            try {paths=preparing.get();}catch(...) {if(cancel->load())return;throw;}
            if(cancel->load())return;
            for(const auto* path:{&paths.renderdocCommand,&paths.renderdocUi})if(!fs::is_regular_file(*path))throw std::runtime_error("A prepared RenderDoc executable is missing.");
            std::string error;
            if(!launcher.Running()&&runtime.launcherPid)launcher.Observe(runtime.launcherPid,runtime.launcherTime,ToUtf8(paths.renderdocCommand.native()),error);
            if(target.Running()) {deadline=GetTickCount64()+30000;Set(AutoStage::WaitingLive,"저장된 게임 PID "+std::to_string(target.Pid())+"에 다시 연결 중입니다.");}
            else if(launcher.Running()) {deadline=GetTickCount64()+30000;Set(AutoStage::Starting,"이전에 시작한 게임의 실행 결과를 확인 중입니다.");}
            else BeginLaunch();
        }
        if(stage==AutoStage::Starting) {PollLaunch();return;}
        if(stage!=AutoStage::WaitingLive&&stage!=AutoStage::WaitingTracy&&stage!=AutoStage::Ready)return;
        if(!target.Running()) {Set(AutoStage::Exited,"자동 실행한 게임이 종료되었습니다. 재시도로 다시 시작할 수 있습니다.");return;}
        if(!evidence.connected)
        {
            if(stage==AutoStage::Ready)deadline=GetTickCount64()+30000;
            Set(AutoStage::WaitingLive,"게임 PID "+std::to_string(target.Pid())+"의 실제 계측 스냅샷 대기 중");
            if(GetTickCount64()>=deadline)Set(AutoStage::Failed,"게임은 실행 중이나 계측 연결이 없습니다. 대상의 Live Profiling 빌드 설정을 확인하세요.");return;
        }
        if(stage==AutoStage::Ready&&((evidence.tracyRequested&&!evidence.embeddedConnected)||(evidence.renderDocConnected&&!*evidence.renderDocConnected)))
            deadline=GetTickCount64()+30000;
        if((evidence.tracyRequested&&!evidence.tracy)||!evidence.captureReady||!evidence.renderDocConnected)
        {
            Set(AutoStage::WaitingLive,"대상의 도구 진단 등록 대기 중");
            if(GetTickCount64()>=deadline)throw std::runtime_error("대상의 도구 진단 정보가 없습니다. Tracy·RenderDoc·Live Profiling을 켜고 대상을 다시 빌드하세요.");return;
        }
        if(evidence.tracyRequested&&*evidence.tracy==0)throw std::runtime_error("대상 빌드에서 Tracy가 꺼져 있습니다. DY_ENABLE_TRACY=ON으로 다시 빌드하세요.");
        if(evidence.tracyRequested&&!evidence.embeddedIssue.empty())throw std::runtime_error("내장 Tracy 연결: "+evidence.embeddedIssue);
        if(!*evidence.captureReady)throw std::runtime_error("RenderDoc 캡처 준비 불가: "+evidence.captureIssue);
        if(!evidence.renderDocError.empty())throw std::runtime_error("RenderDoc 연결 요청 실패: "+evidence.renderDocError);
        requestReplay=!*evidence.renderDocConnected;
        if((!evidence.tracyRequested||evidence.embeddedConnected)&&*evidence.renderDocConnected)
            Set(AutoStage::Ready,"자동 연결 완료 · 게임 PID "+std::to_string(target.Pid())
                +(evidence.tracyRequested ? " · 내장 Tracy 연결됨" : " · Tracy 보기 꺼짐")+" · RenderDoc 연결·캡처 준비됨");
        else
        {
            Set(AutoStage::WaitingTracy,"게임 PID "+std::to_string(target.Pid())+" · Tracy "+(!evidence.tracyRequested ? "보기 꺼짐" : evidence.embeddedConnected ? "내장 연결됨" : evidence.tracy&&*evidence.tracy==2 ? "다른 수신기 연결 확인 필요" : "내장 연결 대기")
                +" · RenderDoc "+(*evidence.renderDocConnected ? "연결됨" : "연결 대기"));
            if(GetTickCount64()>=deadline)throw std::runtime_error("도구 연결을 완료하지 못했습니다. 게임과 도구는 계속 실행됩니다. RenderDoc 첫 실행 Analytics 창이 있으면 사용 통계 선호를 직접 선택하고 OK를 누른 뒤 재시도하세요.");
        }
    }
    catch(const std::exception& exception) {Set(AutoStage::Failed,exception.what());}
}
}
