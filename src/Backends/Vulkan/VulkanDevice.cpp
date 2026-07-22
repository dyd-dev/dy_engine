#include "VulkanDevice.h"
#include "VulkanCommandList.h"
#include "VulkanContext.h"
#include "VulkanPipeline.h"
#include "VulkanResources.h"
#include "VulkanSwapchain.h"
#include "RHI/IBuffer.h"
#include "RHI/GraphicsPipeline.h"
#include "RHI/IResourceSet.h"
#include "RHI/ISampler.h"
#include "RHI/ITexture.h"
#include <GLFW/glfw3.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <cstdio>
#include <fstream>
#include <memory>
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
	const char* VkResultToString(VkResult result);
	constexpr const char* kValidationLayerName = "VK_LAYER_KHRONOS_validation";
	constexpr const uint32_t kFallbackTextureWidth = 2;
	constexpr const uint32_t kFallbackTextureHeight = 2;

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
#if defined(_DEBUG)
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
		case dy::RHI::Format::B8G8R8A8_UNORM: return VK_FORMAT_B8G8R8A8_UNORM;
		case dy::RHI::Format::R8G8B8A8_UNORM_SRGB: return VK_FORMAT_R8G8B8A8_SRGB;
		case dy::RHI::Format::B8G8R8A8_UNORM_SRGB: return VK_FORMAT_B8G8R8A8_SRGB;
		case dy::RHI::Format::R16G16B16A16_FLOAT: return VK_FORMAT_R16G16B16A16_SFLOAT;
		case dy::RHI::Format::R32G32B32A32_FLOAT: return VK_FORMAT_R32G32B32A32_SFLOAT;
		case dy::RHI::Format::R32_UINT: return VK_FORMAT_R32_UINT;
		case dy::RHI::Format::D32_FLOAT: return VK_FORMAT_D32_SFLOAT;
		case dy::RHI::Format::D24_UNORM_S8_UINT: return VK_FORMAT_D24_UNORM_S8_UINT;
		default: return VK_FORMAT_UNDEFINED;
		}
	}

	VkImageUsageFlags ToVkImageUsage(dy::RHI::TextureUsage usage)
	{
		VkImageUsageFlags flags = 0;
		if (HasTextureUsage(usage, dy::RHI::TextureUsage::ShaderResource)) {
			flags |= VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
		}
		if (HasTextureUsage(usage, dy::RHI::TextureUsage::RenderTarget)) {
			flags |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
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
				vkMapMemory(context.device, stagingMemory, 0, size, 0, &data);
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
		VkFormat m_vkFormat = VK_FORMAT_UNDEFINED;
		VkImageAspectFlags m_aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		VkImageLayout m_imageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		uint32_t m_width = 0;
		uint32_t m_height = 0;
		dy::RHI::Format m_format = dy::RHI::Format::Unknown;
	};

	class VulkanSampler final : public dy::RHI::ISampler
	{
	public:
		VulkanSampler(VkDevice device, const dy::RHI::SamplerDesc& desc)
			: dy::RHI::ISampler(desc), m_device(device)
		{
			VkSamplerCreateInfo info{};
			info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
			info.minFilter = desc.minFilter == dy::RHI::SamplerFilter::Linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
			info.magFilter = desc.magFilter == dy::RHI::SamplerFilter::Linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
			info.mipmapMode = desc.mipFilter == dy::RHI::SamplerFilter::Linear ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
			const dy::RHI::SamplerAddressMode sourceModes[] = { desc.addressU, desc.addressV, desc.addressW };
			VkSamplerAddressMode nativeModes[3] = {};
			for(uint32_t index = 0; index < 3; ++index)
			{
				switch(sourceModes[index])
				{
				case dy::RHI::SamplerAddressMode::Repeat: nativeModes[index] = VK_SAMPLER_ADDRESS_MODE_REPEAT; break;
				case dy::RHI::SamplerAddressMode::MirroredRepeat: nativeModes[index] = VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT; break;
				case dy::RHI::SamplerAddressMode::ClampToEdge: nativeModes[index] = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE; break;
				}
			}
			info.addressModeU = nativeModes[0];
			info.addressModeV = nativeModes[1];
			info.addressModeW = nativeModes[2];
			info.minLod = desc.minLod;
			info.maxLod = desc.maxLod;
			if(vkCreateSampler(device, &info, nullptr, &m_sampler) != VK_SUCCESS)
				throw std::runtime_error("failed to create Vulkan sampler");
		}

		~VulkanSampler() override
		{
			if(m_sampler != VK_NULL_HANDLE) vkDestroySampler(m_device, m_sampler, nullptr);
		}

		VkSampler GetHandle() const { return m_sampler; }

	private:
		VkDevice m_device = VK_NULL_HANDLE;
		VkSampler m_sampler = VK_NULL_HANDLE;
	};

	class VulkanPipelineState final : public dy::RHI::IPipelineState
	{
	public:
		VulkanPipelineState(
			const VulkanContext& context,
			const dy::RHI::GraphicsPipelineDesc& desc)
			: m_device(context.device)
		{
			m_desc = desc;
			if((desc.pipelineLayout.resourceBindingCount > 0 && desc.pipelineLayout.resourceBindings == nullptr) ||
				(desc.pipelineLayout.inlineConstantRangeCount > 0 && desc.pipelineLayout.inlineConstantRanges == nullptr))
				throw std::runtime_error("invalid Vulkan pipeline layout");
			if(desc.pipelineLayout.resourceBindingCount > 0)
				m_resourceBindings.assign(
					desc.pipelineLayout.resourceBindings,
					desc.pipelineLayout.resourceBindings + desc.pipelineLayout.resourceBindingCount);
			if(desc.pipelineLayout.inlineConstantRangeCount > 0)
				m_inlineConstantRanges.assign(
					desc.pipelineLayout.inlineConstantRanges,
					desc.pipelineLayout.inlineConstantRanges + desc.pipelineLayout.inlineConstantRangeCount);
			if(desc.inputAssembly.vertexBindingCount > 0)
			{
				m_vertexBindings.assign(
					desc.inputAssembly.vertexBindings,
					desc.inputAssembly.vertexBindings + desc.inputAssembly.vertexBindingCount);
			}
			if(desc.inputAssembly.vertexAttributeCount > 0)
			{
				m_vertexAttributes.assign(
					desc.inputAssembly.vertexAttributes,
					desc.inputAssembly.vertexAttributes + desc.inputAssembly.vertexAttributeCount);
			}
			if(desc.colorAttachmentCount > 0)
			{
				m_colorAttachments.assign(
					desc.colorAttachments,
					desc.colorAttachments + desc.colorAttachmentCount);
			}
			m_desc.inputAssembly.vertexBindings = m_vertexBindings.empty() ? nullptr : m_vertexBindings.data();
			m_desc.inputAssembly.vertexAttributes = m_vertexAttributes.empty() ? nullptr : m_vertexAttributes.data();
			m_desc.colorAttachments = m_colorAttachments.empty() ? nullptr : m_colorAttachments.data();
			m_desc.pipelineLayout.resourceBindings = m_resourceBindings.empty() ? nullptr : m_resourceBindings.data();
			m_desc.pipelineLayout.inlineConstantRanges = m_inlineConstantRanges.empty() ? nullptr : m_inlineConstantRanges.data();

			uint32_t setCount = 0;
			for(uint32_t bindingIndex = 0; bindingIndex < m_resourceBindings.size(); ++bindingIndex)
			{
				const dy::RHI::ResourceBindingDesc& binding = m_resourceBindings[bindingIndex];
				if(binding.arrayCount == 0 || binding.stages == dy::RHI::ShaderStageFlags::None)
					throw std::runtime_error("invalid Vulkan resource binding");
				for(uint32_t previous = 0; previous < bindingIndex; ++previous)
				{
					const dy::RHI::ResourceBindingDesc& other = m_resourceBindings[previous];
					if(other.set == binding.set && other.binding == binding.binding)
						throw std::runtime_error("duplicate Vulkan resource binding");
				}
				setCount = std::max(setCount, binding.set + 1u);
			}
			m_descriptorSetLayouts.resize(setCount, VK_NULL_HANDLE);
			for(uint32_t set = 0; set < setCount; ++set)
			{
				std::vector<VkDescriptorSetLayoutBinding> nativeBindings;
				for(const dy::RHI::ResourceBindingDesc& binding : m_resourceBindings)
				{
					if(binding.set != set) continue;
					VkDescriptorSetLayoutBinding native{};
					native.binding = binding.binding;
					native.descriptorCount = binding.arrayCount;
					switch(binding.type)
					{
					case dy::RHI::ResourceType::ConstantBuffer: native.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; break;
					case dy::RHI::ResourceType::StorageBuffer: native.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; break;
					case dy::RHI::ResourceType::TextureSampler: native.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; break;
					}
					if((binding.stages & dy::RHI::ShaderStageFlags::Vertex) != dy::RHI::ShaderStageFlags::None) native.stageFlags |= VK_SHADER_STAGE_VERTEX_BIT;
					if((binding.stages & dy::RHI::ShaderStageFlags::Fragment) != dy::RHI::ShaderStageFlags::None) native.stageFlags |= VK_SHADER_STAGE_FRAGMENT_BIT;
					nativeBindings.push_back(native);
				}
				VkDescriptorSetLayoutCreateInfo layoutInfo{};
				layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
				layoutInfo.bindingCount = static_cast<uint32_t>(nativeBindings.size());
				layoutInfo.pBindings = nativeBindings.data();
				if(vkCreateDescriptorSetLayout(m_device, &layoutInfo, nullptr, &m_descriptorSetLayouts[set]) != VK_SUCCESS)
					throw std::runtime_error("failed to create Vulkan descriptor set layout");
			}

			std::vector<VkPushConstantRange> nativeRanges;
			for(const dy::RHI::InlineConstantRangeDesc& range : m_inlineConstantRanges)
			{
				if(range.size == 0 || range.stages == dy::RHI::ShaderStageFlags::None)
					throw std::runtime_error("invalid Vulkan inline constant range");
				VkPushConstantRange native{};
				native.offset = range.offset;
				native.size = range.size;
				if((range.stages & dy::RHI::ShaderStageFlags::Vertex) != dy::RHI::ShaderStageFlags::None) native.stageFlags |= VK_SHADER_STAGE_VERTEX_BIT;
				if((range.stages & dy::RHI::ShaderStageFlags::Fragment) != dy::RHI::ShaderStageFlags::None) native.stageFlags |= VK_SHADER_STAGE_FRAGMENT_BIT;
				nativeRanges.push_back(native);
			}
			VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
			pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
			pipelineLayoutInfo.setLayoutCount = static_cast<uint32_t>(m_descriptorSetLayouts.size());
			pipelineLayoutInfo.pSetLayouts = m_descriptorSetLayouts.data();
			pipelineLayoutInfo.pushConstantRangeCount = static_cast<uint32_t>(nativeRanges.size());
			pipelineLayoutInfo.pPushConstantRanges = nativeRanges.data();
			if(vkCreatePipelineLayout(m_device, &pipelineLayoutInfo, nullptr, &m_pipelineLayout) != VK_SUCCESS)
				throw std::runtime_error("failed to create Vulkan pipeline layout");
			m_pipelineCache.reserve(4);
		}

		~VulkanPipelineState() override
		{
			for (PipelineCacheEntry& entry : m_pipelineCache) {
				entry.pipeline.Cleanup(m_device);
			}
			m_pipelineCache.clear();
			if(m_pipelineLayout != VK_NULL_HANDLE) vkDestroyPipelineLayout(m_device, m_pipelineLayout, nullptr);
			for(VkDescriptorSetLayout layout : m_descriptorSetLayouts)
				if(layout != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(m_device, layout, nullptr);
		}

		const VulkanPipeline* GetPipelineForRenderPass(
			const VulkanContext& context,
			VkRenderPass renderPass,
			VkExtent2D extent) const
		{
			for (const PipelineCacheEntry& entry : m_pipelineCache) {
				if (entry.renderPass == renderPass) return &entry.pipeline;
			}

			try {
				PipelineCacheEntry entry = {};
				entry.renderPass = renderPass;
				entry.pipeline.Initialize(context, renderPass, extent, m_pipelineLayout, m_desc);
				m_pipelineCache.push_back(std::move(entry));
				return &m_pipelineCache.back().pipeline;
			} catch (const std::exception& e) {
				SDL_Log("Vulkan pipeline variant creation failed: %s", e.what());
				return nullptr;
			}
		}

		VkPipelineLayout GetLayout() const { return m_pipelineLayout; }
		const std::vector<VkDescriptorSetLayout>& GetDescriptorSetLayouts() const { return m_descriptorSetLayouts; }
		const std::vector<dy::RHI::ResourceBindingDesc>& GetResourceBindings() const { return m_resourceBindings; }
		const std::vector<dy::RHI::InlineConstantRangeDesc>& GetInlineConstantRanges() const { return m_inlineConstantRanges; }
		const std::vector<dy::RHI::VertexBindingDesc>& GetVertexBindings() const { return m_vertexBindings; }
		void RemovePipelineForRenderPass(VkRenderPass renderPass)
		{
			for (auto entry = m_pipelineCache.begin(); entry != m_pipelineCache.end(); ++entry) {
				if (entry->renderPass != renderPass) continue;
				entry->pipeline.Cleanup(m_device);
				m_pipelineCache.erase(entry);
				return;
			}
		}

	private:
		struct PipelineCacheEntry
		{
			VkRenderPass renderPass = VK_NULL_HANDLE;
			VulkanPipeline pipeline;
		};

		VkDevice m_device = VK_NULL_HANDLE;
		dy::RHI::GraphicsPipelineDesc m_desc = {};
		std::vector<dy::RHI::VertexBindingDesc> m_vertexBindings;
		std::vector<dy::RHI::VertexAttributeDesc> m_vertexAttributes;
		std::vector<dy::RHI::ColorAttachmentDesc> m_colorAttachments;
		std::vector<dy::RHI::ResourceBindingDesc> m_resourceBindings;
		std::vector<dy::RHI::InlineConstantRangeDesc> m_inlineConstantRanges;
		std::vector<VkDescriptorSetLayout> m_descriptorSetLayouts;
		VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
		mutable std::vector<PipelineCacheEntry> m_pipelineCache;
	};

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
		VulkanBuffer(const VulkanContext& context, const dy::RHI::BufferDesc& desc)
			: dy::RHI::IBuffer(desc), m_device(context.device)
		{
			VulkanResources::CreateBuffer(
				context,
				desc.size,
				ToVkBufferUsage(desc.usage),
				VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
				m_buffer,
				m_memory
			);
		}

		~VulkanBuffer() override
		{
			if (m_mapped != nullptr) vkUnmapMemory(m_device, m_memory);
			if (m_buffer != VK_NULL_HANDLE) vkDestroyBuffer(m_device, m_buffer, nullptr);
			if (m_memory != VK_NULL_HANDLE) vkFreeMemory(m_device, m_memory, nullptr);
		}

		void* Map(uint32_t offset) override
		{
			if (m_mapped != nullptr) return static_cast<uint8_t*>(m_mapped) + offset;
			vkMapMemory(m_device, m_memory, offset, VK_WHOLE_SIZE, 0, &m_mapped);
			return m_mapped;
		}

		void Unmap() override
		{
			if (m_mapped == nullptr) return;
			vkUnmapMemory(m_device, m_memory);
			m_mapped = nullptr;
		}

		VkBuffer GetHandle() const { return m_buffer; }

	private:
		VkDevice m_device = VK_NULL_HANDLE;
		VkBuffer m_buffer = VK_NULL_HANDLE;
		VkDeviceMemory m_memory = VK_NULL_HANDLE;
		void* m_mapped = nullptr;
	};

	class VulkanResourceSet final : public dy::RHI::IResourceSet
	{
	public:
		VulkanResourceSet(VkDevice device, VulkanPipelineState* pipeline)
			: m_device(device), m_pipeline(pipeline)
		{
			if(device == VK_NULL_HANDLE || pipeline == nullptr) return;
			const std::vector<VkDescriptorSetLayout>& layouts = pipeline->GetDescriptorSetLayouts();
			if(layouts.empty())
			{
				m_valid = true;
				return;
			}
			uint32_t uniformCount = 0;
			uint32_t storageCount = 0;
			uint32_t imageCount = 0;
			for(const dy::RHI::ResourceBindingDesc& binding : pipeline->GetResourceBindings())
			{
				switch(binding.type)
				{
				case dy::RHI::ResourceType::ConstantBuffer: uniformCount += binding.arrayCount; break;
				case dy::RHI::ResourceType::StorageBuffer: storageCount += binding.arrayCount; break;
				case dy::RHI::ResourceType::TextureSampler: imageCount += binding.arrayCount; break;
				}
			}
			std::vector<VkDescriptorPoolSize> poolSizes;
			if(uniformCount > 0) poolSizes.push_back({ VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, uniformCount });
			if(storageCount > 0) poolSizes.push_back({ VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, storageCount });
			if(imageCount > 0) poolSizes.push_back({ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, imageCount });
			VkDescriptorPoolCreateInfo poolInfo{};
			poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
			poolInfo.maxSets = static_cast<uint32_t>(layouts.size());
			poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
			poolInfo.pPoolSizes = poolSizes.data();
			if(vkCreateDescriptorPool(device, &poolInfo, nullptr, &m_pool) != VK_SUCCESS) return;
			m_sets.resize(layouts.size(), VK_NULL_HANDLE);
			VkDescriptorSetAllocateInfo allocateInfo{};
			allocateInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
			allocateInfo.descriptorPool = m_pool;
			allocateInfo.descriptorSetCount = static_cast<uint32_t>(layouts.size());
			allocateInfo.pSetLayouts = layouts.data();
			if(vkAllocateDescriptorSets(device, &allocateInfo, m_sets.data()) != VK_SUCCESS) return;
			m_valid = true;
		}

		~VulkanResourceSet() override
		{
			if(m_pool != VK_NULL_HANDLE) vkDestroyDescriptorPool(m_device, m_pool, nullptr);
		}

		bool IsValid() const { return m_valid; }
		VulkanPipelineState* GetPipeline() const { return m_pipeline; }
		const std::vector<VkDescriptorSet>& GetSets() const { return m_sets; }

		bool Update(const dy::RHI::ResourceSetWrite* writes, uint32_t writeCount)
		{
			if(!m_valid || (writeCount > 0 && writes == nullptr)) return false;
			for(uint32_t writeIndex = 0; writeIndex < writeCount; ++writeIndex)
			{
				const dy::RHI::ResourceSetWrite& write = writes[writeIndex];
				const dy::RHI::ResourceBindingDesc* declared = nullptr;
				for(const dy::RHI::ResourceBindingDesc& binding : m_pipeline->GetResourceBindings())
				{
					if(binding.set == write.set && binding.binding == write.binding)
					{
						declared = &binding;
						break;
					}
				}
				if(declared == nullptr || write.set >= m_sets.size() || write.arrayElement >= declared->arrayCount) return false;

				VkWriteDescriptorSet nativeWrite{};
				nativeWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
				nativeWrite.dstSet = m_sets[write.set];
				nativeWrite.dstBinding = write.binding;
				nativeWrite.dstArrayElement = write.arrayElement;
				nativeWrite.descriptorCount = 1;
				VkDescriptorBufferInfo bufferInfo{};
				VkDescriptorImageInfo imageInfo{};
				if(declared->type == dy::RHI::ResourceType::TextureSampler)
				{
					auto* texture = dynamic_cast<VulkanTexture*>(write.texture);
					auto* sampler = dynamic_cast<VulkanSampler*>(write.sampler);
					if(texture == nullptr || sampler == nullptr || write.buffer != nullptr) return false;
					imageInfo.imageView = texture->GetImageView();
					imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
					imageInfo.sampler = sampler->GetHandle();
					nativeWrite.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
					nativeWrite.pImageInfo = &imageInfo;
				}
				else
				{
					auto* buffer = dynamic_cast<VulkanBuffer*>(write.buffer);
					if(buffer == nullptr || write.texture != nullptr || write.sampler != nullptr || write.bufferOffset >= buffer->GetSize()) return false;
					const uint32_t available = buffer->GetSize() - write.bufferOffset;
					if(write.bufferSize > available) return false;
					bufferInfo.buffer = buffer->GetHandle();
					bufferInfo.offset = write.bufferOffset;
					bufferInfo.range = write.bufferSize == 0 ? available : write.bufferSize;
					nativeWrite.descriptorType = declared->type == dy::RHI::ResourceType::ConstantBuffer
						? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
						: VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
					nativeWrite.pBufferInfo = &bufferInfo;
				}
				vkUpdateDescriptorSets(m_device, 1, &nativeWrite, 0, nullptr);
			}
			return true;
		}

	private:
		VkDevice m_device = VK_NULL_HANDLE;
		VulkanPipelineState* m_pipeline = nullptr;
		VkDescriptorPool m_pool = VK_NULL_HANDLE;
		std::vector<VkDescriptorSet> m_sets;
		bool m_valid = false;
	};
}

