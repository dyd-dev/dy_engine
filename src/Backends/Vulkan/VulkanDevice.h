#pragma once
#include "RHI/IDevice.h"
#include "VulkanCommandList.h"
#include "VulkanContext.h"
#include "VulkanSwapchain.h"
#include <cstdint>
#include <vector>

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
	dy::RHI::IPipelineState* CreateGraphicsPipeline(const dy::RHI::GraphicsPipelineDesc& desc) override;
	[[nodiscard]] dy::RHI::DescriptorIndex AllocateDescriptorSlot() override;
	void UpdateDescriptorSlot(dy::RHI::DescriptorIndex index, dy::RHI::ITexture* texture) override;

	void DestroyBuffer(dy::RHI::IBuffer* buffer) override;
	void DestroyTexture(dy::RHI::ITexture* texture) override;
	void DestroyPipelineState(dy::RHI::IPipelineState* pipeline) override;

	dy::RHI::ITexture* GetBackBuffer() override;

protected:
	int Initialize(const void* windowHandle, const dy::RHI::DeviceDesc& desc) override;

private:
	bool CreateInstance();
	bool CreateSurface();
	bool PickPhysicalDevice();
	bool CreateLogicalDevice();
	bool CreateSyncObjects();
	bool CreateDescriptorSetLayout();
	bool CreateDescriptorPool();
	bool CreateDescriptorSets();
	bool CreateMainRenderPass();
	bool CreateDepthResources();
	bool CreateFramebuffers();
	bool CreateCommandPool();
	bool CreateCommandBuffer();
	bool CreateTextureImage();
	bool CreateTextureImageView();
	bool CreateTextureSampler();

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
	bool UpdateDrawDescriptorSets(const VulkanCommandList& commandList);
	bool UpdateDrawDescriptorSet(const VulkanCommandList::DrawCall& drawCall, uint32_t drawIndex);
	void UpdateBackBufferMetadata();
	VkFormat FindDepthFormat() const;
	bool IsDepthFormatSupported(VkFormat format) const;

	VulkanContext m_context;
	VulkanSwapchain m_swapchain;
	VkRenderPass m_mainRenderPass = VK_NULL_HANDLE;

	void* m_windowHandle = nullptr;

	VkCommandPool m_commandPool = VK_NULL_HANDLE;
	std::vector<VkCommandBuffer> m_commandBuffers;

	VkImage m_textureImage = VK_NULL_HANDLE;
	VkDeviceMemory m_textureImageMemory = VK_NULL_HANDLE;
	VkImageView m_textureImageView = VK_NULL_HANDLE;
	VkSampler m_textureSampler = VK_NULL_HANDLE;
	VkImage m_depthImage = VK_NULL_HANDLE;
	VkDeviceMemory m_depthImageMemory = VK_NULL_HANDLE;
	VkImageView m_depthImageView = VK_NULL_HANDLE;
	VkFormat m_depthFormat = VK_FORMAT_UNDEFINED;

	VkDescriptorSetLayout m_descriptorSetLayout = VK_NULL_HANDLE;
	VkDescriptorPool m_descriptorPool = VK_NULL_HANDLE;
	std::vector<VkDescriptorSet> m_descriptorSets;

	std::vector<VkSemaphore> m_imageAvailableSemaphores;
	std::vector<VkSemaphore> m_renderFinishedSemaphores;
	std::vector<VkFence> m_inFlightFences;
	std::vector<VkFence> m_imagesInFlight;

	std::vector<VkFramebuffer> m_swapchainFramebuffers;
	std::vector<dy::RHI::IBuffer*> m_ownedBuffers;

	static constexpr uint32_t kShadowMapResolution = 2048;
	VkImage m_shadowMapImage = VK_NULL_HANDLE;
	VkDeviceMemory m_shadowMapMemory = VK_NULL_HANDLE;
	VkImageView m_shadowMapView = VK_NULL_HANDLE;
	VkSampler m_shadowMapSampler = VK_NULL_HANDLE;
	VkRenderPass m_shadowRenderPass = VK_NULL_HANDLE;
	VkFramebuffer m_shadowFramebuffer = VK_NULL_HANDLE;
	VkPipelineLayout m_shadowPipelineLayout = VK_NULL_HANDLE;
	VkPipeline m_shadowPipeline = VK_NULL_HANDLE;
	VkFormat m_shadowMapFormat = VK_FORMAT_UNDEFINED;

	static constexpr uint32_t kMaxFramesInFlight = 2;
	static constexpr uint32_t kMaxDrawsPerFrame = 128;
	uint32_t m_currentFrameIndex = 0;
	uint32_t m_currentImageIndex = 0;
	bool m_frameReady = false;
	bool m_frameSubmitted = false;
	dy::RHI::DescriptorIndex m_nextDescriptorIndex = 0;

	VulkanCommandList* m_commandList = nullptr;
	dy::RHI::ITexture* m_backBuffer = nullptr;
	std::vector<dy::RHI::ITexture*> m_ownedTextures;
	std::vector<dy::RHI::IPipelineState*> m_ownedPipelineStates;
};

}
