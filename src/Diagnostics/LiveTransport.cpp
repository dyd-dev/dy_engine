#include "LiveInternal.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <deque>
#include <mutex>
#include <map>
#include <random>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <utility>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <sddl.h>
#endif

namespace dyf::Diagnostics
{
#ifdef _WIN32
namespace
{
struct Handle
{
    HANDLE value=nullptr;
    Handle()=default;explicit Handle(HANDLE h):value(h){}
    ~Handle(){if(value&&value!=INVALID_HANDLE_VALUE)CloseHandle(value);}
    Handle(const Handle&)=delete;Handle& operator=(const Handle&)=delete;
};
std::wstring Wide(const std::string& s){if(s.empty())return {};const int n=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,s.data(),int(s.size()),nullptr,0);if(!n)return {};std::wstring out(n,L'\0');MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,s.data(),int(s.size()),out.data(),n);return out;}
std::string Utf8(const std::wstring& s){if(s.empty())return {};const int n=WideCharToMultiByte(CP_UTF8,0,s.data(),int(s.size()),nullptr,0,nullptr,nullptr);std::string out(n,'\0');WideCharToMultiByte(CP_UTF8,0,s.data(),int(s.size()),out.data(),n,nullptr,nullptr);return out;}
std::vector<std::uint8_t> TokenInfo(HANDLE token,TOKEN_INFORMATION_CLASS kind)
{DWORD size=0;GetTokenInformation(token,kind,nullptr,0,&size);if(!size||size>65536)return {};std::vector<std::uint8_t> out(size);if(!GetTokenInformation(token,kind,out.data(),size,&size))return {};return out;}
PSID LogonSid(std::vector<std::uint8_t>& groups)
{if(groups.empty())return nullptr;auto* g=reinterpret_cast<TOKEN_GROUPS*>(groups.data());for(DWORD i=0;i<g->GroupCount;++i)if((g->Groups[i].Attributes&SE_GROUP_LOGON_ID)==SE_GROUP_LOGON_ID)return g->Groups[i].Sid;return nullptr;}
bool SameLogin(DWORD pid)
{
    Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid));if(!process.value)return false;
    Handle other,current;if(!OpenProcessToken(process.value,TOKEN_QUERY,&other.value)||!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&current.value))return false;
    auto a=TokenInfo(current.value,TokenUser),b=TokenInfo(other.value,TokenUser),ag=TokenInfo(current.value,TokenGroups),bg=TokenInfo(other.value,TokenGroups);
    auto as=TokenInfo(current.value,TokenSessionId),bs=TokenInfo(other.value,TokenSessionId);auto* al=LogonSid(ag);auto* bl=LogonSid(bg);
    return !a.empty()&&!b.empty()&&al&&bl&&as.size()==sizeof(DWORD)&&bs.size()==sizeof(DWORD)&&
        EqualSid(reinterpret_cast<TOKEN_USER*>(a.data())->User.Sid,reinterpret_cast<TOKEN_USER*>(b.data())->User.Sid)&&EqualSid(al,bl)&&*reinterpret_cast<DWORD*>(as.data())==*reinterpret_cast<DWORD*>(bs.data());
}
bool Peer(HANDLE pipe,bool server,DWORD expected=0)
{ULONG pid=0;const BOOL ok=server?GetNamedPipeClientProcessId(pipe,&pid):GetNamedPipeServerProcessId(pipe,&pid);return ok&&(!expected||pid==expected)&&SameLogin(pid);}
bool Transfer(HANDLE pipe,bool write,void* data,DWORD size,const std::atomic<bool>& stop,DWORD timeout=1000)
{
    auto* bytes=static_cast<std::uint8_t*>(data);DWORD done=0;const auto start=GetTickCount64();
    while(done<size&&!stop){Handle event(CreateEventW(nullptr,TRUE,FALSE,nullptr));if(!event.value)return false;OVERLAPPED overlapped{};overlapped.hEvent=event.value;DWORD count=0;
        const BOOL immediate=write?WriteFile(pipe,bytes+done,size-done,&count,&overlapped):ReadFile(pipe,bytes+done,size-done,&count,&overlapped);
        if(!immediate){if(GetLastError()!=ERROR_IO_PENDING)return false;
            while(WaitForSingleObject(event.value,20)==WAIT_TIMEOUT){if(stop||GetTickCount64()-start>=timeout){CancelIoEx(pipe,&overlapped);WaitForSingleObject(event.value,INFINITE);return false;}}
            if(!GetOverlappedResult(pipe,&overlapped,&count,FALSE))return false;
        }
        if(!count)return false;done+=count;
    }
    return done==size;
}
bool Send(HANDLE pipe,Detail::PacketKind kind,std::vector<std::uint8_t>& payload,const std::atomic<bool>& stop)
{
    Detail::PacketHeader h;h.kind=std::uint32_t(kind);h.size=std::uint32_t(payload.size());if(!Detail::ValidateHeader(h))return false;
    return Transfer(pipe,true,&h,sizeof(h),stop)&&Transfer(pipe,true,payload.data(),h.size,stop);
}
bool Receive(HANDLE pipe,Detail::PacketHeader& h,std::vector<std::uint8_t>& payload,const std::atomic<bool>& stop)
{if(!Transfer(pipe,false,&h,sizeof(h),stop)||!Detail::ValidateHeader(h))return false;payload.resize(h.size);return Transfer(pipe,false,payload.data(),h.size,stop);}
bool Available(HANDLE pipe){DWORD size=0;return PeekNamedPipe(pipe,nullptr,0,nullptr,&size,nullptr)&&size>=sizeof(Detail::PacketHeader);}
const std::string& PipePrefix()
{static const std::string prefix="dy_engine.live.v"+std::to_string(LiveProtocolVersion)+".";return prefix;}
std::string PipeName(LiveSessionId session)
{std::ostringstream s;s<<"\\\\.\\pipe\\"<<PipePrefix()<<GetCurrentProcessId()<<'.'<<std::hex<<session.high<<'.'<<session.low;return s.str();}
struct Server;
std::atomic<Server*> existingServer{nullptr};
struct Server
{
    std::mutex mutex;
    std::atomic<bool> stop{false};
    std::atomic<bool> finished{false};
    std::thread worker;
    std::atomic<HANDLE> pipe{nullptr};
    Server(){existingServer=this;}
    ~Server(){existingServer=nullptr;Stop();}
    void Stop() noexcept {std::lock_guard<std::mutex> lock(mutex);stop=true;auto h=pipe.load();if(h&&h!=INVALID_HANDLE_VALUE)CancelIoEx(h,nullptr);if(worker.joinable())worker.join();}
    void Start()
    {
        std::lock_guard<std::mutex> lock(mutex);if(worker.joinable()){if(!finished)return;worker.join();}stop=false;finished=false;
        const auto snapshot=ReadLiveSnapshot();if(!snapshot)return;const auto name=Wide(PipeName(snapshot->session));
        worker=std::thread([this,name]{Run(name);finished=true;});
    }
    void Run(const std::wstring& name)
    {
        try{
            Handle token;if(!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token.value)){std::fprintf(stderr,"dyf live: token query failed (%lu)\n",GetLastError());return;}
            auto groups=TokenInfo(token.value,TokenGroups);auto* sid=LogonSid(groups);if(!sid){std::fprintf(stderr,"dyf live: logon SID unavailable\n");return;}
            LPWSTR textSid=nullptr;if(!ConvertSidToStringSidW(sid,&textSid)){std::fprintf(stderr,"dyf live: SID conversion failed (%lu)\n",GetLastError());return;}
            const std::wstring acl=L"D:P(A;;0x12019b;;;"+std::wstring(textSid)+L")";LocalFree(textSid);
            PSECURITY_DESCRIPTOR descriptor=nullptr;if(!ConvertStringSecurityDescriptorToSecurityDescriptorW(acl.c_str(),SDDL_REVISION_1,&descriptor,nullptr)){std::fprintf(stderr,"dyf live: pipe security descriptor failed (%lu)\n",GetLastError());return;}
            SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES),descriptor,FALSE};
            Handle handle(CreateNamedPipeW(name.c_str(),PIPE_ACCESS_DUPLEX|FILE_FLAG_OVERLAPPED|FILE_FLAG_FIRST_PIPE_INSTANCE,
                PIPE_TYPE_BYTE|PIPE_READMODE_BYTE|PIPE_WAIT|PIPE_REJECT_REMOTE_CLIENTS,1,65536,65536,0,&security));LocalFree(descriptor);
            if(handle.value==INVALID_HANDLE_VALUE){std::fprintf(stderr,"dyf live: named pipe creation failed (%lu)\n",GetLastError());return;}pipe=handle.value;
            while(!stop){
                Handle event(CreateEventW(nullptr,TRUE,FALSE,nullptr));if(!event.value)break;OVERLAPPED overlapped{};overlapped.hEvent=event.value;
                BOOL connected=ConnectNamedPipe(handle.value,&overlapped);DWORD error=connected?ERROR_SUCCESS:GetLastError();
                if(error==ERROR_IO_PENDING){while(!stop&&WaitForSingleObject(event.value,20)==WAIT_TIMEOUT){}if(stop){CancelIoEx(handle.value,&overlapped);WaitForSingleObject(event.value,INFINITE);break;}DWORD bytes;connected=GetOverlappedResult(handle.value,&overlapped,&bytes,FALSE);}
                else connected=connected||error==ERROR_PIPE_CONNECTED;
                if(!connected||!Peer(handle.value,true)){DisconnectNamedPipe(handle.value);continue;}
                auto last=std::uint64_t(0);
                while(!stop){
                    if(Available(handle.value)){Detail::PacketHeader header;std::vector<std::uint8_t> payload;ControlRequest request;std::string reason;
                        if(!Receive(handle.value,header,payload,stop)||header.kind!=std::uint32_t(Detail::PacketKind::Control)||!Detail::DecodeControl(payload,request,reason))break;
                        const auto accepted=Detail::EnqueueControl(request)!=0;auto ack=Detail::EncodeAck({request.request,accepted,accepted?"":"Duplicate request or control queue full"});if(!Send(handle.value,Detail::PacketKind::ControlAck,ack,stop))break;
                    }
                    const auto now=LiveNowNs();if(now-last>=LivePublishIntervalNs){
                        auto value=ReadLiveSnapshot();if(!value)break;auto bounded=*value;auto payload=Detail::EncodeBoundedSnapshot(bounded);
                        if(!Send(handle.value,Detail::PacketKind::Snapshot,payload,stop))break;last=now;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                }
                CancelIoEx(handle.value,nullptr);DisconnectNamedPipe(handle.value);
            }
            pipe=nullptr;
        }catch(const std::exception& e){std::fprintf(stderr,"dyf live: pipe server failed: %s\n",e.what());pipe=nullptr;}catch(...){std::fprintf(stderr,"dyf live: pipe server failed\n");pipe=nullptr;}
    }
};
Server& GetServer(){static Server server;return server;}
}
#endif

