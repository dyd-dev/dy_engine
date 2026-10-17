#pragma once

#include <cstdint>
#include <array>
#include <string>
#include <vector>

namespace dyf::Platform
{
	struct ProfilerTimingSample
	{
		double frameMilliseconds = 0.0;
		double cpuRenderMilliseconds = 0.0;
		double gpuMainMilliseconds = 0.0;
		double gpuShadowMilliseconds = 0.0;
		bool hasGpuMain = false;
		bool hasGpuShadow = false;
		// Wall-clock stages of RenderScene; acquire/readback/present may include waits.
		double cpuAcquireMilliseconds = 0.0;
		double cpuPrepareMilliseconds = 0.0;
		double cpuRecordMilliseconds = 0.0;
		double cpuSubmitMilliseconds = 0.0;
		double cpuPresentMilliseconds = 0.0;
		double cpuReadbackMilliseconds = 0.0;
	};

	struct ProfilerPassTiming
	{
		std::string name;
		double averageMilliseconds = 0.0;
		uint32_t sampleCount = 0;
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
		double cpuAcquireAverageMilliseconds = 0.0;
		double cpuPrepareAverageMilliseconds = 0.0;
		double cpuRecordAverageMilliseconds = 0.0;
		double cpuSubmitAverageMilliseconds = 0.0;
		double cpuPresentAverageMilliseconds = 0.0;
		double cpuReadbackAverageMilliseconds = 0.0;
		// GPU span: frame-data upload through the final graph barrier (including HUD).
		// Excludes initial asset uploads, acquisition, readback and presentation.
		double gpuFrameAverageMilliseconds = 0.0;
		uint32_t gpuFrameSampleCount = 0;
		uint64_t sequence = 0;
		bool hasGpuFrame = false;
		bool gpuTimingSupported = false;
		// Only completed samples appear. Absent/disabled/cached passes are not zero-ms samples.
		std::vector<ProfilerPassTiming> gpuPasses;
	};

	class ProfilerSampler final
	{
	public:
		explicit ProfilerSampler(
			double publishIntervalMilliseconds = 200.0,
			double pauseThresholdMilliseconds = 1000.0);

		[[nodiscard]] bool AddSample(const ProfilerTimingSample& sample, ProfilerTimingSnapshot& snapshot);
		// Called once per completed GPU frame, independently of the CPU publication cadence.
		void AddGpuSample(double frameMilliseconds, const std::vector<ProfilerPassTiming>& passes);
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
		std::array<double,6> m_cpuStageSums = {};
		double m_gpuFrameSumMilliseconds = 0.0;
		uint32_t m_gpuFrameCount = 0;
		std::vector<ProfilerPassTiming> m_gpuPassSums;
		uint64_t m_sequence = 0;
	};
}
