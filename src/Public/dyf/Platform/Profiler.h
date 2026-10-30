#pragma once

#include <cstdint>
#include <array>
#include <string>
#include <vector>
#include <memory>

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
		// Resource gauges are sampled at the same publication boundary.
		std::array<std::uint64_t,3> liveResources={};
	};

	class Profiler final
	{
	public:
		explicit Profiler(
			double publishIntervalMilliseconds = 200.0,
			double pauseThresholdMilliseconds = 1000.0);

		~Profiler();
		Profiler(const Profiler&) = delete;
		Profiler& operator=(const Profiler&) = delete;

		[[nodiscard]] bool AddSample(const ProfilerTimingSample& sample);
		[[nodiscard]] const ProfilerTimingSnapshot& GetSnapshot() const noexcept;
		// Called once per completed GPU frame, independently of the CPU publication cadence.
		void AddGpuSample(double frameMilliseconds, const std::vector<ProfilerPassTiming>& passes);
		void Reset();

	private:
		struct Impl;
		std::unique_ptr<Impl> impl;
	};
	// One clock for renderer timing, CPU scopes, RHI events and snapshot correlation.
	std::uint64_t NowNs() noexcept;

	inline double Milliseconds(std::uint64_t begin, std::uint64_t end) noexcept
	{
		return end >= begin ? static_cast<double>(end - begin) / 1000000.0 : 0.0;
	}

	class CpuTimer final
	{
	public:
		CpuTimer() noexcept : begin(NowNs()) {}
		double ElapsedMilliseconds() const noexcept { return Milliseconds(begin, NowNs()); }
		std::uint64_t StartedAt() const noexcept { return begin; }
		void Reset() noexcept { begin = NowNs(); }

	private:
		std::uint64_t begin;
	};

	struct SourceLocation
	{
		const char* name;
		const char* function;
		const char* file;
		std::uint32_t line;
	};

	// An optional adapter observes existing measurements. Its callbacks and context
	// must remain alive until all instrumented work has stopped and it is detached.
	struct TraceSink
	{
		std::uint64_t (*beginZone)(const SourceLocation&) noexcept = nullptr;
		void (*endZone)(std::uint64_t) noexcept = nullptr;
		void (*frameMark)() noexcept = nullptr;
		void (*value)(const char*, double, bool) noexcept = nullptr;
		void (*frameSample)(const ProfilerTimingSample&) noexcept = nullptr;
		void (*snapshot)(const ProfilerTimingSnapshot&) noexcept = nullptr;
	};

	const TraceSink* SetTraceSink(const TraceSink*) noexcept;
	void RemoveTraceSink(const TraceSink*) noexcept;

	class TraceZone final
	{
	public:
		TraceZone(const SourceLocation&) noexcept;
		~TraceZone();
		TraceZone(const TraceZone&) = delete;
		TraceZone& operator=(const TraceZone&) = delete;

	private:
		const TraceSink* sink = nullptr;
		std::uint64_t token = 0;
	};

	void MarkFrame() noexcept;
	void PublishValue(const char*, double, bool raw = false) noexcept;
	void PublishFrameSample(const ProfilerTimingSample&) noexcept;
	void PublishSnapshot(const ProfilerTimingSnapshot&) noexcept;
}

#define DY_PROFILE_JOIN_IMPL(a, b) a##b
#define DY_PROFILE_JOIN(a, b) DY_PROFILE_JOIN_IMPL(a, b)

// These labels and arguments are emitted only for an attached trace adapter. Internal timers
// and live CPU scopes remain available independently of every external tool option.
#ifdef DY_PROFILER_TRACING_ENABLED
#define DY_PROFILE_CPU_ZONE() DY_PROFILE_CPU_ZONE_NAMED(__func__)
#define DY_PROFILE_CPU_ZONE_NAMED(name) \
	::dyf::Platform::TraceZone DY_PROFILE_JOIN(dyProfileZone_, __LINE__)({ name, __func__, __FILE__, __LINE__ })
#define DY_PROFILE_FRAME_MARK() ::dyf::Platform::MarkFrame()
#define DY_PROFILE_GPU_MILLISECONDS(name, value) ::dyf::Platform::PublishValue(name, static_cast<double>(value))
#define DY_PROFILE_RESOURCE_COUNT(name, value) ::dyf::Platform::PublishValue(name, static_cast<double>(value))
#else
#define DY_PROFILE_CPU_ZONE() ((void)0)
#define DY_PROFILE_CPU_ZONE_NAMED(name) ((void)0)
#define DY_PROFILE_FRAME_MARK() ((void)0)
#define DY_PROFILE_GPU_MILLISECONDS(name, value) ((void)0)
#define DY_PROFILE_RESOURCE_COUNT(name, value) ((void)0)
#endif

#if defined(DY_PROFILER_TRACING_ENABLED)
#define DY_PROFILE_RAW_VALUE(name, value) ::dyf::Platform::PublishValue(name, static_cast<double>(value), true)
#else
#define DY_PROFILE_RAW_VALUE(name, value) ((void)0)
#endif