namespace Detail
{
void StartServer(){
#ifdef _WIN32
    if(IsLiveCollectorEnabled())GetServer().Start();
#endif
}
void StopServer() noexcept {
#ifdef _WIN32
    if(auto* server=existingServer.load())server->Stop();
#endif
}
}
struct LiveClient::Impl
{
    mutable std::mutex mutex;
    LiveClientState state=LiveClientState::Disconnected;
    std::string error;
    std::shared_ptr<const LiveSnapshot> snapshot;
    std::uint64_t delivered=0;
    std::deque<ControlRequest> outbound;
    std::map<LiveId,ControlStatus> localRequests;
    std::uint64_t nextRequest=[] {std::random_device r;return ((std::uint64_t(r())<<32)^r())|(std::uint64_t(1)<<63);}();
    std::atomic<bool> stop{false};
    std::thread worker;
    ~Impl(){Disconnect();}
    void Fail(std::string message){std::lock_guard<std::mutex> lock(mutex);state=LiveClientState::Failed;error=std::move(message);outbound.clear();}
    void Disconnect() noexcept {stop=true;if(worker.joinable())worker.join();std::lock_guard<std::mutex> lock(mutex);state=LiveClientState::Disconnected;outbound.clear();localRequests.clear();snapshot.reset();delivered=0;}
#ifdef _WIN32
    void Run(LiveTarget target)
    {
        try{
            const auto name=Wide(target.endpoint);Handle pipe;const auto openDeadline=LiveNowNs()+2000000000ull;DWORD openError=0;
            while(!stop){pipe.value=CreateFileW(name.c_str(),FILE_READ_DATA|FILE_WRITE_DATA|FILE_READ_ATTRIBUTES|SYNCHRONIZE,0,nullptr,OPEN_EXISTING,FILE_FLAG_OVERLAPPED|SECURITY_SQOS_PRESENT|SECURITY_IDENTIFICATION,nullptr);
                if(pipe.value!=INVALID_HANDLE_VALUE)break;openError=::GetLastError();if((openError!=ERROR_PIPE_BUSY&&openError!=ERROR_FILE_NOT_FOUND)||LiveNowNs()>=openDeadline)break;WaitNamedPipeW(name.c_str(),50);std::this_thread::sleep_for(std::chrono::milliseconds(5));}
            if(pipe.value==INVALID_HANDLE_VALUE||stop){if(!stop)Fail("Named pipe open failed ("+std::to_string(openError)+")");return;}
            if(!Peer(pipe.value,false,target.processId)){Fail("Named pipe peer is outside this login/session");return;}
            std::uint64_t lastReceive=LiveNowNs();
            while(!stop){
                std::deque<ControlRequest> send;{std::lock_guard<std::mutex> lock(mutex);send.swap(outbound);}
                for(const auto& request:send){auto bytes=Detail::EncodeControl(request);if(!Send(pipe.value,Detail::PacketKind::Control,bytes,stop)){Fail("Control write failed");return;}}
                if(Available(pipe.value)){Detail::PacketHeader header;std::vector<std::uint8_t> payload;auto value=std::make_shared<LiveSnapshot>();std::string reason;
                    if(!Receive(pipe.value,header,payload,stop)){Fail("Invalid live packet");return;}
                    if(header.kind==std::uint32_t(Detail::PacketKind::ControlAck)){Detail::ControlAck ack;if(!Detail::DecodeAck(payload,ack,reason)){Fail(reason);return;}if(!ack.accepted){std::lock_guard<std::mutex> lock(mutex);auto it=localRequests.find(ack.request);if(it!=localRequests.end()){it->second.state=ControlState::Rejected;it->second.message=ack.message;}}continue;}
                    if(header.kind!=std::uint32_t(Detail::PacketKind::Snapshot)||!Detail::DecodeSnapshot(payload,*value,reason)||value->session!=target.session){Fail(reason.empty()?"Invalid live session/protocol":reason);return;}
                    {std::lock_guard<std::mutex> lock(mutex);for(const auto& item:localRequests)if(item.second.state==ControlState::Rejected){value->controlStatus.erase(std::remove_if(value->controlStatus.begin(),value->controlStatus.end(),[&](const auto& r){return r.request==item.first;}),value->controlStatus.end());value->controlStatus.push_back(item.second);}snapshot=std::move(value);state=LiveClientState::Connected;error.clear();}lastReceive=LiveNowNs();
                }
                if(LiveNowNs()-lastReceive>2000000000ull){Fail("Live target disconnected or unresponsive");return;}
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        }catch(const std::exception& e){Fail(e.what());}catch(...){Fail("Live client failed");}
    }
#endif
};
LiveClient::LiveClient():impl(std::make_unique<Impl>()){}
LiveClient::~LiveClient()=default;
LiveClient::LiveClient(LiveClient&&) noexcept=default;
LiveClient& LiveClient::operator=(LiveClient&&) noexcept=default;
std::vector<LiveTarget> LiveClient::Discover()
{
    std::vector<LiveTarget> targets;
#ifdef _WIN32
    WIN32_FIND_DATAW data{};HANDLE search=FindFirstFileW(L"\\\\.\\pipe\\*",&data);if(search==INVALID_HANDLE_VALUE)return targets;
    do{const auto full=Utf8(data.cFileName);const auto slash=full.find_last_of('\\');const auto name=slash==std::string::npos?full:full.substr(slash+1);unsigned pid=0;unsigned long long high=0,low=0;
        if(name.rfind(PipePrefix(),0)!=0||std::sscanf(name.c_str()+PipePrefix().size(),"%u.%llx.%llx",&pid,&high,&low)!=3||!SameLogin(pid))continue;
        LiveTarget target;target.session={high,low};target.processId=pid;target.endpoint="\\\\.\\pipe\\"+full;
        Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid));wchar_t path[32768]{};DWORD length=32768;
        if(process.value&&QueryFullProcessImageNameW(process.value,0,path,&length))target.executable=Utf8(std::wstring(path,length));targets.push_back(std::move(target));
    }while(FindNextFileW(search,&data));FindClose(search);
