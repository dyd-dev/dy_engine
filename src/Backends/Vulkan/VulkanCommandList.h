#pragma once
#include "RHI/ICommandList.h"
#include "VulkanDevice.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace dy::Backends
{

// 백엔드 로컬 상한. 렌더러 셰이더 레이아웃을 컴파일타임에 역참조하지 않기 위한 것으로,
// 실제 사용 개수/크기는 런타임 RHI::ShaderLayoutDesc 값으로 결정되고 배열만 이 상한으로 잡는다.
inline constexpr uint32_t kMaxDescriptorBindings = 16u;
inline constexpr uint32_t kMaxMaterialTextures = 8u;
inline constexpr uint32_t kMaxPushConstantBytes = 256u;

class VulkanDevice;

class VulkanCommandList : public dy::RHI::ICommandList
{
public:
	void BindGraphicsPipeline(dy::RHI::IPipelineState* pipelineState) override;
	void BindComputePipeline(dy::RHI::IPipelineState* pipelineState) override;
	void BindGlobalDescriptors() override {}
	void BindGeometry(const dy::RHI::GeometryBinding& geometry) override;
	void BindVertexBuffer(dy::RHI::IBuffer* buffer, uint32_t stride, uint32_t offset) override;
	void BindIndexBuffer(dy::RHI::IBuffer* buffer, dy::RHI::Format format, uint32_t offset) override;
	void BindConstantBuffer(uint32_t binding, dy::RHI::IBuffer* buffer, uint32_t offset, uint32_t size) override;
	void BindStorageBuffer(uint32_t binding, dy::RHI::IBuffer* buffer, uint32_t offset, uint32_t size) override;
	void BindTexture(uint32_t binding, dy::RHI::ITexture* texture) override;
	void SetInlineConstants(uint32_t size, const void* data) override;
	void SetRenderTargets(uint32_t numRenderTargets, dy::RHI::ITexture** renderTargets, dy::RHI::ITexture* depthStencil) override;
	void SetViewport(const dy::RHI::Viewport& viewport) override;
	void SetScissor(const dy::RHI::Rect& rect) override;
	void ClearColor(dy::RHI::ITexture* renderTarget, float r, float g, float b, float a) override;
	void ClearDepth(dy::RHI::ITexture* depthStencil, float depth) override;
	void DrawInstanced(uint32_t vertexCount, uint32_t instanceCount, uint32_t startVertex, uint32_t startInstance) override;
	void DrawIndexedInstanced(uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex, int32_t vertexOffset, uint32_t firstInstance) override;
	void Dispatch(uint32_t threadGroupCountX, uint32_t threadGroupCountY, uint32_t threadGroupCountZ) override;
	void BufferMemoryBarrier(
		dy::RHI::IBuffer* buffer,
		dy::RHI::BufferAccess sourceAccess,
		dy::RHI::BufferAccess destinationAccess,
		uint32_t offset,
		uint32_t size) override;

	void Begin(uint32_t maxColorAttachments, uint32_t maxDrawsPerFrame);
	[[nodiscard]] uint32_t GetRemainingDrawCapacity() const override
	{
		return m_drawCalls.size() < m_maxDrawsPerFrame
			? m_maxDrawsPerFrame - static_cast<uint32_t>(m_drawCalls.size()) : 0u;
	}
	void BeginDebugEvent(const char* name, const dy::RHI::DebugLabelColor& color = {}) override;
	void EndDebugEvent() override;
	void InsertDebugMarker(const char* name, const dy::RHI::DebugLabelColor& color = {}) override;
	bool BeginGpuTimestamp(const char* name) override;
	void EndGpuTimestamp() override;
	void Close() override;

	void SetGpuTimestampScopeCapacity(uint32_t capacity) { m_gpuTimestampScopeCapacity = capacity; }

private:
	struct BufferBinding
	{
		dy::RHI::IBuffer* buffer = nullptr;
		uint32_t offset = 0;
		uint32_t size = 0;
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
		uint32_t pushConstantSize = 0;
		bool hasViewport = false;
		bool hasScissor = false;
		dy::RHI::IPipelineState* pipelineState = nullptr;
		dy::RHI::GeometryBinding geometry = {};
		std::array<BufferBinding, kMaxDescriptorBindings> constantBuffers = {};
		std::array<BufferBinding, kMaxDescriptorBindings> storageBuffers = {};
		std::array<dy::RHI::ITexture*, kMaxDescriptorBindings> textures = {};
		uint32_t renderTargetCount = 0u;
		bool renderTargetsValid = true;
		std::size_t renderTargetOffset = 0u;
		dy::RHI::ITexture* depthStencil = nullptr;
		dy::RHI::Viewport viewport = {};
		dy::RHI::Rect scissor = {};
		std::array<uint8_t, kMaxPushConstantBytes> pushConstants = {};
	};

	struct ComputeDispatch
	{
		dy::RHI::IPipelineState* pipelineState = nullptr;
		uint32_t threadGroupCountX = 0u;
		uint32_t threadGroupCountY = 0u;
		uint32_t threadGroupCountZ = 0u;
		uint32_t inlineConstantSize = 0u;
		std::array<BufferBinding, kMaxDescriptorBindings> storageBuffers = {};
		std::array<uint8_t, kMaxPushConstantBytes> inlineConstants = {};
	};

	struct BufferBarrier
	{
		dy::RHI::IBuffer* buffer = nullptr;
		dy::RHI::BufferAccess sourceAccess = dy::RHI::BufferAccess::ComputeShaderWrite;
		dy::RHI::BufferAccess destinationAccess = dy::RHI::BufferAccess::VertexShaderRead;
		uint32_t offset = 0u;
		uint32_t size = 0u;
	};

	struct DepthClear
	{
		dy::RHI::ITexture* texture = nullptr;
		float depth = 1.0f;
	};

	struct ColorClear
	{
		dy::RHI::ITexture* texture = nullptr;
		std::array<float, 4> color = {};
	};

	enum class WorkType : uint8_t
	{
		Draw,
		Dispatch,
		BufferBarrier,
		ClearColor,
		ClearDepth,
		DebugEvent,
		GpuTimestamp
	};

	struct WorkItem
	{
		WorkType type = WorkType::Draw;
		uint32_t index = 0u;
	};

	enum class DebugEventType
	{
		Begin,
		End,
		Marker
	};

	struct DebugEvent
	{
		DebugEventType type = DebugEventType::Marker;
		uint32_t drawIndex = 0;
		bool depthOnlyPass = false;
		std::string name;
		dy::RHI::DebugLabelColor color = {};
	};

	enum class GpuTimestampEventType
	{
		Begin,
		End
	};

	struct GpuTimestampEvent
	{
		GpuTimestampEventType type = GpuTimestampEventType::Begin;
		uint32_t drawIndex = 0;
		bool depthOnlyPass = false;
		std::string name;
	};

	friend struct VulkanDevice::Impl;
	uint32_t m_maxColorAttachments = 0u;
	uint32_t m_maxDrawsPerFrame = 0u;
	uint32_t m_renderTargetCount = 0;
	bool m_renderTargetsValid = true;
	std::size_t m_renderTargetOffset = 0u;
	std::vector<dy::RHI::ITexture*> m_renderTargets;
	dy::RHI::ITexture* m_depthStencil = nullptr;
	dy::RHI::IPipelineState* m_boundPipeline = nullptr;
	dy::RHI::IPipelineState* m_boundComputePipeline = nullptr;
	std::array<uint8_t, kMaxPushConstantBytes> m_pendingPushConstants = {};
	uint32_t m_pendingPushConstantSize = 0;
	dy::RHI::GeometryBinding m_pendingGeometry = {};
	std::array<BufferBinding, kMaxDescriptorBindings> m_pendingConstantBuffers = {};
	std::array<BufferBinding, kMaxDescriptorBindings> m_pendingStorageBuffers = {};
	std::array<dy::RHI::ITexture*, kMaxDescriptorBindings> m_pendingTextures = {};
	bool m_hasPendingViewport = false;
	bool m_hasPendingScissor = false;
	dy::RHI::Viewport m_pendingViewport = {};
	dy::RHI::Rect m_pendingScissor = {};
	std::vector<DrawCall> m_drawCalls;
	std::vector<ComputeDispatch> m_computeDispatches;
	std::vector<BufferBarrier> m_bufferBarriers;
	std::vector<ColorClear> m_colorClears;
	std::vector<DepthClear> m_depthClears;
	std::vector<WorkItem> m_workItems;
	std::vector<DebugEvent> m_debugEvents;
	uint32_t m_debugEventDepth = 0;
	std::vector<GpuTimestampEvent> m_gpuTimestampEvents;
	uint32_t m_gpuTimestampDepth = 0;
	uint32_t m_gpuTimestampScopeCapacity = 0;
	uint32_t m_gpuTimestampScopeCount = 0;
	bool m_isClosed = false;
};

}
