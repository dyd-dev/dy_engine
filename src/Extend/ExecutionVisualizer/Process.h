#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

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
        const std::string& directory, std::string& error);
    bool Running() const;
    std::optional<uint32_t> ExitCode() const;
    uint32_t Pid() const { return pid; }
private:
    void* process = nullptr;
    uint32_t pid = 0;
};
}