struct VulkanDevice::Impl
{
	explicit Impl(VulkanDevice& owner);
	~Impl();

	int Initialize(const void* windowHandle, const dy::RHI::DeviceDesc& desc);
	bool BeginFrame();
	uint32_t GetCurrentFrameIndex() const { return m_currentFrameIndex; }
	dy::RHI::ICommandList* AcquireCommandList() { return m_commandList; }
	void Submit(dy::RHI::ICommandList** cmdLists, uint32_t count);
	void Present();

	dy::RHI::IBuffer* CreateBuffer(const dy::RHI::BufferDesc& desc);
	dy::RHI::IShader* CreateShader(const dy::RHI::ShaderDesc& desc);
	dy::RHI::ISampler* CreateSampler(const dy::RHI::SamplerDesc& desc);
	dy::RHI::ITexture* CreateTexture(const dy::RHI::TextureDesc& desc);
	bool UpdateTexture(dy::RHI::ITexture* texture, const void* rgba8Pixels, uint32_t rowPitch);
	dy::RHI::IPipelineState* CreateGraphicsPipeline(const dy::RHI::GraphicsPipelineDesc& desc);
	dy::RHI::IResourceSet* CreateResourceSet(dy::RHI::IPipelineState* pipeline);
	bool UpdateResourceSet(dy::RHI::IResourceSet* resourceSet, const dy::RHI::ResourceSetWrite* writes, uint32_t writeCount);
	void DestroyBuffer(dy::RHI::IBuffer* buffer);
	void DestroyShader(dy::RHI::IShader* shader);
	void DestroySampler(dy::RHI::ISampler* sampler);
	void DestroyTexture(dy::RHI::ITexture* texture);
	void DestroyPipelineState(dy::RHI::IPipelineState* pipeline);
	void DestroyResourceSet(dy::RHI::IResourceSet* resourceSet);
	[[nodiscard]] dy::RHI::ITexture* GetBackBuffer();

private:
	const dy::RHI::DeviceDesc& GetDesc() const { return m_owner.GetDesc(); }

