#include "CollectorInternal.h"
#include "FrameProfiler.h"
#include "GpuTimestamp.h"
#include <cmath>
#include <utility>

namespace dyf::Platform
{
	FrameProfiler::FrameProfiler(RHI::IDevice& owner) : device(owner)
	{
#ifdef DY_LIVE_PROFILING_ENABLED
		gpuScopes.reserve(16);
		freeScopeQueries.reserve(16);
#endif
	}

	FrameProfiler::~FrameProfiler() { ResetGpuQueries(); }

	void FrameProfiler::ResetGpuQueries() noexcept
	{
		for(const auto& sample : pending)
		{
			device.DestroyTimestampQuery(sample.query);
			device.DestroyTimestampQuery(sample.start);
		}
		for(const auto& query : availableQueries) device.DestroyTimestampQuery(query.first);
		for(const auto& sample : gpuScopes) device.DestroyTimestampQuery(sample.query);
		for(const auto query : freeScopeQueries) device.DestroyTimestampQuery(query);

		pending.clear();
		availableQueries.clear();
		gpuScopes.clear();
		freeScopeQueries.clear();
		scopeQueryCount = 0;
	}

	void FrameProfiler::SubmitGpuSample(GpuSample&& sample)
	{
		pending.push_back(std::move(sample));
	}

	double FrameProfiler::TakeAcquireWait() noexcept
	{
		const double result = pendingAcquireMilliseconds;
		pendingAcquireMilliseconds = 0;
		return result;
	}

	bool FrameProfiler::RecordFrame(ProfilerTimingSample sample)
	{
		const auto now = NowNs();
		sample.frameMilliseconds = previous ? Milliseconds(previous, now) : 0;
		previous = now;
#ifdef DY_PROFILER_TRACING_ENABLED
		PublishFrameSample(sample);
#endif
		if(!profiler.AddSample(sample)) return false;
		snapshot = profiler.GetSnapshot();

		snapshot.gpuTimingSupported = device.Supports(RHI::Feature::TimestampQuery) &&
			device.GetTimestampValidBits() > 0 && device.GetTimestampPeriodNanoseconds() > 0;
		const auto counters = device.GetResourceAllocationCounters();
		snapshot.liveResources = { counters.buffers.live, counters.textures.live, counters.pipelines.live };
#ifdef DY_PROFILER_TRACING_ENABLED
		PublishSnapshot(snapshot);
#endif
		return true;
	}

	RHI::TimestampQueryHandle FrameProfiler::AcquireQuery(uint32_t count)
	{
		const auto bits = device.GetTimestampValidBits();
		const auto period = device.GetTimestampPeriodNanoseconds();
		if(!device.Supports(RHI::Feature::TimestampQuery) || !bits || bits > 64 || !std::isfinite(period) || period <= 0 ||
			pending.size() > device.GetDesc().maxFramesInFlight + 1)
			return nullptr;

		for(auto it = availableQueries.begin(); it != availableQueries.end(); ++it)
		{
			if(it->second == count)
			{
				auto* query = it->first;
				availableQueries.erase(it);
				return query;
			}
		}
		return device.CreateTimestampQuery({ count });
	}

	void FrameProfiler::PollGpuSamples()
	{
		while(!pending.empty() && pending.front().completion && device.IsComplete(pending.front().completion))
		{
			struct Completed
			{
				RHI::IDevice& device;
				GpuSample sample;

				~Completed()
				{
					device.DestroyTimestampQuery(sample.query);
					device.DestroyTimestampQuery(sample.start);
				}
			} completed{ device, std::move(pending.front()) };

			pending.pop_front();
			auto& sample = completed.sample;
			const auto count = 2 + 2 * static_cast<uint32_t>(sample.passes.size());
			std::vector<uint64_t> ticks(count);
			uint64_t start = 0;
			const bool valid = device.ReadTimestamps(sample.query, 0, count, ticks.data());
			const bool hasStart = sample.start && device.ReadTimestamps(sample.start, 0, 1, &start);
			const auto bits = device.GetTimestampValidBits();
			const auto mask = bits == 64 ? UINT64_MAX : bits > 0 && bits < 64 ? (uint64_t{ 1 } << bits) - 1 : 0;
			const auto period = device.GetTimestampPeriodNanoseconds();
			if(valid && mask && std::isfinite(period) && period > 0)
			{
				const auto milliseconds = [&](uint64_t begin, uint64_t end)
				{
					return ((end - begin) & mask) * period / 1e6;
				};
				std::vector<ProfilerPassTiming> passes;
				passes.reserve(sample.passes.size());
				for(size_t i = 0; i < sample.passes.size(); ++i)
					passes.push_back({ sample.passes[i], milliseconds(ticks[2 + 2 * i], ticks[3 + 2 * i]), 1 });

				profiler.AddGpuSample(hasStart ? milliseconds(start, ticks[1]) : -1.0, passes);
			}

			const auto recycle = [&](RHI::TimestampQueryHandle& query, uint32_t slots)
			{
				if(!query) return;
				if(availableQueries.size() < 16) availableQueries.push_back({ query, slots });
				else device.DestroyTimestampQuery(query);
				query = nullptr;
			};
			recycle(sample.query, count);
			recycle(sample.start, 1);
		}
	}

