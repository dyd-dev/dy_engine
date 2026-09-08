#include "VulkanDevice.h"
#include "VulkanCommandList.h"
#include "VulkanContext.h"
#include "VulkanPipeline.h"
#include "VulkanResources.h"
#include "VulkanSwapchain.h"
#include "VulkanRuntimePolicy.h"
#include "RHI/IBuffer.h"
#include "RHI/IPipelineState.h"
#include "RHI/ITexture.h"
#include <GLFW/glfw3.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <cstdio>
#include <limits>
#include <memory>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>
#if defined(_WIN32)
#include <windows.h>
#endif

#define SDL_Log(fmt, ...) { printf(fmt, ##__VA_ARGS__); printf("\n"); }

namespace dy::Backends
{
namespace {
	const char* VkResultToString(VkResult result);
	constexpr const char* kValidationLayerName = "VK_LAYER_KHRONOS_validation";
	constexpr const uint32_t kFallbackTextureWidth = 2;
	constexpr const uint32_t kFallbackTextureHeight = 2;
	constexpr size_t kGraphicsResourceProfileCount =
		static_cast<size_t>(dy::RHI::GraphicsResourceProfile::Count);
	constexpr std::array<unsigned char, 16> kFallbackTexturePixels = {
		255, 255, 255, 255, 64, 64, 64, 255,
		64, 64, 64, 255, 255, 255, 255, 255
	};
	constexpr uint32_t kRequiredVulkanApiVersion = VK_API_VERSION_1_4;

	constexpr bool SupportsRequiredVulkanApiVersion(uint32_t apiVersion)
	{
		return VK_API_VERSION_VARIANT(apiVersion) == 0u
			&& apiVersion >= kRequiredVulkanApiVersion;
	}

	size_t ResourceProfileIndex(dy::RHI::GraphicsResourceProfile profile)
	{
		return static_cast<size_t>(profile);
	}

	bool IsProfileStorageBinding(
		dy::RHI::GraphicsResourceProfile profile,
		uint32_t binding,
		const dy::RHI::ShaderLayoutDesc& layout)
	{
		if(profile == dy::RHI::GraphicsResourceProfile::PerDrawSkin)
		{
			return binding == layout.skinInfluenceStorageBinding
				|| binding == layout.skinPaletteStorageBinding;
		}
		return binding == layout.bindlessTransformStorageBinding;
	}

	bool IsValidationEnabled() {
#if !defined(NDEBUG)
		uint32_t layerCount = 0;
		VkResult result = vkEnumerateInstanceLayerProperties(&layerCount, nullptr);
		if (result != VK_SUCCESS) {
			SDL_Log("Failed to enumerate Vulkan instance layers: %s (%d)", VkResultToString(result), static_cast<int>(result));
			return false;
		}

		std::vector<VkLayerProperties> availableLayers(layerCount);
		if (layerCount > 0) {
			result = vkEnumerateInstanceLayerProperties(&layerCount, availableLayers.data());
			if (result != VK_SUCCESS) {
				SDL_Log("Failed to read Vulkan instance layers: %s (%d)", VkResultToString(result), static_cast<int>(result));
				return false;
			}
		}

		for (const VkLayerProperties& layer : availableLayers) {
			if (std::strcmp(layer.layerName, kValidationLayerName) == 0) {
				return true;
			}
		}

		SDL_Log("Vulkan validation layer '%s' is not available. Continuing without validation.", kValidationLayerName);
		return false;
#else
		return false;
#endif
	}

	const char* VkResultToString(VkResult result)
	{
		switch (result)
		{
		case VK_SUCCESS: return "VK_SUCCESS";
		case VK_NOT_READY: return "VK_NOT_READY";
		case VK_TIMEOUT: return "VK_TIMEOUT";
		case VK_EVENT_SET: return "VK_EVENT_SET";
		case VK_EVENT_RESET: return "VK_EVENT_RESET";
		case VK_INCOMPLETE: return "VK_INCOMPLETE";
		case VK_SUBOPTIMAL_KHR: return "VK_SUBOPTIMAL_KHR";
		case VK_ERROR_OUT_OF_DATE_KHR: return "VK_ERROR_OUT_OF_DATE_KHR";
		case VK_ERROR_SURFACE_LOST_KHR: return "VK_ERROR_SURFACE_LOST_KHR";
		case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
		case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
		case VK_ERROR_LAYER_NOT_PRESENT: return "VK_ERROR_LAYER_NOT_PRESENT";
		default: return "VK_ERROR_UNKNOWN";
		}
	}

	bool HasTextureUsage(dy::RHI::TextureUsage usage, dy::RHI::TextureUsage flag)
	{
		return (usage & flag) != dy::RHI::TextureUsage::None;
	}

	VkFormat ToVkFormat(dy::RHI::Format format)
	{
		switch (format)
		{
		case dy::RHI::Format::R8G8B8A8_UNORM: return VK_FORMAT_R8G8B8A8_UNORM;
		case dy::RHI::Format::R16G16B16A16_FLOAT: return VK_FORMAT_R16G16B16A16_SFLOAT;
		case dy::RHI::Format::R32G32B32A32_FLOAT: return VK_FORMAT_R32G32B32A32_SFLOAT;
		case dy::RHI::Format::R32_UINT: return VK_FORMAT_R32_UINT;
		case dy::RHI::Format::D32_FLOAT: return VK_FORMAT_D32_SFLOAT;
		case dy::RHI::Format::D24_UNORM_S8_UINT: return VK_FORMAT_D24_UNORM_S8_UINT;
		default: return VK_FORMAT_UNDEFINED;
		}
	}

	dy::RHI::Format ToRhiDepthFormat(VkFormat format)
	{
		switch (format)
		{
		case VK_FORMAT_D24_UNORM_S8_UINT:
			return dy::RHI::Format::D24_UNORM_S8_UINT;
		case VK_FORMAT_D32_SFLOAT:
		case VK_FORMAT_D32_SFLOAT_S8_UINT:
			return dy::RHI::Format::D32_FLOAT;
		default:
			return dy::RHI::Format::Unknown;
		}
	}

	VkImageUsageFlags ToVkImageUsage(dy::RHI::TextureUsage usage)
	{
		VkImageUsageFlags flags = 0;
		if (HasTextureUsage(usage, dy::RHI::TextureUsage::ShaderResource)) {
			flags |= VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
		}
		if (HasTextureUsage(usage, dy::RHI::TextureUsage::RenderTarget)) {
			flags |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
		}
		if (HasTextureUsage(usage, dy::RHI::TextureUsage::DepthStencil)) {
			flags |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
		}
		if (HasTextureUsage(usage, dy::RHI::TextureUsage::Storage)) {
			flags |= VK_IMAGE_USAGE_STORAGE_BIT;
		}
		if (flags == 0) flags = VK_IMAGE_USAGE_SAMPLED_BIT;
		return flags;
	}

	bool IsDepthFormat(dy::RHI::Format format)
	{
		return format == dy::RHI::Format::D32_FLOAT || format == dy::RHI::Format::D24_UNORM_S8_UINT;
	}

	VkImageAspectFlags GetImageAspectMask(dy::RHI::Format format, dy::RHI::TextureUsage usage)
	{
		if (HasTextureUsage(usage, dy::RHI::TextureUsage::DepthStencil) || IsDepthFormat(format)) {
			return VK_IMAGE_ASPECT_DEPTH_BIT;
		}
		return VK_IMAGE_ASPECT_COLOR_BIT;
	}

	class VulkanTexture final : public dy::RHI::ITexture
	{
	public:
		explicit VulkanTexture(const dy::RHI::TextureDesc& desc)
			: dy::RHI::ITexture(desc)
		{
		}

		~VulkanTexture() override
		{
			Cleanup();
		}

		bool Initialize(
			const VulkanContext& context,
			const dy::RHI::TextureDesc& desc,
			VkFormat formatOverride = VK_FORMAT_UNDEFINED,
			VkImageUsageFlags extraUsage = 0,
			bool enableRgba8HostImageCopy = false)
		{
			Cleanup();
			m_device = context.device;
			SetDesc(desc);
			m_vkFormat = formatOverride != VK_FORMAT_UNDEFINED ? formatOverride : ToVkFormat(desc.format);
			const VkImageAspectFlags aspectMask = GetImageAspectMask(desc.format, desc.usage);
			m_imageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
			m_hostImageCopyEnabled = enableRgba8HostImageCopy && m_vkFormat == VK_FORMAT_R8G8B8A8_UNORM;

			if (desc.width == 0 || desc.height == 0 || m_vkFormat == VK_FORMAT_UNDEFINED) return false;

			try {
				VkImageUsageFlags imageUsage = ToVkImageUsage(desc.usage) | extraUsage;
				if (m_hostImageCopyEnabled) imageUsage |= VK_IMAGE_USAGE_HOST_TRANSFER_BIT;
				VulkanResources::CreateImage(
					context,
					desc.width,
					desc.height,
					m_vkFormat,
					VK_IMAGE_TILING_OPTIMAL,
					imageUsage,
					VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
					m_image,
					m_memory);

				m_imageView = VulkanResources::CreateImageView(m_device, m_image, m_vkFormat, aspectMask);
			} catch (const std::exception& e) {
				SDL_Log("Vulkan texture creation failed: %s", e.what());
				Cleanup();
				return false;
			}

			return true;
		}

		void UpdateMetadata(const dy::RHI::TextureDesc& desc)
		{
			SetDesc(desc);
		}

		bool CreateDefaultSampler()
		{
			VkSamplerCreateInfo info{};
			info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
			info.magFilter = VK_FILTER_LINEAR;
			info.minFilter = VK_FILTER_LINEAR;
			info.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
			info.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
			info.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
			info.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
			info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
			return CreateSampler(info);
		}

		bool CreateSampler(const VkSamplerCreateInfo& info)
		{
			if (m_device == VK_NULL_HANDLE) return false;
			if (m_sampler != VK_NULL_HANDLE) {
				vkDestroySampler(m_device, m_sampler, nullptr);
				m_sampler = VK_NULL_HANDLE;
			}

			return vkCreateSampler(m_device, &info, nullptr, &m_sampler) == VK_SUCCESS;
		}

