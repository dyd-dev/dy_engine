#include "VulkanSwapchain.h"
#include <GLFW/glfw3.h>
#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace dyf::Backends
{
namespace
{
    VkResult CreateImageView(VkDevice device, VkImage image, VkFormat format, VkImageView& view)
    {
        VkImageViewCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        info.image = image;
        info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        info.format = format;
        info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        info.subresourceRange.baseMipLevel = 0;
        info.subresourceRange.levelCount = 1;
        info.subresourceRange.baseArrayLayer = 0;
        info.subresourceRange.layerCount = 1;
        return vkCreateImageView(device, &info, nullptr, &view);
    }
}

VulkanSwapchain::VulkanSwapchain(VulkanSwapchain&& other) noexcept
{
    *this = std::move(other);
}

VulkanSwapchain& VulkanSwapchain::operator=(VulkanSwapchain&& other) noexcept
{
    if (this == &other) return *this;
    m_swapchain = other.m_swapchain;
    m_swapchainImageFormat = other.m_swapchainImageFormat;
    m_swapchainExtent = other.m_swapchainExtent;
    m_swapchainImages = std::move(other.m_swapchainImages);
    m_swapchainImageViews = std::move(other.m_swapchainImageViews);

    other.m_swapchain = VK_NULL_HANDLE;
    other.m_swapchainImageFormat = VK_FORMAT_UNDEFINED;
    other.m_swapchainExtent = {};
    return *this;
}

bool VulkanSwapchain::Initialize(
    const VulkanContext& context,
    void* windowHandle,
    VkFormat requestedFormat,
    VkColorSpaceKHR requestedColorSpace,
    VkCompositeAlphaFlagBitsKHR requestedCompositeAlpha,
    VkPresentModeKHR requestedPresentMode,
    uint32_t requestedMinimumImageCount,
    bool allowReadback,
    uint32_t initialWidth,
    uint32_t initialHeight,
    VkSwapchainKHR oldSwapchain,
    bool& oldSwapchainRetired,
    InitializationStatus& status)
{
    oldSwapchainRetired = false;
    status = {};
    if (context.device == VK_NULL_HANDLE || context.physicalDevice == VK_NULL_HANDLE || context.surface == VK_NULL_HANDLE) {
        return false;
    }

    SwapchainSupportDetails swapchainSupport = QuerySwapchainSupport(context.physicalDevice, context.surface, status);
    if (status.result != VK_SUCCESS) return false;
    if (swapchainSupport.formats.empty() || swapchainSupport.presentModes.empty()) return false;

    VkSurfaceFormatKHR surfaceFormat{};
    VkPresentModeKHR presentMode = VK_PRESENT_MODE_FIFO_KHR;
    VkExtent2D extent{};
    if (!ChooseSwapSurfaceFormat(swapchainSupport.formats, requestedFormat, requestedColorSpace, surfaceFormat) ||
        !ChoosePresentMode(swapchainSupport.presentModes, requestedPresentMode, presentMode) ||
        !ChooseSwapExtent(swapchainSupport.capabilities, windowHandle, extent, status)) {
        return false;
    }

    uint32_t imageCount = std::max(requestedMinimumImageCount, swapchainSupport.capabilities.minImageCount);
    if((initialWidth && initialWidth != extent.width) || (initialHeight && initialHeight != extent.height))
        return false;
    if (swapchainSupport.capabilities.maxImageCount > 0 && imageCount > swapchainSupport.capabilities.maxImageCount) return false;
    const uint32_t minimumImageCount = imageCount;

    const VkCompositeAlphaFlagBitsKHR compositeAlpha = requestedCompositeAlpha;
    if ((swapchainSupport.capabilities.supportedCompositeAlpha & compositeAlpha) == 0) return false;
    VkSwapchainCreateInfoKHR createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    createInfo.surface = context.surface;
    createInfo.minImageCount = imageCount;
    createInfo.imageFormat = surfaceFormat.format;
    createInfo.imageColorSpace = surfaceFormat.colorSpace;
    createInfo.imageExtent = extent;
    createInfo.imageArrayLayers = 1;
    createInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if(allowReadback) createInfo.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if((swapchainSupport.capabilities.supportedUsageFlags & createInfo.imageUsage) != createInfo.imageUsage)
        return false;

    uint32_t queueFamilyIndices[] = { context.queueIndices.graphicsFamily, context.queueIndices.presentFamily };

    if (context.queueIndices.graphicsFamily != context.queueIndices.presentFamily) {
        createInfo.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
        createInfo.queueFamilyIndexCount = 2;
        createInfo.pQueueFamilyIndices = queueFamilyIndices;
    } else {
        createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    }

    createInfo.preTransform = swapchainSupport.capabilities.currentTransform;
    createInfo.compositeAlpha = compositeAlpha;
    createInfo.presentMode = presentMode;
    createInfo.clipped = VK_TRUE;
    createInfo.oldSwapchain = oldSwapchain;

    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    oldSwapchainRetired = oldSwapchain != VK_NULL_HANDLE;
    if (!status.Check(vkCreateSwapchainKHR(context.device, &createInfo, nullptr, &swapchain), "vkCreateSwapchainKHR")) return false;
    std::vector<VkImageView> imageViews;
    struct PendingSwapchain {
        VkDevice device;
        VkSwapchainKHR handle;
        std::vector<VkImageView>& views;
        ~PendingSwapchain() {
            if (handle == VK_NULL_HANDLE) return;
            for (VkImageView view : views) vkDestroyImageView(device, view, nullptr);
            vkDestroySwapchainKHR(device, handle, nullptr);
        }
    } pending{context.device, swapchain, imageViews};
    if (!status.Check(vkGetSwapchainImagesKHR(context.device, swapchain, &imageCount, nullptr), "vkGetSwapchainImagesKHR(count)") ||
        imageCount < minimumImageCount) return false;
    std::vector<VkImage> images(imageCount);
    if (!status.Check(vkGetSwapchainImagesKHR(context.device, swapchain, &imageCount, images.data()), "vkGetSwapchainImagesKHR(images)")) return false;
    if (imageCount < minimumImageCount) return false;
    images.resize(imageCount);
    imageViews.reserve(images.size());
    for (VkImage image : images) {
        VkImageView view = VK_NULL_HANDLE;
        if (!status.Check(CreateImageView(context.device, image, surfaceFormat.format, view), "vkCreateImageView(swapchain)")) return false;
        imageViews.push_back(view);
    }

    m_swapchain = swapchain;
    m_swapchainImages = std::move(images);
    m_swapchainImageViews = std::move(imageViews);
    m_swapchainImageFormat = surfaceFormat.format;
    m_swapchainExtent = extent;
    pending.handle = VK_NULL_HANDLE;
    return true;
}

void VulkanSwapchain::Cleanup(VkDevice device) {
    for (auto imageView : m_swapchainImageViews) {
        vkDestroyImageView(device, imageView, nullptr);
    }
    m_swapchainImageViews.clear();
    m_swapchainImages.clear();

    if (m_swapchain != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(device, m_swapchain, nullptr);
        m_swapchain = VK_NULL_HANDLE;
    }
    m_swapchainImageFormat = VK_FORMAT_UNDEFINED;
    m_swapchainExtent = {};
}

VulkanSwapchain::SwapchainSupportDetails VulkanSwapchain::QuerySwapchainSupport(VkPhysicalDevice device, VkSurfaceKHR surface, InitializationStatus& status) {
    SwapchainSupportDetails details{};
    if (!status.Check(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device, surface, &details.capabilities), "vkGetPhysicalDeviceSurfaceCapabilitiesKHR")) return details;

    uint32_t formatCount = 0;
    if (!status.Check(vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface, &formatCount, nullptr), "vkGetPhysicalDeviceSurfaceFormatsKHR(count)")) return details;
    if (formatCount != 0) {
        details.formats.resize(formatCount);
        if (!status.Check(vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface, &formatCount, details.formats.data()), "vkGetPhysicalDeviceSurfaceFormatsKHR(formats)")) {
            details.formats.clear();
            return details;
        }
        details.formats.resize(formatCount);
    }

    uint32_t presentModeCount = 0;
    if (!status.Check(vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface, &presentModeCount, nullptr), "vkGetPhysicalDeviceSurfacePresentModesKHR(count)")) return details;
    if (presentModeCount != 0) {
        details.presentModes.resize(presentModeCount);
        if (!status.Check(vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface, &presentModeCount, details.presentModes.data()), "vkGetPhysicalDeviceSurfacePresentModesKHR(modes)")) {
            details.presentModes.clear();
            return details;
        }
        details.presentModes.resize(presentModeCount);
    }

    return details;
}

