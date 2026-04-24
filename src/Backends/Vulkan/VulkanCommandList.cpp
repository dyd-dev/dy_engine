#include "VulkanCommandList.h"
#include <algorithm>
#include <cstring>

void VulkanCommandList::Begin()
{
	m_clearColor = { { 0.4f, 0.7f, 1.0f, 1.0f } };
	m_boundPipeline = nullptr;
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
	drawCall.pushConstantSize = m_pendingPushConstantSize;
	if (m_pendingPushConstantSize > 0) {
		memcpy(drawCall.pushConstants.data(), m_pendingPushConstants.data(), m_pendingPushConstantSize);
	}
	m_drawCalls.push_back(drawCall);
}

void VulkanCommandList::End()
{
}
