#pragma once

#include "CollectorInternal.h"
#include "dyf/RHI/IDevice.h"
#include <cmath>

namespace dyf::Platform::Detail
{
	inline void CalibrateGpuSpan(
		LiveGpuSpan& span, const RHI::TimestampCalibration& calibration,
		std::uint64_t begin, std::uint64_t end, LiveId domain,
		std::uint64_t submittedNs, std::uint64_t resolvedNs) noexcept
	{
		if(calibration.cpuDomain != RHI::CpuTimestampDomain::QueryPerformanceCounter ||
			!calibration.cpuFrequency || calibration.cpuFrequency != GetLiveClockInfo().frequency ||
			!calibration.gpuValidBits || calibration.gpuValidBits > 64 ||
			!std::isfinite(calibration.gpuPeriodNanoseconds) || calibration.gpuPeriodNanoseconds <= 0)
			return;

		const auto bits = calibration.gpuValidBits;
		const auto mask = bits == 64 ? UINT64_MAX : (std::uint64_t(1) << bits) - 1;
		const auto elapsedTicks = (end - begin) & mask;
		if(bits < 64 && elapsedTicks > mask / 2)
			return;
		if(bits < 64 && (!submittedNs || resolvedNs < submittedNs ||
			static_cast<long double>(resolvedNs - submittedNs) >=
				static_cast<long double>((mask / 2) + 1) * calibration.gpuPeriodNanoseconds))
			return;

		const auto gpuBegin = static_cast<long double>(begin) * calibration.gpuPeriodNanoseconds;
		const auto gpuEnd = static_cast<long double>(end) * calibration.gpuPeriodNanoseconds;
		if(bits == 64 && end >= begin && gpuEnd <= static_cast<long double>(UINT64_MAX))
		{
			span.gpuClockDomain = domain;
			span.hasQueueInterval = true;
			span.beginGpuNs = std::uint64_t(gpuBegin);
			span.endGpuNs = std::uint64_t(gpuEnd);
		}

		long double deltaTicks;
		if(bits == 64)
		{
			deltaTicks = begin >= calibration.gpuTicks
				? static_cast<long double>(begin - calibration.gpuTicks)
				: -static_cast<long double>(calibration.gpuTicks - begin);
		}
		else
		{
			const auto distance = (begin - calibration.gpuTicks) & mask;
			deltaTicks = distance <= mask / 2
				? static_cast<long double>(distance)
				: -static_cast<long double>((mask - distance) + 1);
		}

		const auto cpuBegin = static_cast<long double>(calibration.cpuTicks) * 1000000000.0L /
			calibration.cpuFrequency + deltaTicks * calibration.gpuPeriodNanoseconds;
		const auto cpuEnd = cpuBegin + static_cast<long double>(elapsedTicks) * calibration.gpuPeriodNanoseconds;
		if(!std::isfinite(cpuBegin) || cpuBegin < 0 || !std::isfinite(cpuEnd) ||
			cpuEnd > static_cast<long double>(UINT64_MAX))
			return;

		span.gpuClockDomain = domain;
		span.hasCpuInterval = true;
		span.beginCpuNs = std::uint64_t(cpuBegin);
		span.endCpuNs = std::uint64_t(cpuEnd);
		span.cpuCalibrationErrorKnown = calibration.maximumDeviationNanoseconds.has_value();
		span.cpuCalibrationErrorNs = calibration.maximumDeviationNanoseconds.value_or(0);
	}
}
