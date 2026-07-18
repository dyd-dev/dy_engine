#include "VulkanDevice.h"
#include "VulkanCommandList.h"
#include "VulkanContext.h"
#include "VulkanPipeline.h"
#include "VulkanResources.h"
#include "VulkanSwapchain.h"
#include "RHI/IBuffer.h"
#include "RHI/IPipelineState.h"
#include "RHI/ITexture.h"
#include "Graphics/RendererShaderLayout.h"
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

	struct DrawMetadataPushConstants
	{
		uint32_t firstIndex = 0;
		int32_t vertexOffset = 0;
		uint32_t firstVertex = 0;
		uint32_t padding = 0;
	};

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
			const dy::RHI::GraphicsPipelineDesc& desc,
			uint32_t pushConstantSize)
			: m_device(context.device),
			m_shadowPassEnabled(desc.enableShadowPass),
			m_bindlessTexturesEnabled(desc.enableBindlessTextures),
			m_pushConstantSize(pushConstantSize)
		{
			CopyPipelineDesc(desc);
			m_pipelineCache.reserve(4);
			if (desc.renderTargetFormat != dy::RHI::Format::Unknown &&
				GetPipelineForRenderPass(context, renderPass, extent, descriptorSetLayout, bindlessDescriptorSetLayout) == nullptr) {
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
				entry.pipeline.Initialize(context, renderPass, extent, descriptorSetLayout, m_desc, m_pushConstantSize, bindlessDescriptorSetLayout);
				m_pipelineCache.push_back(std::move(entry));
				return &m_pipelineCache.back().pipeline;
			} catch (const std::exception& e) {
				SDL_Log("Vulkan pipeline variant creation failed: %s", e.what());
				return nullptr;
			}
		}

		bool IsShadowPassEnabled() const { return m_shadowPassEnabled; }
		bool UsesBindlessTextures() const { return m_bindlessTexturesEnabled; }

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
		uint32_t m_pushConstantSize = 0;
		std::vector<uint8_t> m_vertexShader;
		std::vector<uint8_t> m_pixelShader;
		std::vector<uint8_t> m_shadowVertexShader;
		mutable std::vector<PipelineCacheEntry> m_pipelineCache;
		bool m_shadowPassEnabled = false;
		bool m_bindlessTexturesEnabled = false;
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
	dy::RHI::IPipelineState* CreateGraphicsPipeline(const dy::RHI::GraphicsPipelineDesc& desc);
	[[nodiscard]] dy::RHI::DescriptorIndex AllocateDescriptorSlot();
	void UpdateDescriptorSlot(dy::RHI::DescriptorIndex index, dy::RHI::ITexture* texture);
	void DestroyBuffer(dy::RHI::IBuffer* buffer);
	void DestroyTexture(dy::RHI::ITexture* texture);
	void DestroyPipelineState(dy::RHI::IPipelineState* pipeline);
	[[nodiscard]] dy::RHI::ITexture* GetBackBuffer();