#endif
    return targets;
}
bool LiveClient::Connect(const LiveTarget& target)
{
    if(!impl)impl=std::make_unique<Impl>();impl->Disconnect();
#ifdef _WIN32
    if(target.endpoint.rfind("\\\\.\\pipe\\",0)!=0)return false;
    const auto suffix=target.endpoint.substr(9);const auto slash=suffix.find_last_of('\\');const auto leaf=slash==std::string::npos?suffix:suffix.substr(slash+1);
    if(leaf.rfind(PipePrefix(),0)!=0||(slash!=std::string::npos&&(suffix.rfind("Sessions\\",0)!=0||suffix.find("\\AppContainerNamedObjects\\S-1-15-2-")==std::string::npos)))return false;
    {std::lock_guard<std::mutex> lock(impl->mutex);impl->state=LiveClientState::Connecting;impl->error.clear();}impl->stop=false;impl->worker=std::thread([state=impl.get(),target]{state->Run(target);});return true;
#else
    (void)target;impl->state=LiveClientState::Unsupported;return false;
#endif
}
void LiveClient::Disconnect() noexcept {if(impl)impl->Disconnect();}
LiveClientState LiveClient::GetState() const noexcept {if(!impl)return LiveClientState::Disconnected;std::lock_guard<std::mutex> lock(impl->mutex);return impl->state;}
std::string LiveClient::GetLastError() const {if(!impl)return {};std::lock_guard<std::mutex> lock(impl->mutex);return impl->error;}
std::shared_ptr<const LiveSnapshot> LiveClient::Poll()
{if(!impl)return {};std::lock_guard<std::mutex> lock(impl->mutex);if(!impl->snapshot||impl->delivered==impl->snapshot->sequence)return {};impl->delivered=impl->snapshot->sequence;return impl->snapshot;}
LiveId LiveClient::RequestControl(LiveId control,const ControlValue& value)
{
    if(!impl||!control)return 0;
    std::lock_guard<std::mutex> lock(impl->mutex);if(impl->state!=LiveClientState::Connected||impl->outbound.size()>=64)return 0;
    if(!impl->nextRequest){impl->error="Request ID space exhausted";return 0;}
    const auto id=impl->nextRequest++;impl->outbound.push_back({id,control,value});ControlStatus local;local.request=id;local.control=control;local.requested=value;impl->localRequests[id]=std::move(local);while(impl->localRequests.size()>256)impl->localRequests.erase(impl->localRequests.begin());return id;
}
}