		bool UploadRGBA8(const VulkanContext& context, VkCommandPool commandPool, const void* pixels, uint32_t width, uint32_t height)
		{
			if (m_image == VK_NULL_HANDLE || pixels == nullptr || width == 0 || height == 0) return false;

			if (m_hostImageCopyEnabled && vkDeviceWaitIdle(context.device) == VK_SUCCESS) {
				bool layoutReady = m_imageLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
				if (!layoutReady) {
					VkHostImageLayoutTransitionInfo transition{};
					transition.sType = VK_STRUCTURE_TYPE_HOST_IMAGE_LAYOUT_TRANSITION_INFO;
					transition.image = m_image;
					transition.oldLayout = m_imageLayout;
					transition.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
					transition.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
					transition.subresourceRange.levelCount = 1;
					transition.subresourceRange.layerCount = 1;
					layoutReady = vkTransitionImageLayout(context.device, 1, &transition) == VK_SUCCESS;
				}

				if (layoutReady) {
					VkMemoryToImageCopy region{};
					region.sType = VK_STRUCTURE_TYPE_MEMORY_TO_IMAGE_COPY;
					region.pHostPointer = pixels;
					region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
					region.imageSubresource.layerCount = 1;
					region.imageExtent = { width, height, 1 };
					VkCopyMemoryToImageInfo copyInfo{};
					copyInfo.sType = VK_STRUCTURE_TYPE_COPY_MEMORY_TO_IMAGE_INFO;
					copyInfo.dstImage = m_image;
					copyInfo.dstImageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
					copyInfo.regionCount = 1;
					copyInfo.pRegions = &region;
					if (vkCopyMemoryToImage(context.device, &copyInfo) == VK_SUCCESS) {
						m_imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
						return true;
					}
				}
			}

			VkBuffer staging = VK_NULL_HANDLE;
			VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
			try {
				const VkDeviceSize size = static_cast<VkDeviceSize>(width) * static_cast<VkDeviceSize>(height) * 4;
				VulkanResources::CreateBuffer(
					context,
					size,
					VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
					VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
					staging,
					stagingMemory);

				void* data = nullptr;
				if(vkMapMemory(context.device, stagingMemory, 0, size, 0, &data) != VK_SUCCESS || data == nullptr)
					throw std::runtime_error("failed to map Vulkan texture staging memory");
				memcpy(data, pixels, static_cast<size_t>(size));
				vkUnmapMemory(context.device, stagingMemory);

				VulkanResources::TransitionImageLayout(context, commandPool, m_image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
				VulkanResources::CopyBufferToImage(context, commandPool, staging, m_image, width, height);
				VulkanResources::TransitionImageLayout(context, commandPool, m_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
				m_imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

				vkDestroyBuffer(context.device, staging, nullptr);
				vkFreeMemory(context.device, stagingMemory, nullptr);
			} catch (const std::exception& e) {
				if (staging != VK_NULL_HANDLE) vkDestroyBuffer(context.device, staging, nullptr);
				if (stagingMemory != VK_NULL_HANDLE) vkFreeMemory(context.device, stagingMemory, nullptr);
				SDL_Log("Vulkan texture upload failed: %s", e.what());
				return false;
			}

			return true;
		}

		VkImageView GetImageView() const { return m_imageView; }
		VkImage GetImage() const { return m_image; }
		VkSampler GetSampler() const { return m_sampler; }
		VkImageLayout GetImageLayout() const { return m_imageLayout; }
		VkFormat GetVkFormat() const { return m_vkFormat; }

		void SetImageLayout(VkImageLayout layout)
		{
			m_imageLayout = layout;
		}

	private:
		void Cleanup()
		{
			if (m_device == VK_NULL_HANDLE) return;
			if (m_sampler != VK_NULL_HANDLE) {
				vkDestroySampler(m_device, m_sampler, nullptr);
				m_sampler = VK_NULL_HANDLE;
			}
			if (m_imageView != VK_NULL_HANDLE) {
				vkDestroyImageView(m_device, m_imageView, nullptr);
				m_imageView = VK_NULL_HANDLE;
			}
			if (m_image != VK_NULL_HANDLE) {
				vkDestroyImage(m_device, m_image, nullptr);
				m_image = VK_NULL_HANDLE;
			}
			if (m_memory != VK_NULL_HANDLE) {
				vkFreeMemory(m_device, m_memory, nullptr);
				m_memory = VK_NULL_HANDLE;
			}
		}

		VkDevice m_device = VK_NULL_HANDLE;
		VkImage m_image = VK_NULL_HANDLE;
		VkDeviceMemory m_memory = VK_NULL_HANDLE;
		VkImageView m_imageView = VK_NULL_HANDLE;
		VkSampler m_sampler = VK_NULL_HANDLE;
		VkFormat m_vkFormat = VK_FORMAT_UNDEFINED;
		VkImageLayout m_imageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		bool m_hostImageCopyEnabled = false;
	};

	class VulkanPipelineState final : public dy::RHI::IPipelineState
	{
	public:
		VulkanPipelineState(
			const VulkanContext& context,
			VkFormat colorAttachmentFormat,
			VkFormat depthAttachmentFormat,
			VkDescriptorSetLayout descriptorSetLayout,
			VkDescriptorSetLayout bindlessDescriptorSetLayout,
			const dy::RHI::GraphicsPipelineDesc& desc)
			: m_device(context.device)
		{
			CopyPipelineDesc(desc);
			m_pipelineCache.reserve(4);
			const uint32_t colorAttachmentCount = colorAttachmentFormat != VK_FORMAT_UNDEFINED ? 1u : 0u;
			if (GetPipelineForFormats(
				context,
				colorAttachmentCount > 0u ? &colorAttachmentFormat : nullptr,
				colorAttachmentCount,
				depthAttachmentFormat,
				descriptorSetLayout,
				bindlessDescriptorSetLayout) == nullptr) {
				throw std::runtime_error("failed to create graphics pipeline");
			}
		}

		~VulkanPipelineState() override
		{
			for (PipelineCacheEntry& entry : m_pipelineCache) {
				entry.pipeline.Cleanup(m_device);
			}
			m_pipelineCache.clear();
		}

		const VulkanPipeline* GetPipelineForFormats(
			const VulkanContext& context,
			const VkFormat* colorAttachmentFormats,
			uint32_t colorAttachmentCount,
			VkFormat depthAttachmentFormat,
			VkDescriptorSetLayout descriptorSetLayout,
			VkDescriptorSetLayout bindlessDescriptorSetLayout) const
		{
			if(colorAttachmentCount > 0u && colorAttachmentFormats == nullptr) return nullptr;
			for (const PipelineCacheEntry& entry : m_pipelineCache) {
				if (entry.colorAttachmentFormats.size() == colorAttachmentCount
					&& entry.depthAttachmentFormat == depthAttachmentFormat
					&& (colorAttachmentCount == 0u
						|| std::equal(
							entry.colorAttachmentFormats.begin(),
							entry.colorAttachmentFormats.end(),
							colorAttachmentFormats))) return &entry.pipeline;
			}

			try {
				PipelineCacheEntry entry = {};
				if(colorAttachmentCount > 0u)
					entry.colorAttachmentFormats.assign(
						colorAttachmentFormats,
						colorAttachmentFormats + colorAttachmentCount);
				entry.depthAttachmentFormat = depthAttachmentFormat;
				entry.pipeline.Initialize(
					context,
					colorAttachmentFormats,
					colorAttachmentCount,
					depthAttachmentFormat,
					descriptorSetLayout,
					m_desc,
					bindlessDescriptorSetLayout);
				m_pipelineCache.push_back(std::move(entry));
				return &m_pipelineCache.back().pipeline;
			} catch (const std::exception& e) {
				SDL_Log("Vulkan pipeline variant creation failed: %s", e.what());
				return nullptr;
			}
		}

		bool UsesBindlessTextures() const { return m_desc.enableBindlessTextures; }
		dy::RHI::GraphicsResourceProfile GetResourceProfile() const { return m_desc.resourceProfile; }

	private:
		struct PipelineCacheEntry
		{
			std::vector<VkFormat> colorAttachmentFormats;
			VkFormat depthAttachmentFormat = VK_FORMAT_UNDEFINED;
			VulkanPipeline pipeline;
		};

		static void CopyShaderBytes(const void* source, size_t size, std::vector<uint8_t>& destination)
		{
			destination.clear();
			if (source == nullptr || size == 0) return;
			const uint8_t* begin = static_cast<const uint8_t*>(source);
			destination.assign(begin, begin + size);
		}

		void CopyPipelineDesc(const dy::RHI::GraphicsPipelineDesc& desc)
		{
			m_desc = desc;
			CopyShaderBytes(desc.vertexShader, desc.vertexShaderSize, m_vertexShader);
			CopyShaderBytes(desc.pixelShader, desc.pixelShaderSize, m_pixelShader);

			m_desc.vertexShader = m_vertexShader.empty() ? nullptr : m_vertexShader.data();
			m_desc.vertexShaderSize = m_vertexShader.size();
			m_desc.pixelShader = m_pixelShader.empty() ? nullptr : m_pixelShader.data();
			m_desc.pixelShaderSize = m_pixelShader.size();
		}

		VkDevice m_device = VK_NULL_HANDLE;
		dy::RHI::GraphicsPipelineDesc m_desc = {};
		std::vector<uint8_t> m_vertexShader;
		std::vector<uint8_t> m_pixelShader;
		mutable std::vector<PipelineCacheEntry> m_pipelineCache;
	};

	class VulkanComputePipelineState final : public dy::RHI::IPipelineState
	{
	public:
		VulkanComputePipelineState(
			const VulkanContext& context,
			const dy::RHI::ComputePipelineDesc& desc,
			uint32_t frameCount,
			uint32_t descriptorCapacityPerFrame)
			: m_device(context.device)
			, m_storageBufferCount(desc.storageBufferCount)
			, m_inlineConstantSize(desc.inlineConstantSize)
			, m_descriptorCapacityPerFrame(descriptorCapacityPerFrame)
		{
			try
			{
				std::vector<VkDescriptorSetLayoutBinding> bindings(m_storageBufferCount);
				for(uint32_t binding = 0u; binding < m_storageBufferCount; ++binding)
				{
					bindings[binding].binding = binding;
					bindings[binding].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
					bindings[binding].descriptorCount = 1u;
					bindings[binding].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
				}
				VkDescriptorSetLayoutCreateInfo layoutInfo{};
				layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
				layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
				layoutInfo.pBindings = bindings.data();
				if(vkCreateDescriptorSetLayout(m_device, &layoutInfo, nullptr, &m_descriptorSetLayout) != VK_SUCCESS)
					throw std::runtime_error("failed to create compute descriptor set layout");

				const uint64_t setCount = static_cast<uint64_t>(frameCount) * descriptorCapacityPerFrame;
				const uint64_t descriptorCount = setCount * m_storageBufferCount;
				if(setCount == 0u || setCount > std::numeric_limits<uint32_t>::max()
					|| descriptorCount > std::numeric_limits<uint32_t>::max())
					throw std::runtime_error("compute descriptor pool size overflow");
				VkDescriptorPoolSize poolSize{};
				poolSize.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
				poolSize.descriptorCount = static_cast<uint32_t>(descriptorCount);
				VkDescriptorPoolCreateInfo poolInfo{};
				poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
				poolInfo.maxSets = static_cast<uint32_t>(setCount);
				poolInfo.poolSizeCount = 1u;
				poolInfo.pPoolSizes = &poolSize;
				if(vkCreateDescriptorPool(m_device, &poolInfo, nullptr, &m_descriptorPool) != VK_SUCCESS)
					throw std::runtime_error("failed to create compute descriptor pool");
				m_frameDescriptorSets.resize(frameCount);

				VkPushConstantRange pushConstantRange{};
				pushConstantRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
				pushConstantRange.offset = 0u;
				pushConstantRange.size = m_inlineConstantSize;
				VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
				pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
				pipelineLayoutInfo.setLayoutCount = 1u;
				pipelineLayoutInfo.pSetLayouts = &m_descriptorSetLayout;
				pipelineLayoutInfo.pushConstantRangeCount = m_inlineConstantSize > 0u ? 1u : 0u;
				pipelineLayoutInfo.pPushConstantRanges = m_inlineConstantSize > 0u ? &pushConstantRange : nullptr;
				if(vkCreatePipelineLayout(m_device, &pipelineLayoutInfo, nullptr, &m_pipelineLayout) != VK_SUCCESS)
					throw std::runtime_error("failed to create compute pipeline layout");

				VkShaderModuleCreateInfo shaderModuleInfo{};
				shaderModuleInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
				shaderModuleInfo.codeSize = desc.computeShaderSize;
				shaderModuleInfo.pCode = static_cast<const uint32_t*>(desc.computeShader);
				VkPipelineShaderStageCreateInfo stage{};
				stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
				stage.pNext = &shaderModuleInfo;
				stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
				stage.pName = "main";
				VkComputePipelineCreateInfo pipelineInfo{};
				pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
				pipelineInfo.stage = stage;
				pipelineInfo.layout = m_pipelineLayout;
				const VkResult pipelineResult = vkCreateComputePipelines(
					m_device,
					VK_NULL_HANDLE,
					1u,
					&pipelineInfo,
					nullptr,
					&m_pipeline);
				if(pipelineResult != VK_SUCCESS)
					throw std::runtime_error("failed to create compute pipeline");
			}
			catch(...)
			{
				Cleanup();
				throw;
			}
		}

		~VulkanComputePipelineState() override { Cleanup(); }

		bool EnsureDescriptorSets(uint32_t frameIndex, uint32_t requiredCount)
		{
			if(frameIndex >= m_frameDescriptorSets.size() || requiredCount > m_descriptorCapacityPerFrame) return false;
			std::vector<VkDescriptorSet>& sets = m_frameDescriptorSets[frameIndex];
			if(sets.size() >= requiredCount) return true;
			const uint32_t allocationCount = requiredCount - static_cast<uint32_t>(sets.size());
			std::vector<VkDescriptorSetLayout> layouts(allocationCount, m_descriptorSetLayout);
			std::vector<VkDescriptorSet> allocated(allocationCount, VK_NULL_HANDLE);
			VkDescriptorSetAllocateInfo allocation{};
			allocation.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
			allocation.descriptorPool = m_descriptorPool;
			allocation.descriptorSetCount = allocationCount;
			allocation.pSetLayouts = layouts.data();
			if(vkAllocateDescriptorSets(m_device, &allocation, allocated.data()) != VK_SUCCESS) return false;
			sets.insert(sets.end(), allocated.begin(), allocated.end());
			return true;
		}

		VkDescriptorSet GetDescriptorSet(uint32_t frameIndex, uint32_t index) const
		{
			if(frameIndex >= m_frameDescriptorSets.size() || index >= m_frameDescriptorSets[frameIndex].size())
				return VK_NULL_HANDLE;
			return m_frameDescriptorSets[frameIndex][index];
		}

		VkPipeline GetPipeline() const { return m_pipeline; }
		VkPipelineLayout GetLayout() const { return m_pipelineLayout; }
		uint32_t GetStorageBufferCount() const { return m_storageBufferCount; }
		uint32_t GetInlineConstantSize() const { return m_inlineConstantSize; }

	private:
		void Cleanup()
		{
			if(m_pipeline != VK_NULL_HANDLE) vkDestroyPipeline(m_device, m_pipeline, nullptr);
			if(m_pipelineLayout != VK_NULL_HANDLE) vkDestroyPipelineLayout(m_device, m_pipelineLayout, nullptr);
			if(m_descriptorPool != VK_NULL_HANDLE) vkDestroyDescriptorPool(m_device, m_descriptorPool, nullptr);
			if(m_descriptorSetLayout != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(m_device, m_descriptorSetLayout, nullptr);
			m_pipeline = VK_NULL_HANDLE;
			m_pipelineLayout = VK_NULL_HANDLE;
			m_descriptorPool = VK_NULL_HANDLE;
			m_descriptorSetLayout = VK_NULL_HANDLE;
		}

		VkDevice m_device = VK_NULL_HANDLE;
		VkDescriptorSetLayout m_descriptorSetLayout = VK_NULL_HANDLE;
		VkDescriptorPool m_descriptorPool = VK_NULL_HANDLE;
		VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
		VkPipeline m_pipeline = VK_NULL_HANDLE;
		uint32_t m_storageBufferCount = 0u;
		uint32_t m_inlineConstantSize = 0u;
		uint32_t m_descriptorCapacityPerFrame = 0u;
		std::vector<std::vector<VkDescriptorSet>> m_frameDescriptorSets;
	};

	VkBufferUsageFlags ToVkBufferUsage(dy::RHI::BufferUsage usage)
	{
		VkBufferUsageFlags flags = 0;
		if ((usage & dy::RHI::BufferUsage::Vertex) != dy::RHI::BufferUsage::None) flags |= VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
		if ((usage & dy::RHI::BufferUsage::Index) != dy::RHI::BufferUsage::None) flags |= VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
		if ((usage & dy::RHI::BufferUsage::Constant) != dy::RHI::BufferUsage::None) flags |= VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
		if ((usage & dy::RHI::BufferUsage::Storage) != dy::RHI::BufferUsage::None) flags |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
		if ((usage & dy::RHI::BufferUsage::Indirect) != dy::RHI::BufferUsage::None) flags |= VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
		if (flags == 0) return static_cast<VkBufferUsageFlags>(VK_BUFFER_USAGE_TRANSFER_DST_BIT);
		return flags;
	}

	class VulkanBuffer final : public dy::RHI::IBuffer
	{
	public:
		VulkanBuffer(const VulkanContext& context, VkCommandPool commandPool, const dy::RHI::BufferDesc& desc)
			: dy::RHI::IBuffer(desc), m_context(context), m_device(context.device), m_commandPool(commandPool)
		{
			if(desc.memoryUsage == dy::RHI::BufferMemoryUsage::GpuOnly)
			{
				VulkanResources::CreateBuffer(
					context,
					desc.size,
					ToVkBufferUsage(desc.usage) | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
					VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
					m_buffer,
					m_memory);
				try
				{
					VulkanResources::CreateBuffer(
						context,
						desc.size,
						VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
						VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
						m_stagingBuffer,
						m_stagingMemory);
				}
				catch(...)
				{
					vkDestroyBuffer(m_device, m_buffer, nullptr);
					vkFreeMemory(m_device, m_memory, nullptr);
					m_buffer = VK_NULL_HANDLE;
					m_memory = VK_NULL_HANDLE;
					throw;
				}
			}
			else
			{
				VulkanResources::CreateBuffer(
					context,
					desc.size,
					ToVkBufferUsage(desc.usage),
					VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
					m_buffer,
					m_memory);
			}
		}

		~VulkanBuffer() override
		{
			if (m_mapped != nullptr) vkUnmapMemory(m_device, m_memory);
			if (m_stagingMapped != nullptr) vkUnmapMemory(m_device, m_stagingMemory);
			if (m_stagingBuffer != VK_NULL_HANDLE) vkDestroyBuffer(m_device, m_stagingBuffer, nullptr);
			if (m_stagingMemory != VK_NULL_HANDLE) vkFreeMemory(m_device, m_stagingMemory, nullptr);
			if (m_buffer != VK_NULL_HANDLE) vkDestroyBuffer(m_device, m_buffer, nullptr);
			if (m_memory != VK_NULL_HANDLE) vkFreeMemory(m_device, m_memory, nullptr);
		}

		void* Map(uint32_t offset) override
		{
			if(GetMemoryUsage() == dy::RHI::BufferMemoryUsage::GpuOnly)
			{
				if(m_uploaded || m_stagingMemory == VK_NULL_HANDLE) return nullptr;
				if(m_stagingMapped != nullptr) return static_cast<uint8_t*>(m_stagingMapped) + offset;
				if(vkMapMemory(m_device, m_stagingMemory, 0, VK_WHOLE_SIZE, 0, &m_stagingMapped) != VK_SUCCESS)
				{
					m_stagingMapped = nullptr;
					return nullptr;
				}
				return static_cast<uint8_t*>(m_stagingMapped) + offset;
			}
			if (m_mapped != nullptr) return static_cast<uint8_t*>(m_mapped) + offset;
			if (vkMapMemory(m_device, m_memory, offset, VK_WHOLE_SIZE, 0, &m_mapped) != VK_SUCCESS) {
				m_mapped = nullptr;
				return nullptr;
			}
			return m_mapped;
		}

		void Unmap() override
		{
			if(GetMemoryUsage() == dy::RHI::BufferMemoryUsage::GpuOnly)
			{
				if(m_stagingMapped == nullptr || m_uploaded) return;
				vkUnmapMemory(m_device, m_stagingMemory);
				m_stagingMapped = nullptr;
				VulkanResources::CopyBuffer(m_context, m_commandPool, m_stagingBuffer, m_buffer, GetSize());
				vkDestroyBuffer(m_device, m_stagingBuffer, nullptr);
				vkFreeMemory(m_device, m_stagingMemory, nullptr);
				m_stagingBuffer = VK_NULL_HANDLE;
				m_stagingMemory = VK_NULL_HANDLE;
				m_uploaded = true;
				return;
			}
			if (m_mapped == nullptr) return;
			vkUnmapMemory(m_device, m_memory);
			m_mapped = nullptr;
		}

		VkBuffer GetHandle() const { return m_buffer; }
		bool Read(uint32_t offset, void* destination, uint32_t size) const
		{
			if(destination == nullptr || size == 0u || offset > GetSize() || size > GetSize() - offset) return false;
			void* mapped = nullptr;
			if(vkMapMemory(m_device, m_memory, offset, size, 0, &mapped) != VK_SUCCESS || mapped == nullptr) return false;
			std::memcpy(destination, mapped, size);
			vkUnmapMemory(m_device, m_memory);
			return true;
		}

	private:
		VulkanContext m_context = {};
		VkDevice m_device = VK_NULL_HANDLE;
		VkCommandPool m_commandPool = VK_NULL_HANDLE;
		VkBuffer m_buffer = VK_NULL_HANDLE;
		VkDeviceMemory m_memory = VK_NULL_HANDLE;
		void* m_mapped = nullptr;
		VkBuffer m_stagingBuffer = VK_NULL_HANDLE;
		VkDeviceMemory m_stagingMemory = VK_NULL_HANDLE;
		void* m_stagingMapped = nullptr;
		bool m_uploaded = false;
	};

	void CmdTransitionImageLayout(
		VkCommandBuffer commandBuffer,
		VkImage image,
		VkImageAspectFlags aspectMask,
		VkImageLayout oldLayout,
		VkImageLayout newLayout)
	{
		VkImageMemoryBarrier2 barrier{};
		barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
		barrier.oldLayout = oldLayout;
		barrier.newLayout = newLayout;
		barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.image = image;
		barrier.subresourceRange.aspectMask = aspectMask;
		barrier.subresourceRange.levelCount = 1;
		barrier.subresourceRange.layerCount = 1;

		switch (oldLayout) {
		case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
			barrier.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
			barrier.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
			break;
		case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
			barrier.srcStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
			barrier.srcAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
			break;
		case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
			barrier.srcStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
			barrier.srcAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
			break;
		default:
			barrier.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
			barrier.srcAccessMask = VK_ACCESS_2_NONE;
			break;
		}

		switch (newLayout) {
		case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
			barrier.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
			barrier.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
			break;
		case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
			barrier.dstStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
			barrier.dstAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
			break;
		case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
			barrier.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
			barrier.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
			break;
		default:
			barrier.dstStageMask = VK_PIPELINE_STAGE_2_NONE;
			barrier.dstAccessMask = VK_ACCESS_2_NONE;
			break;
		}

		VkDependencyInfo dependencyInfo{};
		dependencyInfo.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
		dependencyInfo.imageMemoryBarrierCount = 1;
		dependencyInfo.pImageMemoryBarriers = &barrier;
		vkCmdPipelineBarrier2(commandBuffer, &dependencyInfo);
	}
}

struct VulkanDevice::Impl
{
	explicit Impl(VulkanDevice& owner);
	~Impl();

	int Initialize(const void* windowHandle, const dy::RHI::DeviceDesc& desc);
	void BeginFrame();
	uint32_t GetCurrentFrameIndex() const { return m_currentFrameIndex; }
	dy::RHI::ICommandList* AcquireCommandList() { return m_commandList; }
	void Submit(dy::RHI::ICommandList** cmdLists, uint32_t count);
	void Present();
	[[nodiscard]] bool SupportsGpuTimestamps() const { return m_gpuTimestampsSupported; }
	[[nodiscard]] uint32_t GetMaxGpuTimestampScopes() const { return m_gpuTimestampsSupported ? 32u : 0u; }
	[[nodiscard]] bool TryGetLastGpuTimestamp(const char* name, dy::RHI::GpuTimestampResult& result) const;

	dy::RHI::IBuffer* CreateBuffer(const dy::RHI::BufferDesc& desc);
	dy::RHI::ITexture* CreateTexture(const dy::RHI::TextureDesc& desc);
	bool UpdateTexture(dy::RHI::ITexture* texture, const void* rgba8Pixels, uint32_t rowPitch);
	bool ReadbackTextureRGBA32Float(dy::RHI::ITexture* texture, std::vector<float>& outPixels);
	dy::RHI::IPipelineState* CreateGraphicsPipeline(const dy::RHI::GraphicsPipelineDesc& desc);
	dy::RHI::IPipelineState* CreateComputePipeline(const dy::RHI::ComputePipelineDesc& desc);
	[[nodiscard]] dy::RHI::DescriptorIndex AllocateDescriptorSlot();
	void UpdateDescriptorSlot(dy::RHI::DescriptorIndex index, dy::RHI::ITexture* texture);
	void DestroyBuffer(dy::RHI::IBuffer* buffer);
	void DestroyTexture(dy::RHI::ITexture* texture);
	void DestroyPipelineState(dy::RHI::IPipelineState* pipeline);
	[[nodiscard]] dy::RHI::ITexture* GetBackBuffer();
	[[nodiscard]] bool SupportsSkinningStorageBindings() const { return m_context.device != VK_NULL_HANDLE; }
	[[nodiscard]] bool SupportsComputeSkinning() const { return m_capabilities.supportsComputeSkinning; }
	[[nodiscard]] uint32_t GetValidationErrorCount() const { return m_validationErrorCount.load(); }
	[[nodiscard]] uint32_t GetValidationVuidCount() const { return m_validationVuidCount.load(); }
	[[nodiscard]] bool IsValidationCaptureEnabled() const { return m_validationEnabled && m_debugMessenger != VK_NULL_HANDLE; }
	[[nodiscard]] bool IsDeviceLost() const { return m_deviceLost; }

private:
	struct ProfileResources
	{
		VkDescriptorSetLayout descriptorSetLayout = VK_NULL_HANDLE;
	};

	bool CreateInstance();
	void DestroyDebugMessenger();
	static VKAPI_ATTR VkBool32 VKAPI_CALL DebugUtilsCallback(
		VkDebugUtilsMessageSeverityFlagBitsEXT severity,
		VkDebugUtilsMessageTypeFlagsEXT,
		const VkDebugUtilsMessengerCallbackDataEXT* callbackData,
		void* userData);
	bool CreateSurface();
	bool PickPhysicalDevice();
	bool CreateLogicalDevice();
	bool CreateSyncObjects();
	bool EnsureProfileResources(dy::RHI::GraphicsResourceProfile profile);
	bool CreateDescriptorSetLayout(dy::RHI::GraphicsResourceProfile profile, VkDescriptorSetLayout& outLayout);
	bool CreateBindlessDescriptorSetLayout();
	bool CreateBindlessDescriptorPool();
	ProfileResources* GetProfileResources(dy::RHI::GraphicsResourceProfile profile);
	const ProfileResources* GetProfileResources(dy::RHI::GraphicsResourceProfile profile) const;
	bool CreateBindlessDescriptorSet();
	bool CreateDepthResources();
	bool CreateCommandPool();
	bool CreateCommandBuffer();
	bool CreateGpuTimestampQueryPools();
	bool CreateFallbackTexture();
	bool CreateDrawConstantBuffers();
	bool UploadDrawConstants(const VulkanCommandList& commandList);
	void DestroyDrawConstantBuffers();
	void CollectRetiredBuffers();
	void DestroyAllRetiredBuffers();

	bool RecreateSwapchain();
	void DestroySwapchainResources();
	void DestroyDeviceResources();
	bool AcquireFrameImage();
	bool RecreateCurrentFrameFenceSignaled();
	void RecoverAbortedAcquiredFrame(bool recreateFence);

	struct RenderingColorTarget
	{
		VkImage image = VK_NULL_HANDLE;
		VkImageView view = VK_NULL_HANDLE;
		VulkanTexture* texture = nullptr;
		bool isSwapchain = false;
	};

	struct RenderingTarget
	{
		std::vector<RenderingColorTarget> colors;
		std::vector<VkFormat> colorFormats;
		VulkanTexture* depthTexture = nullptr;
		VkExtent2D extent = {};
	};

	bool RecordCommandBuffer(const VulkanCommandList& commandList);
	bool RecordComputeDispatch(VkCommandBuffer commandBuffer, const VulkanCommandList& commandList, uint32_t dispatchIndex);
	bool RecordBufferBarrier(VkCommandBuffer commandBuffer, const VulkanCommandList& commandList, uint32_t barrierIndex);
	bool RecordColorClear(VkCommandBuffer commandBuffer, const VulkanCommandList& commandList, uint32_t clearIndex);
	bool RecordDepthClear(VkCommandBuffer commandBuffer, const VulkanCommandList& commandList, uint32_t clearIndex);
	bool RecordGraphicsPass(VkCommandBuffer commandBuffer, const VulkanCommandList& commandList, uint32_t firstDraw, uint32_t drawCount);
	bool ResolveGraphicsTarget(
		const VulkanCommandList& commandList,
		const VulkanCommandList::DrawCall& drawCall,
		RenderingTarget& target);
	bool PushDrawDescriptors(
		VkCommandBuffer commandBuffer,
		VkPipelineLayout pipelineLayout,
		const VulkanCommandList::DrawCall& drawCall,
		uint32_t drawConstantSlot);
	bool UpdateComputeDescriptorSets(const VulkanCommandList& commandList);
	bool ApplyBindlessDescriptorSet(uint32_t frameIndex);
	void RecordDebugEventsAt(VkCommandBuffer commandBuffer, const VulkanCommandList& commandList, uint32_t eventIndex);
	void RecordGpuTimestampEventsAt(VkCommandBuffer commandBuffer, const VulkanCommandList& commandList, uint32_t eventIndex);
	void CollectGpuTimestampResults(uint32_t frameIndex);
	void BeginDebugLabel(VkCommandBuffer commandBuffer, const char* name, const dy::RHI::DebugLabelColor& color) const;
	void EndDebugLabel(VkCommandBuffer commandBuffer) const;
	void InsertDebugLabel(VkCommandBuffer commandBuffer, const char* name, const dy::RHI::DebugLabelColor& color) const;
	void UpdateBackBufferMetadata();
	void SetRecordedImageLayout(VulkanTexture* texture, VkImageLayout layout);
	void RollbackRecordedImageLayouts();
	void CommitRecordedImageLayouts();
	VkFormat FindDepthFormat() const;
	bool IsDepthFormatSupported(VkFormat format) const;

	VulkanDevice& m_owner;
	VulkanContext m_context;
	VkDebugUtilsMessengerEXT m_debugMessenger = VK_NULL_HANDLE;
	VulkanSwapchain m_swapchain;
	bool m_optimalRgba8HostImageCopy = false;
	PFN_vkCmdBeginDebugUtilsLabelEXT m_vkCmdBeginDebugUtilsLabel = nullptr;
	PFN_vkCmdEndDebugUtilsLabelEXT m_vkCmdEndDebugUtilsLabel = nullptr;
	PFN_vkCmdInsertDebugUtilsLabelEXT m_vkCmdInsertDebugUtilsLabel = nullptr;

	void* m_windowHandle = nullptr;

	VkCommandPool m_commandPool = VK_NULL_HANDLE;
	std::vector<VkCommandBuffer> m_commandBuffers;
	static constexpr uint32_t kMaxGpuTimestampQueriesPerFrame = 64u;
	struct GpuTimestampPair
	{
		std::string name;
		uint32_t beginQuery = 0;
		uint32_t endQuery = 0;
	};
	struct OpenGpuTimestamp
	{
		std::string name;
		uint32_t beginQuery = UINT32_MAX;
	};
	std::vector<VkQueryPool> m_gpuTimestampQueryPools;
	std::vector<std::vector<GpuTimestampPair>> m_gpuTimestampPairs;
	std::vector<uint32_t> m_gpuTimestampQueryCounts;
	std::vector<OpenGpuTimestamp> m_recordingGpuTimestampStack;
	std::unordered_map<std::string, dy::RHI::GpuTimestampResult> m_completedGpuTimestamps;
	uint32_t m_recordingGpuTimestampQuery = 0;
	uint32_t m_timestampValidBits = 0;
	double m_timestampPeriodNanoseconds = 0.0;
	uint64_t m_timestampFrameSerial = 0;
	bool m_gpuTimestampsSupported = false;

	VkFormat m_depthFormat = VK_FORMAT_UNDEFINED;

	std::array<ProfileResources, kGraphicsResourceProfileCount> m_profileResources = {};
	VkDescriptorSetLayout m_bindlessDescriptorSetLayout = VK_NULL_HANDLE;
	VkDescriptorPool m_bindlessDescriptorPool = VK_NULL_HANDLE;
	std::vector<VkDescriptorSet> m_bindlessDescriptorSets;
	std::vector<const VulkanTexture*> m_bindlessTextures;
	std::vector<uint64_t> m_bindlessDescriptorFrameRevisions;
	uint64_t m_bindlessDescriptorRevision = 1u;

	struct DrawConstantFrame
	{
		VkBuffer buffer = VK_NULL_HANDLE;
		VkDeviceMemory memory = VK_NULL_HANDLE;
		uint8_t* mapped = nullptr;
	};
	std::vector<DrawConstantFrame> m_drawConstantFrames;
	VkDeviceSize m_drawConstantStride = 0u;
	uint32_t m_drawConstantCapacity = 0u;

	std::vector<VkSemaphore> m_imageAvailableSemaphores;
	std::vector<VkSemaphore> m_renderFinishedSemaphores;
	std::vector<VkFence> m_inFlightFences;
	std::vector<VkFence> m_imagesInFlight;

	std::vector<dy::RHI::IBuffer*> m_ownedBuffers;
	struct RetiredBuffer
	{
		dy::RHI::IBuffer* buffer = nullptr;
		std::vector<uint32_t> pendingFrameIndices;
	};
	std::vector<RetiredBuffer> m_retiredBuffers;

	struct RecordedImageLayout
	{
		VulkanTexture* texture = nullptr;
		VkImageLayout originalLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	};
	std::vector<RecordedImageLayout> m_recordedImageLayouts;
	RenderingTarget m_renderingTargetScratch;
	std::vector<VkRenderingAttachmentInfo> m_colorAttachmentsScratch;

	uint32_t m_maxFramesInFlight = dy::RHI::DeviceDesc{}.maxFramesInFlight;
	uint32_t m_maxDrawsPerFrame = dy::RHI::DeviceDesc{}.maxDrawsPerFrame;
	uint32_t m_maxBindlessTextures = dy::RHI::DeviceDesc{}.maxBindlessTextures;
	uint32_t m_maxColorAttachments = 0u;
	uint32_t m_descriptorCapacityPerFrame = 0u;
	uint64_t m_frameAcquireTimeoutNanoseconds = dy::RHI::DeviceDesc{}.frameAcquireTimeoutNanoseconds;
	VulkanCapabilities m_capabilities = {};
	bool m_validationEnabled = false;
	bool m_debugUtilsEnabled = false;
	std::atomic<uint32_t> m_validationErrorCount = 0u;
	std::atomic<uint32_t> m_validationVuidCount = 0u;
	dy::RHI::ShaderLayoutDesc m_shaderLayout = {};
	uint32_t m_currentFrameIndex = 0;
	uint32_t m_currentImageIndex = 0;
	VkImageLayout m_recordedSwapchainLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	bool m_frameReady = false;
	bool m_frameSubmitted = false;
	bool m_imageAcquired = false;
	bool m_deviceLost = false;
	bool m_drawCapacityErrorReported = false;
	dy::RHI::DescriptorIndex m_nextDescriptorIndex = 0;
	VulkanCommandList* m_commandList = nullptr;
	dy::RHI::ITexture* m_backBuffer = nullptr;
	dy::RHI::ITexture* m_fallbackTexture = nullptr;
	dy::RHI::ITexture* m_depthTexture = nullptr;
	std::vector<dy::RHI::ITexture*> m_ownedTextures;
	std::vector<dy::RHI::IPipelineState*> m_ownedPipelineStates;
};

VulkanDevice::VulkanDevice()
	: m_impl(std::make_unique<Impl>(*this))
{
}

VulkanDevice::~VulkanDevice() = default;

void VulkanDevice::BeginFrame()
{
	m_impl->BeginFrame();
}

uint32_t VulkanDevice::GetCurrentFrameIndex() const
{
	return m_impl->GetCurrentFrameIndex();
}

dy::RHI::ICommandList* VulkanDevice::AcquireCommandList()
{
	return m_impl->AcquireCommandList();
}

void VulkanDevice::Submit(dy::RHI::ICommandList** cmdLists, uint32_t count)
{
	m_impl->Submit(cmdLists, count);
}

void VulkanDevice::Present()
{
	m_impl->Present();
}

dy::RHI::IBuffer* VulkanDevice::CreateBuffer(const dy::RHI::BufferDesc& desc)
{
	dy::RHI::IBuffer* buffer = m_impl->CreateBuffer(desc);
	TrackBufferCreated(buffer);
	return buffer;
}

dy::RHI::ITexture* VulkanDevice::CreateTexture(const dy::RHI::TextureDesc& desc)
{
	dy::RHI::ITexture* texture = m_impl->CreateTexture(desc);
	TrackTextureCreated(texture);
	return texture;
}

bool VulkanDevice::UpdateTexture(dy::RHI::ITexture* texture, const void* rgba8Pixels, uint32_t rowPitch)
{
	return m_impl->UpdateTexture(texture, rgba8Pixels, rowPitch);
}

dy::RHI::IPipelineState* VulkanDevice::CreateGraphicsPipeline(const dy::RHI::GraphicsPipelineDesc& desc)
{
	dy::RHI::IPipelineState* pipeline = m_impl->CreateGraphicsPipeline(desc);
	TrackPipelineCreated(pipeline);
	return pipeline;
}

dy::RHI::IPipelineState* VulkanDevice::CreateComputePipeline(const dy::RHI::ComputePipelineDesc& desc)
{
	auto* pipeline = m_impl->CreateComputePipeline(desc);
	TrackPipelineCreated(pipeline);
	return pipeline;
}

dy::RHI::DescriptorIndex VulkanDevice::AllocateDescriptorSlot()
{
	return m_impl->AllocateDescriptorSlot();
}

void VulkanDevice::UpdateDescriptorSlot(dy::RHI::DescriptorIndex index, dy::RHI::ITexture* texture)
{
	m_impl->UpdateDescriptorSlot(index, texture);
}

void VulkanDevice::DestroyBuffer(dy::RHI::IBuffer* buffer)
{
	if(TrackBufferDestroyed(buffer)) m_impl->DestroyBuffer(buffer);
}

void VulkanDevice::DestroyTexture(dy::RHI::ITexture* texture)
{
	if(TrackTextureDestroyed(texture)) m_impl->DestroyTexture(texture);
}

void VulkanDevice::DestroyPipelineState(dy::RHI::IPipelineState* pipeline)
{
	if(TrackPipelineDestroyed(pipeline)) m_impl->DestroyPipelineState(pipeline);
}

dy::RHI::ITexture* VulkanDevice::GetBackBuffer()
{
	return m_impl->GetBackBuffer();
}

bool VulkanDevice::SupportsSkinningStorageBindings() const
{
	return m_impl != nullptr && m_impl->SupportsSkinningStorageBindings();
}

bool VulkanDevice::SupportsComputeSkinning() const
{
	return m_impl != nullptr && m_impl->SupportsComputeSkinning();
}

uint32_t VulkanDevice::GetValidationErrorCount() const
{
	return m_impl != nullptr ? m_impl->GetValidationErrorCount() : 0u;
}

uint32_t VulkanDevice::GetValidationVuidCount() const
{
	return m_impl != nullptr ? m_impl->GetValidationVuidCount() : 0u;
}

bool VulkanDevice::IsValidationCaptureEnabled() const
{
	return m_impl != nullptr && m_impl->IsValidationCaptureEnabled();
}

bool VulkanDevice::IsDeviceLost() const
{
	return m_impl != nullptr && m_impl->IsDeviceLost();
}

bool VulkanDevice::ReadbackTextureRGBA32Float(
	dy::RHI::ITexture* texture,
	std::vector<float>& outPixels)
{
	return m_impl != nullptr && m_impl->ReadbackTextureRGBA32Float(texture, outPixels);
}

bool VulkanDevice::SupportsGpuTimestamps() const
{
	return m_impl->SupportsGpuTimestamps();
}

uint32_t VulkanDevice::GetMaxGpuTimestampScopes() const
{
	return m_impl->GetMaxGpuTimestampScopes();
}

bool VulkanDevice::TryGetLastGpuTimestamp(const char* name, dy::RHI::GpuTimestampResult& result) const
{
	return m_impl->TryGetLastGpuTimestamp(name, result);
}

int VulkanDevice::Initialize(const void* windowHandle, const dy::RHI::DeviceDesc& desc)
{
	return m_impl->Initialize(windowHandle, desc);
}

VulkanDevice::Impl::Impl(VulkanDevice& owner)
	: m_owner(owner)
{
	m_commandList = new VulkanCommandList();
}

VulkanDevice::Impl::~Impl() {
	DestroyDeviceResources();
}

int VulkanDevice::Impl::Initialize(const void* windowHandle, const dy::RHI::DeviceDesc& desc) {
	m_windowHandle = const_cast<void*>(windowHandle);
	if (!m_windowHandle) return -1;
	if(!TryComputeVulkanDescriptorCapacity(
		desc,
		m_descriptorCapacityPerFrame))
	{
		SDL_Log("Invalid Vulkan descriptor configuration: frames and draws must be non-zero and total capacity must fit uint32.");
		return -1;
	}
	m_maxFramesInFlight = desc.maxFramesInFlight;
	m_maxDrawsPerFrame = desc.maxDrawsPerFrame;
	m_maxBindlessTextures = desc.maxBindlessTextures;
	m_frameAcquireTimeoutNanoseconds = desc.frameAcquireTimeoutNanoseconds;
	m_shaderLayout = desc.shaderLayout;

	try {
		if (!CreateInstance()) return -1;
		if (!CreateSurface()) return -1;
		if (!PickPhysicalDevice()) return -1;
		if (!CreateLogicalDevice()) return -1;
		if (!CreateCommandPool()) return -1;

		const VkResult swapchainResult = m_swapchain.Initialize(
			m_context,
			m_windowHandle,
			dy::RHI::IsSrgbFormat(m_owner.GetDesc().swapchainFormat));
		if(swapchainResult != VK_SUCCESS)
		{
			if(swapchainResult == VK_ERROR_DEVICE_LOST) m_deviceLost = true;
			SDL_Log("Failed to create Vulkan swapchain: %s (%d).",
				VkResultToString(swapchainResult),
				static_cast<int>(swapchainResult));
			return -1;
		}
		UpdateBackBufferMetadata();

		m_depthFormat = FindDepthFormat();
		if (!CreateFallbackTexture()) return -1;
		if (!CreateDrawConstantBuffers()) return -1;
		if (!CreateBindlessDescriptorSetLayout()) return -1;
		if (!CreateBindlessDescriptorPool()) return -1;
		if (!CreateBindlessDescriptorSet()) return -1;
		if (!CreateDepthResources()) return -1;
		if (!CreateCommandBuffer()) return -1;
		if (!CreateGpuTimestampQueryPools()) return -1;
		if (!CreateSyncObjects()) return -1;
	} catch (const std::exception& e) {
		SDL_Log("Vulkan Initialization failed: %s", e.what());
		return -1;
	}

	return 0;
}

dy::RHI::ITexture* VulkanDevice::Impl::CreateTexture(const dy::RHI::TextureDesc& desc) {
	std::unique_ptr<VulkanTexture> texture(new VulkanTexture(desc));
	if (!texture->Initialize(m_context, desc, VK_FORMAT_UNDEFINED, 0, m_optimalRgba8HostImageCopy)) {
		return nullptr;
	}
	if (HasTextureUsage(desc.usage, dy::RHI::TextureUsage::ShaderResource) && !texture->CreateDefaultSampler()) {
		return nullptr;
	}

	m_ownedTextures.push_back(texture.get());
	return texture.release();
}

bool VulkanDevice::Impl::UpdateTexture(dy::RHI::ITexture* texture, const void* rgba8Pixels, uint32_t rowPitch) {
	VulkanTexture* vulkanTexture = dynamic_cast<VulkanTexture*>(texture);
	if (vulkanTexture == nullptr || rgba8Pixels == nullptr) return false;
	if (rowPitch != vulkanTexture->GetWidth() * 4u) {
		SDL_Log("Vulkan texture upload currently expects tightly packed RGBA8 rows.");
		return false;
	}

	return vulkanTexture->UploadRGBA8(
		m_context,
		m_commandPool,
		rgba8Pixels,
		vulkanTexture->GetWidth(),
		vulkanTexture->GetHeight());
}

bool VulkanDevice::Impl::ReadbackTextureRGBA32Float(
	dy::RHI::ITexture* texture,
	std::vector<float>& outPixels)
{
	outPixels.clear();
	VulkanTexture* vulkanTexture = dynamic_cast<VulkanTexture*>(texture);
	if(vulkanTexture == nullptr
		|| vulkanTexture->GetFormat() != dy::RHI::Format::R32G32B32A32_FLOAT
		|| !HasTextureUsage(vulkanTexture->GetUsage(), dy::RHI::TextureUsage::RenderTarget)
		|| vulkanTexture->GetImage() == VK_NULL_HANDLE
		|| vulkanTexture->GetImageLayout() == VK_IMAGE_LAYOUT_UNDEFINED)
	{
		return false;
	}

	const uint64_t floatCount = static_cast<uint64_t>(vulkanTexture->GetWidth())
		* static_cast<uint64_t>(vulkanTexture->GetHeight()) * 4u;
	const uint64_t byteCount = floatCount * sizeof(float);
	if(floatCount == 0u
		|| floatCount > std::numeric_limits<size_t>::max()
		|| byteCount > std::numeric_limits<VkDeviceSize>::max())
	{
		return false;
	}

	VkBuffer stagingBuffer = VK_NULL_HANDLE;
	VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
	try
	{
		const VkImageLayout originalLayout = vulkanTexture->GetImageLayout();
		VulkanResources::CreateBuffer(
			m_context,
			static_cast<VkDeviceSize>(byteCount),
			VK_BUFFER_USAGE_TRANSFER_DST_BIT,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
			stagingBuffer,
			stagingMemory);

		VkCommandBuffer commandBuffer =
			VulkanResources::BeginSingleTimeCommands(m_context, m_commandPool);
		VkImageMemoryBarrier2 toTransfer{};
		toTransfer.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
		const bool shaderReadLayout = originalLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		toTransfer.srcStageMask = shaderReadLayout
			? VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT
			: VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
		toTransfer.srcAccessMask = shaderReadLayout
			? VK_ACCESS_2_SHADER_READ_BIT
			: VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
		toTransfer.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
		toTransfer.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
		toTransfer.oldLayout = originalLayout;
		toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
		toTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		toTransfer.image = vulkanTexture->GetImage();
		toTransfer.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		toTransfer.subresourceRange.levelCount = 1u;
		toTransfer.subresourceRange.layerCount = 1u;
		VkDependencyInfo toTransferDependency{};
		toTransferDependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
		toTransferDependency.imageMemoryBarrierCount = 1u;
		toTransferDependency.pImageMemoryBarriers = &toTransfer;
		vkCmdPipelineBarrier2(commandBuffer, &toTransferDependency);

		VkBufferImageCopy2 copyRegion{};
		copyRegion.sType = VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2;
		copyRegion.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		copyRegion.imageSubresource.layerCount = 1u;
		copyRegion.imageExtent = {
			vulkanTexture->GetWidth(),
			vulkanTexture->GetHeight(),
			1u };
		VkCopyImageToBufferInfo2 copyInfo{};
		copyInfo.sType = VK_STRUCTURE_TYPE_COPY_IMAGE_TO_BUFFER_INFO_2;
		copyInfo.srcImage = vulkanTexture->GetImage();
		copyInfo.srcImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
		copyInfo.dstBuffer = stagingBuffer;
		copyInfo.regionCount = 1u;
		copyInfo.pRegions = &copyRegion;
		vkCmdCopyImageToBuffer2(commandBuffer, &copyInfo);

		VkImageMemoryBarrier2 toShaderRead = toTransfer;
		toShaderRead.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
		toShaderRead.srcAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
		toShaderRead.dstStageMask = shaderReadLayout
			? VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT
			: VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
		toShaderRead.dstAccessMask = shaderReadLayout
			? VK_ACCESS_2_SHADER_READ_BIT
			: VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
		toShaderRead.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
		toShaderRead.newLayout = originalLayout;
		VkDependencyInfo toOriginalDependency{};
		toOriginalDependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
		toOriginalDependency.imageMemoryBarrierCount = 1u;
		toOriginalDependency.pImageMemoryBarriers = &toShaderRead;
		vkCmdPipelineBarrier2(commandBuffer, &toOriginalDependency);
		VulkanResources::EndSingleTimeCommands(m_context, m_commandPool, commandBuffer);
		vulkanTexture->SetImageLayout(originalLayout);

		void* mapped = nullptr;
		if(vkMapMemory(
			m_context.device,
			stagingMemory,
			0u,
			static_cast<VkDeviceSize>(byteCount),
			0u,
			&mapped) != VK_SUCCESS || mapped == nullptr)
		{
			throw std::runtime_error("failed to map Vulkan float texture readback buffer");
		}
		outPixels.resize(static_cast<size_t>(floatCount));
		std::memcpy(outPixels.data(), mapped, static_cast<size_t>(byteCount));
		vkUnmapMemory(m_context.device, stagingMemory);
		vkDestroyBuffer(m_context.device, stagingBuffer, nullptr);
		vkFreeMemory(m_context.device, stagingMemory, nullptr);
		return true;
	}
	catch(const std::exception& e)
	{
		if(stagingBuffer != VK_NULL_HANDLE) vkDestroyBuffer(m_context.device, stagingBuffer, nullptr);
		if(stagingMemory != VK_NULL_HANDLE) vkFreeMemory(m_context.device, stagingMemory, nullptr);
		SDL_Log("Vulkan float texture readback failed: %s", e.what());
		outPixels.clear();
		return false;
	}
}

dy::RHI::IPipelineState* VulkanDevice::Impl::CreateGraphicsPipeline(const dy::RHI::GraphicsPipelineDesc& desc) {
	try {
		if(!EnsureProfileResources(desc.resourceProfile)) return nullptr;
		const ProfileResources* profileResources = GetProfileResources(desc.resourceProfile);
		if(profileResources == nullptr || profileResources->descriptorSetLayout == VK_NULL_HANDLE) return nullptr;
		const VkFormat colorFormat = desc.renderTargetFormat == dy::RHI::Format::Unknown
			? VK_FORMAT_UNDEFINED
			: ToVkFormat(desc.renderTargetFormat);
		const VkFormat depthFormat = desc.depthStencilFormat == dy::RHI::Format::Unknown
			? VK_FORMAT_UNDEFINED
			: ToVkFormat(desc.depthStencilFormat);

		VulkanPipelineState* pipelineState = new VulkanPipelineState(
			m_context,
			colorFormat,
			depthFormat,
			profileResources->descriptorSetLayout,
			desc.enableBindlessTextures ? m_bindlessDescriptorSetLayout : VK_NULL_HANDLE,
			desc);
		m_ownedPipelineStates.push_back(pipelineState);
		return pipelineState;
	} catch (const std::exception& e) {
		SDL_Log("Vulkan pipeline creation failed: %s", e.what());
		return nullptr;
	}
}

dy::RHI::IPipelineState* VulkanDevice::Impl::CreateComputePipeline(const dy::RHI::ComputePipelineDesc& desc)
{
	try
	{
		if(!m_capabilities.supportsComputeSkinning
			|| desc.computeShader == nullptr
			|| desc.computeShaderSize == 0u
			|| desc.storageBufferCount == 0u
			|| desc.storageBufferCount > m_capabilities.limits.maxPerStageDescriptorStorageBuffers
			|| desc.storageBufferCount > m_capabilities.limits.maxDescriptorSetStorageBuffers
			|| desc.inlineConstantSize > m_capabilities.limits.maxPushConstantsSize
			|| (desc.inlineConstantSize & 3u) != 0u)
		{
			SDL_Log("Vulkan compute pipeline description is unsupported by the selected device.");
			return nullptr;
		}
		VulkanComputePipelineState* pipelineState = new VulkanComputePipelineState(
			m_context,
			desc,
			m_maxFramesInFlight,
			m_descriptorCapacityPerFrame);
		m_ownedPipelineStates.push_back(pipelineState);
		return pipelineState;
	}
	catch(const std::exception& error)
	{
		SDL_Log("Vulkan compute pipeline creation failed: %s", error.what());
		return nullptr;
	}
}

dy::RHI::DescriptorIndex VulkanDevice::Impl::AllocateDescriptorSlot() {
	if (m_nextDescriptorIndex >= m_maxBindlessTextures) return dy::RHI::INVALID_DESCRIPTOR_INDEX;
	return m_nextDescriptorIndex++;
}

void VulkanDevice::Impl::UpdateDescriptorSlot(dy::RHI::DescriptorIndex index, dy::RHI::ITexture* texture) {
	if (index == dy::RHI::INVALID_DESCRIPTOR_INDEX || index >= m_maxBindlessTextures) return;
	if (m_bindlessDescriptorSets.empty() || index >= m_bindlessTextures.size()) return;

	const VulkanTexture* vulkanTexture = dynamic_cast<const VulkanTexture*>(texture);
	if (vulkanTexture == nullptr || vulkanTexture->GetImageView() == VK_NULL_HANDLE || vulkanTexture->GetSampler() == VK_NULL_HANDLE) {
		vulkanTexture = static_cast<const VulkanTexture*>(m_fallbackTexture);
	}
	if (vulkanTexture == nullptr || vulkanTexture->GetImageView() == VK_NULL_HANDLE || vulkanTexture->GetSampler() == VK_NULL_HANDLE) return;

	m_bindlessTextures[index] = vulkanTexture;
	++m_bindlessDescriptorRevision;
	if(m_bindlessDescriptorRevision == 0u) m_bindlessDescriptorRevision = 1u;
	if(m_frameReady) (void)ApplyBindlessDescriptorSet(m_currentFrameIndex);
}

void VulkanDevice::Impl::DestroyTexture(dy::RHI::ITexture* texture) {
	if (!texture || texture == m_backBuffer) return;
	const auto it = std::find(m_ownedTextures.begin(), m_ownedTextures.end(), texture);
	if (it != m_ownedTextures.end()) {
		vkDeviceWaitIdle(m_context.device);
		const VulkanTexture* vulkanTexture = dynamic_cast<const VulkanTexture*>(texture);
		const VulkanTexture* fallbackTexture = static_cast<const VulkanTexture*>(m_fallbackTexture);
		bool descriptorChanged = false;
		for(const VulkanTexture*& boundTexture : m_bindlessTextures)
		{
			if(boundTexture == vulkanTexture)
			{
				boundTexture = fallbackTexture;
				descriptorChanged = true;
			}
		}
		if(descriptorChanged)
		{
			++m_bindlessDescriptorRevision;
			for(uint32_t frameIndex = 0u; frameIndex < m_bindlessDescriptorSets.size(); ++frameIndex)
				(void)ApplyBindlessDescriptorSet(frameIndex);
		}
		delete *it;
		m_ownedTextures.erase(it);
	}
}

void VulkanDevice::Impl::DestroyPipelineState(dy::RHI::IPipelineState* pipeline) {
	if (!pipeline) return;
	const auto it = std::find(m_ownedPipelineStates.begin(), m_ownedPipelineStates.end(), pipeline);
	if (it != m_ownedPipelineStates.end()) {
		delete *it;
		m_ownedPipelineStates.erase(it);
	}
}

dy::RHI::ITexture* VulkanDevice::Impl::GetBackBuffer() {
	return m_backBuffer;
}

dy::RHI::IBuffer* VulkanDevice::Impl::CreateBuffer(const dy::RHI::BufferDesc& desc) {
	try
	{
		VulkanBuffer* buffer = new VulkanBuffer(m_context, m_commandPool, desc);
		m_ownedBuffers.push_back(buffer);
		return buffer;
	}
	catch(const std::exception& e)
	{
		SDL_Log("Vulkan buffer creation failed: %s", e.what());
		return nullptr;
	}
}

void VulkanDevice::Impl::DestroyBuffer(dy::RHI::IBuffer* buffer) {
	if (!buffer) return;
	const auto it = std::find(m_ownedBuffers.begin(), m_ownedBuffers.end(), buffer);
	if (it == m_ownedBuffers.end()) return;
	RetiredBuffer retired;
	retired.buffer = *it;
	for(uint32_t frameIndex = 0u; frameIndex < m_inFlightFences.size(); ++frameIndex)
	{
		const VkFence fence = m_inFlightFences[frameIndex];
		if(frameIndex == m_currentFrameIndex
			|| (fence != VK_NULL_HANDLE && vkGetFenceStatus(m_context.device, fence) == VK_NOT_READY))
			retired.pendingFrameIndices.push_back(frameIndex);
	}
	m_ownedBuffers.erase(it);
	if(retired.pendingFrameIndices.empty()) delete retired.buffer;
	else m_retiredBuffers.push_back(std::move(retired));
}

void VulkanDevice::Impl::CollectRetiredBuffers()
{
	for(RetiredBuffer& retired : m_retiredBuffers)
	{
		retired.pendingFrameIndices.erase(
			std::remove_if(
				retired.pendingFrameIndices.begin(),
				retired.pendingFrameIndices.end(),
				[this](uint32_t frameIndex)
				{
					return frameIndex < m_inFlightFences.size()
						&& vkGetFenceStatus(m_context.device, m_inFlightFences[frameIndex]) == VK_SUCCESS;
				}),
			retired.pendingFrameIndices.end());
	}
	m_retiredBuffers.erase(
		std::remove_if(
			m_retiredBuffers.begin(),
			m_retiredBuffers.end(),
			[](RetiredBuffer& retired)
			{
				if(!retired.pendingFrameIndices.empty()) return false;
				delete retired.buffer;
				retired.buffer = nullptr;
				return true;
			}),
		m_retiredBuffers.end());
}

void VulkanDevice::Impl::DestroyAllRetiredBuffers()
{
	for(RetiredBuffer& retired : m_retiredBuffers) delete retired.buffer;
	m_retiredBuffers.clear();
}

void VulkanDevice::Impl::BeginFrame() {
	m_frameReady = false;
	m_frameSubmitted = false;
	m_imageAcquired = false;
	if (m_commandList != nullptr) {
		m_commandList->Begin(m_maxColorAttachments, m_maxDrawsPerFrame);
	}

	if (m_deviceLost || !m_context.device || m_swapchain.GetHandle() == VK_NULL_HANDLE) return;

	const VkResult waitResult = vkWaitForFences(
		m_context.device,
		1,
		&m_inFlightFences[m_currentFrameIndex],
		VK_TRUE,
		UINT64_MAX);
	if(waitResult != VK_SUCCESS)
	{
		if(waitResult == VK_ERROR_DEVICE_LOST) m_deviceLost = true;
		SDL_Log("Failed to wait for Vulkan frame fence: %s (%d)", VkResultToString(waitResult), static_cast<int>(waitResult));
		return;
	}
	CollectRetiredBuffers();
	if(!ApplyBindlessDescriptorSet(m_currentFrameIndex))
	{
		SDL_Log("Failed to update frame-local Vulkan bindless descriptor set.");
		return;
	}
	m_frameReady = true;
}

bool VulkanDevice::Impl::AcquireFrameImage()
{
	CollectGpuTimestampResults(m_currentFrameIndex);

	const VkResult acquireResult = vkAcquireNextImageKHR(
		m_context.device,
		m_swapchain.GetHandle(),
		m_frameAcquireTimeoutNanoseconds,
		m_imageAvailableSemaphores[m_currentFrameIndex],
		VK_NULL_HANDLE,
		&m_currentImageIndex);
	if(acquireResult == VK_ERROR_OUT_OF_DATE_KHR)
	{
		(void)RecreateSwapchain();
		return false;
	}
	if(acquireResult == VK_NOT_READY || acquireResult == VK_TIMEOUT) return false;
	if(acquireResult != VK_SUCCESS && acquireResult != VK_SUBOPTIMAL_KHR)
	{
		if(acquireResult == VK_ERROR_DEVICE_LOST) m_deviceLost = true;
		SDL_Log("Failed to acquire Vulkan swapchain image: %s (%d)", VkResultToString(acquireResult), static_cast<int>(acquireResult));
		return false;
	}
	m_imageAcquired = true;
	if(m_currentImageIndex >= m_imagesInFlight.size())
	{
		SDL_Log("Vulkan swapchain image index %u is out of tracked range %zu.", m_currentImageIndex, m_imagesInFlight.size());
		RecoverAbortedAcquiredFrame(false);
		return false;
	}
	const VkFence imageFence = m_imagesInFlight[m_currentImageIndex];
	if(imageFence != VK_NULL_HANDLE)
	{
		const VkResult waitResult = vkWaitForFences(m_context.device, 1, &imageFence, VK_TRUE, UINT64_MAX);
		if(waitResult != VK_SUCCESS)
		{
			if(waitResult == VK_ERROR_DEVICE_LOST) m_deviceLost = true;
			RecoverAbortedAcquiredFrame(false);
			return false;
		}
	}
	return true;
}

bool VulkanDevice::Impl::RecreateCurrentFrameFenceSignaled()
{
	VkFence& frameFence = m_inFlightFences[m_currentFrameIndex];
	const VkFence oldFence = frameFence;
	for(VkFence& imageFence : m_imagesInFlight)
		if(imageFence == oldFence) imageFence = VK_NULL_HANDLE;
	if(frameFence != VK_NULL_HANDLE) vkDestroyFence(m_context.device, frameFence, nullptr);
	VkFenceCreateInfo fenceInfo{};
	fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
	frameFence = VK_NULL_HANDLE;
	return vkCreateFence(m_context.device, &fenceInfo, nullptr, &frameFence) == VK_SUCCESS;
}

void VulkanDevice::Impl::RecoverAbortedAcquiredFrame(bool recreateFence)
{
	if(m_context.device == VK_NULL_HANDLE) return;
	const VkResult idleResult = vkDeviceWaitIdle(m_context.device);
	if(idleResult != VK_SUCCESS)
	{
		if(idleResult == VK_ERROR_DEVICE_LOST) m_deviceLost = true;
		SDL_Log("Failed to idle Vulkan device while recovering an aborted frame: %s (%d).",
			VkResultToString(idleResult),
			static_cast<int>(idleResult));
		m_frameReady = false;
		m_frameSubmitted = false;
		m_imageAcquired = false;
		return;
	}
	if(recreateFence && !RecreateCurrentFrameFenceSignaled()) m_deviceLost = true;
	if(m_currentFrameIndex < m_imageAvailableSemaphores.size())
	{
		VkSemaphore& semaphore = m_imageAvailableSemaphores[m_currentFrameIndex];
		if(semaphore != VK_NULL_HANDLE) vkDestroySemaphore(m_context.device, semaphore, nullptr);
		VkSemaphoreCreateInfo semaphoreInfo{};
		semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
		semaphore = VK_NULL_HANDLE;
		if(vkCreateSemaphore(m_context.device, &semaphoreInfo, nullptr, &semaphore) != VK_SUCCESS) m_deviceLost = true;
	}
	if(m_imageAcquired && !m_deviceLost) (void)RecreateSwapchain();
	m_frameReady = false;
	m_frameSubmitted = false;
	m_imageAcquired = false;
}

void VulkanDevice::Impl::Submit(dy::RHI::ICommandList** cmdLists, uint32_t count) {
	if(!m_frameReady || count == 0u || cmdLists == nullptr || cmdLists[0] == nullptr)
	{
		m_frameReady = false;
		return;
	}

	VulkanCommandList* vulkanCmd = static_cast<VulkanCommandList*>(cmdLists[0]);
	if (vulkanCmd->m_drawCalls.size() > m_maxDrawsPerFrame) {
		SDL_Log("Vulkan draw count exceeds maxDrawsPerFrame.");
		m_frameReady = false;
		return;
	}
	if(!UploadDrawConstants(*vulkanCmd))
	{
		if(!m_drawCapacityErrorReported) SDL_Log("Failed to upload Vulkan draw constants.");
		m_frameReady = false;
		return;
	}
	if (!UpdateComputeDescriptorSets(*vulkanCmd)) {
		SDL_Log("Failed to update Vulkan compute descriptor sets.");
		m_frameReady = false;
		return;
	}
	if(!AcquireFrameImage())
	{
		m_frameReady = false;
		return;
	}
	if(!RecordCommandBuffer(*vulkanCmd))
	{
		SDL_Log("Failed to record Vulkan command buffer; acquired frame will be recovered.");
		const VkResult discardResult = vkResetCommandBuffer(
			m_commandBuffers[m_currentFrameIndex],
			0u);
		if(discardResult == VK_ERROR_DEVICE_LOST)
		{
			m_deviceLost = true;
			m_frameReady = false;
			m_imageAcquired = false;
			return;
		}
		RecoverAbortedAcquiredFrame(false);
		return;
	}
	const VkResult resetFenceResult = vkResetFences(m_context.device, 1, &m_inFlightFences[m_currentFrameIndex]);
	if(resetFenceResult != VK_SUCCESS)
	{
		RollbackRecordedImageLayouts();
		SDL_Log("Failed to reset Vulkan frame fence before queue submission: %s (%d).",
			VkResultToString(resetFenceResult), static_cast<int>(resetFenceResult));
		if(resetFenceResult == VK_ERROR_DEVICE_LOST)
		{
			m_deviceLost = true;
			m_frameReady = false;
			m_imageAcquired = false;
		}
		else RecoverAbortedAcquiredFrame(true);
		return;
	}

	VkSemaphoreSubmitInfo waitSemaphoreInfo{};
	waitSemaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
	waitSemaphoreInfo.semaphore = m_imageAvailableSemaphores[m_currentFrameIndex];
	waitSemaphoreInfo.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;

	VkCommandBufferSubmitInfo commandBufferInfo{};
	commandBufferInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
	commandBufferInfo.commandBuffer = m_commandBuffers[m_currentFrameIndex];

	VkSemaphoreSubmitInfo signalSemaphoreInfo{};
	signalSemaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
	signalSemaphoreInfo.semaphore = m_renderFinishedSemaphores[m_currentImageIndex];
	signalSemaphoreInfo.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;

	VkSubmitInfo2 submitInfo{};
	submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
	submitInfo.waitSemaphoreInfoCount = 1;
	submitInfo.pWaitSemaphoreInfos = &waitSemaphoreInfo;
	submitInfo.commandBufferInfoCount = 1;
	submitInfo.pCommandBufferInfos = &commandBufferInfo;
	submitInfo.signalSemaphoreInfoCount = 1;
	submitInfo.pSignalSemaphoreInfos = &signalSemaphoreInfo;

	const VkResult submitResult = vkQueueSubmit2(
		m_context.graphicsQueue,
		1,
		&submitInfo,
		m_inFlightFences[m_currentFrameIndex]);
	if (submitResult != VK_SUCCESS) {
		RollbackRecordedImageLayouts();
		SDL_Log("Failed to submit Vulkan draw command buffer: %s (%d)", VkResultToString(submitResult), static_cast<int>(submitResult));
		if(submitResult == VK_ERROR_DEVICE_LOST)
		{
			m_deviceLost = true;
			m_frameReady = false;
			m_imageAcquired = false;
		}
		else RecoverAbortedAcquiredFrame(true);
		return;
	}

	CommitRecordedImageLayouts();
	m_imagesInFlight[m_currentImageIndex] = m_inFlightFences[m_currentFrameIndex];
	m_frameSubmitted = true;
	m_frameReady = false;
}

void VulkanDevice::Impl::Present() {
	if (!m_frameSubmitted) return;

	VkSwapchainKHR swapchainHandle = m_swapchain.GetHandle();
	VkPresentInfoKHR presentInfo{};
	presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
	presentInfo.waitSemaphoreCount = 1;
	presentInfo.pWaitSemaphores = &m_renderFinishedSemaphores[m_currentImageIndex];
	presentInfo.swapchainCount = 1;
	presentInfo.pSwapchains = &swapchainHandle;
	presentInfo.pImageIndices = &m_currentImageIndex;

	const VkResult presentResult = vkQueuePresentKHR(m_context.presentQueue, &presentInfo);
	if(presentResult == VK_ERROR_DEVICE_LOST)
	{
		SDL_Log("Failed to present swapchain image: %s (%d)", VkResultToString(presentResult), static_cast<int>(presentResult));
		m_deviceLost = true;
		m_frameReady = false;
		m_frameSubmitted = false;
		m_imageAcquired = false;
		return;
	}
	if (presentResult == VK_ERROR_OUT_OF_DATE_KHR || presentResult == VK_SUBOPTIMAL_KHR) {
		if(!RecreateSwapchain())
		{
			m_frameReady = false;
			m_frameSubmitted = false;
			m_imageAcquired = false;
			return;
		}
	} else if (presentResult != VK_SUCCESS) {
		SDL_Log("Failed to present swapchain image: %s (%d)", VkResultToString(presentResult), static_cast<int>(presentResult));
	}

	m_currentFrameIndex = (m_currentFrameIndex + 1) % m_maxFramesInFlight;
	m_frameSubmitted = false;
	m_imageAcquired = false;
}

bool VulkanDevice::Impl::CreateInstance() {
	uint32_t supportedApiVersion = VK_API_VERSION_1_0;
	const auto enumerateInstanceVersion = reinterpret_cast<PFN_vkEnumerateInstanceVersion>(
		vkGetInstanceProcAddr(VK_NULL_HANDLE, "vkEnumerateInstanceVersion"));
	const VkResult versionResult = enumerateInstanceVersion != nullptr
		? enumerateInstanceVersion(&supportedApiVersion)
		: VK_ERROR_INCOMPATIBLE_DRIVER;
	if (versionResult != VK_SUCCESS || !SupportsRequiredVulkanApiVersion(supportedApiVersion)) {
		SDL_Log(
			"Vulkan 1.4 is required; loader reports %u.%u.%u.",
			VK_API_VERSION_MAJOR(supportedApiVersion),
			VK_API_VERSION_MINOR(supportedApiVersion),
			VK_API_VERSION_PATCH(supportedApiVersion));
		return false;
	}

	uint32_t extensionCount = 0;
	const char** extensions = glfwGetRequiredInstanceExtensions(&extensionCount);
	std::vector<const char*> enabledExtensions;
	if (extensions != nullptr) {
		enabledExtensions.assign(extensions, extensions + extensionCount);
	}

	std::vector<const char*> enabledLayers;
	m_validationEnabled = IsValidationEnabled();
	if (m_validationEnabled) enabledLayers.push_back(kValidationLayerName);
	uint32_t availableExtensionCount = 0u;
	std::vector<VkExtensionProperties> availableExtensions;
	if(vkEnumerateInstanceExtensionProperties(nullptr, &availableExtensionCount, nullptr) == VK_SUCCESS)
	{
		availableExtensions.resize(availableExtensionCount);
		if(availableExtensionCount > 0u)
			vkEnumerateInstanceExtensionProperties(nullptr, &availableExtensionCount, availableExtensions.data());
	}
	m_debugUtilsEnabled = std::any_of(
		availableExtensions.begin(),
		availableExtensions.end(),
		[](const VkExtensionProperties& extension)
		{
			return std::strcmp(extension.extensionName, VK_EXT_DEBUG_UTILS_EXTENSION_NAME) == 0;
		});
	if(m_debugUtilsEnabled) enabledExtensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
	else if(m_validationEnabled) SDL_Log("VK_EXT_debug_utils is unavailable; validation messages cannot be counted.");

	VkApplicationInfo appInfo{};
	appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
	appInfo.pApplicationName = "dy_engine Vulkan";
	appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
	appInfo.pEngineName = "dy_engine";
	appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
	appInfo.apiVersion = kRequiredVulkanApiVersion;

	VkInstanceCreateInfo createInfo{};
	createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
	createInfo.pApplicationInfo = &appInfo;
	createInfo.enabledLayerCount = static_cast<uint32_t>(enabledLayers.size());
	createInfo.ppEnabledLayerNames = enabledLayers.empty() ? nullptr : enabledLayers.data();
	createInfo.enabledExtensionCount = static_cast<uint32_t>(enabledExtensions.size());
	createInfo.ppEnabledExtensionNames = enabledExtensions.data();
	VkDebugUtilsMessengerCreateInfoEXT debugCreateInfo{};
	if(m_debugUtilsEnabled && m_validationEnabled)
	{
		debugCreateInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
		debugCreateInfo.messageSeverity =
			VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT
			| VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT
			| VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT
			| VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
		debugCreateInfo.messageType =
			VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT
			| VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT
			| VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
		debugCreateInfo.pfnUserCallback = &VulkanDevice::Impl::DebugUtilsCallback;
		debugCreateInfo.pUserData = this;
		createInfo.pNext = &debugCreateInfo;
	}

	const VkResult result = vkCreateInstance(&createInfo, nullptr, &m_context.instance);
	if (result != VK_SUCCESS) {
		SDL_Log("Failed to create Vulkan instance: %s (%d)", VkResultToString(result), static_cast<int>(result));
		return false;
	}

	if (m_debugUtilsEnabled) {
		m_vkCmdBeginDebugUtilsLabel = reinterpret_cast<PFN_vkCmdBeginDebugUtilsLabelEXT>(
			vkGetInstanceProcAddr(m_context.instance, "vkCmdBeginDebugUtilsLabelEXT"));
		m_vkCmdEndDebugUtilsLabel = reinterpret_cast<PFN_vkCmdEndDebugUtilsLabelEXT>(
			vkGetInstanceProcAddr(m_context.instance, "vkCmdEndDebugUtilsLabelEXT"));
		m_vkCmdInsertDebugUtilsLabel = reinterpret_cast<PFN_vkCmdInsertDebugUtilsLabelEXT>(
			vkGetInstanceProcAddr(m_context.instance, "vkCmdInsertDebugUtilsLabelEXT"));
	}
	if(!m_debugUtilsEnabled || !m_validationEnabled) return true;
	const auto createMessenger = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
		vkGetInstanceProcAddr(m_context.instance, "vkCreateDebugUtilsMessengerEXT"));
	return createMessenger != nullptr
		&& createMessenger(m_context.instance, &debugCreateInfo, nullptr, &m_debugMessenger) == VK_SUCCESS;
}

bool VulkanDevice::Impl::CreateSurface() {
#if defined(_WIN32)
	VkWin32SurfaceCreateInfoKHR createInfo{};
	createInfo.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
	createInfo.hinstance = GetModuleHandle(nullptr);
	createInfo.hwnd = static_cast<HWND>(m_windowHandle);
	return vkCreateWin32SurfaceKHR(m_context.instance, &createInfo, nullptr, &m_context.surface) == VK_SUCCESS;
#else
	return glfwCreateWindowSurface(m_context.instance, static_cast<GLFWwindow*>(m_windowHandle), nullptr, &m_context.surface) == VK_SUCCESS;
#endif
}

bool VulkanDevice::Impl::PickPhysicalDevice() {
	m_optimalRgba8HostImageCopy = false;
	uint32_t deviceCount = 0;
	if(vkEnumeratePhysicalDevices(m_context.instance, &deviceCount, nullptr) != VK_SUCCESS || deviceCount == 0u) return false;
	std::vector<VkPhysicalDevice> devices(deviceCount);
	if(vkEnumeratePhysicalDevices(m_context.instance, &deviceCount, devices.data()) != VK_SUCCESS) return false;

	int32_t bestScore = std::numeric_limits<int32_t>::min();
	VkPhysicalDevice bestDevice = VK_NULL_HANDLE;
	VulkanContext::QueueFamilyIndices bestIndices;
	VulkanCapabilities bestCapabilities = {};
	bool bestOptimalRgba8HostImageCopy = false;
	uint32_t bestMaxColorAttachments = 0u;
	for (VkPhysicalDevice device : devices) {
		uint32_t extensionCount = 0u;
		if(vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, nullptr) != VK_SUCCESS) continue;
		std::vector<VkExtensionProperties> extensions(extensionCount);
		if(extensionCount > 0u
			&& vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, extensions.data()) != VK_SUCCESS) continue;
		const bool hasSwapchainExtension = std::any_of(
			extensions.begin(), extensions.end(), [](const VkExtensionProperties& extension) {
				return std::strcmp(extension.extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME) == 0;
			});
		if(!hasSwapchainExtension) continue;