private:
	const dy::RHI::DeviceDesc& GetDesc() const { return m_owner.GetDesc(); }

	bool CreateInstance();
	bool CreateSurface();
	bool PickPhysicalDevice();
	bool CreateLogicalDevice();
	bool CreateSyncObjects();
	bool CreateDescriptorSetLayout();
	bool CreateBindlessDescriptorSetLayout();
	bool CreateDescriptorPool();
	bool CreateDescriptorSets();
	bool CreateBindlessDescriptorSet();
	bool CreateMainRenderPass();
	bool CreateDepthResources();
	bool CreateFramebuffers();
	bool CreateCommandPool();
	bool CreateCommandBuffer();
	bool CreateFallbackTexture();

	bool CreateShadowMapResources();
	bool CreateShadowRenderPass();
	bool CreateShadowPipeline(const dy::RHI::GraphicsPipelineDesc& desc);
	void DestroyShadowResources();

	void RecreateSwapchain();
	void DestroySwapchainResources();
	void DestroyDeviceResources();

	void RecordCommandBuffer(const VulkanCommandList& commandList);
	void RecordShadowPass(VkCommandBuffer commandBuffer, const VulkanCommandList& commandList);
	void RecordPass(VkCommandBuffer commandBuffer, const VulkanCommandList& commandList, const VulkanCommandList::PassRecord& passRecord, uint32_t firstDraw, uint32_t drawCount);
	bool ResolvePassTarget(const VulkanCommandList::PassRecord& passRecord, VkRenderPass& renderPass, VkFramebuffer& framebuffer, VkExtent2D& extent);
	bool GetOrCreateOffscreenFramebuffer(dy::RHI::ITexture* colorTarget, dy::RHI::ITexture* depthTarget, VkRenderPass& renderPass, VkFramebuffer& framebuffer, VkExtent2D& extent);
	bool GetOrCreateDepthOnlyFramebuffer(dy::RHI::ITexture* depthTarget, VkRenderPass& renderPass, VkFramebuffer& framebuffer, VkExtent2D& extent);
	bool CreateOffscreenRenderPass(VkFormat colorFormat, VkFormat depthFormat, VkImageLayout colorFinalLayout, VkRenderPass& renderPass);
	bool CreateDepthOnlyRenderPass(VkFormat depthFormat, bool shaderReadable, VkRenderPass& renderPass);
	void DestroyRenderTargetCache();
	bool UpdateDrawDescriptorSets(const VulkanCommandList& commandList);
	bool UpdateDrawDescriptorSet(const VulkanCommandList::DrawCall& drawCall, uint32_t drawIndex);
	bool InitializeBindlessDescriptorSet();
	void UpdateBackBufferMetadata();
	VkFormat FindDepthFormat() const;
	bool IsDepthFormatSupported(VkFormat format) const;

	VulkanDevice& m_owner;
	VulkanContext m_context;
	VulkanSwapchain m_swapchain;
	VkRenderPass m_mainRenderPass = VK_NULL_HANDLE;

	void* m_windowHandle = nullptr;

	VkCommandPool m_commandPool = VK_NULL_HANDLE;
	std::vector<VkCommandBuffer> m_commandBuffers;

	VkFormat m_depthFormat = VK_FORMAT_UNDEFINED;

	VkDescriptorSetLayout m_descriptorSetLayout = VK_NULL_HANDLE;
	VkDescriptorSetLayout m_bindlessDescriptorSetLayout = VK_NULL_HANDLE;
	VkDescriptorPool m_descriptorPool = VK_NULL_HANDLE;
	std::vector<VkDescriptorSet> m_descriptorSets;
	VkDescriptorSet m_bindlessDescriptorSet = VK_NULL_HANDLE;

	std::vector<VkSemaphore> m_imageAvailableSemaphores;
	std::vector<VkSemaphore> m_renderFinishedSemaphores;
	std::vector<VkFence> m_inFlightFences;
	std::vector<VkFence> m_imagesInFlight;

	std::vector<VkFramebuffer> m_swapchainFramebuffers;
	std::vector<dy::RHI::IBuffer*> m_ownedBuffers;

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
	VkPipelineLayout m_shadowPipelineLayout = VK_NULL_HANDLE;
	VkPipeline m_shadowPipeline = VK_NULL_HANDLE;
	VkFormat m_shadowMapFormat = VK_FORMAT_UNDEFINED;
	uint32_t m_shadowMapResolution = 0;

	uint32_t m_maxFramesInFlight = dy::RHI::DeviceDesc{}.maxFramesInFlight;
	uint32_t m_maxDrawsPerFrame = dy::RHI::DeviceDesc{}.maxDrawsPerFrame;
	uint32_t m_maxBindlessTextures = dy::RHI::DeviceDesc{}.maxBindlessTextures;
	uint32_t m_fallbackTextureWidth = kFallbackTextureWidth;
	uint32_t m_fallbackTextureHeight = kFallbackTextureHeight;
	uint64_t m_frameAcquireTimeoutNanoseconds = dy::RHI::DeviceDesc{}.frameAcquireTimeoutNanoseconds;
	bool m_depthBiasClampSupported = false;
	dy::RHI::ShaderLayoutDesc m_shaderLayout = {};
	uint32_t m_currentFrameIndex = 0;
	uint32_t m_currentImageIndex = 0;
	bool m_frameReady = false;
	bool m_frameSubmitted = false;
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
	m_maxDrawsPerFrame = desc.maxDrawsPerFrame;
	m_maxBindlessTextures = desc.maxBindlessTextures;
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

		m_swapchain.Initialize(m_context, m_windowHandle, dy::RHI::IsSrgbFormat(GetDesc().swapchainFormat));
		UpdateBackBufferMetadata();

		m_depthFormat = FindDepthFormat();
		if (!CreateFallbackTexture()) return -1;
		if (!CreateDescriptorSetLayout()) return -1;
		if (!CreateBindlessDescriptorSetLayout()) return -1;
		if (!CreateDescriptorPool()) return -1;
		if (!CreateDescriptorSets()) return -1;
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

