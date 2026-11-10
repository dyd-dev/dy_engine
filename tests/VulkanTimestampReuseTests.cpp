#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <vulkan/vulkan.h>
#include "dyf/RHI.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <stdexcept>

namespace {
std::atomic<bool> holdCompletions{false};
VkQueryPool observedPool=VK_NULL_HANDLE;
uint64_t previousTicks[2]={};
unsigned staleReads=0;
template<class Function> Function Native(const char* name)
{
    auto function=GetProcAddress(GetModuleHandleW(L"vulkan-1.dll"),name);
    if(!function)std::abort();
    return reinterpret_cast<Function>(function);
}
void Check(bool value,const char* message) {if(!value)throw std::runtime_error(message);}
struct ReleaseProbe {~ReleaseProbe(){holdCompletions=false;}};
}
extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkGetFenceStatus(VkDevice device,VkFence fence)
{
    if(holdCompletions)return VK_NOT_READY;
    static auto real=Native<PFN_vkGetFenceStatus>("vkGetFenceStatus");return real(device,fence);
}
extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkGetQueryPoolResults(VkDevice device,VkQueryPool pool,
    uint32_t first,uint32_t count,size_t size,void* output,VkDeviceSize stride,VkQueryResultFlags flags)
{
    if(holdCompletions && pool==observedPool)
    {
        ++staleReads;
        if(first+count>2 || stride!=sizeof(uint64_t) || size<count*sizeof(uint64_t))return VK_ERROR_UNKNOWN;
        std::memcpy(output,previousTicks+first,count*sizeof(uint64_t));return VK_SUCCESS;
    }
    static auto real=Native<PFN_vkGetQueryPoolResults>("vkGetQueryPoolResults");
    observedPool=pool;return real(device,pool,first,count,size,output,stride,flags);
}
int main() try
{
    using namespace dyf::RHI;
    DeviceDesc desc;desc.enableValidation=true;
    std::unique_ptr<IDevice> device(IDevice::Create(desc));Check(bool(device),"Vulkan device creation failed");
    if(!device->Supports(Feature::TimestampQuery)) {std::puts("SKIP timestamp queries unsupported");return 77;}
    ReleaseProbe release;ResourceScope own(*device);
    auto* query=own.Keep(device->CreateTimestampQuery({2}));
    auto* initial=own.Keep(device->AcquireCommandList());
    Check(query && initial,"Timestamp fixture creation failed");
    initial->ResetTimestamps(query,0,2);initial->WriteTimestamp(query,0);initial->WriteTimestamp(query,1);
    Check(initial->Close() && device->Submit(&initial,1) && device->WaitIdle(),"Initial query submission failed");
    Check(device->ReadTimestamps(query,0,2,previousTicks),"Initial completed timestamps unavailable");
    auto* reused=own.Keep(device->AcquireCommandList());
    reused->ResetTimestamps(query,0,1);reused->WriteTimestamp(query,0);
    Check(reused->Close(),"Reused query recording failed");
    uint64_t tick=0;
    Check(device->ReadTimestamps(query,0,1,&tick) && tick==previousTicks[0],"Recording alone invalidated completed results");
    Check(device->Submit(&reused,1),"Reused query submission failed");
    holdCompletions=true;staleReads=0;
    Check(!device->ReadTimestamps(query,0,1,&tick) && staleReads==0,
        "A pending latest submission consulted stale native availability");
    Check(device->ReadTimestamps(query,1,1,&tick) && tick==previousTicks[1] && staleReads==1,
        "Partial query reuse invalidated an untouched result");
    holdCompletions=false;
    Check(device->WaitIdle() && device->ReadTimestamps(query,0,1,&tick),"New query completion did not become readable");
    std::puts("PASS Vulkan timestamp reuse: native submissions, latest completion gate, recording-only and partial-range controls");
    return 0;
}
catch(const std::exception& error)
{
    holdCompletions=false;std::fprintf(stderr,"FAIL VulkanTimestampReuse: %s\n",error.what());return 1;
}
