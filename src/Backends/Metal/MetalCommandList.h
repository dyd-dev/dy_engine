#pragma once

#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "dyf/RHI/ICommandList.h"
#include "MetalUploadPool.h"

namespace dyf::Backends
{
	class MetalBuffer;
	class MetalTexture;
	struct MetalObjectDeleter;
	struct MetalUploadPage;

	class MetalCommandList final : public RHI::ICommandList
	{
	public:
		explicit MetalCommandList(void* commandQueue,
			std::shared_ptr<MetalDetail::UploadPagePool<MetalUploadPage>> uploadPool = {});

	protected:
        bool ReplayNative(const std::vector<std::function<bool(ICommandList&)>>& commands) override;
        void GlobalBarrierNative() override;
        void CopyBufferNative(RHI::BufferHandle source, uint32_t sourceOffset,
            RHI::BufferHandle destination, uint32_t destinationOffset, uint32_t size) override;

		void ResourceBarrierNative(
			const RHI::ResourceBarrierDesc* barriers,
			uint32_t count) override;
		void BeginRenderingNative(const RHI::RenderingDesc& desc) override;
		void EndRenderingNative() override;

		void BindGraphicsPipelineNative(RHI::PipelineHandle pipelineState) override;
		void BindComputePipelineNative(RHI::PipelineHandle pipelineState) override;
		void DispatchNative(uint32_t x, uint32_t y, uint32_t z) override;
		void DispatchIndirectNative(RHI::BufferHandle arguments, uint32_t offset) override;
		void BindResourceSetNative(RHI::ResourceSetHandle resourceSet) override;
		void BindVertexBufferNative(
			uint32_t binding,
			RHI::BufferHandle buffer,
			uint32_t offset) override;
		void BindIndexBufferNative(
			RHI::BufferHandle buffer,
			RHI::Format format,
			uint32_t offset) override;
		void SetInlineConstantsNative(
			uint32_t offset,
			uint32_t size,
			const void* data) override;

		void SetViewportNative(const RHI::Viewport& viewport) override;
		void SetScissorNative(const RHI::Rect& rect) override;
		void SetStencilReferenceNative(uint32_t reference) override;

		void DrawInstancedNative(
			uint32_t vertexCount,
			uint32_t instanceCount,
			uint32_t startVertex,
			uint32_t startInstance) override;
		void DrawIndexedInstancedNative(
			uint32_t indexCount,
			uint32_t instanceCount,
			uint32_t firstIndex,
			int32_t vertexOffset,
			uint32_t firstInstance) override;
		void DrawIndirectNative(RHI::BufferHandle arguments, uint32_t offset) override;
		void DrawIndexedIndirectNative(RHI::BufferHandle arguments, uint32_t offset, uint32_t drawCount, uint32_t stride) override;
		void DispatchMeshNative(
			uint32_t threadGroupCountX,
			uint32_t threadGroupCountY,
			uint32_t threadGroupCountZ) override;

		bool CloseNative() override;

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

		void CommitResourceStates();

	private:
		friend class MetalDevice;
		friend struct MetalObjectDeleter;

		~MetalCommandList() override;
		void DispatchCompute(uint32_t x, uint32_t y, uint32_t z,
			MetalBuffer* arguments, uint32_t offset);

		struct Impl;
		Impl* m_impl = nullptr;
	};
}