dy::RHI::IPipelineState* VulkanDevice::Impl::CreateGraphicsPipeline(const dy::RHI::GraphicsPipelineDesc& desc) {
	const bool hasColorAttachment = desc.renderTargetFormat != dy::RHI::Format::Unknown;
	const bool hasDepthAttachment = desc.depthStencilFormat != dy::RHI::Format::Unknown;
	if ((!hasColorAttachment && !hasDepthAttachment) || (desc.depthEnable && !hasDepthAttachment)) return nullptr;
	if (desc.vertexShader == nullptr || desc.vertexShaderSize == 0) return nullptr;
	if (desc.depthBiasClamp != 0.0f && !m_depthBiasClampSupported) return nullptr;

	try {
		if (desc.enableShadowPass) {
			if (desc.shadowMapResolution == 0) return nullptr;
			const uint32_t requestedShadowMapResolution = desc.shadowMapResolution;
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
		if (desc.enableShadowPass && m_shadowPipeline == VK_NULL_HANDLE && !CreateShadowPipeline(desc)) {
			return nullptr;
		}

		VulkanPipelineState* pipelineState = new VulkanPipelineState(
			m_context,
			m_mainRenderPass,
			m_swapchain.GetExtent(),
			m_descriptorSetLayout,
			desc.enableBindlessTextures ? m_bindlessDescriptorSetLayout : VK_NULL_HANDLE,
			desc,
			m_shaderLayout.pushConstantRangeSize);
		m_ownedPipelineStates.push_back(pipelineState);
		return pipelineState;
	} catch (const std::exception& e) {
		SDL_Log("Vulkan pipeline creation failed: %s", e.what());
		return nullptr;
	}
}

dy::RHI::DescriptorIndex VulkanDevice::Impl::AllocateDescriptorSlot() {
	if (m_nextDescriptorIndex >= m_maxBindlessTextures) return dy::RHI::INVALID_DESCRIPTOR_INDEX;
	return m_nextDescriptorIndex++;
}

void VulkanDevice::Impl::UpdateDescriptorSlot(dy::RHI::DescriptorIndex index, dy::RHI::ITexture* texture) {
	if (index == dy::RHI::INVALID_DESCRIPTOR_INDEX || index >= m_maxBindlessTextures) return;
	if (m_bindlessDescriptorSet == VK_NULL_HANDLE) return;

	const VulkanTexture* vulkanTexture = dynamic_cast<const VulkanTexture*>(texture);
	if (vulkanTexture == nullptr || vulkanTexture->GetImageView() == VK_NULL_HANDLE || vulkanTexture->GetSampler() == VK_NULL_HANDLE) {
		vulkanTexture = static_cast<const VulkanTexture*>(m_fallbackTexture);
	}
	if (vulkanTexture == nullptr || vulkanTexture->GetImageView() == VK_NULL_HANDLE || vulkanTexture->GetSampler() == VK_NULL_HANDLE) return;

	VkDescriptorImageInfo imageInfo{};
	imageInfo.sampler = vulkanTexture->GetSampler();
	imageInfo.imageView = vulkanTexture->GetImageView();
	imageInfo.imageLayout = vulkanTexture->GetImageLayout();

	VkWriteDescriptorSet write{};
	write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	write.dstSet = m_bindlessDescriptorSet;
	write.dstBinding = 0;
	write.dstArrayElement = index;
	write.descriptorCount = 1;
	write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	write.pImageInfo = &imageInfo;
	vkUpdateDescriptorSets(m_context.device, 1, &write, 0, nullptr);
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

void VulkanDevice::Impl::BeginFrame() {
	m_frameReady = false;
	m_frameSubmitted = false;
	if (m_commandList != nullptr) {
		m_commandList->Begin();
	}

	if (!m_context.device || m_swapchain.GetHandle() == VK_NULL_HANDLE) return;

	vkWaitForFences(m_context.device, 1, &m_inFlightFences[m_currentFrameIndex], VK_TRUE, UINT64_MAX);

	const VkResult acquireResult = vkAcquireNextImageKHR(
		m_context.device, m_swapchain.GetHandle(), m_frameAcquireTimeoutNanoseconds,
		m_imageAvailableSemaphores[m_currentFrameIndex],
		VK_NULL_HANDLE, &m_currentImageIndex
	);

	if (acquireResult == VK_ERROR_OUT_OF_DATE_KHR) {
		RecreateSwapchain();
		return;
	} else if (acquireResult == VK_NOT_READY || acquireResult == VK_TIMEOUT) {
		return;
	} else if (acquireResult != VK_SUCCESS && acquireResult != VK_SUBOPTIMAL_KHR) {
		SDL_Log("Failed to acquire swapchain image: %s (%d)", VkResultToString(acquireResult), static_cast<int>(acquireResult));
		return;
	}

	if (m_currentImageIndex >= m_imagesInFlight.size()) {
		SDL_Log("Swapchain image index %u is out of tracked range %zu. Recreating swapchain.", m_currentImageIndex, m_imagesInFlight.size());
		RecreateSwapchain();
		return;
	}

	if (m_imagesInFlight[m_currentImageIndex] != VK_NULL_HANDLE) {
		vkWaitForFences(m_context.device, 1, &m_imagesInFlight[m_currentImageIndex], VK_TRUE, UINT64_MAX);
	}
	m_imagesInFlight[m_currentImageIndex] = m_inFlightFences[m_currentFrameIndex];

	vkResetFences(m_context.device, 1, &m_inFlightFences[m_currentFrameIndex]);
	m_frameReady = true;
}

void VulkanDevice::Impl::Submit(dy::RHI::ICommandList** cmdLists, uint32_t count) {
	if (!m_frameReady || count == 0 || !cmdLists || !cmdLists[0]) return;

	VulkanCommandList* vulkanCmd = static_cast<VulkanCommandList*>(cmdLists[0]);
	RecordCommandBuffer(*vulkanCmd);
	if (!UpdateDrawDescriptorSets(*vulkanCmd)) {
		SDL_Log("Failed to update Vulkan draw descriptor sets.");
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
	appInfo.apiVersion = VK_API_VERSION_1_0;

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
	m_depthBiasClampSupported = supportedFeatures.depthBiasClamp == VK_TRUE;
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

	if (m_shadowRenderPass != VK_NULL_HANDLE && m_shadowPipeline != VK_NULL_HANDLE) {
		RecordShadowPass(commandBuffer, commandList);
	}

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

void VulkanDevice::Impl::RecordShadowPass(VkCommandBuffer commandBuffer, const VulkanCommandList& commandList) {
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
	vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_shadowPipeline);

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

	VkDescriptorSet currentDescriptorSet = VK_NULL_HANDLE;
	for (uint32_t drawIndex = 0; drawIndex < commandList.m_drawCalls.size(); ++drawIndex) {
		const VulkanCommandList::DrawCall& drawCall = commandList.m_drawCalls[drawIndex];
		const VulkanPipelineState* pipelineState = dynamic_cast<const VulkanPipelineState*>(drawCall.pipelineState);
		if (pipelineState == nullptr || !pipelineState->IsShadowPassEnabled()) continue;
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
		const uint32_t descriptorIndex = m_currentFrameIndex * m_maxDrawsPerFrame + descriptorSlot;
		if (descriptorIndex < m_descriptorSets.size()) {
			VkDescriptorSet descriptorSet = m_descriptorSets[descriptorIndex];
			if (descriptorSet != currentDescriptorSet) {
				vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_shadowPipelineLayout, 0, 1, &descriptorSet, 0, nullptr);
				currentDescriptorSet = descriptorSet;
			}
		}

		if (drawCall.pushConstantSize > 0) {
			vkCmdPushConstants(
				commandBuffer,
				m_shadowPipelineLayout,
				VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
				0,
				drawCall.pushConstantSize,
				drawCall.pushConstants.data());
		}

		DrawMetadataPushConstants metadata{};
		if (drawCall.pushConstantSize >= m_shaderLayout.drawMetadataPushConstantOffset + sizeof(metadata)) {
			std::memcpy(&metadata, drawCall.pushConstants.data() + m_shaderLayout.drawMetadataPushConstantOffset, sizeof(metadata));
		}
		metadata.firstIndex = drawCall.firstIndex;
		metadata.vertexOffset = drawCall.baseVertex;
		if (!drawCall.indexed) metadata.firstVertex = drawCall.startVertex;
		vkCmdPushConstants(
			commandBuffer,
			m_shadowPipelineLayout,
			VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
			m_shaderLayout.drawMetadataPushConstantOffset,
			sizeof(metadata),
			&metadata);

		if (drawCall.indexed) {
			if (drawCall.indexCount > 0) {
				vkCmdDraw(commandBuffer, drawCall.indexCount, drawCall.instanceCount, 0, drawCall.startInstance);
			}
		} else {
			vkCmdDraw(commandBuffer, drawCall.vertexCount, drawCall.instanceCount, 0, drawCall.startInstance);
		}
	}

	vkCmdEndRenderPass(commandBuffer);
	return;
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

	std::array<VkClearValue, 2> clearValues = {};
	uint32_t clearValueCount = 0;
	if (passRecord.renderTargetCount == 0 && passRecord.depthStencil != nullptr) {
		clearValues[0].depthStencil = { passRecord.clearDepth, 0 };
		clearValueCount = 1;
	} else {
		clearValues[0].color = { {
			passRecord.clearColor[0],
			passRecord.clearColor[1],
			passRecord.clearColor[2],
			passRecord.clearColor[3]
		} };
		clearValues[1].depthStencil = { passRecord.clearDepth, 0 };
		clearValueCount = 2;
	}
	VkRenderPassBeginInfo renderPassInfo{};
	renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
	renderPassInfo.renderPass = renderPass;
	renderPassInfo.framebuffer = framebuffer;
	renderPassInfo.renderArea.offset = {0, 0};
	renderPassInfo.renderArea.extent = renderExtent;
	renderPassInfo.clearValueCount = clearValueCount;
	renderPassInfo.pClearValues = clearValues.data();

	vkCmdBeginRenderPass(commandBuffer, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);

	VkPipeline currentPipeline = VK_NULL_HANDLE;
	VkPipelineLayout currentPipelineLayout = VK_NULL_HANDLE;
	VkDescriptorSet currentDescriptorSet = VK_NULL_HANDLE;
	VkDescriptorSet currentBindlessDescriptorSet = VK_NULL_HANDLE;
	const uint32_t drawEnd = firstDraw + drawCount;
	for (uint32_t drawIndex = firstDraw; drawIndex < drawEnd; ++drawIndex) {
		const VulkanCommandList::DrawCall& drawCall = commandList.m_drawCalls[drawIndex];
		const VulkanPipelineState* pipelineState = dynamic_cast<const VulkanPipelineState*>(drawCall.pipelineState);
		if (pipelineState == nullptr) continue;
		const bool usesBindlessTextures = pipelineState->UsesBindlessTextures();
		const VulkanPipeline* pipeline = pipelineState->GetPipelineForRenderPass(
			m_context,
			renderPass,
			renderExtent,
			m_descriptorSetLayout,
			usesBindlessTextures ? m_bindlessDescriptorSetLayout : VK_NULL_HANDLE);
		if (pipeline == nullptr) continue;

		if (currentPipeline != pipeline->GetPipeline()) {
			vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline->GetPipeline());
			currentPipeline = pipeline->GetPipeline();
			currentPipelineLayout = pipeline->GetLayout();
			currentDescriptorSet = VK_NULL_HANDLE;
			currentBindlessDescriptorSet = VK_NULL_HANDLE;
		}

		const uint32_t descriptorSlot = usesBindlessTextures ? 0u : drawIndex;
		const uint32_t descriptorIndex = m_currentFrameIndex * m_maxDrawsPerFrame + descriptorSlot;
		if (descriptorIndex < m_descriptorSets.size()) {
			VkDescriptorSet descriptorSet = m_descriptorSets[descriptorIndex];
			if (descriptorSet != currentDescriptorSet || pipeline->GetLayout() != currentPipelineLayout) {
				vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline->GetLayout(), 0, 1, &descriptorSet, 0, nullptr);
				currentDescriptorSet = descriptorSet;
			}
		}
		if (usesBindlessTextures && m_bindlessDescriptorSet != VK_NULL_HANDLE) {
			if (m_bindlessDescriptorSet != currentBindlessDescriptorSet || pipeline->GetLayout() != currentPipelineLayout) {
				vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline->GetLayout(), 1, 1, &m_bindlessDescriptorSet, 0, nullptr);
				currentBindlessDescriptorSet = m_bindlessDescriptorSet;
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

		if (drawCall.pushConstantSize > 0) {
			vkCmdPushConstants(
				commandBuffer,
				pipeline->GetLayout(),
				VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
				0,
				drawCall.pushConstantSize,
				drawCall.pushConstants.data()
			);
		}

		DrawMetadataPushConstants metadata{};
		if (drawCall.pushConstantSize >= m_shaderLayout.drawMetadataPushConstantOffset + sizeof(metadata)) {
			std::memcpy(&metadata, drawCall.pushConstants.data() + m_shaderLayout.drawMetadataPushConstantOffset, sizeof(metadata));
		}
		metadata.firstIndex = drawCall.firstIndex;
		metadata.vertexOffset = drawCall.baseVertex;
		if (!drawCall.indexed) metadata.firstVertex = drawCall.startVertex;
		vkCmdPushConstants(
			commandBuffer,
			pipeline->GetLayout(),
			VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
			m_shaderLayout.drawMetadataPushConstantOffset,
			sizeof(metadata),
			&metadata
		);

		// 메인 셰이더는 정점/인덱스를 storage 버퍼에서 수동 fetch한다(gl_VertexIndex 선형 →
		// indexStorage[firstIndex+id] → vertexStorage). 따라서 IA 인덱스버퍼 없이 vkCmdDraw 로
		// indexCount 개의 정점을 그린다(섀도우 패스와 동일 모델).
		const uint32_t drawVertexCount = drawCall.indexed ? drawCall.indexCount : drawCall.vertexCount;
		if (drawVertexCount > 0) {
			vkCmdDraw(commandBuffer, drawVertexCount, drawCall.instanceCount, 0, drawCall.startInstance);
		}
	}

	vkCmdEndRenderPass(commandBuffer);
}

bool VulkanDevice::Impl::ResolvePassTarget(const VulkanCommandList::PassRecord& passRecord, VkRenderPass& renderPass, VkFramebuffer& framebuffer, VkExtent2D& extent) {
	if (passRecord.renderTargetCount > 1) {
		SDL_Log("Vulkan main pass currently supports one color render target.");
		return false;
	}
	if (passRecord.renderTargetCount == 0 && passRecord.depthStencil != nullptr) {
		return GetOrCreateDepthOnlyFramebuffer(passRecord.depthStencil, renderPass, framebuffer, extent);
	}

	dy::RHI::ITexture* colorTarget = m_backBuffer;
	if (passRecord.renderTargetCount == 1 && passRecord.renderTargets[0] != nullptr) {
		colorTarget = passRecord.renderTargets[0];
	}

	if (colorTarget == nullptr || colorTarget == m_backBuffer) {
		if (passRecord.depthStencil != nullptr && passRecord.depthStencil != m_depthTexture) {
			for (const RenderTargetCacheEntry& entry : m_renderTargetCache) {
				if (entry.colorTarget == m_backBuffer &&
					entry.depthTarget == passRecord.depthStencil &&
					entry.imageIndex == m_currentImageIndex) {
					renderPass = entry.renderPass;
					framebuffer = entry.framebuffer;
					extent = { entry.width, entry.height };
					return renderPass != VK_NULL_HANDLE && framebuffer != VK_NULL_HANDLE;
				}
			}

			const VulkanTexture* depthTexture = dynamic_cast<const VulkanTexture*>(passRecord.depthStencil);
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
			entry.depthTarget = passRecord.depthStencil;
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

	return GetOrCreateOffscreenFramebuffer(colorTarget, passRecord.depthStencil, renderPass, framebuffer, extent);
}

bool VulkanDevice::Impl::GetOrCreateDepthOnlyFramebuffer(
	dy::RHI::ITexture* depthTarget,
	VkRenderPass& renderPass,
	VkFramebuffer& framebuffer,
	VkExtent2D& extent)
{
	for (const RenderTargetCacheEntry& entry : m_renderTargetCache) {
		if (entry.colorTarget == nullptr && entry.depthTarget == depthTarget) {
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
	if (!CreateOffscreenRenderPass(
		colorTexture->GetVkFormat(),
		depthTexture->GetVkFormat(),
		VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
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

	colorTexture->SetImageLayout(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

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

bool VulkanDevice::Impl::CreateDescriptorSetLayout() {
	// binding 0: Base color sampler (FS)
	// binding 1: Lighting UBO (VS+FS)
	// binding 2: Shadow map sampler (FS)
	// binding 3: ShadowMatrix UBO (lightViewProj, VS)
	// binding 4: Vertex storage buffer (VS)
	// binding 5: Index storage buffer (VS)
	// binding 6-9: Metallic/Roughness, Normal, Occlusion, Emissive samplers (FS)
	// binding 10-12: Bindless material/transform/draw storage buffers (VS+FS)
	std::array<VkDescriptorSetLayoutBinding, kMaxDescriptorBindings> bindings = {};
	auto setBinding = [&bindings](uint32_t index, uint32_t binding, VkDescriptorType type, VkShaderStageFlags stageFlags) {
		bindings[index].binding = binding;
		bindings[index].descriptorType = type;
		bindings[index].descriptorCount = 1;
		bindings[index].stageFlags = stageFlags;
	};
	setBinding(m_shaderLayout.baseColorTextureBinding, m_shaderLayout.baseColorTextureBinding, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT);
	setBinding(m_shaderLayout.lightingConstantBinding, m_shaderLayout.lightingConstantBinding, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT);
	setBinding(m_shaderLayout.shadowSamplerBinding, m_shaderLayout.shadowSamplerBinding, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT);
	setBinding(m_shaderLayout.shadowMatrixBinding, m_shaderLayout.shadowMatrixBinding, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SHADER_STAGE_VERTEX_BIT);
	setBinding(m_shaderLayout.vertexStorageBinding, m_shaderLayout.vertexStorageBinding, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_VERTEX_BIT);
	setBinding(m_shaderLayout.indexStorageBinding, m_shaderLayout.indexStorageBinding, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_VERTEX_BIT);
	setBinding(m_shaderLayout.metallicRoughnessTextureBinding, m_shaderLayout.metallicRoughnessTextureBinding, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT);
	setBinding(m_shaderLayout.normalTextureBinding, m_shaderLayout.normalTextureBinding, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT);
	setBinding(m_shaderLayout.occlusionTextureBinding, m_shaderLayout.occlusionTextureBinding, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT);
	setBinding(m_shaderLayout.emissiveTextureBinding, m_shaderLayout.emissiveTextureBinding, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT);
	setBinding(m_shaderLayout.bindlessMaterialStorageBinding, m_shaderLayout.bindlessMaterialStorageBinding, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT);
	setBinding(m_shaderLayout.bindlessTransformStorageBinding, m_shaderLayout.bindlessTransformStorageBinding, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT);
	setBinding(m_shaderLayout.bindlessDrawStorageBinding, m_shaderLayout.bindlessDrawStorageBinding, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT);
	VkDescriptorSetLayoutCreateInfo info{};
	info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	info.bindingCount = m_shaderLayout.descriptorBindingCount;
	info.pBindings = bindings.data();
	return vkCreateDescriptorSetLayout(m_context.device, &info, nullptr, &m_descriptorSetLayout) == VK_SUCCESS;
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

bool VulkanDevice::Impl::CreateDescriptorPool() {
	const uint32_t descriptorSetCount = m_maxFramesInFlight * m_maxDrawsPerFrame;
	std::array<VkDescriptorPoolSize, 3> newPoolSizes = {};
	newPoolSizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	newPoolSizes[0].descriptorCount = descriptorSetCount * m_shaderLayout.samplerDescriptorCount + m_maxBindlessTextures;
	newPoolSizes[1].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
	newPoolSizes[1].descriptorCount = descriptorSetCount * m_shaderLayout.constantBufferDescriptorCount;
	newPoolSizes[2].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	newPoolSizes[2].descriptorCount = descriptorSetCount * m_shaderLayout.storageBufferDescriptorCount;
	VkDescriptorPoolCreateInfo newInfo{};
	newInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	newInfo.maxSets = descriptorSetCount + 1u;
	newInfo.poolSizeCount = static_cast<uint32_t>(newPoolSizes.size());
	newInfo.pPoolSizes = newPoolSizes.data();
	return vkCreateDescriptorPool(m_context.device, &newInfo, nullptr, &m_descriptorPool) == VK_SUCCESS;
}
bool VulkanDevice::Impl::CreateDescriptorSets() {
	const uint32_t descriptorSetCount = m_maxFramesInFlight * m_maxDrawsPerFrame;
	std::vector<VkDescriptorSetLayout> newLayouts(descriptorSetCount, m_descriptorSetLayout);
	VkDescriptorSetAllocateInfo newAlloc{};
	newAlloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	newAlloc.descriptorPool = m_descriptorPool;
	newAlloc.descriptorSetCount = descriptorSetCount;
	newAlloc.pSetLayouts = newLayouts.data();
	m_descriptorSets.resize(descriptorSetCount);
	return vkAllocateDescriptorSets(m_context.device, &newAlloc, m_descriptorSets.data()) == VK_SUCCESS;
}

bool VulkanDevice::Impl::CreateBindlessDescriptorSet() {
	if (m_bindlessDescriptorSetLayout == VK_NULL_HANDLE) return false;

	VkDescriptorSetAllocateInfo alloc{};
	alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	alloc.descriptorPool = m_descriptorPool;
	alloc.descriptorSetCount = 1;
	alloc.pSetLayouts = &m_bindlessDescriptorSetLayout;
	if (vkAllocateDescriptorSets(m_context.device, &alloc, &m_bindlessDescriptorSet) != VK_SUCCESS) return false;

	return InitializeBindlessDescriptorSet();
}

bool VulkanDevice::Impl::InitializeBindlessDescriptorSet() {
	const VulkanTexture* fallbackTexture = static_cast<const VulkanTexture*>(m_fallbackTexture);
	if (fallbackTexture == nullptr || fallbackTexture->GetImageView() == VK_NULL_HANDLE || fallbackTexture->GetSampler() == VK_NULL_HANDLE) {
		return false;
	}

	std::vector<VkDescriptorImageInfo> imageInfos(m_maxBindlessTextures);
	for (VkDescriptorImageInfo& imageInfo : imageInfos) {
		imageInfo.sampler = fallbackTexture->GetSampler();
		imageInfo.imageView = fallbackTexture->GetImageView();
		imageInfo.imageLayout = fallbackTexture->GetImageLayout();
	}

	VkWriteDescriptorSet write{};
	write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	write.dstSet = m_bindlessDescriptorSet;
	write.dstBinding = 0;
	write.descriptorCount = m_maxBindlessTextures;
	write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	write.pImageInfo = imageInfos.data();
	vkUpdateDescriptorSets(m_context.device, 1, &write, 0, nullptr);
	return true;
}

bool VulkanDevice::Impl::UpdateDrawDescriptorSets(const VulkanCommandList& commandList) {
	if (commandList.m_drawCalls.size() > m_maxDrawsPerFrame) return false;
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
	const uint32_t descriptorIndex = m_currentFrameIndex * m_maxDrawsPerFrame + drawIndex;
	if (descriptorIndex >= m_descriptorSets.size()) return false;

	const VulkanPipelineState* pipelineState = dynamic_cast<const VulkanPipelineState*>(drawCall.pipelineState);
	const bool usesBindlessTextures = pipelineState != nullptr && pipelineState->UsesBindlessTextures();
	const VulkanBuffer* vertexBuffer = dynamic_cast<const VulkanBuffer*>(drawCall.geometry.vertexBuffer);
	const VulkanBuffer* indexBuffer = dynamic_cast<const VulkanBuffer*>(drawCall.geometry.indexBuffer);
	if (vertexBuffer == nullptr || (drawCall.indexed && indexBuffer == nullptr)) return false;

	std::vector<VkWriteDescriptorSet> writes;
	writes.reserve(m_shaderLayout.descriptorBindingCount);

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
			textureWrite.dstSet = m_descriptorSets[descriptorIndex];
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

		constantInfos[binding].buffer = buffer->GetHandle();
		constantInfos[binding].offset = constant.offset;
		constantInfos[binding].range = constant.size > 0 ? constant.size : VK_WHOLE_SIZE;

		VkWriteDescriptorSet constantWrite{};
		constantWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		constantWrite.dstSet = m_descriptorSets[descriptorIndex];
		constantWrite.dstBinding = binding;
		constantWrite.descriptorCount = 1;
		constantWrite.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
		constantWrite.pBufferInfo = &constantInfos[binding];
		writes.push_back(constantWrite);
	}

	std::array<VkDescriptorBufferInfo, VulkanCommandList::kMaxConstantBufferBindings> storageInfos = {};
	for (uint32_t binding = 0; binding < drawCall.storageBuffers.size(); ++binding) {
		const auto& storage = drawCall.storageBuffers[binding];
		const VulkanBuffer* buffer = dynamic_cast<const VulkanBuffer*>(storage.buffer);
		if (buffer == nullptr) continue;

		storageInfos[binding].buffer = buffer->GetHandle();
		storageInfos[binding].offset = storage.offset;
		storageInfos[binding].range = storage.size > 0 ? storage.size : VK_WHOLE_SIZE;

		VkWriteDescriptorSet storageWrite{};
		storageWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		storageWrite.dstSet = m_descriptorSets[descriptorIndex];
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
		shadowWrite.dstSet = m_descriptorSets[descriptorIndex];
		shadowWrite.dstBinding = m_shaderLayout.shadowSamplerBinding;
		shadowWrite.descriptorCount = 1;
		shadowWrite.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		shadowWrite.pImageInfo = &shadowInfo;
		writes.push_back(shadowWrite);
	}

	std::array<VkDescriptorBufferInfo, 2> geometryInfos = {};
	geometryInfos[0].buffer = vertexBuffer->GetHandle();
	geometryInfos[0].offset = drawCall.geometry.vertexOffset;
	geometryInfos[0].range = VK_WHOLE_SIZE;
	geometryInfos[1].buffer = indexBuffer != nullptr ? indexBuffer->GetHandle() : vertexBuffer->GetHandle();
	geometryInfos[1].offset = indexBuffer != nullptr ? drawCall.geometry.indexOffset : 0;
	geometryInfos[1].range = VK_WHOLE_SIZE;

	VkWriteDescriptorSet vertexWrite{};
	vertexWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	vertexWrite.dstSet = m_descriptorSets[descriptorIndex];
	vertexWrite.dstBinding = m_shaderLayout.vertexStorageBinding;
	vertexWrite.descriptorCount = 1;
	vertexWrite.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	vertexWrite.pBufferInfo = &geometryInfos[0];
	writes.push_back(vertexWrite);

	VkWriteDescriptorSet indexWrite{};
	indexWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	indexWrite.dstSet = m_descriptorSets[descriptorIndex];
	indexWrite.dstBinding = m_shaderLayout.indexStorageBinding;
	indexWrite.descriptorCount = 1;
	indexWrite.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	indexWrite.pBufferInfo = &geometryInfos[1];
	writes.push_back(indexWrite);

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
	dependencies[1].srcStageMask = VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
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
	// PipelineLayout은 main PSO와 같은 DescriptorSetLayout + PushConstantRange를 공유한다.
	// Shadow pass도 같은 binding 3의 lightViewProj UBO를 읽기 때문에 호환 가능하다.
	VkPushConstantRange pushConstantRange{};
	pushConstantRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
	pushConstantRange.offset = 0;
	pushConstantRange.size = m_shaderLayout.pushConstantRangeSize;

	VkPipelineLayoutCreateInfo layoutInfo{};
	layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	layoutInfo.setLayoutCount = 1;
	layoutInfo.pSetLayouts = &m_descriptorSetLayout;
	layoutInfo.pushConstantRangeCount = 1;
	layoutInfo.pPushConstantRanges = &pushConstantRange;
	if (vkCreatePipelineLayout(m_context.device, &layoutInfo, nullptr, &m_shadowPipelineLayout) != VK_SUCCESS) {
		return false;
	}

	// Shadow vertex shader 로드
	if (desc.shadowVertexShader == nullptr || desc.shadowVertexShaderSize == 0) {
		SDL_Log("Shadow pipeline shader bytecode is missing");
		return false;
	}
	VkShaderModule vertModule = CreateShaderModule(m_context.device, desc.shadowVertexShader, desc.shadowVertexShaderSize);
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
	pipelineInfo.layout = m_shadowPipelineLayout;
	pipelineInfo.renderPass = m_shadowRenderPass;
	pipelineInfo.subpass = 0;

	const VkResult result = vkCreateGraphicsPipelines(m_context.device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &m_shadowPipeline);
	vkDestroyShaderModule(m_context.device, vertModule, nullptr);
	return result == VK_SUCCESS;
}

void VulkanDevice::Impl::DestroyShadowResources() {
	if (m_context.device == VK_NULL_HANDLE) return;

	if (m_shadowPipeline != VK_NULL_HANDLE) {
		vkDestroyPipeline(m_context.device, m_shadowPipeline, nullptr);
		m_shadowPipeline = VK_NULL_HANDLE;
	}
	if (m_shadowPipelineLayout != VK_NULL_HANDLE) {
		vkDestroyPipelineLayout(m_context.device, m_shadowPipelineLayout, nullptr);
		m_shadowPipelineLayout = VK_NULL_HANDLE;
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

void VulkanDevice::Impl::RecreateSwapchain() {
	vkDeviceWaitIdle(m_context.device);
	DestroySwapchainResources();
	m_swapchain.Initialize(m_context, m_windowHandle, dy::RHI::IsSrgbFormat(GetDesc().swapchainFormat));
	UpdateBackBufferMetadata();
	m_depthFormat = FindDepthFormat();
	CreateMainRenderPass();
	CreateDepthResources();
	CreateFramebuffers();
	m_imagesInFlight.assign(m_swapchain.GetImageCount(), VK_NULL_HANDLE);
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
		for (dy::RHI::IBuffer* buffer : m_ownedBuffers) delete buffer;
		m_ownedBuffers.clear();
		DestroyShadowResources();
		if (m_descriptorPool != VK_NULL_HANDLE) vkDestroyDescriptorPool(m_context.device, m_descriptorPool, nullptr);
		if (m_descriptorSetLayout != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(m_context.device, m_descriptorSetLayout, nullptr);
		if (m_bindlessDescriptorSetLayout != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(m_context.device, m_bindlessDescriptorSetLayout, nullptr);
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