	bool CreateInstance();
	bool CreateSurface();
	bool PickPhysicalDevice();
	bool CreateLogicalDevice();
	bool CreateSyncObjects();
	bool CreateMainRenderPass();
	bool CreateFramebuffers();
	bool CreateCommandPool();
	bool CreateCommandBuffer();
	bool CreateFallbackTexture();

	void RecreateSwapchain();
	void DestroySwapchainResources();
	void DestroyDeviceResources();

	void RecordCommandBuffer(const VulkanCommandList& commandList);
	void RecordPass(VkCommandBuffer commandBuffer, const VulkanCommandList& commandList, const VulkanCommandList::PassRecord& passRecord, uint32_t firstDraw, uint32_t drawCount);
	bool ResolvePassTarget(const VulkanCommandList::PassRecord& passRecord, VkRenderPass& renderPass, VkFramebuffer& framebuffer, VkExtent2D& extent);
	bool GetOrCreateColorFramebuffer(const std::vector<dy::RHI::ITexture*>& colorTargets, dy::RHI::ITexture* depthTarget, VkRenderPass& renderPass, VkFramebuffer& framebuffer, VkExtent2D& extent);
	bool GetOrCreateDepthOnlyFramebuffer(dy::RHI::ITexture* depthTarget, VkRenderPass& renderPass, VkFramebuffer& framebuffer, VkExtent2D& extent);
	bool CreateColorRenderPass(const std::vector<VkFormat>& colorFormats, VkFormat depthFormat, const std::vector<VkImageLayout>& colorFinalLayouts, VkImageLayout depthFinalLayout, VkRenderPass& renderPass);
	bool CreateDepthOnlyRenderPass(VkFormat depthFormat, bool shaderReadable, VkRenderPass& renderPass);
	void ReleaseRenderPassPipelines(VkRenderPass renderPass);
	void DestroyRenderTargetCache();
	void UpdateBackBufferMetadata();

	VulkanDevice& m_owner;
	VulkanContext m_context;
	VulkanSwapchain m_swapchain;
	VkRenderPass m_mainRenderPass = VK_NULL_HANDLE;

	void* m_windowHandle = nullptr;

	VkCommandPool m_commandPool = VK_NULL_HANDLE;
	std::vector<VkCommandBuffer> m_commandBuffers;

	std::vector<VkSemaphore> m_imageAvailableSemaphores;
	std::vector<VkSemaphore> m_renderFinishedSemaphores;
	std::vector<VkFence> m_inFlightFences;
	std::vector<VkFence> m_imagesInFlight;

	std::vector<VkFramebuffer> m_swapchainFramebuffers;
	std::vector<dy::RHI::IBuffer*> m_ownedBuffers;

	struct RenderTargetCacheEntry
	{
		std::vector<dy::RHI::ITexture*> colorTargets;
		dy::RHI::ITexture* depthTarget = nullptr;
		VkRenderPass renderPass = VK_NULL_HANDLE;
		VkFramebuffer framebuffer = VK_NULL_HANDLE;
		uint32_t imageIndex = 0;
		uint32_t width = 0;
		uint32_t height = 0;
		bool ownsRenderPass = true;
	};
	std::vector<RenderTargetCacheEntry> m_renderTargetCache;

	uint32_t m_maxFramesInFlight = dy::RHI::DeviceDesc{}.maxFramesInFlight;
	uint32_t m_fallbackTextureWidth = kFallbackTextureWidth;
	uint32_t m_fallbackTextureHeight = kFallbackTextureHeight;
	uint64_t m_frameAcquireTimeoutNanoseconds = dy::RHI::DeviceDesc{}.frameAcquireTimeoutNanoseconds;
	bool m_depthBiasClampSupported = false;
	bool m_fillModeNonSolidSupported = false;
	uint32_t m_currentFrameIndex = 0;
	uint32_t m_currentImageIndex = 0;
	bool m_frameReady = false;
	bool m_frameSubmitted = false;
	VulkanCommandList* m_commandList = nullptr;
	dy::RHI::ITexture* m_backBuffer = nullptr;
	dy::RHI::ITexture* m_fallbackTexture = nullptr;
	std::vector<dy::RHI::ITexture*> m_ownedTextures;
	std::vector<dy::RHI::IShader*> m_ownedShaders;
	std::vector<dy::RHI::ISampler*> m_ownedSamplers;
	std::vector<dy::RHI::IPipelineState*> m_ownedPipelineStates;
	std::vector<dy::RHI::IResourceSet*> m_ownedResourceSets;
};

VulkanDevice::VulkanDevice()
	: m_impl(std::make_unique<Impl>(*this))
{
}

VulkanDevice::~VulkanDevice() = default;

