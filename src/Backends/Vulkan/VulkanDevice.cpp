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
#include "Graphics/RendererShaderLayout.h"
#include <GLFW/glfw3.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <cstdio>
#include <fstream>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#if defined(_WIN32)
#include <windows.h>
#endif

#define SDL_Log(fmt, ...) { printf(fmt, ##__VA_ARGS__); printf("\n"); }

namespace dy::Backends
{
namespace {
	namespace Layout = dy::Graphics::RendererShaderLayout;
	const char* VkResultToString(VkResult result);
	constexpr const char* kValidationLayerName = "VK_LAYER_KHRONOS_validation";
	constexpr const uint32_t kFallbackTextureWidth = 2;
	constexpr const uint32_t kFallbackTextureHeight = 2;
	constexpr uint32_t kMaxDescriptorPagesPerFrame = 4u;
	constexpr size_t kGraphicsResourceProfileCount = 3u;

	size_t ResourceProfileIndex(dy::RHI::GraphicsResourceProfile profile)
	{
		return static_cast<size_t>(profile);
	}

	bool IsProfileStorageBinding(dy::RHI::GraphicsResourceProfile profile, uint32_t binding)
	{
		if(profile == dy::RHI::GraphicsResourceProfile::PerDrawSkin)
		{
			return binding == Layout::kSkinInfluenceStorageBinding
				|| binding == Layout::kSkinPaletteStorageBinding;
		}
		return binding == Layout::kBindlessTransformStorageBinding;
	}

	enum class VulkanBufferKind // 불칸 사용 버퍼 종류
	{
		Vertex,
		Index,
		Constant,
		Storage,
		Indirect
	};

	struct VulkanBufferKindInfo
	{
		VulkanBufferKind kind;
		dy::RHI::BufferUsage usage;
		VkBufferUsageFlags vkUsage;
		const char* name;
	};

	constexpr std::array<VulkanBufferKindInfo, 5> kVulkanBufferKinds = {
		VulkanBufferKindInfo{ VulkanBufferKind::Vertex, dy::RHI::BufferUsage::Vertex, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, "Vertex" },
		VulkanBufferKindInfo{ VulkanBufferKind::Index, dy::RHI::BufferUsage::Index, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, "Index" },
		VulkanBufferKindInfo{ VulkanBufferKind::Constant, dy::RHI::BufferUsage::Constant, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, "Constant" },
		VulkanBufferKindInfo{ VulkanBufferKind::Storage, dy::RHI::BufferUsage::Storage, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, "Storage" },
		VulkanBufferKindInfo{ VulkanBufferKind::Indirect, dy::RHI::BufferUsage::Indirect, VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, "Indirect" }
	};

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
			: dy::RHI::ITexture(desc), m_width(desc.width), m_height(desc.height), m_format(desc.format)
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
			VkImageUsageFlags extraUsage = 0)
		{
			Cleanup();
			m_device = context.device;
			m_width = desc.width;
			m_height = desc.height;
			m_format = desc.format;
			SetDesc(desc);
			m_vkFormat = formatOverride != VK_FORMAT_UNDEFINED ? formatOverride : ToVkFormat(desc.format);
			m_aspectMask = GetImageAspectMask(desc.format, desc.usage);
			m_imageLayout = VK_IMAGE_LAYOUT_UNDEFINED;

			if (m_width == 0 || m_height == 0 || m_vkFormat == VK_FORMAT_UNDEFINED) return false;

			try {
				VulkanResources::CreateImage(
					context,
					m_width,
					m_height,
					m_vkFormat,
					VK_IMAGE_TILING_OPTIMAL,
					ToVkImageUsage(desc.usage) | extraUsage,
					VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
					m_image,
					m_memory);

				m_imageView = VulkanResources::CreateImageView(m_device, m_image, m_vkFormat, m_aspectMask);
			} catch (const std::exception& e) {
				SDL_Log("Vulkan texture creation failed: %s", e.what());
				Cleanup();
				return false;
			}

			return true;
		}

