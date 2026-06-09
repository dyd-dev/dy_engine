#pragma once
#include "RHI/IDevice.h"
#include "VulkanCommandList.h"
#include "VulkanContext.h"
#include "VulkanSwapchain.h"
#include <cstdint>
#include <vector>

namespace
{
	constexpr const char* kValidationLayerName = "VK_LAYER_KHRONOS_validation";
	constexpr const uint32_t kFallbackTextureWidth = 2;
	constexpr const uint32_t kFallbackTextureHeight = 2;
}

namespace dy::Backends
{

class VulkanDevice : public dy::RHI::IDevice
{
public:
	VulkanDevice();
	~VulkanDevice() override;

	void BeginFrame() override;
	uint32_t GetCurrentFrameIndex() const override { return m_currentFrameIndex; }
	dy::RHI::ICommandList* AcquireCommandList() override { return m_commandList; }
	void Submit(dy::RHI::ICommandList** cmdLists, uint32_t count) override;
	void Present() override;

	dy::RHI::IBuffer* CreateBuffer(const dy::RHI::BufferDesc& desc) override;
	dy::RHI::ITexture* CreateTexture(const dy::RHI::TextureDesc& desc) override;
	bool UpdateTexture(dy::RHI::ITexture* texture, const void* rgba8Pixels, uint32_t rowPitch) override;
	dy::RHI::IPipelineState* CreateGraphicsPipeline(const dy::RHI::GraphicsPipelineDesc& desc) override;
	[[nodiscard]] dy::RHI::DescriptorIndex AllocateDescriptorSlot() override;
	void UpdateDescriptorSlot(dy::RHI::DescriptorIndex index, dy::RHI::ITexture* texture) override;
	void UpdateDescriptorSlot(dy::RHI::DescriptorIndex, dy::RHI::IBuffer*) override {}

	void DestroyBuffer(dy::RHI::IBuffer* buffer) override;
	void DestroyTexture(dy::RHI::ITexture* texture) override;
	void DestroyPipelineState(dy::RHI::IPipelineState* pipeline) override;

	[[nodiscard]] dy::RHI::ITexture* GetBackBuffer() override;
	
	[[nodiscard]] bool RequiresClipSpaceYFlip() const override { return true; }

protected:
	int Initialize(const void* windowHandle, const dy::RHI::DeviceDesc& desc) override;

private:
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
	void RecordMainPass(VkCommandBuffer commandBuffer, const VulkanCommandList& commandList);
	bool ResolveMainPassTarget(const VulkanCommandList& commandList, VkRenderPass& renderPass, VkFramebuffer& framebuffer, VkExtent2D& extent);
	bool GetOrCreateOffscreenFramebuffer(dy::RHI::ITexture* colorTarget, dy::RHI::ITexture* depthTarget, VkRenderPass& renderPass, VkFramebuffer& framebuffer, VkExtent2D& extent);
	bool CreateOffscreenRenderPass(VkFormat colorFormat, VkFormat depthFormat, VkImageLayout colorFinalLayout, VkRenderPass& renderPass);
	void DestroyRenderTargetCache();
	bool UpdateDrawDescriptorSets(const VulkanCommandList& commandList);
	bool UpdateDrawDescriptorSet(const VulkanCommandList::DrawCall& drawCall, uint32_t drawIndex);
	bool InitializeBindlessDescriptorSet();
	void UpdateBackBufferMetadata();
	VkFormat FindDepthFormat() const;
	bool IsDepthFormatSupported(VkFormat format) const;

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
	uint32_t m_shadowMapResolution = dy::RHI::DeviceDesc{}.defaultShadowMapResolution;

	uint32_t m_maxFramesInFlight = dy::RHI::DeviceDesc{}.maxFramesInFlight;
	uint32_t m_maxDrawsPerFrame = dy::RHI::DeviceDesc{}.maxDrawsPerFrame;
	uint32_t m_maxBindlessTextures = dy::RHI::DeviceDesc{}.maxBindlessTextures;
	uint32_t m_defaultShadowMapResolution = dy::RHI::DeviceDesc{}.defaultShadowMapResolution;
	uint32_t m_fallbackTextureWidth = kFallbackTextureWidth;
	uint32_t m_fallbackTextureHeight = kFallbackTextureHeight;
	uint64_t m_frameAcquireTimeoutNanoseconds = dy::RHI::DeviceDesc{}.frameAcquireTimeoutNanoseconds;
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

}
