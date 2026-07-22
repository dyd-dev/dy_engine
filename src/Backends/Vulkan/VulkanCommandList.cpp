#include "VulkanCommandList.h"
#include <algorithm>
#include <cstring>

namespace dy::Backends
{

void VulkanCommandList::Begin()
{
	m_boundPipeline = nullptr;
	m_pendingInlineConstants.clear();
	m_pendingInlineConstantOffset = 0;
	m_pendingResourceSet = nullptr;
	m_pendingVertexBuffers.clear();
	m_pendingIndexBuffer = nullptr;
	m_pendingIndexFormat = dy::RHI::Format::Unknown;
	m_pendingIndexOffset = 0;
	m_hasPendingViewport = false;
	m_hasPendingScissor = false;
	m_passRecords.clear();
	m_drawCalls.clear();
	m_isClosed = false;
	m_isRendering = false;
}

void VulkanCommandList::BindGraphicsPipeline(dy::RHI::IPipelineState* pipelineState)
{
	m_boundPipeline = pipelineState;
}

void VulkanCommandList::BindResourceSet(dy::RHI::IResourceSet* resourceSet)
{
	m_pendingResourceSet = resourceSet;
}

void VulkanCommandList::SetInlineConstants(uint32_t offset, uint32_t size, const void* data)
{
	if (data == nullptr || size == 0) {
		m_pendingInlineConstants.clear();
		return;
	}
	m_pendingInlineConstantOffset = offset;
	m_pendingInlineConstants.resize(size);
	memcpy(m_pendingInlineConstants.data(), data, size);
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

void VulkanCommandList::BeginRendering(const dy::RHI::RenderingInfo& renderingInfo)
{
	if(m_isRendering ||
	   (renderingInfo.colorAttachmentCount > 0 && renderingInfo.colorAttachments == nullptr) ||
	   (renderingInfo.colorAttachmentCount == 0 && renderingInfo.depthStencilAttachment == nullptr)) return;
	PassRecord passRecord = {};
	passRecord.firstDraw = static_cast<uint32_t>(m_drawCalls.size());
	if(renderingInfo.colorAttachmentCount > 0)
		passRecord.colorAttachments.assign(
			renderingInfo.colorAttachments,
			renderingInfo.colorAttachments + renderingInfo.colorAttachmentCount);
	if(renderingInfo.depthStencilAttachment != nullptr)
	{
		passRecord.hasDepthStencilAttachment = true;
		passRecord.depthStencilAttachment = *renderingInfo.depthStencilAttachment;
	}
	m_passRecords.push_back(passRecord);
	m_isRendering = true;
}

void VulkanCommandList::EndRendering()
{
	m_isRendering = false;
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
	if (!m_isRendering) return;
	DrawCall drawCall = {};
	drawCall.indexed = false;
	drawCall.vertexCount = vertexCount;
	drawCall.instanceCount = instanceCount;
	drawCall.startVertex = startVertex;
	drawCall.startInstance = startInstance;
	drawCall.inlineConstantOffset = m_pendingInlineConstantOffset;
	drawCall.pipelineState = m_boundPipeline;
	drawCall.resourceSet = m_pendingResourceSet;
	drawCall.vertexBuffers = m_pendingVertexBuffers;
	drawCall.indexBuffer = m_pendingIndexBuffer;
	drawCall.indexFormat = m_pendingIndexFormat;
	drawCall.indexOffset = m_pendingIndexOffset;
	drawCall.hasViewport = m_hasPendingViewport;
	drawCall.hasScissor = m_hasPendingScissor;
	drawCall.viewport = m_pendingViewport;
	drawCall.scissor = m_pendingScissor;
	drawCall.inlineConstants = m_pendingInlineConstants;
	m_drawCalls.push_back(drawCall);
}

void VulkanCommandList::DrawIndexedInstanced(uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex, int32_t vertexOffset, uint32_t firstInstance)
{
	if (!m_isRendering) return;
	DrawCall drawCall = {};
	drawCall.indexed = true;
	drawCall.indexCount = indexCount;
	drawCall.instanceCount = instanceCount;
	drawCall.firstIndex = firstIndex;
	drawCall.baseVertex = vertexOffset;
	drawCall.startInstance = firstInstance;
	drawCall.inlineConstantOffset = m_pendingInlineConstantOffset;
	drawCall.pipelineState = m_boundPipeline;
	drawCall.resourceSet = m_pendingResourceSet;
	drawCall.vertexBuffers = m_pendingVertexBuffers;
	drawCall.indexBuffer = m_pendingIndexBuffer;
	drawCall.indexFormat = m_pendingIndexFormat;
	drawCall.indexOffset = m_pendingIndexOffset;
	drawCall.hasViewport = m_hasPendingViewport;
	drawCall.hasScissor = m_hasPendingScissor;
	drawCall.viewport = m_pendingViewport;
	drawCall.scissor = m_pendingScissor;
	drawCall.inlineConstants = m_pendingInlineConstants;
	m_drawCalls.push_back(drawCall);
}

void VulkanCommandList::End()
{
}

void VulkanCommandList::Close()
{
	EndRendering();
	m_isClosed = true;
}

}
