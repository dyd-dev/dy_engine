#include "VulkanCommandList.h"

#include "VulkanPipeline.h"
#include "VulkanResources.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace dy::Backends
{
	namespace
	{
		struct StateInfo
		{
			VkPipelineStageFlags2 stages = VK_PIPELINE_STAGE_2_NONE;
			VkAccessFlags2 access = VK_ACCESS_2_NONE;
		};

		StateInfo GetStateInfo(dy::RHI::ResourceState state)
		{
			switch (state)
			{
			case dy::RHI::ResourceState::Undefined:
				return {};
			case dy::RHI::ResourceState::Common:
				return { VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT };
			case dy::RHI::ResourceState::CopyDestination:
				return { VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT };
			case dy::RHI::ResourceState::VertexBuffer:
				return { VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT, VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT };
			case dy::RHI::ResourceState::IndexBuffer:
				return { VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT, VK_ACCESS_2_INDEX_READ_BIT };
			case dy::RHI::ResourceState::ConstantBuffer:
				return { VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_UNIFORM_READ_BIT };
			case dy::RHI::ResourceState::ShaderResource:
				return { VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT };
			case dy::RHI::ResourceState::UnorderedAccess:
				return { VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT };
			case dy::RHI::ResourceState::RenderTarget:
				return { VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT };
			case dy::RHI::ResourceState::DepthRead:
				return { VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT };
			case dy::RHI::ResourceState::DepthWrite:
				return { VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT };
			case dy::RHI::ResourceState::Present:
				return {};
			default:
				return { VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT };
			}
		}

		VkShaderStageFlags ToShaderStages(dy::RHI::ShaderStageFlags stages)
		{
			VkShaderStageFlags result = 0;
			if ((stages & dy::RHI::ShaderStageFlags::Vertex) != dy::RHI::ShaderStageFlags::None) result |= VK_SHADER_STAGE_VERTEX_BIT;
			if ((stages & dy::RHI::ShaderStageFlags::Fragment) != dy::RHI::ShaderStageFlags::None) result |= VK_SHADER_STAGE_FRAGMENT_BIT;
			return result;
		}

		VkAttachmentLoadOp ToLoadOp(dy::RHI::LoadOp op)
		{
			switch (op)
			{
			case dy::RHI::LoadOp::Load: return VK_ATTACHMENT_LOAD_OP_LOAD;
			case dy::RHI::LoadOp::Clear: return VK_ATTACHMENT_LOAD_OP_CLEAR;
			case dy::RHI::LoadOp::Discard: return VK_ATTACHMENT_LOAD_OP_DONT_CARE;
			default: return VK_ATTACHMENT_LOAD_OP_MAX_ENUM;
			}
		}

		VkAttachmentStoreOp ToStoreOp(dy::RHI::StoreOp op)
		{
			switch (op)
			{
			case dy::RHI::StoreOp::Store: return VK_ATTACHMENT_STORE_OP_STORE;
			case dy::RHI::StoreOp::Discard: return VK_ATTACHMENT_STORE_OP_DONT_CARE;
			default: return VK_ATTACHMENT_STORE_OP_MAX_ENUM;
			}
		}

		uint32_t BytesPerPixel(dy::RHI::Format format)
		{
			switch (format)
			{
			case dy::RHI::Format::R8G8B8A8_UNORM:
			case dy::RHI::Format::B8G8R8A8_UNORM:
			case dy::RHI::Format::R8G8B8A8_UNORM_SRGB:
			case dy::RHI::Format::B8G8R8A8_UNORM_SRGB:
			case dy::RHI::Format::R32_UINT:
				return 4;
			case dy::RHI::Format::R16G16B16A16_FLOAT:
			case dy::RHI::Format::R32G32_FLOAT:
				return 8;
			case dy::RHI::Format::R32G32B32_FLOAT:
				return 12;
			case dy::RHI::Format::R32G32B32A32_FLOAT:
				return 16;
			case dy::RHI::Format::R16_UINT:
				return 2;
			default:
				return 0;
			}
		}

		uint32_t FindHostMemoryType(VkPhysicalDevice physicalDevice, uint32_t typeBits)
		{
			VkPhysicalDeviceMemoryProperties memoryProperties{};
			vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memoryProperties);
			const VkMemoryPropertyFlags wanted = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
			for (uint32_t i = 0; i < memoryProperties.memoryTypeCount; ++i)
			{
				if ((typeBits & (1u << i)) != 0 &&
					(memoryProperties.memoryTypes[i].propertyFlags & wanted) == wanted)
				{
					return i;
				}
			}
			return std::numeric_limits<uint32_t>::max();
		}

		std::pair<VulkanTexture*, uint32_t> TextureKey(
			VulkanTexture* texture,
			uint32_t mipLevel,
			uint32_t arrayLayer)
		{
			return { texture, arrayLayer * texture->GetDesc().mipLevels + mipLevel };
		}

		bool ResolveTextureSubresourceRange(
			const VulkanTexture& texture,
			const dy::RHI::TextureSubresourceRange& range,
			uint32_t& mipLevelCount,
			uint32_t& arrayLayerCount)
		{
			if (range.firstMipLevel >= texture.GetDesc().mipLevels ||
				range.firstArrayLayer >= texture.GetDesc().depthOrArraySize)
			{
				return false;
			}
			mipLevelCount = range.mipLevelCount == 0
				? texture.GetDesc().mipLevels - range.firstMipLevel
				: range.mipLevelCount;
			arrayLayerCount = range.arrayLayerCount == 0
				? texture.GetDesc().depthOrArraySize - range.firstArrayLayer
				: range.arrayLayerCount;
			return mipLevelCount != 0 && arrayLayerCount != 0 &&
				mipLevelCount <= texture.GetDesc().mipLevels - range.firstMipLevel &&
				arrayLayerCount <= texture.GetDesc().depthOrArraySize - range.firstArrayLayer;
		}

		const dy::RHI::ResourceBindingLayout* FindLayoutBinding(
			const dy::RHI::PipelineLayoutDesc& layout,
			uint32_t binding)
		{
			for (uint32_t index = 0; index < layout.bindingCount; ++index)
			{
				const dy::RHI::ResourceBindingLayout& candidate = layout.bindings[index];
				if (candidate.binding == binding) return &candidate;
			}
			return nullptr;
		}

	}

	VulkanCommandList::VulkanCommandList(const VulkanContext& context)
		: m_context(context)
	{
		VkCommandPoolCreateInfo poolInfo{};
		poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
		poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
		poolInfo.queueFamilyIndex = context.queueIndices.graphicsFamily;
		if (vkCreateCommandPool(context.device, &poolInfo, nullptr, &m_commandPool) != VK_SUCCESS)
		{
			throw std::runtime_error("Failed to create Vulkan command pool");
		}

		VkCommandBufferAllocateInfo allocation{};
		allocation.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
		allocation.commandPool = m_commandPool;
		allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		allocation.commandBufferCount = 1;
		if (vkAllocateCommandBuffers(context.device, &allocation, &m_commandBuffer) != VK_SUCCESS)
		{
			vkDestroyCommandPool(context.device, m_commandPool, nullptr);
			m_commandPool = VK_NULL_HANDLE;
			throw std::runtime_error("Failed to allocate Vulkan command buffer");
		}

		VkCommandBufferBeginInfo beginInfo{};
		beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
		beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
		if (vkBeginCommandBuffer(m_commandBuffer, &beginInfo) != VK_SUCCESS)
		{
			vkDestroyCommandPool(context.device, m_commandPool, nullptr);
			m_commandPool = VK_NULL_HANDLE;
			m_commandBuffer = VK_NULL_HANDLE;
			throw std::runtime_error("Failed to begin Vulkan command buffer");
		}
	}

	VulkanCommandList::~VulkanCommandList()
	{
		for (const StagingAllocation& allocation : m_stagingAllocations)
		{
			if (allocation.buffer != VK_NULL_HANDLE) vkDestroyBuffer(m_context.device, allocation.buffer, nullptr);
			if (allocation.memory != VK_NULL_HANDLE) vkFreeMemory(m_context.device, allocation.memory, nullptr);
		}
		if (m_commandPool != VK_NULL_HANDLE) vkDestroyCommandPool(m_context.device, m_commandPool, nullptr);
	}

	void VulkanCommandList::ResourceBarrier(const dy::RHI::ResourceBarrierDesc* barriers, uint32_t count)
	{
		if (m_closed || m_rendering || (count > 0 && barriers == nullptr))
		{
			Fail();
			return;
		}
		if (count == 0) return;

		std::vector<VkBufferMemoryBarrier2> bufferBarriers;
		std::vector<VkImageMemoryBarrier2> imageBarriers;
		bufferBarriers.reserve(count);
		imageBarriers.reserve(count);
		for (uint32_t i = 0; i < count; ++i)
		{
			const dy::RHI::ResourceBarrierDesc& source = barriers[i];
			const bool unorderedAccessBarrier = source.before == dy::RHI::ResourceState::UnorderedAccess &&
				source.after == dy::RHI::ResourceState::UnorderedAccess;
			if ((source.buffer == nullptr) == (source.texture == nullptr) ||
				source.after == dy::RHI::ResourceState::Undefined ||
				(source.before == source.after && !unorderedAccessBarrier))
			{
				Fail();
				return;
			}
			StateInfo before = GetStateInfo(source.before);
			StateInfo after = GetStateInfo(source.after);
			if (unorderedAccessBarrier)
			{
				before.access = VK_ACCESS_2_SHADER_WRITE_BIT;
				after.access = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT;
			}
			if (source.buffer != nullptr)
			{
				VulkanBuffer* buffer = dynamic_cast<VulkanBuffer*>(source.buffer);
				if (buffer == nullptr ||
					!buffer->IsStateAllowed(source.before) || !buffer->IsStateAllowed(source.after) ||
					source.subresources.firstMipLevel != 0 || source.subresources.mipLevelCount != 0 ||
					source.subresources.firstArrayLayer != 0 || source.subresources.arrayLayerCount != 0)
				{
					Fail();
					return;
				}
				const auto prior = m_bufferStates.find(buffer);
				if (prior != m_bufferStates.end() && prior->second != source.before)
				{
					Fail();
					return;
				}
				VkBufferMemoryBarrier2 barrier{};
				barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
				barrier.srcStageMask = before.stages;
				barrier.srcAccessMask = before.access;
				barrier.dstStageMask = after.stages;
				barrier.dstAccessMask = after.access;
				barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
				barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
				barrier.buffer = buffer->GetHandle();
				barrier.offset = 0;
				barrier.size = VK_WHOLE_SIZE;
				bufferBarriers.push_back(barrier);
				Operation operation{};
				operation.kind = OperationKind::BufferBarrier;
				operation.buffer = buffer;
				operation.before = source.before;
				operation.after = source.after;
				m_operations.push_back(operation);
				m_bufferStates[buffer] = source.after;
			}
			else
			{
				VulkanTexture* texture = dynamic_cast<VulkanTexture*>(source.texture);
				uint32_t mipLevelCount = 0;
				uint32_t arrayLayerCount = 0;
				if (texture == nullptr ||
					!texture->IsStateAllowed(source.before) || !texture->IsStateAllowed(source.after) ||
					!ResolveTextureSubresourceRange(*texture, source.subresources, mipLevelCount, arrayLayerCount))
				{
					Fail();
					return;
				}
				for (uint32_t layer = source.subresources.firstArrayLayer;
					layer < source.subresources.firstArrayLayer + arrayLayerCount; ++layer)
				{
					for (uint32_t mip = source.subresources.firstMipLevel;
						mip < source.subresources.firstMipLevel + mipLevelCount; ++mip)
					{
						const auto prior = m_textureStates.find(TextureKey(texture, mip, layer));
						if (prior != m_textureStates.end() && prior->second != source.before)
						{
							Fail();
							return;
						}
					}
				}
				VkImageMemoryBarrier2 barrier{};
				barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
				barrier.srcStageMask = before.stages;
				barrier.srcAccessMask = before.access;
				barrier.dstStageMask = after.stages;
				barrier.dstAccessMask = after.access;
				barrier.oldLayout = texture->GetBarrierOldLayout(source.before);
				barrier.newLayout = ToVulkanImageLayout(source.after);
				barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
				barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
				barrier.image = texture->GetImage();
				barrier.subresourceRange.aspectMask = texture->GetAspectMask();
				barrier.subresourceRange.baseMipLevel = source.subresources.firstMipLevel;
				barrier.subresourceRange.levelCount = mipLevelCount;
				barrier.subresourceRange.baseArrayLayer = source.subresources.firstArrayLayer;
				barrier.subresourceRange.layerCount = arrayLayerCount;
				imageBarriers.push_back(barrier);
				for (uint32_t layer = source.subresources.firstArrayLayer;
					layer < source.subresources.firstArrayLayer + arrayLayerCount; ++layer)
				{
					for (uint32_t mip = source.subresources.firstMipLevel;
						mip < source.subresources.firstMipLevel + mipLevelCount; ++mip)
					{
						Operation operation{};
						operation.kind = OperationKind::TextureBarrier;
						operation.texture = texture;
						operation.before = source.before;
						operation.after = source.after;
						operation.mipLevel = mip;
						operation.arrayLayer = layer;
						m_operations.push_back(operation);
						m_textureStates[TextureKey(texture, mip, layer)] = source.after;
					}
				}
				TrackSwapchainImage(texture);
			}
		}

		VkDependencyInfo dependency{};
		dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
		dependency.bufferMemoryBarrierCount = static_cast<uint32_t>(bufferBarriers.size());
		dependency.pBufferMemoryBarriers = bufferBarriers.empty() ? nullptr : bufferBarriers.data();
		dependency.imageMemoryBarrierCount = static_cast<uint32_t>(imageBarriers.size());
		dependency.pImageMemoryBarriers = imageBarriers.empty() ? nullptr : imageBarriers.data();
		vkCmdPipelineBarrier2(m_commandBuffer, &dependency);
	}

	void VulkanCommandList::BeginRendering(const dy::RHI::RenderingDesc& desc)
	{
		if (m_closed || m_rendering ||
			(desc.colorAttachmentCount > 0 && desc.colorAttachments == nullptr) ||
			(desc.colorAttachmentCount == 0 && desc.depthStencilAttachment == nullptr))
		{
			Fail();
			return;
		}

		m_boundPipeline = nullptr;
		m_boundResourceSet = nullptr;
		m_vertexBindings.clear();
		m_indexBuffer = nullptr;
		m_indexFormat = dy::RHI::Format::Unknown;
		m_indexOffset = 0;
		m_inlineConstantCoverage.clear();
		m_hasViewport = false;
		m_hasScissor = false;
		m_hasStencilReference = false;
		m_colorFormats.clear();
		m_depthTexture = nullptr;
		m_depthFormat = dy::RHI::Format::Unknown;
		m_depthState = dy::RHI::ResourceState::Undefined;
		m_depthMipLevel = 0;
		m_depthArrayLayer = 0;
		m_stencilConfigured = false;

		uint32_t width = 0;
		uint32_t height = 0;
		std::vector<VkRenderingAttachmentInfo> colorAttachments;
		colorAttachments.reserve(desc.colorAttachmentCount);
		for (uint32_t i = 0; i < desc.colorAttachmentCount; ++i)
		{
			const dy::RHI::ColorAttachment& source = desc.colorAttachments[i];
			VulkanTexture* texture = dynamic_cast<VulkanTexture*>(source.texture);
			const VkImageView imageView = texture == nullptr ? VK_NULL_HANDLE : texture->GetSubresourceView(source.mipLevel, source.arrayLayer);
			const uint32_t attachmentWidth = texture == nullptr || source.mipLevel >= texture->GetDesc().mipLevels
				? 0
				: std::max(1u, texture->GetDesc().width >> source.mipLevel);
			const uint32_t attachmentHeight = texture == nullptr || source.mipLevel >= texture->GetDesc().mipLevels
				? 0
				: std::max(1u, texture->GetDesc().height >> source.mipLevel);
			const VkAttachmentLoadOp loadOp = ToLoadOp(source.loadOp);
			const VkAttachmentStoreOp storeOp = ToStoreOp(source.storeOp);
			bool clearValueValid = true;
			if (source.loadOp == dy::RHI::LoadOp::Clear)
			{
				for (float component : source.clearColor)
				{
					clearValueValid = clearValueValid && std::isfinite(component);
				}
			}
			if (texture == nullptr || imageView == VK_NULL_HANDLE || attachmentWidth == 0 || attachmentHeight == 0 ||
				source.arrayLayer >= texture->GetDesc().depthOrArraySize ||
				loadOp == VK_ATTACHMENT_LOAD_OP_MAX_ENUM || storeOp == VK_ATTACHMENT_STORE_OP_MAX_ENUM ||
				(texture->GetDesc().usage & dy::RHI::TextureUsage::RenderTarget) == dy::RHI::TextureUsage::None ||
				!RequireTextureState(texture, source.mipLevel, source.arrayLayer, dy::RHI::ResourceState::RenderTarget) ||
				!clearValueValid ||
				(i > 0 && (attachmentWidth != width || attachmentHeight != height)))
			{
				Fail();
				return;
			}
			width = attachmentWidth;
			height = attachmentHeight;
			VkRenderingAttachmentInfo attachment{};
			attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
			attachment.imageView = imageView;
			attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
			attachment.loadOp = loadOp;
			attachment.storeOp = storeOp;
			std::copy(source.clearColor, source.clearColor + 4, attachment.clearValue.color.float32);
			colorAttachments.push_back(attachment);
			m_colorFormats.push_back(texture->GetDesc().format);
			TrackSwapchainImage(texture);
		}

		VkRenderingAttachmentInfo depthAttachment{};
		VkRenderingAttachmentInfo stencilAttachment{};
		const VkRenderingAttachmentInfo* depthPointer = nullptr;
		const VkRenderingAttachmentInfo* stencilPointer = nullptr;
		if (desc.depthStencilAttachment != nullptr)
		{
			const dy::RHI::DepthStencilAttachment& source = *desc.depthStencilAttachment;
			VulkanTexture* texture = dynamic_cast<VulkanTexture*>(source.texture);
			const VkImageView imageView = texture == nullptr ? VK_NULL_HANDLE : texture->GetSubresourceView(source.mipLevel, source.arrayLayer);
			const uint32_t attachmentWidth = texture == nullptr || source.mipLevel >= texture->GetDesc().mipLevels
				? 0
				: std::max(1u, texture->GetDesc().width >> source.mipLevel);
			const uint32_t attachmentHeight = texture == nullptr || source.mipLevel >= texture->GetDesc().mipLevels
				? 0
				: std::max(1u, texture->GetDesc().height >> source.mipLevel);
			const VkAttachmentLoadOp depthLoadOp = ToLoadOp(source.depthLoadOp);
			const VkAttachmentStoreOp depthStoreOp = ToStoreOp(source.depthStoreOp);
			const bool hasStencil = texture != nullptr && texture->GetDesc().format == dy::RHI::Format::D24_UNORM_S8_UINT;
			const bool declaredStateValid = source.state == dy::RHI::ResourceState::DepthRead ||
				source.state == dy::RHI::ResourceState::DepthWrite;
			const bool requiresWrite = source.depthLoadOp == dy::RHI::LoadOp::Clear ||
				(hasStencil && source.stencilLoadOp == dy::RHI::LoadOp::Clear);
			const bool depthClearValid = source.depthLoadOp != dy::RHI::LoadOp::Clear ||
				(std::isfinite(source.clearDepth) && source.clearDepth >= 0.0f && source.clearDepth <= 1.0f);
			const bool stencilClearValid = source.stencilLoadOp != dy::RHI::LoadOp::Clear ||
				source.clearStencil <= std::numeric_limits<uint8_t>::max();
			const bool stencilLoadDefined = source.stencilLoadOp != dy::RHI::LoadOp::Undefined;
			const bool stencilStoreDefined = source.stencilStoreOp != dy::RHI::StoreOp::Undefined;
			if (texture == nullptr || imageView == VK_NULL_HANDLE || attachmentWidth == 0 || attachmentHeight == 0 ||
				source.arrayLayer >= texture->GetDesc().depthOrArraySize ||
				depthLoadOp == VK_ATTACHMENT_LOAD_OP_MAX_ENUM || depthStoreOp == VK_ATTACHMENT_STORE_OP_MAX_ENUM ||
				(texture->GetDesc().usage & dy::RHI::TextureUsage::DepthStencil) == dy::RHI::TextureUsage::None ||
				!declaredStateValid || (requiresWrite && source.state != dy::RHI::ResourceState::DepthWrite) ||
				!RequireTextureState(texture, source.mipLevel, source.arrayLayer, source.state) ||
				!depthClearValid || !stencilClearValid ||
				(hasStencil && stencilLoadDefined != stencilStoreDefined) ||
				(!hasStencil && (stencilLoadDefined || stencilStoreDefined)) ||
				(width != 0 && (attachmentWidth != width || attachmentHeight != height)))
			{
				Fail();
				return;
			}
			width = attachmentWidth;
			height = attachmentHeight;
			depthAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
			depthAttachment.imageView = imageView;
			depthAttachment.imageLayout = ToVulkanImageLayout(source.state);
			depthAttachment.loadOp = depthLoadOp;
			depthAttachment.storeOp = depthStoreOp;
			depthAttachment.clearValue.depthStencil.depth = source.clearDepth;
			depthAttachment.clearValue.depthStencil.stencil = source.clearStencil;
			depthPointer = &depthAttachment;

			if (hasStencil && stencilLoadDefined)
			{
				const VkAttachmentLoadOp stencilLoadOp = ToLoadOp(source.stencilLoadOp);
				const VkAttachmentStoreOp stencilStoreOp = ToStoreOp(source.stencilStoreOp);
				if (stencilLoadOp == VK_ATTACHMENT_LOAD_OP_MAX_ENUM || stencilStoreOp == VK_ATTACHMENT_STORE_OP_MAX_ENUM)
				{
					Fail();
					return;
				}
				stencilAttachment = depthAttachment;
				stencilAttachment.loadOp = stencilLoadOp;
				stencilAttachment.storeOp = stencilStoreOp;
				stencilPointer = &stencilAttachment;
				m_stencilConfigured = true;
			}
			m_depthTexture = texture;
			m_depthFormat = texture->GetDesc().format;
			m_depthState = source.state;
			m_depthMipLevel = source.mipLevel;
			m_depthArrayLayer = source.arrayLayer;
		}

		if (width == 0 || height == 0)
		{
			Fail();
			return;
		}
		VkRenderingInfo rendering{};
		rendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
		rendering.renderArea.offset = { 0, 0 };
		rendering.renderArea.extent = { width, height };
		rendering.layerCount = 1;
		rendering.colorAttachmentCount = static_cast<uint32_t>(colorAttachments.size());
		rendering.pColorAttachments = colorAttachments.empty() ? nullptr : colorAttachments.data();
		rendering.pDepthAttachment = depthPointer;
		rendering.pStencilAttachment = stencilPointer;
		vkCmdBeginRendering(m_commandBuffer, &rendering);
		m_rendering = true;
	}

	void VulkanCommandList::EndRendering()
	{
		if (m_closed || !m_rendering)
		{
			Fail();
			return;
		}
		vkCmdEndRendering(m_commandBuffer);
		m_rendering = false;
	}

	void VulkanCommandList::BindGraphicsPipeline(dy::RHI::PipelineHandle pipelineState)
	{
		VulkanPipeline* pipeline = dynamic_cast<VulkanPipeline*>(pipelineState);
		if (m_closed || !m_rendering || pipeline == nullptr)
		{
			Fail();
			return;
		}
		if (pipeline->RequiresDepthWrite() &&
			(m_depthTexture == nullptr || m_depthState != dy::RHI::ResourceState::DepthWrite ||
				!RequireTextureState(
					m_depthTexture,
					m_depthMipLevel,
					m_depthArrayLayer,
					dy::RHI::ResourceState::DepthWrite)))
		{
			Fail();
			return;
		}
		m_inlineConstantCoverage.assign(pipeline->GetLayout().inlineConstantSize, 0);
		vkCmdBindPipeline(m_commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline->GetHandle());
		m_boundPipeline = pipeline;
		m_boundResourceSet = nullptr;
	}

	void VulkanCommandList::BindResourceSet(dy::RHI::ResourceSetHandle resourceSet)
	{
		VulkanResourceSet* set = dynamic_cast<VulkanResourceSet*>(resourceSet);
		if (m_closed || !m_rendering || m_boundPipeline == nullptr ||
			set == nullptr || set->GetVulkanPipeline() != m_boundPipeline)
		{
			Fail();
			return;
		}
		const dy::RHI::PipelineLayoutDesc& layout = m_boundPipeline->GetLayout();
		for (uint32_t index = 0; index < set->GetBindingCount(); ++index)
		{
			const dy::RHI::ResourceBinding& binding = set->GetBindings()[index];
			const dy::RHI::ResourceBindingLayout* declaration =
				FindLayoutBinding(layout, binding.binding);
			if (declaration == nullptr)
			{
				Fail();
				return;
			}
			if (declaration->type == dy::RHI::ResourceBindingType::ConstantBuffer ||
				declaration->type == dy::RHI::ResourceBindingType::ReadOnlyStorageBuffer ||
				declaration->type == dy::RHI::ResourceBindingType::ReadWriteStorageBuffer)
			{
				VulkanBuffer* buffer = dynamic_cast<VulkanBuffer*>(binding.buffer);
				const dy::RHI::ResourceState requiredState =
					declaration->type == dy::RHI::ResourceBindingType::ConstantBuffer
					? dy::RHI::ResourceState::ConstantBuffer
					: declaration->type == dy::RHI::ResourceBindingType::ReadOnlyStorageBuffer
						? dy::RHI::ResourceState::ShaderResource
						: dy::RHI::ResourceState::UnorderedAccess;
				if (buffer == nullptr || !RequireBufferState(buffer, requiredState))
				{
					Fail();
					return;
				}
			}
			else if (declaration->type == dy::RHI::ResourceBindingType::SampledTexture ||
				declaration->type == dy::RHI::ResourceBindingType::StorageTexture)
			{
				VulkanTexture* texture = dynamic_cast<VulkanTexture*>(binding.texture);
				const dy::RHI::ResourceState requiredState =
					declaration->type == dy::RHI::ResourceBindingType::SampledTexture
					? dy::RHI::ResourceState::ShaderResource
					: dy::RHI::ResourceState::UnorderedAccess;
				if (texture == nullptr ||
					!RequireTextureSubresourcesInState(texture, binding.subresources, requiredState))
				{
					Fail();
					return;
				}
				TrackSwapchainImage(texture);
			}
			else
			{
				Fail();
				return;
			}
		}
		const VkDescriptorSet descriptorSet = set->GetSet();
		if (descriptorSet != VK_NULL_HANDLE)
		{
			vkCmdBindDescriptorSets(
				m_commandBuffer,
				VK_PIPELINE_BIND_POINT_GRAPHICS,
				m_boundPipeline->GetPipelineLayout(),
				0,
				1,
				&descriptorSet,
				0,
				nullptr);
		}
		m_boundResourceSet = set;
	}

	void VulkanCommandList::BindVertexBuffer(uint32_t binding, dy::RHI::BufferHandle buffer, uint32_t offset)
	{
		VulkanBuffer* nativeBuffer = dynamic_cast<VulkanBuffer*>(buffer);
		const dy::RHI::VertexBufferLayout* layout = nullptr;
		if (m_boundPipeline != nullptr)
		{
			for (const dy::RHI::VertexBufferLayout& candidate : m_boundPipeline->GetVertexBuffers())
			{
				if (candidate.binding == binding)
				{
					layout = &candidate;
					break;
				}
			}
		}
		if (m_closed || !m_rendering || m_boundPipeline == nullptr || layout == nullptr ||
			nativeBuffer == nullptr || offset >= nativeBuffer->GetDesc().size ||
			(nativeBuffer->GetDesc().usage & dy::RHI::BufferUsage::Vertex) == dy::RHI::BufferUsage::None)
		{
			Fail();
			return;
		}
		if (!RequireBufferState(nativeBuffer, dy::RHI::ResourceState::VertexBuffer))
		{
			Fail();
			return;
		}
		const VkBuffer handle = nativeBuffer->GetHandle();
		const VkDeviceSize nativeOffset = offset;
		vkCmdBindVertexBuffers(m_commandBuffer, binding, 1, &handle, &nativeOffset);
		m_vertexBindings[binding] = { nativeBuffer, offset };
	}

	void VulkanCommandList::BindIndexBuffer(dy::RHI::BufferHandle buffer, dy::RHI::Format format, uint32_t offset)
	{
		VulkanBuffer* nativeBuffer = dynamic_cast<VulkanBuffer*>(buffer);
		const uint32_t indexSize = format == dy::RHI::Format::R16_UINT ? 2u : 4u;
		if (m_closed || !m_rendering || nativeBuffer == nullptr || offset >= nativeBuffer->GetDesc().size ||
			(nativeBuffer->GetDesc().usage & dy::RHI::BufferUsage::Index) == dy::RHI::BufferUsage::None ||
			(format != dy::RHI::Format::R16_UINT && format != dy::RHI::Format::R32_UINT) ||
			(offset % indexSize) != 0 || nativeBuffer->GetDesc().size - offset < indexSize)
		{
			Fail();
			return;
		}
		if (!RequireBufferState(nativeBuffer, dy::RHI::ResourceState::IndexBuffer))
		{
			Fail();
			return;
		}
		vkCmdBindIndexBuffer(
			m_commandBuffer,
			nativeBuffer->GetHandle(),
			offset,
			format == dy::RHI::Format::R16_UINT ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32);
		m_indexBuffer = nativeBuffer;
		m_indexFormat = format;
		m_indexOffset = offset;
	}

	void VulkanCommandList::SetInlineConstants(uint32_t offset, uint32_t size, const void* data)
	{
		if (m_closed || !m_rendering || m_boundPipeline == nullptr || data == nullptr || size == 0)
		{
			Fail();
			return;
		}
		const dy::RHI::PipelineLayoutDesc& layout = m_boundPipeline->GetLayout();
		if (offset > layout.inlineConstantSize || size > layout.inlineConstantSize - offset ||
			(offset % sizeof(uint32_t)) != 0 || (size % sizeof(uint32_t)) != 0)
		{
			Fail();
			return;
		}
		const VkShaderStageFlags stages = ToShaderStages(layout.inlineConstantStages);
		if (stages == 0)
		{
			Fail();
			return;
		}
		vkCmdPushConstants(m_commandBuffer, m_boundPipeline->GetPipelineLayout(), stages, offset, size, data);
		std::fill(
			m_inlineConstantCoverage.begin() + static_cast<std::ptrdiff_t>(offset),
			m_inlineConstantCoverage.begin() + static_cast<std::ptrdiff_t>(offset + size),
			1);
	}

	void VulkanCommandList::SetViewport(const dy::RHI::Viewport& viewport)
	{
		if (m_closed || !m_rendering ||
			!std::isfinite(viewport.x) || !std::isfinite(viewport.y) ||
			!std::isfinite(viewport.width) || !std::isfinite(viewport.height) ||
			!std::isfinite(viewport.minDepth) || !std::isfinite(viewport.maxDepth) ||
			viewport.width <= 0.0f || viewport.height <= 0.0f ||
			viewport.minDepth < 0.0f || viewport.maxDepth > 1.0f ||
			viewport.minDepth > viewport.maxDepth)
		{
			Fail();
			return;
		}
		VkViewport native{};
		native.x = viewport.x;
		native.y = viewport.y + viewport.height;
		native.width = viewport.width;
		native.height = -viewport.height;
		native.minDepth = viewport.minDepth;
		native.maxDepth = viewport.maxDepth;
		vkCmdSetViewport(m_commandBuffer, 0, 1, &native);
		m_hasViewport = true;
	}

	void VulkanCommandList::SetScissor(const dy::RHI::Rect& rect)
	{
		if (m_closed || !m_rendering || rect.x < 0 || rect.y < 0 || rect.width == 0 || rect.height == 0)
		{
			Fail();
			return;
		}
		VkRect2D native{};
		native.offset = { rect.x, rect.y };
		native.extent = { rect.width, rect.height };
		vkCmdSetScissor(m_commandBuffer, 0, 1, &native);
		m_hasScissor = true;
	}

	void VulkanCommandList::SetStencilReference(uint32_t reference)
	{
		if (m_closed || !m_rendering || reference > std::numeric_limits<uint8_t>::max())
		{
			Fail();
			return;
		}
		vkCmdSetStencilReference(m_commandBuffer, VK_STENCIL_FACE_FRONT_AND_BACK, reference);
		m_hasStencilReference = true;
	}

	void VulkanCommandList::DrawInstanced(uint32_t vertexCount, uint32_t instanceCount, uint32_t startVertex, uint32_t startInstance)
	{
		if (vertexCount == 0 || instanceCount == 0 ||
			!ValidateDraw(false, vertexCount, instanceCount, startVertex, startInstance))
		{
			Fail();
			return;
		}
		vkCmdDraw(m_commandBuffer, vertexCount, instanceCount, startVertex, startInstance);
	}

	void VulkanCommandList::DrawIndexedInstanced(uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex, int32_t vertexOffset, uint32_t firstInstance)
	{
		const uint32_t indexSize = m_indexFormat == dy::RHI::Format::R16_UINT ? 2u : 4u;
		const uint64_t end = static_cast<uint64_t>(m_indexOffset) +
			(static_cast<uint64_t>(firstIndex) + indexCount) * indexSize;
		if (indexCount == 0 || instanceCount == 0 ||
			!ValidateDraw(true, 0, instanceCount, 0, firstInstance) ||
			m_indexBuffer == nullptr || end > m_indexBuffer->GetDesc().size)
		{
			Fail();
			return;
		}
		vkCmdDrawIndexed(m_commandBuffer, indexCount, instanceCount, firstIndex, vertexOffset, firstInstance);
	}

	void VulkanCommandList::Close()
	{
		if (m_closed) return;
		if (m_rendering)
		{
			Fail();
			vkCmdEndRendering(m_commandBuffer);
			m_rendering = false;
		}
		if (vkEndCommandBuffer(m_commandBuffer) != VK_SUCCESS) Fail();
		m_closed = true;
	}

	bool VulkanCommandList::CreateStagingAllocation(const void* data, uint32_t size, StagingAllocation& allocation)
	{
		if (data == nullptr || size == 0) return false;
		VkBufferCreateInfo bufferInfo{};
		bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
		bufferInfo.size = size;
		bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
		bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
		if (vkCreateBuffer(m_context.device, &bufferInfo, nullptr, &allocation.buffer) != VK_SUCCESS) return false;

		VkMemoryRequirements requirements{};
		vkGetBufferMemoryRequirements(m_context.device, allocation.buffer, &requirements);
		const uint32_t memoryType = FindHostMemoryType(m_context.physicalDevice, requirements.memoryTypeBits);
		if (memoryType == std::numeric_limits<uint32_t>::max())
		{
			vkDestroyBuffer(m_context.device, allocation.buffer, nullptr);
			allocation.buffer = VK_NULL_HANDLE;
			return false;
		}
		VkMemoryAllocateInfo memoryInfo{};
		memoryInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
		memoryInfo.allocationSize = requirements.size;
		memoryInfo.memoryTypeIndex = memoryType;
		if (vkAllocateMemory(m_context.device, &memoryInfo, nullptr, &allocation.memory) != VK_SUCCESS ||
			vkBindBufferMemory(m_context.device, allocation.buffer, allocation.memory, 0) != VK_SUCCESS)
		{
			if (allocation.memory != VK_NULL_HANDLE) vkFreeMemory(m_context.device, allocation.memory, nullptr);
			vkDestroyBuffer(m_context.device, allocation.buffer, nullptr);
			allocation = {};
			return false;
		}

		void* mapped = nullptr;
		if (vkMapMemory(m_context.device, allocation.memory, 0, size, 0, &mapped) != VK_SUCCESS)
		{
			vkFreeMemory(m_context.device, allocation.memory, nullptr);
			vkDestroyBuffer(m_context.device, allocation.buffer, nullptr);
			allocation = {};
			return false;
		}
		std::memcpy(mapped, data, size);
		vkUnmapMemory(m_context.device, allocation.memory);
		return true;
	}

	bool VulkanCommandList::RecordBufferUpdate(VulkanBuffer& buffer, uint32_t offset, const void* data, uint32_t size)
	{
		if (m_closed || m_failed || m_rendering || data == nullptr || size == 0 ||
			offset > buffer.GetDesc().size || size > buffer.GetDesc().size - offset)
		{
			return false;
		}
		StagingAllocation staging{};
		if (!CreateStagingAllocation(data, size, staging)) return false;
		VkBufferCopy region{};
		region.srcOffset = 0;
		region.dstOffset = offset;
		region.size = size;
		vkCmdCopyBuffer(m_commandBuffer, staging.buffer, buffer.GetHandle(), 1, &region);
		m_stagingAllocations.push_back(staging);
		Operation operation{};
		operation.kind = OperationKind::BufferWrite;
		operation.buffer = &buffer;
		m_operations.push_back(operation);
		return true;
	}

	bool VulkanCommandList::RecordTextureUpdate(
		VulkanTexture& texture,
		uint32_t mipLevel,
		uint32_t arrayLayer,
		const void* data,
		uint32_t dataSize,
		uint32_t rowPitch,
		uint32_t slicePitch)
	{
		const uint32_t bytesPerPixel = BytesPerPixel(texture.GetDesc().format);
		if (m_closed || m_failed || m_rendering || texture.IsSwapchainImage() || data == nullptr || bytesPerPixel == 0 ||
			mipLevel >= texture.GetDesc().mipLevels || arrayLayer >= texture.GetDesc().depthOrArraySize)
		{
			return false;
		}
		const uint32_t width = std::max(1u, texture.GetDesc().width >> mipLevel);
		const uint32_t height = std::max(1u, texture.GetDesc().height >> mipLevel);
		const uint64_t minimumRowPitch = static_cast<uint64_t>(width) * bytesPerPixel;
		const uint64_t requiredSlicePitch = static_cast<uint64_t>(rowPitch) * height;
		if (rowPitch < minimumRowPitch || rowPitch % bytesPerPixel != 0 ||
			slicePitch != requiredSlicePitch || dataSize != slicePitch)
		{
			return false;
		}

		StagingAllocation staging{};
		if (!CreateStagingAllocation(data, dataSize, staging)) return false;
		VkBufferImageCopy region{};
		region.bufferOffset = 0;
		region.bufferRowLength = rowPitch / bytesPerPixel;
		region.bufferImageHeight = height;
		region.imageSubresource.aspectMask = texture.GetAspectMask();
		region.imageSubresource.mipLevel = mipLevel;
		region.imageSubresource.baseArrayLayer = arrayLayer;
		region.imageSubresource.layerCount = 1;
		region.imageExtent = { width, height, 1 };
		vkCmdCopyBufferToImage(
			m_commandBuffer,
			staging.buffer,
			texture.GetImage(),
			VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			1,
			&region);
		m_stagingAllocations.push_back(staging);
		Operation operation{};
		operation.kind = OperationKind::TextureWrite;
		operation.texture = &texture;
		operation.mipLevel = mipLevel;
		operation.arrayLayer = arrayLayer;
		m_operations.push_back(operation);
		return true;
	}

	bool VulkanCommandList::RequireBufferState(VulkanBuffer* buffer, dy::RHI::ResourceState state)
	{
		if (buffer == nullptr || !buffer->IsStateAllowed(state)) return false;
		const auto found = m_bufferStates.find(buffer);
		if (found != m_bufferStates.end() && found->second != state) return false;
		Operation operation{};
		operation.kind = OperationKind::BufferRequirement;
		operation.buffer = buffer;
		operation.before = state;
		m_operations.push_back(operation);
		return true;
	}

	bool VulkanCommandList::RequireTextureState(
		VulkanTexture* texture,
		uint32_t mipLevel,
		uint32_t arrayLayer,
		dy::RHI::ResourceState state)
	{
		if (texture == nullptr || !texture->IsStateAllowed(state) ||
			mipLevel >= texture->GetDesc().mipLevels || arrayLayer >= texture->GetDesc().depthOrArraySize)
		{
			return false;
		}
		const auto key = TextureKey(texture, mipLevel, arrayLayer);
		const auto found = m_textureStates.find(key);
		if (found != m_textureStates.end() && found->second != state) return false;
		Operation operation{};
		operation.kind = OperationKind::TextureRequirement;
		operation.texture = texture;
		operation.mipLevel = mipLevel;
		operation.arrayLayer = arrayLayer;
		operation.before = state;
		m_operations.push_back(operation);
		return true;
	}

	bool VulkanCommandList::RequireTextureSubresourcesInState(
		VulkanTexture* texture,
		const dy::RHI::TextureSubresourceRange& range,
		dy::RHI::ResourceState state)
	{
		uint32_t mipLevelCount = 0;
		uint32_t arrayLayerCount = 0;
		if (texture == nullptr ||
			!ResolveTextureSubresourceRange(*texture, range, mipLevelCount, arrayLayerCount))
		{
			return false;
		}
		for (uint32_t layer = range.firstArrayLayer;
			layer < range.firstArrayLayer + arrayLayerCount; ++layer)
		{
			for (uint32_t mip = range.firstMipLevel;
				mip < range.firstMipLevel + mipLevelCount; ++mip)
			{
				if (!RequireTextureState(texture, mip, layer, state)) return false;
			}
		}
		return true;
	}

	void VulkanCommandList::TrackSwapchainImage(VulkanTexture* texture)
	{
		if (texture == nullptr || !texture->IsSwapchainImage()) return;
		if (std::find(m_referencedSwapchainImages.begin(), m_referencedSwapchainImages.end(), texture) ==
			m_referencedSwapchainImages.end())
		{
			m_referencedSwapchainImages.push_back(texture);
		}
	}

	bool VulkanCommandList::ValidateDraw(
		bool indexed,
		uint32_t vertexCount,
		uint32_t instanceCount,
		uint32_t startVertex,
		uint32_t startInstance) const
	{
		if (m_closed || m_failed || !m_rendering || m_boundPipeline == nullptr ||
			!m_hasViewport || !m_hasScissor || (indexed && m_indexBuffer == nullptr))
		{
			return false;
		}
		if (m_boundPipeline->GetColorFormats() != m_colorFormats ||
			m_boundPipeline->GetDepthFormat() != m_depthFormat)
		{
			return false;
		}
		if (m_boundPipeline->UsesStencil() && (!m_stencilConfigured || !m_hasStencilReference))
		{
			return false;
		}
		for (const dy::RHI::VertexBufferLayout& layout : m_boundPipeline->GetVertexBuffers())
		{
			const auto found = m_vertexBindings.find(layout.binding);
			if (found == m_vertexBindings.end()) return false;
			uint64_t first = 0;
			uint64_t count = 0;
			if (layout.stepMode == dy::RHI::VertexStepMode::Instance)
			{
				first = startInstance;
				count = instanceCount;
			}
			else if (!indexed)
			{
				first = startVertex;
				count = vertexCount;
			}
			const uint64_t available = found->second.buffer->GetDesc().size - found->second.offset;
			if (first + count > available / layout.stride) return false;
		}
		bool resourceSetRequired = false;
		const dy::RHI::PipelineLayoutDesc& layout = m_boundPipeline->GetLayout();
		for (uint32_t index = 0; index < layout.bindingCount; ++index)
		{
			if (layout.bindings[index].type != dy::RHI::ResourceBindingType::StaticSampler)
			{
				resourceSetRequired = true;
				break;
			}
		}
		if (resourceSetRequired && m_boundResourceSet == nullptr) return false;
		return std::find(m_inlineConstantCoverage.begin(), m_inlineConstantCoverage.end(), 0) ==
			m_inlineConstantCoverage.end();
	}

	bool VulkanCommandList::ValidateForSubmit(VulkanSubmissionState& state) const
	{
		if (m_failed) return false;
		for (const Operation& operation : m_operations)
		{
			switch (operation.kind)
			{
			case OperationKind::BufferBarrier:
			{
				const auto found = state.buffers.find(operation.buffer);
				const dy::RHI::ResourceState current = found == state.buffers.end()
					? operation.buffer->GetState() : found->second;
				if (current != operation.before) return false;
				state.buffers[operation.buffer] = operation.after;
				break;
			}
			case OperationKind::TextureBarrier:
			{
				const auto key = TextureKey(operation.texture, operation.mipLevel, operation.arrayLayer);
				const auto found = state.textureSubresources.find(key);
				const dy::RHI::ResourceState current = found == state.textureSubresources.end()
					? operation.texture->GetState(operation.mipLevel, operation.arrayLayer)
					: found->second;
				if (current != operation.before) return false;
				state.textureSubresources[key] = operation.after;
				break;
			}
			case OperationKind::BufferRequirement:
			{
				const auto found = state.buffers.find(operation.buffer);
				const dy::RHI::ResourceState current = found == state.buffers.end()
					? operation.buffer->GetState() : found->second;
				if (current != operation.before) return false;
				break;
			}
			case OperationKind::TextureRequirement:
			{
				const auto key = TextureKey(operation.texture, operation.mipLevel, operation.arrayLayer);
				const auto found = state.textureSubresources.find(key);
				const dy::RHI::ResourceState current = found == state.textureSubresources.end()
					? operation.texture->GetState(operation.mipLevel, operation.arrayLayer)
					: found->second;
				if (current != operation.before) return false;
				break;
			}
			case OperationKind::BufferWrite:
			{
				const auto found = state.buffers.find(operation.buffer);
				const dy::RHI::ResourceState current = found == state.buffers.end()
					? operation.buffer->GetState() : found->second;
				if (current != dy::RHI::ResourceState::CopyDestination) return false;
				break;
			}
			case OperationKind::TextureWrite:
			{
				const auto key = TextureKey(operation.texture, operation.mipLevel, operation.arrayLayer);
				const auto found = state.textureSubresources.find(key);
				const dy::RHI::ResourceState current = found == state.textureSubresources.end()
					? operation.texture->GetState(operation.mipLevel, operation.arrayLayer)
					: found->second;
				if (current != dy::RHI::ResourceState::CopyDestination) return false;
				break;
			}
			}
		}
		return true;
	}

	void VulkanCommandList::CommitResourceStates()
	{
		for (const Operation& operation : m_operations)
		{
			if (operation.kind == OperationKind::BufferBarrier)
			{
				operation.buffer->SetState(operation.after);
			}
			else if (operation.kind == OperationKind::TextureBarrier)
			{
				operation.texture->SetState(operation.mipLevel, operation.arrayLayer, operation.after);
			}
		}
	}
}
