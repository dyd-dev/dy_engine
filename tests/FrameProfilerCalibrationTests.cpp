// CPU-only audit of current linked FrameProfiler.cpp, not extracted logic.
// NullDevice owns the real public RHI; native calibration is an instrumented stub.
#include "Platform/Profiler/FrameProfiler.h"
#include "Backends/Null/NullDevice.h"
#include "dyf/RHI/Submission.h"
#include <cstdio>
#include <stdexcept>

namespace R = dyf::RHI;
namespace P = dyf::Platform;
static void Check(bool value,const char* message)
{if(!value)throw std::runtime_error(message);}

class CalibrationDevice final : public dyf::Backends::NullDevice
{
public:
    mutable unsigned nativeCalibrationCalls=0;
    CalibrationDevice() {Check(Initialize(nullptr,{})==0,"Null initialization");}
protected:
    bool SupportsNative(R::Feature feature) const override
    {
        if(feature==R::Feature::TimestampCalibration)return true;
        return NullDevice::SupportsNative(feature);
    }
    bool GetTimestampCalibrationNative(R::TimestampCalibration&) const override
    {
        ++nativeCalibrationCalls;
        return false; // A failing native calibration still proves the unwanted call.
    }
};

int main()
{
    try
    {
        Check(!P::IsLiveCollectorEnabled(),"Probe requires live collector OFF");
        CalibrationDevice device;
        P::FrameProfiler profiler(device);
        profiler.PrepareGpuScopes(4); // Actual disabled path: no scope is prepared.
        Check(profiler.PendingGpuSamples()==0,"No frame timing packet submitted");
        profiler.SubmitGpuScopes({},{});
        Check(device.nativeCalibrationCalls==0,"Empty completion control must not calibrate");

        // Obtain an authentic nonempty FenceHandle through current public RHI.
        auto* commands=device.AcquireCommandList();
        Check(commands && commands->Close(),"Close empty CPU Null command list");
        R::FenceHandle completion;
        const bool submitted=device.Submit({&commands,1,nullptr,0},completion);
        device.DestroyCommandList(commands);
        Check(submitted && bool(completion),"Submit must return an authentic completion");

        profiler.SubmitGpuScopes(completion,{});
        std::printf("OBSERVED live=OFF prepared_scopes=0 pending_packets=%zu native_calibration_calls=%u\n",
            profiler.PendingGpuSamples(),device.nativeCalibrationCalls);
        Check(device.nativeCalibrationCalls==0,"Empty-scope handoff must not request calibration");
        Check(device.WaitIdle(),"CPU Null final drain");
        std::puts("PASS empty live scope avoids calibration; authentic completion retained");
        return 0;
    }
    catch(const std::exception& error)
    {std::fprintf(stderr,"FAIL FrameProfiler calibration audit: %s\n",error.what());return 1;}
}
