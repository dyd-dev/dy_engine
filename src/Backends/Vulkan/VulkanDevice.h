#pragma once
#include "RHI/IDevice.h"
#include "VulkanCommandList.h"
#include "VulkanContext.h"
#include "VulkanSwapchain.h"
#include "VulkanPipeline.h"
#include <cstdint>
#include <vector>

struct VulkanLightingVolumeProfile
{
	float globalLightDirection[4] = { -0.45f, -0.8f, -0.35f, 1.0f };
	float globalLightColor[4] = { 1.0f, 0.96f, 0.86f, 1.0f };
	float spotLightPosition[4] = { 0.0f, 1.6f, 1.8f, 1.0f };
	float spotLightDirection[4] = { 0.0f, -0.7f, -1.0f, 1.0f };
	float spotLightColor[4] = { 0.55f, 0.72f, 1.0f, 4.0f };
	float volumeParams[4] = { 0.16f, 1.0f, 0.08f, 0.68f };
	float volumeParams2[4] = { 0.88f, 0.0f, 0.0f, 0.0f };
};

class VulkanDevice : public dy::RHI::IDevice
{
public:
	VulkanDevice();
	~VulkanDevice() override;

	// Implementation of IDevice
	void BeginFrame() override;
	uint32_t GetCurrentFrameIndex() const override { return m_currentFrameIndex; }
	dy::RHI::ICommandList* AcquireCommandList() override { return m_commandList; }
	void Submit(dy::RHI::ICommandList** cmdLists, uint32_t count) override;
	void Present() override;

	dy::RHI::IBuffer* CreateBuffer(const dy::RHI::BufferDesc& desc) override
	{
		(void)desc;
		return nullptr;
	}
	dy::RHI::ITexture* CreateTexture(const dy::RHI::TextureDesc& desc) override;
	dy::RHI::IPipelineState* CreateGraphicsPipeline(const dy::RHI::GraphicsPipelineDesc& desc) override;
	[[nodiscard]] dy::RHI::DescriptorIndex AllocateDescriptorSlot() override;
	void UpdateDescriptorSlot(dy::RHI::DescriptorIndex index, dy::RHI::ITexture* texture) override;

	void DestroyBuffer(dy::RHI::IBuffer* buffer) override
	{
		(void)buffer;
	}
	void DestroyTexture(dy::RHI::ITexture* texture) override;
	void DestroyPipelineState(dy::RHI::IPipelineState* pipeline) override;

	dy::RHI::ITexture* GetBackBuffer() override;

public:
    int InitializeForTest(const void* windowHandle, const std::string& shaderDir);
    bool UploadTestMesh(const std::vector<float>& vertices, const std::vector<uint32_t>& indices);
	void SetLightingVolumeProfile(const VulkanLightingVolumeProfile& profile);
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
	bool CreateLightingVolumeBuffers();
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
	void UpdateLightingVolumeBuffer();
	void UpdateBackBufferMetadata();

	// Context and Components
	VulkanContext m_context;
	VulkanSwapchain m_swapchain;
	VulkanPipeline m_pipeline;

	void* m_windowHandle = nullptr;
	std::string m_shaderOutputDirectory;

	// Mesh Data
	VkBuffer m_vertexBuffer = VK_NULL_HANDLE;
    bool m_useVertexInput = false;
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
	std::vector<VkBuffer> m_lightingVolumeBuffers;
	std::vector<VkDeviceMemory> m_lightingVolumeBufferMemories;
	std::vector<void*> m_lightingVolumeMappedBuffers;
	VulkanLightingVolumeProfile m_lightingVolumeProfile;
	
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
	dy::RHI::DescriptorIndex m_nextDescriptorIndex = 0;

	VulkanCommandList* m_commandList = nullptr;
	dy::RHI::ITexture* m_backBuffer = nullptr;
	std::vector<dy::RHI::ITexture*> m_ownedTextures;
	std::vector<dy::RHI::IPipelineState*> m_ownedPipelineStates;
};



