#pragma once
#include "RHI/ICommandList.h"
#include "VulkanDevice.h"
#include <array>
#include <cstdint>
#include <utility>
#include <vector>

namespace dy::Backends
{

class VulkanDevice;

class VulkanCommandList : public dy::RHI::ICommandList
{
public:
	void BindGraphicsPipeline(dy::RHI::IPipelineState* pipelineState) override;
	void BindResourceSet(dy::RHI::IResourceSet* resourceSet) override;
	void BindVertexBuffer(uint32_t slot, dy::RHI::IBuffer* buffer, uint32_t offset) override;
	void BindIndexBuffer(dy::RHI::IBuffer* buffer, dy::RHI::Format format, uint32_t offset) override;
	void SetInlineConstants(uint32_t offset, uint32_t size, const void* data) override;
	void Barrier(const dy::RHI::TextureBarrier* barriers, uint32_t barrierCount) override;
	void BeginRendering(const dy::RHI::RenderingInfo& renderingInfo) override;
	void EndRendering() override;
	void SetViewport(const dy::RHI::Viewport& viewport) override;
	void SetScissor(const dy::RHI::Rect& rect) override;
	void DrawInstanced(uint32_t vertexCount, uint32_t instanceCount, uint32_t startVertex, uint32_t startInstance) override;
	void DrawIndexedInstanced(uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex, int32_t vertexOffset, uint32_t firstInstance) override;
	void Close() override;

	void Begin();
	void End();

private:
	struct VertexBufferBinding
	{
		uint32_t slot = 0;
		dy::RHI::IBuffer* buffer = nullptr;
		uint32_t offset = 0;
	};

	struct PassRecord
	{
		uint32_t firstDraw = 0;
		std::vector<dy::RHI::ColorAttachmentInfo> colorAttachments;
		bool hasDepthStencilAttachment = false;
		dy::RHI::DepthStencilAttachmentInfo depthStencilAttachment = {};
	};

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
		uint32_t inlineConstantOffset = 0;
		bool hasViewport = false;
		bool hasScissor = false;
		dy::RHI::IPipelineState* pipelineState = nullptr;
		dy::RHI::IResourceSet* resourceSet = nullptr;
		std::vector<VertexBufferBinding> vertexBuffers;
		dy::RHI::IBuffer* indexBuffer = nullptr;
		dy::RHI::Format indexFormat = dy::RHI::Format::Unknown;
		uint32_t indexOffset = 0;
		dy::RHI::Viewport viewport = {};
		dy::RHI::Rect scissor = {};
		std::vector<uint8_t> inlineConstants;
	};

	friend struct VulkanDevice::Impl;
	dy::RHI::IPipelineState* m_boundPipeline = nullptr;
	std::vector<uint8_t> m_pendingInlineConstants;
	uint32_t m_pendingInlineConstantOffset = 0;
	dy::RHI::IResourceSet* m_pendingResourceSet = nullptr;
	std::vector<VertexBufferBinding> m_pendingVertexBuffers;
	dy::RHI::IBuffer* m_pendingIndexBuffer = nullptr;
	dy::RHI::Format m_pendingIndexFormat = dy::RHI::Format::Unknown;
	uint32_t m_pendingIndexOffset = 0;
	bool m_hasPendingViewport = false;
	bool m_hasPendingScissor = false;
	dy::RHI::Viewport m_pendingViewport = {};
	dy::RHI::Rect m_pendingScissor = {};
	std::vector<PassRecord> m_passRecords;
	std::vector<std::pair<uint32_t, dy::RHI::TextureBarrier>> m_barriers;
	std::vector<DrawCall> m_drawCalls;
	bool m_isClosed = false;
	bool m_isRendering = false;
};

}
