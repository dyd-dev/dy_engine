#pragma once
#include <cstdint>

namespace dyf::RHI
{
// Native upload-page counters, distinct from public Buffer allocations. Request
// bytes describe requested spans (including row padding, excluding alignment
// gaps between allocations), not source asset sizes or total page capacity.
struct UploadMemoryStatistics
{
    uint64_t allocationCount=0, reuseCount=0, allocationBytes=0;
    uint64_t requestCount=0, requestedBytes=0;
    uint64_t cachedBytes=0, inUseBytes=0, peakInUseBytes=0;
};
}