bool VulkanSwapchain::ChooseSwapSurfaceFormat(
    const std::vector<VkSurfaceFormatKHR>& availableFormats,
    VkFormat requestedFormat,
    VkColorSpaceKHR requestedColorSpace,
    VkSurfaceFormatKHR& selectedFormat)
{
    if (requestedFormat == VK_FORMAT_UNDEFINED) return false;
    for (const auto& format : availableFormats)
    {
        if ((format.format == requestedFormat || format.format == VK_FORMAT_UNDEFINED) &&
            format.colorSpace == requestedColorSpace)
        {
            selectedFormat = {requestedFormat, requestedColorSpace};
            return true;
        }
    }
    return false;
}

bool VulkanSwapchain::ChoosePresentMode(
    const std::vector<VkPresentModeKHR>& availablePresentModes,
    VkPresentModeKHR requestedPresentMode,
    VkPresentModeKHR& selectedPresentMode)
{
    const auto it = std::find(availablePresentModes.begin(), availablePresentModes.end(), requestedPresentMode);
    if (it == availablePresentModes.end()) return false;
    selectedPresentMode = *it;
    return true;
}

bool VulkanSwapchain::ChooseSwapExtent(
    const VkSurfaceCapabilitiesKHR& capabilities,
    void* windowHandle,
    VkExtent2D& selectedExtent,
    InitializationStatus& status)
{
    if (capabilities.currentExtent.width != std::numeric_limits<uint32_t>::max()) {
        selectedExtent = capabilities.currentExtent;
        status.retryLater = selectedExtent.width == 0 || selectedExtent.height == 0;
        return selectedExtent.width > 0 && selectedExtent.height > 0;
    } else {
        int width = 0;
        int height = 0;
#if defined(_WIN32)
        RECT clientRect = {};
        const HWND hwnd = static_cast<HWND>(windowHandle);
        if (hwnd != nullptr && GetClientRect(hwnd, &clientRect)) {
            width = clientRect.right - clientRect.left;
            height = clientRect.bottom - clientRect.top;
        } else return false;
#else
        if (windowHandle == nullptr) return false;
        glfwGetFramebufferSize(static_cast<GLFWwindow*>(windowHandle), &width, &height);
#endif
        if (width <= 0 || height <= 0) {status.retryLater = true; return false;}

        selectedExtent = {
            static_cast<uint32_t>(width),
            static_cast<uint32_t>(height)
        };

        selectedExtent.width = std::clamp(selectedExtent.width, capabilities.minImageExtent.width, capabilities.maxImageExtent.width);
        selectedExtent.height = std::clamp(selectedExtent.height, capabilities.minImageExtent.height, capabilities.maxImageExtent.height);

        status.retryLater = selectedExtent.width == 0 || selectedExtent.height == 0;
        return !status.retryLater;
    }
}

}
