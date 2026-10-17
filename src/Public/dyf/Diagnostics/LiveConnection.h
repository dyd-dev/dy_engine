#pragma once
#include "dyf/Diagnostics/LiveProfiling.h"

namespace dyf::Diagnostics
{
// Optional Windows named-pipe connection. Collection never starts this service.
// One application owner starts it after enabling collection and stops it before
// shutdown. Re-enabling collection requires an explicit start again.
void StartLiveConnection();
void StopLiveConnection() noexcept;
class LiveConnectionScope final
{
public:
    LiveConnectionScope() { StartLiveConnection(); }
    ~LiveConnectionScope() { StopLiveConnection(); }
    LiveConnectionScope(const LiveConnectionScope&)=delete;
    LiveConnectionScope& operator=(const LiveConnectionScope&)=delete;
};

struct LiveTarget { LiveSessionId session; std::uint32_t processId=0; std::string executable,endpoint; };
enum class LiveClientState : std::uint8_t { Disconnected,Connecting,Connected,Failed,Unsupported };
class LiveClient final
{
public:
    LiveClient();
    ~LiveClient();
    LiveClient(LiveClient&&) noexcept;
    LiveClient& operator=(LiveClient&&) noexcept;
    LiveClient(const LiveClient&)=delete;
    LiveClient& operator=(const LiveClient&)=delete;
    static std::vector<LiveTarget> Discover();
    bool Connect(const LiveTarget&);
    void Disconnect() noexcept;
    LiveClientState GetState() const noexcept;
    std::string GetLastError() const;
    // Returns only a new immutable snapshot. Ownership survives reconnect/disconnect.
    std::shared_ptr<const LiveSnapshot> Poll();
    LiveId RequestControl(LiveId,const ControlValue&);
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

}
