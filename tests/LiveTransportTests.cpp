#include "dyf/Diagnostics/LiveProfiling.h"
#include <chrono>
#include <iostream>
#include <thread>
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
using namespace dyf::Diagnostics;
int main(int argc,char** argv)
{
    if(argc!=2)return 1;int identity=0;const auto device=RegisterLiveObject(&identity,{LiveObjectKind::Device,0,0,0,0,"transport fixture"});const auto source=AllocateLiveId();bool actual=false;
    ControlDesc desc;desc.device=device;desc.source=source;desc.owner=source;desc.key="transport.toggle";desc.label=desc.key;
    auto control=RegisterControl(desc,[&](const ControlRequest& request,LiveFrameKey){actual=std::get<bool>(request.value);return ControlApplyResult{ControlState::Applied,actual,{}};});
    {
        {LiveFrameScope frame(device,source);for(unsigned i=0;i<8192;++i){LiveEvent e;e.kind=LiveEventKind::CpuWork;RecordLiveEvent(e);}frame.SetOutcome(LiveOutcome::Completed);}
        LiveTarget target;const auto discoverDeadline=LiveNowNs()+2000000000ull;
        while(!target.processId&&LiveNowNs()<discoverDeadline){for(const auto& candidate:LiveClient::Discover())if(candidate.processId==GetCurrentProcessId()){target=candidate;break;}if(!target.processId)std::this_thread::sleep_for(std::chrono::milliseconds(5));}
        if(!target.processId)return 6;
        const int chars=MultiByteToWideChar(CP_UTF8,0,target.endpoint.c_str(),-1,nullptr,0);std::wstring endpoint(chars,L'\0');MultiByteToWideChar(CP_UTF8,0,target.endpoint.c_str(),-1,endpoint.data(),chars);
        HANDLE stalled=CreateFileW(endpoint.c_str(),FILE_READ_DATA|FILE_WRITE_DATA|FILE_READ_ATTRIBUTES|SYNCHRONIZE,0,nullptr,OPEN_EXISTING,FILE_FLAG_OVERLAPPED|SECURITY_SQOS_PRESENT|SECURITY_IDENTIFICATION,nullptr);
        if(stalled==INVALID_HANDLE_VALUE){std::cerr<<"slow reader open failed "<<GetLastError()<<"\n";return 7;}
        DWORD available=0;const auto fillDeadline=LiveNowNs()+2000000000ull;
        while(available<32768&&LiveNowNs()<fillDeadline){if(!PeekNamedPipe(stalled,nullptr,0,nullptr,&available,nullptr))break;std::this_thread::sleep_for(std::chrono::milliseconds(5));}
        if(available<32768){CloseHandle(stalled);return 8;}
        const auto started=LiveNowNs();
        for(unsigned i=0;i<256;++i){LiveFrameScope frame(device,source);LiveEvent e;e.kind=LiveEventKind::Command;e.commandKind=LiveCommandKind::Draw;e.arguments={3,1,0,0,0};RecordLiveEvent(e);frame.SetOutcome(LiveOutcome::Completed);}
        const auto elapsed=LiveNowNs()-started;CloseHandle(stalled);if(elapsed>=750000000ull)return 9;
        const auto snapshot=ReadLiveSnapshot();for(const auto& current:snapshot->sources)if(current.source==source&&current.latest.recorded.draw!=1)return 10;
        std::cout<<"Slow pipe reader: 256 producer frames completed in "<<elapsed/1000000.0<<" ms without consuming snapshots\n";
    }
    for(int pass=0;pass<2;++pass){
        const int count=MultiByteToWideChar(CP_UTF8,0,argv[1],-1,nullptr,0);std::wstring executable(count,L'\0');MultiByteToWideChar(CP_UTF8,0,argv[1],-1,executable.data(),count);executable.pop_back();
        std::wstring command=L"\""+executable+L"\" "+std::to_wstring(GetCurrentProcessId())+L" "+(pass==0?L"1":L"0");
        STARTUPINFOW startup{};startup.cb=sizeof(startup);PROCESS_INFORMATION process{};
        startup.dwFlags=STARTF_USESTDHANDLES;startup.hStdInput=GetStdHandle(STD_INPUT_HANDLE);startup.hStdOutput=GetStdHandle(STD_OUTPUT_HANDLE);startup.hStdError=GetStdHandle(STD_ERROR_HANDLE);
        SetHandleInformation(startup.hStdOutput,HANDLE_FLAG_INHERIT,HANDLE_FLAG_INHERIT);SetHandleInformation(startup.hStdError,HANDLE_FLAG_INHERIT,HANDLE_FLAG_INHERIT);
        if(!CreateProcessW(executable.c_str(),command.data(),nullptr,nullptr,TRUE,CREATE_NO_WINDOW,nullptr,nullptr,&startup,&process))return 2;
        const auto deadline=LiveNowNs()+8000000000ull;
        while(WaitForSingleObject(process.hProcess,0)==WAIT_TIMEOUT&&LiveNowNs()<deadline){LiveFrameScope frame(device,source);PollControls(source,frame.GetKey());frame.SetOutcome(LiveOutcome::Completed);std::this_thread::sleep_for(std::chrono::milliseconds(3));}
        DWORD exitCode=1;GetExitCodeProcess(process.hProcess,&exitCode);if(exitCode==STILL_ACTIVE)TerminateProcess(process.hProcess,10);CloseHandle(process.hThread);CloseHandle(process.hProcess);
        if(exitCode!=0||actual!=(pass==0)){std::cerr<<"client process "<<pass<<" exit="<<exitCode<<" actual="<<actual<<" own_discovery="<<LiveClient::Discover().size()<<"\n";return 3;}
    }
    const auto snapshot=ReadLiveSnapshot();std::vector<LiveId> applied;for(const auto& status:snapshot->controlStatus)if(status.state==ControlState::Applied)applied.push_back(status.request);
    if(applied.size()!=2||applied[0]==applied[1])return 4;
    std::cout<<"Two new producer-off client processes applied opposite controls, unique request IDs\n";SetLiveCollectorEnabled(false);
}
