#pragma once
#include "RHI/ICommandList.h"
#include "VulkanContext.h"
#include <array>
#include <cstdint>
#include <vector>

class VulkanDevice;

class VulkanCommandList : public dy::RHI::ICommandList
{
public:
	void BindGraphicsPipeline(dy::RHI::IPipelineState* pipelineState) override;
	void BindGlobalDescriptorHeap() override {}
	void BindIndexBuffer(dy::RHI::IBuffer* buffer, dy::RHI::Format format, uint32_t offset) override {}
	void SetPushConstants(uint32_t size, const void* data) override;
	void SetRenderTargets(uint32_t numRenderTargets, dy::RHI::ITexture** renderTargets, dy::RHI::ITexture* depthStencil) override {}
	void ClearColor(dy::RHI::ITexture* renderTarget, float r, float g, float b, float a) override;
	void ClearDepth(dy::RHI::ITexture* depthStencil, float depth) override {}
	void DrawInstanced(uint32_t vertexCount, uint32_t instanceCount, uint32_t startVertex, uint32_t startInstance) override;
	void ResourceBarrier(dy::RHI::IBuffer* buffer, dy::RHI::ResourceState before, dy::RHI::ResourceState after) override {}
	void ResourceBarrier(dy::RHI::ITexture* texture, dy::RHI::ResourceState before, dy::RHI::ResourceState after) override {}
	void Close() override { m_isClosed = true; }

	void Begin();
	void End();

private:
	struct DrawCall
	{
		uint32_t vertexCount = 0;
		uint32_t instanceCount = 0;
		uint32_t startVertex = 0;
		uint32_t startInstance = 0;
		uint32_t pushConstantSize = 0;
		std::array<uint8_t, 128> pushConstants = {};
	};

	friend class VulkanDevice;
	VkClearColorValue m_clearColor = { { 0.4f, 0.7f, 1.0f, 1.0f } };
	dy::RHI::IPipelineState* m_boundPipeline = nullptr;
	std::array<uint8_t, 128> m_pendingPushConstants = {};
	uint32_t m_pendingPushConstantSize = 0;
	std::vector<DrawCall> m_drawCalls;
	bool m_isClosed = false;
};
