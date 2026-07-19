#pragma once
#include "RHI/Format.h"
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

    [[nodiscard]] bool Initialize(const VulkanContext& context, void* windowHandle, RHI::Format requestedFormat);
    void Cleanup(VkDevice device);

    VkSwapchainKHR GetHandle() const { return m_swapchain; }
    VkFormat GetImageFormat() const { return m_swapchainImageFormat; }
    RHI::Format GetFormat() const { return m_format; }
    VkExtent2D GetExtent() const { return m_swapchainExtent; }
    const std::vector<VkImageView>& GetImageViews() const { return m_swapchainImageViews; }
    size_t GetImageCount() const { return m_swapchainImages.size(); }

    static SwapchainSupportDetails QuerySwapchainSupport(VkPhysicalDevice device, VkSurfaceKHR surface);

private:
    bool ChooseSwapSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& availableFormats, RHI::Format requestedFormat, VkSurfaceFormatKHR& selectedFormat, RHI::Format& actualFormat);
    VkPresentModeKHR ChoosePresentMode(const std::vector<VkPresentModeKHR>& availablePresentModes);
    VkExtent2D ChooseSwapExtent(const VkSurfaceCapabilitiesKHR& capabilities, void* windowHandle);

    VkSwapchainKHR m_swapchain = VK_NULL_HANDLE;
    VkFormat m_swapchainImageFormat = VK_FORMAT_UNDEFINED;
    RHI::Format m_format = RHI::Format::Unknown;
    VkExtent2D m_swapchainExtent = {};
    std::vector<VkImage> m_swapchainImages;
    std::vector<VkImageView> m_swapchainImageViews;
};

}
