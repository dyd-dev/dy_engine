#pragma once
#include "RHI/IDevice.h"
#include "VulkanCommandList.h"
#include <filesystem>
#include <vulkan/vulkan.h>
#include <vector>

struct GLFWwindow;
struct WindowHandle;

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
	struct QueueFamilyIndices
	{
		uint32_t graphicsFamily = UINT32_MAX;
		uint32_t presentFamily = UINT32_MAX;

		bool IsComplete() const
		{
			return graphicsFamily != UINT32_MAX && presentFamily != UINT32_MAX;
		}
	};

	struct SwapchainSupportDetails
	{
		VkSurfaceCapabilitiesKHR capabilities = {};
		std::vector<VkSurfaceFormatKHR> formats;
		std::vector<VkPresentModeKHR> presentModes;
	};

	bool CreateInstance();
	bool CreateSurface();
	bool PickPhysicalDevice();
	bool CreateLogicalDevice();
	bool CreateSwapchain();
	bool CreateRenderPass();
	bool CreateDescriptorSetLayout();
	bool CreateGraphicsPipeline();
	bool CreateFramebuffers();
	bool CreateCommandPool();
	bool CreateTextureImage();
	bool CreateTextureImageView();
	bool CreateTextureSampler();
	bool CreateCommandBuffer();
	bool CreateSyncObjects();
	bool CreateDescriptorPool();
	bool CreateDescriptorSets();
	
	// Mesh
	bool CreateMeshBuffers();

	bool CheckShaderHotReload();
	bool ReloadShaders();
	bool CompileShaderSource(const std::filesystem::path& sourcePath, const std::filesystem::path& outputPath, const char* stageLabel) const;

	bool RecreateSwapchain();
	void DestroySwapchainResources();
	void DestroyDeviceResources();

	void RecordCommandBuffer(const VulkanCommandList& commandList);

	uint32_t FindMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties) const;
	void CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties, VkBuffer& buffer, VkDeviceMemory& bufferMemory);
	void CopyBuffer(VkBuffer srcBuffer, VkBuffer dstBuffer, VkDeviceSize size);
	void CreateImage(uint32_t width, uint32_t height, VkFormat format, VkImageTiling tiling, VkImageUsageFlags usage, VkMemoryPropertyFlags properties, VkImage& image, VkDeviceMemory& imageMemory);
	void TransitionImageLayout(VkImage image, VkFormat format, VkImageLayout oldLayout, VkImageLayout newLayout);
	void CopyBufferToImage(VkBuffer buffer, VkImage image, uint32_t width, uint32_t height);
	VkCommandBuffer BeginSingleTimeCommands();
	void EndSingleTimeCommands(VkCommandBuffer commandBuffer);
	VkImageView CreateImageView(VkImage image, VkFormat format);

	QueueFamilyIndices FindQueueFamilies(VkPhysicalDevice device) const;
	SwapchainSupportDetails QuerySwapchainSupport(VkPhysicalDevice device) const;
	VkSurfaceFormatKHR ChooseSwapSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& formats) const;
	VkPresentModeKHR ChoosePresentMode(const std::vector<VkPresentModeKHR>& presentModes) const;
	VkExtent2D ChooseSwapExtent(const VkSurfaceCapabilitiesKHR& capabilities) const;
	VkShaderModule LoadShaderModule(const char* path) const;

	GLFWwindow* m_window = nullptr;
	uint32_t m_windowWidth = 0;
	uint32_t m_windowHeight = 0;
	std::filesystem::path m_shaderSourceDirectory;
	std::filesystem::path m_shaderOutputDirectory;
	std::filesystem::file_time_type m_vertexShaderTimestamp = {};
	std::filesystem::file_time_type m_fragmentShaderTimestamp = {};
	bool m_shaderHotReloadInitialized = false;

	VkInstance m_instance = VK_NULL_HANDLE;
	VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
	VkDevice m_device = VK_NULL_HANDLE;
	VkSurfaceKHR m_surface = VK_NULL_HANDLE;
	VkQueue m_graphicsQueue = VK_NULL_HANDLE;
	VkQueue m_presentQueue = VK_NULL_HANDLE;
	VkSwapchainKHR m_swapchain = VK_NULL_HANDLE;
	VkFormat m_swapchainImageFormat = VK_FORMAT_UNDEFINED;
	VkExtent2D m_swapchainExtent = {};
	VkRenderPass m_renderPass = VK_NULL_HANDLE;
	VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
	VkPipeline m_graphicsPipeline = VK_NULL_HANDLE;

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

	std::vector<VkImage> m_swapchainImages;
	std::vector<VkImageView> m_swapchainImageViews;
	std::vector<VkFramebuffer> m_swapchainFramebuffers;

	static constexpr uint32_t kMaxFramesInFlight = 2;
	uint32_t m_currentFrameIndex = 0;
	uint32_t m_currentImageIndex = 0;
	bool m_frameReady = false;
	bool m_frameSubmitted = false;

	VulkanCommandList m_commandList;
};
