#pragma once
#include "Process.h"
#include "ToolBootstrap.h"
#include <future>
#include <memory>
#include <mutex>

namespace viewer
{
struct SessionProfile
{
    bool enabled=true;
    std::string executable,arguments,directory;
    bool tracyVisible=false;
};
bool LoadSessionProfile(const std::filesystem::path&,SessionProfile&,std::string& error);
bool SaveSessionProfile(const std::filesystem::path&,const SessionProfile&,std::string& error);
int RunAutoSessionHelper(const std::filesystem::path& requestPath);
std::filesystem::path CurrentExecutable();

enum class AutoStage { Idle,NeedsTarget,Preparing,Starting,WaitingLive,WaitingTracy,Ready,Failed,Cancelled,Exited };
struct AutoEvidence
{
    bool connected=false;
    std::optional<int64_t> tracy;
    std::optional<bool> captureReady;
    std::string captureIssue;
    std::optional<bool> renderDocConnected;
    std::string renderDocError;
    bool tracyRequested=false,embeddedConnected=false;
    std::string embeddedIssue;
};
class AutoSession
{
public:
    using Prepare=std::function<ToolPaths(const std::filesystem::path&,const ToolBootstrapProgress&,const std::atomic_bool&)>;
    AutoSession(const std::filesystem::path& profilePath,const std::filesystem::path& analyzerPath,Prepare=EnsureProfilerTools);
    ~AutoSession();
    bool Load(std::string& error);
    bool SetEnabled(bool value);
    bool SetTracyVisible(bool value);
    bool Start(const SessionProfile&);
    void Cancel();
    void Tick(const AutoEvidence& evidence={});
    AutoStage Stage() const {return stage;}
    std::string Status() const;
    bool Busy() const {return stage==AutoStage::Preparing||stage==AutoStage::Starting;}
    const SessionProfile& Profile() const {return profile;}
    const ToolPaths& Paths() const {return paths;}
    const TargetProcess& Target() const {return target;}
    uint16_t TracyPort() const {return runtime.port;}
    bool LegacyTracyRunning() const {return SameRunningProcess(runtime.tracyPid,runtime.tracyTime);}
    bool WantsRenderDocConnection() const {return stage==AutoStage::WaitingTracy&&requestReplay;}
private:
    struct Runtime
    {
        uint64_t nonce=0;
        uint16_t port=0;
        uint32_t launcherPid=0,targetPid=0,tracyPid=0;
        uint64_t launcherTime=0,targetTime=0,tracyTime=0;
    } runtime;
    struct Progress {std::mutex mutex;std::string text;};
    std::filesystem::path profilePath,analyzerPath;
    SessionProfile profile;
    ToolPaths paths;
    Prepare prepare;
    std::shared_ptr<Progress> progress=std::make_shared<Progress>();
    std::shared_ptr<std::atomic_bool> cancel=std::make_shared<std::atomic_bool>(false);
    std::future<ToolPaths> preparing;
    TargetProcess launcher,helper,target;
    void* lock=nullptr;
    AutoStage stage=AutoStage::Idle;
    uint64_t deadline=0,nextOwnershipPoll=0;
    std::string status;
    bool requestReplay=false;
    void Set(AutoStage,std::string);
    bool SetPreference(bool value,bool tracyView);
    bool ReadRuntime(std::string& error);
    bool SaveRuntime(std::string& error);
    void BeginLaunch();
    void PollLaunch();
};
}
