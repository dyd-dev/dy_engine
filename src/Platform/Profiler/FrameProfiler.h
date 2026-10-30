#pragma once
#include "CollectorInternal.h"

#include "dyf/Platform/Profiler.h"
#include "dyf/RHI/IDevice.h"
#include "dyf/RHI/ICommandList.h"
#include <deque>
#include <array>
#include <atomic>
#include <optional>
#include <utility>

namespace dyf::Platform
{
	// Owns frame sampling and completed GPU measurements. Renderer only supplies
	// stage boundaries and consumes the published snapshot for presentation.
	class FrameProfiler final
	{
	public:
		explicit FrameProfiler(RHI::IDevice&);
		~FrameProfiler();
		FrameProfiler(const FrameProfiler&) = delete;
		FrameProfiler& operator=(const FrameProfiler&) = delete;

		struct GpuSample
		{
			RHI::TimestampQueryHandle query = nullptr, start = nullptr;
			RHI::FenceHandle completion;
			std::vector<std::string> passes;
            LiveFrameKey frame;
		};

		RHI::TimestampQueryHandle AcquireQuery(std::uint32_t count);
		void SubmitGpuSample(GpuSample&&, LiveFrameKey frame={});
		void PollGpuSamples();
		bool RecordFrame(ProfilerTimingSample);
		const ProfilerTimingSnapshot& GetSnapshot() const noexcept { return snapshot; }
		void AddAcquireWait(double milliseconds) noexcept { pendingAcquireMilliseconds += milliseconds; }
		double TakeAcquireWait() noexcept;
		void ResetGpuQueries() noexcept;
        // Owner-only boundary before graph callbacks; workers borrow unique slots.
        void PrepareGpuScopes(std::uint32_t count) noexcept;
		std::size_t PendingGpuSamples() const noexcept { return pending.size(); }

		class GpuScope final
		{
		public:
			GpuScope(FrameProfiler&, RHI::ICommandList&, LiveGpuScope, LiveId owner) noexcept;
			~GpuScope();
			GpuScope(const GpuScope&) = delete;
			GpuScope& operator=(const GpuScope&) = delete;

		private:
			FrameProfiler& profiler;
			RHI::ICommandList& commands;
			RHI::TimestampQueryHandle query = nullptr;
			LiveGpuSpan span;
            std::uint32_t slot=UINT32_MAX;
		};

		void SubmitGpuScopes(RHI::FenceHandle, LiveFrameKey) noexcept;
		void CollectGpuScopes() noexcept;

	private:
		RHI::IDevice& device;
		Profiler profiler;
		ProfilerTimingSnapshot snapshot;
		std::uint64_t previous = 0;
		double pendingAcquireMilliseconds = 0;
		std::deque<GpuSample> pending;
		std::vector<std::pair<RHI::TimestampQueryHandle, std::uint32_t>> availableQueries;

		struct GpuScopeSample
		{
			RHI::TimestampQueryHandle query = nullptr;
			RHI::FenceHandle completion;
			LiveGpuSpan span;
			std::optional<RHI::TimestampCalibration> calibration;
			std::uint64_t submittedNs = 0;
		};

		std::vector<GpuScopeSample> gpuScopes;
		std::vector<RHI::TimestampQueryHandle> freeScopeQueries;
		std::uint32_t scopeQueryCount = 0;
        struct RecordingGpuScope
        {
            RHI::TimestampQueryHandle query=nullptr;
            LiveGpuSpan span;
            bool recorded=false;
        };
        std::array<RecordingGpuScope,16> recordingScopes;
        std::uint32_t recordingScopeCount=0;
        std::atomic<std::uint32_t> nextRecordingScope{0};
	};
}
