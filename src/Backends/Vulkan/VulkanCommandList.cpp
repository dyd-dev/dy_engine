#include "VulkanCommandList.h"
#include <algorithm>
#include <cstring>

namespace dy::Backends
{

void VulkanCommandList::Begin()
{
	m_clearColor = { { 0.4f, 0.7f, 1.0f, 1.0f } };
	m_boundPipeline = nullptr;
	m_pendingPushConstantSize = 0;
	m_pendingVertexBuffer = nullptr;
	m_pendingVertexStride = 0;
	m_pendingVertexOffset = 0;
	m_pendingVertexStorageBuffer = nullptr;
	m_pendingVertexStorageStride = 0;
	m_pendingVertexStorageOffset = 0;
	m_pendingIndexBuffer = nullptr;
	m_pendingIndexFormat = dy::RHI::Format::Unknown;
	m_pendingIndexOffset = 0;
	m_pendingIndexStorageBuffer = nullptr;
	m_pendingIndexStorageFormat = dy::RHI::Format::Unknown;
	m_pendingIndexStorageOffset = 0;
	m_hasPendingViewport = false;
	m_hasPendingScissor = false;
	m_drawCalls.clear();
	m_isClosed = false;
}

void VulkanCommandList::BindGraphicsPipeline(dy::RHI::IPipelineState* pipelineState)
{
	m_boundPipeline = pipelineState;
}

void VulkanCommandList::SetPushConstants(uint32_t size, const void* data)
{
	if (data == nullptr) {
		m_pendingPushConstantSize = 0;
		return;
	}

	m_pendingPushConstantSize = std::min<uint32_t>(size, static_cast<uint32_t>(m_pendingPushConstants.size()));
	memcpy(m_pendingPushConstants.data(), data, m_pendingPushConstantSize);
}

void VulkanCommandList::BindVertexBuffer(dy::RHI::IBuffer* buffer, uint32_t stride, uint32_t offset)
{
	m_pendingVertexBuffer = buffer;
	m_pendingVertexStride = stride;
	m_pendingVertexOffset = offset;
}

void VulkanCommandList::BindVertexStorageBuffer(dy::RHI::IBuffer* buffer, uint32_t stride, uint32_t offset)
{
	m_pendingVertexStorageBuffer = buffer;
	m_pendingVertexStorageStride = stride;
	m_pendingVertexStorageOffset = offset;
}

void VulkanCommandList::BindIndexBuffer(dy::RHI::IBuffer* buffer, dy::RHI::Format format, uint32_t offset)
{
	m_pendingIndexBuffer = buffer;
	m_pendingIndexFormat = format;
	m_pendingIndexOffset = offset;
}

void VulkanCommandList::BindIndexStorageBuffer(dy::RHI::IBuffer* buffer, dy::RHI::Format format, uint32_t offset)
{
	m_pendingIndexStorageBuffer = buffer;
	m_pendingIndexStorageFormat = format;
	m_pendingIndexStorageOffset = offset;
}

void VulkanCommandList::ClearColor(dy::RHI::ITexture* renderTarget, float r, float g, float b, float a)
{
	(void)renderTarget;
	m_clearColor = { { r, g, b, a } };
}

void VulkanCommandList::SetViewport(const dy::RHI::Viewport& viewport)
{
	m_pendingViewport = viewport;
	m_hasPendingViewport = true;
}

void VulkanCommandList::SetScissor(const dy::RHI::Rect& rect)
{
	m_pendingScissor = rect;
	m_hasPendingScissor = true;
}

void VulkanCommandList::DrawInstanced(uint32_t vertexCount, uint32_t instanceCount, uint32_t startVertex, uint32_t startInstance)
{
	DrawCall drawCall = {};
	drawCall.indexed = false;
	drawCall.vertexCount = vertexCount;
	drawCall.instanceCount = instanceCount;
	drawCall.startVertex = startVertex;
	drawCall.startInstance = startInstance;
	drawCall.pushConstantSize = m_pendingPushConstantSize;
	drawCall.vertexBuffer = m_pendingVertexBuffer;
	drawCall.vertexStride = m_pendingVertexStride;
	drawCall.vertexBufferOffset = m_pendingVertexOffset;
	drawCall.vertexStorageBuffer = m_pendingVertexStorageBuffer;
	drawCall.vertexStorageStride = m_pendingVertexStorageStride;
	drawCall.vertexStorageOffset = m_pendingVertexStorageOffset;
	drawCall.indexBuffer = m_pendingIndexBuffer;
	drawCall.indexFormat = m_pendingIndexFormat;
	drawCall.indexOffset = m_pendingIndexOffset;
	drawCall.indexStorageBuffer = m_pendingIndexStorageBuffer;
	drawCall.indexStorageFormat = m_pendingIndexStorageFormat;
	drawCall.indexStorageOffset = m_pendingIndexStorageOffset;
	drawCall.hasViewport = m_hasPendingViewport;
	drawCall.hasScissor = m_hasPendingScissor;
	drawCall.viewport = m_pendingViewport;
	drawCall.scissor = m_pendingScissor;
	if (m_pendingPushConstantSize > 0) {
		memcpy(drawCall.pushConstants.data(), m_pendingPushConstants.data(), m_pendingPushConstantSize);
	}
	m_drawCalls.push_back(drawCall);
}

void VulkanCommandList::DrawIndexedInstanced(uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex, int32_t vertexOffset, uint32_t firstInstance)
{
	DrawCall drawCall = {};
	drawCall.indexed = true;
	drawCall.indexCount = indexCount;
	drawCall.instanceCount = instanceCount;
	drawCall.firstIndex = firstIndex;
	drawCall.baseVertex = vertexOffset;
	drawCall.startInstance = firstInstance;
	drawCall.pushConstantSize = m_pendingPushConstantSize;
	drawCall.vertexBuffer = m_pendingVertexBuffer;
	drawCall.vertexStride = m_pendingVertexStride;
	drawCall.vertexBufferOffset = m_pendingVertexOffset;
	drawCall.vertexStorageBuffer = m_pendingVertexStorageBuffer;
	drawCall.vertexStorageStride = m_pendingVertexStorageStride;
	drawCall.vertexStorageOffset = m_pendingVertexStorageOffset;
	drawCall.indexBuffer = m_pendingIndexBuffer;
	drawCall.indexFormat = m_pendingIndexFormat;
	drawCall.indexOffset = m_pendingIndexOffset;
	drawCall.indexStorageBuffer = m_pendingIndexStorageBuffer;
	drawCall.indexStorageFormat = m_pendingIndexStorageFormat;
	drawCall.indexStorageOffset = m_pendingIndexStorageOffset;
	drawCall.hasViewport = m_hasPendingViewport;
	drawCall.hasScissor = m_hasPendingScissor;
	drawCall.viewport = m_pendingViewport;
	drawCall.scissor = m_pendingScissor;
	if (m_pendingPushConstantSize > 0) {
		memcpy(drawCall.pushConstants.data(), m_pendingPushConstants.data(), m_pendingPushConstantSize);
	}
	m_drawCalls.push_back(drawCall);
}

void VulkanCommandList::End()
{
}

}
