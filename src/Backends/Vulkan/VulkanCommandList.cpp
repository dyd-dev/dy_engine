#include "VulkanCommandList.h"
#include <algorithm>
#include <cstring>

namespace dy::Backends
{

void VulkanCommandList::Begin()
{
	m_clearColor = { { 0.4f, 0.7f, 1.0f, 1.0f } };
	m_clearDepth = 1.0f;
	m_renderTargetCount = 0;
	m_renderTargets = {};
	m_depthStencil = nullptr;
	m_boundPipeline = nullptr;
	m_pendingPushConstantSize = 0;
	m_pendingGeometry = {};
	m_pendingConstantBuffers = {};
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

void VulkanCommandList::BindGeometry(const dy::RHI::GeometryBinding& geometry)
{
	m_pendingGeometry = geometry;
}

void VulkanCommandList::BindConstantBuffer(uint32_t binding, dy::RHI::IBuffer* buffer, uint32_t offset, uint32_t size)
{
	if (binding >= m_pendingConstantBuffers.size()) return;

	m_pendingConstantBuffers[binding].buffer = buffer;
	m_pendingConstantBuffers[binding].offset = offset;
	m_pendingConstantBuffers[binding].size = size;
}

void VulkanCommandList::SetRenderTargets(uint32_t numRenderTargets, dy::RHI::ITexture** renderTargets, dy::RHI::ITexture* depthStencil)
{
	m_renderTargetCount = std::min<uint32_t>(numRenderTargets, kMaxRenderTargets);
	m_renderTargets = {};
	for (uint32_t i = 0; i < m_renderTargetCount; ++i) {
		m_renderTargets[i] = renderTargets != nullptr ? renderTargets[i] : nullptr;
	}
	m_depthStencil = depthStencil;
}

void VulkanCommandList::ClearColor(dy::RHI::ITexture* renderTarget, float r, float g, float b, float a)
{
	(void)renderTarget;
	m_clearColor = { { r, g, b, a } };
}

void VulkanCommandList::ClearDepth(dy::RHI::ITexture* depthStencil, float depth)
{
	(void)depthStencil;
	m_clearDepth = depth;
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
	drawCall.pipelineState = m_boundPipeline;
	drawCall.vertexStride = m_pendingGeometry.vertexStride;
	drawCall.geometry = m_pendingGeometry;
	drawCall.constantBuffers = m_pendingConstantBuffers;
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
	drawCall.pipelineState = m_boundPipeline;
	drawCall.vertexStride = m_pendingGeometry.vertexStride;
	drawCall.geometry = m_pendingGeometry;
	drawCall.constantBuffers = m_pendingConstantBuffers;
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
