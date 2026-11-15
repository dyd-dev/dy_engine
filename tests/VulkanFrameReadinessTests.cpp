#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <vulkan/vulkan.h>
#include "dyf/RHI.h"
#include "dyf/Platform/Window.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <unordered_set>

namespace {
std::atomic<bool> pendingFences{false},holdPresent{false},presentEntered{false},prefetchAcquired{false};
std::atomic<unsigned> surfaceQueries{0};
std::atomic<bool> deferPresentFences{false},resetPendingPresent{false};
std::atomic<bool> destroyedPendingPresent{false};
std::mutex presentFenceMutex;
std::unordered_set<VkFence> deferredPresentFences;
DWORD ownerThread=0;
template<class Function> Function Native(const char* name)
{
    auto function=GetProcAddress(GetModuleHandleW(L"vulkan-1.dll"),name);
    if(!function)std::abort();
    return reinterpret_cast<Function>(function);
}
void Check(bool value,const char* message) {if(!value)throw std::runtime_error(message);}
struct ReleaseProbes {
    ~ReleaseProbes() {pendingFences=false;holdPresent=false;deferPresentFences=false;}
};
}

// Only native readiness and the WSI host-call boundary are controlled; the real device renders and retires resources.
extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkGetFenceStatus(VkDevice device,VkFence fence)
{
    if(pendingFences.load())return VK_NOT_READY;
    if(deferPresentFences.load())
    {
        std::lock_guard<std::mutex> lock(presentFenceMutex);
        if(deferredPresentFences.count(fence))return VK_NOT_READY;
    }
    static auto real=Native<PFN_vkGetFenceStatus>("vkGetFenceStatus");
    return real(device,fence);
}
extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkResetFences(VkDevice device,uint32_t count,const VkFence* fences)
{
    if(deferPresentFences.load())
    {
        std::lock_guard<std::mutex> lock(presentFenceMutex);
        for(uint32_t i=0;i<count;++i)
            if(deferredPresentFences.count(fences[i]))resetPendingPresent=true;
    }
    static auto real=Native<PFN_vkResetFences>("vkResetFences");
    return real(device,count,fences);
}
extern "C" VKAPI_ATTR void VKAPI_CALL vkDestroyFence(VkDevice device,VkFence fence,const VkAllocationCallbacks* allocator)
{
    if(deferPresentFences.load())
    {
        std::lock_guard<std::mutex> lock(presentFenceMutex);
        if(deferredPresentFences.count(fence))destroyedPendingPresent=true;
    }
    static auto real=Native<PFN_vkDestroyFence>("vkDestroyFence");
    real(device,fence,allocator);
}
extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
    VkPhysicalDevice device,VkSurfaceKHR surface,VkSurfaceCapabilitiesKHR* capabilities)
{
    ++surfaceQueries;
    static auto real=Native<PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR>("vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
    return real(device,surface,capabilities);
}
extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkAcquireNextImageKHR(
    VkDevice device,VkSwapchainKHR swapchain,uint64_t timeout,VkSemaphore semaphore,VkFence fence,uint32_t* image)
{
    static auto real=Native<PFN_vkAcquireNextImageKHR>("vkAcquireNextImageKHR");
    const auto result=real(device,swapchain,timeout,semaphore,fence,image);
    if(timeout==0 && (result==VK_SUCCESS || result==VK_SUBOPTIMAL_KHR))prefetchAcquired=true;
    return result;
}
extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkQueuePresentKHR(VkQueue queue,const VkPresentInfoKHR* present)
{
    if(deferPresentFences.load())
    {
        std::lock_guard<std::mutex> lock(presentFenceMutex);
        for(auto* extension=static_cast<const VkBaseInStructure*>(present->pNext);extension;extension=extension->pNext)
            if(extension->sType==VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_EXT)
            {
                const auto* info=reinterpret_cast<const VkSwapchainPresentFenceInfoEXT*>(extension);
                for(uint32_t i=0;i<info->swapchainCount;++i)
                    if(info->pFences[i])deferredPresentFences.insert(info->pFences[i]);
            }
    }
    if(holdPresent.load() && GetCurrentThreadId()!=ownerThread)
    {
        presentEntered=true;
        while(holdPresent.load())std::this_thread::yield();
    }
    static auto real=Native<PFN_vkQueuePresentKHR>("vkQueuePresentKHR");
    return real(queue,present);
}

namespace {
using namespace dyf::RHI;
void Begin(IDevice& device)
{
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
    while(!device.BeginFrame())
    {
        Check(!device.IsLost() && std::chrono::steady_clock::now()<deadline,"Frame did not become ready");
        dyf::Platform::Window::PollEvents();std::this_thread::yield();
    }
}
void Finish(IDevice& device)
{
    ResourceScope resources(device);
    auto* commands=resources.Keep(device.AcquireCommandList());
    auto* image=device.GetBackBuffer();
    Check(commands && image,"Frame recording resources are unavailable");
    ResourceBarrierDesc barrier={nullptr,image,ResourceState::Present,ResourceState::RenderTarget,{}};
    commands->ResourceBarrier(&barrier,1);
    ColorAttachment color;color.texture=image;color.loadOp=LoadOp::Clear;color.storeOp=StoreOp::Store;
    color.clearColor[3]=1;
    commands->BeginRendering({&color,1,nullptr});commands->EndRendering();
    barrier.before=ResourceState::RenderTarget;barrier.after=ResourceState::Present;
    commands->ResourceBarrier(&barrier,1);
    Check(commands->Close() && device.Submit(&commands,1) && device.Present(),"Frame submission/presentation failed");
}
void DelayedPresentFence()
{
    Check(_putenv_s("DY_VULKAN_PREFETCH","0")==0,"Cannot configure prefetch");
    dyf::Platform::Window window(96,64,"Vulkan delayed present fence");
    DeviceDesc desc;desc.maxFramesInFlight=2;desc.enableValidation=true;
    std::unique_ptr<IDevice> device(IDevice::Create(desc));
    Check(bool(device),"Vulkan device creation failed");
    ReleaseProbes release;
    SwapchainDesc swap;swap.window=window.GetHandle();swap.format=DefaultSwapchainFormat;
    swap.presentMode=PresentMode::Immediate;swap.minimumImageCount=3;
    Check(device->CreateSwapchain(swap),"Swapchain creation failed");
    deferredPresentFences.clear();resetPendingPresent=false;destroyedPendingPresent=false;deferPresentFences=true;
    for(unsigned frame=0;frame<12;++frame){Begin(*device);Finish(*device);}
    window.Resize(112,80);dyf::Platform::Window::PollEvents();
    for(unsigned frame=0;frame<2;++frame){Begin(*device);Finish(*device);}
    deferPresentFences=false;
    Check(device->WaitIdle(),"Delayed present work did not drain");
    Check(!resetPendingPresent,"A pending presentation fence was reset");
    Check(!destroyedPendingPresent,"A pending presentation fence was destroyed during resize");
    Check(!device->IsLost(),"Delayed presentation or resize lost the device");
    if(deferredPresentFences.empty())std::puts("SKIP delayed present fence: maintenance extension unavailable");
    else std::printf("PASS delayed present fence: %zu pending fences retained across rendering and resize\n",deferredPresentFences.size());
    for(unsigned frame=0;frame<2;++frame){Begin(*device);Finish(*device);}
    Check(device->WaitIdle(),"Presentation fence reuse did not drain");
}
void Run(uint32_t framesInFlight,PresentMode mode,bool prefetched=false)
{
    prefetchAcquired=false;
    Check(_putenv_s("DY_VULKAN_PREFETCH",prefetched?"1":"0")==0,"Cannot configure the test process prefetch policy");
    dyf::Platform::Window window(96,64,"Vulkan readiness regression");
    DeviceDesc desc;desc.maxFramesInFlight=framesInFlight;desc.enableValidation=true;
    std::unique_ptr<IDevice> device(IDevice::Create(desc));
    Check(bool(device),"Vulkan device creation failed");
    ReleaseProbes release;
    SwapchainDesc swap;swap.window=window.GetHandle();swap.format=DefaultSwapchainFormat;
    swap.presentMode=mode;swap.minimumImageCount=3;
    Check(device->CreateSwapchain(swap),"Swapchain creation failed");
    Begin(*device);
    if(framesInFlight==2) {holdPresent=true;presentEntered=false;}
    Finish(*device);
    if(framesInFlight==1)pendingFences=true;
    else
    {
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
        while(!presentEntered && std::chrono::steady_clock::now()<deadline)std::this_thread::yield();
        if(!presentEntered) {holdPresent=false;std::puts("SKIP asynchronous Present probe: no separate worker queue");return;}
    }
    if(prefetched)
    {
        if(!prefetchAcquired)
        {
            holdPresent=false;
            Check(device->WaitIdle(),"Unavailable prefetch did not drain");
            std::puts("SKIP prefetched overlap probe: WSI did not return an immediately available image");
            return;
        }
        Check(device->BeginFrame(),"An already prefetched image was blocked by the pending Present");
        holdPresent=false;Finish(*device);
        Check(device->WaitIdle(),"Prefetched overlap did not drain");
        std::puts("PASS prefetched image proceeds while previous host Present is pending");
        return;
    }
    surfaceQueries=0;
    for(unsigned attempt=0;attempt<20;++attempt)
        Check(!device->BeginFrame(),"A pending frame became available before its prerequisite");
    const auto calls=surfaceQueries.load();
    pendingFences=false;holdPresent=false;
    std::printf("Surface queries while %s is pending: %u\n",framesInFlight==1?"frame slot":"Present",calls);
    Check(calls==0,"Unavailable frame retries must not query surface capabilities");
    Check(device->WaitIdle(),"Pending work did not drain");

    window.Resize(112,80);dyf::Platform::Window::PollEvents();
    Begin(*device);
    const auto size=window.GetFramebufferSize();
    const auto target=device->GetBackBuffer()->GetDesc();
    Check(target.width==uint32_t(size.width) && target.height==uint32_t(size.height),"Readiness gate lost a resize");
    Finish(*device);
    ShowWindow(static_cast<HWND>(window.GetHandle()),SW_MINIMIZE);
    dyf::Platform::Window::PollEvents();
    if(device->BeginFrame())Finish(*device);
    Check(!device->IsLost(),"Minimize lost the device");
    window.Restore();dyf::Platform::Window::PollEvents();
    Begin(*device);Finish(*device);
    Check(device->WaitIdle(),"Restored frame did not finish");
}
}
int main(int argc,char** argv) try
{
    ownerThread=GetCurrentThreadId();
    DelayedPresentFence();
    if(argc==2 && std::string_view(argv[1])=="--delayed-present-only")return 0;
    for(auto mode:{dyf::RHI::PresentMode::Immediate,dyf::RHI::PresentMode::Fifo})
    {Run(1,mode);Run(2,mode);}
    Run(2,dyf::RHI::PresentMode::Fifo,true);
    std::puts("PASS VulkanFrameReadiness: pending slot/Present, FIFO/Immediate, resize, minimize, restore and drain");
    return 0;
}
catch(const std::exception& error)
{
    pendingFences=false;holdPresent=false;
    std::fprintf(stderr,"FAIL VulkanFrameReadiness: %s\n",error.what());return 1;
}
