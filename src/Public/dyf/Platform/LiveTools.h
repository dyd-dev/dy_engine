#pragma once
#include "dyf/Diagnostics/LiveProfiling.h"
#include <array>

namespace dyf::Platform
{
class LiveTools final
{
public:
    LiveTools(Diagnostics::LiveId device,Diagnostics::LiveId source);
    void Poll(Diagnostics::LiveFrameKey);
private:
    enum Slot { Tracy,Bookmark,Capture,Open,Captures,Connected,Connect,Count };
    Diagnostics::LiveId owner=0;
    std::array<Diagnostics::ControlRegistration,Count> controls;
    std::uint64_t nextRefresh=0;
    void Refresh();
};
}
