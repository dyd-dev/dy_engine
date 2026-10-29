#include "CollectorInternal.h"
#include "FrameProfiler.h"
#include "GpuTimestamp.h"
#include <algorithm>
#include <cassert>
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
        for(auto& scope:recordingScopes) {device.DestroyTimestampQuery(scope.query);scope.query=nullptr;}

		pending.clear();
		availableQueries.clear();
		gpuScopes.clear();
		freeScopeQueries.clear();
		scopeQueryCount = 0;
        recordingScopeCount=0;
        nextRecordingScope.store(0,std::memory_order_relaxed);
	}

	void FrameProfiler::SubmitGpuSample(GpuSample&& sample, LiveFrameKey frame)
	{
        assert((frame==LiveFrameKey{}) || (frame.device && frame.source && frame.frame));
        sample.frame=frame;
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

#ifdef DY_PROFILER_TRACING_ENABLED
				PublishValue("GPU graph span ms",milliseconds(ticks[0],ticks[1]),true);
				if(hasStart) PublishValue("GPU upload to graph ms",milliseconds(start,ticks[0]),true);
#endif
				profiler.AddGpuSample(hasStart ? milliseconds(start, ticks[1]) : -1.0, passes, sample.frame);
                if(hasStart) PublishGpuTiming({sample.frame,milliseconds(start,ticks[1]),
                    milliseconds(start,ticks[0]),milliseconds(ticks[0],ticks[1])});
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

    void FrameProfiler::PrepareGpuScopes(std::uint32_t count) noexcept
    {
        if(!IsLiveCollectorEnabled())return;
        try
        {
            CollectGpuScopes();
            count=std::min(count,static_cast<uint32_t>(recordingScopes.size()));
            gpuScopes.reserve(gpuScopes.size()+count);
            nextRecordingScope.store(0,std::memory_order_relaxed);
            recordingScopeCount=0;
            for(uint32_t index=0;index<count;++index)
            {
                RHI::TimestampQueryHandle query=nullptr;
                if(!freeScopeQueries.empty())
                {query=freeScopeQueries.back();freeScopeQueries.pop_back();}
                else if(scopeQueryCount<recordingScopes.size())
                {query=device.CreateTimestampQuery({2});if(query)++scopeQueryCount;}
                if(!query)break;
                auto& slot=recordingScopes[index];
                slot.query=query;slot.recorded=false;
                ++recordingScopeCount;
            }
        }
        catch(...) {} // Optional profiling must not prevent rendering.
    }

	FrameProfiler::GpuScope::GpuScope(FrameProfiler& owner, RHI::ICommandList& list, LiveGpuScope scope, LiveId ownerId) noexcept
		: profiler(owner), commands(list)
	{
		if(!IsLiveCollectorEnabled()) return;

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

            slot=owner.nextRecordingScope.fetch_add(1,std::memory_order_relaxed);
            if(slot<owner.recordingScopeCount)query=owner.recordingScopes[slot].query;
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
            query=nullptr; // The owner recycles the reserved slot after all callbacks join.
			span.duration.state = LiveValueState::Unavailable;
			RecordLiveGpuSpan(span);
		}
	}

	FrameProfiler::GpuScope::~GpuScope()
	{
		if(!query) return;

		try
		{
			commands.WriteTimestamp(query, 1);
			span.end.command = commands.GetLiveCommandPosition();
            auto& destination=profiler.recordingScopes[slot];
            destination.span=span;
            destination.recorded=true;
			RecordLiveGpuSpan(span);
		}
		catch(...)
		{
            // The failed command list is never submitted; slot ownership stays on the owner.
		}
	}

	void FrameProfiler::SubmitGpuScopes(RHI::FenceHandle completion, LiveFrameKey frame) noexcept
	{
		try
		{
            // Recording is joined before this owner-only handoff. Each slot has
            // exactly one writer, so worker recording needs no shared vector lock.
            for(uint32_t index=0;index<recordingScopeCount;++index)
            {
                auto& slot=recordingScopes[index];
                if(!slot.query)continue;
                if(slot.recorded)gpuScopes.push_back({slot.query,{},slot.span});
                else freeScopeQueries.push_back(slot.query);
                slot.query=nullptr;
            }
            recordingScopeCount=0;
			RHI::TimestampCalibration pair;
			const bool calibrated = completion && device.GetTimestampCalibration(pair);
			const auto now = LiveNowNs();
			for(auto it = gpuScopes.begin(); it != gpuScopes.end();)
			{
				if(it->span.frame == frame && !it->completion)
				{
					if(completion)
					{
						it->completion = completion;
						it->submittedNs = now;
						if(calibrated) it->calibration = pair;
						it->span.begin.submission = it->span.end.submission = device.GetLiveSubmissionId(completion);
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
		try
		{
			for(auto it = gpuScopes.begin(); it != gpuScopes.end();)
			{
				if(!it->completion || !device.IsComplete(it->completion))
				{
					++it;
					continue;
				}

				std::uint64_t ticks[2]{};
				auto& span = it->span;
				span.outcome = LiveOutcome::Completed;
				span.duration.state = LiveValueState::Unavailable;
				const auto bits = device.GetTimestampValidBits();
				const auto period = device.GetTimestampPeriodNanoseconds();
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
