#include "Graphics/ProfilerSampler.h"

#include <algorithm>
#include <cmath>

namespace dy::Graphics
{
	ProfilerSampler::ProfilerSampler(double publishIntervalMilliseconds, double pauseThresholdMilliseconds)
		: m_publishIntervalMilliseconds(std::max(publishIntervalMilliseconds, 1.0))
		, m_pauseThresholdMilliseconds(std::max(pauseThresholdMilliseconds, m_publishIntervalMilliseconds))
	{
	}

	bool ProfilerSampler::AddSample(const ProfilerTimingSample& sample, ProfilerTimingSnapshot& snapshot)
	{
		if(!std::isfinite(sample.frameMilliseconds) || sample.frameMilliseconds <= 0.0)
		{
			return false;
		}
		if(sample.frameMilliseconds >= m_pauseThresholdMilliseconds)
		{
			Reset();
			return false;
		}

		m_frameSumMilliseconds += sample.frameMilliseconds;
		m_frameMaximumMilliseconds = std::max(m_frameMaximumMilliseconds, sample.frameMilliseconds);
		m_cpuSumMilliseconds += std::max(sample.cpuRenderMilliseconds, 0.0);
		++m_frameCount;

		if(sample.hasGpuMain && std::isfinite(sample.gpuMainMilliseconds) && sample.gpuMainMilliseconds >= 0.0)
		{
			m_gpuMainSumMilliseconds += sample.gpuMainMilliseconds;
			++m_gpuMainCount;
		}
		if(sample.hasGpuShadow && std::isfinite(sample.gpuShadowMilliseconds) && sample.gpuShadowMilliseconds >= 0.0)
		{
			m_gpuShadowSumMilliseconds += sample.gpuShadowMilliseconds;
			++m_gpuShadowCount;
		}

		if(m_frameSumMilliseconds + 0.1 < m_publishIntervalMilliseconds)
		{
			return false;
		}

		snapshot = {};
		snapshot.sampleWindowMilliseconds = m_frameSumMilliseconds;
		snapshot.frameCount = m_frameCount;
		snapshot.fps = static_cast<double>(m_frameCount) * 1000.0 / m_frameSumMilliseconds;
		snapshot.frameAverageMilliseconds = m_frameSumMilliseconds / static_cast<double>(m_frameCount);
		snapshot.frameMaximumMilliseconds = m_frameMaximumMilliseconds;
		snapshot.cpuRenderAverageMilliseconds = m_cpuSumMilliseconds / static_cast<double>(m_frameCount);
		snapshot.hasGpuMain = m_gpuMainCount > 0u;
		snapshot.hasGpuShadow = m_gpuShadowCount > 0u;
		if(snapshot.hasGpuMain)
		{
			snapshot.gpuMainAverageMilliseconds = m_gpuMainSumMilliseconds / static_cast<double>(m_gpuMainCount);
		}
		if(snapshot.hasGpuShadow)
		{
			snapshot.gpuShadowAverageMilliseconds = m_gpuShadowSumMilliseconds / static_cast<double>(m_gpuShadowCount);
		}
		Reset();
		return true;
	}

	void ProfilerSampler::Reset()
	{
		m_frameSumMilliseconds = 0.0;
		m_frameMaximumMilliseconds = 0.0;
		m_cpuSumMilliseconds = 0.0;
		m_gpuMainSumMilliseconds = 0.0;
		m_gpuShadowSumMilliseconds = 0.0;
		m_frameCount = 0;
		m_gpuMainCount = 0;
		m_gpuShadowCount = 0;
	}
}
