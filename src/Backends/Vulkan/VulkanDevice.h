#pragma once
#include "RHI/IDevice.h"
#include "VulkanCommandList.h"
#include "VulkanContext.h"
#include "VulkanSwapchain.h"
#include "VulkanPipeline.h"
#include <filesystem>
#include <vector>

struct GLFWwindow;

class VulkanDevice : public dy::RHI::IDevice
{
public:
	~VulkanDevice() override;

	// Implementation of IDevice
	void BeginFrame() override;
	uint32_t GetCurrentFrameIndex() const override { return m_currentFrameIndex; }
	dy::RHI::ICommandList* AcquireCommandList() override { return &m_commandList; }
	void Submit(dy::RHI::ICommandList** cmdLists, uint32_t count) override;
	void Present() override;

	dy::RHI::IBuffer* CreateBuffer(const dy::RHI::BufferDesc& desc) override { return nullptr; }
	dy::RHI::ITexture* CreateTexture(const dy::RHI::TextureDesc& desc) override { return nullptr; }
	dy::RHI::IPipelineState* CreateGraphicsPipeline(const dy::RHI::GraphicsPipelineDesc& desc) override { return nullptr; }

	void DestroyBuffer(dy::RHI::IBuffer* buffer) override {}
	void DestroyTexture(dy::RHI::ITexture* texture) override {}
	void DestroyPipelineState(dy::RHI::IPipelineState* pipeline) override {}

	dy::RHI::ITexture* GetBackBuffer() override { return nullptr; }

protected:
	int Initialize(const void *windowHandle) override;

private:
	bool CreateInstance();
	bool CreateSurface();
	bool PickPhysicalDevice();
	bool CreateLogicalDevice();
	bool CreateSyncObjects();
	bool CreateDescriptorSetLayout();
	bool CreateDescriptorPool();
	bool CreateDescriptorSets();
	bool CreateFramebuffers();
	bool CreateCommandPool();
	bool CreateCommandBuffer();
	bool CreateMeshBuffers();
	bool CreateTextureImage();
	bool CreateTextureImageView();
	bool CreateTextureSampler();

	void RecreateSwapchain();
	void DestroySwapchainResources();
	void DestroyDeviceResources();

	void RecordCommandBuffer(const VulkanCommandList& commandList);

	// Context and Components
	VulkanContext m_context;
	VulkanSwapchain m_swapchain;
	VulkanPipeline m_pipeline;

	GLFWwindow* m_window = nullptr;
	std::string m_shaderOutputDirectory;

	// Mesh Data
	VkBuffer m_vertexBuffer = VK_NULL_HANDLE;
	VkDeviceMemory m_vertexBufferMemory = VK_NULL_HANDLE;
	VkBuffer m_indexBuffer = VK_NULL_HANDLE;
	VkDeviceMemory m_indexBufferMemory = VK_NULL_HANDLE;
	uint32_t m_indexCount = 0;

	VkCommandPool m_commandPool = VK_NULL_HANDLE;
	std::vector<VkCommandBuffer> m_commandBuffers;

	VkImage m_textureImage = VK_NULL_HANDLE;
	VkDeviceMemory m_textureImageMemory = VK_NULL_HANDLE;
	VkImageView m_textureImageView = VK_NULL_HANDLE;
	VkSampler m_textureSampler = VK_NULL_HANDLE;

	VkDescriptorSetLayout m_descriptorSetLayout = VK_NULL_HANDLE;
	VkDescriptorPool m_descriptorPool = VK_NULL_HANDLE;
	std::vector<VkDescriptorSet> m_descriptorSets;
	
	std::vector<VkSemaphore> m_imageAvailableSemaphores;
	std::vector<VkSemaphore> m_renderFinishedSemaphores;
	std::vector<VkFence> m_inFlightFences;
	std::vector<VkFence> m_imagesInFlight;

	std::vector<VkFramebuffer> m_swapchainFramebuffers;

	static constexpr uint32_t kMaxFramesInFlight = 2;
	uint32_t m_currentFrameIndex = 0;
	uint32_t m_currentImageIndex = 0;
	bool m_frameReady = false;
	bool m_frameSubmitted = false;

	VulkanCommandList m_commandList;
};
