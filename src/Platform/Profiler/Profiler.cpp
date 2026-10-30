#include "dyf/Platform/Profiler.h"
#include "CollectorInternal.h"
#include <algorithm>
#include <cmath>
#include <atomic>
#include <cstring>

namespace dyf::Platform
{
	std::uint64_t NowNs() noexcept { return LiveNowNs(); }

	namespace
	{
		std::atomic<const TraceSink*> traceSink{ nullptr };
	}

	const TraceSink* SetTraceSink(const TraceSink* sink) noexcept
	{
		return traceSink.exchange(sink);
	}

	void RemoveTraceSink(const TraceSink* sink) noexcept
	{
		traceSink.compare_exchange_strong(sink, nullptr);
	}

	TraceZone::TraceZone(const SourceLocation& location) noexcept
	{
#ifdef DY_PROFILER_TRACING_ENABLED
		sink = traceSink.load();
		if(sink && sink->beginZone) token = sink->beginZone(location);
#else
		(void)location;
#endif
	}

	TraceZone::~TraceZone()
	{
#ifdef DY_PROFILER_TRACING_ENABLED
		if(sink && sink->endZone) sink->endZone(token);
#endif
	}

	CpuScope::CpuScope(LiveStage stage, const char* label) noexcept
		: CpuScope(LiveEventKind::CpuWork, stage, label) {}

	CpuScope::CpuScope(LiveEventKind kind, LiveStage stage, const char* label, bool enabled) noexcept
	{
		if(!enabled) return;

#ifdef DY_LIVE_PROFILING_ENABLED
		if(IsLiveCollectorEnabled())
		{
			event.id = IsLiveDetailEnabled() ? AllocateLiveId() : 0;
			event.context = GetLiveContext();
			event.kind = kind;
			event.stage = stage;
			event.beginNs = NowNs();
			if(event.id && label) std::strncpy(event.label, label, sizeof(event.label) - 1);
		}
#else
		(void)kind;
		(void)stage;
#endif

#ifdef DY_PROFILER_TRACING_ENABLED
		sink = traceSink.load();
		if(sink && sink->beginZone) token = sink->beginZone({ label ? label : "CPU scope", "", "", 0 });
#else
		(void)label;
#endif
	}

	CpuScope::~CpuScope() { Finish(); }

	void CpuScope::Finish() noexcept
	{
#ifdef DY_LIVE_PROFILING_ENABLED
		if(event.beginNs)
		{
			event.endNs = NowNs();
			RecordLiveEvent(event);
			event.beginNs = 0;
		}
#endif
#ifdef DY_PROFILER_TRACING_ENABLED
		if(sink && sink->endZone) sink->endZone(token);
		sink = nullptr;
		token = 0;
#endif
	}

	void MarkFrame() noexcept
	{
#ifdef DY_PROFILER_TRACING_ENABLED
		if(const auto* sink = traceSink.load(); sink && sink->frameMark) sink->frameMark();
#endif
	}

	void PublishValue(const char* name, double value, bool raw) noexcept
	{
#ifdef DY_PROFILER_TRACING_ENABLED
		if(const auto* sink = traceSink.load(); sink && sink->value) sink->value(name, value, raw);
#else
		(void)name;
		(void)value;
		(void)raw;
#endif
	}

	void PublishFrameSample(const ProfilerTimingSample& sample) noexcept
	{
#ifdef DY_PROFILER_TRACING_ENABLED
		if(const auto* sink = traceSink.load(); sink && sink->frameSample) sink->frameSample(sample);
#else
		(void)sample;
#endif
	}

	void PublishSnapshot(const ProfilerTimingSnapshot& sample) noexcept
	{
#ifdef DY_PROFILER_TRACING_ENABLED
		if(const auto* sink = traceSink.load(); sink && sink->snapshot) sink->snapshot(sample);
#else
		(void)sample;
#endif
	}
}



namespace dyf::Platform
{
	struct Profiler::Impl
	{
		ProfilerTimingSnapshot m_snapshot;
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

	Profiler::Profiler(double publishIntervalMilliseconds, double pauseThresholdMilliseconds)
        : impl(std::make_unique<Impl>())
    {
        impl->m_publishIntervalMilliseconds = std::max(publishIntervalMilliseconds, 1.0);
        impl->m_pauseThresholdMilliseconds = std::max(pauseThresholdMilliseconds, impl->m_publishIntervalMilliseconds);
    }

    Profiler::~Profiler() = default;
    const ProfilerTimingSnapshot& Profiler::GetSnapshot() const noexcept { return impl->m_snapshot; }

