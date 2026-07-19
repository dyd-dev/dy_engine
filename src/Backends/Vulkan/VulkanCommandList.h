#pragma once
#include "RHI/ICommandList.h"
#include "VulkanDevice.h"
#include <array>
#include <cstdint>
#include <vector>

namespace dy::Backends
{

// 백엔드 로컬 상한. 렌더러 셰이더 레이아웃을 컴파일타임에 역참조하지 않기 위한 것으로,
// 실제 사용 개수/크기는 런타임 RHI::ShaderLayoutDesc 값으로 결정되고 배열만 이 상한으로 잡는다.
inline constexpr uint32_t kMaxDescriptorBindings = 16u;
inline constexpr uint32_t kMaxMaterialTextures = 8u;
inline constexpr uint32_t kMaxPushConstantBytes = 256u;
inline constexpr uint32_t kDefaultMaxRenderTargets = 4u;

class VulkanDevice;

class VulkanCommandList : public dy::RHI::ICommandList
{
public:
	void BindGraphicsPipeline(dy::RHI::IPipelineState* pipelineState) override;
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
	void Close() override { m_isClosed = true; }

	void Begin();
	void End();

private:
	static constexpr uint32_t kMaxConstantBufferBindings = kMaxDescriptorBindings;
	static constexpr uint32_t kMaxRenderTargets = kDefaultMaxRenderTargets;
	static constexpr uint32_t kMaxTextureBindings = kMaxDescriptorBindings;

	struct ConstantBufferBinding
	{
		dy::RHI::IBuffer* buffer = nullptr;
		uint32_t offset = 0;
		uint32_t size = 0;
	};

	struct StorageBufferBinding
	{
		dy::RHI::IBuffer* buffer = nullptr;
		uint32_t offset = 0;
		uint32_t size = 0;
	};

	struct PassRecord
	{
		uint32_t firstDraw = 0;
		uint32_t renderTargetCount = 0;
		std::array<dy::RHI::ITexture*, kMaxRenderTargets> renderTargets = {};
		dy::RHI::ITexture* depthStencil = nullptr;
		std::array<float, 4> clearColor = { 0.4f, 0.7f, 1.0f, 1.0f };
		float clearDepth = 1.0f;
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
		uint32_t vertexStride = 0;
		dy::RHI::GeometryBinding geometry = {};
		std::array<ConstantBufferBinding, kMaxConstantBufferBindings> constantBuffers = {};
		std::array<StorageBufferBinding, kMaxConstantBufferBindings> storageBuffers = {};
		std::array<dy::RHI::ITexture*, kMaxTextureBindings> textures = {};
		dy::RHI::Viewport viewport = {};
		dy::RHI::Rect scissor = {};
		std::array<uint8_t, kMaxPushConstantBytes> pushConstants = {};
	};

	friend struct VulkanDevice::Impl;
	std::array<float, 4> m_clearColor = { 0.4f, 0.7f, 1.0f, 1.0f };
	float m_clearDepth = 1.0f;
	dy::RHI::IPipelineState* m_boundPipeline = nullptr;
	std::array<uint8_t, kMaxPushConstantBytes> m_pendingPushConstants = {};
	uint32_t m_pendingPushConstantSize = 0;
	dy::RHI::GeometryBinding m_pendingGeometry = {};
	std::array<ConstantBufferBinding, kMaxConstantBufferBindings> m_pendingConstantBuffers = {};
	std::array<StorageBufferBinding, kMaxConstantBufferBindings> m_pendingStorageBuffers = {};
	std::array<dy::RHI::ITexture*, kMaxTextureBindings> m_pendingTextures = {};
	bool m_hasPendingViewport = false;
	bool m_hasPendingScissor = false;
	dy::RHI::Viewport m_pendingViewport = {};
	dy::RHI::Rect m_pendingScissor = {};
	std::vector<PassRecord> m_passRecords;
	std::vector<DrawCall> m_drawCalls;
	bool m_isClosed = false;
};

}