		VkPhysicalDeviceVulkan14Properties vulkan14Properties{};
		vulkan14Properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_PROPERTIES;
		VkPhysicalDeviceProperties2 properties{};
		properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
		properties.pNext = &vulkan14Properties;
		vkGetPhysicalDeviceProperties2(device, &properties);
		if (!SupportsRequiredVulkanApiVersion(properties.properties.apiVersion)) continue;

		VkPhysicalDeviceVulkan14Features vulkan14Features{};
		vulkan14Features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES;
		VkPhysicalDeviceFeatures2 supportedFeatures{};
		supportedFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
		supportedFeatures.pNext = &vulkan14Features;
		vkGetPhysicalDeviceFeatures2(device, &supportedFeatures);

		bool optimalRgba8HostImageCopy = false;
		if (vulkan14Features.hostImageCopy == VK_TRUE) {
			std::vector<VkImageLayout> copySrcLayouts(vulkan14Properties.copySrcLayoutCount);
			std::vector<VkImageLayout> copyDstLayouts(vulkan14Properties.copyDstLayoutCount);
			vulkan14Properties.pCopySrcLayouts = copySrcLayouts.data();
			vulkan14Properties.pCopyDstLayouts = copyDstLayouts.data();
			vkGetPhysicalDeviceProperties2(device, &properties);

			VkFormatProperties3 formatProperties3{};
			formatProperties3.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3;
			VkFormatProperties2 formatProperties{};
			formatProperties.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2;
			formatProperties.pNext = &formatProperties3;
			vkGetPhysicalDeviceFormatProperties2(device, VK_FORMAT_R8G8B8A8_UNORM, &formatProperties);
			const bool formatSupportsHostTransfer =
				(formatProperties3.optimalTilingFeatures & VK_FORMAT_FEATURE_2_HOST_IMAGE_TRANSFER_BIT) != 0;
			const bool shaderReadLayoutSupported = std::find(
				copyDstLayouts.begin(),
				copyDstLayouts.end(),
				VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) != copyDstLayouts.end();

			VkPhysicalDeviceImageFormatInfo2 imageFormatInfo{};
			imageFormatInfo.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2;
			imageFormatInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
			imageFormatInfo.type = VK_IMAGE_TYPE_2D;
			imageFormatInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
			imageFormatInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_HOST_TRANSFER_BIT;
			VkHostImageCopyDevicePerformanceQuery performance{};
			performance.sType = VK_STRUCTURE_TYPE_HOST_IMAGE_COPY_DEVICE_PERFORMANCE_QUERY;
			VkImageFormatProperties2 imageProperties{};
			imageProperties.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2;
			imageProperties.pNext = &performance;
			optimalRgba8HostImageCopy = formatSupportsHostTransfer &&
				shaderReadLayoutSupported &&
				vkGetPhysicalDeviceImageFormatProperties2(device, &imageFormatInfo, &imageProperties) == VK_SUCCESS &&
				performance.optimalDeviceAccess == VK_TRUE &&
				performance.identicalMemoryLayout == VK_TRUE;
		}

