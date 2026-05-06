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
	void BindVertexBuffer(dy::RHI::IBuffer* buffer, uint32_t stride, uint32_t offset) override;
	void BindIndexBuffer(dy::RHI::IBuffer* buffer, dy::RHI::Format format, uint32_t offset) override;
	void SetPushConstants(uint32_t size, const void* data) override;
	void SetRenderTargets(uint32_t, dy::RHI::ITexture**, dy::RHI::ITexture*) override {}
	void SetViewport(const dy::RHI::Viewport& viewport) override;
	void SetScissor(const dy::RHI::Rect& rect) override;
	void ClearColor(dy::RHI::ITexture* renderTarget, float r, float g, float b, float a) override;
	void ClearDepth(dy::RHI::ITexture*, float) override {}
	void DrawInstanced(uint32_t vertexCount, uint32_t instanceCount, uint32_t startVertex, uint32_t startInstance) override;
	void DrawIndexedInstanced(uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex, int32_t vertexOffset, uint32_t firstInstance) override;
	void ResourceBarrier(dy::RHI::IBuffer*, dy::RHI::ResourceState, dy::RHI::ResourceState) override {}
	void ResourceBarrier(dy::RHI::ITexture*, dy::RHI::ResourceState, dy::RHI::ResourceState) override {}
	void Close() override { m_isClosed = true; }

	void Begin();
	void End();

private:
	struct DrawCall
	{
		bool indexed = false;
		uint32_t vertexCount = 0;
		uint32_t indexCount = 0;
		uint32_t instanceCount = 0;
		uint32_t startVertex = 0;
		uint32_t firstIndex = 0;
		int32_t baseVertex = 0;
		uint32_t startInstance = 0;
		uint32_t pushConstantSize = 0;
		bool hasViewport = false;
		bool hasScissor = false;
		dy::RHI::IBuffer* vertexBuffer = nullptr;
		uint32_t vertexStride = 0;
		uint32_t vertexBufferOffset = 0;
		dy::RHI::IBuffer* indexBuffer = nullptr;
		dy::RHI::Format indexFormat = dy::RHI::Format::Unknown;
		uint32_t indexOffset = 0;
		dy::RHI::Viewport viewport = {};
		dy::RHI::Rect scissor = {};
		std::array<uint8_t, 128> pushConstants = {};
	};

	friend class VulkanDevice;
	VkClearColorValue m_clearColor = { { 0.4f, 0.7f, 1.0f, 1.0f } };
	dy::RHI::IPipelineState* m_boundPipeline = nullptr;
	std::array<uint8_t, 128> m_pendingPushConstants = {};
	uint32_t m_pendingPushConstantSize = 0;
	dy::RHI::IBuffer* m_pendingVertexBuffer = nullptr;
	uint32_t m_pendingVertexStride = 0;
	uint32_t m_pendingVertexOffset = 0;
	dy::RHI::IBuffer* m_pendingIndexBuffer = nullptr;
	dy::RHI::Format m_pendingIndexFormat = dy::RHI::Format::Unknown;
	uint32_t m_pendingIndexOffset = 0;
	bool m_hasPendingViewport = false;
	bool m_hasPendingScissor = false;
	dy::RHI::Viewport m_pendingViewport = {};
	dy::RHI::Rect m_pendingScissor = {};
	std::vector<DrawCall> m_drawCalls;
	bool m_isClosed = false;
};
