#include "VulkanSwapchain.h"
#include "VulkanResources.h"
#include <GLFW/glfw3.h>
#include <algorithm>
#include <limits>
#include <stdexcept>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace dy::Backends
{

bool VulkanSwapchain::Initialize(const VulkanContext& context, void* windowHandle, RHI::Format requestedFormat) {
    SwapchainSupportDetails swapchainSupport = QuerySwapchainSupport(context.physicalDevice, context.surface);

    VkSurfaceFormatKHR surfaceFormat{};
    RHI::Format actualFormat = RHI::Format::Unknown;
    if (!ChooseSwapSurfaceFormat(swapchainSupport.formats, requestedFormat, surfaceFormat, actualFormat)) {
        return false;
    }
    VkPresentModeKHR presentMode = ChoosePresentMode(swapchainSupport.presentModes);
    VkExtent2D extent = ChooseSwapExtent(swapchainSupport.capabilities, windowHandle);

    uint32_t imageCount = swapchainSupport.capabilities.minImageCount + 1;
    if (swapchainSupport.capabilities.maxImageCount > 0 && imageCount > swapchainSupport.capabilities.maxImageCount) {
        imageCount = swapchainSupport.capabilities.maxImageCount;
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

    uint32_t queueFamilyIndices[] = { context.queueIndices.graphicsFamily, context.queueIndices.presentFamily };

    if (context.queueIndices.graphicsFamily != context.queueIndices.presentFamily) {
        createInfo.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
        createInfo.queueFamilyIndexCount = 2;
        createInfo.pQueueFamilyIndices = queueFamilyIndices;
    } else {
        createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    }

    createInfo.preTransform = swapchainSupport.capabilities.currentTransform;
    createInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    createInfo.presentMode = presentMode;
    createInfo.clipped = VK_TRUE;

    if (vkCreateSwapchainKHR(context.device, &createInfo, nullptr, &m_swapchain) != VK_SUCCESS) {
        return false;
    }

    vkGetSwapchainImagesKHR(context.device, m_swapchain, &imageCount, nullptr);
    m_swapchainImages.resize(imageCount);
    vkGetSwapchainImagesKHR(context.device, m_swapchain, &imageCount, m_swapchainImages.data());

    m_swapchainImageFormat = surfaceFormat.format;
    m_format = actualFormat;
    m_swapchainExtent = extent;

    try {
        m_swapchainImageViews.resize(m_swapchainImages.size());
        for (size_t i = 0; i < m_swapchainImages.size(); i++) {
            m_swapchainImageViews[i] = VulkanResources::CreateImageView(context.device, m_swapchainImages[i], m_swapchainImageFormat);
        }
    } catch (const std::exception&) {
        Cleanup(context.device);
        return false;
    }

    return true;
}

void VulkanSwapchain::Cleanup(VkDevice device) {
    for (auto imageView : m_swapchainImageViews) {
        vkDestroyImageView(device, imageView, nullptr);
    }
    m_swapchainImageViews.clear();

    if (m_swapchain != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(device, m_swapchain, nullptr);
        m_swapchain = VK_NULL_HANDLE;
    }
    m_swapchainImages.clear();
    m_swapchainImageFormat = VK_FORMAT_UNDEFINED;
    m_format = RHI::Format::Unknown;
    m_swapchainExtent = {};
}

VulkanSwapchain::SwapchainSupportDetails VulkanSwapchain::QuerySwapchainSupport(VkPhysicalDevice device, VkSurfaceKHR surface) {
    SwapchainSupportDetails details;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device, surface, &details.capabilities);

    uint32_t formatCount;
    vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface, &formatCount, nullptr);
    if (formatCount != 0) {
        details.formats.resize(formatCount);
        vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface, &formatCount, details.formats.data());
    }

    uint32_t presentModeCount;
    vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface, &presentModeCount, nullptr);
    if (presentModeCount != 0) {
        details.presentModes.resize(presentModeCount);
        vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface, &presentModeCount, details.presentModes.data());
    }

    return details;
}

