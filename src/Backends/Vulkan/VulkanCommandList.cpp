#include "VulkanCommandList.h"
#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace dy::Backends
{

void VulkanCommandList::Begin(uint32_t maxColorAttachments, uint32_t maxDrawsPerFrame)
{
	m_maxColorAttachments = maxColorAttachments;
	m_maxDrawsPerFrame = maxDrawsPerFrame;
	m_renderTargetCount = 0;
	m_renderTargetsValid = true;
	m_renderTargetOffset = 0u;
	m_renderTargets.clear();
	m_depthStencil = nullptr;
	m_boundPipeline = nullptr;
	m_boundComputePipeline = nullptr;
	m_pendingPushConstantSize = 0;
	m_pendingGeometry = {};
	m_pendingConstantBuffers = {};
	m_pendingStorageBuffers = {};
	m_pendingTextures = {};
	m_hasPendingViewport = false;
	m_hasPendingScissor = false;
	m_drawCalls.clear();
	m_computeDispatches.clear();
	m_bufferBarriers.clear();
	m_colorClears.clear();
	m_depthClears.clear();
	m_workItems.clear();
	m_debugEvents.clear();
	m_debugEventDepth = 0;
	m_gpuTimestampEvents.clear();
	m_gpuTimestampDepth = 0;
	m_gpuTimestampScopeCount = 0;
	m_isClosed = false;
}

void VulkanCommandList::BindGraphicsPipeline(dy::RHI::IPipelineState* pipelineState)
{
	m_boundPipeline = pipelineState;
}

void VulkanCommandList::BindComputePipeline(dy::RHI::IPipelineState* pipelineState)
{
	m_boundComputePipeline = pipelineState;
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

void VulkanCommandList::BindGeometry(const dy::RHI::GeometryBinding& geometry)
{
	m_pendingGeometry = geometry;
}

void VulkanCommandList::BindVertexBuffer(dy::RHI::IBuffer* buffer, uint32_t stride, uint32_t offset)
{
	m_pendingGeometry.vertexBuffer = buffer;
	m_pendingGeometry.vertexStride = stride;
	m_pendingGeometry.vertexOffset = offset;
}

void VulkanCommandList::BindIndexBuffer(dy::RHI::IBuffer* buffer, dy::RHI::Format format, uint32_t offset)
{
	m_pendingGeometry.indexBuffer = buffer;
	m_pendingGeometry.indexFormat = format;
	m_pendingGeometry.indexOffset = offset;
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
	m_renderTargetOffset = m_renderTargets.size();
	m_renderTargetCount = numRenderTargets;
	m_renderTargetsValid = numRenderTargets <= m_maxColorAttachments
		&& (numRenderTargets == 0u || renderTargets != nullptr);
	if(m_renderTargetsValid && numRenderTargets > 0u)
		m_renderTargets.insert(m_renderTargets.end(), renderTargets, renderTargets + numRenderTargets);
	m_depthStencil = depthStencil;
}

void VulkanCommandList::ClearColor(dy::RHI::ITexture* renderTarget, float r, float g, float b, float a)
{
	if(renderTarget == nullptr) return;
	m_colorClears.push_back(ColorClear{ renderTarget, { { r, g, b, a } } });
	m_workItems.push_back(WorkItem{ WorkType::ClearColor, static_cast<uint32_t>(m_colorClears.size() - 1u) });
}

void VulkanCommandList::ClearDepth(dy::RHI::ITexture* depthStencil, float depth)
{
	if(depthStencil == nullptr) return;
	m_depthClears.push_back(DepthClear{ depthStencil, depth });
	m_workItems.push_back(WorkItem{ WorkType::ClearDepth, static_cast<uint32_t>(m_depthClears.size() - 1u) });
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
	drawCall.geometry = m_pendingGeometry;
	drawCall.constantBuffers = m_pendingConstantBuffers;
	drawCall.storageBuffers = m_pendingStorageBuffers;
	drawCall.textures = m_pendingTextures;
	drawCall.renderTargetCount = m_renderTargetCount;
	drawCall.renderTargetsValid = m_renderTargetsValid;
	drawCall.renderTargetOffset = m_renderTargetOffset;
	drawCall.depthStencil = m_depthStencil;
	drawCall.hasViewport = m_hasPendingViewport;
	drawCall.hasScissor = m_hasPendingScissor;
	drawCall.viewport = m_pendingViewport;
	drawCall.scissor = m_pendingScissor;
	if (m_pendingPushConstantSize > 0) {
		memcpy(drawCall.pushConstants.data(), m_pendingPushConstants.data(), m_pendingPushConstantSize);
	}
	m_drawCalls.push_back(drawCall);
	m_workItems.push_back(WorkItem{ WorkType::Draw, static_cast<uint32_t>(m_drawCalls.size() - 1u) });
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
	drawCall.geometry = m_pendingGeometry;
	drawCall.constantBuffers = m_pendingConstantBuffers;
	drawCall.storageBuffers = m_pendingStorageBuffers;
	drawCall.textures = m_pendingTextures;
	drawCall.renderTargetCount = m_renderTargetCount;
	drawCall.renderTargetsValid = m_renderTargetsValid;
	drawCall.renderTargetOffset = m_renderTargetOffset;
	drawCall.depthStencil = m_depthStencil;
	drawCall.hasViewport = m_hasPendingViewport;
	drawCall.hasScissor = m_hasPendingScissor;
	drawCall.viewport = m_pendingViewport;
	drawCall.scissor = m_pendingScissor;
	if (m_pendingPushConstantSize > 0) {
		memcpy(drawCall.pushConstants.data(), m_pendingPushConstants.data(), m_pendingPushConstantSize);
	}
	m_drawCalls.push_back(drawCall);
	m_workItems.push_back(WorkItem{ WorkType::Draw, static_cast<uint32_t>(m_drawCalls.size() - 1u) });
}

void VulkanCommandList::BeginDebugEvent(const char* name, const dy::RHI::DebugLabelColor& color)
{
	if (name == nullptr || name[0] == '\0') throw std::invalid_argument("Vulkan debug event name must not be empty.");
	const bool depthOnlyPass = m_renderTargetCount == 0 && m_depthStencil != nullptr;
	m_debugEvents.push_back({ DebugEventType::Begin, static_cast<uint32_t>(m_drawCalls.size()), depthOnlyPass, name, color });
	m_workItems.push_back({ WorkType::DebugEvent, static_cast<uint32_t>(m_debugEvents.size() - 1u) });
	++m_debugEventDepth;
}

void VulkanCommandList::EndDebugEvent()
{
	if (m_debugEventDepth == 0) throw std::logic_error("Vulkan debug event end has no matching begin.");
	const bool depthOnlyPass = m_renderTargetCount == 0 && m_depthStencil != nullptr;
	m_debugEvents.push_back({ DebugEventType::End, static_cast<uint32_t>(m_drawCalls.size()), depthOnlyPass, {}, {} });
	m_workItems.push_back({ WorkType::DebugEvent, static_cast<uint32_t>(m_debugEvents.size() - 1u) });
	--m_debugEventDepth;
}

void VulkanCommandList::InsertDebugMarker(const char* name, const dy::RHI::DebugLabelColor& color)
{
	if (name == nullptr || name[0] == '\0') throw std::invalid_argument("Vulkan debug marker name must not be empty.");
	const bool depthOnlyPass = m_renderTargetCount == 0 && m_depthStencil != nullptr;
	m_debugEvents.push_back({ DebugEventType::Marker, static_cast<uint32_t>(m_drawCalls.size()), depthOnlyPass, name, color });
	m_workItems.push_back({ WorkType::DebugEvent, static_cast<uint32_t>(m_debugEvents.size() - 1u) });
}

bool VulkanCommandList::BeginGpuTimestamp(const char* name)
{
	if (name == nullptr || name[0] == '\0') throw std::invalid_argument("Vulkan GPU timestamp name must not be empty.");
	if (m_gpuTimestampScopeCount >= m_gpuTimestampScopeCapacity) return false;
	const bool depthOnlyPass = m_renderTargetCount == 0 && m_depthStencil != nullptr;
	m_gpuTimestampEvents.push_back({ GpuTimestampEventType::Begin, static_cast<uint32_t>(m_drawCalls.size()), depthOnlyPass, name });
	m_workItems.push_back({ WorkType::GpuTimestamp, static_cast<uint32_t>(m_gpuTimestampEvents.size() - 1u) });
	++m_gpuTimestampDepth;
	++m_gpuTimestampScopeCount;
	return true;
}

void VulkanCommandList::EndGpuTimestamp()
{
	if (m_gpuTimestampDepth == 0) throw std::logic_error("Vulkan GPU timestamp end has no matching begin.");
	const bool depthOnlyPass = m_renderTargetCount == 0 && m_depthStencil != nullptr;
	m_gpuTimestampEvents.push_back({ GpuTimestampEventType::End, static_cast<uint32_t>(m_drawCalls.size()), depthOnlyPass, {} });
	m_workItems.push_back({ WorkType::GpuTimestamp, static_cast<uint32_t>(m_gpuTimestampEvents.size() - 1u) });
	--m_gpuTimestampDepth;
}

void VulkanCommandList::Close()
{
	if (m_debugEventDepth != 0) throw std::logic_error("Vulkan command list closed with an unbalanced debug event.");
	if (m_gpuTimestampDepth != 0) throw std::logic_error("Vulkan command list closed with an unbalanced GPU timestamp.");
	m_isClosed = true;
}

void VulkanCommandList::Dispatch(
	uint32_t threadGroupCountX,
	uint32_t threadGroupCountY,
	uint32_t threadGroupCountZ)
{
	ComputeDispatch dispatch = {};
	dispatch.pipelineState = m_boundComputePipeline;
	dispatch.threadGroupCountX = threadGroupCountX;
	dispatch.threadGroupCountY = threadGroupCountY;
	dispatch.threadGroupCountZ = threadGroupCountZ;
	dispatch.inlineConstantSize = m_pendingPushConstantSize;
	dispatch.storageBuffers = m_pendingStorageBuffers;
	if(m_pendingPushConstantSize > 0u)
	{
		std::memcpy(
			dispatch.inlineConstants.data(),
			m_pendingPushConstants.data(),
			m_pendingPushConstantSize);
	}
	m_computeDispatches.push_back(dispatch);
	m_workItems.push_back(WorkItem{ WorkType::Dispatch, static_cast<uint32_t>(m_computeDispatches.size() - 1u) });
}

void VulkanCommandList::BufferMemoryBarrier(
	dy::RHI::IBuffer* buffer,
	dy::RHI::BufferAccess sourceAccess,
	dy::RHI::BufferAccess destinationAccess,
	uint32_t offset,
	uint32_t size)
{
	if(buffer == nullptr) return;
	m_bufferBarriers.push_back(BufferBarrier{
		buffer,
		sourceAccess,
		destinationAccess,
		offset,
		size });
	m_workItems.push_back(WorkItem{ WorkType::BufferBarrier, static_cast<uint32_t>(m_bufferBarriers.size() - 1u) });
}

}