		void UpdateMetadata(uint32_t width, uint32_t height, dy::RHI::Format format)
		{
			m_width = width;
			m_height = height;
			m_format = format;
			dy::RHI::TextureDesc desc = GetDesc();
			desc.width = width;
			desc.height = height;
			desc.format = format;
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

				VulkanResources::TransitionImageLayout(context, commandPool, m_image, m_vkFormat, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
				VulkanResources::CopyBufferToImage(context, commandPool, staging, m_image, width, height);
				VulkanResources::TransitionImageLayout(context, commandPool, m_image, m_vkFormat, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
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
		VkImageAspectFlags m_aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		VkImageLayout m_imageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		uint32_t m_width = 0;
		uint32_t m_height = 0;
		dy::RHI::Format m_format = dy::RHI::Format::Unknown;
	};

	class VulkanPipelineState final : public dy::RHI::IPipelineState
	{
	public:
		VulkanPipelineState(
			const VulkanContext& context,
			VkRenderPass renderPass,
			VkExtent2D extent,
			VkDescriptorSetLayout descriptorSetLayout,
			VkDescriptorSetLayout bindlessDescriptorSetLayout,
			const dy::RHI::GraphicsPipelineDesc& desc)
			: m_device(context.device),
			m_shadowPassEnabled(desc.enableShadowPass),
			m_bindlessTexturesEnabled(desc.enableBindlessTextures)
		{
			CopyPipelineDesc(desc);
			m_pipelineCache.reserve(4);
			if (GetPipelineForRenderPass(context, renderPass, extent, descriptorSetLayout, bindlessDescriptorSetLayout) == nullptr) {
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

		const VulkanPipeline* GetPipelineForRenderPass(
			const VulkanContext& context,
			VkRenderPass renderPass,
			VkExtent2D extent,
			VkDescriptorSetLayout descriptorSetLayout,
			VkDescriptorSetLayout bindlessDescriptorSetLayout) const
		{
			for (const PipelineCacheEntry& entry : m_pipelineCache) {
				if (entry.renderPass == renderPass) return &entry.pipeline;
			}

			try {
				PipelineCacheEntry entry = {};
				entry.renderPass = renderPass;
				entry.pipeline.Initialize(context, renderPass, extent, descriptorSetLayout, m_desc, bindlessDescriptorSetLayout);
				m_pipelineCache.push_back(std::move(entry));
				return &m_pipelineCache.back().pipeline;
			} catch (const std::exception& e) {
				SDL_Log("Vulkan pipeline variant creation failed: %s", e.what());
				return nullptr;
			}
		}

		bool IsShadowPassEnabled() const { return m_shadowPassEnabled; }
		bool UsesBindlessTextures() const { return m_bindlessTexturesEnabled; }
		dy::RHI::GraphicsResourceProfile GetResourceProfile() const { return m_desc.resourceProfile; }

	private:
		struct PipelineCacheEntry
		{
			VkRenderPass renderPass = VK_NULL_HANDLE;
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
			CopyShaderBytes(desc.shadowVertexShader, desc.shadowVertexShaderSize, m_shadowVertexShader);

			m_desc.vertexShader = m_vertexShader.empty() ? nullptr : m_vertexShader.data();
			m_desc.vertexShaderSize = m_vertexShader.size();
			m_desc.pixelShader = m_pixelShader.empty() ? nullptr : m_pixelShader.data();
			m_desc.pixelShaderSize = m_pixelShader.size();
			m_desc.shadowVertexShader = m_shadowVertexShader.empty() ? nullptr : m_shadowVertexShader.data();
			m_desc.shadowVertexShaderSize = m_shadowVertexShader.size();
		}

		VkDevice m_device = VK_NULL_HANDLE;
		dy::RHI::GraphicsPipelineDesc m_desc = {};
		std::vector<uint8_t> m_vertexShader;
		std::vector<uint8_t> m_pixelShader;
		std::vector<uint8_t> m_shadowVertexShader;
		mutable std::vector<PipelineCacheEntry> m_pipelineCache;
		bool m_shadowPassEnabled = false;
		bool m_bindlessTexturesEnabled = false;
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

				VkShaderModule shaderModule = CreateShaderModule(desc.computeShader, desc.computeShaderSize);
				VkPipelineShaderStageCreateInfo stage{};
				stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
				stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
				stage.module = shaderModule;
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
				vkDestroyShaderModule(m_device, shaderModule, nullptr);
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
		VkShaderModule CreateShaderModule(const void* shaderCode, size_t shaderSize)
		{
			VkShaderModuleCreateInfo createInfo{};
			createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
			createInfo.codeSize = shaderSize;
			createInfo.pCode = reinterpret_cast<const uint32_t*>(shaderCode);
			VkShaderModule shaderModule = VK_NULL_HANDLE;
			if(vkCreateShaderModule(m_device, &createInfo, nullptr, &shaderModule) != VK_SUCCESS)
				throw std::runtime_error("failed to create compute shader module");
			return shaderModule;
		}

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

	VkShaderModule CreateShaderModule(VkDevice device, const void* shaderCode, size_t shaderSize)
	{
		VkShaderModuleCreateInfo createInfo{};
		createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
		createInfo.codeSize = shaderSize;
		createInfo.pCode = reinterpret_cast<const uint32_t*>(shaderCode);

		VkShaderModule shaderModule = VK_NULL_HANDLE;
		if (vkCreateShaderModule(device, &createInfo, nullptr, &shaderModule) != VK_SUCCESS) {
			throw std::runtime_error("failed to create shader module");
		}
		return shaderModule;
	}

	VkBufferUsageFlags ToVkBufferUsage(dy::RHI::BufferUsage usage)
	{
		VkBufferUsageFlags flags = 0;
		for (const VulkanBufferKindInfo& kind : kVulkanBufferKinds) {
			if ((usage & kind.usage) != dy::RHI::BufferUsage::None) flags |= kind.vkUsage;
		}
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
	[[nodiscard]] bool SupportsSkinningStorageBindings() const { return m_capabilities.supportsSkinningStorageBindings; }
	[[nodiscard]] bool SupportsComputeSkinning() const { return m_capabilities.supportsComputeSkinning; }
	[[nodiscard]] uint32_t GetValidationErrorCount() const { return m_validationErrorCount.load(); }
	[[nodiscard]] uint32_t GetValidationVuidCount() const { return m_validationVuidCount.load(); }
	[[nodiscard]] bool IsValidationCaptureEnabled() const { return m_validationEnabled && m_debugMessenger != VK_NULL_HANDLE; }
	[[nodiscard]] bool IsDeviceLost() const { return m_deviceLost; }

private:
	struct ProfileResources
	{
		VkDescriptorSetLayout descriptorSetLayout = VK_NULL_HANDLE;
		VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
		std::vector<std::vector<VkDescriptorSet>> frameDescriptorSets;
	};

	const dy::RHI::DeviceDesc& GetDesc() const { return m_owner.GetDesc(); }

	bool CreateInstance();
	bool CreateDebugMessenger();
	void DestroyDebugMessenger();
	static VKAPI_ATTR VkBool32 VKAPI_CALL DebugUtilsCallback(
		VkDebugUtilsMessageSeverityFlagBitsEXT severity,
		VkDebugUtilsMessageTypeFlagsEXT type,
		const VkDebugUtilsMessengerCallbackDataEXT* callbackData,
		void* userData);
	bool CreateSurface();
	bool PickPhysicalDevice();
	bool CreateLogicalDevice();
	bool CreateSyncObjects();
	bool EnsureProfileResources(dy::RHI::GraphicsResourceProfile profile);
	bool CreateDescriptorSetLayout(dy::RHI::GraphicsResourceProfile profile, VkDescriptorSetLayout& outLayout);
	bool CreateBindlessDescriptorSetLayout();
	bool CreateDescriptorPool(dy::RHI::GraphicsResourceProfile profile, VkDescriptorPool& outPool);
	bool CreateBindlessDescriptorPool();
	bool EnsureDescriptorSets(ProfileResources& resources, uint32_t frameIndex, uint32_t requiredSetCount);
	VkDescriptorSet GetProfileDescriptorSet(
		const ProfileResources& resources,
		uint32_t frameIndex,
		uint32_t drawSlot) const;
	ProfileResources* GetProfileResources(dy::RHI::GraphicsResourceProfile profile);
	const ProfileResources* GetProfileResources(dy::RHI::GraphicsResourceProfile profile) const;
	bool CreateBindlessDescriptorSet();
	bool CreateMainRenderPass();
	bool CreateDepthResources();
	bool CreateFramebuffers();
	bool CreateCommandPool();
	bool CreateCommandBuffer();
	bool CreateFallbackTexture();
	bool CreateDrawConstantBuffers();
	bool UploadDrawConstants(const VulkanCommandList& commandList);
	void DestroyDrawConstantBuffers();
	void CollectRetiredBuffers();
	void DestroyAllRetiredBuffers();

	bool CreateShadowMapResources();
	bool CreateShadowRenderPass();
	bool CreateShadowPipeline(const dy::RHI::GraphicsPipelineDesc& desc);
	void DestroyShadowResources();

	bool RecreateSwapchain();
	void DestroySwapchainResources();
	void DestroyDeviceResources();
	bool AcquireFrameImage();
	bool RecreateCurrentFrameFenceSignaled();
	void RecoverAbortedAcquiredFrame(bool recreateFence);

	bool RecordCommandBuffer(const VulkanCommandList& commandList);
	bool RecordComputePass(VkCommandBuffer commandBuffer, const VulkanCommandList& commandList);
	bool RecordShadowPass(VkCommandBuffer commandBuffer, const VulkanCommandList& commandList);
	bool RecordMainPass(VkCommandBuffer commandBuffer, const VulkanCommandList& commandList);
	bool ResolveMainPassTarget(const VulkanCommandList& commandList, VkRenderPass& renderPass, VkFramebuffer& framebuffer, VkExtent2D& extent);
	bool GetOrCreateOffscreenFramebuffer(dy::RHI::ITexture* colorTarget, dy::RHI::ITexture* depthTarget, VkRenderPass& renderPass, VkFramebuffer& framebuffer, VkExtent2D& extent);
	bool CreateOffscreenRenderPass(VkFormat colorFormat, VkFormat depthFormat, VkImageLayout colorFinalLayout, VkRenderPass& renderPass);
	void DestroyRenderTargetCache();
	bool UpdateDrawDescriptorSets(const VulkanCommandList& commandList);
	bool UpdateDrawDescriptorSet(const VulkanCommandList::DrawCall& drawCall, uint32_t drawIndex);
	bool UpdateComputeDescriptorSets(const VulkanCommandList& commandList);
	bool ApplyBindlessDescriptorSet(uint32_t frameIndex);
	void UpdateBackBufferMetadata();
	VkFormat FindDepthFormat() const;
	bool IsDepthFormatSupported(VkFormat format) const;

	VulkanDevice& m_owner;
	VulkanContext m_context;
	VkDebugUtilsMessengerEXT m_debugMessenger = VK_NULL_HANDLE;
	VulkanSwapchain m_swapchain;
	VkRenderPass m_mainRenderPass = VK_NULL_HANDLE;

	void* m_windowHandle = nullptr;

	VkCommandPool m_commandPool = VK_NULL_HANDLE;
	std::vector<VkCommandBuffer> m_commandBuffers;

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

	std::vector<VkSemaphore> m_imageAvailableSemaphores;
	std::vector<VkSemaphore> m_renderFinishedSemaphores;
	std::vector<VkFence> m_inFlightFences;
	std::vector<VkFence> m_imagesInFlight;

	std::vector<VkFramebuffer> m_swapchainFramebuffers;
	std::vector<dy::RHI::IBuffer*> m_ownedBuffers;
	struct RetiredBuffer
	{
		dy::RHI::IBuffer* buffer = nullptr;
		std::vector<uint32_t> pendingFrameIndices;
	};
	std::vector<RetiredBuffer> m_retiredBuffers;

	struct RenderTargetCacheEntry
	{
		dy::RHI::ITexture* colorTarget = nullptr;
		dy::RHI::ITexture* depthTarget = nullptr;
		dy::RHI::ITexture* ownedDepthTarget = nullptr;
		VkRenderPass renderPass = VK_NULL_HANDLE;
		VkFramebuffer framebuffer = VK_NULL_HANDLE;
		uint32_t imageIndex = 0;
		uint32_t width = 0;
		uint32_t height = 0;
		bool ownsRenderPass = true;
	};
	std::vector<RenderTargetCacheEntry> m_renderTargetCache;

	VkRenderPass m_shadowRenderPass = VK_NULL_HANDLE;
	VkFramebuffer m_shadowFramebuffer = VK_NULL_HANDLE;
	std::array<VkPipelineLayout, kGraphicsResourceProfileCount> m_shadowPipelineLayouts = {};
	std::array<VkPipeline, kGraphicsResourceProfileCount> m_shadowPipelines = {};
	VkFormat m_shadowMapFormat = VK_FORMAT_UNDEFINED;
	uint32_t m_shadowMapResolution = dy::RHI::DeviceDesc{}.defaultShadowMapResolution;

	uint32_t m_maxFramesInFlight = dy::RHI::DeviceDesc{}.maxFramesInFlight;
	uint32_t m_maxDrawsPerFrame = dy::RHI::DeviceDesc{}.maxDrawsPerFrame;
	uint32_t m_maxBindlessTextures = dy::RHI::DeviceDesc{}.maxBindlessTextures;
	uint32_t m_defaultShadowMapResolution = dy::RHI::DeviceDesc{}.defaultShadowMapResolution;
	uint32_t m_fallbackTextureWidth = kFallbackTextureWidth;
	uint32_t m_fallbackTextureHeight = kFallbackTextureHeight;
	uint32_t m_descriptorSetCount = 0u;
	uint32_t m_descriptorCapacityPerFrame = 0u;
	uint64_t m_frameAcquireTimeoutNanoseconds = dy::RHI::DeviceDesc{}.frameAcquireTimeoutNanoseconds;
	VulkanCapabilities m_capabilities = {};
	bool m_validationEnabled = false;
	bool m_debugUtilsEnabled = false;
	struct ValidationMessage
	{
		VkDebugUtilsMessageSeverityFlagBitsEXT severity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT;
		VkDebugUtilsMessageTypeFlagsEXT type = 0u;
		std::string message;
	};
	std::atomic<uint32_t> m_validationErrorCount = 0u;
	std::atomic<uint32_t> m_validationVuidCount = 0u;
	std::mutex m_validationMessageMutex;
	std::vector<ValidationMessage> m_validationMessages;
	dy::RHI::ShaderLayoutDesc m_shaderLayout = {};
	uint32_t m_currentFrameIndex = 0;
	uint32_t m_currentImageIndex = 0;
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
	dy::RHI::ITexture* m_shadowMapTexture = nullptr;
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
	return m_impl->CreateBuffer(desc);
}

dy::RHI::ITexture* VulkanDevice::CreateTexture(const dy::RHI::TextureDesc& desc)
{
	return m_impl->CreateTexture(desc);
}

bool VulkanDevice::UpdateTexture(dy::RHI::ITexture* texture, const void* rgba8Pixels, uint32_t rowPitch)
{
	return m_impl->UpdateTexture(texture, rgba8Pixels, rowPitch);
}

dy::RHI::IPipelineState* VulkanDevice::CreateGraphicsPipeline(const dy::RHI::GraphicsPipelineDesc& desc)
{
	return m_impl->CreateGraphicsPipeline(desc);
}

dy::RHI::IPipelineState* VulkanDevice::CreateComputePipeline(const dy::RHI::ComputePipelineDesc& desc)
{
	return m_impl->CreateComputePipeline(desc);
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
	m_impl->DestroyBuffer(buffer);
}

void VulkanDevice::DestroyTexture(dy::RHI::ITexture* texture)
{
	m_impl->DestroyTexture(texture);
}

void VulkanDevice::DestroyPipelineState(dy::RHI::IPipelineState* pipeline)
{
	m_impl->DestroyPipelineState(pipeline);
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
	if (!ValidateVulkanDeviceConfig(desc, m_descriptorSetCount)) {
		SDL_Log("Invalid Vulkan device configuration: frames and draws must be non-zero and their product must fit uint32.");
		return -1;
	}
	if(!TryComputeVulkanDescriptorPageCapacity(
		desc,
		kMaxDescriptorPagesPerFrame,
		m_descriptorCapacityPerFrame,
		m_descriptorSetCount))
	{
		SDL_Log("Invalid Vulkan descriptor page capacity: expanded frame/page count overflows uint32.");
		return -1;
	}
	m_maxFramesInFlight = desc.maxFramesInFlight;
	m_maxDrawsPerFrame = desc.maxDrawsPerFrame;
	m_maxBindlessTextures = desc.maxBindlessTextures;
	m_defaultShadowMapResolution = desc.defaultShadowMapResolution;
	m_shadowMapResolution = m_defaultShadowMapResolution;
	m_fallbackTextureWidth = kFallbackTextureWidth;
	m_fallbackTextureHeight = kFallbackTextureHeight;
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
			dy::RHI::IsSrgbFormat(GetDesc().swapchainFormat));
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
		if (!CreateMainRenderPass()) return -1;
		if (!CreateDepthResources()) return -1;
		if (!CreateFramebuffers()) return -1;
		if (!CreateCommandBuffer()) return -1;
		if (!CreateSyncObjects()) return -1;
	} catch (const std::exception& e) {
		SDL_Log("Vulkan Initialization failed: %s", e.what());
		return -1;
	}

	return 0;
}

dy::RHI::ITexture* VulkanDevice::Impl::CreateTexture(const dy::RHI::TextureDesc& desc) {
	std::unique_ptr<VulkanTexture> texture(new VulkanTexture(desc));
	if (!texture->Initialize(m_context, desc)) {
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
		VkImageMemoryBarrier toTransfer{};
		toTransfer.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		const bool shaderReadLayout = originalLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		toTransfer.srcAccessMask = shaderReadLayout
			? VK_ACCESS_SHADER_READ_BIT
			: VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
		toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
		toTransfer.oldLayout = originalLayout;
		toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
		toTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		toTransfer.image = vulkanTexture->GetImage();
		toTransfer.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		toTransfer.subresourceRange.baseMipLevel = 0u;
		toTransfer.subresourceRange.levelCount = 1u;
		toTransfer.subresourceRange.baseArrayLayer = 0u;
		toTransfer.subresourceRange.layerCount = 1u;
		vkCmdPipelineBarrier(
			commandBuffer,
			shaderReadLayout
				? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
				: VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
			VK_PIPELINE_STAGE_TRANSFER_BIT,
			0u,
			0u,
			nullptr,
			0u,
			nullptr,
			1u,
			&toTransfer);

		VkBufferImageCopy copyRegion{};
		copyRegion.bufferOffset = 0u;
		copyRegion.bufferRowLength = 0u;
		copyRegion.bufferImageHeight = 0u;
		copyRegion.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		copyRegion.imageSubresource.mipLevel = 0u;
		copyRegion.imageSubresource.baseArrayLayer = 0u;
		copyRegion.imageSubresource.layerCount = 1u;
		copyRegion.imageExtent = {
			vulkanTexture->GetWidth(),
			vulkanTexture->GetHeight(),
			1u };
		vkCmdCopyImageToBuffer(
			commandBuffer,
			vulkanTexture->GetImage(),
			VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			stagingBuffer,
			1u,
			&copyRegion);

		VkImageMemoryBarrier toShaderRead = toTransfer;
		toShaderRead.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
		toShaderRead.dstAccessMask = shaderReadLayout
			? VK_ACCESS_SHADER_READ_BIT
			: VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
		toShaderRead.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
		toShaderRead.newLayout = originalLayout;
		vkCmdPipelineBarrier(
			commandBuffer,
			VK_PIPELINE_STAGE_TRANSFER_BIT,
			shaderReadLayout
				? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
				: VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
			0u,
			0u,
			nullptr,
			0u,
			nullptr,
			1u,
			&toShaderRead);
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
		if(!SupportsVulkanResourceProfile(m_capabilities, desc.resourceProfile))
		{
			SDL_Log("Vulkan graphics resource profile %u is unsupported by the selected device.", static_cast<uint32_t>(desc.resourceProfile));
			return nullptr;
		}
		if(!EnsureProfileResources(desc.resourceProfile)) return nullptr;
		const ProfileResources* profileResources = GetProfileResources(desc.resourceProfile);
		if(profileResources == nullptr || profileResources->descriptorSetLayout == VK_NULL_HANDLE) return nullptr;
		if (desc.enableShadowPass) {
			const uint32_t requestedShadowMapResolution = desc.shadowMapResolution > 0
				? desc.shadowMapResolution
				: m_defaultShadowMapResolution;
			if (m_shadowRenderPass != VK_NULL_HANDLE && requestedShadowMapResolution != m_shadowMapResolution) {
				vkDeviceWaitIdle(m_context.device);
				DestroyShadowResources();
			}
			m_shadowMapResolution = requestedShadowMapResolution;
		}
		if (desc.enableShadowPass && m_shadowRenderPass == VK_NULL_HANDLE) {
			m_shadowMapFormat = m_depthFormat;
			if (!CreateShadowRenderPass()) return nullptr;
			if (!CreateShadowMapResources()) return nullptr;
		}
		const size_t shadowProfileIndex = ResourceProfileIndex(desc.resourceProfile);
		if (desc.enableShadowPass
			&& shadowProfileIndex < m_shadowPipelines.size()
			&& m_shadowPipelines[shadowProfileIndex] == VK_NULL_HANDLE
			&& !CreateShadowPipeline(desc)) {
			return nullptr;
		}

		VulkanPipelineState* pipelineState = new VulkanPipelineState(
			m_context,
			m_mainRenderPass,
			m_swapchain.GetExtent(),
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
		DestroyRenderTargetCache();
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
		m_commandList->Begin();
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
	const bool commandListValid = count > 0u && cmdLists != nullptr && cmdLists[0] != nullptr;
	if(!m_frameReady || !commandListValid)
	{
		m_frameReady = false;
		return;
	}

	VulkanCommandList* vulkanCmd = static_cast<VulkanCommandList*>(cmdLists[0]);
	if(!UploadDrawConstants(*vulkanCmd))
	{
		if(!m_drawCapacityErrorReported) SDL_Log("Failed to upload Vulkan draw constants.");
		m_frameReady = false;
		return;
	}
	const bool descriptorsUpdated = UpdateComputeDescriptorSets(*vulkanCmd)
		&& UpdateDrawDescriptorSets(*vulkanCmd);
	if (!descriptorsUpdated) {
		SDL_Log("Failed to update Vulkan draw descriptor sets.");
		m_frameReady = false;
		return;
	}
	if(!AcquireFrameImage())
	{
		m_frameReady = false;
		return;
	}
	const bool commandRecorded = RecordCommandBuffer(*vulkanCmd);
	const VulkanSubmissionDecision decision = EvaluateVulkanSubmissionPreparation(
		m_frameReady,
		commandListValid,
		descriptorsUpdated,
		commandRecorded);
	if(!decision.submit)
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

	const VkPipelineStageFlags waitStages[] = { VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT };
	VkSubmitInfo submitInfo{};
	submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submitInfo.waitSemaphoreCount = 1;
	submitInfo.pWaitSemaphores = &m_imageAvailableSemaphores[m_currentFrameIndex];
	submitInfo.pWaitDstStageMask = waitStages;
	submitInfo.commandBufferCount = 1;
	submitInfo.pCommandBuffers = &m_commandBuffers[m_currentFrameIndex];
	submitInfo.signalSemaphoreCount = 1;
	submitInfo.pSignalSemaphores = &m_renderFinishedSemaphores[m_currentImageIndex];

	const VkResult submitResult = vkQueueSubmit(
		m_context.graphicsQueue,
		1,
		&submitInfo,
		m_inFlightFences[m_currentFrameIndex]);
	if (submitResult != VK_SUCCESS) {
		SDL_Log("Failed to submit Vulkan draw command buffer: %s (%d)", VkResultToString(submitResult), static_cast<int>(submitResult));
		const VulkanQueueFailureAction failureAction = EvaluateVulkanQueueFailure(submitResult);
		if(failureAction == VulkanQueueFailureAction::MarkDeviceLost)
		{
			m_deviceLost = true;
			m_frameReady = false;
			m_imageAcquired = false;
		}
		else
		{
			RecoverAbortedAcquiredFrame(true);
		}
		return;
	}

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
	m_debugUtilsEnabled = m_validationEnabled && std::any_of(
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
	appInfo.apiVersion = VK_API_VERSION_1_0;

	VkInstanceCreateInfo createInfo{};
	createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
	createInfo.pApplicationInfo = &appInfo;
	createInfo.enabledLayerCount = static_cast<uint32_t>(enabledLayers.size());
	createInfo.ppEnabledLayerNames = enabledLayers.empty() ? nullptr : enabledLayers.data();
	createInfo.enabledExtensionCount = static_cast<uint32_t>(enabledExtensions.size());
	createInfo.ppEnabledExtensionNames = enabledExtensions.data();
	VkDebugUtilsMessengerCreateInfoEXT debugCreateInfo{};
	if(m_debugUtilsEnabled)
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

	return !m_debugUtilsEnabled || CreateDebugMessenger();
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
	uint32_t deviceCount = 0;
	if(vkEnumeratePhysicalDevices(m_context.instance, &deviceCount, nullptr) != VK_SUCCESS || deviceCount == 0u) return false;
	std::vector<VkPhysicalDevice> devices(deviceCount);
	if(vkEnumeratePhysicalDevices(m_context.instance, &deviceCount, devices.data()) != VK_SUCCESS) return false;

	int32_t bestScore = std::numeric_limits<int32_t>::min();
	VkPhysicalDevice bestDevice = VK_NULL_HANDLE;
	VulkanContext::QueueFamilyIndices bestIndices;
	VulkanCapabilities bestCapabilities = {};
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
		if(!swapchainSupport.querySucceeded || swapchainSupport.formats.empty() || swapchainSupport.presentModes.empty()) continue;

		VkPhysicalDeviceProperties properties{};
		VkPhysicalDeviceFeatures features{};
		vkGetPhysicalDeviceProperties(device, &properties);
		vkGetPhysicalDeviceFeatures(device, &features);
		VulkanDeviceLimits limits;
		limits.maxPushConstantsSize = properties.limits.maxPushConstantsSize;
		limits.maxPerStageDescriptorStorageBuffers = properties.limits.maxPerStageDescriptorStorageBuffers;
		limits.maxDescriptorSetStorageBuffers = properties.limits.maxDescriptorSetStorageBuffers;
		limits.maxStorageBufferRange = properties.limits.maxStorageBufferRange;
		limits.maxUniformBufferRange = properties.limits.maxUniformBufferRange;
		limits.minUniformBufferOffsetAlignment = std::max<VkDeviceSize>(properties.limits.minUniformBufferOffsetAlignment, 1u);
		limits.minStorageBufferOffsetAlignment = std::max<VkDeviceSize>(properties.limits.minStorageBufferOffsetAlignment, 1u);
		const VulkanCapabilities capabilities = BuildVulkanCapabilities(
			limits,
			features.shaderSampledImageArrayDynamicIndexing == VK_TRUE,
			m_validationEnabled,
			families[indices.graphicsFamily].queueFlags);

		int32_t score = static_cast<int32_t>(properties.limits.maxImageDimension2D);
		switch(properties.deviceType)
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
		}
	}
	if(bestDevice == VK_NULL_HANDLE) return false;
	m_context.physicalDevice = bestDevice;
	m_context.queueIndices = bestIndices;
	m_capabilities = bestCapabilities;
	return true;
}

bool VulkanDevice::Impl::CreateDebugMessenger()
{
	const auto createMessenger = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
		vkGetInstanceProcAddr(m_context.instance, "vkCreateDebugUtilsMessengerEXT"));
	if(createMessenger == nullptr) return false;
	VkDebugUtilsMessengerCreateInfoEXT info{};
	info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
	info.messageSeverity =
		VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT
		| VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT
		| VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT
		| VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
	info.messageType =
		VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT
		| VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT
		| VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
	info.pfnUserCallback = &VulkanDevice::Impl::DebugUtilsCallback;
	info.pUserData = this;
	return createMessenger(m_context.instance, &info, nullptr, &m_debugMessenger) == VK_SUCCESS;
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
	VkDebugUtilsMessageTypeFlagsEXT type,
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
		std::lock_guard<std::mutex> lock(impl->m_validationMessageMutex);
		impl->m_validationMessages.push_back(ValidationMessage{ severity, type, message });
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
	VkPhysicalDeviceFeatures supportedFeatures{};
	vkGetPhysicalDeviceFeatures(m_context.physicalDevice, &supportedFeatures);
	VkPhysicalDeviceFeatures deviceFeatures = {};
	deviceFeatures.shaderSampledImageArrayDynamicIndexing = supportedFeatures.shaderSampledImageArrayDynamicIndexing;
	VkDeviceCreateInfo createInfo{};
	createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
	createInfo.queueCreateInfoCount = static_cast<uint32_t>(queueCreateInfos.size());
	createInfo.pQueueCreateInfos = queueCreateInfos.data();
	createInfo.enabledExtensionCount = 1;
	createInfo.ppEnabledExtensionNames = deviceExtensions;
	createInfo.pEnabledFeatures = &deviceFeatures;

	if (vkCreateDevice(m_context.physicalDevice, &createInfo, nullptr, &m_context.device) != VK_SUCCESS) return false;

	vkGetDeviceQueue(m_context.device, m_context.queueIndices.graphicsFamily, 0, &m_context.graphicsQueue);
	vkGetDeviceQueue(m_context.device, m_context.queueIndices.presentFamily, 0, &m_context.presentQueue);
	return true;
}

bool VulkanDevice::Impl::RecordCommandBuffer(const VulkanCommandList& commandList) {
	VkCommandBuffer commandBuffer = m_commandBuffers[m_currentFrameIndex];
	if(vkResetCommandBuffer(commandBuffer, 0) != VK_SUCCESS) return false;
	VkCommandBufferBeginInfo beginInfo{};
	beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	if(vkBeginCommandBuffer(commandBuffer, &beginInfo) != VK_SUCCESS) return false;

	// Pass 0: Compute Skinning
	if(!RecordComputePass(commandBuffer, commandList)) return false;

	// Pass 1: Shadow Pass 
	if (m_shadowRenderPass != VK_NULL_HANDLE) {
		if(!RecordShadowPass(commandBuffer, commandList)) return false;
	}

	// Pass 2: Main Pass
	if(!RecordMainPass(commandBuffer, commandList)) return false;
	const bool rendersToSwapchain = commandList.m_renderTargetCount == 0u
		|| commandList.m_renderTargets[0] == nullptr
		|| commandList.m_renderTargets[0] == m_backBuffer;
	if(!rendersToSwapchain)
	{
		std::array<VkClearValue, 2> clearValues = {};
		clearValues[0].color = { { 0.0f, 0.0f, 0.0f, 1.0f } };
		clearValues[1].depthStencil = { 1.0f, 0u };
		VkRenderPassBeginInfo presentTransition{};
		presentTransition.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
		presentTransition.renderPass = m_mainRenderPass;
		presentTransition.framebuffer = m_swapchainFramebuffers[m_currentImageIndex];
		presentTransition.renderArea.extent = m_swapchain.GetExtent();
		presentTransition.clearValueCount = static_cast<uint32_t>(clearValues.size());
		presentTransition.pClearValues = clearValues.data();
		vkCmdBeginRenderPass(commandBuffer, &presentTransition, VK_SUBPASS_CONTENTS_INLINE);
		vkCmdEndRenderPass(commandBuffer);
	}

	return vkEndCommandBuffer(commandBuffer) == VK_SUCCESS;
}

bool VulkanDevice::Impl::RecordComputePass(
	VkCommandBuffer commandBuffer,
	const VulkanCommandList& commandList)
{
	VkPipeline currentPipeline = VK_NULL_HANDLE;
	for(uint32_t dispatchIndex = 0u; dispatchIndex < commandList.m_computeDispatches.size(); ++dispatchIndex)
	{
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
		if(currentPipeline != pipelineState->GetPipeline())
		{
			vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineState->GetPipeline());
			currentPipeline = pipelineState->GetPipeline();
		}
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
	}

	std::vector<VkBufferMemoryBarrier> barriers;
	barriers.reserve(commandList.m_bufferBarriers.size());
	for(const VulkanCommandList::BufferBarrier& captured : commandList.m_bufferBarriers)
	{
		if(captured.sourceAccess != dy::RHI::BufferAccess::ComputeShaderWrite
			|| captured.destinationAccess != dy::RHI::BufferAccess::VertexShaderRead) return false;
		const VulkanBuffer* buffer = dynamic_cast<const VulkanBuffer*>(captured.buffer);
		if(buffer == nullptr || captured.offset > buffer->GetSize()) return false;
		const uint64_t range = captured.size > 0u
			? captured.size
			: static_cast<uint64_t>(buffer->GetSize()) - captured.offset;
		if(range == 0u || range > static_cast<uint64_t>(buffer->GetSize()) - captured.offset) return false;
		VkBufferMemoryBarrier barrier{};
		barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
		barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
		barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
		barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.buffer = buffer->GetHandle();
		barrier.offset = captured.offset;
		barrier.size = range;
		barriers.push_back(barrier);
	}
	if(!barriers.empty())
	{
		vkCmdPipelineBarrier(
			commandBuffer,
			VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
			VK_PIPELINE_STAGE_VERTEX_SHADER_BIT,
			0u,
			0u,
			nullptr,
			static_cast<uint32_t>(barriers.size()),
			barriers.data(),
			0u,
			nullptr);
	}
	return true;
}

bool VulkanDevice::Impl::RecordShadowPass(VkCommandBuffer commandBuffer, const VulkanCommandList& commandList) {
	VkClearValue newShadowClear = {};
	newShadowClear.depthStencil = { 1.0f, 0 };

	VkRenderPassBeginInfo newShadowPassInfo{};
	newShadowPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
	newShadowPassInfo.renderPass = m_shadowRenderPass;
	newShadowPassInfo.framebuffer = m_shadowFramebuffer;
	newShadowPassInfo.renderArea.offset = {0, 0};
	newShadowPassInfo.renderArea.extent = { m_shadowMapResolution, m_shadowMapResolution };
	newShadowPassInfo.clearValueCount = 1;
	newShadowPassInfo.pClearValues = &newShadowClear;

	vkCmdBeginRenderPass(commandBuffer, &newShadowPassInfo, VK_SUBPASS_CONTENTS_INLINE);

	VkViewport newShadowViewport{};
	newShadowViewport.x = 0.0f;
	newShadowViewport.y = 0.0f;
	newShadowViewport.width = static_cast<float>(m_shadowMapResolution);
	newShadowViewport.height = static_cast<float>(m_shadowMapResolution);
	newShadowViewport.minDepth = 0.0f;
	newShadowViewport.maxDepth = 1.0f;
	VkRect2D newShadowScissor{};
	newShadowScissor.offset = {0, 0};
	newShadowScissor.extent = { m_shadowMapResolution, m_shadowMapResolution };
	vkCmdSetViewport(commandBuffer, 0, 1, &newShadowViewport);
	vkCmdSetScissor(commandBuffer, 0, 1, &newShadowScissor);

	VkPipeline currentShadowPipeline = VK_NULL_HANDLE;
	for (uint32_t drawIndex = 0; drawIndex < commandList.m_drawCalls.size(); ++drawIndex) {
		const VulkanCommandList::DrawCall& drawCall = commandList.m_drawCalls[drawIndex];
		const VulkanPipelineState* pipelineState = dynamic_cast<const VulkanPipelineState*>(drawCall.pipelineState);
		if (pipelineState == nullptr) return false;
		if (!pipelineState->IsShadowPassEnabled()) continue;
		const size_t profileIndex = ResourceProfileIndex(pipelineState->GetResourceProfile());
		if(profileIndex >= m_shadowPipelines.size()) return false;
		const VkPipeline shadowPipeline = m_shadowPipelines[profileIndex];
		const VkPipelineLayout shadowPipelineLayout = m_shadowPipelineLayouts[profileIndex];
		const ProfileResources* profileResources = GetProfileResources(pipelineState->GetResourceProfile());
		if(shadowPipeline == VK_NULL_HANDLE || shadowPipelineLayout == VK_NULL_HANDLE || profileResources == nullptr) return false;
		if(currentShadowPipeline != shadowPipeline)
		{
			vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, shadowPipeline);
			currentShadowPipeline = shadowPipeline;
		}
		const bool usesBindlessTextures = pipelineState->UsesBindlessTextures();

		bool shouldCastShadow = true;
		if (drawCall.pushConstantSize >= m_shaderLayout.drawModePushConstantOffset + sizeof(float)) {
			float drawMode = 0.0f;
			std::memcpy(&drawMode, drawCall.pushConstants.data() + m_shaderLayout.drawModePushConstantOffset, sizeof(drawMode));
			if (drawCall.pushConstantSize >= m_shaderLayout.pushConstantRangeSize) {
				const uint32_t drawFlags = static_cast<uint32_t>(drawMode + 0.5f);
				shouldCastShadow = (drawFlags & m_shaderLayout.castShadowFlag) != 0;
			} else {
				shouldCastShadow = drawMode >= 0.0f && drawMode <= 1.5f;
			}
		}
		if (!shouldCastShadow) continue;

		const uint32_t descriptorSlot = usesBindlessTextures ? 0u : drawIndex;
		const VkDescriptorSet descriptorSet = GetProfileDescriptorSet(*profileResources, m_currentFrameIndex, descriptorSlot);
		if (descriptorSet == VK_NULL_HANDLE) return false;
		const uint32_t drawConstantOffset = static_cast<uint32_t>(m_drawConstantStride * drawIndex);
		vkCmdBindDescriptorSets(
			commandBuffer,
			VK_PIPELINE_BIND_POINT_GRAPHICS,
			shadowPipelineLayout,
			0,
			1,
			&descriptorSet,
			1,
			&drawConstantOffset);

		if (drawCall.indexed) {
			if (drawCall.indexCount > 0) {
				vkCmdDraw(commandBuffer, drawCall.indexCount, drawCall.instanceCount, 0, drawCall.startInstance);
			}
		} else {
			vkCmdDraw(commandBuffer, drawCall.vertexCount, drawCall.instanceCount, 0, drawCall.startInstance);
		}
	}

	vkCmdEndRenderPass(commandBuffer);
	return true;
}
bool VulkanDevice::Impl::RecordMainPass(VkCommandBuffer commandBuffer, const VulkanCommandList& commandList) {
	VkRenderPass renderPass = VK_NULL_HANDLE;
	VkFramebuffer framebuffer = VK_NULL_HANDLE;
	VkExtent2D renderExtent = {};
	if (!ResolveMainPassTarget(commandList, renderPass, framebuffer, renderExtent)) return false;

	std::array<VkClearValue, 2> clearValues = {};
	clearValues[0].color = { {
		commandList.m_clearColor[0],
		commandList.m_clearColor[1],
		commandList.m_clearColor[2],
		commandList.m_clearColor[3]
	} };
	clearValues[1].depthStencil = { commandList.m_clearDepth, 0 };
	VkRenderPassBeginInfo renderPassInfo{};
	renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
	renderPassInfo.renderPass = renderPass;
	renderPassInfo.framebuffer = framebuffer;
	renderPassInfo.renderArea.offset = {0, 0};
	renderPassInfo.renderArea.extent = renderExtent;
	renderPassInfo.clearValueCount = static_cast<uint32_t>(clearValues.size());
	renderPassInfo.pClearValues = clearValues.data();

	vkCmdBeginRenderPass(commandBuffer, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);

	VkPipeline currentPipeline = VK_NULL_HANDLE;
	VkPipelineLayout currentPipelineLayout = VK_NULL_HANDLE;
	VkDescriptorSet currentDescriptorSet = VK_NULL_HANDLE;
	VkDescriptorSet currentBindlessDescriptorSet = VK_NULL_HANDLE;
	for (uint32_t drawIndex = 0; drawIndex < commandList.m_drawCalls.size(); ++drawIndex) {
		const VulkanCommandList::DrawCall& drawCall = commandList.m_drawCalls[drawIndex];
		const VulkanPipelineState* pipelineState = dynamic_cast<const VulkanPipelineState*>(drawCall.pipelineState);
		if (pipelineState == nullptr) return false;
		const ProfileResources* profileResources = GetProfileResources(pipelineState->GetResourceProfile());
		if(profileResources == nullptr || profileResources->descriptorSetLayout == VK_NULL_HANDLE) return false;
		const bool usesBindlessTextures = pipelineState->UsesBindlessTextures();
		const VulkanPipeline* pipeline = pipelineState->GetPipelineForRenderPass(
			m_context,
			renderPass,
			renderExtent,
			profileResources->descriptorSetLayout,
			usesBindlessTextures ? m_bindlessDescriptorSetLayout : VK_NULL_HANDLE);
		if (pipeline == nullptr) return false;

		if (currentPipeline != pipeline->GetPipeline()) {
			vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline->GetPipeline());
			currentPipeline = pipeline->GetPipeline();
			currentPipelineLayout = pipeline->GetLayout();
			currentDescriptorSet = VK_NULL_HANDLE;
			currentBindlessDescriptorSet = VK_NULL_HANDLE;
		}

		const uint32_t descriptorSlot = usesBindlessTextures ? 0u : drawIndex;
		const VkDescriptorSet descriptorSet = GetProfileDescriptorSet(*profileResources, m_currentFrameIndex, descriptorSlot);
		if (descriptorSet == VK_NULL_HANDLE) return false;
		const uint32_t drawConstantOffset = static_cast<uint32_t>(m_drawConstantStride * drawIndex);
		vkCmdBindDescriptorSets(
			commandBuffer,
			VK_PIPELINE_BIND_POINT_GRAPHICS,
			pipeline->GetLayout(),
			0,
			1,
			&descriptorSet,
			1,
			&drawConstantOffset);
		currentDescriptorSet = descriptorSet;
		const VkDescriptorSet bindlessDescriptorSet = m_currentFrameIndex < m_bindlessDescriptorSets.size()
			? m_bindlessDescriptorSets[m_currentFrameIndex]
			: VK_NULL_HANDLE;
		if (usesBindlessTextures && bindlessDescriptorSet == VK_NULL_HANDLE) return false;
		if (usesBindlessTextures) {
			if (bindlessDescriptorSet != currentBindlessDescriptorSet || pipeline->GetLayout() != currentPipelineLayout) {
				vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline->GetLayout(), 1, 1, &bindlessDescriptorSet, 0, nullptr);
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

		// 메인 셰이더는 정점/인덱스를 storage 버퍼에서 수동 fetch한다(gl_VertexIndex 선형 →
		// indexStorage[firstIndex+id] → vertexStorage). 따라서 IA 인덱스버퍼 없이 vkCmdDraw 로
		// indexCount 개의 정점을 그린다(섀도우 패스와 동일 모델).
		const uint32_t drawVertexCount = drawCall.indexed ? drawCall.indexCount : drawCall.vertexCount;
		if (drawVertexCount > 0) {
			vkCmdDraw(commandBuffer, drawVertexCount, drawCall.instanceCount, 0, drawCall.startInstance);
		}
	}

	vkCmdEndRenderPass(commandBuffer);
	return true;
}

bool VulkanDevice::Impl::ResolveMainPassTarget(const VulkanCommandList& commandList, VkRenderPass& renderPass, VkFramebuffer& framebuffer, VkExtent2D& extent) {
	if (commandList.m_renderTargetCount > 1) {
		SDL_Log("Vulkan main pass currently supports one color render target.");
		return false;
	}

	dy::RHI::ITexture* colorTarget = m_backBuffer;
	if (commandList.m_renderTargetCount == 1 && commandList.m_renderTargets[0] != nullptr) {
		colorTarget = commandList.m_renderTargets[0];
	}

	if (colorTarget == nullptr || colorTarget == m_backBuffer) {
		if (commandList.m_depthStencil != nullptr && commandList.m_depthStencil != m_depthTexture) {
			for (const RenderTargetCacheEntry& entry : m_renderTargetCache) {
				if (entry.colorTarget == m_backBuffer &&
					entry.depthTarget == commandList.m_depthStencil &&
					entry.imageIndex == m_currentImageIndex) {
					renderPass = entry.renderPass;
					framebuffer = entry.framebuffer;
					extent = { entry.width, entry.height };
					return renderPass != VK_NULL_HANDLE && framebuffer != VK_NULL_HANDLE;
				}
			}

			const VulkanTexture* depthTexture = dynamic_cast<const VulkanTexture*>(commandList.m_depthStencil);
			if (depthTexture == nullptr || depthTexture->GetImageView() == VK_NULL_HANDLE) {
				SDL_Log("Vulkan swapchain pass depth target is not a valid Vulkan texture.");
				return false;
			}
			if (depthTexture->GetVkFormat() != m_depthFormat) {
				SDL_Log("Vulkan swapchain pass depth target format does not match the main render pass.");
				return false;
			}

			const auto& views = m_swapchain.GetImageViews();
			if (m_currentImageIndex >= views.size()) return false;

			std::array<VkImageView, 2> attachments = { views[m_currentImageIndex], depthTexture->GetImageView() };
			VkFramebufferCreateInfo fbInfo{};
			fbInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
			fbInfo.renderPass = m_mainRenderPass;
			fbInfo.attachmentCount = static_cast<uint32_t>(attachments.size());
			fbInfo.pAttachments = attachments.data();
			fbInfo.width = m_swapchain.GetExtent().width;
			fbInfo.height = m_swapchain.GetExtent().height;
			fbInfo.layers = 1;

			VkFramebuffer swapchainFramebuffer = VK_NULL_HANDLE;
			if (vkCreateFramebuffer(m_context.device, &fbInfo, nullptr, &swapchainFramebuffer) != VK_SUCCESS) return false;

			RenderTargetCacheEntry entry{};
			entry.colorTarget = m_backBuffer;
			entry.depthTarget = commandList.m_depthStencil;
			entry.renderPass = m_mainRenderPass;
			entry.framebuffer = swapchainFramebuffer;
			entry.imageIndex = m_currentImageIndex;
			entry.width = m_swapchain.GetExtent().width;
			entry.height = m_swapchain.GetExtent().height;
			entry.ownsRenderPass = false;
			m_renderTargetCache.push_back(entry);

			renderPass = entry.renderPass;
			framebuffer = entry.framebuffer;
			extent = { entry.width, entry.height };
			return renderPass != VK_NULL_HANDLE && framebuffer != VK_NULL_HANDLE;
		}
		renderPass = m_mainRenderPass;
		framebuffer = m_swapchainFramebuffers[m_currentImageIndex];
		extent = m_swapchain.GetExtent();
		return renderPass != VK_NULL_HANDLE && framebuffer != VK_NULL_HANDLE;
	}

	return GetOrCreateOffscreenFramebuffer(colorTarget, commandList.m_depthStencil, renderPass, framebuffer, extent);
}

bool VulkanDevice::Impl::GetOrCreateOffscreenFramebuffer(
	dy::RHI::ITexture* colorTarget,
	dy::RHI::ITexture* depthTarget,
	VkRenderPass& renderPass,
	VkFramebuffer& framebuffer,
	VkExtent2D& extent)
{
	for (const RenderTargetCacheEntry& entry : m_renderTargetCache) {
		if (entry.colorTarget == colorTarget && entry.depthTarget == depthTarget) {
			renderPass = entry.renderPass;
			framebuffer = entry.framebuffer;
			extent = { entry.width, entry.height };
			return renderPass != VK_NULL_HANDLE && framebuffer != VK_NULL_HANDLE;
		}
	}

	VulkanTexture* colorTexture = dynamic_cast<VulkanTexture*>(colorTarget);
	if (colorTexture == nullptr || colorTexture->GetImageView() == VK_NULL_HANDLE) {
		SDL_Log("Vulkan offscreen color target is not a valid Vulkan texture.");
		return false;
	}

	std::unique_ptr<VulkanTexture> ownedDepthTexture;
	if (depthTarget == nullptr) {
		dy::RHI::TextureDesc depthDesc{};
		depthDesc.width = colorTexture->GetWidth();
		depthDesc.height = colorTexture->GetHeight();
		depthDesc.depthOrArraySize = 1;
		depthDesc.mipLevels = 1;
		depthDesc.format = ToRhiDepthFormat(m_depthFormat);
		depthDesc.usage = dy::RHI::TextureUsage::DepthStencil;
		ownedDepthTexture.reset(new VulkanTexture(depthDesc));
		if (!ownedDepthTexture->Initialize(m_context, depthDesc, m_depthFormat)) return false;
		depthTarget = ownedDepthTexture.get();
	}

	VulkanTexture* depthTexture = dynamic_cast<VulkanTexture*>(depthTarget);
	if (depthTexture == nullptr || depthTexture->GetImageView() == VK_NULL_HANDLE) {
		SDL_Log("Vulkan offscreen depth target is not a valid Vulkan texture.");
		return false;
	}
	if (depthTexture->GetWidth() != colorTexture->GetWidth() || depthTexture->GetHeight() != colorTexture->GetHeight()) {
		SDL_Log("Vulkan offscreen color/depth target sizes do not match.");
		return false;
	}

	VkRenderPass offscreenRenderPass = VK_NULL_HANDLE;
	const VkImageLayout colorFinalLayout = HasTextureUsage(
		colorTexture->GetUsage(),
		dy::RHI::TextureUsage::ShaderResource)
		? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
		: VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	if (!CreateOffscreenRenderPass(
		colorTexture->GetVkFormat(),
		depthTexture->GetVkFormat(),
		colorFinalLayout,
		offscreenRenderPass)) {
		return false;
	}

	std::array<VkImageView, 2> attachments = { colorTexture->GetImageView(), depthTexture->GetImageView() };
	VkFramebufferCreateInfo fbInfo{};
	fbInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
	fbInfo.renderPass = offscreenRenderPass;
	fbInfo.attachmentCount = static_cast<uint32_t>(attachments.size());
	fbInfo.pAttachments = attachments.data();
	fbInfo.width = colorTexture->GetWidth();
	fbInfo.height = colorTexture->GetHeight();
	fbInfo.layers = 1;

	VkFramebuffer offscreenFramebuffer = VK_NULL_HANDLE;
	if (vkCreateFramebuffer(m_context.device, &fbInfo, nullptr, &offscreenFramebuffer) != VK_SUCCESS) {
		vkDestroyRenderPass(m_context.device, offscreenRenderPass, nullptr);
		return false;
	}

	colorTexture->SetImageLayout(colorFinalLayout);

	RenderTargetCacheEntry entry{};
	entry.colorTarget = colorTarget;
	entry.depthTarget = depthTarget == ownedDepthTexture.get() ? nullptr : depthTarget;
	entry.ownedDepthTarget = ownedDepthTexture.release();
	entry.renderPass = offscreenRenderPass;
	entry.framebuffer = offscreenFramebuffer;
	entry.width = colorTexture->GetWidth();
	entry.height = colorTexture->GetHeight();
	entry.ownsRenderPass = true;
	m_renderTargetCache.push_back(entry);

	renderPass = offscreenRenderPass;
	framebuffer = offscreenFramebuffer;
	extent = { entry.width, entry.height };
	return true;
}

bool VulkanDevice::Impl::CreateOffscreenRenderPass(VkFormat colorFormat, VkFormat depthFormat, VkImageLayout colorFinalLayout, VkRenderPass& renderPass) {
	VkAttachmentDescription colorAttachment{};
	colorAttachment.format = colorFormat;
	colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
	colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
	colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	colorAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	colorAttachment.finalLayout = colorFinalLayout;

	VkAttachmentDescription depthAttachment{};
	depthAttachment.format = depthFormat;
	depthAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
	depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
	depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	depthAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	depthAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	depthAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	depthAttachment.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

	VkAttachmentReference colorAttachmentRef{};
	colorAttachmentRef.attachment = 0;
	colorAttachmentRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

	VkAttachmentReference depthAttachmentRef{};
	depthAttachmentRef.attachment = 1;
	depthAttachmentRef.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

	VkSubpassDescription subpass{};
	subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
	subpass.colorAttachmentCount = 1;
	subpass.pColorAttachments = &colorAttachmentRef;
	subpass.pDepthStencilAttachment = &depthAttachmentRef;

	VkSubpassDependency dependency{};
	dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
	dependency.dstSubpass = 0;
	dependency.srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
	dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
	dependency.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
	dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
	dependency.dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

	std::array<VkAttachmentDescription, 2> attachments = { colorAttachment, depthAttachment };
	VkRenderPassCreateInfo renderPassInfo{};
	renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
	renderPassInfo.attachmentCount = static_cast<uint32_t>(attachments.size());
	renderPassInfo.pAttachments = attachments.data();
	renderPassInfo.subpassCount = 1;
	renderPassInfo.pSubpasses = &subpass;
	renderPassInfo.dependencyCount = 1;
	renderPassInfo.pDependencies = &dependency;

	return vkCreateRenderPass(m_context.device, &renderPassInfo, nullptr, &renderPass) == VK_SUCCESS;
}

bool VulkanDevice::Impl::CreateMainRenderPass() {
	VkAttachmentDescription colorAttachment{};
	colorAttachment.format = m_swapchain.GetImageFormat();
	colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
	colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
	colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	colorAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	colorAttachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

	VkAttachmentDescription depthAttachment{};
	depthAttachment.format = m_depthFormat;
	depthAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
	depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
	depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	depthAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	depthAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	depthAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	depthAttachment.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

	VkAttachmentReference colorAttachmentRef{};
	colorAttachmentRef.attachment = 0;
	colorAttachmentRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

	VkAttachmentReference depthAttachmentRef{};
	depthAttachmentRef.attachment = 1;
	depthAttachmentRef.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

	VkSubpassDescription subpass{};
	subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
	subpass.colorAttachmentCount = 1;
	subpass.pColorAttachments = &colorAttachmentRef;
	subpass.pDepthStencilAttachment = &depthAttachmentRef;

	VkSubpassDependency dependency{};
	dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
	dependency.dstSubpass = 0;
	dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
	dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
	dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

	std::array<VkAttachmentDescription, 2> attachments = { colorAttachment, depthAttachment };

	VkRenderPassCreateInfo renderPassInfo{};
	renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
	renderPassInfo.attachmentCount = static_cast<uint32_t>(attachments.size());
	renderPassInfo.pAttachments = attachments.data();
	renderPassInfo.subpassCount = 1;
	renderPassInfo.pSubpasses = &subpass;
	renderPassInfo.dependencyCount = 1;
	renderPassInfo.pDependencies = &dependency;

	return vkCreateRenderPass(m_context.device, &renderPassInfo, nullptr, &m_mainRenderPass) == VK_SUCCESS;
}

bool VulkanDevice::Impl::CreateFramebuffers() {
	const VulkanTexture* depthTexture = static_cast<const VulkanTexture*>(m_depthTexture);
	if (depthTexture == nullptr || depthTexture->GetImageView() == VK_NULL_HANDLE) return false;

	const auto& views = m_swapchain.GetImageViews();
	m_swapchainFramebuffers.resize(views.size());
	for (size_t i = 0; i < views.size(); ++i) {
		VkImageView attachments[] = { views[i], depthTexture->GetImageView() };
		VkFramebufferCreateInfo fbInfo{};
		fbInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
		fbInfo.renderPass = m_mainRenderPass;
		fbInfo.attachmentCount = static_cast<uint32_t>(sizeof(attachments) / sizeof(attachments[0]));
		fbInfo.pAttachments = attachments;
		fbInfo.width = m_swapchain.GetExtent().width;
		fbInfo.height = m_swapchain.GetExtent().height;
		fbInfo.layers = 1;
		if (vkCreateFramebuffer(m_context.device, &fbInfo, nullptr, &m_swapchainFramebuffers[i]) != VK_SUCCESS) return false;
	}
	return true;
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
	const int w = static_cast<int>(m_fallbackTextureWidth);
	const int h = static_cast<int>(m_fallbackTextureHeight);
	std::vector<unsigned char> fallbackPixels(static_cast<size_t>(w) * static_cast<size_t>(h) * 4u);
	for (int y = 0; y < h; ++y) {
		for (int x = 0; x < w; ++x) {
			const bool bright = ((x + y) & 1) == 0;
			const size_t offset = (static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)) * 4u;
			fallbackPixels[offset + 0u] = bright ? 255 : 64;
			fallbackPixels[offset + 1u] = bright ? 255 : 64;
			fallbackPixels[offset + 2u] = bright ? 255 : 64;
			fallbackPixels[offset + 3u] = 255;
		}
	}
	const unsigned char* pixels = fallbackPixels.data();

	dy::RHI::TextureDesc desc{};
	desc.width = static_cast<uint32_t>(w);
	desc.height = static_cast<uint32_t>(h);
	desc.depthOrArraySize = 1;
	desc.mipLevels = 1;
	desc.format = dy::RHI::Format::R8G8B8A8_UNORM;
	desc.usage = dy::RHI::TextureUsage::ShaderResource;

	std::unique_ptr<VulkanTexture> texture(new VulkanTexture(desc));
	const bool created =
		texture->Initialize(m_context, desc, VK_FORMAT_R8G8B8A8_SRGB) &&
		texture->CreateDefaultSampler() &&
		texture->UploadRGBA8(m_context, m_commandPool, pixels, desc.width, desc.height);

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
		sizeof(Layout::DrawConstants),
		m_capabilities.limits.minUniformBufferOffsetAlignment,
		alignedStride))
	{
		SDL_Log("Failed to align Vulkan draw constant UBO stride.");
		return false;
	}
	if(alignedStride > std::numeric_limits<uint32_t>::max()) return false;
	const uint64_t totalSize = alignedStride * static_cast<uint64_t>(m_descriptorCapacityPerFrame);
	if(m_descriptorCapacityPerFrame != 0u && totalSize / m_descriptorCapacityPerFrame != alignedStride) return false;
	if(totalSize == 0u || totalSize > std::numeric_limits<uint32_t>::max())
	{
		SDL_Log("Vulkan draw constant UBO capacity exceeds dynamic-offset range.");
		return false;
	}

	m_drawConstantStride = static_cast<VkDeviceSize>(alignedStride);
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
			SDL_Log("Vulkan draw capacity exceeded: requested=%zu capacity=%u pageSize=%u pages=%u.",
				commandList.m_drawCalls.size(),
				m_descriptorCapacityPerFrame,
				m_maxDrawsPerFrame,
				kMaxDescriptorPagesPerFrame);
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
		if(drawCall.indexed && drawCall.pushConstantSize >= offsetof(Layout::DrawConstants, firstVertex) + sizeof(firstVertex))
		{
			std::memcpy(
				&firstVertex,
				drawCall.pushConstants.data() + offsetof(Layout::DrawConstants, firstVertex),
				sizeof(firstVertex));
		}
		Layout::DrawConstants constants{};
		if(!PrepareVulkanDrawConstants(
			drawCall.pushConstants.data(),
			drawCall.pushConstantSize,
			drawCall.firstIndex,
			drawCall.baseVertex,
			firstVertex,
			constants))
		{
			return false;
		}
		std::memcpy(
			frame.mapped + static_cast<size_t>(m_drawConstantStride) * drawIndex,
			&constants,
			sizeof(constants));
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
	addBinding(m_shaderLayout.shadowMatrixBinding, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SHADER_STAGE_VERTEX_BIT);
	addBinding(m_shaderLayout.vertexStorageBinding, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_VERTEX_BIT);
	addBinding(m_shaderLayout.indexStorageBinding, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_VERTEX_BIT);
	if(profile == dy::RHI::GraphicsResourceProfile::PerDrawSkin)
	{
		addBinding(Layout::kSkinInfluenceStorageBinding, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_VERTEX_BIT);
		addBinding(Layout::kSkinPaletteStorageBinding, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_VERTEX_BIT);
	}
	else
	{
		addBinding(m_shaderLayout.bindlessTransformStorageBinding, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_VERTEX_BIT);
	}
	addBinding(Layout::kVulkanDrawConstantsBinding, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT);

	VkDescriptorSetLayoutCreateInfo info{};
	info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
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

