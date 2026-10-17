#include "dyf/Platform/ProfilerSampler.h"

#include <algorithm>
#include <cmath>

namespace dyf::Platform
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
		m_cpuSumMilliseconds += std::isfinite(sample.cpuRenderMilliseconds)
            ? std::max(sample.cpuRenderMilliseconds, 0.0) : 0.0;
		++m_frameCount;
		const std::array<double,6> stages={sample.cpuAcquireMilliseconds,sample.cpuPrepareMilliseconds,
			sample.cpuRecordMilliseconds,sample.cpuSubmitMilliseconds,sample.cpuPresentMilliseconds,sample.cpuReadbackMilliseconds};
		for(size_t i=0;i<stages.size();++i)
			if(std::isfinite(stages[i]) && stages[i]>=0.0) m_cpuStageSums[i]+=stages[i];

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
		snapshot.sequence=++m_sequence;
		snapshot.sampleWindowMilliseconds = m_frameSumMilliseconds;
		snapshot.frameCount = m_frameCount;
		snapshot.fps = static_cast<double>(m_frameCount) * 1000.0 / m_frameSumMilliseconds;
		snapshot.frameAverageMilliseconds = m_frameSumMilliseconds / static_cast<double>(m_frameCount);
		snapshot.frameMaximumMilliseconds = m_frameMaximumMilliseconds;
		snapshot.cpuRenderAverageMilliseconds = m_cpuSumMilliseconds / static_cast<double>(m_frameCount);
		snapshot.cpuAcquireAverageMilliseconds=m_cpuStageSums[0]/m_frameCount;
		snapshot.cpuPrepareAverageMilliseconds=m_cpuStageSums[1]/m_frameCount;
		snapshot.cpuRecordAverageMilliseconds=m_cpuStageSums[2]/m_frameCount;
		snapshot.cpuSubmitAverageMilliseconds=m_cpuStageSums[3]/m_frameCount;
		snapshot.cpuPresentAverageMilliseconds=m_cpuStageSums[4]/m_frameCount;
		snapshot.cpuReadbackAverageMilliseconds=m_cpuStageSums[5]/m_frameCount;
		snapshot.hasGpuFrame=m_gpuFrameCount>0;
		snapshot.gpuFrameSampleCount=m_gpuFrameCount;
		if(snapshot.hasGpuFrame) snapshot.gpuFrameAverageMilliseconds=m_gpuFrameSumMilliseconds/m_gpuFrameCount;
		snapshot.gpuPasses=m_gpuPassSums;
		for(auto& pass:snapshot.gpuPasses) pass.averageMilliseconds/=pass.sampleCount;
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

	void ProfilerSampler::AddGpuSample(double frameMilliseconds,const std::vector<ProfilerPassTiming>& passes)
	{
		if(std::isfinite(frameMilliseconds) && frameMilliseconds>=0.0)
		{
			m_gpuFrameSumMilliseconds+=frameMilliseconds;
			++m_gpuFrameCount;
		}
		for(const auto& pass:passes)
		{
			if(!std::isfinite(pass.averageMilliseconds) || pass.averageMilliseconds<0.0) continue;
			auto found=std::find_if(m_gpuPassSums.begin(),m_gpuPassSums.end(),[&](const auto& entry){return entry.name==pass.name;});
			if(found==m_gpuPassSums.end()) {m_gpuPassSums.push_back({pass.name,0.0,0});found=m_gpuPassSums.end()-1;}
			found->averageMilliseconds+=pass.averageMilliseconds;
			++found->sampleCount;
			if(pass.name=="MainForward") {m_gpuMainSumMilliseconds+=pass.averageMilliseconds;++m_gpuMainCount;}
			if(pass.name=="Shadow") {m_gpuShadowSumMilliseconds+=pass.averageMilliseconds;++m_gpuShadowCount;}
		}
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
		m_cpuStageSums={};
		m_gpuFrameSumMilliseconds=0.0;
		m_gpuFrameCount=0;
		m_gpuPassSums.clear();
	}
}
