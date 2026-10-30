#include "Process.h"
#include <windows.h>
#include <filesystem>
#include <limits>
#include <climits>
#include <stdexcept>

namespace viewer
{
std::wstring ToWide(std::string_view utf8)
{
    if(utf8.find('\0') != utf8.npos || utf8.size() > size_t(INT_MAX))
        throw std::invalid_argument("Invalid UTF-8 text length or embedded NUL.");
    if(utf8.empty()) return {};
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), int(utf8.size()), nullptr, 0);
    if(!count) throw std::invalid_argument("Text is not valid UTF-8.");
    std::wstring result(size_t(count), L'\0');
    if(MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), int(utf8.size()), result.data(), count) != count)
        throw std::invalid_argument("UTF-8 conversion failed.");
    return result;
}

std::string ToUtf8(std::wstring_view wide)
{
    if(wide.find(L'\0') != wide.npos || wide.size() > size_t(INT_MAX))
        throw std::invalid_argument("Invalid Unicode text length or embedded NUL.");
    if(wide.empty()) return {};
    const int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), int(wide.size()), nullptr, 0, nullptr, nullptr);
    if(!count) throw std::invalid_argument("Text is not valid Unicode.");
    std::string result(size_t(count), '\0');
    if(WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), int(wide.size()), result.data(), count, nullptr, nullptr) != count)
        throw std::invalid_argument("Unicode conversion failed.");
    return result;
}

std::wstring QuoteArgument(std::wstring_view argument)
{
    if(argument.find(L'\0') != argument.npos) throw std::invalid_argument("Argument contains NUL.");
    std::wstring result = L"\"";
    size_t slashes = 0;
    for(const auto c : argument)
    {
        if(c == L'\\') { ++slashes; continue; }
        result.append(c == L'\"' ? slashes * 2 + 1 : slashes, L'\\');
        result += c;
        slashes = 0;
    }
    result.append(slashes * 2, L'\\');
    result += L'\"';
    return result;
}

static std::string WindowsError(DWORD code)
{
    wchar_t* message = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, 0, reinterpret_cast<wchar_t*>(&message), 0, nullptr);
    std::string result = "Win32 " + std::to_string(code);
    if(message)
    {
        const std::wstring copy(message);
        LocalFree(message);
        result += ": " + ToUtf8(copy);
    }
    return result;
}

TargetProcess::~TargetProcess() { if(process) CloseHandle(process); }
bool TargetProcess::Running() const { return process && WaitForSingleObject(process, 0) == WAIT_TIMEOUT; }
std::optional<uint32_t> TargetProcess::ExitCode() const
{
    if(!process || WaitForSingleObject(process, 0) != WAIT_OBJECT_0) return {};
    DWORD code = 0;
    if(!GetExitCodeProcess(process, &code)) return {};
    return code;
}

bool TargetProcess::Launch(const std::string& executable, const std::string& arguments,
    const std::string& directory, std::string& error)
{
    error.clear();
    if(Running()) { error = "The launched target is still running."; return false; }
    try
    {
        if(executable.empty()) throw std::invalid_argument("Select a target executable.");
        const auto path = std::filesystem::absolute(std::filesystem::path(ToWide(executable)));
        const auto folder = directory.empty() ? path.parent_path() : std::filesystem::absolute(std::filesystem::path(ToWide(directory)));
        auto command = QuoteArgument(path.native());
        if(!arguments.empty()) command += L" " + ToWide(arguments);
        if(command.size() >= 32767) throw std::invalid_argument("Windows command line exceeds 32766 characters.");
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION created{};
        if(!CreateProcessW(path.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_UNICODE_ENVIRONMENT,
            nullptr, folder.c_str(), &startup, &created))
        { error = WindowsError(GetLastError()); return false; }
        CloseHandle(created.hThread);
        if(process) CloseHandle(process);
        process = created.hProcess;
        pid = created.dwProcessId;
        return true;
    }
    catch(const std::exception& exception) { error = exception.what(); return false; }
}
}