bool VulkanDevice::Impl::CreateDescriptorPool(
	dy::RHI::GraphicsResourceProfile profile,
	VkDescriptorPool& outPool)
{
	outPool = VK_NULL_HANDLE;
	const uint64_t descriptorSetCount = m_descriptorSetCount;
	const uint64_t samplerCount = profile == dy::RHI::GraphicsResourceProfile::Bindless ? 1u : m_shaderLayout.samplerDescriptorCount;
	const uint64_t uniformCount = 3u;
	const uint64_t storageCount = RequiredVulkanStorageBufferCount(profile);
	if(descriptorSetCount * samplerCount > std::numeric_limits<uint32_t>::max()
		|| descriptorSetCount * uniformCount > std::numeric_limits<uint32_t>::max()
		|| descriptorSetCount * storageCount > std::numeric_limits<uint32_t>::max()) return false;

	std::array<VkDescriptorPoolSize, 4> poolSizes = {};
	poolSizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	poolSizes[0].descriptorCount = static_cast<uint32_t>(descriptorSetCount * samplerCount);
	poolSizes[1].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
	poolSizes[1].descriptorCount = static_cast<uint32_t>(descriptorSetCount * 2u);
	poolSizes[2].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
	poolSizes[2].descriptorCount = static_cast<uint32_t>(descriptorSetCount);
	poolSizes[3].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	poolSizes[3].descriptorCount = static_cast<uint32_t>(descriptorSetCount * storageCount);
	VkDescriptorPoolCreateInfo info{};
	info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	info.maxSets = static_cast<uint32_t>(descriptorSetCount);
	info.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
	info.pPoolSizes = poolSizes.data();
	return vkCreateDescriptorPool(m_context.device, &info, nullptr, &outPool) == VK_SUCCESS;
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

bool VulkanDevice::Impl::EnsureDescriptorSets(
	ProfileResources& resources,
	uint32_t frameIndex,
	uint32_t requiredSetCount)
{
	if(frameIndex >= resources.frameDescriptorSets.size()
		|| requiredSetCount > m_descriptorCapacityPerFrame) return false;
	std::vector<VkDescriptorSet>& frameSets = resources.frameDescriptorSets[frameIndex];
	while(frameSets.size() < requiredSetCount)
	{
		const uint32_t remainingCapacity = m_descriptorCapacityPerFrame - static_cast<uint32_t>(frameSets.size());
		if(remainingCapacity == 0u) return false;
		const uint32_t allocationCount = std::min(m_maxDrawsPerFrame, remainingCapacity);
		std::vector<VkDescriptorSetLayout> layouts(allocationCount, resources.descriptorSetLayout);
		std::vector<VkDescriptorSet> page(allocationCount, VK_NULL_HANDLE);
		VkDescriptorSetAllocateInfo alloc{};
		alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
		alloc.descriptorPool = resources.descriptorPool;
		alloc.descriptorSetCount = allocationCount;
		alloc.pSetLayouts = layouts.data();
		if(vkAllocateDescriptorSets(m_context.device, &alloc, page.data()) != VK_SUCCESS) return false;
		frameSets.insert(frameSets.end(), page.begin(), page.end());
	}
	return true;
}

VkDescriptorSet VulkanDevice::Impl::GetProfileDescriptorSet(
	const ProfileResources& resources,
	uint32_t frameIndex,
	uint32_t drawSlot) const
{
	if(frameIndex >= resources.frameDescriptorSets.size()) return VK_NULL_HANDLE;
	const std::vector<VkDescriptorSet>& frameSets = resources.frameDescriptorSets[frameIndex];
	return drawSlot < frameSets.size() ? frameSets[drawSlot] : VK_NULL_HANDLE;
}

bool VulkanDevice::Impl::EnsureProfileResources(dy::RHI::GraphicsResourceProfile profile)
{
	ProfileResources* resources = GetProfileResources(profile);
	if(resources == nullptr) return false;
	if(resources->descriptorSetLayout != VK_NULL_HANDLE) return true;
	if(!SupportsVulkanResourceProfile(m_capabilities, profile)) return false;
	if(!CreateDescriptorSetLayout(profile, resources->descriptorSetLayout)) return false;
	if(!CreateDescriptorPool(profile, resources->descriptorPool))
	{
		vkDestroyDescriptorSetLayout(m_context.device, resources->descriptorSetLayout, nullptr);
		resources->descriptorSetLayout = VK_NULL_HANDLE;
		return false;
	}
	resources->frameDescriptorSets.resize(m_maxFramesInFlight);
	return true;
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
			const VulkanCommandList::StorageBufferBinding& captured = dispatch.storageBuffers[binding];
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

bool VulkanDevice::Impl::UpdateDrawDescriptorSets(const VulkanCommandList& commandList) {
	if (commandList.m_drawCalls.size() > m_descriptorCapacityPerFrame) {
		SDL_Log("Vulkan draw descriptor capacity exceeded: requested=%zu capacity=%u pageSize=%u.",
			commandList.m_drawCalls.size(), m_descriptorCapacityPerFrame, m_maxDrawsPerFrame);
		return false;
	}
	if (!commandList.m_drawCalls.empty()) {
		const VulkanCommandList::DrawCall& firstDraw = commandList.m_drawCalls.front();
		const VulkanPipelineState* pipelineState = dynamic_cast<const VulkanPipelineState*>(firstDraw.pipelineState);
		if (pipelineState != nullptr && pipelineState->UsesBindlessTextures()) {
			return UpdateDrawDescriptorSet(firstDraw, 0);
		}
	}
	for (uint32_t drawIndex = 0; drawIndex < commandList.m_drawCalls.size(); ++drawIndex) {
		if (!UpdateDrawDescriptorSet(commandList.m_drawCalls[drawIndex], drawIndex)) return false;
	}
	return true;
}

bool VulkanDevice::Impl::UpdateDrawDescriptorSet(const VulkanCommandList::DrawCall& drawCall, uint32_t drawIndex) {
	const VulkanPipelineState* pipelineState = dynamic_cast<const VulkanPipelineState*>(drawCall.pipelineState);
	if(pipelineState == nullptr) return false;
	ProfileResources* profileResources = GetProfileResources(pipelineState->GetResourceProfile());
	if(profileResources == nullptr) return false;
	if(!EnsureDescriptorSets(*profileResources, m_currentFrameIndex, drawIndex + 1u)) return false;
	const VkDescriptorSet descriptorSet = GetProfileDescriptorSet(*profileResources, m_currentFrameIndex, drawIndex);
	if(descriptorSet == VK_NULL_HANDLE) return false;

	const bool usesBindlessTextures = pipelineState != nullptr && pipelineState->UsesBindlessTextures();
	const VulkanBuffer* vertexBuffer = dynamic_cast<const VulkanBuffer*>(drawCall.geometry.vertexBuffer);
	const VulkanBuffer* indexBuffer = dynamic_cast<const VulkanBuffer*>(drawCall.geometry.indexBuffer);
	if (vertexBuffer == nullptr || (drawCall.indexed && indexBuffer == nullptr)) return false;

	std::vector<VkWriteDescriptorSet> writes;
	writes.reserve(Layout::kVulkanDescriptorBindingCount);

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
			textureWrite.dstSet = descriptorSet;
			textureWrite.dstBinding = binding;
			textureWrite.descriptorCount = 1;
			textureWrite.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
			textureWrite.pImageInfo = &materialTextureInfos[i];
			writes.push_back(textureWrite);
		}
	}

	std::array<VkDescriptorBufferInfo, VulkanCommandList::kMaxConstantBufferBindings> constantInfos = {};
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
		constantWrite.dstSet = descriptorSet;
		constantWrite.dstBinding = binding;
		constantWrite.descriptorCount = 1;
		constantWrite.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
		constantWrite.pBufferInfo = &constantInfos[binding];
		writes.push_back(constantWrite);
	}

	std::array<VkDescriptorBufferInfo, VulkanCommandList::kMaxConstantBufferBindings> storageInfos = {};
	for (uint32_t binding = 0; binding < drawCall.storageBuffers.size(); ++binding) {
		if(!IsProfileStorageBinding(pipelineState->GetResourceProfile(), binding)) continue;
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
		storageWrite.dstSet = descriptorSet;
		storageWrite.dstBinding = binding;
		storageWrite.descriptorCount = 1;
		storageWrite.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		storageWrite.pBufferInfo = &storageInfos[binding];
		writes.push_back(storageWrite);
	}

	const VulkanTexture* shadowTexture = static_cast<const VulkanTexture*>(m_shadowMapTexture);
	const bool usingFallbackShadowTexture = shadowTexture == nullptr;
	if (usingFallbackShadowTexture && hasFallbackTexture) {
		shadowTexture = fallbackTexture;
	}
	VkDescriptorImageInfo shadowInfo{};
	if (shadowTexture != nullptr && shadowTexture->GetImageView() != VK_NULL_HANDLE && shadowTexture->GetSampler() != VK_NULL_HANDLE) {
		shadowInfo.sampler = shadowTexture->GetSampler();
		shadowInfo.imageView = shadowTexture->GetImageView();
		shadowInfo.imageLayout = usingFallbackShadowTexture
			? shadowTexture->GetImageLayout()
			: VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		VkWriteDescriptorSet shadowWrite{};
		shadowWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		shadowWrite.dstSet = descriptorSet;
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
	vertexWrite.dstSet = descriptorSet;
	vertexWrite.dstBinding = m_shaderLayout.vertexStorageBinding;
	vertexWrite.descriptorCount = 1;
	vertexWrite.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	vertexWrite.pBufferInfo = &geometryInfos[0];
	writes.push_back(vertexWrite);

	VkWriteDescriptorSet indexWrite{};
	indexWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	indexWrite.dstSet = descriptorSet;
	indexWrite.dstBinding = m_shaderLayout.indexStorageBinding;
	indexWrite.descriptorCount = 1;
	indexWrite.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	indexWrite.pBufferInfo = &geometryInfos[1];
	writes.push_back(indexWrite);

	if(m_currentFrameIndex >= m_drawConstantFrames.size()) return false;
	const DrawConstantFrame& drawConstantFrame = m_drawConstantFrames[m_currentFrameIndex];
	if(drawConstantFrame.buffer == VK_NULL_HANDLE) return false;
	VkDescriptorBufferInfo drawConstantInfo{};
	drawConstantInfo.buffer = drawConstantFrame.buffer;
	drawConstantInfo.offset = 0u;
	drawConstantInfo.range = sizeof(Layout::DrawConstants);
	VkWriteDescriptorSet drawConstantWrite{};
	drawConstantWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	drawConstantWrite.dstSet = descriptorSet;
	drawConstantWrite.dstBinding = Layout::kVulkanDrawConstantsBinding;
	drawConstantWrite.descriptorCount = 1u;
	drawConstantWrite.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
	drawConstantWrite.pBufferInfo = &drawConstantInfo;
	writes.push_back(drawConstantWrite);

	vkUpdateDescriptorSets(m_context.device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
	return true;
}

bool VulkanDevice::Impl::CreateShadowRenderPass() {
	// Depth-only RenderPass.
	// Color attachment 없음. Fragment shader에서 색을 출력하지 않는다.
	// finalLayout을 SHADER_READ_ONLY_OPTIMAL로 두면 render pass 종료 후 shader sample에 바로 사용할 수 있다.
	VkAttachmentDescription depthAttachment{};
	depthAttachment.format = m_shadowMapFormat;
	depthAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
	depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
	depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;            // Fragment shader에서 sample해야 하므로 STORE
	depthAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	depthAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	depthAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	depthAttachment.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

	VkAttachmentReference depthAttachmentRef{};
	depthAttachmentRef.attachment = 0;
	depthAttachmentRef.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

	VkSubpassDescription subpass{};
	subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
	subpass.colorAttachmentCount = 0;
	subpass.pDepthStencilAttachment = &depthAttachmentRef;

	// 두 개의 dependency: 이전 pass의 sample 완료 후 depth 기록 시작, depth 기록 완료 후 다음 sample 허용.
	std::array<VkSubpassDependency, 2> dependencies = {};
	dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
	dependencies[0].dstSubpass = 0;
	dependencies[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
	dependencies[0].dstStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
	dependencies[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
	dependencies[0].dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
	dependencies[0].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

	dependencies[1].srcSubpass = 0;
	dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
	dependencies[1].srcStageMask =
		VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT
		| VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
	dependencies[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
	dependencies[1].srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
	dependencies[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
	dependencies[1].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

	VkRenderPassCreateInfo info{};
	info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
	info.attachmentCount = 1;
	info.pAttachments = &depthAttachment;
	info.subpassCount = 1;
	info.pSubpasses = &subpass;
	info.dependencyCount = static_cast<uint32_t>(dependencies.size());
	info.pDependencies = dependencies.data();
	return vkCreateRenderPass(m_context.device, &info, nullptr, &m_shadowRenderPass) == VK_SUCCESS;
}

bool VulkanDevice::Impl::CreateShadowMapResources() {
	dy::RHI::TextureDesc desc{};
	desc.width = m_shadowMapResolution;
	desc.height = m_shadowMapResolution;
	desc.depthOrArraySize = 1;
	desc.mipLevels = 1;
	desc.format = ToRhiDepthFormat(m_shadowMapFormat);
	desc.usage = dy::RHI::TextureUsage::DepthStencil | dy::RHI::TextureUsage::ShaderResource;

	std::unique_ptr<VulkanTexture> shadowTexture(new VulkanTexture(desc));
	if (!shadowTexture->Initialize(m_context, desc, m_shadowMapFormat)) return false;

	// Sampler: CLAMP_TO_BORDER + 흰색 border. Shadow frustum 밖 좌표는 그림자가 없는 것으로 처리한다.
	VkSamplerCreateInfo samplerInfo{};
	samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
	samplerInfo.magFilter = VK_FILTER_LINEAR;
	samplerInfo.minFilter = VK_FILTER_LINEAR;
	samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
	samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
	samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
	samplerInfo.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;     // 1.0 = 그림자를 받지 않음
	samplerInfo.compareEnable = VK_FALSE;                              // PCF는 shader에서 직접 계산
	samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
	if (!shadowTexture->CreateSampler(samplerInfo)) {
		return false;
	}

	// Framebuffer
	VkImageView shadowView = shadowTexture->GetImageView();
	VkFramebufferCreateInfo fbInfo{};
	fbInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
	fbInfo.renderPass = m_shadowRenderPass;
	fbInfo.attachmentCount = 1;
	fbInfo.pAttachments = &shadowView;
	fbInfo.width = m_shadowMapResolution;
	fbInfo.height = m_shadowMapResolution;
	fbInfo.layers = 1;
	if (vkCreateFramebuffer(m_context.device, &fbInfo, nullptr, &m_shadowFramebuffer) != VK_SUCCESS) return false;

	delete m_shadowMapTexture;
	m_shadowMapTexture = shadowTexture.release();
	return true;
}

bool VulkanDevice::Impl::CreateShadowPipeline(const dy::RHI::GraphicsPipelineDesc& desc) {
	if (desc.shadowVertexShader == nullptr || desc.shadowVertexShaderSize == 0) {
		SDL_Log("Shadow pipeline shader bytecode is missing");
		return false;
	}
	const size_t profileIndex = ResourceProfileIndex(desc.resourceProfile);
	if(profileIndex >= m_shadowPipelines.size()) return false;
	ProfileResources* profileResources = GetProfileResources(desc.resourceProfile);
	if(profileResources == nullptr || profileResources->descriptorSetLayout == VK_NULL_HANDLE) return false;
	VkPipelineLayout& shadowPipelineLayout = m_shadowPipelineLayouts[profileIndex];
	VkPipeline& shadowPipeline = m_shadowPipelines[profileIndex];
	// PipelineLayout은 main PSO와 같은 DescriptorSetLayout + PushConstantRange를 공유한다.
	// Shadow pass도 같은 binding 3의 lightViewProj UBO를 읽기 때문에 호환 가능하다.
	VkPipelineLayoutCreateInfo layoutInfo{};
	layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	layoutInfo.setLayoutCount = 1;
	layoutInfo.pSetLayouts = &profileResources->descriptorSetLayout;
	layoutInfo.pushConstantRangeCount = 0;
	layoutInfo.pPushConstantRanges = nullptr;
	if (vkCreatePipelineLayout(m_context.device, &layoutInfo, nullptr, &shadowPipelineLayout) != VK_SUCCESS) {
		return false;
	}

	// Shadow vertex shader 로드
	VkShaderModule vertModule = VK_NULL_HANDLE;
	try {
		vertModule = CreateShaderModule(m_context.device, desc.shadowVertexShader, desc.shadowVertexShaderSize);
	} catch (...) {
		vkDestroyPipelineLayout(m_context.device, shadowPipelineLayout, nullptr);
		shadowPipelineLayout = VK_NULL_HANDLE;
		throw;
	}
	VkPipelineShaderStageCreateInfo stage{};
	stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stage.stage = VK_SHADER_STAGE_VERTEX_BIT;
	stage.module = vertModule;
	stage.pName = "main";

	// Vertex data is pulled from the storage buffer in the shader.
	VkPipelineVertexInputStateCreateInfo vertexInput{};
	vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
	vertexInput.vertexBindingDescriptionCount = 0;
	vertexInput.pVertexBindingDescriptions = nullptr;
	vertexInput.vertexAttributeDescriptionCount = 0;
	vertexInput.pVertexAttributeDescriptions = nullptr;

	VkPipelineInputAssemblyStateCreateInfo ia{};
	ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
	ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

	VkPipelineViewportStateCreateInfo viewportState{};
	viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
	viewportState.viewportCount = 1;
	viewportState.scissorCount = 1;

	VkPipelineRasterizationStateCreateInfo rast{};
	rast.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
	rast.polygonMode = VK_POLYGON_MODE_FILL;
	rast.cullMode = VK_CULL_MODE_NONE;             // 양면 메시지도 그림자에 기여해야 한다.
	rast.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
	rast.lineWidth = 1.0f;
	rast.depthBiasEnable = VK_TRUE;                // Slope-scaled bias로 shadow acne 완화
	rast.depthBiasConstantFactor = 1.25f;
	rast.depthBiasSlopeFactor = 1.75f;

	VkPipelineMultisampleStateCreateInfo ms{};
	ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
	ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

	VkPipelineDepthStencilStateCreateInfo ds{};
	ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
	ds.depthTestEnable = VK_TRUE;
	ds.depthWriteEnable = VK_TRUE;
	ds.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
	ds.depthBoundsTestEnable = VK_FALSE;
	ds.stencilTestEnable = VK_FALSE;

	// Color attachment가 0개여도 color blend state 구조체는 필요하다.
	VkPipelineColorBlendStateCreateInfo cb{};
	cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
	cb.attachmentCount = 0;
	cb.pAttachments = nullptr;

	const VkDynamicState dynamicStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
	VkPipelineDynamicStateCreateInfo dyn{};
	dyn.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
	dyn.dynamicStateCount = 2;
	dyn.pDynamicStates = dynamicStates;

	VkGraphicsPipelineCreateInfo pipelineInfo{};
	pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
	pipelineInfo.stageCount = 1;
	pipelineInfo.pStages = &stage;                 // Fragment shader 없음
	pipelineInfo.pVertexInputState = &vertexInput;
	pipelineInfo.pInputAssemblyState = &ia;
	pipelineInfo.pViewportState = &viewportState;
	pipelineInfo.pRasterizationState = &rast;
	pipelineInfo.pMultisampleState = &ms;
	pipelineInfo.pDepthStencilState = &ds;
	pipelineInfo.pColorBlendState = &cb;
	pipelineInfo.pDynamicState = &dyn;
	pipelineInfo.layout = shadowPipelineLayout;
	pipelineInfo.renderPass = m_shadowRenderPass;
	pipelineInfo.subpass = 0;

	const VkResult result = vkCreateGraphicsPipelines(m_context.device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &shadowPipeline);
	vkDestroyShaderModule(m_context.device, vertModule, nullptr);
	if(result != VK_SUCCESS)
	{
		vkDestroyPipelineLayout(m_context.device, shadowPipelineLayout, nullptr);
		shadowPipelineLayout = VK_NULL_HANDLE;
	}
	return result == VK_SUCCESS;
}

void VulkanDevice::Impl::DestroyShadowResources() {
	if (m_context.device == VK_NULL_HANDLE) return;

	for(size_t index = 0u; index < m_shadowPipelines.size(); ++index)
	{
		if (m_shadowPipelines[index] != VK_NULL_HANDLE) {
			vkDestroyPipeline(m_context.device, m_shadowPipelines[index], nullptr);
			m_shadowPipelines[index] = VK_NULL_HANDLE;
		}
		if (m_shadowPipelineLayouts[index] != VK_NULL_HANDLE) {
			vkDestroyPipelineLayout(m_context.device, m_shadowPipelineLayouts[index], nullptr);
			m_shadowPipelineLayouts[index] = VK_NULL_HANDLE;
		}
	}
	if (m_shadowFramebuffer != VK_NULL_HANDLE) {
		vkDestroyFramebuffer(m_context.device, m_shadowFramebuffer, nullptr);
		m_shadowFramebuffer = VK_NULL_HANDLE;
	}
	delete m_shadowMapTexture;
	m_shadowMapTexture = nullptr;
	if (m_shadowRenderPass != VK_NULL_HANDLE) {
		vkDestroyRenderPass(m_context.device, m_shadowRenderPass, nullptr);
		m_shadowRenderPass = VK_NULL_HANDLE;
	}
}

void VulkanDevice::Impl::DestroyRenderTargetCache() {
	if (m_context.device == VK_NULL_HANDLE) return;

	for (RenderTargetCacheEntry& entry : m_renderTargetCache) {
		if (entry.framebuffer != VK_NULL_HANDLE) {
			vkDestroyFramebuffer(m_context.device, entry.framebuffer, nullptr);
			entry.framebuffer = VK_NULL_HANDLE;
		}
		if (entry.ownsRenderPass && entry.renderPass != VK_NULL_HANDLE) {
			vkDestroyRenderPass(m_context.device, entry.renderPass, nullptr);
			entry.renderPass = VK_NULL_HANDLE;
		}
		delete entry.ownedDepthTarget;
		entry.ownedDepthTarget = nullptr;
	}
	m_renderTargetCache.clear();
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
		dy::RHI::IsSrgbFormat(GetDesc().swapchainFormat));
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
		resourcesCreated = CreateMainRenderPass()
			&& CreateDepthResources()
			&& CreateFramebuffers();
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
	DestroyRenderTargetCache();
	for (auto fb : m_swapchainFramebuffers) vkDestroyFramebuffer(m_context.device, fb, nullptr);
	m_swapchainFramebuffers.clear();
	delete m_depthTexture;
	m_depthTexture = nullptr;
	if (m_mainRenderPass != VK_NULL_HANDLE) {
		vkDestroyRenderPass(m_context.device, m_mainRenderPass, nullptr);
		m_mainRenderPass = VK_NULL_HANDLE;
	}
	m_swapchain.Cleanup(m_context.device);
}

void VulkanDevice::Impl::DestroyDeviceResources() {
	if (m_context.device != VK_NULL_HANDLE) {
		vkDeviceWaitIdle(m_context.device);
		DestroyRenderTargetCache();
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
		vkDeviceWaitIdle(m_context.device);
		DestroyDrawConstantBuffers();
		for (dy::RHI::IBuffer* buffer : m_ownedBuffers) delete buffer;
		m_ownedBuffers.clear();
		DestroyAllRetiredBuffers();
		DestroyShadowResources();
		for(ProfileResources& resources : m_profileResources)
		{
			if(resources.descriptorPool != VK_NULL_HANDLE) vkDestroyDescriptorPool(m_context.device, resources.descriptorPool, nullptr);
			if(resources.descriptorSetLayout != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(m_context.device, resources.descriptorSetLayout, nullptr);
			resources = {};
		}
		if (m_bindlessDescriptorPool != VK_NULL_HANDLE) vkDestroyDescriptorPool(m_context.device, m_bindlessDescriptorPool, nullptr);
		if (m_bindlessDescriptorSetLayout != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(m_context.device, m_bindlessDescriptorSetLayout, nullptr);
		for (auto s : m_imageAvailableSemaphores) vkDestroySemaphore(m_context.device, s, nullptr);
		for (auto s : m_renderFinishedSemaphores) vkDestroySemaphore(m_context.device, s, nullptr);
		for (auto f : m_inFlightFences) vkDestroyFence(m_context.device, f, nullptr);
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
	desc.format = GetDesc().swapchainFormat;
	desc.usage = dy::RHI::TextureUsage::RenderTarget;

	if (m_backBuffer == nullptr) {
		m_backBuffer = new VulkanTexture(desc);
		return;
	}

	static_cast<VulkanTexture*>(m_backBuffer)->UpdateMetadata(desc.width, desc.height, desc.format);
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
