#pragma once
#include "dyf/Diagnostics/LiveProfiling.h"
#include "dyf/RHI/Query.h"
#include <cmath>

namespace dyf::Diagnostics::Detail
{
inline void CalibrateGpuSpan(LiveGpuSpan& span,const RHI::TimestampCalibration& pair,
    std::uint64_t begin,std::uint64_t end,LiveId domain,std::uint64_t submittedNs,std::uint64_t resolvedNs) noexcept
{
    if(pair.cpuDomain!=RHI::CpuTimestampDomain::QueryPerformanceCounter||!pair.cpuFrequency||
        pair.cpuFrequency!=GetLiveClockInfo().frequency||!pair.gpuValidBits||pair.gpuValidBits>64||
        !std::isfinite(pair.gpuPeriodNanoseconds)||pair.gpuPeriodNanoseconds<=0)return;
    const auto bits=pair.gpuValidBits;const auto mask=bits==64?UINT64_MAX:(std::uint64_t(1)<<bits)-1;
    const auto elapsed=(end-begin)&mask;if(bits<64&&elapsed>mask/2)return;
    if(bits<64&&(!submittedNs||resolvedNs<submittedNs||static_cast<long double>(resolvedNs-submittedNs)>=static_cast<long double>((mask/2)+1)*pair.gpuPeriodNanoseconds))return;
    const auto gpuBegin=static_cast<long double>(begin)*pair.gpuPeriodNanoseconds;
    const auto gpuEnd=static_cast<long double>(end)*pair.gpuPeriodNanoseconds;
    if(bits==64&&end>=begin&&gpuEnd<=static_cast<long double>(UINT64_MAX)){span.gpuClockDomain=domain;span.hasQueueInterval=true;span.beginGpuNs=std::uint64_t(gpuBegin);span.endGpuNs=std::uint64_t(gpuEnd);}
    long double delta;
    if(bits==64)delta=begin>=pair.gpuTicks?static_cast<long double>(begin-pair.gpuTicks):-static_cast<long double>(pair.gpuTicks-begin);
    else{const auto distance=(begin-pair.gpuTicks)&mask;delta=distance<=mask/2?static_cast<long double>(distance):-static_cast<long double>((mask-distance)+1);}
    const auto cpu=static_cast<long double>(pair.cpuTicks)*1000000000.0L/pair.cpuFrequency+delta*pair.gpuPeriodNanoseconds;
    const auto finish=cpu+static_cast<long double>(elapsed)*pair.gpuPeriodNanoseconds;
    if(!std::isfinite(cpu)||cpu<0||!std::isfinite(finish)||finish>static_cast<long double>(UINT64_MAX))return;
    span.gpuClockDomain=domain;span.hasCpuInterval=true;span.beginCpuNs=std::uint64_t(cpu);span.endCpuNs=std::uint64_t(finish);
    span.cpuCalibrationErrorKnown=pair.maximumDeviationNanoseconds.has_value();span.cpuCalibrationErrorNs=pair.maximumDeviationNanoseconds.value_or(0);
}
}
