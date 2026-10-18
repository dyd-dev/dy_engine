#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace viewer
{
std::wstring ToWide(std::string_view utf8);
std::string ToUtf8(std::wstring_view wide);
std::wstring QuoteArgument(std::wstring_view argument);

class TargetProcess
{
public:
    ~TargetProcess();
    TargetProcess() = default;
    TargetProcess(const TargetProcess&) = delete;
    TargetProcess& operator=(const TargetProcess&) = delete;
    bool Launch(const std::string& executable, const std::string& arguments,
        const std::string& directory, std::string& error,
        const std::vector<std::pair<std::string,std::string>>& environment={},bool hidden=false);
    bool Observe(uint32_t processId,uint64_t creationTime,const std::string& executable,std::string& error);
    bool ObserveChild(const TargetProcess& parent,const std::string& executable,std::string& error,
        uint32_t expectedPid=0,uint64_t expectedCreation=0);
    bool Running() const;
    std::optional<uint32_t> ExitCode() const;
    uint32_t Pid() const { return pid; }
    uint64_t CreationTime() const;
    void Forget();
private:
    void* process = nullptr;
    uint32_t pid = 0;
};
uint16_t FreeTcpPort();
bool OwnsTcpListener(uint32_t pid,uint16_t port);
bool SameRunningProcess(uint32_t pid,uint64_t creationTime);
}
