#include "dyf/Platform/Profiler.h"
#include "dyf/Extends/Tracy.h"

#ifdef DY_TRACY_ENABLED
#include <tracy/TracyC.h>
#include <cstring>

namespace dyf
{
	namespace
	{
		std::uint64_t BeginZone(const Platform::SourceLocation& location) noexcept
		{
			if(!TracyCIsConnected) return 0;

			const auto* name = location.name ? location.name : "CPU scope";
			const auto source = ___tracy_alloc_srcloc_name(location.line, location.file, std::strlen(location.file),
				location.function, std::strlen(location.function), name, std::strlen(name), 0);
			const auto zone = ___tracy_emit_zone_begin_alloc(source, 1);
			return (std::uint64_t(zone.active != 0) << 32) | zone.id;
		}

		void EndZone(std::uint64_t token) noexcept
		{
			if(token >> 32) ___tracy_emit_zone_end({ std::uint32_t(token), 1 });
		}

		void FrameMark() noexcept { ___tracy_emit_frame_mark(nullptr); }

		void SendValue(const char* name, double value, bool raw) noexcept
		{
			(void)raw;
			___tracy_emit_plot(name, value);
		}

		void SendFrameSample(const Platform::ProfilerTimingSample& sample) noexcept
		{
			SendValue("Frame.Raw.ms", sample.frameMilliseconds, true);
			SendValue("CPU.Render.Raw.ms", sample.cpuRenderMilliseconds, true);
		}

		void SendSnapshot(const Platform::ProfilerTimingSnapshot& sample) noexcept
		{
			SendValue("Frame.Average.ms", sample.frameAverageMilliseconds, false);
			SendValue("Frame.Maximum.ms", sample.frameMaximumMilliseconds, false);
			SendValue("CPU.Render.Average.ms", sample.cpuRenderAverageMilliseconds, false);
			if(sample.hasGpuFrame) SendValue("GPU.Frame.Average.ms", sample.gpuFrameAverageMilliseconds, false);
			if(sample.hasGpuMain) SendValue("GPU.MainForward.Average.ms", sample.gpuMainAverageMilliseconds, false);
			if(sample.hasGpuShadow) SendValue("GPU.Shadow.Average.ms", sample.gpuShadowAverageMilliseconds, false);
			SendValue("GPU.Resources.Buffers.Live", static_cast<double>(sample.liveResources[0]), false);
			SendValue("GPU.Resources.Textures.Live", static_cast<double>(sample.liveResources[1]), false);
			SendValue("GPU.Resources.Pipelines.Live", static_cast<double>(sample.liveResources[2]), false);
		}
	}

	const Platform::TraceSink& GetTracySink() noexcept
	{
		static const Platform::TraceSink sink = {
			BeginZone, EndZone, FrameMark, SendValue, SendFrameSample, SendSnapshot
		};
		return sink;
	}
}
#endif

namespace dyf
{
	TracyAdapter::TracyAdapter() noexcept
	{
#ifdef DY_TRACY_ENABLED
		sink = &GetTracySink();
		Platform::SetTraceSink(sink);
#endif
	}

	TracyAdapter::~TracyAdapter()
	{
#ifdef DY_TRACY_ENABLED
		Platform::RemoveTraceSink(sink);
#endif
	}
}
