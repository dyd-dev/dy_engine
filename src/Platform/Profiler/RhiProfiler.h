#pragma once
#include "CollectorInternal.h"
#include "dyf/RHI/IDevice.h"
#include "dyf/RHI/ICommandList.h"
#include "dyf/RHI/Query.h"
#include "dyf/RHI/Backend/UploadMemory.h"
#include "../../RHI/InlineConstantCoverage.h"
#include <array>
#include <atomic>
#include <deque>
#include <thread>
#include <unordered_map>

namespace dyf::RHI
{
    struct ICommandList::LiveDeferred
    {
        Detail::InlineConstantCoverage inlineConstants;
        // Opaque storage also holds lookup ownership for caller-managed workers.
        // Ordinary entries share the device cache; only borrowed image generations
        // are captured per list. Close discards unused owners promptly.
        std::shared_ptr<const IDevice::RecordingResources> resourceLookup;
        struct BorrowedResource
        {
            const void* handle=nullptr;
            std::shared_ptr<IDevice::ResourceRecord> resource;
            uint64_t generation=0;
        };
        std::vector<BorrowedResource> borrowedLookup;
        void ClearRecordingLookup() noexcept {resourceLookup.reset();borrowedLookup.clear();}
        // Always present opaque command-list storage, independent of profiling.
        // Exact hits only skip the lookup in the owning reference containers.
        // Collisions replace a hint; they never establish resource ownership.
        std::array<const void*,16> recentTrackedHandles={};
        const void*& TrackedHandleSlot(const void* handle) noexcept
        {
            const auto address=reinterpret_cast<uintptr_t>(handle);
            return recentTrackedHandles[((address>>3)^(address>>11))&(recentTrackedHandles.size()-1)];
        }
        void ClearTrackedHandles() noexcept {recentTrackedHandles={};}
        // A small exact cache avoids tree searches for the VB/IB states that
        // Bind and Draw repeatedly require between barriers.
        struct RecentState
        {
            uintptr_t resource=0;
            uint32_t mip=0,layer=0;
            ResourceState state=ResourceState::Undefined;
        };
        std::array<RecentState,8> recentStates={};
        uint32_t recentStateCount=0,recentStateNext=0;
        void ClearRecentStates() noexcept {recentStateCount=recentStateNext=0;}
        Platform::LiveCommandRef m_livePosition;
        Platform::LiveContext m_liveContext;
        Platform::LiveContext m_liveExecutionContext;
        Platform::LiveFrameToken m_liveRecordFrame;
        bool m_liveInExecution=false,m_liveSubmitted=false,m_liveTerminal=false;
        uint64_t m_liveCommandBeginNs=0;
        Platform::LiveId m_livePreviousEvent=0;
        TimestampQueryHandle m_liveReplayQuery=nullptr;
        uint32_t m_liveFirstQuery=0;
#ifdef DY_LIVE_PROFILING_ENABLED
        std::vector<Platform::LiveEvent> events;
        std::vector<Platform::LiveFlowLink> links;
        Platform::LiveEventSummary summary;
        std::unordered_map<const void*, Platform::LiveId> objectIds;
        Platform::LiveId FindObject(const void* object) noexcept
        {
            if(!object || !Platform::IsLiveCollectorEnabled()) return 0;
            const auto found = objectIds.find(object);
            if(found != objectIds.end()) return found->second;
            const auto id = Platform::FindLiveObject(object);
            if(id) try { objectIds.emplace(object, id); } catch(...) {}
            return id;
        }
#endif
    };

    struct ProfilerAccess
    {
        static bool HasRecordedWork(const ICommandList& commands) noexcept { return !commands.m_commands.empty(); }
        static Platform::LiveCommandRef Position(const ICommandList& commands) noexcept { return commands.GetLiveCommandPosition(); }
        static void RecordLink(ICommandList& commands,Platform::LiveId from,Platform::LiveId to) noexcept
        { commands.RecordLiveResourceLink(from,to); }
        static std::uint64_t Submission(const IDevice& device,FenceHandle fence) noexcept { return device.GetLiveSubmissionId(fence); }
        static void Prepare(IDevice& device,Platform::LiveFrameKey frame) noexcept { device.PrepareLiveControls(frame); }
        static void Release(IDevice& device,std::uint64_t source) noexcept { device.ReleaseLiveControls(source); }
        static std::uint64_t Source(const dyf::Renderer&) noexcept;
        // Reuse the existing private Renderer friendship without a public API.
        static bool GetCurrentNoAreaProof(const dyf::Renderer&,BufferHandle lights) noexcept;
    };

	// Process-local 63-bit hash/token; not an OS TID and not guaranteed collision-free.
	inline int64_t LiveRhiThreadToken() noexcept
	{
		return static_cast<int64_t>(std::hash<std::thread::id> { }(std::this_thread::get_id()) & INT64_MAX);
	}