	bool Profiler::AddSample(const ProfilerTimingSample& sample)
	{
		auto& snapshot = impl->m_snapshot;
		if(!std::isfinite(sample.frameMilliseconds) || sample.frameMilliseconds <= 0.0)
			return false;
		if(sample.frameMilliseconds >= impl->m_pauseThresholdMilliseconds)
		{
			Reset();
			return false;
		}

		impl->m_frameSumMilliseconds += sample.frameMilliseconds;
		impl->m_frameMaximumMilliseconds = std::max(impl->m_frameMaximumMilliseconds, sample.frameMilliseconds);
		impl->m_cpuSumMilliseconds += std::isfinite(sample.cpuRenderMilliseconds)
			? std::max(sample.cpuRenderMilliseconds, 0.0) : 0.0;
		++impl->m_frameCount;

		const std::array<double, 6> stages = {
			sample.cpuAcquireMilliseconds, sample.cpuPrepareMilliseconds,
			sample.cpuRecordMilliseconds, sample.cpuSubmitMilliseconds,
			sample.cpuPresentMilliseconds, sample.cpuReadbackMilliseconds
		};
		for(std::size_t i = 0; i < stages.size(); ++i)
		{
			if(std::isfinite(stages[i]) && stages[i] >= 0.0)
				impl->m_cpuStageSums[i] += stages[i];
		}
		if(sample.hasGpuMain && std::isfinite(sample.gpuMainMilliseconds) && sample.gpuMainMilliseconds >= 0.0)
		{
			impl->m_gpuMainSumMilliseconds += sample.gpuMainMilliseconds;
			++impl->m_gpuMainCount;
		}
		if(sample.hasGpuShadow && std::isfinite(sample.gpuShadowMilliseconds) && sample.gpuShadowMilliseconds >= 0.0)
		{
			impl->m_gpuShadowSumMilliseconds += sample.gpuShadowMilliseconds;
			++impl->m_gpuShadowCount;
		}
		if(impl->m_frameSumMilliseconds + 0.1 < impl->m_publishIntervalMilliseconds)
			return false;

		snapshot = {};
		snapshot.sequence = ++impl->m_sequence;
		snapshot.sampleWindowMilliseconds = impl->m_frameSumMilliseconds;
		snapshot.frameCount = impl->m_frameCount;
		snapshot.fps = static_cast<double>(impl->m_frameCount) * 1000.0 / impl->m_frameSumMilliseconds;
		snapshot.frameAverageMilliseconds = impl->m_frameSumMilliseconds / impl->m_frameCount;
		snapshot.frameMaximumMilliseconds = impl->m_frameMaximumMilliseconds;
		snapshot.cpuRenderAverageMilliseconds = impl->m_cpuSumMilliseconds / impl->m_frameCount;
		snapshot.cpuAcquireAverageMilliseconds = impl->m_cpuStageSums[0] / impl->m_frameCount;
		snapshot.cpuPrepareAverageMilliseconds = impl->m_cpuStageSums[1] / impl->m_frameCount;
		snapshot.cpuRecordAverageMilliseconds = impl->m_cpuStageSums[2] / impl->m_frameCount;
		snapshot.cpuSubmitAverageMilliseconds = impl->m_cpuStageSums[3] / impl->m_frameCount;
		snapshot.cpuPresentAverageMilliseconds = impl->m_cpuStageSums[4] / impl->m_frameCount;
		snapshot.cpuReadbackAverageMilliseconds = impl->m_cpuStageSums[5] / impl->m_frameCount;
		snapshot.hasGpuFrame = impl->m_gpuFrameCount > 0;
		snapshot.gpuFrameSampleCount = impl->m_gpuFrameCount;
		if(snapshot.hasGpuFrame)
			snapshot.gpuFrameAverageMilliseconds = impl->m_gpuFrameSumMilliseconds / impl->m_gpuFrameCount;
		snapshot.gpuPasses = impl->m_gpuPassSums;
		for(auto& pass : snapshot.gpuPasses)
			pass.averageMilliseconds /= pass.sampleCount;
		snapshot.hasGpuMain = impl->m_gpuMainCount > 0;
		snapshot.hasGpuShadow = impl->m_gpuShadowCount > 0;
		if(snapshot.hasGpuMain)
			snapshot.gpuMainAverageMilliseconds = impl->m_gpuMainSumMilliseconds / impl->m_gpuMainCount;
		if(snapshot.hasGpuShadow)
			snapshot.gpuShadowAverageMilliseconds = impl->m_gpuShadowSumMilliseconds / impl->m_gpuShadowCount;
		Reset();
		return true;
	}

	void Profiler::AddGpuSample(double frameMilliseconds, const std::vector<ProfilerPassTiming>& passes)
	{
		if(std::isfinite(frameMilliseconds) && frameMilliseconds >= 0.0)
		{
			impl->m_gpuFrameSumMilliseconds += frameMilliseconds;
			++impl->m_gpuFrameCount;
		}
		for(const auto& pass : passes)
		{
			if(!std::isfinite(pass.averageMilliseconds) || pass.averageMilliseconds < 0.0)
				continue;
			auto found = std::find_if(impl->m_gpuPassSums.begin(), impl->m_gpuPassSums.end(),
				[&](const auto& entry) { return entry.name == pass.name; });
			if(found == impl->m_gpuPassSums.end())
			{
				impl->m_gpuPassSums.push_back({pass.name, 0.0, 0});
				found = impl->m_gpuPassSums.end() - 1;
			}
			found->averageMilliseconds += pass.averageMilliseconds;
			++found->sampleCount;
			if(pass.name == "MainForward")
			{
				impl->m_gpuMainSumMilliseconds += pass.averageMilliseconds;
				++impl->m_gpuMainCount;
			}
			if(pass.name == "Shadow")
			{
				impl->m_gpuShadowSumMilliseconds += pass.averageMilliseconds;
				++impl->m_gpuShadowCount;
			}
		}
	}

	void Profiler::Reset()
	{
		impl->m_frameSumMilliseconds = 0.0;
		impl->m_frameMaximumMilliseconds = 0.0;
		impl->m_cpuSumMilliseconds = 0.0;
		impl->m_gpuMainSumMilliseconds = 0.0;
		impl->m_gpuShadowSumMilliseconds = 0.0;
		impl->m_frameCount = 0;
		impl->m_gpuMainCount = 0;
		impl->m_gpuShadowCount = 0;
		impl->m_cpuStageSums = {};
		impl->m_gpuFrameSumMilliseconds = 0.0;
		impl->m_gpuFrameCount = 0;
		impl->m_gpuPassSums.clear();
	}
}
