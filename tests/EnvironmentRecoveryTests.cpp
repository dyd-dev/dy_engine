// Faults target CPU ownership and recording, before any native GPU submission.
#define main CanvasReuseEntryUnused
#include "CanvasReuseTests.cpp"
#undef main
#include "Extends/AdvancedRendering/EnvironmentLighting.h"
#include <cstdlib>
#include <new>

namespace {thread_local long failAfter=-1;thread_local bool allocationHit=false;}
void* operator new(size_t size)
{
    if(failAfter>=0 && failAfter--==0) {failAfter=-1;allocationHit=true;throw std::bad_alloc{};}
    if(void* memory=std::malloc(size?size:1))return memory;
    throw std::bad_alloc{};
}
void* operator new[](size_t size) {return ::operator new(size);}
void operator delete(void* memory) noexcept {std::free(memory);}
void operator delete[](void* memory) noexcept {std::free(memory);}
void operator delete(void* memory,size_t) noexcept {std::free(memory);}
void operator delete[](void* memory,size_t) noexcept {std::free(memory);}

namespace {
struct EnvironmentDevice final:Device
{
    unsigned failTexture=0,armTexture=0;
    long allocationPosition=-1;
    bool throwing=false,creationHit=false;
    RHI::TextureHandle CreateTextureNative(const RHI::TextureDesc& desc) override
    {
        if(failTexture && --failTexture==0) {
            creationHit=true;if(throwing)throw std::bad_alloc{};return nullptr;
        }
        auto* image=NullDevice::CreateTextureNative(desc);
        if(armTexture && --armTexture==0)failAfter=allocationPosition;
        return image;
    }
    void CheckEmpty()
    {
        Check(WaitIdle(),"environment drain");const auto counts=GetResourceAllocationCounters();
        Check(!counts.textures.live && !counts.buffers.live && !counts.pipelines.live,"temporary environment resources leaked");
    }
};

void Retry(EnvironmentDevice& device,dyf::EnvironmentLighting& environment)
{
    RHI::ResourceScope own(device);auto* commands=own.Keep(device.AcquireCommandList());
    Check(environment.Prepare(device,*commands,{},dyf::Camera{},1,1,false,false),"environment prepare retry");
    Check(commands->Close() && device.Submit(&commands,1),"environment retry submission");
    environment.CommitFrame();
}

void CreationRecovery()
{
    for(bool throwing:{false,true})for(unsigned position=1;position<=4;++position) {
        EnvironmentDevice device;dyf::EnvironmentLighting environment;
        auto* commands=device.AcquireCommandList();Check(commands!=nullptr,"creation command fixture");
        device.failTexture=position;device.throwing=throwing;
        bool prepared=false;
        try {prepared=environment.Prepare(device,*commands,{},dyf::Camera{},1,1,false,false);}
        catch(const std::bad_alloc&) {Check(throwing,"unexpected creation exception");}
        device.failTexture=0;device.throwing=false;
        Check(device.creationHit && !prepared,"texture creation fault not reached");
        device.DestroyCommandList(commands);environment.Reset();device.CheckEmpty();
        Retry(device,environment);environment.Reset();device.CheckEmpty();
    }
    std::puts("PASS environment texture creation: eight null/exception failures and retries");
}

void RecordingRecovery(bool probes)
{
    unsigned failures=0;
    for(long position=0;position<128;++position) {
        EnvironmentDevice device;dyf::EnvironmentLighting environment;
        auto* commands=device.AcquireCommandList();Check(commands!=nullptr,"recording command fixture");
        device.armTexture=probes?5:2;device.allocationPosition=position;allocationHit=false;
        bool prepared=false;
        try {prepared=environment.Prepare(device,*commands,{},dyf::Camera{},1,1,false,false);}
        catch(const std::bad_alloc&) {}
        const bool hit=allocationHit;failAfter=-1;device.armTexture=0;
        if(hit)++failures;else Check(prepared,"recording control failed");
        device.DestroyCommandList(commands);environment.Reset();device.CheckEmpty();
        if(!hit)break;
    }
    Check(failures>0 && failures<128,"recording allocation sweep incomplete");
    EnvironmentDevice device;dyf::EnvironmentLighting environment;
    Retry(device,environment);environment.Reset();device.CheckEmpty();
    std::printf("PASS environment %s recording: %u allocation failures\n",probes?"probe":"panorama",failures);
}
}

int main(int argc,char** argv) try
{
    Check(argc==2,"expected environment recovery case");
    const std::string mode=argv[1];
    if(mode=="creation")CreationRecovery();
    else if(mode=="panorama")RecordingRecovery(false);
    else if(mode=="probes")RecordingRecovery(true);
    else throw std::runtime_error("unknown environment recovery case");
    return 0;
}catch(const std::exception& error) {failAfter=-1;std::fprintf(stderr,"FAIL environment recovery: %s\n",error.what());return 1;}
