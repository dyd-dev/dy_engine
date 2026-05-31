#include "VulkanCommandList.h"
#include <algorithm>
#include <cstring>

void VulkanCommandList::Begin()
{
	m_clearColor = { { 0.4f, 0.7f, 1.0f, 1.0f } };
	m_boundPipeline = nullptr;
	m_pendingVertexBuffer = nullptr;
	m_pendingVertexStride = 0;
	m_pendingVertexBufferOffset = 0;
	m_pendingIndexBuffer = nullptr;
	m_pendingIndexFormat = dy::RHI::Format::Unknown;
	m_pendingIndexBufferOffset = 0;
	m_pendingPushConstantSize = 0;
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
	m_pendingVertexBufferOffset = offset;
}

void VulkanCommandList::BindIndexBuffer(dy::RHI::IBuffer* buffer, dy::RHI::Format format, uint32_t offset)
{
	m_pendingIndexBuffer = buffer;
	m_pendingIndexFormat = format;
	m_pendingIndexBufferOffset = offset;
}

void VulkanCommandList::ClearColor(dy::RHI::ITexture* renderTarget, float r, float g, float b, float a)
{
	(void)renderTarget;
	m_clearColor = { { r, g, b, a } };
}

void VulkanCommandList::DrawInstanced(uint32_t vertexCount, uint32_t instanceCount, uint32_t startVertex, uint32_t startInstance)
{
	DrawCall drawCall = {};
	drawCall.vertexCount = vertexCount;
	drawCall.instanceCount = instanceCount;
	drawCall.startVertex = startVertex;
	drawCall.startInstance = startInstance;
	drawCall.vertexBuffer = m_pendingVertexBuffer;
	drawCall.vertexStride = m_pendingVertexStride;
	drawCall.vertexBufferOffset = m_pendingVertexBufferOffset;
	drawCall.indexBuffer = m_pendingIndexBuffer;
	drawCall.indexFormat = m_pendingIndexFormat;
	drawCall.indexBufferOffset = m_pendingIndexBufferOffset;
	drawCall.pushConstantSize = m_pendingPushConstantSize;
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
	drawCall.vertexOffset = vertexOffset;
	drawCall.startInstance = firstInstance;
	drawCall.vertexBuffer = m_pendingVertexBuffer;
	drawCall.vertexStride = m_pendingVertexStride;
	drawCall.vertexBufferOffset = m_pendingVertexBufferOffset;
	drawCall.indexBuffer = m_pendingIndexBuffer;
	drawCall.indexFormat = m_pendingIndexFormat;
	drawCall.indexBufferOffset = m_pendingIndexBufferOffset;
	drawCall.pushConstantSize = m_pendingPushConstantSize;
	if (m_pendingPushConstantSize > 0) {
		memcpy(drawCall.pushConstants.data(), m_pendingPushConstants.data(), m_pendingPushConstantSize);
	}
	m_drawCalls.push_back(drawCall);
}

void VulkanCommandList::End()
{
}
