#pragma once

#include <cstdint>

namespace dy::Graphics
{
	struct ProfilerTimingSample
	{
		double frameMilliseconds = 0.0;
		double cpuRenderMilliseconds = 0.0;
		double gpuMainMilliseconds = 0.0;
		double gpuShadowMilliseconds = 0.0;
		bool hasGpuMain = false;
		bool hasGpuShadow = false;
	};

	struct ProfilerTimingSnapshot
	{
		double fps = 0.0;
		double frameAverageMilliseconds = 0.0;
		double frameMaximumMilliseconds = 0.0;
		double cpuRenderAverageMilliseconds = 0.0;
		double gpuMainAverageMilliseconds = 0.0;
		double gpuShadowAverageMilliseconds = 0.0;
		double sampleWindowMilliseconds = 0.0;
		uint32_t frameCount = 0;
		bool hasGpuMain = false;
		bool hasGpuShadow = false;
	};

	class ProfilerSampler final
	{
	public:
		explicit ProfilerSampler(
			double publishIntervalMilliseconds = 200.0,
			double pauseThresholdMilliseconds = 1000.0);

		[[nodiscard]] bool AddSample(const ProfilerTimingSample& sample, ProfilerTimingSnapshot& snapshot);
		void Reset();

	private:
		double m_publishIntervalMilliseconds = 200.0;
		double m_pauseThresholdMilliseconds = 1000.0;
		double m_frameSumMilliseconds = 0.0;
		double m_frameMaximumMilliseconds = 0.0;
		double m_cpuSumMilliseconds = 0.0;
		double m_gpuMainSumMilliseconds = 0.0;
		double m_gpuShadowSumMilliseconds = 0.0;
		uint32_t m_frameCount = 0;
		uint32_t m_gpuMainCount = 0;
		uint32_t m_gpuShadowCount = 0;
	};
}
