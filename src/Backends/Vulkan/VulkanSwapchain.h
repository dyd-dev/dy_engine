#pragma once
#include "VulkanContext.h"
#include <vector>

namespace dy::Backends
{

class VulkanSwapchain {
public:
    struct SwapchainSupportDetails {
        VkSurfaceCapabilitiesKHR capabilities;
        std::vector<VkSurfaceFormatKHR> formats;
        std::vector<VkPresentModeKHR> presentModes;
    };

    // preferSrgb: true 면 sRGB 서피스 포맷(하드웨어 감마), false 면 UNORM(셰이더 수동 감마)을 고른다.
    void Initialize(const VulkanContext& context, void* windowHandle, bool preferSrgb = false);
    void Cleanup(VkDevice device);

    VkSwapchainKHR GetHandle() const { return m_swapchain; }
    VkFormat GetImageFormat() const { return m_swapchainImageFormat; }
    VkExtent2D GetExtent() const { return m_swapchainExtent; }
    const std::vector<VkImageView>& GetImageViews() const { return m_swapchainImageViews; }
    size_t GetImageCount() const { return m_swapchainImages.size(); }

    static SwapchainSupportDetails QuerySwapchainSupport(VkPhysicalDevice device, VkSurfaceKHR surface);

private:
    VkSurfaceFormatKHR ChooseSwapSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& availableFormats, bool preferSrgb);
    VkPresentModeKHR ChoosePresentMode(const std::vector<VkPresentModeKHR>& availablePresentModes);
    VkExtent2D ChooseSwapExtent(const VkSurfaceCapabilitiesKHR& capabilities, void* windowHandle);

    VkSwapchainKHR m_swapchain = VK_NULL_HANDLE;
    VkFormat m_swapchainImageFormat;
    VkExtent2D m_swapchainExtent;
    std::vector<VkImage> m_swapchainImages;
    std::vector<VkImageView> m_swapchainImageViews;
};

}
