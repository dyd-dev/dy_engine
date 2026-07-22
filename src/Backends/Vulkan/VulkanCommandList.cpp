#include "VulkanCommandList.h"
#include <algorithm>
#include <cstring>

namespace dy::Backends
{

void VulkanCommandList::Begin()
{
	m_clearColor = { { 0.4f, 0.7f, 1.0f, 1.0f } };
	m_clearDepth = 1.0f;
	m_boundPipeline = nullptr;
	m_pendingPushConstantSize = 0;
	m_pendingVertexBuffers.clear();
	m_pendingIndexBuffer = nullptr;
	m_pendingIndexFormat = dy::RHI::Format::Unknown;
	m_pendingIndexOffset = 0;
	m_pendingConstantBuffers = {};
	m_pendingStorageBuffers = {};
	m_pendingTextures = {};
	m_hasPendingViewport = false;
	m_hasPendingScissor = false;
	m_passRecords.clear();
	m_drawCalls.clear();
	m_isClosed = false;
}

void VulkanCommandList::BindGraphicsPipeline(dy::RHI::IPipelineState* pipelineState)
{
	m_boundPipeline = pipelineState;
}

void VulkanCommandList::SetInlineConstants(uint32_t size, const void* data)
{
	if (data == nullptr) {
		m_pendingPushConstantSize = 0;
		return;
	}

	m_pendingPushConstantSize = std::min<uint32_t>(size, static_cast<uint32_t>(m_pendingPushConstants.size()));
	memcpy(m_pendingPushConstants.data(), data, m_pendingPushConstantSize);
}

void VulkanCommandList::BindVertexBuffer(uint32_t slot, dy::RHI::IBuffer* buffer, uint32_t offset)
{
	for(VertexBufferBinding& binding : m_pendingVertexBuffers)
	{
		if(binding.slot != slot) continue;
		binding.buffer = buffer;
		binding.offset = offset;
		return;
	}
	m_pendingVertexBuffers.push_back(VertexBufferBinding{ slot, buffer, offset });
}

void VulkanCommandList::BindIndexBuffer(dy::RHI::IBuffer* buffer, dy::RHI::Format format, uint32_t offset)
{
	m_pendingIndexBuffer = buffer;
	m_pendingIndexFormat = format;
	m_pendingIndexOffset = offset;
}

void VulkanCommandList::BindConstantBuffer(uint32_t binding, dy::RHI::IBuffer* buffer, uint32_t offset, uint32_t size)
{
	if (binding >= m_pendingConstantBuffers.size()) return;

	m_pendingConstantBuffers[binding].buffer = buffer;
	m_pendingConstantBuffers[binding].offset = offset;
	m_pendingConstantBuffers[binding].size = size;
}

void VulkanCommandList::BindStorageBuffer(uint32_t binding, dy::RHI::IBuffer* buffer, uint32_t offset, uint32_t size)
{
	if (binding >= m_pendingStorageBuffers.size()) return;

	m_pendingStorageBuffers[binding].buffer = buffer;
	m_pendingStorageBuffers[binding].offset = offset;
	m_pendingStorageBuffers[binding].size = size;
}

void VulkanCommandList::BindTexture(uint32_t binding, dy::RHI::ITexture* texture)
{
	if (binding >= m_pendingTextures.size()) return;
	m_pendingTextures[binding] = texture;
}

void VulkanCommandList::SetRenderTargets(uint32_t numRenderTargets, dy::RHI::ITexture** renderTargets, dy::RHI::ITexture* depthStencil)
{
	PassRecord passRecord = {};
	passRecord.firstDraw = static_cast<uint32_t>(m_drawCalls.size());
	if(numRenderTargets > 0 && renderTargets != nullptr)
	{
		passRecord.renderTargets.assign(renderTargets, renderTargets + numRenderTargets);
		passRecord.clearColors.assign(numRenderTargets, m_clearColor);
	}
	passRecord.depthStencil = depthStencil;
	passRecord.clearDepth = m_clearDepth;
	m_passRecords.push_back(passRecord);
}

void VulkanCommandList::ClearColor(dy::RHI::ITexture* renderTarget, float r, float g, float b, float a)
{
	if (m_passRecords.empty()) return;
	PassRecord& passRecord = m_passRecords.back();
	if(renderTarget == nullptr) return;
	for(size_t attachmentIndex = 0; attachmentIndex < passRecord.renderTargets.size(); ++attachmentIndex)
	{
		if(passRecord.renderTargets[attachmentIndex] != renderTarget) continue;
		m_clearColor = { { r, g, b, a } };
		passRecord.clearColors[attachmentIndex] = m_clearColor;
		return;
	}
}

void VulkanCommandList::ClearDepth(dy::RHI::ITexture* depthStencil, float depth)
{
	if (m_passRecords.empty()) return;
	PassRecord& passRecord = m_passRecords.back();
	if (depthStencil == nullptr || passRecord.depthStencil != depthStencil) return;
	m_clearDepth = depth;
	passRecord.clearDepth = depth;
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
	if (m_passRecords.empty()) return;
	DrawCall drawCall = {};
	drawCall.indexed = false;
	drawCall.vertexCount = vertexCount;
	drawCall.instanceCount = instanceCount;
	drawCall.startVertex = startVertex;
	drawCall.startInstance = startInstance;
	drawCall.pushConstantSize = m_pendingPushConstantSize;
	drawCall.pipelineState = m_boundPipeline;
	drawCall.vertexBuffers = m_pendingVertexBuffers;
	drawCall.indexBuffer = m_pendingIndexBuffer;
	drawCall.indexFormat = m_pendingIndexFormat;
	drawCall.indexOffset = m_pendingIndexOffset;
	drawCall.constantBuffers = m_pendingConstantBuffers;
	drawCall.storageBuffers = m_pendingStorageBuffers;
	drawCall.textures = m_pendingTextures;
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
	if (m_passRecords.empty()) return;
	DrawCall drawCall = {};
	drawCall.indexed = true;
	drawCall.indexCount = indexCount;
	drawCall.instanceCount = instanceCount;
	drawCall.firstIndex = firstIndex;
	drawCall.baseVertex = vertexOffset;
	drawCall.startInstance = firstInstance;
	drawCall.pushConstantSize = m_pendingPushConstantSize;
	drawCall.pipelineState = m_boundPipeline;
	drawCall.vertexBuffers = m_pendingVertexBuffers;
	drawCall.indexBuffer = m_pendingIndexBuffer;
	drawCall.indexFormat = m_pendingIndexFormat;
	drawCall.indexOffset = m_pendingIndexOffset;
	drawCall.constantBuffers = m_pendingConstantBuffers;
	drawCall.storageBuffers = m_pendingStorageBuffers;
	drawCall.textures = m_pendingTextures;
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
