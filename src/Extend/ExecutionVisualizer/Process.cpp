#include "Process.h"
#include <winsock2.h>
#include <windows.h>
#include <iphlpapi.h>
#include <tlhelp32.h>
#include <filesystem>
#include <algorithm>
#include <map>
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
void TargetProcess::Forget() {if(process)CloseHandle(process);process=nullptr;pid=0;}
bool TargetProcess::Running() const { return process && WaitForSingleObject(process, 0) == WAIT_TIMEOUT; }
static uint64_t Created(HANDLE handle)
{
    FILETIME creation{},exit{},kernel{},user{};
    if(!GetProcessTimes(handle,&creation,&exit,&kernel,&user)) return 0;
    return (uint64_t(creation.dwHighDateTime)<<32)|creation.dwLowDateTime;
}
uint64_t TargetProcess::CreationTime() const { return process ? Created(process) : 0; }
bool SameRunningProcess(uint32_t pid,uint64_t creationTime)
{
    if(!pid||!creationTime)return false;
    const auto opened=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION|SYNCHRONIZE,FALSE,pid);if(!opened)return false;
    const bool same=Created(opened)==creationTime&&WaitForSingleObject(opened,0)==WAIT_TIMEOUT;CloseHandle(opened);return same;
}
std::optional<uint32_t> TargetProcess::ExitCode() const
{
    if(!process || WaitForSingleObject(process, 0) != WAIT_OBJECT_0) return {};
    DWORD code = 0;
    if(!GetExitCodeProcess(process, &code)) return {};
    return code;
}

bool TargetProcess::Launch(const std::string& executable, const std::string& arguments,
    const std::string& directory, std::string& error,
    const std::vector<std::pair<std::string,std::string>>& environment,bool hidden)
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
        std::vector<wchar_t> environmentBlock;
        if(!environment.empty())
        {
            struct IgnoreCase { bool operator()(const std::wstring& a,const std::wstring& b) const {return CompareStringOrdinal(a.c_str(),-1,b.c_str(),-1,TRUE)==CSTR_LESS_THAN;} };
            std::map<std::wstring,std::wstring,IgnoreCase> variables;
            auto* inherited=GetEnvironmentStringsW();
            if(!inherited) {error=WindowsError(GetLastError());return false;}
            for(auto* entry=inherited;*entry;entry+=wcslen(entry)+1)
            {
                std::wstring value(entry);const auto equals=value.find(L'=',value[0]==L'=' ? 1 : 0);
                if(equals!=value.npos) variables[value.substr(0,equals)]=value.substr(equals+1);
            }
            FreeEnvironmentStringsW(inherited);
            for(const auto& [name,value]:environment)
            {
                if(name.empty()||name.find('=')!=name.npos) throw std::invalid_argument("Invalid child environment name.");
                variables[ToWide(name)]=ToWide(value);
            }
            for(const auto& [name,value]:variables)
            {
                const auto entry=name+L"="+value;
                environmentBlock.insert(environmentBlock.end(),entry.begin(),entry.end());environmentBlock.push_back(0);
            }
            environmentBlock.push_back(0);
        }
        PROCESS_INFORMATION created{};
        if(!CreateProcessW(path.c_str(), command.data(), nullptr, nullptr, FALSE,
            CREATE_UNICODE_ENVIRONMENT|(hidden ? CREATE_NO_WINDOW : 0),
            environmentBlock.empty() ? nullptr : environmentBlock.data(), folder.c_str(), &startup, &created))
        { error = WindowsError(GetLastError()); return false; }
        CloseHandle(created.hThread);
        if(process) CloseHandle(process);
        process = created.hProcess;
        pid = created.dwProcessId;
        return true;
    }
    catch(const std::exception& exception) { error = exception.what(); return false; }
}

