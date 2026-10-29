// Standalone GCC test: -flto -ffunction-sections -Wl,--gc-sections.
#include "../src/Platform/Profiler/Profiler.cpp"
#include <cstdio>
#include <cstdlib>

#define CHECK(condition) do { if(!(condition)) { std::fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#condition); std::exit(1); } } while(false)
namespace
{
    dyf::Platform::FrameTimingEvent received[2]{};
    unsigned count=0;
    unsigned gpuCount=0;
    dyf::Platform::GpuTimingSample gpuReceived{};
    void CaptureGpu(const dyf::Platform::GpuTimingSample& sample) noexcept
    {gpuReceived=sample;++gpuCount;}
    void Capture(const dyf::Platform::FrameTimingEvent& event) noexcept
    {CHECK(count<2);received[count++]=event;}
}
int main()
{
    using namespace dyf::Platform;
    CHECK(!HasFrameTimingSink());
    PublishGpuTiming({{3,4,42},20,7,13});
    CHECK(gpuCount==0);
    PublishFrameTiming({17,42,"Vulkan present",10,30,0});
    CHECK(count==0);
    TraceSink sink{};sink.frameTiming=Capture;
    sink.gpuTiming=CaptureGpu;
    const auto* previous=SetTraceSink(&sink);
    CHECK(HasFrameTimingSink());
    PublishFrameTiming({17,43,"Vulkan acquire",40,50,0});
    // An older worker result arrives after the next frame has already started.
    PublishFrameTiming({17,42,"Vulkan present",10,30,0});
    CHECK(count==2 && received[0].frame==43 && received[1].frame==42);
    CHECK(received[1].source==17 && received[1].beginNs==10 && received[1].endNs==30);
    CHECK(std::strcmp(received[1].name,"Vulkan present")==0);
    PublishGpuTiming({{3,4,42},20,7,13});
    CHECK(gpuCount==1 && gpuReceived.frame.frame==42 && gpuReceived.frame.source==4);
    CHECK(gpuReceived.frameMilliseconds==20 && gpuReceived.uploadToGraphMilliseconds==7 && gpuReceived.graphMilliseconds==13);
    TraceSink unrelated{};RemoveTraceSink(&unrelated);
    CHECK(HasFrameTimingSink());
    SetTraceSink(previous);
    CHECK(!HasFrameTimingSink());
    PublishGpuTiming({{3,4,43},21,8,13});
    CHECK(gpuCount==1);
    std::puts("PASS FrameTimingTrace: opt-in callback, delayed original frame identity, sink ownership");
}
