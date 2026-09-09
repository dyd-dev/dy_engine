#pragma once
#include "VulkanContext.h"
#include <cstdint>
#include <vector>

namespace dy::Backends
{

class VulkanSwapchain {
public:
    VulkanSwapchain() = default;
    VulkanSwapchain(const VulkanSwapchain&) = delete;
    VulkanSwapchain& operator=(const VulkanSwapchain&) = delete;
    VulkanSwapchain(VulkanSwapchain&& other) noexcept;
    VulkanSwapchain& operator=(VulkanSwapchain&& other) noexcept;

    struct SwapchainSupportDetails {
        VkSurfaceCapabilitiesKHR capabilities;
        std::vector<VkSurfaceFormatKHR> formats;
        std::vector<VkPresentModeKHR> presentModes;
    };

    bool Initialize(
        const VulkanContext& context,
        void* windowHandle,
        VkFormat requestedFormat,
        VkPresentModeKHR requestedPresentMode,
        uint32_t requestedMinimumImageCount,
        bool allowReadback,
        uint32_t initialWidth,
        uint32_t initialHeight,
        VkSwapchainKHR oldSwapchain,
        bool& oldSwapchainRetired);
    void Cleanup(VkDevice device);

    VkSwapchainKHR GetHandle() const { return m_swapchain; }
    VkFormat GetImageFormat() const { return m_swapchainImageFormat; }
    VkExtent2D GetExtent() const { return m_swapchainExtent; }
	const std::vector<VkImage>& GetImages() const { return m_swapchainImages; }
    const std::vector<VkImageView>& GetImageViews() const { return m_swapchainImageViews; }
    size_t GetImageCount() const { return m_swapchainImages.size(); }

    static SwapchainSupportDetails QuerySwapchainSupport(VkPhysicalDevice device, VkSurfaceKHR surface);

private:
    static bool ChooseSwapSurfaceFormat(
        const std::vector<VkSurfaceFormatKHR>& availableFormats,
        VkFormat requestedFormat,
        VkSurfaceFormatKHR& selectedFormat);
    static bool ChoosePresentMode(
        const std::vector<VkPresentModeKHR>& availablePresentModes,
        VkPresentModeKHR requestedPresentMode,
        VkPresentModeKHR& selectedPresentMode);
    static bool ChooseSwapExtent(
        const VkSurfaceCapabilitiesKHR& capabilities,
        void* windowHandle,
        VkExtent2D& selectedExtent);

    VkSwapchainKHR m_swapchain = VK_NULL_HANDLE;
    VkFormat m_swapchainImageFormat = VK_FORMAT_UNDEFINED;
    VkExtent2D m_swapchainExtent = {};
    std::vector<VkImage> m_swapchainImages;
    std::vector<VkImageView> m_swapchainImageViews;
};

}
