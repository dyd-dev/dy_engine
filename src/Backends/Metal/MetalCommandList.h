#pragma once

#include <cstdint>
#include <map>
#include <unordered_map>
#include <utility>
#include <vector>

#include "dyf/RHI/ICommandList.h"

namespace dyf::Backends
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
        bool ReplayNative(const std::vector<std::function<bool(ICommandList&)>>& commands) override;
        void GlobalBarrierNative() override;

		void ResourceBarrierNative(
			const RHI::ResourceBarrierDesc* barriers,
			uint32_t count) override;
		void BeginRenderingNative(const RHI::RenderingDesc& desc) override;
		void EndRenderingNative() override;

		void BindGraphicsPipelineNative(RHI::PipelineHandle pipelineState) override;
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

		[[nodiscard]] bool ValidateForSubmit(MetalSubmissionState& state) const;
		void CommitResourceStates();

	private:
		friend struct MetalObjectDeleter;

		~MetalCommandList() override;

		struct Impl;
		Impl* m_impl = nullptr;
	};
}