bool VulkanDevice::BeginFrame()
{
	return m_impl->BeginFrame();
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

dy::RHI::IShader* VulkanDevice::CreateShader(const dy::RHI::ShaderDesc& desc)
{
	return m_impl->CreateShader(desc);
}

dy::RHI::ISampler* VulkanDevice::CreateSampler(const dy::RHI::SamplerDesc& desc)
{
	return m_impl->CreateSampler(desc);
}

bool VulkanDevice::UpdateTexture(dy::RHI::ITexture* texture, const void* rgba8Pixels, uint32_t rowPitch)
{
	return m_impl->UpdateTexture(texture, rgba8Pixels, rowPitch);
}

dy::RHI::IPipelineState* VulkanDevice::CreateGraphicsPipeline(const dy::RHI::GraphicsPipelineDesc& desc)
{
	return m_impl->CreateGraphicsPipeline(desc);
}

dy::RHI::IResourceSet* VulkanDevice::CreateResourceSet(dy::RHI::IPipelineState* pipeline)
{
	return m_impl->CreateResourceSet(pipeline);
}

bool VulkanDevice::UpdateResourceSet(dy::RHI::IResourceSet* resourceSet, const dy::RHI::ResourceSetWrite* writes, uint32_t writeCount)
{
	return m_impl->UpdateResourceSet(resourceSet, writes, writeCount);
}

void VulkanDevice::DestroyBuffer(dy::RHI::IBuffer* buffer)
{
	m_impl->DestroyBuffer(buffer);
}

void VulkanDevice::DestroyShader(dy::RHI::IShader* shader)
{
	m_impl->DestroyShader(shader);
}

void VulkanDevice::DestroySampler(dy::RHI::ISampler* sampler)
{
	m_impl->DestroySampler(sampler);
}

void VulkanDevice::DestroyTexture(dy::RHI::ITexture* texture)
{
	m_impl->DestroyTexture(texture);
}

void VulkanDevice::DestroyPipelineState(dy::RHI::IPipelineState* pipeline)
{
	m_impl->DestroyPipelineState(pipeline);
}

void VulkanDevice::DestroyResourceSet(dy::RHI::IResourceSet* resourceSet)
{
	m_impl->DestroyResourceSet(resourceSet);
}

dy::RHI::ITexture* VulkanDevice::GetBackBuffer()
{
	return m_impl->GetBackBuffer();
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
	m_maxFramesInFlight = desc.maxFramesInFlight;
	m_fallbackTextureWidth = kFallbackTextureWidth;
	m_fallbackTextureHeight = kFallbackTextureHeight;
	m_frameAcquireTimeoutNanoseconds = desc.frameAcquireTimeoutNanoseconds;

	try {
		if (!CreateInstance()) return -1;
		if (!CreateSurface()) return -1;
		if (!PickPhysicalDevice()) return -1;
		if (!CreateLogicalDevice()) return -1;
		if (!CreateCommandPool()) return -1;

		if (!m_swapchain.Initialize(m_context, m_windowHandle, GetDesc().swapchainFormat)) return -1;
		UpdateBackBufferMetadata();

		if (!CreateFallbackTexture()) return -1;
		if (!CreateMainRenderPass()) return -1;
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
	m_ownedTextures.push_back(texture.get());
	return texture.release();
}

dy::RHI::IShader* VulkanDevice::Impl::CreateShader(const dy::RHI::ShaderDesc& desc) {
	if(desc.stage == dy::RHI::ShaderStage::Unknown ||
		desc.binary == nullptr || desc.binarySize == 0 ||
		desc.binarySize % sizeof(uint32_t) != 0 ||
		desc.entryPoint == nullptr || desc.entryPoint[0] == '\0') return nullptr;

	try {
		VulkanShader* shader = new VulkanShader(m_context.device, desc);
		m_ownedShaders.push_back(shader);
		return shader;
	} catch(const std::exception& e) {
		SDL_Log("Vulkan shader creation failed: %s", e.what());
		return nullptr;
	}
}

dy::RHI::ISampler* VulkanDevice::Impl::CreateSampler(const dy::RHI::SamplerDesc& desc) {
	if(desc.minLod > desc.maxLod ||
		static_cast<uint32_t>(desc.minFilter) > static_cast<uint32_t>(dy::RHI::SamplerFilter::Linear) ||
		static_cast<uint32_t>(desc.magFilter) > static_cast<uint32_t>(dy::RHI::SamplerFilter::Linear) ||
		static_cast<uint32_t>(desc.mipFilter) > static_cast<uint32_t>(dy::RHI::SamplerFilter::Linear) ||
		static_cast<uint32_t>(desc.addressU) > static_cast<uint32_t>(dy::RHI::SamplerAddressMode::ClampToEdge) ||
		static_cast<uint32_t>(desc.addressV) > static_cast<uint32_t>(dy::RHI::SamplerAddressMode::ClampToEdge) ||
		static_cast<uint32_t>(desc.addressW) > static_cast<uint32_t>(dy::RHI::SamplerAddressMode::ClampToEdge)) return nullptr;
	try {
		auto* sampler = new VulkanSampler(m_context.device, desc);
		m_ownedSamplers.push_back(sampler);
		return sampler;
	} catch(const std::exception& e) {
		SDL_Log("Vulkan sampler creation failed: %s", e.what());
		return nullptr;
	}
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

dy::RHI::IPipelineState* VulkanDevice::Impl::CreateGraphicsPipeline(const dy::RHI::GraphicsPipelineDesc& desc) {
	const bool hasColorAttachment = desc.colorAttachmentCount > 0;
	const bool hasDepthAttachment = desc.depthStencilFormat != dy::RHI::Format::Unknown;
	const bool hasFragmentShader = desc.fragmentShader != nullptr;
	if ((!hasColorAttachment && !hasDepthAttachment) ||
		((desc.depthStencil.depthTestEnable || desc.depthStencil.depthWriteEnable || desc.depthStencil.stencilTestEnable) && !hasDepthAttachment) ||
		(desc.depthStencil.depthWriteEnable && !desc.depthStencil.depthTestEnable) ||
		(desc.depthStencil.stencilTestEnable && desc.depthStencilFormat != dy::RHI::Format::D24_UNORM_S8_UINT)) return nullptr;
	VkPhysicalDeviceProperties deviceProperties{};
	vkGetPhysicalDeviceProperties(m_context.physicalDevice, &deviceProperties);
	if(desc.colorAttachmentCount > deviceProperties.limits.maxColorAttachments ||
		(desc.colorAttachmentCount > 0 && desc.colorAttachments == nullptr)) return nullptr;
	for(uint32_t attachmentIndex = 0; attachmentIndex < desc.colorAttachmentCount; ++attachmentIndex)
	{
		const dy::RHI::ColorAttachmentDesc& attachment = desc.colorAttachments[attachmentIndex];
		switch(attachment.format)
		{
		case dy::RHI::Format::R8G8B8A8_UNORM:
		case dy::RHI::Format::B8G8R8A8_UNORM:
		case dy::RHI::Format::R8G8B8A8_UNORM_SRGB:
		case dy::RHI::Format::B8G8R8A8_UNORM_SRGB:
		case dy::RHI::Format::R16G16B16A16_FLOAT:
		case dy::RHI::Format::R32G32B32A32_FLOAT:
			break;
		default:
			return nullptr;
		}
		if(ToVkFormat(attachment.format) == VK_FORMAT_UNDEFINED ||
			(attachment.writeMask & ~dy::RHI::ColorWriteAll) != 0 ||
			static_cast<uint32_t>(attachment.sourceColorFactor) > static_cast<uint32_t>(dy::RHI::BlendFactor::OneMinusDestinationAlpha) ||
			static_cast<uint32_t>(attachment.destinationColorFactor) > static_cast<uint32_t>(dy::RHI::BlendFactor::OneMinusDestinationAlpha) ||
			static_cast<uint32_t>(attachment.sourceAlphaFactor) > static_cast<uint32_t>(dy::RHI::BlendFactor::OneMinusDestinationAlpha) ||
			static_cast<uint32_t>(attachment.destinationAlphaFactor) > static_cast<uint32_t>(dy::RHI::BlendFactor::OneMinusDestinationAlpha) ||
			static_cast<uint32_t>(attachment.colorOp) > static_cast<uint32_t>(dy::RHI::BlendOp::Max) ||
			static_cast<uint32_t>(attachment.alphaOp) > static_cast<uint32_t>(dy::RHI::BlendOp::Max)) return nullptr;
	}
	const VulkanShader* vertexShader = dynamic_cast<const VulkanShader*>(desc.vertexShader);
	const VulkanShader* fragmentShader = dynamic_cast<const VulkanShader*>(desc.fragmentShader);
	if (vertexShader == nullptr || vertexShader->GetStage() != dy::RHI::ShaderStage::Vertex) return nullptr;
	if (hasFragmentShader &&
		(fragmentShader == nullptr || fragmentShader->GetStage() != dy::RHI::ShaderStage::Fragment)) return nullptr;
	if ((desc.inputAssembly.vertexBindingCount > 0 && desc.inputAssembly.vertexBindings == nullptr) ||
		(desc.inputAssembly.vertexAttributeCount > 0 && desc.inputAssembly.vertexAttributes == nullptr)) return nullptr;
	if (desc.rasterization.depthBiasClamp != 0.0f && !m_depthBiasClampSupported) return nullptr;
	if (desc.rasterization.fillMode == dy::RHI::FillMode::Wireframe && !m_fillModeNonSolidSupported) return nullptr;

	try {
		VulkanPipelineState* pipelineState = new VulkanPipelineState(m_context, desc);
		if (desc.colorAttachmentCount == 1 && pipelineState->GetPipelineForRenderPass(
			m_context,
			m_mainRenderPass,
			m_swapchain.GetExtent()) == nullptr) {
			delete pipelineState;
			return nullptr;
		}
		m_ownedPipelineStates.push_back(pipelineState);
		return pipelineState;
	} catch (const std::exception& e) {
		SDL_Log("Vulkan pipeline creation failed: %s", e.what());
		return nullptr;
	}
}

dy::RHI::IResourceSet* VulkanDevice::Impl::CreateResourceSet(dy::RHI::IPipelineState* pipeline) {
	auto* vulkanPipeline = dynamic_cast<VulkanPipelineState*>(pipeline);
	if(vulkanPipeline == nullptr) return nullptr;
	auto* resourceSet = new VulkanResourceSet(m_context.device, vulkanPipeline);
	if(!resourceSet->IsValid()) {
		delete resourceSet;
		return nullptr;
	}
	m_ownedResourceSets.push_back(resourceSet);
	return resourceSet;
}

bool VulkanDevice::Impl::UpdateResourceSet(dy::RHI::IResourceSet* resourceSet, const dy::RHI::ResourceSetWrite* writes, uint32_t writeCount) {
	auto* vulkanSet = dynamic_cast<VulkanResourceSet*>(resourceSet);
	return vulkanSet != nullptr && vulkanSet->Update(writes, writeCount);
}

void VulkanDevice::Impl::DestroyTexture(dy::RHI::ITexture* texture) {
	if (!texture || texture == m_backBuffer) return;
	const auto it = std::find(m_ownedTextures.begin(), m_ownedTextures.end(), texture);
	if (it != m_ownedTextures.end()) {
		vkDeviceWaitIdle(m_context.device);
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

void VulkanDevice::Impl::DestroyShader(dy::RHI::IShader* shader) {
	if(!shader) return;
	const auto it = std::find(m_ownedShaders.begin(), m_ownedShaders.end(), shader);
	if(it != m_ownedShaders.end()) {
		delete *it;
		m_ownedShaders.erase(it);
	}
}

void VulkanDevice::Impl::DestroySampler(dy::RHI::ISampler* sampler) {
	if(!sampler) return;
	const auto it = std::find(m_ownedSamplers.begin(), m_ownedSamplers.end(), sampler);
	if(it != m_ownedSamplers.end()) {
		delete *it;
		m_ownedSamplers.erase(it);
	}
}

void VulkanDevice::Impl::DestroyResourceSet(dy::RHI::IResourceSet* resourceSet) {
	if(!resourceSet) return;
	const auto it = std::find(m_ownedResourceSets.begin(), m_ownedResourceSets.end(), resourceSet);
	if(it != m_ownedResourceSets.end()) {
		delete *it;
		m_ownedResourceSets.erase(it);
	}
}

dy::RHI::ITexture* VulkanDevice::Impl::GetBackBuffer() {
	return m_backBuffer;
}

dy::RHI::IBuffer* VulkanDevice::Impl::CreateBuffer(const dy::RHI::BufferDesc& desc) {
	VulkanBuffer* buffer = new VulkanBuffer(m_context, desc);
	m_ownedBuffers.push_back(buffer);
	return buffer;
}

void VulkanDevice::Impl::DestroyBuffer(dy::RHI::IBuffer* buffer) {
	if (!buffer) return;
	const auto it = std::find(m_ownedBuffers.begin(), m_ownedBuffers.end(), buffer);
	if (it == m_ownedBuffers.end()) return;
	vkDeviceWaitIdle(m_context.device);
	delete *it;
	m_ownedBuffers.erase(it);
}

bool VulkanDevice::Impl::BeginFrame() {
	m_frameReady = false;
	m_frameSubmitted = false;

	if (!m_context.device || m_swapchain.GetHandle() == VK_NULL_HANDLE || m_commandList == nullptr) return false;

	if (vkWaitForFences(m_context.device, 1, &m_inFlightFences[m_currentFrameIndex], VK_TRUE, UINT64_MAX) != VK_SUCCESS) {
		return false;
	}

	const VkResult acquireResult = vkAcquireNextImageKHR(
		m_context.device, m_swapchain.GetHandle(), m_frameAcquireTimeoutNanoseconds,
		m_imageAvailableSemaphores[m_currentFrameIndex],
		VK_NULL_HANDLE, &m_currentImageIndex
	);

	if (acquireResult == VK_ERROR_OUT_OF_DATE_KHR) {
		RecreateSwapchain();
		return false;
	} else if (acquireResult == VK_NOT_READY || acquireResult == VK_TIMEOUT) {
		return false;
	} else if (acquireResult != VK_SUCCESS && acquireResult != VK_SUBOPTIMAL_KHR) {
		SDL_Log("Failed to acquire swapchain image: %s (%d)", VkResultToString(acquireResult), static_cast<int>(acquireResult));
		return false;
	}

	if (m_currentImageIndex >= m_imagesInFlight.size()) {
		SDL_Log("Swapchain image index %u is out of tracked range %zu. Recreating swapchain.", m_currentImageIndex, m_imagesInFlight.size());
		RecreateSwapchain();
		return false;
	}

	if (m_imagesInFlight[m_currentImageIndex] != VK_NULL_HANDLE) {
		if (vkWaitForFences(m_context.device, 1, &m_imagesInFlight[m_currentImageIndex], VK_TRUE, UINT64_MAX) != VK_SUCCESS) {
			return false;
		}
	}
	m_imagesInFlight[m_currentImageIndex] = m_inFlightFences[m_currentFrameIndex];

	if (vkResetFences(m_context.device, 1, &m_inFlightFences[m_currentFrameIndex]) != VK_SUCCESS) {
		return false;
	}
	m_commandList->Begin();
	m_frameReady = true;
	return true;
}

void VulkanDevice::Impl::Submit(dy::RHI::ICommandList** cmdLists, uint32_t count) {
	if (!m_frameReady || count == 0 || !cmdLists || !cmdLists[0]) return;

	VulkanCommandList* vulkanCmd = static_cast<VulkanCommandList*>(cmdLists[0]);
	RecordCommandBuffer(*vulkanCmd);

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

	if (vkQueueSubmit(m_context.graphicsQueue, 1, &submitInfo, m_inFlightFences[m_currentFrameIndex]) != VK_SUCCESS) {
		SDL_Log("failed to submit draw command buffer!");
		return;
	}

	m_frameSubmitted = true;
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
	if (presentResult == VK_ERROR_OUT_OF_DATE_KHR || presentResult == VK_SUBOPTIMAL_KHR) {
		RecreateSwapchain();
	} else if (presentResult != VK_SUCCESS) {
		SDL_Log("Failed to present swapchain image: %s (%d)", VkResultToString(presentResult), static_cast<int>(presentResult));
	}

	m_currentFrameIndex = (m_currentFrameIndex + 1) % m_maxFramesInFlight;
}

bool VulkanDevice::Impl::CreateInstance() {
	uint32_t extensionCount = 0;
	const char** extensions = glfwGetRequiredInstanceExtensions(&extensionCount);
	std::vector<const char*> enabledExtensions;
	if (extensions != nullptr) {
		enabledExtensions.assign(extensions, extensions + extensionCount);
	}
	std::vector<const char*> enabledLayers;
	if (IsValidationEnabled()) enabledLayers.push_back(kValidationLayerName);

	VkApplicationInfo appInfo{};
	appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
	appInfo.pApplicationName = "dy_engine Vulkan";
	appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
	appInfo.pEngineName = "dy_engine";
	appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
	appInfo.apiVersion = VK_API_VERSION_1_1;

	VkInstanceCreateInfo createInfo{};
	createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
	createInfo.pApplicationInfo = &appInfo;
	createInfo.enabledLayerCount = static_cast<uint32_t>(enabledLayers.size());
	createInfo.ppEnabledLayerNames = enabledLayers.empty() ? nullptr : enabledLayers.data();
	createInfo.enabledExtensionCount = static_cast<uint32_t>(enabledExtensions.size());
	createInfo.ppEnabledExtensionNames = enabledExtensions.data();

	const VkResult result = vkCreateInstance(&createInfo, nullptr, &m_context.instance);
	if (result != VK_SUCCESS) {
		SDL_Log("Failed to create Vulkan instance: %s (%d)", VkResultToString(result), static_cast<int>(result));
		return false;
	}

	return true;
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
	vkEnumeratePhysicalDevices(m_context.instance, &deviceCount, nullptr);
	std::vector<VkPhysicalDevice> devices(deviceCount);
	vkEnumeratePhysicalDevices(m_context.instance, &deviceCount, devices.data());

	for (VkPhysicalDevice device : devices) {
		VkPhysicalDeviceProperties properties{};
		vkGetPhysicalDeviceProperties(device, &properties);
		if (properties.apiVersion < VK_API_VERSION_1_1) continue;

		uint32_t count = 0; vkGetPhysicalDeviceQueueFamilyProperties(device, &count, nullptr);
		std::vector<VkQueueFamilyProperties> families(count); vkGetPhysicalDeviceQueueFamilyProperties(device, &count, families.data());
		
		VulkanContext::QueueFamilyIndices indices;
		for (uint32_t i = 0; i < count; ++i) {
			if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) indices.graphicsFamily = i;
			VkBool32 presentSupport = false; vkGetPhysicalDeviceSurfaceSupportKHR(device, i, m_context.surface, &presentSupport);
			if (presentSupport) indices.presentFamily = i;
			if (indices.IsComplete()) break;
		}

		auto swapchainSupport = VulkanSwapchain::QuerySwapchainSupport(device, m_context.surface);
		if (indices.IsComplete() && !swapchainSupport.formats.empty() && !swapchainSupport.presentModes.empty()) {
			m_context.physicalDevice = device;
			m_context.queueIndices = indices;
			return true;
		}
	}
	return false;
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
	deviceFeatures.depthBiasClamp = supportedFeatures.depthBiasClamp;
	deviceFeatures.fillModeNonSolid = supportedFeatures.fillModeNonSolid;
	m_depthBiasClampSupported = supportedFeatures.depthBiasClamp == VK_TRUE;
	m_fillModeNonSolidSupported = supportedFeatures.fillModeNonSolid == VK_TRUE;
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

void VulkanDevice::Impl::RecordCommandBuffer(const VulkanCommandList& commandList) {
	VkCommandBuffer commandBuffer = m_commandBuffers[m_currentFrameIndex];
	vkResetCommandBuffer(commandBuffer, 0);
	VkCommandBufferBeginInfo beginInfo{};
	beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	vkBeginCommandBuffer(commandBuffer, &beginInfo);

	const uint32_t passCount = static_cast<uint32_t>(commandList.m_passRecords.size());
	const uint32_t totalDrawCount = static_cast<uint32_t>(commandList.m_drawCalls.size());
	for (uint32_t passIndex = 0; passIndex < passCount; ++passIndex) {
		const VulkanCommandList::PassRecord& passRecord = commandList.m_passRecords[passIndex];
		const uint32_t firstDraw = std::min(passRecord.firstDraw, totalDrawCount);
		const uint32_t nextDraw = passIndex + 1 < passCount
			? std::min(commandList.m_passRecords[passIndex + 1].firstDraw, totalDrawCount)
			: totalDrawCount;
		RecordPass(commandBuffer, commandList, passRecord, firstDraw, nextDraw - firstDraw);
	}

	vkEndCommandBuffer(commandBuffer);
}

void VulkanDevice::Impl::RecordPass(
	VkCommandBuffer commandBuffer,
	const VulkanCommandList& commandList,
	const VulkanCommandList::PassRecord& passRecord,
	uint32_t firstDraw,
	uint32_t drawCount)
{
	VkRenderPass renderPass = VK_NULL_HANDLE;
	VkFramebuffer framebuffer = VK_NULL_HANDLE;
	VkExtent2D renderExtent = {};
	if (!ResolvePassTarget(passRecord, renderPass, framebuffer, renderExtent)) return;

	std::vector<VkClearValue> clearValues(passRecord.renderTargets.size() + (passRecord.depthStencil != nullptr ? 1u : 0u));
	for(size_t attachmentIndex = 0; attachmentIndex < passRecord.renderTargets.size(); ++attachmentIndex)
	{
		const std::array<float, 4>& color = passRecord.clearColors[attachmentIndex];
		clearValues[attachmentIndex].color = { { color[0], color[1], color[2], color[3] } };
	}
	if(passRecord.depthStencil != nullptr)
	{
		clearValues.back().depthStencil = { passRecord.clearDepth, 0 };
	}
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
	const VulkanResourceSet* currentResourceSet = nullptr;
	const uint32_t drawEnd = firstDraw + drawCount;
	for (uint32_t drawIndex = firstDraw; drawIndex < drawEnd; ++drawIndex) {
		const VulkanCommandList::DrawCall& drawCall = commandList.m_drawCalls[drawIndex];
		const VulkanPipelineState* pipelineState = dynamic_cast<const VulkanPipelineState*>(drawCall.pipelineState);
		if (pipelineState == nullptr) continue;
		const VulkanPipeline* pipeline = pipelineState->GetPipelineForRenderPass(
			m_context,
			renderPass,
			renderExtent);
		if (pipeline == nullptr) continue;

		if (currentPipeline != pipeline->GetPipeline()) {
			vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline->GetPipeline());
			currentPipeline = pipeline->GetPipeline();
			currentResourceSet = nullptr;
		}

		const auto* resourceSet = dynamic_cast<const VulkanResourceSet*>(drawCall.resourceSet);
		if(resourceSet != nullptr && resourceSet->GetPipeline() == pipelineState && resourceSet != currentResourceSet)
		{
			const std::vector<VkDescriptorSet>& sets = resourceSet->GetSets();
			if(!sets.empty())
				vkCmdBindDescriptorSets(
					commandBuffer,
					VK_PIPELINE_BIND_POINT_GRAPHICS,
					pipelineState->GetLayout(),
					0,
					static_cast<uint32_t>(sets.size()),
					sets.data(),
					0,
					nullptr);
			currentResourceSet = resourceSet;
		}

		VkViewport viewport{};
		if (drawCall.hasViewport) {
			viewport.x = drawCall.viewport.x;
			viewport.y = drawCall.viewport.y + drawCall.viewport.height;
			viewport.width = drawCall.viewport.width;
			viewport.height = -drawCall.viewport.height;
			viewport.minDepth = drawCall.viewport.minDepth;
			viewport.maxDepth = drawCall.viewport.maxDepth;
		} else {
			viewport.x = 0.0f;
			viewport.y = static_cast<float>(renderExtent.height);
			viewport.width = static_cast<float>(renderExtent.width);
			viewport.height = -static_cast<float>(renderExtent.height);
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

		if (!drawCall.inlineConstants.empty()) {
			for(const dy::RHI::InlineConstantRangeDesc& range : pipelineState->GetInlineConstantRanges())
			{
				if(drawCall.inlineConstantOffset < range.offset ||
					drawCall.inlineConstantOffset + drawCall.inlineConstants.size() > range.offset + range.size) continue;
				VkShaderStageFlags stages = 0;
				if((range.stages & dy::RHI::ShaderStageFlags::Vertex) != dy::RHI::ShaderStageFlags::None) stages |= VK_SHADER_STAGE_VERTEX_BIT;
				if((range.stages & dy::RHI::ShaderStageFlags::Fragment) != dy::RHI::ShaderStageFlags::None) stages |= VK_SHADER_STAGE_FRAGMENT_BIT;
				vkCmdPushConstants(
					commandBuffer,
					pipelineState->GetLayout(),
					stages,
					drawCall.inlineConstantOffset,
					static_cast<uint32_t>(drawCall.inlineConstants.size()),
					drawCall.inlineConstants.data());
				break;
			}
		}


		bool hasAllVertexBuffers = true;
		for(const dy::RHI::VertexBindingDesc& requiredBinding : pipelineState->GetVertexBindings())
		{
			const VulkanCommandList::VertexBufferBinding* recordedBinding = nullptr;
			for(const VulkanCommandList::VertexBufferBinding& candidate : drawCall.vertexBuffers)
			{
				if(candidate.slot != requiredBinding.slot) continue;
				recordedBinding = &candidate;
				break;
			}
			const VulkanBuffer* vertexBuffer = recordedBinding != nullptr
				? dynamic_cast<const VulkanBuffer*>(recordedBinding->buffer)
				: nullptr;
			if(vertexBuffer == nullptr)
			{
				hasAllVertexBuffers = false;
				break;
			}
			const VkBuffer nativeBuffer = vertexBuffer->GetHandle();
			const VkDeviceSize nativeOffset = recordedBinding->offset;
			vkCmdBindVertexBuffers(commandBuffer, requiredBinding.slot, 1, &nativeBuffer, &nativeOffset);
		}
		if(!hasAllVertexBuffers) continue;

		if(drawCall.indexed)
		{
			const VulkanBuffer* indexBuffer = dynamic_cast<const VulkanBuffer*>(drawCall.indexBuffer);
			if(indexBuffer == nullptr ||
				(drawCall.indexFormat != dy::RHI::Format::R16_UINT && drawCall.indexFormat != dy::RHI::Format::R32_UINT)) continue;
			const VkIndexType indexType = drawCall.indexFormat == dy::RHI::Format::R16_UINT
				? VK_INDEX_TYPE_UINT16
				: VK_INDEX_TYPE_UINT32;
			vkCmdBindIndexBuffer(commandBuffer, indexBuffer->GetHandle(), drawCall.indexOffset, indexType);
			vkCmdDrawIndexed(
				commandBuffer,
				drawCall.indexCount,
				drawCall.instanceCount,
				drawCall.firstIndex,
				drawCall.baseVertex,
				drawCall.startInstance);
		}
		else if(drawCall.vertexCount > 0)
		{
			vkCmdDraw(
				commandBuffer,
				drawCall.vertexCount,
				drawCall.instanceCount,
				drawCall.startVertex,
				drawCall.startInstance);
		}
	}

	vkCmdEndRenderPass(commandBuffer);
}

bool VulkanDevice::Impl::ResolvePassTarget(const VulkanCommandList::PassRecord& passRecord, VkRenderPass& renderPass, VkFramebuffer& framebuffer, VkExtent2D& extent) {
	if (passRecord.renderTargets.empty()) {
		if (passRecord.depthStencil == nullptr) {
			SDL_Log("Vulkan pass has no attachments.");
			return false;
		}
		return GetOrCreateDepthOnlyFramebuffer(passRecord.depthStencil, renderPass, framebuffer, extent);
	}
	for(dy::RHI::ITexture* colorTarget : passRecord.renderTargets)
	{
		if(colorTarget != nullptr) continue;
		SDL_Log("Vulkan pass color target is null.");
		return false;
	}
	if (passRecord.renderTargets.size() == 1 && passRecord.renderTargets[0] == m_backBuffer && passRecord.depthStencil == nullptr) {
		if (m_currentImageIndex >= m_swapchainFramebuffers.size()) return false;
		renderPass = m_mainRenderPass;
		framebuffer = m_swapchainFramebuffers[m_currentImageIndex];
		extent = m_swapchain.GetExtent();
		return renderPass != VK_NULL_HANDLE && framebuffer != VK_NULL_HANDLE;
	}

	return GetOrCreateColorFramebuffer(passRecord.renderTargets, passRecord.depthStencil, renderPass, framebuffer, extent);
}

bool VulkanDevice::Impl::GetOrCreateDepthOnlyFramebuffer(
	dy::RHI::ITexture* depthTarget,
	VkRenderPass& renderPass,
	VkFramebuffer& framebuffer,
	VkExtent2D& extent)
{
	for (const RenderTargetCacheEntry& entry : m_renderTargetCache) {
		if (entry.colorTargets.empty() && entry.depthTarget == depthTarget) {
			renderPass = entry.renderPass;
			framebuffer = entry.framebuffer;
			extent = { entry.width, entry.height };
			return renderPass != VK_NULL_HANDLE && framebuffer != VK_NULL_HANDLE;
		}
	}

	VulkanTexture* depthTexture = dynamic_cast<VulkanTexture*>(depthTarget);
	if (depthTexture == nullptr || depthTexture->GetImageView() == VK_NULL_HANDLE) {
		SDL_Log("Vulkan depth-only target is not a valid Vulkan texture.");
		return false;
	}
	if (!HasTextureUsage(depthTarget->GetUsage(), dy::RHI::TextureUsage::DepthStencil)) {
		SDL_Log("Vulkan depth-only target is missing DepthStencil usage.");
		return false;
	}
	if (depthTexture->GetWidth() == 0u || depthTexture->GetHeight() == 0u) return false;

	const bool shaderReadable = HasTextureUsage(depthTarget->GetUsage(), dy::RHI::TextureUsage::ShaderResource);
	VkRenderPass depthOnlyRenderPass = VK_NULL_HANDLE;
	if (!CreateDepthOnlyRenderPass(depthTexture->GetVkFormat(), shaderReadable, depthOnlyRenderPass)) {
		return false;
	}

	VkImageView attachment = depthTexture->GetImageView();
	VkFramebufferCreateInfo fbInfo{};
	fbInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
	fbInfo.renderPass = depthOnlyRenderPass;
	fbInfo.attachmentCount = 1;
	fbInfo.pAttachments = &attachment;
	fbInfo.width = depthTexture->GetWidth();
	fbInfo.height = depthTexture->GetHeight();
	fbInfo.layers = 1;

	VkFramebuffer depthOnlyFramebuffer = VK_NULL_HANDLE;
	if (vkCreateFramebuffer(m_context.device, &fbInfo, nullptr, &depthOnlyFramebuffer) != VK_SUCCESS) {
		vkDestroyRenderPass(m_context.device, depthOnlyRenderPass, nullptr);
		return false;
	}

	const VkImageLayout finalLayout = shaderReadable
		? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
		: VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
	depthTexture->SetImageLayout(finalLayout);

	RenderTargetCacheEntry entry{};
	entry.depthTarget = depthTarget;
	entry.renderPass = depthOnlyRenderPass;
	entry.framebuffer = depthOnlyFramebuffer;
	entry.width = depthTexture->GetWidth();
	entry.height = depthTexture->GetHeight();
	entry.ownsRenderPass = true;
	m_renderTargetCache.push_back(entry);

	renderPass = depthOnlyRenderPass;
	framebuffer = depthOnlyFramebuffer;
	extent = { entry.width, entry.height };
	return true;
}

bool VulkanDevice::Impl::GetOrCreateColorFramebuffer(
	const std::vector<dy::RHI::ITexture*>& colorTargets,
	dy::RHI::ITexture* depthTarget,
	VkRenderPass& renderPass,
	VkFramebuffer& framebuffer,
	VkExtent2D& extent)
{
	const bool swapchainTarget = std::find(colorTargets.begin(), colorTargets.end(), m_backBuffer) != colorTargets.end();
	const uint32_t imageIndex = swapchainTarget ? m_currentImageIndex : 0u;
	for (const RenderTargetCacheEntry& entry : m_renderTargetCache) {
		if (entry.colorTargets == colorTargets &&
			entry.depthTarget == depthTarget &&
			(!swapchainTarget || entry.imageIndex == imageIndex)) {
			renderPass = entry.renderPass;
			framebuffer = entry.framebuffer;
			extent = { entry.width, entry.height };
			return renderPass != VK_NULL_HANDLE && framebuffer != VK_NULL_HANDLE;
		}
	}

	std::vector<VkImageView> attachments;
	std::vector<VkFormat> colorFormats;
	std::vector<VkImageLayout> colorFinalLayouts;
	std::vector<VulkanTexture*> colorTextures;
	attachments.reserve(colorTargets.size() + (depthTarget != nullptr ? 1u : 0u));
	colorFormats.reserve(colorTargets.size());
	colorFinalLayouts.reserve(colorTargets.size());
	colorTextures.reserve(colorTargets.size());
	uint32_t width = 0;
	uint32_t height = 0;
	for(size_t attachmentIndex = 0; attachmentIndex < colorTargets.size(); ++attachmentIndex)
	{
		dy::RHI::ITexture* colorTarget = colorTargets[attachmentIndex];
		if(!HasTextureUsage(colorTarget->GetUsage(), dy::RHI::TextureUsage::RenderTarget))
		{
			SDL_Log("Vulkan color target is missing RenderTarget usage.");
			return false;
		}
		VkImageView colorView = VK_NULL_HANDLE;
		VkFormat colorFormat = VK_FORMAT_UNDEFINED;
		VkImageLayout finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		uint32_t targetWidth = colorTarget->GetWidth();
		uint32_t targetHeight = colorTarget->GetHeight();
		VulkanTexture* colorTexture = nullptr;
		if(colorTarget == m_backBuffer)
		{
			const auto& views = m_swapchain.GetImageViews();
			if(imageIndex >= views.size()) return false;
			colorView = views[imageIndex];
			colorFormat = m_swapchain.GetImageFormat();
			finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
			targetWidth = m_swapchain.GetExtent().width;
			targetHeight = m_swapchain.GetExtent().height;
		}
		else
		{
			colorTexture = dynamic_cast<VulkanTexture*>(colorTarget);
			if(colorTexture == nullptr || colorTexture->GetImageView() == VK_NULL_HANDLE)
			{
				SDL_Log("Vulkan color target is not a valid Vulkan texture.");
				return false;
			}
			colorView = colorTexture->GetImageView();
			colorFormat = colorTexture->GetVkFormat();
			if(HasTextureUsage(colorTarget->GetUsage(), dy::RHI::TextureUsage::ShaderResource))
				finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		}
		if(attachmentIndex == 0)
		{
			width = targetWidth;
			height = targetHeight;
		}
		else if(width != targetWidth || height != targetHeight)
		{
			SDL_Log("Vulkan color target sizes do not match.");
			return false;
		}
		attachments.push_back(colorView);
		colorFormats.push_back(colorFormat);
		colorFinalLayouts.push_back(finalLayout);
		colorTextures.push_back(colorTexture);
	}
	if (width == 0u || height == 0u) return false;

	VulkanTexture* depthTexture = nullptr;
	VkFormat depthFormat = VK_FORMAT_UNDEFINED;
	VkImageLayout depthFinalLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	if (depthTarget != nullptr) {
		depthTexture = dynamic_cast<VulkanTexture*>(depthTarget);
		if (depthTexture == nullptr || depthTexture->GetImageView() == VK_NULL_HANDLE) {
			SDL_Log("Vulkan depth target is not a valid Vulkan texture.");
			return false;
		}
		if (!HasTextureUsage(depthTarget->GetUsage(), dy::RHI::TextureUsage::DepthStencil)) {
			SDL_Log("Vulkan depth target is missing DepthStencil usage.");
			return false;
		}
		if (depthTexture->GetWidth() != width || depthTexture->GetHeight() != height) {
			SDL_Log("Vulkan color/depth target sizes do not match.");
			return false;
		}
		depthFormat = depthTexture->GetVkFormat();
		depthFinalLayout = HasTextureUsage(depthTarget->GetUsage(), dy::RHI::TextureUsage::ShaderResource)
			? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
			: VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
	}

	VkRenderPass colorRenderPass = VK_NULL_HANDLE;
	if (!CreateColorRenderPass(
		colorFormats,
		depthFormat,
		colorFinalLayouts,
		depthFinalLayout,
		colorRenderPass)) {
		return false;
	}

	if (depthTexture != nullptr) {
		attachments.push_back(depthTexture->GetImageView());
	}
	VkFramebufferCreateInfo fbInfo{};
	fbInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
	fbInfo.renderPass = colorRenderPass;
	fbInfo.attachmentCount = static_cast<uint32_t>(attachments.size());
	fbInfo.pAttachments = attachments.data();
	fbInfo.width = width;
	fbInfo.height = height;
	fbInfo.layers = 1;

	VkFramebuffer colorFramebuffer = VK_NULL_HANDLE;
	if (vkCreateFramebuffer(m_context.device, &fbInfo, nullptr, &colorFramebuffer) != VK_SUCCESS) {
		vkDestroyRenderPass(m_context.device, colorRenderPass, nullptr);
		return false;
	}

	for(size_t attachmentIndex = 0; attachmentIndex < colorTextures.size(); ++attachmentIndex)
	{
		if(colorTextures[attachmentIndex] != nullptr)
			colorTextures[attachmentIndex]->SetImageLayout(colorFinalLayouts[attachmentIndex]);
	}
	if (depthTexture != nullptr) depthTexture->SetImageLayout(depthFinalLayout);

	RenderTargetCacheEntry entry{};
	entry.colorTargets = colorTargets;
	entry.depthTarget = depthTarget;
	entry.renderPass = colorRenderPass;
	entry.framebuffer = colorFramebuffer;
	entry.imageIndex = imageIndex;
	entry.width = width;
	entry.height = height;
	entry.ownsRenderPass = true;
	m_renderTargetCache.push_back(entry);

	renderPass = colorRenderPass;
	framebuffer = colorFramebuffer;
	extent = { entry.width, entry.height };
	return true;
}

bool VulkanDevice::Impl::CreateColorRenderPass(
	const std::vector<VkFormat>& colorFormats,
	VkFormat depthFormat,
	const std::vector<VkImageLayout>& colorFinalLayouts,
	VkImageLayout depthFinalLayout,
	VkRenderPass& renderPass)
{
	if(colorFormats.empty() || colorFormats.size() != colorFinalLayouts.size()) return false;
	const bool hasDepthAttachment = depthFormat != VK_FORMAT_UNDEFINED;
	std::vector<VkAttachmentDescription> attachments;
	std::vector<VkAttachmentReference> colorAttachmentRefs;
	attachments.reserve(colorFormats.size() + (hasDepthAttachment ? 1u : 0u));
	colorAttachmentRefs.reserve(colorFormats.size());
	for(size_t attachmentIndex = 0; attachmentIndex < colorFormats.size(); ++attachmentIndex)
	{
		VkAttachmentDescription colorAttachment{};
		colorAttachment.format = colorFormats[attachmentIndex];
		colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
		colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
		colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
		colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
		colorAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		colorAttachment.finalLayout = colorFinalLayouts[attachmentIndex];
		attachments.push_back(colorAttachment);

		VkAttachmentReference colorAttachmentRef{};
		colorAttachmentRef.attachment = static_cast<uint32_t>(attachmentIndex);
		colorAttachmentRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		colorAttachmentRefs.push_back(colorAttachmentRef);
	}

	VkAttachmentDescription depthAttachment{};
	depthAttachment.format = depthFormat;
	depthAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
	depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
	depthAttachment.storeOp = depthFinalLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
		? VK_ATTACHMENT_STORE_OP_STORE
		: VK_ATTACHMENT_STORE_OP_DONT_CARE;
	depthAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	depthAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	depthAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	depthAttachment.finalLayout = depthFinalLayout;
	if(hasDepthAttachment) attachments.push_back(depthAttachment);

	VkAttachmentReference depthAttachmentRef{};
	depthAttachmentRef.attachment = static_cast<uint32_t>(colorFormats.size());
	depthAttachmentRef.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

	VkSubpassDescription subpass{};
	subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
	subpass.colorAttachmentCount = static_cast<uint32_t>(colorAttachmentRefs.size());
	subpass.pColorAttachments = colorAttachmentRefs.data();
	subpass.pDepthStencilAttachment = hasDepthAttachment ? &depthAttachmentRef : nullptr;

	std::array<VkSubpassDependency, 2> dependencies = {};
	dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
	dependencies[0].dstSubpass = 0;
	dependencies[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	dependencies[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
	dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	if (hasDepthAttachment) {
		dependencies[0].srcStageMask |= VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
		dependencies[0].dstStageMask |= VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
		dependencies[0].dstAccessMask |= VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
	}
	dependencies[0].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

	uint32_t dependencyCount = 1;
	const bool shaderReadableColor = std::find(
		colorFinalLayouts.begin(),
		colorFinalLayouts.end(),
		VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) != colorFinalLayouts.end();
	const bool shaderReadableDepth = depthFinalLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	if (shaderReadableColor || shaderReadableDepth) {
		dependencies[1].srcSubpass = 0;
		dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
		dependencies[1].srcStageMask = shaderReadableColor ? VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT : 0;
		dependencies[1].dstStageMask = VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
		dependencies[1].srcAccessMask = shaderReadableColor ? VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT : 0;
		dependencies[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
		if (shaderReadableDepth) {
			dependencies[1].srcStageMask |= VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
			dependencies[1].srcAccessMask |= VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
		}
		dependencies[1].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;
		dependencyCount = 2;
	}

	VkRenderPassCreateInfo renderPassInfo{};
	renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
	renderPassInfo.attachmentCount = static_cast<uint32_t>(attachments.size());
	renderPassInfo.pAttachments = attachments.data();
	renderPassInfo.subpassCount = 1;
	renderPassInfo.pSubpasses = &subpass;
	renderPassInfo.dependencyCount = dependencyCount;
	renderPassInfo.pDependencies = dependencies.data();

	return vkCreateRenderPass(m_context.device, &renderPassInfo, nullptr, &renderPass) == VK_SUCCESS;
}

bool VulkanDevice::Impl::CreateDepthOnlyRenderPass(VkFormat depthFormat, bool shaderReadable, VkRenderPass& renderPass) {
	VkAttachmentDescription depthAttachment{};
	depthAttachment.format = depthFormat;
	depthAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
	depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
	depthAttachment.storeOp = shaderReadable ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE;
	depthAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	depthAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	depthAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	depthAttachment.finalLayout = shaderReadable
		? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
		: VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

	VkAttachmentReference depthAttachmentRef{};
	depthAttachmentRef.attachment = 0;
	depthAttachmentRef.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

	VkSubpassDescription subpass{};
	subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
	subpass.colorAttachmentCount = 0;
	subpass.pColorAttachments = nullptr;
	subpass.pDepthStencilAttachment = &depthAttachmentRef;

	std::array<VkSubpassDependency, 2> dependencies = {};
	dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
	dependencies[0].dstSubpass = 0;
	dependencies[0].srcStageMask = shaderReadable
		? VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
		: VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
	dependencies[0].dstStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
	dependencies[0].srcAccessMask = shaderReadable ? VK_ACCESS_SHADER_READ_BIT : 0;
	dependencies[0].dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
	dependencies[0].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

	uint32_t dependencyCount = 1;
	if (shaderReadable) {
		dependencies[1].srcSubpass = 0;
		dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
		dependencies[1].srcStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
		dependencies[1].dstStageMask = VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
		dependencies[1].srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
		dependencies[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
		dependencies[1].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;
		dependencyCount = 2;
	}

	VkRenderPassCreateInfo renderPassInfo{};
	renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
	renderPassInfo.attachmentCount = 1;
	renderPassInfo.pAttachments = &depthAttachment;
	renderPassInfo.subpassCount = 1;
	renderPassInfo.pSubpasses = &subpass;
	renderPassInfo.dependencyCount = dependencyCount;
	renderPassInfo.pDependencies = dependencies.data();

	return vkCreateRenderPass(m_context.device, &renderPassInfo, nullptr, &renderPass) == VK_SUCCESS;
}

bool VulkanDevice::Impl::CreateMainRenderPass() {
	const std::vector<VkFormat> colorFormats = { m_swapchain.GetImageFormat() };
	const std::vector<VkImageLayout> finalLayouts = { VK_IMAGE_LAYOUT_PRESENT_SRC_KHR };
	return CreateColorRenderPass(
		colorFormats,
		VK_FORMAT_UNDEFINED,
		finalLayouts,
		VK_IMAGE_LAYOUT_UNDEFINED,
		m_mainRenderPass);
}

bool VulkanDevice::Impl::CreateFramebuffers() {
	const auto& views = m_swapchain.GetImageViews();
	m_swapchainFramebuffers.resize(views.size());
	for (size_t i = 0; i < views.size(); ++i) {
		VkImageView attachment = views[i];
		VkFramebufferCreateInfo fbInfo{};
		fbInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
		fbInfo.renderPass = m_mainRenderPass;
		fbInfo.attachmentCount = 1;
		fbInfo.pAttachments = &attachment;
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
		texture->UploadRGBA8(m_context, m_commandPool, pixels, desc.width, desc.height);

	if (!created) return false;

	delete m_fallbackTexture;
	m_fallbackTexture = texture.release();
	return true;
}

void VulkanDevice::Impl::ReleaseRenderPassPipelines(VkRenderPass renderPass) {
	if (renderPass == VK_NULL_HANDLE) return;
	for (dy::RHI::IPipelineState* pipelineState : m_ownedPipelineStates) {
		auto* vulkanPipelineState = dynamic_cast<VulkanPipelineState*>(pipelineState);
		if (vulkanPipelineState != nullptr) {
			vulkanPipelineState->RemovePipelineForRenderPass(renderPass);
		}
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
			ReleaseRenderPassPipelines(entry.renderPass);
			vkDestroyRenderPass(m_context.device, entry.renderPass, nullptr);
			entry.renderPass = VK_NULL_HANDLE;
		}
	}
	m_renderTargetCache.clear();
}

void VulkanDevice::Impl::RecreateSwapchain() {
	vkDeviceWaitIdle(m_context.device);
	DestroySwapchainResources();
	if (!m_swapchain.Initialize(m_context, m_windowHandle, GetDesc().swapchainFormat)) {
		SDL_Log("Failed to recreate Vulkan swapchain with the requested format.");
		return;
	}
	UpdateBackBufferMetadata();
	if (!CreateMainRenderPass() || !CreateFramebuffers()) {
		SDL_Log("Failed to recreate Vulkan swapchain render targets.");
		return;
	}
	m_imagesInFlight.assign(m_swapchain.GetImageCount(), VK_NULL_HANDLE);
}

void VulkanDevice::Impl::DestroySwapchainResources() {
	DestroyRenderTargetCache();
	for (auto fb : m_swapchainFramebuffers) vkDestroyFramebuffer(m_context.device, fb, nullptr);
	m_swapchainFramebuffers.clear();
	if (m_mainRenderPass != VK_NULL_HANDLE) {
		ReleaseRenderPassPipelines(m_mainRenderPass);
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
	for (dy::RHI::IResourceSet* resourceSet : m_ownedResourceSets) delete resourceSet;
	m_ownedResourceSets.clear();
	for (dy::RHI::IPipelineState* pipelineState : m_ownedPipelineStates) delete pipelineState;
	m_ownedPipelineStates.clear();
	for (dy::RHI::ISampler* sampler : m_ownedSamplers) delete sampler;
	m_ownedSamplers.clear();
	for (dy::RHI::IShader* shader : m_ownedShaders) delete shader;
	m_ownedShaders.clear();

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
		for (dy::RHI::IBuffer* buffer : m_ownedBuffers) delete buffer;
		m_ownedBuffers.clear();
		for (auto s : m_imageAvailableSemaphores) vkDestroySemaphore(m_context.device, s, nullptr);
		for (auto s : m_renderFinishedSemaphores) vkDestroySemaphore(m_context.device, s, nullptr);
		for (auto f : m_inFlightFences) vkDestroyFence(m_context.device, f, nullptr);
		if (m_commandPool != VK_NULL_HANDLE) vkDestroyCommandPool(m_context.device, m_commandPool, nullptr);
		DestroySwapchainResources();
		vkDestroyDevice(m_context.device, nullptr);
	}
	if (m_context.surface != VK_NULL_HANDLE) vkDestroySurfaceKHR(m_context.instance, m_context.surface, nullptr);
	if (m_context.instance != VK_NULL_HANDLE) vkDestroyInstance(m_context.instance, nullptr);
}

void VulkanDevice::Impl::UpdateBackBufferMetadata() {
	const VkExtent2D extent = m_swapchain.GetExtent();
	dy::RHI::TextureDesc desc{};
	desc.width = extent.width;
	desc.height = extent.height;
	desc.depthOrArraySize = 1;
	desc.mipLevels = 1;
	desc.format = m_swapchain.GetFormat();
	desc.usage = dy::RHI::TextureUsage::RenderTarget;

	if (m_backBuffer == nullptr) {
		m_backBuffer = new VulkanTexture(desc);
		return;
	}

	static_cast<VulkanTexture*>(m_backBuffer)->UpdateMetadata(desc.width, desc.height, desc.format);
}

}
