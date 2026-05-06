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
	float cameraPosition[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
	float materialParams[4] = { 0.34f, 36.0f, 0.0f, 0.0f };
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

	dy::RHI::IBuffer* CreateBuffer(const dy::RHI::BufferDesc& desc) override;
	dy::RHI::ITexture* CreateTexture(const dy::RHI::TextureDesc& desc) override;
	dy::RHI::IPipelineState* CreateGraphicsPipeline(const dy::RHI::GraphicsPipelineDesc& desc) override;
	[[nodiscard]] dy::RHI::DescriptorIndex AllocateDescriptorSlot() override;
	void UpdateDescriptorSlot(dy::RHI::DescriptorIndex index, dy::RHI::ITexture* texture) override;

	void DestroyBuffer(dy::RHI::IBuffer* buffer) override;
	void DestroyTexture(dy::RHI::ITexture* texture) override;
	void DestroyPipelineState(dy::RHI::IPipelineState* pipeline) override;

	dy::RHI::ITexture* GetBackBuffer() override;

public:
    int InitializeForTest(const void* windowHandle, const std::string& shaderDir);
    bool UploadTestMesh(const std::vector<float>& vertices, const std::vector<uint32_t>& indices);
	void SetLightingVolumeProfile(const VulkanLightingVolumeProfile& profile);

	// Directional Light용 Shadow Map의 Light-Space ViewProjection 갱신.
	// Graphics::ComputeDirectionalLightViewProj 결과를 매 프레임 전달.
	// 16개 float (column-major mat4) 포인터.
	void SetShadowLightMatrix(const float* lightViewProjColumnMajor);
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
	bool CreateDepthResources();
	bool CreateFramebuffers();
	bool CreateCommandPool();
	bool CreateCommandBuffer();
	bool CreateMeshBuffers();
	bool CreateTextureImage();
	bool CreateTextureImageView();
	bool CreateTextureSampler();

	// Shadow Map 인프라 (광원 시점 깊이 텍스처)
	bool CreateShadowMapResources();        // VkImage + View + Sampler + Framebuffer
	bool CreateShadowRenderPass();          // depth-only RenderPass
	bool CreateShadowPipeline();            // depth-only Pipeline (FS 없음)
	bool CreateShadowMatrixBuffers();       // LightViewProj UBO (frames in flight)
	void DestroyShadowResources();

	void RecreateSwapchain();
	void DestroySwapchainResources();
	void DestroyDeviceResources();

	void RecordCommandBuffer(const VulkanCommandList& commandList);
	void RecordShadowPass(VkCommandBuffer commandBuffer, const VulkanCommandList& commandList);
	void RecordMainPass(VkCommandBuffer commandBuffer, const VulkanCommandList& commandList);
	void UpdateLightingVolumeBuffer();
	void UpdateShadowMatrixBuffer();
	void UpdateBackBufferMetadata();
	VkFormat FindDepthFormat() const;
	bool IsDepthFormatSupported(VkFormat format) const;

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
	VkImage m_depthImage = VK_NULL_HANDLE;
	VkDeviceMemory m_depthImageMemory = VK_NULL_HANDLE;
	VkImageView m_depthImageView = VK_NULL_HANDLE;
	VkFormat m_depthFormat = VK_FORMAT_UNDEFINED;

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
	std::vector<dy::RHI::IBuffer*> m_ownedBuffers;

	// ========== Shadow Map 리소스 ==========
	static constexpr uint32_t kShadowMapResolution = 2048;
	VkImage         m_shadowMapImage      = VK_NULL_HANDLE;
	VkDeviceMemory  m_shadowMapMemory     = VK_NULL_HANDLE;
	VkImageView     m_shadowMapView       = VK_NULL_HANDLE;
	VkSampler       m_shadowMapSampler    = VK_NULL_HANDLE;
	VkRenderPass    m_shadowRenderPass    = VK_NULL_HANDLE;
	VkFramebuffer   m_shadowFramebuffer   = VK_NULL_HANDLE;
	VkPipelineLayout m_shadowPipelineLayout = VK_NULL_HANDLE;
	VkPipeline      m_shadowPipeline      = VK_NULL_HANDLE;
	VkFormat        m_shadowMapFormat     = VK_FORMAT_UNDEFINED;

	// LightViewProj UBO (frames in flight 만큼 다중 버퍼링)
	std::vector<VkBuffer>       m_shadowMatrixBuffers;
	std::vector<VkDeviceMemory> m_shadowMatrixMemories;
	std::vector<void*>          m_shadowMatrixMapped;
	float                       m_shadowLightViewProj[16] = {
		1.0f, 0.0f, 0.0f, 0.0f,
		0.0f, 1.0f, 0.0f, 0.0f,
		0.0f, 0.0f, 1.0f, 0.0f,
		0.0f, 0.0f, 0.0f, 1.0f
	};

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



