#include "dyf/Diagnostics/Profiler.h"
#include "dyf/Platform/ProfilerSampler.h"
#include "dyf/RHI/IDevice.h"

#if defined(DY_TRACY_ENABLED)
#include <tracy/TracyC.h>
#include <cstring>
#endif

namespace dyf::Diagnostics
{
CpuZone::CpuZone(const char* name,const char* function,const char* file,std::uint32_t line) noexcept
{
#if defined(DY_TRACY_ENABLED)
    if(!TracyCIsConnected)return;
    const auto location=___tracy_alloc_srcloc_name(line,file,std::strlen(file),
        function,std::strlen(function),name,std::strlen(name),0);
    const auto context=___tracy_emit_zone_begin_alloc(location,1);
    id=context.id;active=context.active!=0;
#else
    (void)name;(void)function;(void)file;(void)line;
#endif
}
CpuZone::~CpuZone()
{
#if defined(DY_TRACY_ENABLED)
    if(active)___tracy_emit_zone_end({id,1});
#endif
}
void MarkProfilerFrame() noexcept
{
#if defined(DY_TRACY_ENABLED)
    ___tracy_emit_frame_mark(nullptr);
#endif
}
void PublishProfilerValue(const char* name,double value,bool raw) noexcept
{
#if defined(DY_TRACY_ENABLED)
#if !defined(DY_TRACY_RAW_PLOTS_ENABLED)
    if(raw)return;
#else
    (void)raw;
#endif
    ___tracy_emit_plot(name,value);
#else
    (void)name;(void)value;(void)raw;
#endif
}
void PublishProfilerRawFrame(const Platform::ProfilerTimingSample& sample) noexcept
{
    PublishProfilerValue("Frame.Raw.ms",sample.frameMilliseconds,true);
    PublishProfilerValue("CPU.Render.Raw.ms",sample.cpuRenderMilliseconds,true);
}
void PublishProfilerSnapshot(const Platform::ProfilerTimingSnapshot& sample,const RHI::ResourceAllocationCounters& counters) noexcept
{
    PublishProfilerValue("Frame.Average.ms",sample.frameAverageMilliseconds);
    PublishProfilerValue("Frame.Maximum.ms",sample.frameMaximumMilliseconds);
    PublishProfilerValue("CPU.Render.Average.ms",sample.cpuRenderAverageMilliseconds);
    if(sample.hasGpuFrame)PublishProfilerValue("GPU.Frame.Average.ms",sample.gpuFrameAverageMilliseconds);
    if(sample.hasGpuMain)PublishProfilerValue("GPU.MainForward.Average.ms",sample.gpuMainAverageMilliseconds);
    if(sample.hasGpuShadow)PublishProfilerValue("GPU.Shadow.Average.ms",sample.gpuShadowAverageMilliseconds);
    PublishProfilerValue("GPU.Resources.Buffers.Live",static_cast<double>(counters.buffers.live));
    PublishProfilerValue("GPU.Resources.Textures.Live",static_cast<double>(counters.textures.live));
    PublishProfilerValue("GPU.Resources.Pipelines.Live",static_cast<double>(counters.pipelines.live));
}
}
