#include <windows.h>
#include <dbghelp.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <cwchar>
#include <cerrno>

// External Windows x64 debugger: only the explicitly launched process is monitored.
// The target inherits stdout/stderr; monitor diagnostics never share those streams.
struct Result
{
    DWORD pid=0, exitCode=0, error=0, exception=0, thread=0;
    bool exited=false, crashed=false;
    const char* dump="not_triggered";
};

static bool Save(const std::filesystem::path& folder, const Result& r)
{
    auto temporary=folder/L"crash.tmp";
    std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
    out << "{\"monitor_pid\":" << GetCurrentProcessId()
        << ",\"target_pid\":" << (r.pid ? std::to_string(r.pid) : "null")
        << ",\"target_exit_code\":" << (r.exited ? std::to_string(r.exitCode) : "null")
        << ",\"crash_observed\":" << (r.crashed ? "true" : "false")
        << ",\"dump_status\":\"" << r.dump << "\",\"monitor_error\":" << r.error
        << ",\"exception_code\":" << r.exception << ",\"exception_thread\":" << r.thread << "}\n";
    out.flush();
    if(!out) return false;
    out.close();
    return MoveFileExW(temporary.c_str(),(folder/L"crash.json").c_str(),MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)!=0;
}

// CRT-compatible quoting, including embedded quotes and trailing backslashes.
static std::wstring Quote(const std::wstring& value)
{
    std::wstring out=L"\"";
    size_t slashes=0;
    for(wchar_t c:value)
    {
        if(c==L'\\') {++slashes; continue;}
        out.append(c==L'"' ? slashes*2+1 : slashes,L'\\');
        out+=c; slashes=0;
    }
    out.append(slashes*2,L'\\');
    out+=L'"';
    return out;
}

static void Dump(HANDLE process, const DEBUG_EVENT& event, const std::filesystem::path& folder, Result& r)
{
    r.crashed=true;
    r.exception=event.u.Exception.ExceptionRecord.ExceptionCode;
    r.thread=event.dwThreadId;
    r.dump="failed";
    HANDLE thread=OpenThread(THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION,FALSE,event.dwThreadId);
    if(!thread) {r.error=GetLastError(); return;}
    CONTEXT context{};
    context.ContextFlags=CONTEXT_FULL;
    const BOOL contextRead=GetThreadContext(thread,&context);
    const DWORD contextError=contextRead ? 0 : GetLastError();
    CloseHandle(thread);
    if(!contextRead) {r.error=contextError; return;}
    EXCEPTION_RECORD record=event.u.Exception.ExceptionRecord;
    record.ExceptionRecord=nullptr; // The chained pointer belongs to the target, not this process.
    EXCEPTION_POINTERS pointers{&record,&context};
    MINIDUMP_EXCEPTION_INFORMATION info{event.dwThreadId,&pointers,FALSE};
    const auto temporary=folder/L"crash.dmp.partial";
    HANDLE file=CreateFileW(temporary.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file==INVALID_HANDLE_VALUE) {r.error=GetLastError(); return;}
    BOOL saved=MiniDumpWriteDump(process,event.dwProcessId,file,
        static_cast<MINIDUMP_TYPE>(MiniDumpNormal | MiniDumpWithThreadInfo),&info,nullptr,nullptr);
    DWORD error=saved ? 0 : GetLastError();
    if(saved && !FlushFileBuffers(file)) {saved=FALSE; error=GetLastError();}
    CloseHandle(file);
    if(saved && MoveFileExW(temporary.c_str(),(folder/L"crash.dmp").c_str(),MOVEFILE_WRITE_THROUGH))
        r.dump="captured";
    else {r.dump="partial"; r.error=error ? error : GetLastError();}
}