		uint32_t count = 0; vkGetPhysicalDeviceQueueFamilyProperties(device, &count, nullptr);
		if(count == 0u) continue;
		std::vector<VkQueueFamilyProperties> families(count); vkGetPhysicalDeviceQueueFamilyProperties(device, &count, families.data());
		
		VulkanContext::QueueFamilyIndices indices;
		uint32_t fallbackGraphicsFamily = UINT32_MAX;
		uint32_t computeGraphicsFamily = UINT32_MAX;
		for (uint32_t i = 0; i < count; ++i) {
			if((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0u)
			{
				if(fallbackGraphicsFamily == UINT32_MAX) fallbackGraphicsFamily = i;
				if((families[i].queueFlags & VK_QUEUE_COMPUTE_BIT) != 0u && computeGraphicsFamily == UINT32_MAX)
					computeGraphicsFamily = i;
			}
			VkBool32 presentSupport = false;
			if(vkGetPhysicalDeviceSurfaceSupportKHR(device, i, m_context.surface, &presentSupport) != VK_SUCCESS) presentSupport = VK_FALSE;
			if (presentSupport && indices.presentFamily == UINT32_MAX) indices.presentFamily = i;
		}
		indices.graphicsFamily = computeGraphicsFamily != UINT32_MAX
			? computeGraphicsFamily
			: fallbackGraphicsFamily;

		if(!indices.IsComplete()) continue;
		auto swapchainSupport = VulkanSwapchain::QuerySwapchainSupport(device, m_context.surface);
		if(swapchainSupport.result != VK_SUCCESS || swapchainSupport.formats.empty() || swapchainSupport.presentModes.empty()) continue;

		VulkanDeviceLimits limits;
		limits.maxPushConstantsSize = properties.properties.limits.maxPushConstantsSize;
		limits.maxPerStageDescriptorStorageBuffers = properties.properties.limits.maxPerStageDescriptorStorageBuffers;
		limits.maxDescriptorSetStorageBuffers = properties.properties.limits.maxDescriptorSetStorageBuffers;
		limits.maxStorageBufferRange = properties.properties.limits.maxStorageBufferRange;
		limits.maxUniformBufferRange = properties.properties.limits.maxUniformBufferRange;
		limits.minUniformBufferOffsetAlignment = std::max<VkDeviceSize>(properties.properties.limits.minUniformBufferOffsetAlignment, 1u);
		limits.minStorageBufferOffsetAlignment = std::max<VkDeviceSize>(properties.properties.limits.minStorageBufferOffsetAlignment, 1u);
		VulkanCapabilities capabilities{};
		capabilities.limits = limits;
		capabilities.supportsComputeSkinning =
			(families[indices.graphicsFamily].queueFlags & VK_QUEUE_COMPUTE_BIT) != 0u;

		int32_t score = static_cast<int32_t>(properties.properties.limits.maxImageDimension2D);
		switch(properties.properties.deviceType)
		{
		case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: score += 300000; break;
		case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: score += 200000; break;
		case VK_PHYSICAL_DEVICE_TYPE_CPU: score += 100000; break;
		default: break;
		}
		if(score > bestScore)
		{
			bestScore = score;
			bestDevice = device;
			bestIndices = indices;
			bestCapabilities = capabilities;
			bestOptimalRgba8HostImageCopy = optimalRgba8HostImageCopy;
			bestMaxColorAttachments = properties.properties.limits.maxColorAttachments;
			m_timestampValidBits = families[indices.graphicsFamily].timestampValidBits;
			m_timestampPeriodNanoseconds = properties.properties.limits.timestampPeriod;
		}
	}
	if(bestDevice == VK_NULL_HANDLE)
	{
		SDL_Log("No compatible Vulkan 1.4 physical device found.");
		return false;
	}
	m_context.physicalDevice = bestDevice;
	m_context.queueIndices = bestIndices;
	m_capabilities = bestCapabilities;
	m_optimalRgba8HostImageCopy = bestOptimalRgba8HostImageCopy;
	m_maxColorAttachments = bestMaxColorAttachments;
	SDL_Log(
		"Vulkan RGBA8 Host Image Copy: %s.",
		m_optimalRgba8HostImageCopy ? "enabled" : "staging fallback");
	return true;
}

void VulkanDevice::Impl::DestroyDebugMessenger()
{
	if(m_context.instance == VK_NULL_HANDLE || m_debugMessenger == VK_NULL_HANDLE) return;
	const auto destroyMessenger = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
		vkGetInstanceProcAddr(m_context.instance, "vkDestroyDebugUtilsMessengerEXT"));
	if(destroyMessenger != nullptr) destroyMessenger(m_context.instance, m_debugMessenger, nullptr);
	m_debugMessenger = VK_NULL_HANDLE;
}

VKAPI_ATTR VkBool32 VKAPI_CALL VulkanDevice::Impl::DebugUtilsCallback(
	VkDebugUtilsMessageSeverityFlagBitsEXT severity,
	VkDebugUtilsMessageTypeFlagsEXT,
	const VkDebugUtilsMessengerCallbackDataEXT* callbackData,
	void* userData)
{
	VulkanDevice::Impl* impl = static_cast<VulkanDevice::Impl*>(userData);
	const char* message = callbackData != nullptr && callbackData->pMessage != nullptr
		? callbackData->pMessage
		: "Vulkan validation message without text";
	if(impl != nullptr)
	{
		if((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0u)
			impl->m_validationErrorCount.fetch_add(1u);
		const char* messageId = callbackData != nullptr ? callbackData->pMessageIdName : nullptr;
		if((messageId != nullptr && std::strstr(messageId, "VUID-") != nullptr)
			|| std::strstr(message, "VUID-") != nullptr)
			impl->m_validationVuidCount.fetch_add(1u);
	}
	if((severity & (VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)) != 0u)
		SDL_Log("Vulkan validation: %s", message);
	return VK_FALSE;
}

bool VulkanDevice::Impl::CreateLogicalDevice() {
	std::vector<VkDeviceQueueCreateInfo> queueCreateInfos;
	std::vector<uint32_t> uniqueFamilies = { m_context.queueIndices.graphicsFamily };
	if (m_context.queueIndices.presentFamily != m_context.queueIndices.graphicsFamily) uniqueFamilies.push_back(m_context.queueIndices.presentFamily);

	const float queuePriority = 1.0f;
	for (uint32_t family : uniqueFamilies) {
		VkDeviceQueueCreateInfo queueCreateInfo{};
		queueCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
		queueCreateInfo.queueFamilyIndex = family;
		queueCreateInfo.queueCount = 1;
		queueCreateInfo.pQueuePriorities = &queuePriority;
		queueCreateInfos.push_back(queueCreateInfo);
	}

	const char* deviceExtensions[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
	VkPhysicalDeviceFeatures2 enabledFeatures{};
	enabledFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
	enabledFeatures.features.shaderSampledImageArrayDynamicIndexing = VK_TRUE;
	VkPhysicalDeviceVulkan13Features vulkan13Features{};
	vulkan13Features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
	vulkan13Features.dynamicRendering = VK_TRUE;
	vulkan13Features.synchronization2 = VK_TRUE;
	VkPhysicalDeviceVulkan14Features vulkan14Features{};
	vulkan14Features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES;
	vulkan14Features.pushDescriptor = VK_TRUE;
	vulkan14Features.maintenance5 = VK_TRUE;
	vulkan14Features.hostImageCopy = m_optimalRgba8HostImageCopy ? VK_TRUE : VK_FALSE;
	enabledFeatures.pNext = &vulkan13Features;
	vulkan13Features.pNext = &vulkan14Features;
	VkDeviceCreateInfo createInfo{};
	createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
	createInfo.queueCreateInfoCount = static_cast<uint32_t>(queueCreateInfos.size());
	createInfo.pQueueCreateInfos = queueCreateInfos.data();
	createInfo.enabledExtensionCount = 1;
	createInfo.ppEnabledExtensionNames = deviceExtensions;
	createInfo.pNext = &enabledFeatures;
	createInfo.pEnabledFeatures = nullptr;

	if (vkCreateDevice(m_context.physicalDevice, &createInfo, nullptr, &m_context.device) != VK_SUCCESS) return false;

	vkGetDeviceQueue(m_context.device, m_context.queueIndices.graphicsFamily, 0, &m_context.graphicsQueue);
	vkGetDeviceQueue(m_context.device, m_context.queueIndices.presentFamily, 0, &m_context.presentQueue);
	return true;
}

void VulkanDevice::Impl::BeginDebugLabel(VkCommandBuffer commandBuffer, const char* name, const dy::RHI::DebugLabelColor& color) const {
	if (m_vkCmdBeginDebugUtilsLabel == nullptr || name == nullptr || name[0] == '\0') return;
	VkDebugUtilsLabelEXT label{};
	label.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT;
	label.pLabelName = name;
	label.color[0] = color.r;
	label.color[1] = color.g;
	label.color[2] = color.b;
	label.color[3] = color.a;
	m_vkCmdBeginDebugUtilsLabel(commandBuffer, &label);
}

void VulkanDevice::Impl::EndDebugLabel(VkCommandBuffer commandBuffer) const {
	if (m_vkCmdEndDebugUtilsLabel != nullptr) m_vkCmdEndDebugUtilsLabel(commandBuffer);
}

void VulkanDevice::Impl::InsertDebugLabel(VkCommandBuffer commandBuffer, const char* name, const dy::RHI::DebugLabelColor& color) const {
	if (m_vkCmdInsertDebugUtilsLabel == nullptr || name == nullptr || name[0] == '\0') return;
	VkDebugUtilsLabelEXT label{};
	label.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT;
	label.pLabelName = name;
	label.color[0] = color.r;
	label.color[1] = color.g;
	label.color[2] = color.b;
	label.color[3] = color.a;
	m_vkCmdInsertDebugUtilsLabel(commandBuffer, &label);
}

void VulkanDevice::Impl::RecordDebugEventsAt(
	VkCommandBuffer commandBuffer,
	const VulkanCommandList& commandList,
	uint32_t eventIndex) {
	if (eventIndex < commandList.m_debugEvents.size()) {
		const VulkanCommandList::DebugEvent& event = commandList.m_debugEvents[eventIndex];

		switch (event.type) {
		case VulkanCommandList::DebugEventType::Begin:
			BeginDebugLabel(commandBuffer, event.name.c_str(), event.color);
			break;
		case VulkanCommandList::DebugEventType::End:
			EndDebugLabel(commandBuffer);
			break;
		case VulkanCommandList::DebugEventType::Marker:
			InsertDebugLabel(commandBuffer, event.name.c_str(), event.color);
			break;
		}
	}
}

void VulkanDevice::Impl::RecordGpuTimestampEventsAt(
	VkCommandBuffer commandBuffer,
	const VulkanCommandList& commandList,
	uint32_t eventIndex) {
	if (eventIndex < commandList.m_gpuTimestampEvents.size()) {
		const VulkanCommandList::GpuTimestampEvent& event = commandList.m_gpuTimestampEvents[eventIndex];

		if (event.type == VulkanCommandList::GpuTimestampEventType::Begin) {
			OpenGpuTimestamp open{};
			open.name = event.name;
			if (m_recordingGpuTimestampQuery + m_recordingGpuTimestampStack.size() + 2u <= kMaxGpuTimestampQueriesPerFrame) {
				open.beginQuery = m_recordingGpuTimestampQuery++;
				vkCmdWriteTimestamp2(
					commandBuffer,
					VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
					m_gpuTimestampQueryPools[m_currentFrameIndex],
					open.beginQuery);
			}
			m_recordingGpuTimestampStack.push_back(std::move(open));
		} else {
			if (m_recordingGpuTimestampStack.empty()) return;
			OpenGpuTimestamp open = std::move(m_recordingGpuTimestampStack.back());
			m_recordingGpuTimestampStack.pop_back();
			if (open.beginQuery == UINT32_MAX) return;
			const uint32_t endQuery = m_recordingGpuTimestampQuery++;
			vkCmdWriteTimestamp2(
				commandBuffer,
				VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT,
				m_gpuTimestampQueryPools[m_currentFrameIndex],
				endQuery);
			m_gpuTimestampPairs[m_currentFrameIndex].push_back({ std::move(open.name), open.beginQuery, endQuery });
		}
	}
}

bool VulkanDevice::Impl::RecordCommandBuffer(const VulkanCommandList& commandList) {
	m_recordedImageLayouts.clear();
	m_recordedSwapchainLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	const auto fail = [this]()
	{
		RollbackRecordedImageLayouts();
		return false;
	};
	VkCommandBuffer commandBuffer = m_commandBuffers[m_currentFrameIndex];
	if(vkResetCommandBuffer(commandBuffer, 0) != VK_SUCCESS) return fail();
	VkCommandBufferBeginInfo beginInfo{};
	beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	if(vkBeginCommandBuffer(commandBuffer, &beginInfo) != VK_SUCCESS) return fail();

	if (m_gpuTimestampsSupported) {
		vkCmdResetQueryPool(commandBuffer, m_gpuTimestampQueryPools[m_currentFrameIndex], 0, kMaxGpuTimestampQueriesPerFrame);
		m_recordingGpuTimestampQuery = 0;
		m_recordingGpuTimestampStack.clear();
		m_gpuTimestampPairs[m_currentFrameIndex].clear();
		m_gpuTimestampQueryCounts[m_currentFrameIndex] = 0;
	}

	for(size_t workIndex = 0u; workIndex < commandList.m_workItems.size();)
	{
		const VulkanCommandList::WorkItem& work = commandList.m_workItems[workIndex];
		if(work.type == VulkanCommandList::WorkType::DebugEvent)
		{
			RecordDebugEventsAt(commandBuffer, commandList, work.index);
			++workIndex;
			continue;
		}
		if(work.type == VulkanCommandList::WorkType::GpuTimestamp)
		{
			if(m_gpuTimestampsSupported) RecordGpuTimestampEventsAt(commandBuffer, commandList, work.index);
			++workIndex;
			continue;
		}
		if(work.type == VulkanCommandList::WorkType::Dispatch)
		{
			if(!RecordComputeDispatch(commandBuffer, commandList, work.index)) return fail();
			++workIndex;
			continue;
		}
		if(work.type == VulkanCommandList::WorkType::BufferBarrier)
		{
			if(!RecordBufferBarrier(commandBuffer, commandList, work.index)) return fail();
			++workIndex;
			continue;
		}
		if(work.type == VulkanCommandList::WorkType::ClearColor)
		{
			if(!RecordColorClear(commandBuffer, commandList, work.index)) return fail();
			++workIndex;
			continue;
		}
		if(work.type == VulkanCommandList::WorkType::ClearDepth)
		{
			if(!RecordDepthClear(commandBuffer, commandList, work.index)) return fail();
			++workIndex;
			continue;
		}

		if(work.index >= commandList.m_drawCalls.size()) return fail();
		const uint32_t firstDraw = work.index;
		const VulkanCommandList::DrawCall& first = commandList.m_drawCalls[firstDraw];
		uint32_t drawCount = 1u;
		while(workIndex + drawCount < commandList.m_workItems.size())
		{
			const VulkanCommandList::WorkItem& nextWork = commandList.m_workItems[workIndex + drawCount];
			if(nextWork.type != VulkanCommandList::WorkType::Draw || nextWork.index != firstDraw + drawCount) break;
			const VulkanCommandList::DrawCall& next = commandList.m_drawCalls[nextWork.index];
			bool sameTargets = first.renderTargetsValid
				&& next.renderTargetsValid
				&& next.renderTargetCount == first.renderTargetCount
				&& next.depthStencil == first.depthStencil
				&& first.renderTargetOffset <= commandList.m_renderTargets.size()
				&& next.renderTargetOffset <= commandList.m_renderTargets.size()
				&& first.renderTargetCount <= commandList.m_renderTargets.size() - first.renderTargetOffset
				&& next.renderTargetCount <= commandList.m_renderTargets.size() - next.renderTargetOffset;
			for(uint32_t targetIndex = 0u; sameTargets && targetIndex < first.renderTargetCount; ++targetIndex)
			{
				sameTargets = commandList.m_renderTargets[first.renderTargetOffset + targetIndex]
					== commandList.m_renderTargets[next.renderTargetOffset + targetIndex];
			}
			if(!sameTargets) break;
			++drawCount;
		}
		if(!RecordGraphicsPass(commandBuffer, commandList, firstDraw, drawCount)) return fail();
		workIndex += drawCount;
	}
	const bool rendersToSwapchain = std::any_of(
		commandList.m_drawCalls.begin(),
		commandList.m_drawCalls.end(),
		[this, &commandList](const VulkanCommandList::DrawCall& drawCall)
		{
			if(!drawCall.renderTargetsValid) return false;
			if(drawCall.renderTargetCount == 0u) return drawCall.depthStencil == nullptr;
			if(drawCall.renderTargetOffset > commandList.m_renderTargets.size()
				|| drawCall.renderTargetCount > commandList.m_renderTargets.size() - drawCall.renderTargetOffset) return false;
			for(uint32_t targetIndex = 0u; targetIndex < drawCall.renderTargetCount; ++targetIndex)
			{
				dy::RHI::ITexture* renderTarget =
					commandList.m_renderTargets[drawCall.renderTargetOffset + targetIndex];
				if(renderTarget == nullptr || renderTarget == m_backBuffer) return true;
			}
			return false;
		}) || std::any_of(
		commandList.m_colorClears.begin(),
		commandList.m_colorClears.end(),
		[this](const VulkanCommandList::ColorClear& clear) { return clear.texture == m_backBuffer; });
	if(!rendersToSwapchain)
	{
		const auto& images = m_swapchain.GetImages();
		const auto& views = m_swapchain.GetImageViews();
		VulkanTexture* depthTexture = static_cast<VulkanTexture*>(m_depthTexture);
		if(m_currentImageIndex >= images.size() || m_currentImageIndex >= views.size() || depthTexture == nullptr) return fail();
		CmdTransitionImageLayout(commandBuffer, images[m_currentImageIndex], VK_IMAGE_ASPECT_COLOR_BIT,
			m_recordedSwapchainLayout, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
		CmdTransitionImageLayout(commandBuffer, depthTexture->GetImage(), VK_IMAGE_ASPECT_DEPTH_BIT,
			depthTexture->GetImageLayout(), VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
		VkRenderingAttachmentInfo colorAttachment{};
		colorAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
		colorAttachment.imageView = views[m_currentImageIndex];
		colorAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
		colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
		colorAttachment.clearValue.color = { { 0.0f, 0.0f, 0.0f, 1.0f } };
		VkRenderingAttachmentInfo depthAttachment{};
		depthAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
		depthAttachment.imageView = depthTexture->GetImageView();
		depthAttachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
		depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
		depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
		depthAttachment.clearValue.depthStencil = { 1.0f, 0u };
		VkRenderingInfo renderingInfo{};
		renderingInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
		renderingInfo.renderArea.extent = m_swapchain.GetExtent();
		renderingInfo.layerCount = 1u;
		renderingInfo.colorAttachmentCount = 1u;
		renderingInfo.pColorAttachments = &colorAttachment;
		renderingInfo.pDepthAttachment = &depthAttachment;
		vkCmdBeginRendering(commandBuffer, &renderingInfo);
		vkCmdEndRendering(commandBuffer);
		CmdTransitionImageLayout(commandBuffer, images[m_currentImageIndex], VK_IMAGE_ASPECT_COLOR_BIT,
			VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
		m_recordedSwapchainLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
		SetRecordedImageLayout(depthTexture, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
	}

	if(m_gpuTimestampsSupported) m_gpuTimestampQueryCounts[m_currentFrameIndex] = m_recordingGpuTimestampQuery;
	if(vkEndCommandBuffer(commandBuffer) != VK_SUCCESS) return fail();
	return true;
}

bool VulkanDevice::Impl::RecordComputeDispatch(
	VkCommandBuffer commandBuffer,
	const VulkanCommandList& commandList,
	uint32_t dispatchIndex)
{
	if(dispatchIndex >= commandList.m_computeDispatches.size()) return false;
	const VulkanCommandList::ComputeDispatch& dispatch = commandList.m_computeDispatches[dispatchIndex];
	const VulkanComputePipelineState* pipelineState =
		dynamic_cast<const VulkanComputePipelineState*>(dispatch.pipelineState);
	if(pipelineState == nullptr
		|| dispatch.threadGroupCountX == 0u
		|| dispatch.threadGroupCountY == 0u
		|| dispatch.threadGroupCountZ == 0u
		|| dispatch.inlineConstantSize > pipelineState->GetInlineConstantSize()) return false;
	const VkDescriptorSet descriptorSet = pipelineState->GetDescriptorSet(m_currentFrameIndex, dispatchIndex);
	if(descriptorSet == VK_NULL_HANDLE) return false;
	vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineState->GetPipeline());
	vkCmdBindDescriptorSets(
		commandBuffer,
		VK_PIPELINE_BIND_POINT_COMPUTE,
		pipelineState->GetLayout(),
		0u,
		1u,
		&descriptorSet,
		0u,
		nullptr);
	if(dispatch.inlineConstantSize > 0u)
	{
		vkCmdPushConstants(
			commandBuffer,
			pipelineState->GetLayout(),
			VK_SHADER_STAGE_COMPUTE_BIT,
			0u,
			dispatch.inlineConstantSize,
			dispatch.inlineConstants.data());
	}
	vkCmdDispatch(
		commandBuffer,
		dispatch.threadGroupCountX,
		dispatch.threadGroupCountY,
		dispatch.threadGroupCountZ);
	return true;
}

bool VulkanDevice::Impl::RecordBufferBarrier(
	VkCommandBuffer commandBuffer,
	const VulkanCommandList& commandList,
	uint32_t barrierIndex)
{
	if(barrierIndex >= commandList.m_bufferBarriers.size()) return false;
	const VulkanCommandList::BufferBarrier& captured = commandList.m_bufferBarriers[barrierIndex];
	if(captured.sourceAccess != dy::RHI::BufferAccess::ComputeShaderWrite
		|| captured.destinationAccess != dy::RHI::BufferAccess::VertexShaderRead) return false;
	const VulkanBuffer* buffer = dynamic_cast<const VulkanBuffer*>(captured.buffer);
	if(buffer == nullptr || captured.offset > buffer->GetSize()) return false;
	const uint64_t range = captured.size > 0u
		? captured.size
		: static_cast<uint64_t>(buffer->GetSize()) - captured.offset;
	if(range == 0u || range > static_cast<uint64_t>(buffer->GetSize()) - captured.offset) return false;
	VkBufferMemoryBarrier2 barrier{};
	barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
	barrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
	barrier.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
	barrier.dstStageMask = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT;
	barrier.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.buffer = buffer->GetHandle();
	barrier.offset = captured.offset;
	barrier.size = range;
	VkDependencyInfo dependencyInfo{};
	dependencyInfo.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
	dependencyInfo.bufferMemoryBarrierCount = 1u;
	dependencyInfo.pBufferMemoryBarriers = &barrier;
	vkCmdPipelineBarrier2(commandBuffer, &dependencyInfo);
	return true;
}

bool VulkanDevice::Impl::RecordColorClear(
	VkCommandBuffer commandBuffer,
	const VulkanCommandList& commandList,
	uint32_t clearIndex)
{
	if(clearIndex >= commandList.m_colorClears.size()) return false;
	const VulkanCommandList::ColorClear& clear = commandList.m_colorClears[clearIndex];
	const bool isSwapchain = clear.texture == m_backBuffer;
	VulkanTexture* texture = isSwapchain ? nullptr : dynamic_cast<VulkanTexture*>(clear.texture);
	VkImage image = VK_NULL_HANDLE;
	VkImageView view = VK_NULL_HANDLE;
	VkExtent2D extent = {};
	VkImageLayout oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	if(isSwapchain)
	{
		const auto& images = m_swapchain.GetImages();
		const auto& views = m_swapchain.GetImageViews();
		if(m_currentImageIndex >= images.size() || m_currentImageIndex >= views.size()) return false;
		image = images[m_currentImageIndex];
		view = views[m_currentImageIndex];
		extent = m_swapchain.GetExtent();
		oldLayout = m_recordedSwapchainLayout;
	}
	else
	{
		if(texture == nullptr || texture->GetImageView() == VK_NULL_HANDLE
			|| !HasTextureUsage(texture->GetUsage(), dy::RHI::TextureUsage::RenderTarget)) return false;
		image = texture->GetImage();
		view = texture->GetImageView();
		extent = { texture->GetWidth(), texture->GetHeight() };
		oldLayout = texture->GetImageLayout();
	}
	CmdTransitionImageLayout(
		commandBuffer,
		image,
		VK_IMAGE_ASPECT_COLOR_BIT,
		oldLayout,
		VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
	VkRenderingAttachmentInfo attachment{};
	attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
	attachment.imageView = view;
	attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
	attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	attachment.clearValue.color = { { clear.color[0], clear.color[1], clear.color[2], clear.color[3] } };
	VkRenderingInfo renderingInfo{};
	renderingInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
	renderingInfo.renderArea.extent = extent;
	renderingInfo.layerCount = 1u;
	renderingInfo.colorAttachmentCount = 1u;
	renderingInfo.pColorAttachments = &attachment;
	vkCmdBeginRendering(commandBuffer, &renderingInfo);
	vkCmdEndRendering(commandBuffer);
	const VkImageLayout finalLayout = isSwapchain
		? VK_IMAGE_LAYOUT_PRESENT_SRC_KHR
		: HasTextureUsage(texture->GetUsage(), dy::RHI::TextureUsage::ShaderResource)
			? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
			: VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	CmdTransitionImageLayout(
		commandBuffer,
		image,
		VK_IMAGE_ASPECT_COLOR_BIT,
		VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		finalLayout);
	if(isSwapchain) m_recordedSwapchainLayout = finalLayout;
	else SetRecordedImageLayout(texture, finalLayout);
	return true;
}

bool VulkanDevice::Impl::RecordDepthClear(
	VkCommandBuffer commandBuffer,
	const VulkanCommandList& commandList,
	uint32_t clearIndex)
{
	if(clearIndex >= commandList.m_depthClears.size()) return false;
	const VulkanCommandList::DepthClear& clear = commandList.m_depthClears[clearIndex];
	VulkanTexture* texture = dynamic_cast<VulkanTexture*>(clear.texture);
	if(texture == nullptr || texture->GetImageView() == VK_NULL_HANDLE
		|| !HasTextureUsage(texture->GetUsage(), dy::RHI::TextureUsage::DepthStencil)) return false;
	CmdTransitionImageLayout(
		commandBuffer,
		texture->GetImage(),
		VK_IMAGE_ASPECT_DEPTH_BIT,
		texture->GetImageLayout(),
		VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
	VkRenderingAttachmentInfo attachment{};
	attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
	attachment.imageView = texture->GetImageView();
	attachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
	attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
	attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	attachment.clearValue.depthStencil = { clear.depth, 0u };
	VkRenderingInfo renderingInfo{};
	renderingInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
	renderingInfo.renderArea.extent = { texture->GetWidth(), texture->GetHeight() };
	renderingInfo.layerCount = 1u;
	renderingInfo.pDepthAttachment = &attachment;
	vkCmdBeginRendering(commandBuffer, &renderingInfo);
	vkCmdEndRendering(commandBuffer);
	const VkImageLayout finalLayout = HasTextureUsage(texture->GetUsage(), dy::RHI::TextureUsage::ShaderResource)
		? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
		: VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
	if(finalLayout != VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL)
	{
		CmdTransitionImageLayout(
			commandBuffer,
			texture->GetImage(),
			VK_IMAGE_ASPECT_DEPTH_BIT,
			VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
			finalLayout);
	}
	SetRecordedImageLayout(texture, finalLayout);
	return true;
}

bool VulkanDevice::Impl::RecordGraphicsPass(
	VkCommandBuffer commandBuffer,
	const VulkanCommandList& commandList,
	uint32_t firstDraw,
	uint32_t drawCount) {
	if(drawCount == 0u || firstDraw >= commandList.m_drawCalls.size()) return true;
	const VulkanCommandList::DrawCall& passDraw = commandList.m_drawCalls[firstDraw];
	RenderingTarget& target = m_renderingTargetScratch;
	if (!ResolveGraphicsTarget(commandList, passDraw, target)) return false;
	const VkExtent2D renderExtent = target.extent;

	for(RenderingColorTarget& color : target.colors)
	{
		const VkImageLayout oldColorLayout = color.isSwapchain
			? m_recordedSwapchainLayout
			: color.texture->GetImageLayout();
		CmdTransitionImageLayout(
			commandBuffer,
			color.image,
			VK_IMAGE_ASPECT_COLOR_BIT,
			oldColorLayout,
			VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
	}
	if(target.depthTexture != nullptr)
	{
		CmdTransitionImageLayout(
			commandBuffer,
			target.depthTexture->GetImage(),
			VK_IMAGE_ASPECT_DEPTH_BIT,
			target.depthTexture->GetImageLayout(),
			VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
	}

	std::vector<VkRenderingAttachmentInfo>& colorAttachments = m_colorAttachmentsScratch;
	colorAttachments.clear();
	colorAttachments.resize(target.colors.size());
	for(size_t colorIndex = 0u; colorIndex < target.colors.size(); ++colorIndex)
	{
		VkRenderingAttachmentInfo& attachment = colorAttachments[colorIndex];
		attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
		attachment.imageView = target.colors[colorIndex].view;
		attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
		attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	}
	VkRenderingAttachmentInfo depthAttachment{};
	depthAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
	depthAttachment.imageView = target.depthTexture != nullptr ? target.depthTexture->GetImageView() : VK_NULL_HANDLE;
	depthAttachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
	depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
	depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	VkRenderingInfo renderingInfo{};
	renderingInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
	renderingInfo.renderArea.extent = renderExtent;
	renderingInfo.layerCount = 1;
	renderingInfo.colorAttachmentCount = static_cast<uint32_t>(colorAttachments.size());
	renderingInfo.pColorAttachments = colorAttachments.empty() ? nullptr : colorAttachments.data();
	renderingInfo.pDepthAttachment = target.depthTexture != nullptr ? &depthAttachment : nullptr;

	vkCmdBeginRendering(commandBuffer, &renderingInfo);

	VkPipeline currentPipeline = VK_NULL_HANDLE;
	VkDescriptorSet currentBindlessDescriptorSet = VK_NULL_HANDLE;
	const uint32_t endDraw = std::min<uint32_t>(firstDraw + drawCount, static_cast<uint32_t>(commandList.m_drawCalls.size()));
	for (uint32_t drawIndex = firstDraw; drawIndex < endDraw; ++drawIndex) {
		const VulkanCommandList::DrawCall& drawCall = commandList.m_drawCalls[drawIndex];
		const VulkanPipelineState* pipelineState = dynamic_cast<const VulkanPipelineState*>(drawCall.pipelineState);
		if (pipelineState == nullptr) return false;
		const ProfileResources* profileResources = GetProfileResources(pipelineState->GetResourceProfile());
		if(profileResources == nullptr || profileResources->descriptorSetLayout == VK_NULL_HANDLE) return false;
		const bool usesBindlessTextures = pipelineState->UsesBindlessTextures();
		const VulkanPipeline* pipeline = pipelineState->GetPipelineForFormats(
			m_context,
			target.colorFormats.empty() ? nullptr : target.colorFormats.data(),
			static_cast<uint32_t>(target.colorFormats.size()),
			target.depthTexture != nullptr ? target.depthTexture->GetVkFormat() : VK_FORMAT_UNDEFINED,
			profileResources->descriptorSetLayout,
			usesBindlessTextures ? m_bindlessDescriptorSetLayout : VK_NULL_HANDLE);
		if (pipeline == nullptr) return false;

		if (currentPipeline != pipeline->GetPipeline()) {
			vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline->GetPipeline());
			currentPipeline = pipeline->GetPipeline();
			currentBindlessDescriptorSet = VK_NULL_HANDLE;
		}

		if (!PushDrawDescriptors(commandBuffer, pipeline->GetLayout(), drawCall, drawIndex)) return false;
		const VkDescriptorSet bindlessDescriptorSet = m_currentFrameIndex < m_bindlessDescriptorSets.size()
			? m_bindlessDescriptorSets[m_currentFrameIndex]
			: VK_NULL_HANDLE;
		if (usesBindlessTextures && bindlessDescriptorSet == VK_NULL_HANDLE) return false;
		if (usesBindlessTextures) {
			if (bindlessDescriptorSet != currentBindlessDescriptorSet) {
				vkCmdBindDescriptorSets(
					commandBuffer,
					VK_PIPELINE_BIND_POINT_GRAPHICS,
					pipeline->GetLayout(),
					1u,
					1u,
					&bindlessDescriptorSet,
					0u,
					nullptr);
				currentBindlessDescriptorSet = bindlessDescriptorSet;
			}
		}

		VkViewport viewport{};
		if (drawCall.hasViewport) {
			viewport.x = drawCall.viewport.x;
			viewport.y = drawCall.viewport.y;
			viewport.width = drawCall.viewport.width;
			viewport.height = drawCall.viewport.height;
			viewport.minDepth = drawCall.viewport.minDepth;
			viewport.maxDepth = drawCall.viewport.maxDepth;
		} else {
			viewport.x = 0.0f;
			viewport.y = 0.0f;
			viewport.width = static_cast<float>(renderExtent.width);
			viewport.height = static_cast<float>(renderExtent.height);
			viewport.minDepth = 0.0f;
			viewport.maxDepth = 1.0f;
		}

		VkRect2D scissor{};
		if (drawCall.hasScissor) {
			scissor.offset = { drawCall.scissor.x, drawCall.scissor.y };
			scissor.extent = { drawCall.scissor.width, drawCall.scissor.height };
		} else {
			scissor.offset = { 0, 0 };
			scissor.extent = renderExtent;
		}

		vkCmdSetViewport(commandBuffer, 0, 1, &viewport);
		vkCmdSetScissor(commandBuffer, 0, 1, &scissor);

		// 셰이더가 storage 버퍼에서 정점/인덱스를 수동 fetch하므로 IA 인덱스버퍼 없이 그린다.
		const uint32_t drawVertexCount = drawCall.indexed ? drawCall.indexCount : drawCall.vertexCount;
		if (drawVertexCount > 0) {
			vkCmdDraw(commandBuffer, drawVertexCount, drawCall.instanceCount, 0, drawCall.startInstance);
		}
	}

	vkCmdEndRendering(commandBuffer);
	for(RenderingColorTarget& color : target.colors)
	{
		const VkImageLayout finalColorLayout = color.isSwapchain
			? VK_IMAGE_LAYOUT_PRESENT_SRC_KHR
			: HasTextureUsage(color.texture->GetUsage(), dy::RHI::TextureUsage::ShaderResource)
				? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
				: VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		CmdTransitionImageLayout(
			commandBuffer,
			color.image,
			VK_IMAGE_ASPECT_COLOR_BIT,
			VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
			finalColorLayout);
		if(color.isSwapchain) m_recordedSwapchainLayout = finalColorLayout;
		else SetRecordedImageLayout(color.texture, finalColorLayout);
	}
	if(target.depthTexture != nullptr)
	{
		const VkImageLayout finalDepthLayout = target.colors.empty()
			&& HasTextureUsage(target.depthTexture->GetUsage(), dy::RHI::TextureUsage::ShaderResource)
			? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
			: VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
		if(finalDepthLayout != VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL)
		{
			CmdTransitionImageLayout(
				commandBuffer,
				target.depthTexture->GetImage(),
				VK_IMAGE_ASPECT_DEPTH_BIT,
				VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
				finalDepthLayout);
		}
		SetRecordedImageLayout(target.depthTexture, finalDepthLayout);
	}
	return true;
}

bool VulkanDevice::Impl::ResolveGraphicsTarget(
	const VulkanCommandList& commandList,
	const VulkanCommandList::DrawCall& drawCall,
	RenderingTarget& target)
{
	target.colors.clear();
	target.colorFormats.clear();
	target.depthTexture = nullptr;
	target.extent = {};
	if(drawCall.renderTargetCount > m_maxColorAttachments)
	{
		SDL_Log("Vulkan color render-target count exceeds the selected device limit.");
		return false;
	}
	if(!drawCall.renderTargetsValid)
	{
		SDL_Log("Vulkan color render-target array is invalid.");
		return false;
	}
	if(drawCall.renderTargetOffset > commandList.m_renderTargets.size()
		|| drawCall.renderTargetCount > commandList.m_renderTargets.size() - drawCall.renderTargetOffset)
	{
		SDL_Log("Vulkan color render-target array is invalid.");
		return false;
	}

	VulkanTexture* depthTexture = dynamic_cast<VulkanTexture*>(drawCall.depthStencil);
	if(drawCall.depthStencil != nullptr
		&& (depthTexture == nullptr
			|| depthTexture->GetImageView() == VK_NULL_HANDLE
			|| !HasTextureUsage(depthTexture->GetUsage(), dy::RHI::TextureUsage::DepthStencil)))
	{
		SDL_Log("Vulkan depth target is not a valid depth-stencil texture.");
		return false;
	}

	if(drawCall.renderTargetCount == 0u && depthTexture != nullptr)
	{
		target.depthTexture = depthTexture;
		target.extent = { depthTexture->GetWidth(), depthTexture->GetHeight() };
		return true;
	}

	const uint32_t colorTargetCount = drawCall.renderTargetCount == 0u
		? 1u
		: drawCall.renderTargetCount;
	target.colors.reserve(colorTargetCount);
	target.colorFormats.reserve(colorTargetCount);
	for(uint32_t colorIndex = 0u; colorIndex < colorTargetCount; ++colorIndex)
	{
		dy::RHI::ITexture* requestedTarget = drawCall.renderTargetCount == 0u
			? m_backBuffer
			: commandList.m_renderTargets[drawCall.renderTargetOffset + colorIndex];
		if(requestedTarget == nullptr && drawCall.renderTargetCount > 1u)
		{
			SDL_Log("Vulkan MRT color targets must not contain null entries.");
			return false;
		}

		RenderingColorTarget color{};
		VkFormat colorFormat = VK_FORMAT_UNDEFINED;
		VkExtent2D colorExtent{};
		if(requestedTarget == nullptr || requestedTarget == m_backBuffer)
		{
			const auto& images = m_swapchain.GetImages();
			const auto& views = m_swapchain.GetImageViews();
			if(m_currentImageIndex >= images.size() || m_currentImageIndex >= views.size()) return false;
			color.image = images[m_currentImageIndex];
			color.view = views[m_currentImageIndex];
			color.isSwapchain = true;
			colorFormat = m_swapchain.GetImageFormat();
			colorExtent = m_swapchain.GetExtent();
		}
		else
		{
			VulkanTexture* texture = dynamic_cast<VulkanTexture*>(requestedTarget);
			if(texture == nullptr
				|| texture->GetImageView() == VK_NULL_HANDLE
				|| !HasTextureUsage(texture->GetUsage(), dy::RHI::TextureUsage::RenderTarget))
			{
				SDL_Log("Vulkan color target is not a valid render-target texture.");
				return false;
			}
			color.image = texture->GetImage();
			color.view = texture->GetImageView();
			color.texture = texture;
			colorFormat = texture->GetVkFormat();
			colorExtent = { texture->GetWidth(), texture->GetHeight() };
		}

		if(std::any_of(
			target.colors.begin(),
			target.colors.end(),
			[&color](const RenderingColorTarget& existing) { return existing.image == color.image; }))
		{
			SDL_Log("Vulkan MRT color targets must reference distinct images.");
			return false;
		}
		if(!target.colors.empty()
			&& (target.extent.width != colorExtent.width || target.extent.height != colorExtent.height))
		{
			SDL_Log("Vulkan MRT color target sizes do not match.");
			return false;
		}
		if(target.colors.empty()) target.extent = colorExtent;
		target.colors.push_back(color);
		target.colorFormats.push_back(colorFormat);
	}

	if(depthTexture != nullptr
		&& (depthTexture->GetWidth() != target.extent.width || depthTexture->GetHeight() != target.extent.height))
	{
		SDL_Log("Vulkan color/depth target sizes do not match.");
		return false;
	}
	target.depthTexture = depthTexture;
	return true;
}

void VulkanDevice::Impl::SetRecordedImageLayout(VulkanTexture* texture, VkImageLayout layout)
{
	if(texture == nullptr) return;
	const auto recorded = std::find_if(
		m_recordedImageLayouts.begin(),
		m_recordedImageLayouts.end(),
		[texture](const RecordedImageLayout& item) { return item.texture == texture; });
	if(recorded == m_recordedImageLayouts.end())
	{
		m_recordedImageLayouts.push_back(RecordedImageLayout{ texture, texture->GetImageLayout() });
	}
	texture->SetImageLayout(layout);
}

void VulkanDevice::Impl::RollbackRecordedImageLayouts()
{
	for(auto item = m_recordedImageLayouts.rbegin(); item != m_recordedImageLayouts.rend(); ++item)
	{
		if(item->texture != nullptr) item->texture->SetImageLayout(item->originalLayout);
	}
	m_recordedImageLayouts.clear();
}

void VulkanDevice::Impl::CommitRecordedImageLayouts()
{
	m_recordedImageLayouts.clear();
}

bool VulkanDevice::Impl::CreateCommandPool() {
	VkCommandPoolCreateInfo poolInfo{};
	poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	poolInfo.queueFamilyIndex = m_context.queueIndices.graphicsFamily;
	return vkCreateCommandPool(m_context.device, &poolInfo, nullptr, &m_commandPool) == VK_SUCCESS;
}

bool VulkanDevice::Impl::CreateCommandBuffer() {
	VkCommandBufferAllocateInfo allocInfo{};
	allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	allocInfo.commandPool = m_commandPool;
	allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	allocInfo.commandBufferCount = m_maxFramesInFlight;
	m_commandBuffers.resize(m_maxFramesInFlight);
	return vkAllocateCommandBuffers(m_context.device, &allocInfo, m_commandBuffers.data()) == VK_SUCCESS;
}

bool VulkanDevice::Impl::CreateGpuTimestampQueryPools() {
	m_gpuTimestampsSupported = m_timestampValidBits > 0u && m_timestampPeriodNanoseconds > 0.0;
	if (!m_gpuTimestampsSupported) return true;

	m_gpuTimestampQueryPools.assign(m_maxFramesInFlight, VK_NULL_HANDLE);
	m_gpuTimestampPairs.resize(m_maxFramesInFlight);
	m_gpuTimestampQueryCounts.assign(m_maxFramesInFlight, 0u);
	VkQueryPoolCreateInfo createInfo{};
	createInfo.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
	createInfo.queryType = VK_QUERY_TYPE_TIMESTAMP;
	createInfo.queryCount = kMaxGpuTimestampQueriesPerFrame;
	for (uint32_t i = 0; i < m_maxFramesInFlight; ++i) {
		if (vkCreateQueryPool(m_context.device, &createInfo, nullptr, &m_gpuTimestampQueryPools[i]) != VK_SUCCESS) {
			for (VkQueryPool queryPool : m_gpuTimestampQueryPools) {
				if (queryPool != VK_NULL_HANDLE) vkDestroyQueryPool(m_context.device, queryPool, nullptr);
			}
			m_gpuTimestampQueryPools.clear();
			m_gpuTimestampPairs.clear();
			m_gpuTimestampQueryCounts.clear();
			m_gpuTimestampsSupported = false;
			break;
		}
	}
	m_commandList->SetGpuTimestampScopeCapacity(m_gpuTimestampsSupported ? 32u : 0u);
	return true;
}

void VulkanDevice::Impl::CollectGpuTimestampResults(uint32_t frameIndex) {
	if (!m_gpuTimestampsSupported || frameIndex >= m_gpuTimestampQueryPools.size()) return;
	const uint32_t queryCount = m_gpuTimestampQueryCounts[frameIndex];
	if (queryCount == 0u || m_gpuTimestampPairs[frameIndex].empty()) return;

	std::vector<uint64_t> ticks(queryCount);
	const VkResult result = vkGetQueryPoolResults(
		m_context.device,
		m_gpuTimestampQueryPools[frameIndex],
		0,
		queryCount,
		ticks.size() * sizeof(uint64_t),
		ticks.data(),
		sizeof(uint64_t),
		VK_QUERY_RESULT_64_BIT);
	if (result != VK_SUCCESS) return;

	++m_timestampFrameSerial;
	const uint64_t validMask = m_timestampValidBits >= 64u
		? UINT64_MAX
		: ((uint64_t{1} << m_timestampValidBits) - 1u);
	for (const GpuTimestampPair& pair : m_gpuTimestampPairs[frameIndex]) {
		const uint64_t begin = ticks[pair.beginQuery] & validMask;
		const uint64_t end = ticks[pair.endQuery] & validMask;
		const uint64_t delta = m_timestampValidBits >= 64u
			? end - begin
			: (end - begin) & validMask;
		const uint64_t durationNanoseconds = static_cast<uint64_t>(
			static_cast<long double>(delta) * static_cast<long double>(m_timestampPeriodNanoseconds));
		m_completedGpuTimestamps[pair.name] = { durationNanoseconds, m_timestampFrameSerial };
	}
	m_gpuTimestampPairs[frameIndex].clear();
	m_gpuTimestampQueryCounts[frameIndex] = 0u;
}

bool VulkanDevice::Impl::TryGetLastGpuTimestamp(const char* name, dy::RHI::GpuTimestampResult& result) const {
	if (name == nullptr) return false;
	const auto it = m_completedGpuTimestamps.find(name);
	if (it == m_completedGpuTimestamps.end()) return false;
	result = it->second;
	return true;
}

bool VulkanDevice::Impl::CreateSyncObjects() {
	VkSemaphoreCreateInfo semInfo{};
	semInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
	VkFenceCreateInfo fenceInfo{};
	fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
	
	m_imageAvailableSemaphores.resize(m_maxFramesInFlight);
	m_inFlightFences.resize(m_maxFramesInFlight);
	m_renderFinishedSemaphores.resize(m_swapchain.GetImageCount()); 

	for (uint32_t i = 0; i < m_maxFramesInFlight; ++i) {
		if (vkCreateSemaphore(m_context.device, &semInfo, nullptr, &m_imageAvailableSemaphores[i]) != VK_SUCCESS || 
			vkCreateFence(m_context.device, &fenceInfo, nullptr, &m_inFlightFences[i]) != VK_SUCCESS) return false;
	}
	for (size_t i = 0; i < m_swapchain.GetImageCount(); ++i) {
		if (vkCreateSemaphore(m_context.device, &semInfo, nullptr, &m_renderFinishedSemaphores[i]) != VK_SUCCESS) return false;
	}
	m_imagesInFlight.assign(m_swapchain.GetImageCount(), VK_NULL_HANDLE);
	return true;
}

bool VulkanDevice::Impl::CreateFallbackTexture() {
	dy::RHI::TextureDesc desc{};
	desc.width = kFallbackTextureWidth;
	desc.height = kFallbackTextureHeight;
	desc.depthOrArraySize = 1;
	desc.mipLevels = 1;
	desc.format = dy::RHI::Format::R8G8B8A8_UNORM;
	desc.usage = dy::RHI::TextureUsage::ShaderResource;

	std::unique_ptr<VulkanTexture> texture(new VulkanTexture(desc));
	const bool created =
		texture->Initialize(m_context, desc, VK_FORMAT_R8G8B8A8_SRGB) &&
		texture->CreateDefaultSampler() &&
		texture->UploadRGBA8(m_context, m_commandPool, kFallbackTexturePixels.data(), desc.width, desc.height);

	if (!created) return false;

	delete m_fallbackTexture;
	m_fallbackTexture = texture.release();
	return true;
}

bool VulkanDevice::Impl::CreateDepthResources() {
	const VkExtent2D extent = m_swapchain.GetExtent();
	if (m_depthFormat == VK_FORMAT_UNDEFINED) {
		m_depthFormat = FindDepthFormat();
	}

	dy::RHI::TextureDesc desc{};
	desc.width = extent.width;
	desc.height = extent.height;
	desc.depthOrArraySize = 1;
	desc.mipLevels = 1;
	desc.format = ToRhiDepthFormat(m_depthFormat);
	desc.usage = dy::RHI::TextureUsage::DepthStencil;

	std::unique_ptr<VulkanTexture> depthTexture(new VulkanTexture(desc));
	if (!depthTexture->Initialize(m_context, desc, m_depthFormat)) return false;

	delete m_depthTexture;
	m_depthTexture = depthTexture.release();
	return true;
}

bool VulkanDevice::Impl::CreateDrawConstantBuffers()
{
	uint64_t alignedStride = 0u;
	if(!TryAlignVulkanUniformStride(
		m_shaderLayout.pushConstantRangeSize,
		m_capabilities.limits.minUniformBufferOffsetAlignment,
		alignedStride))
	{
		SDL_Log("Failed to align Vulkan draw constant UBO stride.");
		return false;
	}
	if(alignedStride > std::numeric_limits<uint32_t>::max()) return false;
	const uint64_t drawConstantCapacity = m_descriptorCapacityPerFrame;
	if(drawConstantCapacity == 0u || drawConstantCapacity > std::numeric_limits<uint32_t>::max()) return false;
	const uint64_t totalSize = alignedStride * drawConstantCapacity;
	if(totalSize / drawConstantCapacity != alignedStride) return false;
	if(totalSize == 0u || totalSize > std::numeric_limits<uint32_t>::max())
	{
		SDL_Log("Vulkan draw constant UBO capacity exceeds dynamic-offset range.");
		return false;
	}

	m_drawConstantStride = static_cast<VkDeviceSize>(alignedStride);
	m_drawConstantCapacity = static_cast<uint32_t>(drawConstantCapacity);
	m_drawConstantFrames.resize(m_maxFramesInFlight);
	try
	{
		for(DrawConstantFrame& frame : m_drawConstantFrames)
		{
			VulkanResources::CreateBuffer(
				m_context,
				static_cast<VkDeviceSize>(totalSize),
				VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
				VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
				frame.buffer,
				frame.memory);
			void* mapped = nullptr;
			if(vkMapMemory(m_context.device, frame.memory, 0, static_cast<VkDeviceSize>(totalSize), 0, &mapped) != VK_SUCCESS || mapped == nullptr)
				throw std::runtime_error("failed to persistently map Vulkan draw constant UBO");
			frame.mapped = static_cast<uint8_t*>(mapped);
			std::memset(frame.mapped, 0, static_cast<size_t>(totalSize));
		}
	}
	catch(const std::exception& e)
	{
		SDL_Log("Vulkan draw constant UBO creation failed: %s", e.what());
		DestroyDrawConstantBuffers();
		return false;
	}
	return true;
}

bool VulkanDevice::Impl::UploadDrawConstants(const VulkanCommandList& commandList)
{
	if(commandList.m_drawCalls.size() > m_descriptorCapacityPerFrame)
	{
		if(!m_drawCapacityErrorReported)
		{
			SDL_Log("Vulkan draw capacity exceeded: requested=%zu capacity=%u.",
				commandList.m_drawCalls.size(),
				m_descriptorCapacityPerFrame);
			m_drawCapacityErrorReported = true;
		}
		return false;
	}
	m_drawCapacityErrorReported = false;
	if(m_currentFrameIndex >= m_drawConstantFrames.size()) return false;
	DrawConstantFrame& frame = m_drawConstantFrames[m_currentFrameIndex];
	if(frame.mapped == nullptr) return false;

	for(uint32_t drawIndex = 0u; drawIndex < commandList.m_drawCalls.size(); ++drawIndex)
	{
		const VulkanCommandList::DrawCall& drawCall = commandList.m_drawCalls[drawIndex];
		uint32_t firstVertex = drawCall.startVertex;
		const uint32_t firstVertexOffset = m_shaderLayout.drawMetadataPushConstantOffset + 2u * sizeof(uint32_t);
		if(drawCall.indexed && drawCall.pushConstantSize >= firstVertexOffset + sizeof(firstVertex))
		{
			std::memcpy(
				&firstVertex,
				drawCall.pushConstants.data() + firstVertexOffset,
				sizeof(firstVertex));
		}
		uint8_t* constants = frame.mapped + static_cast<size_t>(m_drawConstantStride) * drawIndex;
		if(!PrepareVulkanDrawConstants(
			drawCall.pushConstants.data(),
			drawCall.pushConstantSize,
			m_shaderLayout,
			drawCall.firstIndex,
			drawCall.baseVertex,
			firstVertex,
			constants,
			static_cast<uint32_t>(m_drawConstantStride)))
		{
			return false;
		}
	}
	return true;
}

void VulkanDevice::Impl::DestroyDrawConstantBuffers()
{
	if(m_context.device == VK_NULL_HANDLE)
	{
		m_drawConstantFrames.clear();
		return;
	}
	for(DrawConstantFrame& frame : m_drawConstantFrames)
	{
		if(frame.mapped != nullptr && frame.memory != VK_NULL_HANDLE) vkUnmapMemory(m_context.device, frame.memory);
		if(frame.buffer != VK_NULL_HANDLE) vkDestroyBuffer(m_context.device, frame.buffer, nullptr);
		if(frame.memory != VK_NULL_HANDLE) vkFreeMemory(m_context.device, frame.memory, nullptr);
		frame = {};
	}
	m_drawConstantFrames.clear();
	m_drawConstantStride = 0u;
	m_drawConstantCapacity = 0u;
}

bool VulkanDevice::Impl::CreateDescriptorSetLayout(
	dy::RHI::GraphicsResourceProfile profile,
	VkDescriptorSetLayout& outLayout)
{
	outLayout = VK_NULL_HANDLE;
	std::vector<VkDescriptorSetLayoutBinding> bindings;
	bindings.reserve(13u);
	auto addBinding = [&bindings](uint32_t binding, VkDescriptorType type, VkShaderStageFlags stageFlags)
	{
		VkDescriptorSetLayoutBinding value{};
		value.binding = binding;
		value.descriptorType = type;
		value.descriptorCount = 1u;
		value.stageFlags = stageFlags;
		bindings.push_back(value);
	};

	if(profile != dy::RHI::GraphicsResourceProfile::Bindless)
	{
		addBinding(m_shaderLayout.baseColorTextureBinding, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT);
		addBinding(m_shaderLayout.metallicRoughnessTextureBinding, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT);
		addBinding(m_shaderLayout.normalTextureBinding, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT);
		addBinding(m_shaderLayout.occlusionTextureBinding, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT);
		addBinding(m_shaderLayout.emissiveTextureBinding, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT);
	}
	addBinding(m_shaderLayout.shadowSamplerBinding, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT);
	addBinding(m_shaderLayout.lightingConstantBinding, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT);
	addBinding(m_shaderLayout.shadowMatrixBinding, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT);
	addBinding(m_shaderLayout.vertexStorageBinding, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_VERTEX_BIT);
	addBinding(m_shaderLayout.indexStorageBinding, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_VERTEX_BIT);
	if(profile == dy::RHI::GraphicsResourceProfile::PerDrawSkin)
	{
		addBinding(m_shaderLayout.skinInfluenceStorageBinding, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_VERTEX_BIT);
		addBinding(m_shaderLayout.skinPaletteStorageBinding, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_VERTEX_BIT);
	}
	else
	{
		addBinding(m_shaderLayout.bindlessTransformStorageBinding, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_VERTEX_BIT);
	}
	addBinding(m_shaderLayout.drawConstantsBinding, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT);

	VkDescriptorSetLayoutCreateInfo info{};
	info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	info.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT;
	info.bindingCount = static_cast<uint32_t>(bindings.size());
	info.pBindings = bindings.data();
	return vkCreateDescriptorSetLayout(m_context.device, &info, nullptr, &outLayout) == VK_SUCCESS;
}

bool VulkanDevice::Impl::CreateBindlessDescriptorSetLayout() {
	VkPhysicalDeviceProperties properties{};
	vkGetPhysicalDeviceProperties(m_context.physicalDevice, &properties);
	const uint32_t textureLimit = std::min(
		properties.limits.maxPerStageDescriptorSampledImages,
		properties.limits.maxDescriptorSetSampledImages);
	if (m_maxBindlessTextures > textureLimit) {
		m_maxBindlessTextures = textureLimit;
	}
	if (m_maxBindlessTextures == 0u) return false;

	VkDescriptorSetLayoutBinding textureBinding{};
	textureBinding.binding = 0;
	textureBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	textureBinding.descriptorCount = m_maxBindlessTextures;
	textureBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

	VkDescriptorSetLayoutCreateInfo info{};
	info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	info.bindingCount = 1;
	info.pBindings = &textureBinding;
	return vkCreateDescriptorSetLayout(m_context.device, &info, nullptr, &m_bindlessDescriptorSetLayout) == VK_SUCCESS;
}

bool VulkanDevice::Impl::CreateBindlessDescriptorPool()
{
	const uint64_t descriptorCount = static_cast<uint64_t>(m_maxBindlessTextures) * m_maxFramesInFlight;
	if(descriptorCount == 0u || descriptorCount > std::numeric_limits<uint32_t>::max()) return false;
	VkDescriptorPoolSize poolSize{};
	poolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	poolSize.descriptorCount = static_cast<uint32_t>(descriptorCount);
	VkDescriptorPoolCreateInfo info{};
	info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	info.maxSets = m_maxFramesInFlight;
	info.poolSizeCount = 1u;
	info.pPoolSizes = &poolSize;
	return vkCreateDescriptorPool(m_context.device, &info, nullptr, &m_bindlessDescriptorPool) == VK_SUCCESS;
}

bool VulkanDevice::Impl::EnsureProfileResources(dy::RHI::GraphicsResourceProfile profile)
{
	ProfileResources* resources = GetProfileResources(profile);
	if(resources == nullptr) return false;
	if(resources->descriptorSetLayout != VK_NULL_HANDLE) return true;
	return CreateDescriptorSetLayout(profile, resources->descriptorSetLayout);
}

VulkanDevice::Impl::ProfileResources* VulkanDevice::Impl::GetProfileResources(dy::RHI::GraphicsResourceProfile profile)
{
	const size_t index = ResourceProfileIndex(profile);
	return index < m_profileResources.size() ? &m_profileResources[index] : nullptr;
}

const VulkanDevice::Impl::ProfileResources* VulkanDevice::Impl::GetProfileResources(dy::RHI::GraphicsResourceProfile profile) const
{
	const size_t index = ResourceProfileIndex(profile);
	return index < m_profileResources.size() ? &m_profileResources[index] : nullptr;
}

bool VulkanDevice::Impl::CreateBindlessDescriptorSet() {
	if (m_bindlessDescriptorSetLayout == VK_NULL_HANDLE) return false;

	std::vector<VkDescriptorSetLayout> layouts(m_maxFramesInFlight, m_bindlessDescriptorSetLayout);
	VkDescriptorSetAllocateInfo alloc{};
	alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	alloc.descriptorPool = m_bindlessDescriptorPool;
	alloc.descriptorSetCount = m_maxFramesInFlight;
	alloc.pSetLayouts = layouts.data();
	m_bindlessDescriptorSets.resize(m_maxFramesInFlight, VK_NULL_HANDLE);
	if (vkAllocateDescriptorSets(m_context.device, &alloc, m_bindlessDescriptorSets.data()) != VK_SUCCESS) return false;
	m_bindlessDescriptorFrameRevisions.assign(m_maxFramesInFlight, 0u);
	m_bindlessTextures.assign(m_maxBindlessTextures, static_cast<const VulkanTexture*>(m_fallbackTexture));
	for(uint32_t frameIndex = 0u; frameIndex < m_maxFramesInFlight; ++frameIndex)
		if(!ApplyBindlessDescriptorSet(frameIndex)) return false;

	return true;
}

bool VulkanDevice::Impl::ApplyBindlessDescriptorSet(uint32_t frameIndex) {
	if(frameIndex >= m_bindlessDescriptorSets.size()
		|| frameIndex >= m_bindlessDescriptorFrameRevisions.size()) return false;
	if(m_bindlessDescriptorFrameRevisions[frameIndex] == m_bindlessDescriptorRevision) return true;
	const VulkanTexture* fallbackTexture = static_cast<const VulkanTexture*>(m_fallbackTexture);
	if (fallbackTexture == nullptr || fallbackTexture->GetImageView() == VK_NULL_HANDLE || fallbackTexture->GetSampler() == VK_NULL_HANDLE) {
		return false;
	}

	std::vector<VkDescriptorImageInfo> imageInfos(m_maxBindlessTextures);
	for (uint32_t index = 0u; index < imageInfos.size(); ++index) {
		const VulkanTexture* texture = index < m_bindlessTextures.size() ? m_bindlessTextures[index] : nullptr;
		if(texture == nullptr || texture->GetImageView() == VK_NULL_HANDLE || texture->GetSampler() == VK_NULL_HANDLE)
			texture = fallbackTexture;
		imageInfos[index].sampler = texture->GetSampler();
		imageInfos[index].imageView = texture->GetImageView();
		imageInfos[index].imageLayout = texture->GetImageLayout();
	}

	VkWriteDescriptorSet write{};
	write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	write.dstSet = m_bindlessDescriptorSets[frameIndex];
	write.dstBinding = 0;
	write.descriptorCount = m_maxBindlessTextures;
	write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	write.pImageInfo = imageInfos.data();
	vkUpdateDescriptorSets(m_context.device, 1, &write, 0, nullptr);
	m_bindlessDescriptorFrameRevisions[frameIndex] = m_bindlessDescriptorRevision;
	return true;
}

bool VulkanDevice::Impl::UpdateComputeDescriptorSets(const VulkanCommandList& commandList)
{
	if(commandList.m_computeDispatches.size() > m_descriptorCapacityPerFrame)
	{
		SDL_Log(
			"Vulkan compute descriptor capacity exceeded: requested=%zu capacity=%u.",
			commandList.m_computeDispatches.size(),
			m_descriptorCapacityPerFrame);
		return false;
	}
	for(uint32_t dispatchIndex = 0u; dispatchIndex < commandList.m_computeDispatches.size(); ++dispatchIndex)
	{
		const VulkanCommandList::ComputeDispatch& dispatch = commandList.m_computeDispatches[dispatchIndex];
		VulkanComputePipelineState* pipelineState =
			dynamic_cast<VulkanComputePipelineState*>(dispatch.pipelineState);
		if(pipelineState == nullptr
			|| !pipelineState->EnsureDescriptorSets(m_currentFrameIndex, dispatchIndex + 1u)) return false;
		const VkDescriptorSet descriptorSet = pipelineState->GetDescriptorSet(m_currentFrameIndex, dispatchIndex);
		if(descriptorSet == VK_NULL_HANDLE) return false;

		const uint32_t storageBufferCount = pipelineState->GetStorageBufferCount();
		std::vector<VkDescriptorBufferInfo> bufferInfos(storageBufferCount);
		std::vector<VkWriteDescriptorSet> writes(storageBufferCount);
		for(uint32_t binding = 0u; binding < storageBufferCount; ++binding)
		{
			const VulkanCommandList::BufferBinding& captured = dispatch.storageBuffers[binding];
			const VulkanBuffer* buffer = dynamic_cast<const VulkanBuffer*>(captured.buffer);
			if(buffer == nullptr) return false;
			const uint64_t range = captured.size > 0u
				? captured.size
				: static_cast<uint64_t>(buffer->GetSize())
					- std::min<uint64_t>(captured.offset, buffer->GetSize());
			if(!ValidateStorageBufferBinding(
				m_capabilities,
				captured.offset,
				range,
				buffer->GetSize()))
			{
				SDL_Log(
					"Invalid Vulkan compute storage buffer binding %u: offset=%u range=%llu buffer=%u.",
					binding,
					captured.offset,
					static_cast<unsigned long long>(range),
					buffer->GetSize());
				return false;
			}
			bufferInfos[binding].buffer = buffer->GetHandle();
			bufferInfos[binding].offset = captured.offset;
			bufferInfos[binding].range = range;
			writes[binding].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
			writes[binding].dstSet = descriptorSet;
			writes[binding].dstBinding = binding;
			writes[binding].descriptorCount = 1u;
			writes[binding].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
			writes[binding].pBufferInfo = &bufferInfos[binding];
		}
		vkUpdateDescriptorSets(
			m_context.device,
			static_cast<uint32_t>(writes.size()),
			writes.data(),
			0u,
			nullptr);
	}
	return true;
}

bool VulkanDevice::Impl::PushDrawDescriptors(
	VkCommandBuffer commandBuffer,
	VkPipelineLayout pipelineLayout,
	const VulkanCommandList::DrawCall& drawCall,
	uint32_t drawConstantSlot)
{
	const VulkanPipelineState* pipelineState = dynamic_cast<const VulkanPipelineState*>(drawCall.pipelineState);
	if(pipelineState == nullptr) return false;

	const bool usesBindlessTextures = pipelineState->UsesBindlessTextures();
	const VulkanBuffer* vertexBuffer = dynamic_cast<const VulkanBuffer*>(drawCall.geometry.vertexBuffer);
	const VulkanBuffer* indexBuffer = dynamic_cast<const VulkanBuffer*>(drawCall.geometry.indexBuffer);
	if (vertexBuffer == nullptr || (drawCall.indexed && indexBuffer == nullptr)) return false;

	std::vector<VkWriteDescriptorSet> writes;
	writes.reserve(m_shaderLayout.extendedDescriptorBindingCount);

	const VulkanTexture* fallbackTexture = static_cast<const VulkanTexture*>(m_fallbackTexture);
	const bool hasFallbackTexture =
		fallbackTexture != nullptr &&
		fallbackTexture->GetImageView() != VK_NULL_HANDLE &&
		fallbackTexture->GetSampler() != VK_NULL_HANDLE;

	const std::array<uint32_t, kMaxMaterialTextures> materialSamplerBindings = {
		m_shaderLayout.baseColorTextureBinding,
		m_shaderLayout.metallicRoughnessTextureBinding,
		m_shaderLayout.normalTextureBinding,
		m_shaderLayout.occlusionTextureBinding,
		m_shaderLayout.emissiveTextureBinding
	};
	std::array<VkDescriptorImageInfo, kMaxMaterialTextures> materialTextureInfos = {};
	if (!usesBindlessTextures) {
		for (uint32_t i = 0; i < m_shaderLayout.materialTextureBindingCount; ++i) {
			const uint32_t binding = materialSamplerBindings[i];
			const VulkanTexture* texture = nullptr;
			if (binding < drawCall.textures.size()) {
				texture = dynamic_cast<const VulkanTexture*>(drawCall.textures[binding]);
			}
			if (texture == nullptr || texture->GetImageView() == VK_NULL_HANDLE || texture->GetSampler() == VK_NULL_HANDLE) {
				texture = hasFallbackTexture ? fallbackTexture : nullptr;
			}
			if (texture == nullptr) continue;

			materialTextureInfos[i].sampler = texture->GetSampler();
			materialTextureInfos[i].imageView = texture->GetImageView();
			materialTextureInfos[i].imageLayout = texture->GetImageLayout();

			VkWriteDescriptorSet textureWrite{};
			textureWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
			textureWrite.dstBinding = binding;
			textureWrite.descriptorCount = 1;
			textureWrite.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
			textureWrite.pImageInfo = &materialTextureInfos[i];
			writes.push_back(textureWrite);
		}
	}

	std::array<VkDescriptorBufferInfo, kMaxDescriptorBindings> constantInfos = {};
	for (uint32_t binding = 0; binding < drawCall.constantBuffers.size(); ++binding) {
		const auto& constant = drawCall.constantBuffers[binding];
		const VulkanBuffer* buffer = dynamic_cast<const VulkanBuffer*>(constant.buffer);
		if (buffer == nullptr) continue;
		if(binding != m_shaderLayout.lightingConstantBinding
			&& binding != m_shaderLayout.shadowMatrixBinding)
		{
			SDL_Log("Invalid Vulkan uniform buffer binding %u for the active graphics layout.", binding);
			return false;
		}
		const uint64_t constantRange = constant.size > 0u
			? static_cast<uint64_t>(constant.size)
			: static_cast<uint64_t>(buffer->GetSize())
				- std::min<uint64_t>(constant.offset, buffer->GetSize());
		if(!ValidateUniformBufferBinding(
			m_capabilities,
			constant.offset,
			constantRange,
			buffer->GetSize()))
		{
			SDL_Log("Invalid Vulkan uniform buffer binding %u: offset=%u range=%llu buffer=%u.",
				binding,
				constant.offset,
				static_cast<unsigned long long>(constantRange),
				buffer->GetSize());
			return false;
		}

		constantInfos[binding].buffer = buffer->GetHandle();
		constantInfos[binding].offset = constant.offset;
		constantInfos[binding].range = constantRange;

		VkWriteDescriptorSet constantWrite{};
		constantWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		constantWrite.dstBinding = binding;
		constantWrite.descriptorCount = 1;
		constantWrite.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
		constantWrite.pBufferInfo = &constantInfos[binding];
		writes.push_back(constantWrite);
	}

	std::array<VkDescriptorBufferInfo, kMaxDescriptorBindings> storageInfos = {};
	for (uint32_t binding = 0; binding < drawCall.storageBuffers.size(); ++binding) {
		if(!IsProfileStorageBinding(pipelineState->GetResourceProfile(), binding, m_shaderLayout)) continue;
		const auto& storage = drawCall.storageBuffers[binding];
		const VulkanBuffer* buffer = dynamic_cast<const VulkanBuffer*>(storage.buffer);
		if (buffer == nullptr) continue;
		const uint64_t storageRange = storage.size > 0u
			? static_cast<uint64_t>(storage.size)
			: static_cast<uint64_t>(buffer->GetSize()) - std::min<uint64_t>(storage.offset, buffer->GetSize());
		if(!ValidateStorageBufferBinding(
			m_capabilities,
			storage.offset,
			storageRange,
			buffer->GetSize()))
		{
			SDL_Log("Invalid Vulkan storage buffer binding %u: offset=%u range=%llu buffer=%u.",
				binding,
				storage.offset,
				static_cast<unsigned long long>(storageRange),
				buffer->GetSize());
			return false;
		}

		storageInfos[binding].buffer = buffer->GetHandle();
		storageInfos[binding].offset = storage.offset;
		storageInfos[binding].range = storageRange;

		VkWriteDescriptorSet storageWrite{};
		storageWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		storageWrite.dstBinding = binding;
		storageWrite.descriptorCount = 1;
		storageWrite.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		storageWrite.pBufferInfo = &storageInfos[binding];
		writes.push_back(storageWrite);
	}

	const VulkanTexture* shadowTexture = m_shaderLayout.shadowSamplerBinding < drawCall.textures.size()
		? dynamic_cast<const VulkanTexture*>(drawCall.textures[m_shaderLayout.shadowSamplerBinding])
		: nullptr;
	const bool usingFallbackShadowTexture = shadowTexture == nullptr;
	if (usingFallbackShadowTexture && hasFallbackTexture) {
		shadowTexture = fallbackTexture;
	}
	VkDescriptorImageInfo shadowInfo{};
	if (shadowTexture != nullptr && shadowTexture->GetImageView() != VK_NULL_HANDLE && shadowTexture->GetSampler() != VK_NULL_HANDLE) {
		shadowInfo.sampler = shadowTexture->GetSampler();
		shadowInfo.imageView = shadowTexture->GetImageView();
		shadowInfo.imageLayout = shadowTexture->GetImageLayout();
		VkWriteDescriptorSet shadowWrite{};
		shadowWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		shadowWrite.dstBinding = m_shaderLayout.shadowSamplerBinding;
		shadowWrite.descriptorCount = 1;
		shadowWrite.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		shadowWrite.pImageInfo = &shadowInfo;
		writes.push_back(shadowWrite);
	}

	std::array<VkDescriptorBufferInfo, 2> geometryInfos = {};
	const uint64_t vertexRange = static_cast<uint64_t>(vertexBuffer->GetSize())
		- std::min<uint64_t>(drawCall.geometry.vertexOffset, vertexBuffer->GetSize());
	const VulkanBuffer* resolvedIndexBuffer = indexBuffer != nullptr ? indexBuffer : vertexBuffer;
	const uint32_t resolvedIndexOffset = indexBuffer != nullptr ? drawCall.geometry.indexOffset : 0u;
	const uint64_t indexRange = static_cast<uint64_t>(resolvedIndexBuffer->GetSize())
		- std::min<uint64_t>(resolvedIndexOffset, resolvedIndexBuffer->GetSize());
	if(!ValidateStorageBufferBinding(
		m_capabilities,
		drawCall.geometry.vertexOffset,
		vertexRange,
		vertexBuffer->GetSize())
		|| !ValidateStorageBufferBinding(
			m_capabilities,
			resolvedIndexOffset,
			indexRange,
			resolvedIndexBuffer->GetSize()))
	{
		SDL_Log("Invalid Vulkan geometry storage buffer range.");
		return false;
	}
	geometryInfos[0].buffer = vertexBuffer->GetHandle();
	geometryInfos[0].offset = drawCall.geometry.vertexOffset;
	geometryInfos[0].range = vertexRange;
	geometryInfos[1].buffer = resolvedIndexBuffer->GetHandle();
	geometryInfos[1].offset = resolvedIndexOffset;
	geometryInfos[1].range = indexRange;

	VkWriteDescriptorSet vertexWrite{};
	vertexWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	vertexWrite.dstBinding = m_shaderLayout.vertexStorageBinding;
	vertexWrite.descriptorCount = 1;
	vertexWrite.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	vertexWrite.pBufferInfo = &geometryInfos[0];
	writes.push_back(vertexWrite);

	VkWriteDescriptorSet indexWrite{};
	indexWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	indexWrite.dstBinding = m_shaderLayout.indexStorageBinding;
	indexWrite.descriptorCount = 1;
	indexWrite.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	indexWrite.pBufferInfo = &geometryInfos[1];
	writes.push_back(indexWrite);

	if(m_currentFrameIndex >= m_drawConstantFrames.size()) return false;
	const DrawConstantFrame& drawConstantFrame = m_drawConstantFrames[m_currentFrameIndex];
	if(drawConstantFrame.buffer == VK_NULL_HANDLE || drawConstantSlot >= m_drawConstantCapacity) return false;
	VkDescriptorBufferInfo drawConstantInfo{};
	drawConstantInfo.buffer = drawConstantFrame.buffer;
	drawConstantInfo.offset = m_drawConstantStride * drawConstantSlot;
	drawConstantInfo.range = m_shaderLayout.pushConstantRangeSize;
	VkWriteDescriptorSet drawConstantWrite{};
	drawConstantWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	drawConstantWrite.dstBinding = m_shaderLayout.drawConstantsBinding;
	drawConstantWrite.descriptorCount = 1u;
	drawConstantWrite.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
	drawConstantWrite.pBufferInfo = &drawConstantInfo;
	writes.push_back(drawConstantWrite);

	vkCmdPushDescriptorSet(
		commandBuffer,
		VK_PIPELINE_BIND_POINT_GRAPHICS,
		pipelineLayout,
		0u,
		static_cast<uint32_t>(writes.size()),
		writes.data());
	return true;
}

bool VulkanDevice::Impl::RecreateSwapchain() {
	if(m_context.device == VK_NULL_HANDLE || m_deviceLost) return false;
	const VkResult idleResult = vkDeviceWaitIdle(m_context.device);
	if(idleResult != VK_SUCCESS)
	{
		if(idleResult == VK_ERROR_DEVICE_LOST) m_deviceLost = true;
		SDL_Log("Failed to idle Vulkan device before swapchain recreation: %s (%d).",
			VkResultToString(idleResult),
			static_cast<int>(idleResult));
		return false;
	}

	DestroySwapchainResources();
	const VkResult swapchainResult = m_swapchain.Initialize(
		m_context,
		m_windowHandle,
		dy::RHI::IsSrgbFormat(m_owner.GetDesc().swapchainFormat));
	if(swapchainResult != VK_SUCCESS)
	{
		if(swapchainResult == VK_ERROR_DEVICE_LOST) m_deviceLost = true;
		SDL_Log("Failed to recreate Vulkan swapchain: %s (%d).",
			VkResultToString(swapchainResult),
			static_cast<int>(swapchainResult));
		DestroySwapchainResources();
		m_frameReady = false;
		m_frameSubmitted = false;
		m_imageAcquired = false;
		return false;
	}

	bool resourcesCreated = false;
	try
	{
		UpdateBackBufferMetadata();
		m_depthFormat = FindDepthFormat();
		resourcesCreated = CreateDepthResources();
	}
	catch(const std::exception& error)
	{
		SDL_Log("Vulkan swapchain resource recreation failed: %s", error.what());
	}
	if(!resourcesCreated)
	{
		SDL_Log("Failed to recreate Vulkan swapchain-dependent resources.");
		DestroySwapchainResources();
		m_frameReady = false;
		m_frameSubmitted = false;
		m_imageAcquired = false;
		return false;
	}

	m_imagesInFlight.assign(m_swapchain.GetImageCount(), VK_NULL_HANDLE);
	m_imageAcquired = false;
	return true;
}

void VulkanDevice::Impl::DestroySwapchainResources() {
	delete m_depthTexture;
	m_depthTexture = nullptr;
	m_swapchain.Cleanup(m_context.device);
}

void VulkanDevice::Impl::DestroyDeviceResources() {
	if (m_context.device != VK_NULL_HANDLE) {
		vkDeviceWaitIdle(m_context.device);
	}
	for (dy::RHI::IPipelineState* pipelineState : m_ownedPipelineStates) delete pipelineState;
	m_ownedPipelineStates.clear();

	for (dy::RHI::ITexture* texture : m_ownedTextures) delete texture;
	m_ownedTextures.clear();

	delete m_backBuffer;
	m_backBuffer = nullptr;

	delete m_fallbackTexture;
	m_fallbackTexture = nullptr;

	delete m_commandList;
	m_commandList = nullptr;

	if (m_context.device != VK_NULL_HANDLE) {
		DestroyDrawConstantBuffers();
		for (dy::RHI::IBuffer* buffer : m_ownedBuffers) delete buffer;
		m_ownedBuffers.clear();
		DestroyAllRetiredBuffers();
		for(ProfileResources& resources : m_profileResources)
		{
			if(resources.descriptorSetLayout != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(m_context.device, resources.descriptorSetLayout, nullptr);
			resources = {};
		}
		if (m_bindlessDescriptorPool != VK_NULL_HANDLE) vkDestroyDescriptorPool(m_context.device, m_bindlessDescriptorPool, nullptr);
		if (m_bindlessDescriptorSetLayout != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(m_context.device, m_bindlessDescriptorSetLayout, nullptr);
		for (auto s : m_imageAvailableSemaphores) vkDestroySemaphore(m_context.device, s, nullptr);
		for (auto s : m_renderFinishedSemaphores) vkDestroySemaphore(m_context.device, s, nullptr);
		for (auto f : m_inFlightFences) vkDestroyFence(m_context.device, f, nullptr);
		for (VkQueryPool queryPool : m_gpuTimestampQueryPools) {
			if (queryPool != VK_NULL_HANDLE) vkDestroyQueryPool(m_context.device, queryPool, nullptr);
		}
		m_gpuTimestampQueryPools.clear();
		if (m_commandPool != VK_NULL_HANDLE) vkDestroyCommandPool(m_context.device, m_commandPool, nullptr);
		DestroySwapchainResources();
		vkDestroyDevice(m_context.device, nullptr);
	}
	if (m_context.surface != VK_NULL_HANDLE) vkDestroySurfaceKHR(m_context.instance, m_context.surface, nullptr);
	DestroyDebugMessenger();
	if (m_context.instance != VK_NULL_HANDLE) vkDestroyInstance(m_context.instance, nullptr);
}

void VulkanDevice::Impl::UpdateBackBufferMetadata() {
	const VkExtent2D extent = m_swapchain.GetExtent();
	dy::RHI::TextureDesc desc{};
	desc.width = extent.width;
	desc.height = extent.height;
	desc.depthOrArraySize = 1;
	desc.mipLevels = 1;
	// 실제 스왑체인 포맷을 정직하게 보고한다(요청한 DeviceDesc 포맷과 동일하게 선택됨).
	desc.format = m_owner.GetDesc().swapchainFormat;
	desc.usage = dy::RHI::TextureUsage::RenderTarget;

	if (m_backBuffer == nullptr) {
		m_backBuffer = new VulkanTexture(desc);
		return;
	}

	static_cast<VulkanTexture*>(m_backBuffer)->UpdateMetadata(desc);
}

VkFormat VulkanDevice::Impl::FindDepthFormat() const {
	const std::array<VkFormat, 3> candidates = {
		VK_FORMAT_D32_SFLOAT,
		VK_FORMAT_D32_SFLOAT_S8_UINT,
		VK_FORMAT_D24_UNORM_S8_UINT
	};

	for (VkFormat format : candidates) {
		if (IsDepthFormatSupported(format)) {
			return format;
		}
	}

	throw std::runtime_error("failed to find supported depth format!");
}

bool VulkanDevice::Impl::IsDepthFormatSupported(VkFormat format) const {
	VkFormatProperties properties{};
	vkGetPhysicalDeviceFormatProperties(m_context.physicalDevice, format, &properties);
	return (properties.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0;
}

}
