#include "VulkanResources.h"

#include <algorithm>
#include <stdexcept>

namespace dy::Backends
{
	namespace
	{
		bool HasUsage(dy::RHI::BufferUsage usage, dy::RHI::BufferUsage value)
		{
			return (usage & value) != dy::RHI::BufferUsage::None;
		}

		bool HasUsage(dy::RHI::TextureUsage usage, dy::RHI::TextureUsage value)
		{
			return (usage & value) != dy::RHI::TextureUsage::None;
		}

		uint32_t FindMemoryType(VkPhysicalDevice physicalDevice, uint32_t typeBits, VkMemoryPropertyFlags properties)
		{
			VkPhysicalDeviceMemoryProperties memoryProperties{};
			vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memoryProperties);
			for (uint32_t i = 0; i < memoryProperties.memoryTypeCount; ++i)
			{
				if ((typeBits & (1u << i)) != 0 &&
					(memoryProperties.memoryTypes[i].propertyFlags & properties) == properties)
				{
					return i;
				}
			}
			throw std::runtime_error("Vulkan memory type is unavailable");
		}

		VkBufferUsageFlags ToBufferUsage(dy::RHI::BufferUsage usage)
		{
			VkBufferUsageFlags result = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
			if (HasUsage(usage, dy::RHI::BufferUsage::Vertex)) result |= VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
			if (HasUsage(usage, dy::RHI::BufferUsage::Index)) result |= VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
			if (HasUsage(usage, dy::RHI::BufferUsage::Constant)) result |= VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
			if (HasUsage(usage, dy::RHI::BufferUsage::Storage)) result |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
			if (HasUsage(usage, dy::RHI::BufferUsage::Indirect)) result |= VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
			return result;
		}

		VkImageUsageFlags ToImageUsage(dy::RHI::TextureUsage usage)
		{
			VkImageUsageFlags result = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
			if (HasUsage(usage, dy::RHI::TextureUsage::ShaderResource)) result |= VK_IMAGE_USAGE_SAMPLED_BIT;
			if (HasUsage(usage, dy::RHI::TextureUsage::RenderTarget)) result |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
			if (HasUsage(usage, dy::RHI::TextureUsage::DepthStencil)) result |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
			if (HasUsage(usage, dy::RHI::TextureUsage::Storage)) result |= VK_IMAGE_USAGE_STORAGE_BIT;
			return result;
		}

