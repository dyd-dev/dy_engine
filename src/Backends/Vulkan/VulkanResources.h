#pragma once
#include <mutex>

#include "dyf/RHI/Buffer.h"
#include "dyf/RHI/ResourceState.h"
#include "dyf/RHI/Texture.h"
#include "VulkanContext.h"

#include <vector>

namespace dyf::Backends
{
	struct VulkanObjectDeleter;

	class VulkanBuffer final : public dyf::RHI::Buffer
	{
	public:
		VulkanBuffer(const VulkanContext& context, const dyf::RHI::BufferDesc& desc);

		[[nodiscard]] VkBuffer GetHandle() const { return m_buffer; }
		[[nodiscard]] bool IsStateAllowed(dyf::RHI::ResourceState state) const;
		[[nodiscard]] dyf::RHI::ResourceState GetState() const { return m_state; }
		void SetState(dyf::RHI::ResourceState state) { m_state = state; }

	private:
		friend struct VulkanObjectDeleter;

		~VulkanBuffer() override;

		VkDevice m_device = VK_NULL_HANDLE;
		VkBuffer m_buffer = VK_NULL_HANDLE;
		VkDeviceMemory m_memory = VK_NULL_HANDLE;
		dyf::RHI::ResourceState m_state = dyf::RHI::ResourceState::Undefined;
	};

	class VulkanTexture final : public dyf::RHI::Texture
	{
	public:
		VulkanTexture(const VulkanContext& context, const dyf::RHI::TextureDesc& desc);
		VulkanTexture(const dyf::RHI::TextureDesc& desc, VkImage image, VkImageView imageView);

		[[nodiscard]] VkImage GetImage() const { return m_image; }
		[[nodiscard]] VkImageView GetImageView() const { return m_imageView; }
		[[nodiscard]] VkImageAspectFlags GetAspectMask() const { return m_aspectMask; }
		[[nodiscard]] bool IsSwapchainImage() const { return !m_ownsImage; }
		[[nodiscard]] bool IsStateAllowed(dyf::RHI::ResourceState state) const;
		[[nodiscard]] dyf::RHI::ResourceState GetState(uint32_t mipLevel, uint32_t arrayLayer) const;
		void SetState(uint32_t mipLevel, uint32_t arrayLayer, dyf::RHI::ResourceState state);
		[[nodiscard]] VkImageView GetSubresourceView(uint32_t mipLevel, uint32_t arrayLayer);
		[[nodiscard]] VkImageView GetResourceView(const dyf::RHI::TextureSubresourceRange& subresources, bool sampled = false);
		[[nodiscard]] VkImageLayout GetBarrierOldLayout(dyf::RHI::ResourceState state) const;
		void MarkLayoutInitialized() { m_hasKnownLayout = true; }

	private:
		friend struct VulkanObjectDeleter;

		~VulkanTexture() override;

		VkDevice m_device = VK_NULL_HANDLE;
		VkImage m_image = VK_NULL_HANDLE;
		VkDeviceMemory m_memory = VK_NULL_HANDLE;
		VkImageView m_imageView = VK_NULL_HANDLE;
		VkImageAspectFlags m_aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		struct ResourceView
		{
			dyf::RHI::TextureSubresourceRange subresources = {};
			VkImageAspectFlags aspectMask = 0;
			VkImageView view = VK_NULL_HANDLE;
		};
		std::vector<ResourceView> m_resourceViews;
        std::mutex m_viewMutex;
		std::vector<dyf::RHI::ResourceState> m_states;
		bool m_ownsImage = false;
		bool m_hasKnownLayout = false;
	};

	[[nodiscard]] VkFormat ToVulkanFormat(dyf::RHI::Format format);
	[[nodiscard]] dyf::RHI::Format FromVulkanColorFormat(VkFormat format);
	[[nodiscard]] VkImageLayout ToVulkanImageLayout(dyf::RHI::ResourceState state);
}
