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

namespace dy::Backends
{
namespace
{
    VkImageView CreateImageView(VkDevice device, VkImage image, VkFormat format)
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
        VkImageView view = VK_NULL_HANDLE;
        if (vkCreateImageView(device, &info, nullptr, &view) != VK_SUCCESS) {
            throw std::runtime_error("Failed to create Vulkan swapchain image view");
        }
        return view;
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
    VkPresentModeKHR requestedPresentMode,
    uint32_t requestedMinimumImageCount,
    bool allowReadback,
    uint32_t initialWidth,
    uint32_t initialHeight,
    VkSwapchainKHR oldSwapchain,
    bool& oldSwapchainRetired)
{
    oldSwapchainRetired = false;
    if (context.device == VK_NULL_HANDLE || context.physicalDevice == VK_NULL_HANDLE || context.surface == VK_NULL_HANDLE) {
        return false;
    }

    SwapchainSupportDetails swapchainSupport = QuerySwapchainSupport(context.physicalDevice, context.surface);
    if (swapchainSupport.formats.empty() || swapchainSupport.presentModes.empty()) return false;

    VkSurfaceFormatKHR surfaceFormat{};
    VkPresentModeKHR presentMode = VK_PRESENT_MODE_FIFO_KHR;
    VkExtent2D extent{};
    if (!ChooseSwapSurfaceFormat(swapchainSupport.formats, requestedFormat, surfaceFormat) ||
        !ChoosePresentMode(swapchainSupport.presentModes, requestedPresentMode, presentMode) ||
        !ChooseSwapExtent(swapchainSupport.capabilities, windowHandle, extent)) {
        return false;
    }

    uint32_t imageCount = std::max(requestedMinimumImageCount, swapchainSupport.capabilities.minImageCount);
    if((initialWidth && initialWidth != extent.width) || (initialHeight && initialHeight != extent.height))
        return false;
    if (swapchainSupport.capabilities.maxImageCount > 0 && imageCount > swapchainSupport.capabilities.maxImageCount) return false;
    const uint32_t minimumImageCount = imageCount;

    VkCompositeAlphaFlagBitsKHR compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    if ((swapchainSupport.capabilities.supportedCompositeAlpha & compositeAlpha) == 0) {
        if ((swapchainSupport.capabilities.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR) != 0) {
            compositeAlpha = VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR;
        } else if ((swapchainSupport.capabilities.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR) != 0) {
            compositeAlpha = VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR;
        } else if ((swapchainSupport.capabilities.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR) != 0) {
            compositeAlpha = VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
        } else {
            return false;
        }
    }

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
    if (vkCreateSwapchainKHR(context.device, &createInfo, nullptr, &swapchain) != VK_SUCCESS) return false;

    if (vkGetSwapchainImagesKHR(context.device, swapchain, &imageCount, nullptr) != VK_SUCCESS || imageCount < minimumImageCount) {
        vkDestroySwapchainKHR(context.device, swapchain, nullptr);
        return false;
    }

    std::vector<VkImage> images(imageCount);
    if (vkGetSwapchainImagesKHR(context.device, swapchain, &imageCount, images.data()) != VK_SUCCESS) {
        vkDestroySwapchainKHR(context.device, swapchain, nullptr);
        return false;
    }
    images.resize(imageCount);

    std::vector<VkImageView> imageViews;
    imageViews.reserve(images.size());
    try {
        for (VkImage image : images) {
            imageViews.push_back(CreateImageView(context.device, image, surfaceFormat.format));
        }
    } catch (...) {
        for (VkImageView imageView : imageViews) vkDestroyImageView(context.device, imageView, nullptr);
        vkDestroySwapchainKHR(context.device, swapchain, nullptr);
        return false;
    }

    m_swapchain = swapchain;
    m_swapchainImages = std::move(images);
    m_swapchainImageViews = std::move(imageViews);
    m_swapchainImageFormat = surfaceFormat.format;
    m_swapchainExtent = extent;
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

VulkanSwapchain::SwapchainSupportDetails VulkanSwapchain::QuerySwapchainSupport(VkPhysicalDevice device, VkSurfaceKHR surface) {
    SwapchainSupportDetails details{};
    if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device, surface, &details.capabilities) != VK_SUCCESS) return details;

    uint32_t formatCount = 0;
    if (vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface, &formatCount, nullptr) != VK_SUCCESS) return details;
    if (formatCount != 0) {
        details.formats.resize(formatCount);
        if (vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface, &formatCount, details.formats.data()) != VK_SUCCESS) {
            details.formats.clear();
            return details;
        }
        details.formats.resize(formatCount);
    }

    uint32_t presentModeCount = 0;
    if (vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface, &presentModeCount, nullptr) != VK_SUCCESS) return details;
    if (presentModeCount != 0) {
        details.presentModes.resize(presentModeCount);
        if (vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface, &presentModeCount, details.presentModes.data()) != VK_SUCCESS) {
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
    VkSurfaceFormatKHR& selectedFormat)
{
    if (availableFormats.empty()) return false;
    if (requestedFormat == VK_FORMAT_UNDEFINED) {
        if (availableFormats.size() == 1 && availableFormats.front().format == VK_FORMAT_UNDEFINED) {
            selectedFormat = availableFormats.front();
            selectedFormat.format = VK_FORMAT_B8G8R8A8_UNORM;
            return true;
        }

        const auto it = std::find_if(availableFormats.begin(), availableFormats.end(), [](const VkSurfaceFormatKHR& format) {
            return format.format == VK_FORMAT_R8G8B8A8_UNORM ||
                format.format == VK_FORMAT_B8G8R8A8_UNORM ||
                format.format == VK_FORMAT_R8G8B8A8_SRGB ||
                format.format == VK_FORMAT_B8G8R8A8_SRGB ||
                format.format == VK_FORMAT_R16G16B16A16_SFLOAT ||
                format.format == VK_FORMAT_R32G32B32A32_SFLOAT;
        });
        if (it == availableFormats.end()) return false;
        selectedFormat = *it;
        return true;
    }

    if (availableFormats.size() == 1 && availableFormats.front().format == VK_FORMAT_UNDEFINED) {
        selectedFormat = availableFormats.front();
        selectedFormat.format = requestedFormat;
        return true;
    }

    const auto it = std::find_if(availableFormats.begin(), availableFormats.end(), [requestedFormat](const VkSurfaceFormatKHR& format) {
        return format.format == requestedFormat;
    });
    if (it == availableFormats.end()) return false;
    selectedFormat = *it;
    return true;
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
    VkExtent2D& selectedExtent)
{
    if (capabilities.currentExtent.width != std::numeric_limits<uint32_t>::max()) {
        selectedExtent = capabilities.currentExtent;
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
        if (width <= 0 || height <= 0) return false;

        selectedExtent = {
            static_cast<uint32_t>(width),
            static_cast<uint32_t>(height)
        };

        selectedExtent.width = std::clamp(selectedExtent.width, capabilities.minImageExtent.width, capabilities.maxImageExtent.width);
        selectedExtent.height = std::clamp(selectedExtent.height, capabilities.minImageExtent.height, capabilities.maxImageExtent.height);

        return selectedExtent.width > 0 && selectedExtent.height > 0;
    }
}

}
