#pragma once

#include "RHI/Buffer.h"
#include "RHI/ResourceState.h"
#include "RHI/Texture.h"
#include "VulkanContext.h"

#include <vector>

namespace dy::Backends
{
	struct VulkanObjectDeleter;

	class VulkanBuffer final : public dy::RHI::Buffer
	{
	public:
		VulkanBuffer(const VulkanContext& context, const dy::RHI::BufferDesc& desc);

		[[nodiscard]] VkBuffer GetHandle() const { return m_buffer; }
		[[nodiscard]] bool IsStateAllowed(dy::RHI::ResourceState state) const;
		[[nodiscard]] dy::RHI::ResourceState GetState() const { return m_state; }
		void SetState(dy::RHI::ResourceState state) { m_state = state; }

	private:
		friend struct VulkanObjectDeleter;

		~VulkanBuffer() override;

		VkDevice m_device = VK_NULL_HANDLE;
		VkBuffer m_buffer = VK_NULL_HANDLE;
		VkDeviceMemory m_memory = VK_NULL_HANDLE;
		dy::RHI::ResourceState m_state = dy::RHI::ResourceState::Undefined;
	};

	class VulkanTexture final : public dy::RHI::Texture
	{
	public:
		VulkanTexture(const VulkanContext& context, const dy::RHI::TextureDesc& desc);
		VulkanTexture(const dy::RHI::TextureDesc& desc, VkImage image, VkImageView imageView);

		[[nodiscard]] VkImage GetImage() const { return m_image; }
		[[nodiscard]] VkImageView GetImageView() const { return m_imageView; }
		[[nodiscard]] VkImageAspectFlags GetAspectMask() const { return m_aspectMask; }
		[[nodiscard]] bool IsSwapchainImage() const { return !m_ownsImage; }
		[[nodiscard]] bool IsStateAllowed(dy::RHI::ResourceState state) const;
		[[nodiscard]] dy::RHI::ResourceState GetState(uint32_t mipLevel, uint32_t arrayLayer) const;
		void SetState(uint32_t mipLevel, uint32_t arrayLayer, dy::RHI::ResourceState state);
		[[nodiscard]] VkImageView GetSubresourceView(uint32_t mipLevel, uint32_t arrayLayer);
		[[nodiscard]] VkImageView GetResourceView(const dy::RHI::TextureSubresourceRange& subresources);
		[[nodiscard]] VkImageLayout GetBarrierOldLayout(dy::RHI::ResourceState state) const;
		void MarkPresented() { m_hasPresented = true; }

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
			dy::RHI::TextureSubresourceRange subresources = {};
			VkImageView view = VK_NULL_HANDLE;
		};
		std::vector<ResourceView> m_resourceViews;
		std::vector<dy::RHI::ResourceState> m_states;
		bool m_ownsImage = false;
		bool m_hasPresented = false;
	};

	[[nodiscard]] VkFormat ToVulkanFormat(dy::RHI::Format format);
	[[nodiscard]] dy::RHI::Format FromVulkanColorFormat(VkFormat format);
	[[nodiscard]] VkImageLayout ToVulkanImageLayout(dy::RHI::ResourceState state);
}