	struct IDevice::LiveState
	{
		static constexpr uint32_t QuerySlots = 512;
		static constexpr size_t PendingLimit = 1024, SubmissionIdLimit = 4096;
		static constexpr uint64_t PendingBytesLimit = 64u * 1024u * 1024u;
		Platform::LiveId device = 0, gpuClock = 0, recordingSource = 0;
		Platform::LiveId backBufferSubmission = 0;
		Platform::LiveFrameToken frame;
		bool hasFrame = false;
		struct UploadControls
		{
			Platform::LiveId owner = 0;
			std::array<Platform::ControlRegistration, 2> controls;
			Platform::LiveFrameKey frame;
			bool reusePages = true, suballocate = true, applying = false;
		};
		std::unordered_map<Platform::LiveId, UploadControls> uploadControls;
		struct GraphControls
		{
			Platform::LiveId owner = 0;
			Platform::ControlRegistration control;
			Platform::LiveFrameKey frame;
			bool parallel = true, applying = false;
		};
		std::unordered_map<Platform::LiveId, GraphControls> graphControls;
		bool uploadPolicyInitialized = false, uploadPolicySupported = false;
		bool uploadPageReuse = true, uploadSuballocation = true;
		TimestampQueryHandle query = nullptr;
		std::array<bool, QuerySlots> reserved { };
		std::array<uint64_t, QuerySlots> untrackedCompletions { };
		struct Prepared
		{
			uint32_t slot = UINT32_MAX;
		};
		struct Sample
		{
			Platform::LiveGpuSpan span;
			uint32_t slot = UINT32_MAX;
		};
		struct Submission
		{
			uint64_t completion = 0, submittedNs = 0, bytes = 0;
			std::vector<Platform::LiveEvent> lists;
			std::vector<Sample> samples;
			std::optional<TimestampCalibration> calibration;
		};
		std::unordered_map<ICommandList *, Prepared> prepared;
		struct SubmissionId
		{
			Platform::LiveId id = 0;
			uint64_t completedNs = 0;
		};
		std::unordered_map<uint64_t, SubmissionId> submissions;
		std::deque<Submission> pending;
		uint64_t pendingBytes = 0;
		uint64_t lastGpuBegin = 0, gpuWrapBase = 0;
		bool hasGpuBegin = false;
		bool stopping=false;
	};

	struct IDevice::LiveExecutionScope
	{
		IDevice & device;
		std::vector<Platform::LiveFrameToken> frames;
		std::vector<ICommandList *> activated;
		Platform::LiveOutcome outcome = Platform::LiveOutcome::Failed;
		LiveExecutionScope(IDevice & device, ICommandList * const * lists, uint32_t count) noexcept : device(device)
		{
			device.BeginLiveExecution(lists, count, * this);
		}

		~LiveExecutionScope()
		{
			device.EndLiveExecution(* this);
		}

	};

	// A profiling token may end on a different recording worker; never restore its creator's TLS.
	inline void EndLiveRhiFrame(Platform::LiveFrameToken token, Platform::LiveOutcome outcome) noexcept
	{
		token.previous = Platform::GetLiveContext();
		Platform::EndLiveFrame(token, outcome);
	}

	inline void RecordLiveReadbackCompletion(Platform::LiveId submission, Platform::LiveId resource) noexcept
	{
#ifdef DY_LIVE_PROFILING_ENABLED
		const auto context = Platform::GetLiveContext();
		if(!submission || !context.frame.frame) return;
		// The native fence proves completion; this work has no GPU timestamp measurement.
		Platform::LiveGpuSpan span;
		span.id = Platform::AllocateLiveId();
		span.frame = context.frame;
		span.appliedRevision = context.appliedRevision;
		span.owner = context.owner;
		span.aggregation = Platform::LiveGpuAggregation::FrameCoverage;
		span.begin.submission = span.end.submission = submission;
		span.outcome = Platform::LiveOutcome::Completed;
		Platform::RecordLiveGpuSpan(span);
		Platform::RecordLiveLink({ resource, span.id });
#else
		(void) submission;
		(void) resource;
#endif
	}

	struct LiveRhiScope : Platform::CpuScope
	{
		explicit LiveRhiScope(Platform::LiveEventKind kind, Platform::LiveStage stage, const char * label, bool enabled = true) noexcept
		: Platform::CpuScope(kind, stage, label, enabled)
		{
#ifdef DY_LIVE_PROFILING_ENABLED
			if(event.beginNs && kind == Platform::LiveEventKind::NativeTranslation)
			event.arguments [4] = LiveRhiThreadToken();
#endif
		}

	};
	inline void RecordLiveRelease(const void * resource) noexcept
	{
#ifdef DY_LIVE_PROFILING_ENABLED
		if(!Platform::IsLiveCollectorEnabled()) return;
		Platform::LiveEvent event;
		event.context = Platform::GetLiveContext();
		event.resource = Platform::FindLiveObject(resource);
		event.kind = Platform::LiveEventKind::CpuWork;
		event.stage = Platform::LiveStage::Data;
		event.outcome = Platform::LiveOutcome::Pending;
		const char label [] = "Resource release requested";
		for(size_t i = 0; i < sizeof(label); ++i) event.label [i] = label [i];
		Platform::RecordLiveEvent(event);
#else
		(void) resource;
#endif
	}

}
