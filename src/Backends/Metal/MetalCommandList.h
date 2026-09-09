#pragma once

#include <cstdint>
#include <map>
#include <unordered_map>
#include <utility>
#include <vector>

#include "RHI/ICommandList.h"

namespace dy::Backends
{
	class MetalBuffer;
	class MetalTexture;
	struct MetalObjectDeleter;

	struct MetalSubmissionState
	{
		std::unordered_map<MetalBuffer*, RHI::ResourceState> buffers;
		std::map<std::pair<MetalTexture*, uint32_t>, RHI::ResourceState> textureSubresources;
	};

	class MetalCommandList final : public RHI::ICommandList
	{
	public:
		explicit MetalCommandList(void* commandQueue);

		void ResourceBarrier(
			const RHI::ResourceBarrierDesc* barriers,
			uint32_t count) override;
		void BeginRendering(const RHI::RenderingDesc& desc) override;
		void EndRendering() override;

		void BindGraphicsPipeline(RHI::PipelineHandle pipelineState) override;
		void BindResourceSet(RHI::ResourceSetHandle resourceSet) override;
		void BindVertexBuffer(
			uint32_t binding,
			RHI::BufferHandle buffer,
			uint32_t offset) override;
		void BindIndexBuffer(
			RHI::BufferHandle buffer,
			RHI::Format format,
			uint32_t offset) override;
		void SetInlineConstants(
			uint32_t offset,
			uint32_t size,
			const void* data) override;

		void SetViewport(const RHI::Viewport& viewport) override;
		void SetScissor(const RHI::Rect& rect) override;
		void SetStencilReference(uint32_t reference) override;

		void DrawInstanced(
			uint32_t vertexCount,
			uint32_t instanceCount,
			uint32_t startVertex,
			uint32_t startInstance) override;
		void DrawIndexedInstanced(
			uint32_t indexCount,
			uint32_t instanceCount,
			uint32_t firstIndex,
			int32_t vertexOffset,
			uint32_t firstInstance) override;

		void Close() override;

		[[nodiscard]] bool Begin();
		void Reset();
		[[nodiscard]] bool IsClosed() const;
		[[nodiscard]] bool IsValid() const;
		[[nodiscard]] bool UsesBackBuffer() const;
		[[nodiscard]] void* GetNativeCommandBuffer() const;

		[[nodiscard]] bool RecordBufferUpdate(
			MetalBuffer* buffer,
			uint32_t offset,
			const void* data,
			uint32_t size);
		[[nodiscard]] bool RecordTextureUpdate(
			MetalTexture* texture,
			uint32_t mipLevel,
			uint32_t arrayLayer,
			const void* data,
			uint32_t dataSize,
			uint32_t rowPitch,
			uint32_t slicePitch);

		[[nodiscard]] bool ValidateForSubmit(MetalSubmissionState& state) const;
		void CommitResourceStates();

	private:
		friend struct MetalObjectDeleter;

		~MetalCommandList() override;

		struct Impl;
		Impl* m_impl = nullptr;
	};
}
