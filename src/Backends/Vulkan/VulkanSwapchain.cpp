#include "VulkanSwapchain.h"
#include <GLFW/glfw3.h>
#include <algorithm>
#include <limits>
#include <utility>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace dy::Backends
{

VkResult VulkanSwapchain::Initialize(const VulkanContext& context, void* windowHandle, bool preferSrgb) {
	SwapchainSupportDetails swapchainSupport = QuerySwapchainSupport(context.physicalDevice, context.surface);
	if(!swapchainSupport.querySucceeded || swapchainSupport.formats.empty() || swapchainSupport.presentModes.empty()) {
		return swapchainSupport.result != VK_SUCCESS ? swapchainSupport.result : VK_ERROR_INITIALIZATION_FAILED;
	}

    VkSurfaceFormatKHR surfaceFormat = ChooseSwapSurfaceFormat(swapchainSupport.formats, preferSrgb);
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

	VkSwapchainKHR newSwapchain = VK_NULL_HANDLE;
	VkResult result = vkCreateSwapchainKHR(context.device, &createInfo, nullptr, &newSwapchain);
	if(result != VK_SUCCESS) return result;

	result = vkGetSwapchainImagesKHR(context.device, newSwapchain, &imageCount, nullptr);
	if(result != VK_SUCCESS || imageCount == 0u) {
		vkDestroySwapchainKHR(context.device, newSwapchain, nullptr);
		return result != VK_SUCCESS ? result : VK_ERROR_INITIALIZATION_FAILED;
	}
	std::vector<VkImage> newImages(imageCount);
	result = vkGetSwapchainImagesKHR(context.device, newSwapchain, &imageCount, newImages.data());
	if(result != VK_SUCCESS) {
		vkDestroySwapchainKHR(context.device, newSwapchain, nullptr);
		return result;
	}

	std::vector<VkImageView> newImageViews(newImages.size(), VK_NULL_HANDLE);
	for(size_t imageIndex = 0u; imageIndex < newImages.size(); ++imageIndex) {
		VkImageViewCreateInfo viewInfo{};
		viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
		viewInfo.image = newImages[imageIndex];
		viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
		viewInfo.format = surfaceFormat.format;
		viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		viewInfo.subresourceRange.baseMipLevel = 0u;
		viewInfo.subresourceRange.levelCount = 1u;
		viewInfo.subresourceRange.baseArrayLayer = 0u;
		viewInfo.subresourceRange.layerCount = 1u;
		result = vkCreateImageView(context.device, &viewInfo, nullptr, &newImageViews[imageIndex]);
		if(result != VK_SUCCESS) {
			for(VkImageView imageView : newImageViews)
				if(imageView != VK_NULL_HANDLE) vkDestroyImageView(context.device, imageView, nullptr);
			vkDestroySwapchainKHR(context.device, newSwapchain, nullptr);
			return result;
		}
    }

	Cleanup(context.device);
	m_swapchain = newSwapchain;
	m_swapchainImages = std::move(newImages);
	m_swapchainImageViews = std::move(newImageViews);
	m_swapchainImageFormat = surfaceFormat.format;
	m_swapchainExtent = extent;
	return VK_SUCCESS;
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
	m_swapchainExtent = {};
}

VulkanSwapchain::SwapchainSupportDetails VulkanSwapchain::QuerySwapchainSupport(VkPhysicalDevice device, VkSurfaceKHR surface) {
    SwapchainSupportDetails details;
	details.result = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device, surface, &details.capabilities);
	if(details.result != VK_SUCCESS) return details;

	uint32_t formatCount = 0u;
	details.result = vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface, &formatCount, nullptr);
	if(details.result != VK_SUCCESS) return details;
    if (formatCount != 0) {
        details.formats.resize(formatCount);
		details.result = vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface, &formatCount, details.formats.data());
		if(details.result != VK_SUCCESS) {
			details.formats.clear();
			return details;
		}
    }

	uint32_t presentModeCount = 0u;
	details.result = vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface, &presentModeCount, nullptr);
	if(details.result != VK_SUCCESS) return details;
    if (presentModeCount != 0) {
        details.presentModes.resize(presentModeCount);
		details.result = vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface, &presentModeCount, details.presentModes.data());
		if(details.result != VK_SUCCESS) {
			details.presentModes.clear();
			return details;
		}
    }

	details.querySucceeded = true;
	details.result = VK_SUCCESS;
    return details;
}

VkSurfaceFormatKHR VulkanSwapchain::ChooseSwapSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& availableFormats, bool preferSrgb) {
    // 채널 순서(BGRA vs RGBA)는 색 정확도와 무관(셰이더 RGBA 출력을 포맷이 매핑)하므로
    // sRGB 여부만 기준으로 고른다. preferSrgb=false → UNORM(셰이더 수동 감마)을 우선.
    auto isSrgb = [](VkFormat f) {
        return f == VK_FORMAT_B8G8R8A8_SRGB || f == VK_FORMAT_R8G8B8A8_SRGB;
    };
    for (const auto& availableFormat : availableFormats) {
        if (availableFormat.colorSpace != VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) continue;
        if (isSrgb(availableFormat.format) == preferSrgb) {
            return availableFormat;
        }
    }
    return availableFormats[0];
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