bool TargetProcess::Observe(uint32_t processId,uint64_t creationTime,const std::string& executable,std::string& error)
{
    error.clear();
    if(Running()) {error="An observed target is still running.";return false;}
    HANDLE opened=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION|SYNCHRONIZE,FALSE,processId);
    if(!opened) {error=WindowsError(GetLastError());return false;}
    try
    {
        wchar_t image[32768]{};DWORD length=32768;
        if(!creationTime||Created(opened)!=creationTime||WaitForSingleObject(opened,0)!=WAIT_TIMEOUT
            || !QueryFullProcessImageNameW(opened,0,image,&length)) throw std::runtime_error("Process creation identity does not match a running target.");
        const auto expected=std::filesystem::weakly_canonical(std::filesystem::path(ToWide(executable)));
        const auto actual=std::filesystem::weakly_canonical(std::filesystem::path(image));
        if(CompareStringOrdinal(expected.c_str(),-1,actual.c_str(),-1,TRUE)!=CSTR_EQUAL)
            throw std::runtime_error("Process executable path does not match the selected target.");
        if(process)CloseHandle(process);process=opened;pid=processId;return true;
    }
    catch(const std::exception& exception) {CloseHandle(opened);error=exception.what();return false;}
}

bool TargetProcess::ObserveChild(const TargetProcess& parent,const std::string& executable,std::string& error,
    uint32_t expectedPid,uint64_t expectedCreation)
{
    error.clear();if(Running())return !expectedPid||expectedPid==pid;
    if(!parent.Running()) {error="The launch parent is no longer running.";return false;}
    const auto snapshot=CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0);
    if(snapshot==INVALID_HANDLE_VALUE) {error=WindowsError(GetLastError());return false;}
    PROCESSENTRY32W entry{};entry.dwSize=sizeof(entry);
    uint32_t match=0;uint64_t time=0;
    if(Process32FirstW(snapshot,&entry)) do
    {
        if(entry.th32ParentProcessID!=parent.Pid() || (expectedPid&&entry.th32ProcessID!=expectedPid)) continue;
        HANDLE candidate=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION|SYNCHRONIZE,FALSE,entry.th32ProcessID);
        const auto created=candidate ? Created(candidate) : 0;if(candidate)CloseHandle(candidate);
        if(created<parent.CreationTime() || (expectedCreation&&created!=expectedCreation))continue;
        TargetProcess verified;std::string issue;
        if(!verified.Observe(entry.th32ProcessID,created,executable,issue))continue;
        if(match) {error="More than one matching child; automatic ownership is ambiguous.";match=0;break;}
        match=entry.th32ProcessID;time=created;
    }while(Process32NextW(snapshot,&entry));
    CloseHandle(snapshot);
    return match&&Observe(match,time,executable,error);
}

uint16_t FreeTcpPort()
{
    WSADATA data{};if(WSAStartup(MAKEWORD(2,2),&data))throw std::runtime_error("Winsock initialization failed.");
    SOCKET socket=::socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);uint16_t port=0;
    if(socket!=INVALID_SOCKET)
    {
        const BOOL exclusive=TRUE;setsockopt(socket,SOL_SOCKET,SO_EXCLUSIVEADDRUSE,reinterpret_cast<const char*>(&exclusive),sizeof(exclusive));
        sockaddr_in address{};address.sin_family=AF_INET;int length=sizeof(address);
        if(bind(socket,reinterpret_cast<sockaddr*>(&address),sizeof(address))==0
            &&getsockname(socket,reinterpret_cast<sockaddr*>(&address),&length)==0)port=ntohs(address.sin_port);
        closesocket(socket);
    }
    WSACleanup();if(!port)throw std::runtime_error("No free local Tracy port could be reserved.");return port;
}
bool OwnsTcpListener(uint32_t pid,uint16_t port)
{
    if(!pid||!port)return false;
    DWORD bytes=0;if(GetExtendedTcpTable(nullptr,&bytes,FALSE,AF_INET,TCP_TABLE_OWNER_PID_LISTENER,0)!=ERROR_INSUFFICIENT_BUFFER)return false;
    std::vector<unsigned char> data(bytes);
    if(GetExtendedTcpTable(data.data(),&bytes,FALSE,AF_INET,TCP_TABLE_OWNER_PID_LISTENER,0)!=NO_ERROR)return false;
    const auto* table=reinterpret_cast<const MIB_TCPTABLE_OWNER_PID*>(data.data());
    for(DWORD i=0;i<table->dwNumEntries;++i)
        if(table->table[i].dwOwningPid==pid&&ntohs(uint16_t(table->table[i].dwLocalPort))==port)return true;
    return false;
}
}
