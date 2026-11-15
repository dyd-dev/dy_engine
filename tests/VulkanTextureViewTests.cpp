#include <algorithm>
#include <cstdio>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <vector>
#include "Backends/Vulkan/VulkanContext.h"
#include "dyf/RHI/Texture.h"
#include "Backends/Vulkan/VulkanResources.h"

namespace dyf::Backends {
struct VulkanObjectDeleter {
    template<typename Object> void operator()(Object* object) const {delete object;}
};
}

namespace {
VkImageViewCreateInfo lastView{};
unsigned createdViews=0;
void Check(bool value,const char* message) {if(!value)throw std::runtime_error(message);}
}
extern "C" {
VKAPI_ATTR VkResult VKAPI_CALL vkCreateImageView(VkDevice,const VkImageViewCreateInfo* info,const VkAllocationCallbacks*,VkImageView* view)
{lastView=*info;*view=reinterpret_cast<VkImageView>(uintptr_t(++createdViews+10));return VK_SUCCESS;}
VKAPI_ATTR void VKAPI_CALL vkDestroyImageView(VkDevice,VkImageView,const VkAllocationCallbacks*) {}
VKAPI_ATTR void VKAPI_CALL vkDestroyImage(VkDevice,VkImage,const VkAllocationCallbacks*) {}
VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceImageFormatProperties(VkPhysicalDevice,VkFormat,VkImageType,VkImageTiling,VkImageUsageFlags,VkImageCreateFlags,VkImageFormatProperties* properties)
{*properties={{16,16,1},4,4,VK_SAMPLE_COUNT_1_BIT,4096};return VK_SUCCESS;}
VKAPI_ATTR VkResult VKAPI_CALL vkCreateImage(VkDevice,const VkImageCreateInfo*,const VkAllocationCallbacks*,VkImage* image)
{*image=reinterpret_cast<VkImage>(uintptr_t{1});return VK_SUCCESS;}
VKAPI_ATTR void VKAPI_CALL vkGetImageMemoryRequirements(VkDevice,VkImage,VkMemoryRequirements* requirements)
{*requirements={4096,4,1};}
VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceMemoryProperties(VkPhysicalDevice,VkPhysicalDeviceMemoryProperties* properties)
{*properties={};properties->memoryTypeCount=1;properties->memoryTypes[0].propertyFlags=VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;}
VKAPI_ATTR VkResult VKAPI_CALL vkAllocateMemory(VkDevice,const VkMemoryAllocateInfo*,const VkAllocationCallbacks*,VkDeviceMemory* memory)
{*memory=reinterpret_cast<VkDeviceMemory>(uintptr_t{1});return VK_SUCCESS;}
VKAPI_ATTR VkResult VKAPI_CALL vkBindImageMemory(VkDevice,VkImage,VkDeviceMemory,VkDeviceSize) {return VK_SUCCESS;}
VKAPI_ATTR void VKAPI_CALL vkFreeMemory(VkDevice,VkDeviceMemory,const VkAllocationCallbacks*) {}
}
int main() try
{
    using namespace dyf;
    RHI::TextureDesc desc;desc.width=desc.height=4;desc.depthOrArraySize=2;desc.mipLevels=2;
    desc.format=RHI::Format::R8G8B8A8_UNORM;desc.usage=RHI::TextureUsage::ShaderResource;
    Backends::VulkanContext context;context.device=reinterpret_cast<VkDevice>(uintptr_t{1});
    context.physicalDevice=reinterpret_cast<VkPhysicalDevice>(uintptr_t{1});
    std::unique_ptr<Backends::VulkanTexture,Backends::VulkanObjectDeleter> array(new Backends::VulkanTexture(context,desc));
    const RHI::TextureSubresourceRange layer={0,1,1,1};
    const auto view=array->GetResourceView(layer,true);
    Check(view && lastView.viewType==VK_IMAGE_VIEW_TYPE_2D_ARRAY,"One layer of an array lost its array shader dimension");
    Check(lastView.subresourceRange.baseArrayLayer==1 && lastView.subresourceRange.layerCount==1,"Layer range changed");
    Check(array->GetResourceView(layer,true)==view && createdViews==2,"Repeated view was not cached");
    desc.depthOrArraySize=1;
    std::unique_ptr<Backends::VulkanTexture,Backends::VulkanObjectDeleter> single(new Backends::VulkanTexture(context,desc));
    Check(single->GetResourceView({1,1,0,1},true) && lastView.viewType==VK_IMAGE_VIEW_TYPE_2D,"Single-layer resource became an array");
    std::puts("PASS texture subviews preserve resource shader dimension and exact subresource range");
    return 0;
}
catch(const std::exception& error) {std::fprintf(stderr,"FAIL VulkanTextureView: %s\n",error.what());return 1;}
