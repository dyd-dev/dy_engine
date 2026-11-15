#include "dyf/RHI.h"
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <new>
#include <stdexcept>

namespace {
thread_local bool countAllocations=false,failNextAllocation=false;
thread_local size_t allocations=0;
void Check(bool value,const char* message) {if(!value)throw std::runtime_error(message);}
}
void* operator new(size_t size)
{
    if(countAllocations)++allocations;
    if(failNextAllocation) {failNextAllocation=false;throw std::bad_alloc{};}
    if(void* memory=std::malloc(size?size:1))return memory;
    throw std::bad_alloc{};
}
void* operator new[](size_t size) {return ::operator new(size);}
void operator delete(void* memory) noexcept {std::free(memory);}
void operator delete[](void* memory) noexcept {std::free(memory);}
void operator delete(void* memory,size_t) noexcept {std::free(memory);}
void operator delete[](void* memory,size_t) noexcept {std::free(memory);}

int main() try
{
    using namespace dyf::RHI;
    std::unique_ptr<IDevice> device(IDevice::Create({}));
    Check(bool(device),"Device creation failed");
    ResourceScope resources(*device);
    auto* commands=resources.Keep(device->AcquireCommandList());
    Check(commands!=nullptr,"Command acquisition failed");
    BufferHandle first=nullptr;
    for(uint32_t i=0;i<64;++i)
    {
        auto* buffer=resources.Keep(device->CreateBuffer({16,0,BufferUsage::Storage,ResourceState::Common}));
        Check(buffer!=nullptr,"Buffer creation failed");
        if(!first)first=buffer;
        const ResourceBarrierDesc cycle[]={
            {buffer,nullptr,ResourceState::Common,ResourceState::CopyDestination,{}},
            {buffer,nullptr,ResourceState::CopyDestination,ResourceState::Common,{}}};
        commands->ResourceBarrier(cycle,2);
    }
    allocations=0;countAllocations=true;
    const bool closed=commands->Close();
    countAllocations=false;
    std::printf("Close allocations for 64 recorded transition cycles: %zu\n",allocations);
    Check(closed,"Recorded transition cycles did not close");
    Check(allocations==0,"Close must not allocate a second per-resource state summary");
    Check(device->Submit(&commands,1) && device->WaitIdle(),"Transition cycle submission failed");

    Check(device->ResetCommandList(commands),"Completed command Reset failed");
    const ResourceBarrierDesc invalid[]={
        {first,nullptr,ResourceState::Common,ResourceState::CopyDestination,{}},
        {first,nullptr,ResourceState::Common,ResourceState::UnorderedAccess,{}},
        {first,nullptr,ResourceState::UnorderedAccess,ResourceState::Common,{}}};
    commands->ResourceBarrier(invalid,3);
    Check(commands->Close(),"An internal mismatch must remain a submission error");
    Check(!device->Submit(&commands,1),"A later operation erased an earlier state mismatch");
    Check(device->ResetCommandList(commands),"Rejected command Reset failed");
    const ResourceBarrierDesc valid={first,nullptr,ResourceState::Common,ResourceState::UnorderedAccess,{}};
    commands->ResourceBarrier(&valid,1);
    Check(commands->Close() && device->Submit(&commands,1) && device->WaitIdle(),
        "Rejected submission changed the global state or Reset retained invalidity");

    auto* failure=resources.Keep(device->AcquireCommandList());
    Check(failure!=nullptr,"Failure fixture command acquisition failed");
    bool threw=false;
    failNextAllocation=true;
    try {failure->ResourceBarrier(&valid,1);} catch(const std::bad_alloc&) {threw=true;}
    failNextAllocation=false;
    Check(threw && !failure->Close(),"A failed recording allocation must invalidate the command list");
    Check(device->ResetCommandList(failure),"Allocation failure Reset failed");
    const ResourceBarrierDesc recovered={first,nullptr,ResourceState::UnorderedAccess,ResourceState::Common,{}};
    failure->ResourceBarrier(&recovered,1);
    Check(failure->Close() && device->Submit(&failure,1) && device->WaitIdle(),"Allocation failure recovery failed");
    std::puts("PASS RhiCommandClose: allocation-free close, latched mismatch, rollback, Reset and recording failure recovery");
    return 0;
}
catch(const std::exception& error)
{
    countAllocations=false;failNextAllocation=false;
    std::fprintf(stderr,"FAIL RhiCommandClose: %s\n",error.what());return 1;
}
