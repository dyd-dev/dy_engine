#pragma once

#include <cstdint>

namespace dyf::Platform { struct ProfilerTimingSample; struct ProfilerTimingSnapshot; }
namespace dyf::RHI { struct ResourceAllocationCounters; }

namespace dyf::Diagnostics
{
// Optional external instrumentation. Internal timing, snapshots and the HUD do
// not depend on the connection being enabled or a viewer being present.
class CpuZone final
{
public:
    CpuZone(const char* name,const char* function,const char* file,std::uint32_t line) noexcept;
    ~CpuZone();
    CpuZone(const CpuZone&)=delete;
    CpuZone& operator=(const CpuZone&)=delete;
private:
    std::uint32_t id=0;
    bool active=false;
};
void MarkProfilerFrame() noexcept;
// Plot names must have static lifetime, as required by the external adapter.
void PublishProfilerValue(const char* name,double value,bool raw=false) noexcept;
void PublishProfilerRawFrame(const Platform::ProfilerTimingSample&) noexcept;
void PublishProfilerSnapshot(const Platform::ProfilerTimingSnapshot&,const RHI::ResourceAllocationCounters&) noexcept;
}

#define DY_PROFILE_JOIN_IMPL(a,b) a##b
#define DY_PROFILE_JOIN(a,b) DY_PROFILE_JOIN_IMPL(a,b)
#define DY_PROFILE_CPU_ZONE() DY_PROFILE_CPU_ZONE_NAMED(__func__)
#define DY_PROFILE_CPU_ZONE_NAMED(name) \
    ::dyf::Diagnostics::CpuZone DY_PROFILE_JOIN(dyProfileZone_,__LINE__)(name,__func__,__FILE__,__LINE__)
#define DY_PROFILE_FRAME_MARK() ::dyf::Diagnostics::MarkProfilerFrame()
#define DY_PROFILE_GPU_MILLISECONDS(name,value) ::dyf::Diagnostics::PublishProfilerValue(name,static_cast<double>(value))
#define DY_PROFILE_RESOURCE_COUNT(name,value) ::dyf::Diagnostics::PublishProfilerValue(name,static_cast<double>(value))
#define DY_PROFILE_RAW_VALUE(name,value) ::dyf::Diagnostics::PublishProfilerValue(name,static_cast<double>(value),true)
