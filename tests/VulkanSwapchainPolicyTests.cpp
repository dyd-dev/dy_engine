#include "Backends/Vulkan/VulkanSwapchain.h"
#include <cstdio>
#include <stdexcept>
#include <utility>

namespace {
VkSurfaceCapabilitiesKHR capabilities{};
VkSwapchainCreateInfoKHR created{};
unsigned createCalls=0;
void Check(bool value,const char* message) {if(!value)throw std::runtime_error(message);}
}
extern "C" {
VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceSurfaceCapabilitiesKHR(VkPhysicalDevice,VkSurfaceKHR,VkSurfaceCapabilitiesKHR* value)
{*value=capabilities;return VK_SUCCESS;}
VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceSurfaceFormatsKHR(VkPhysicalDevice,VkSurfaceKHR,uint32_t* count,VkSurfaceFormatKHR* values)
{if(values)*values={VK_FORMAT_B8G8R8A8_UNORM,VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};*count=1;return VK_SUCCESS;}
VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceSurfacePresentModesKHR(VkPhysicalDevice,VkSurfaceKHR,uint32_t* count,VkPresentModeKHR* values)
{if(values)*values=VK_PRESENT_MODE_IMMEDIATE_KHR;*count=1;return VK_SUCCESS;}
VKAPI_ATTR VkResult VKAPI_CALL vkCreateSwapchainKHR(VkDevice,const VkSwapchainCreateInfoKHR* info,const VkAllocationCallbacks*,VkSwapchainKHR* value)
{created=*info;++createCalls;*value=reinterpret_cast<VkSwapchainKHR>(uintptr_t{1});return VK_SUCCESS;}
VKAPI_ATTR void VKAPI_CALL vkDestroySwapchainKHR(VkDevice,VkSwapchainKHR,const VkAllocationCallbacks*) {}
VKAPI_ATTR VkResult VKAPI_CALL vkGetSwapchainImagesKHR(VkDevice,VkSwapchainKHR,uint32_t* count,VkImage* images)
{if(images)for(uint32_t i=0;i<3;++i)images[i]=reinterpret_cast<VkImage>(uintptr_t(i+1));*count=3;return VK_SUCCESS;}
VKAPI_ATTR VkResult VKAPI_CALL vkCreateImageView(VkDevice,const VkImageViewCreateInfo*,const VkAllocationCallbacks*,VkImageView* view)
{*view=reinterpret_cast<VkImageView>(uintptr_t{1});return VK_SUCCESS;}
VKAPI_ATTR void VKAPI_CALL vkDestroyImageView(VkDevice,VkImageView,const VkAllocationCallbacks*) {}
}
int main(int argc,char** argv) try
{
    using namespace dyf::Backends;
    VulkanContext context;context.device=reinterpret_cast<VkDevice>(uintptr_t{1});
    context.physicalDevice=reinterpret_cast<VkPhysicalDevice>(uintptr_t{1});context.surface=reinterpret_cast<VkSurfaceKHR>(uintptr_t{1});
    context.queueIndices={0,0};
    capabilities.minImageCount=2;capabilities.maxImageCount=3;capabilities.currentExtent={64,32};
    capabilities.supportedUsageFlags=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    capabilities.supportedTransforms=VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR|VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR;
    capabilities.currentTransform=VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR;
    capabilities.supportedCompositeAlpha=VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    const auto initialize=[&](VulkanSwapchain& swap,VkCompositeAlphaFlagBitsKHR alpha) {
        VulkanSwapchain::InitializationStatus status;bool retired=false;
        return swap.Initialize(context,nullptr,VK_FORMAT_B8G8R8A8_UNORM,VK_COLOR_SPACE_SRGB_NONLINEAR_KHR,
            alpha,VK_PRESENT_MODE_IMMEDIATE_KHR,2,false,0,0,VK_NULL_HANDLE,retired,status);
    };
    const bool alphaOnly=argc==2 && argv[1][0]=='a';
    if(alphaOnly)
    {
        for(auto requested:{VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR,VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR})
        {
            VulkanSwapchain swap;createCalls=0;
            const bool accepted=initialize(swap,requested);swap.Cleanup(context.device);
            Check(!accepted && createCalls==0,"Unsupported composite alpha was substituted instead of rejected");
        }
        VulkanSwapchain supported;Check(initialize(supported,VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR),"Supported alpha was rejected");
        Check(created.compositeAlpha==VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,"Supported alpha changed");supported.Cleanup(context.device);
        std::puts("PASS swapchain preserves requested composite alpha or rejects before native creation");
        return 0;
    }
    VulkanSwapchain swap;Check(initialize(swap,VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR),"Transform fixture initialization failed");
    Check(created.preTransform==VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,"Fixture did not select a transform different from currentTransform");
    VulkanSwapchain moved(std::move(swap));bool changed=true;VulkanSwapchain::InitializationStatus status;
    Check(moved.IsSurfaceConfigurationChanged(context,nullptr,changed,status) && !changed,
        "Unchanged rotated surface was detected as a resize");
    capabilities.currentTransform=VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    Check(moved.IsSurfaceConfigurationChanged(context,nullptr,changed,status) && changed,"A real surface rotation was ignored");
    capabilities.currentTransform=VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR;capabilities.currentExtent.width=65;
    Check(moved.IsSurfaceConfigurationChanged(context,nullptr,changed,status) && changed,"A real extent change was ignored");
    moved.Cleanup(context.device);
    std::puts("PASS surface change detection preserves the creation-time surface transform across moves");
    return 0;
}
catch(const std::exception& error) {std::fprintf(stderr,"FAIL VulkanSwapchainPolicy: %s\n",error.what());return 1;}
