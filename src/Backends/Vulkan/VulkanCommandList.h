#pragma once
#include "RHI/ICommandList.h"
#include <vulkan/vulkan.h>

class VulkanDevice; // Forward declaration for friend class

class VulkanCommandList : public dy::RHI::ICommandList
{
public:
        // Implementation of ICommandList
        void BindGraphicsPipeline(dy::RHI::IPipelineState* pipelineState) override {}
        void BindGlobalDescriptorHeap() override {}
        void BindIndexBuffer(dy::RHI::IBuffer* buffer, dy::RHI::Format format, uint32_t offset) override {}	void SetPushConstants(uint32_t size, const void* data) override {}
	void SetRenderTargets(uint32_t numRenderTargets, dy::RHI::ITexture** renderTargets, dy::RHI::ITexture* depthStencil) override {}
	void ClearColor(dy::RHI::ITexture* renderTarget, float r, float g, float b, float a) override;
	void ClearDepth(dy::RHI::ITexture* depthStencil, float depth) override {}
	void DrawInstanced(uint32_t vertexCount, uint32_t instanceCount, uint32_t startVertex, uint32_t startInstance) override {}
	void ResourceBarrier(dy::RHI::IBuffer* buffer, dy::RHI::ResourceState before, dy::RHI::ResourceState after) override {}
	void ResourceBarrier(dy::RHI::ITexture* texture, dy::RHI::ResourceState before, dy::RHI::ResourceState after) override {}
	void Close() override {}

	// Legacy methods (temporarily kept for internal use if needed)
	void Begin();
	void End();

private:
	friend class VulkanDevice; // Allow VulkanDevice to access internal state for RenderPass building
	VkClearColorValue m_clearColor = { { 0.4f, 0.7f, 1.0f, 1.0f } };
};