	FrameProfiler::GpuScope::GpuScope(FrameProfiler& owner, RHI::ICommandList& list, LiveGpuScope scope, LiveId ownerId) noexcept
		: profiler(owner), commands(list)
	{
		if(!IsLiveCollectorEnabled()) return;

		std::lock_guard<std::mutex> lock(owner.gpuMutex);
		try
		{
			const auto context = GetLiveContext();
			span.id = AllocateLiveId();
			span.frame = context.frame;
			span.appliedRevision = context.appliedRevision;
			span.owner = ownerId;
			span.scope = scope;
			if(!owner.device.Supports(RHI::Feature::TimestampQuery))
			{
				span.duration.state = LiveValueState::Unsupported;
				RecordLiveGpuSpan(span);
				return;
			}

			if(!owner.freeScopeQueries.empty())
			{
				query = owner.freeScopeQueries.back();
				owner.freeScopeQueries.pop_back();
			}
			else if(owner.scopeQueryCount < 16)
			{
				query = owner.device.CreateTimestampQuery({ 2 });
				if(query) ++owner.scopeQueryCount;
			}
			if(!query)
			{
				RecordLiveGpuSpan(span);
				return;
			}

			commands.ResetTimestamps(query, 0, 2);
			commands.WriteTimestamp(query, 0);
			span.begin.command = commands.GetLiveCommandPosition();
			span.duration.state = LiveValueState::Pending;
		}
		catch(...)
		{
			if(query)
			{
				owner.device.DestroyTimestampQuery(query);
				--owner.scopeQueryCount;
				query = nullptr;
			}
			span.duration.state = LiveValueState::Unavailable;
			RecordLiveGpuSpan(span);
		}
	}

	FrameProfiler::GpuScope::~GpuScope()
	{
		if(!query) return;

		std::lock_guard<std::mutex> lock(profiler.gpuMutex);
		try
		{
			commands.WriteTimestamp(query, 1);
			span.end.command = commands.GetLiveCommandPosition();
			profiler.gpuScopes.push_back({ query, {}, span });
			RecordLiveGpuSpan(span);
		}
		catch(...)
		{
			profiler.device.DestroyTimestampQuery(query);
			--profiler.scopeQueryCount;
		}
	}

	void FrameProfiler::SubmitGpuScopes(RHI::FenceHandle completion, LiveFrameKey frame) noexcept
	{
		std::lock_guard<std::mutex> lock(gpuMutex);
		try
		{
			RHI::TimestampCalibration pair;
			bool initialized = false, calibrated = false;
			std::uint64_t now = 0;
			LiveId submission = 0;
			for(auto it = gpuScopes.begin(); it != gpuScopes.end();)
			{
				if(it->span.frame == frame && !it->completion)
				{
					if(completion)
					{
						if(!initialized)
						{
							calibrated = device.GetTimestampCalibration(pair);
							now = LiveNowNs();
							submission = device.GetLiveSubmissionId(completion);
							initialized = true;
						}
						it->completion = completion;
						it->submittedNs = now;
						if(calibrated) it->calibration = pair;
						it->span.begin.submission = it->span.end.submission = submission;
						it->span.outcome = LiveOutcome::Submitted;
						RecordLiveGpuSpan(it->span);
						++it;
					}
					else
					{
						it->span.outcome = LiveOutcome::Discarded;
						it->span.duration.state = LiveValueState::Unavailable;
						RecordLiveGpuSpan(it->span);
						device.DestroyTimestampQuery(it->query);
						--scopeQueryCount;
						it = gpuScopes.erase(it);
					}
				}
				else ++it;
			}
		}
		catch(...) {}
	}

	void FrameProfiler::CollectGpuScopes() noexcept
	{
		std::lock_guard<std::mutex> lock(gpuMutex);
		if(gpuScopes.empty()) return;
		try
		{
			RHI::FenceHandle completed;
			const auto bits = device.GetTimestampValidBits();
			const auto period = device.GetTimestampPeriodNanoseconds();
			for(auto it = gpuScopes.begin(); it != gpuScopes.end();)
			{
				if(!it->completion || (it->completion != completed && !device.IsComplete(it->completion)))
				{
					++it;
					continue;
				}
				completed = it->completion;

				std::uint64_t ticks[2]{};
				auto& span = it->span;
				span.outcome = LiveOutcome::Completed;
				span.duration.state = LiveValueState::Unavailable;
				if(bits && bits <= 64 && period > 0 && device.ReadTimestamps(it->query, 0, 2, ticks))
				{
					const auto mask = bits == 64 ? UINT64_MAX : (std::uint64_t(1) << bits) - 1;
					span.duration = { LiveValueState::Ready, double((ticks[1] - ticks[0]) & mask) * period / 1e6 };
				}
				if(span.duration.state == LiveValueState::Ready && it->calibration)
					Detail::CalibrateGpuSpan(span, *it->calibration, ticks[0], ticks[1], FindLiveObject(&device), it->submittedNs, LiveNowNs());

				RecordLiveGpuSpan(span);
				freeScopeQueries.push_back(it->query);
				it = gpuScopes.erase(it);
			}
		}
		catch(...) {}
	}
}