bool VulkanSwapchain::ChooseSwapSurfaceFormat(
    const std::vector<VkSurfaceFormatKHR>& availableFormats,
    RHI::Format requestedFormat,
    VkSurfaceFormatKHR& selectedFormat,
    RHI::Format& actualFormat) {
    if (availableFormats.empty()) return false;

    const bool acceptsAnyFormat = availableFormats.size() == 1 && availableFormats[0].format == VK_FORMAT_UNDEFINED;
    if (requestedFormat != RHI::Format::Unknown) {
        VkFormat requestedVkFormat = VK_FORMAT_UNDEFINED;
        switch(requestedFormat) {
        case RHI::Format::R8G8B8A8_UNORM: requestedVkFormat = VK_FORMAT_R8G8B8A8_UNORM; break;
        case RHI::Format::B8G8R8A8_UNORM: requestedVkFormat = VK_FORMAT_B8G8R8A8_UNORM; break;
        case RHI::Format::R8G8B8A8_UNORM_SRGB: requestedVkFormat = VK_FORMAT_R8G8B8A8_SRGB; break;
        case RHI::Format::B8G8R8A8_UNORM_SRGB: requestedVkFormat = VK_FORMAT_B8G8R8A8_SRGB; break;
        case RHI::Format::R16G16B16A16_FLOAT: requestedVkFormat = VK_FORMAT_R16G16B16A16_SFLOAT; break;
        case RHI::Format::R32G32B32A32_FLOAT: requestedVkFormat = VK_FORMAT_R32G32B32A32_SFLOAT; break;
        case RHI::Format::Unknown:
        case RHI::Format::D32_FLOAT:
        case RHI::Format::D24_UNORM_S8_UINT:
        case RHI::Format::R32_UINT:
        case RHI::Format::R16_UINT:
        default:
            return false;
        }

        if (acceptsAnyFormat) {
            selectedFormat = { requestedVkFormat, availableFormats[0].colorSpace };
            actualFormat = requestedFormat;
            return true;
        }

        for (const VkSurfaceFormatKHR& availableFormat : availableFormats) {
            if (availableFormat.format == requestedVkFormat) {
                selectedFormat = availableFormat;
                actualFormat = requestedFormat;
                return true;
            }
        }
        return false;
    }

    if (acceptsAnyFormat) {
        selectedFormat = { VK_FORMAT_B8G8R8A8_UNORM, availableFormats[0].colorSpace };
        actualFormat = RHI::Format::B8G8R8A8_UNORM;
        return true;
    }

    // Native 선택도 RHI가 정확히 보고할 수 있는 포맷으로 제한하며 수동 감마 경로인 UNORM을 우선한다.
    for (const VkSurfaceFormatKHR& availableFormat : availableFormats) {
        if (availableFormat.format == VK_FORMAT_B8G8R8A8_UNORM) {
            actualFormat = RHI::Format::B8G8R8A8_UNORM;
            selectedFormat = availableFormat;
            return true;
        }
        if (availableFormat.format == VK_FORMAT_R8G8B8A8_UNORM) {
            actualFormat = RHI::Format::R8G8B8A8_UNORM;
            selectedFormat = availableFormat;
            return true;
        }
    }

    for (const VkSurfaceFormatKHR& availableFormat : availableFormats) {
        switch(availableFormat.format) {
        case VK_FORMAT_B8G8R8A8_SRGB: actualFormat = RHI::Format::B8G8R8A8_UNORM_SRGB; break;
        case VK_FORMAT_R8G8B8A8_SRGB: actualFormat = RHI::Format::R8G8B8A8_UNORM_SRGB; break;
        case VK_FORMAT_R16G16B16A16_SFLOAT: actualFormat = RHI::Format::R16G16B16A16_FLOAT; break;
        case VK_FORMAT_R32G32B32A32_SFLOAT: actualFormat = RHI::Format::R32G32B32A32_FLOAT; break;
        default: continue;
        }
        selectedFormat = availableFormat;
        return true;
    }
    return false;
}

VkPresentModeKHR VulkanSwapchain::ChoosePresentMode(const std::vector<VkPresentModeKHR>& availablePresentModes) {
    for (const auto& availablePresentMode : availablePresentModes) {
        if (availablePresentMode == VK_PRESENT_MODE_MAILBOX_KHR) {
            return availablePresentMode;
        }
    }
    return VK_PRESENT_MODE_FIFO_KHR;
}

VkExtent2D VulkanSwapchain::ChooseSwapExtent(const VkSurfaceCapabilitiesKHR& capabilities, void* windowHandle) {
    if (capabilities.currentExtent.width != std::numeric_limits<uint32_t>::max()) {
        return capabilities.currentExtent;
    } else {
        int width, height;
#if defined(_WIN32)
        RECT clientRect = {};
        const HWND hwnd = static_cast<HWND>(windowHandle);
        if (hwnd != nullptr && GetClientRect(hwnd, &clientRect)) {
            width = clientRect.right - clientRect.left;
            height = clientRect.bottom - clientRect.top;
        } else {
            width = 1;
            height = 1;
        }
#else
        glfwGetFramebufferSize(static_cast<GLFWwindow*>(windowHandle), &width, &height);
#endif

        VkExtent2D actualExtent = {
            static_cast<uint32_t>(width),
            static_cast<uint32_t>(height)
        };

        actualExtent.width = std::clamp(actualExtent.width, capabilities.minImageExtent.width, capabilities.maxImageExtent.width);
        actualExtent.height = std::clamp(actualExtent.height, capabilities.minImageExtent.height, capabilities.maxImageExtent.height);

        return actualExtent;
    }
}

}