int wmain(int argc, wchar_t** argv)
{
    if(argc<4) return 64; // monitor <existing report directory> <cwd> <exe> [args...]
    const bool attached = argc==5 && std::wstring_view(argv[1])==L"--attach";
    const std::filesystem::path folder=argv[attached ? 2 : 1];
    Result result;
    std::ofstream capture(folder/L"capture.log",std::ios::binary);
    if(!capture || !Save(folder,result)) return 74;
    PROCESS_INFORMATION process{};
    HANDLE ready=nullptr;
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    if(attached)
    {
        wchar_t* end=nullptr;
        errno=0;
        const auto pid=std::wcstoul(argv[3],&end,10);
        if(errno || !pid || end==argv[3] || *end || argv[3][0]==L'-') return 64;
        ready=OpenEventW(EVENT_MODIFY_STATE,FALSE,argv[4]);
        process.hProcess=OpenProcess(PROCESS_ALL_ACCESS,FALSE,pid);
        BOOL wow64=FALSE;
        if(!ready || !process.hProcess || !IsWow64Process(process.hProcess,&wow64) || wow64 || !DebugActiveProcess(pid))
        {
            result.error=wow64 ? ERROR_BAD_EXE_FORMAT : GetLastError();
            if(!result.error) result.error=ERROR_INVALID_PARAMETER;
            if(ready) CloseHandle(ready);
            if(process.hProcess) CloseHandle(process.hProcess);
            Save(folder,result);
            return 71;
        }
        process.dwProcessId=pid;
        // An auto-started observer must not kill its parent when the observer exits.
        if(!DebugSetProcessKillOnExit(FALSE))
        {
            result.error=GetLastError();
            DebugActiveProcessStop(pid);
            CloseHandle(ready); CloseHandle(process.hProcess);
            Save(folder,result);
            return 71;
        }
    }
    else
    {
        DWORD binaryType=0;
        if(!GetBinaryTypeW(argv[3],&binaryType) || binaryType!=SCS_64BIT_BINARY)
        {
            result.error=ERROR_BAD_EXE_FORMAT;
            result.dump="unavailable";
            Save(folder,result);
            return 65;
        }
        std::wstring command;
        for(int i=3;i<argc;++i) {if(i>3)command+=L' '; command+=Quote(argv[i]);}
        STARTUPINFOW startup{};
        startup.cb=sizeof(startup);
        startup.dwFlags=STARTF_USESTDHANDLES;
        startup.hStdInput=GetStdHandle(STD_INPUT_HANDLE);
        startup.hStdOutput=GetStdHandle(STD_OUTPUT_HANDLE);
        startup.hStdError=GetStdHandle(STD_ERROR_HANDLE);
        if(!CreateProcessW(argv[3],command.data(),nullptr,nullptr,TRUE,
            DEBUG_ONLY_THIS_PROCESS | CREATE_NO_WINDOW,nullptr,argv[2],&startup,&process))
        {
            result.error=GetLastError();
            Save(folder,result);
            return 71;
        }
    }
    result.pid=process.dwProcessId;
    capture << "target_pid=" << result.pid << '\n'; capture.flush();
    bool initialBreakpoint=true;
    if(!Save(folder,result)) result.error=ERROR_WRITE_FAULT;
    // A report/dump storage error must not prevent consuming debug events.
    while(!result.exited)
    {
        DEBUG_EVENT event{};
        if(!WaitForDebugEvent(&event,INFINITE)) {result.error=GetLastError(); break;}
        DWORD continuation=DBG_CONTINUE;
        bool signalReady=false;
        if(event.dwDebugEventCode==CREATE_PROCESS_DEBUG_EVENT)
        {
            if(event.u.CreateProcessInfo.hFile) CloseHandle(event.u.CreateProcessInfo.hFile);
            // Debug-event process/thread handles are released by ContinueDebugEvent on exit.
        }
        else if(event.dwDebugEventCode==LOAD_DLL_DEBUG_EVENT)
        {
            if(event.u.LoadDll.hFile) CloseHandle(event.u.LoadDll.hFile);
        }
        else if(event.dwDebugEventCode==EXCEPTION_DEBUG_EVENT)
        {
            auto& exception=event.u.Exception;
            continuation=DBG_EXCEPTION_NOT_HANDLED;
            if(initialBreakpoint && exception.dwFirstChance && exception.ExceptionRecord.ExceptionCode==EXCEPTION_BREAKPOINT)
            {
                initialBreakpoint=false;
                continuation=DBG_CONTINUE;
                signalReady=attached;
            }
            else if(!exception.dwFirstChance && !result.crashed)
            {
                Dump(process.hProcess,event,folder,result);
                capture << "exception=" << result.exception << " dump=" << result.dump << '\n'; capture.flush();
                if(!Save(folder,result)) result.error=ERROR_WRITE_FAULT;
            }
        }
        else if(event.dwDebugEventCode==EXIT_PROCESS_DEBUG_EVENT)
        {
            result.exited=true;
            result.exitCode=event.u.ExitProcess.dwExitCode;
        }
        if(!ContinueDebugEvent(event.dwProcessId,event.dwThreadId,continuation)) {result.error=GetLastError(); break;}
        if(signalReady && !SetEvent(ready)) result.error=GetLastError();
    }
    if(!result.exited)
    {
        if(attached) DebugActiveProcessStop(process.dwProcessId);
        else {TerminateProcess(process.hProcess,74); WaitForSingleObject(process.hProcess,5000);}
    }
    if(ready) CloseHandle(ready);
    if(process.hThread) CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    if(!Save(folder,result)) return 74;
    return result.error ? 74 : 0;
}