	VkImageAspectFlags ToImageAspectMask(dy::RHI::Format format)
		{
			switch (format)
			{
			case dy::RHI::Format::D32_FLOAT:
				return VK_IMAGE_ASPECT_DEPTH_BIT;
			case dy::RHI::Format::D24_UNORM_S8_UINT:
				return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
			default:
				return VK_IMAGE_ASPECT_COLOR_BIT;
			}
		}
	}

	VkFormat ToVulkanFormat(dy::RHI::Format format)
	{
		switch (format)
		{
		case dy::RHI::Format::R8G8B8A8_UNORM: return VK_FORMAT_R8G8B8A8_UNORM;
		case dy::RHI::Format::B8G8R8A8_UNORM: return VK_FORMAT_B8G8R8A8_UNORM;
		case dy::RHI::Format::R8G8B8A8_UNORM_SRGB: return VK_FORMAT_R8G8B8A8_SRGB;
		case dy::RHI::Format::B8G8R8A8_UNORM_SRGB: return VK_FORMAT_B8G8R8A8_SRGB;
		case dy::RHI::Format::R16G16B16A16_FLOAT: return VK_FORMAT_R16G16B16A16_SFLOAT;
		case dy::RHI::Format::R32G32_FLOAT: return VK_FORMAT_R32G32_SFLOAT;
		case dy::RHI::Format::R32G32B32_FLOAT: return VK_FORMAT_R32G32B32_SFLOAT;
		case dy::RHI::Format::R32G32B32A32_FLOAT: return VK_FORMAT_R32G32B32A32_SFLOAT;
		case dy::RHI::Format::D32_FLOAT: return VK_FORMAT_D32_SFLOAT;
		case dy::RHI::Format::D24_UNORM_S8_UINT: return VK_FORMAT_D24_UNORM_S8_UINT;
		case dy::RHI::Format::R32_UINT: return VK_FORMAT_R32_UINT;
		case dy::RHI::Format::R16_UINT: return VK_FORMAT_R16_UINT;
		default: return VK_FORMAT_UNDEFINED;
		}
	}

	dy::RHI::Format FromVulkanColorFormat(VkFormat format)
	{
		switch (format)
		{
		case VK_FORMAT_R8G8B8A8_UNORM: return dy::RHI::Format::R8G8B8A8_UNORM;
		case VK_FORMAT_B8G8R8A8_UNORM: return dy::RHI::Format::B8G8R8A8_UNORM;
		case VK_FORMAT_R8G8B8A8_SRGB: return dy::RHI::Format::R8G8B8A8_UNORM_SRGB;
		case VK_FORMAT_B8G8R8A8_SRGB: return dy::RHI::Format::B8G8R8A8_UNORM_SRGB;
		case VK_FORMAT_R16G16B16A16_SFLOAT: return dy::RHI::Format::R16G16B16A16_FLOAT;
		case VK_FORMAT_R32G32B32A32_SFLOAT: return dy::RHI::Format::R32G32B32A32_FLOAT;
		default: return dy::RHI::Format::Unknown;
		}
	}

	VkImageLayout ToVulkanImageLayout(dy::RHI::ResourceState state)
	{
		switch (state)
		{
		case dy::RHI::ResourceState::Undefined: return VK_IMAGE_LAYOUT_UNDEFINED;
		case dy::RHI::ResourceState::Common: return VK_IMAGE_LAYOUT_GENERAL;
		case dy::RHI::ResourceState::CopyDestination: return VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		case dy::RHI::ResourceState::ShaderResource: return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		case dy::RHI::ResourceState::UnorderedAccess: return VK_IMAGE_LAYOUT_GENERAL;
		case dy::RHI::ResourceState::RenderTarget: return VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		case dy::RHI::ResourceState::DepthRead: return VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
		case dy::RHI::ResourceState::DepthWrite: return VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
		case dy::RHI::ResourceState::Present: return VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
		default: return VK_IMAGE_LAYOUT_GENERAL;
		}
	}

	VulkanBuffer::VulkanBuffer(const VulkanContext& context, const dy::RHI::BufferDesc& desc)
		: dy::RHI::Buffer(desc)
		, m_device(context.device)
		, m_state(desc.initialState)
	{
		if (desc.size == 0 || desc.usage == dy::RHI::BufferUsage::None || !IsStateAllowed(desc.initialState))
		{
			throw std::runtime_error("Invalid Vulkan buffer description");
		}

		VkBufferCreateInfo bufferInfo{};
		bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
		bufferInfo.size = desc.size;
		bufferInfo.usage = ToBufferUsage(desc.usage);
		bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
		if (vkCreateBuffer(m_device, &bufferInfo, nullptr, &m_buffer) != VK_SUCCESS)
		{
			throw std::runtime_error("Failed to create Vulkan buffer");
		}

		try
		{
			VkMemoryRequirements requirements{};
			vkGetBufferMemoryRequirements(m_device, m_buffer, &requirements);
			VkMemoryAllocateInfo allocation{};
			allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
			allocation.allocationSize = requirements.size;
			allocation.memoryTypeIndex = FindMemoryType(
				context.physicalDevice,
				requirements.memoryTypeBits,
				VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
			if (vkAllocateMemory(m_device, &allocation, nullptr, &m_memory) != VK_SUCCESS ||
				vkBindBufferMemory(m_device, m_buffer, m_memory, 0) != VK_SUCCESS)
			{
				throw std::runtime_error("Failed to allocate Vulkan buffer memory");
			}
		}
		catch (...)
		{
			if (m_memory != VK_NULL_HANDLE) vkFreeMemory(m_device, m_memory, nullptr);
			vkDestroyBuffer(m_device, m_buffer, nullptr);
			m_memory = VK_NULL_HANDLE;
			m_buffer = VK_NULL_HANDLE;
			throw;
		}
	}

	VulkanBuffer::~VulkanBuffer()
	{
		if (m_buffer != VK_NULL_HANDLE) vkDestroyBuffer(m_device, m_buffer, nullptr);
		if (m_memory != VK_NULL_HANDLE) vkFreeMemory(m_device, m_memory, nullptr);
	}

	bool VulkanBuffer::IsStateAllowed(dy::RHI::ResourceState state) const
	{
		switch (state)
		{
		case dy::RHI::ResourceState::Undefined:
		case dy::RHI::ResourceState::Common:
		case dy::RHI::ResourceState::CopyDestination:
			return true;
		case dy::RHI::ResourceState::VertexBuffer:
			return HasUsage(GetDesc().usage, dy::RHI::BufferUsage::Vertex);
		case dy::RHI::ResourceState::IndexBuffer:
			return HasUsage(GetDesc().usage, dy::RHI::BufferUsage::Index);
		case dy::RHI::ResourceState::ConstantBuffer:
			return HasUsage(GetDesc().usage, dy::RHI::BufferUsage::Constant);
		case dy::RHI::ResourceState::ShaderResource:
		case dy::RHI::ResourceState::UnorderedAccess:
			return HasUsage(GetDesc().usage, dy::RHI::BufferUsage::Storage);
		default:
			return false;
		}
	}

	VulkanTexture::VulkanTexture(const VulkanContext& context, const dy::RHI::TextureDesc& desc)
		: dy::RHI::Texture(desc)
		, m_device(context.device)
		, m_aspectMask(ToImageAspectMask(desc.format))
		, m_states(static_cast<size_t>(desc.mipLevels) * desc.depthOrArraySize, dy::RHI::ResourceState::Undefined)
		, m_ownsImage(true)
	{
		const VkFormat format = ToVulkanFormat(desc.format);
		if (desc.width == 0 || desc.height == 0 || desc.depthOrArraySize == 0 || desc.mipLevels == 0 ||
			format == VK_FORMAT_UNDEFINED || desc.usage == dy::RHI::TextureUsage::None)
		{
			throw std::runtime_error("Invalid Vulkan texture description");
		}

		VkImageCreateInfo imageInfo{};
		imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
		imageInfo.imageType = VK_IMAGE_TYPE_2D;
		imageInfo.format = format;
		imageInfo.extent = { desc.width, desc.height, 1 };
		imageInfo.mipLevels = desc.mipLevels;
		imageInfo.arrayLayers = desc.depthOrArraySize;
		imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
		imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
		imageInfo.usage = ToImageUsage(desc.usage);
		imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
		imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		if (vkCreateImage(m_device, &imageInfo, nullptr, &m_image) != VK_SUCCESS)
		{
			throw std::runtime_error("Failed to create Vulkan image");
		}

		try
		{
			VkMemoryRequirements requirements{};
			vkGetImageMemoryRequirements(m_device, m_image, &requirements);
			VkMemoryAllocateInfo allocation{};
			allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
			allocation.allocationSize = requirements.size;
			allocation.memoryTypeIndex = FindMemoryType(
				context.physicalDevice,
				requirements.memoryTypeBits,
				VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
			if (vkAllocateMemory(m_device, &allocation, nullptr, &m_memory) != VK_SUCCESS ||
				vkBindImageMemory(m_device, m_image, m_memory, 0) != VK_SUCCESS)
			{
				throw std::runtime_error("Failed to allocate Vulkan image memory");
			}

			VkImageViewCreateInfo viewInfo{};
			viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
			viewInfo.image = m_image;
			viewInfo.viewType = desc.depthOrArraySize > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
			viewInfo.format = format;
			viewInfo.subresourceRange.aspectMask = m_aspectMask;
			viewInfo.subresourceRange.baseMipLevel = 0;
			viewInfo.subresourceRange.levelCount = desc.mipLevels;
			viewInfo.subresourceRange.baseArrayLayer = 0;
			viewInfo.subresourceRange.layerCount = desc.depthOrArraySize;
			if (vkCreateImageView(m_device, &viewInfo, nullptr, &m_imageView) != VK_SUCCESS)
			{
				throw std::runtime_error("Failed to create Vulkan image view");
			}
		}
		catch (...)
		{
			if (m_imageView != VK_NULL_HANDLE) vkDestroyImageView(m_device, m_imageView, nullptr);
			vkDestroyImage(m_device, m_image, nullptr);
			if (m_memory != VK_NULL_HANDLE) vkFreeMemory(m_device, m_memory, nullptr);
			m_imageView = VK_NULL_HANDLE;
			m_memory = VK_NULL_HANDLE;
			m_image = VK_NULL_HANDLE;
			throw;
		}
	}

	VulkanTexture::VulkanTexture(const dy::RHI::TextureDesc& desc, VkImage image, VkImageView imageView)
		: dy::RHI::Texture(desc)
		, m_image(image)
		, m_imageView(imageView)
		, m_aspectMask(VK_IMAGE_ASPECT_COLOR_BIT)
		, m_states(static_cast<size_t>(desc.mipLevels) * desc.depthOrArraySize, dy::RHI::ResourceState::Present)
	{
		if (image == VK_NULL_HANDLE || imageView == VK_NULL_HANDLE) throw std::runtime_error("Invalid Vulkan swapchain image");
	}

	VulkanTexture::~VulkanTexture()
	{
		if (!m_ownsImage) return;
		for (const ResourceView& resourceView : m_resourceViews)
		{
			if (resourceView.view != VK_NULL_HANDLE) vkDestroyImageView(m_device, resourceView.view, nullptr);
		}
		if (m_imageView != VK_NULL_HANDLE) vkDestroyImageView(m_device, m_imageView, nullptr);
		if (m_image != VK_NULL_HANDLE) vkDestroyImage(m_device, m_image, nullptr);
		if (m_memory != VK_NULL_HANDLE) vkFreeMemory(m_device, m_memory, nullptr);
	}

	bool VulkanTexture::IsStateAllowed(dy::RHI::ResourceState state) const
	{
		switch (state)
		{
		case dy::RHI::ResourceState::Undefined:
		case dy::RHI::ResourceState::Common:
		case dy::RHI::ResourceState::CopyDestination:
			return true;
		case dy::RHI::ResourceState::ShaderResource:
			return HasUsage(GetDesc().usage, dy::RHI::TextureUsage::ShaderResource);
		case dy::RHI::ResourceState::UnorderedAccess:
			return HasUsage(GetDesc().usage, dy::RHI::TextureUsage::Storage);
		case dy::RHI::ResourceState::RenderTarget:
			return HasUsage(GetDesc().usage, dy::RHI::TextureUsage::RenderTarget);
		case dy::RHI::ResourceState::DepthRead:
		case dy::RHI::ResourceState::DepthWrite:
			return HasUsage(GetDesc().usage, dy::RHI::TextureUsage::DepthStencil);
		case dy::RHI::ResourceState::Present:
			return IsSwapchainImage();
		default:
			return false;
		}
	}

	dy::RHI::ResourceState VulkanTexture::GetState(uint32_t mipLevel, uint32_t arrayLayer) const
	{
		if (mipLevel >= GetDesc().mipLevels || arrayLayer >= GetDesc().depthOrArraySize)
		{
			return dy::RHI::ResourceState::Undefined;
		}
		return m_states[static_cast<size_t>(arrayLayer) * GetDesc().mipLevels + mipLevel];
	}

	void VulkanTexture::SetState(uint32_t mipLevel, uint32_t arrayLayer, dy::RHI::ResourceState state)
	{
		if (mipLevel >= GetDesc().mipLevels || arrayLayer >= GetDesc().depthOrArraySize) return;
		m_states[static_cast<size_t>(arrayLayer) * GetDesc().mipLevels + mipLevel] = state;
	}

	VkImageView VulkanTexture::GetSubresourceView(uint32_t mipLevel, uint32_t arrayLayer)
	{
		dy::RHI::TextureSubresourceRange subresources{};
		subresources.firstMipLevel = mipLevel;
		subresources.mipLevelCount = 1;
		subresources.firstArrayLayer = arrayLayer;
		subresources.arrayLayerCount = 1;
		return GetResourceView(subresources);
	}

	VkImageView VulkanTexture::GetResourceView(const dy::RHI::TextureSubresourceRange& subresources)
	{
		if (subresources.firstMipLevel >= GetDesc().mipLevels || subresources.firstArrayLayer >= GetDesc().depthOrArraySize) return VK_NULL_HANDLE;
		const uint32_t mipLevelCount = subresources.mipLevelCount == 0
			? GetDesc().mipLevels - subresources.firstMipLevel
			: subresources.mipLevelCount;
		const uint32_t arrayLayerCount = subresources.arrayLayerCount == 0
			? GetDesc().depthOrArraySize - subresources.firstArrayLayer
			: subresources.arrayLayerCount;
		if (mipLevelCount == 0 || arrayLayerCount == 0 ||
			mipLevelCount > GetDesc().mipLevels - subresources.firstMipLevel ||
			arrayLayerCount > GetDesc().depthOrArraySize - subresources.firstArrayLayer)
		{
			return VK_NULL_HANDLE;
		}
		if (subresources.firstMipLevel == 0 && mipLevelCount == GetDesc().mipLevels &&
			subresources.firstArrayLayer == 0 && arrayLayerCount == GetDesc().depthOrArraySize)
		{
			return m_imageView;
		}
		if (!m_ownsImage) return VK_NULL_HANDLE;
		dy::RHI::TextureSubresourceRange resolved = subresources;
		resolved.mipLevelCount = mipLevelCount;
		resolved.arrayLayerCount = arrayLayerCount;
		const auto existing = std::find_if(
			m_resourceViews.begin(),
			m_resourceViews.end(),
			[&resolved](const ResourceView& resourceView) {
				const dy::RHI::TextureSubresourceRange& view = resourceView.subresources;
				return view.firstMipLevel == resolved.firstMipLevel &&
					view.mipLevelCount == resolved.mipLevelCount &&
					view.firstArrayLayer == resolved.firstArrayLayer &&
					view.arrayLayerCount == resolved.arrayLayerCount;
			});
		if (existing != m_resourceViews.end()) return existing->view;

		VkImageViewCreateInfo info{};
		info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
		info.image = m_image;
		info.viewType = arrayLayerCount > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
		info.format = ToVulkanFormat(GetDesc().format);
		info.subresourceRange.aspectMask = m_aspectMask;
		info.subresourceRange.baseMipLevel = resolved.firstMipLevel;
		info.subresourceRange.levelCount = resolved.mipLevelCount;
		info.subresourceRange.baseArrayLayer = resolved.firstArrayLayer;
		info.subresourceRange.layerCount = resolved.arrayLayerCount;
		VkImageView view = VK_NULL_HANDLE;
		if (vkCreateImageView(m_device, &info, nullptr, &view) != VK_SUCCESS) return VK_NULL_HANDLE;
		try
		{
			m_resourceViews.push_back({ resolved, view });
		}
		catch (...)
		{
			vkDestroyImageView(m_device, view, nullptr);
			throw;
		}
		return view;
	}

	VkImageLayout VulkanTexture::GetBarrierOldLayout(dy::RHI::ResourceState state) const
	{
		if (IsSwapchainImage() && state == dy::RHI::ResourceState::Present && !m_hasPresented)
		{
			return VK_IMAGE_LAYOUT_UNDEFINED;
		}
		return ToVulkanImageLayout(state);
	}
}
