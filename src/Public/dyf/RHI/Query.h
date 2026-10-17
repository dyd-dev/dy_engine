#pragma once
#include <cstdint>
#include <optional>

namespace dyf::RHI
{
enum class CpuTimestampDomain : uint8_t { Unsupported, QueryPerformanceCounter };
struct TimestampCalibration
{
    CpuTimestampDomain cpuDomain = CpuTimestampDomain::Unsupported;
    uint64_t cpuTicks = 0, cpuFrequency = 0, gpuTicks = 0;
    double gpuPeriodNanoseconds = 0;
    uint32_t gpuValidBits = 0;
    // CPU and GPU ticks have separate origins. An absent bound is not zero error.
    std::optional<uint64_t> maximumDeviationNanoseconds;
};
struct TimestampQueryDesc { uint32_t count=0; };
class TimestampQuery
{
public:
    TimestampQuery(const TimestampQuery&) = delete;
    TimestampQuery& operator=(const TimestampQuery&) = delete;
    uint32_t GetCount() const { return m_count; }
protected:
    explicit TimestampQuery(uint32_t count):m_count(count) {}
    virtual ~TimestampQuery()=default;
private:
    uint32_t m_count;
};
using TimestampQueryHandle=TimestampQuery*;
}
