#pragma once

#include <cstdint>

#include "Barrier.h"
#include "Format.h"
#include "Rendering.h"
#include "ResourceHandles.h"

namespace dy::RHI
{
	class ICommandList
	{
	public:
		virtual void ResourceBarrier(const ResourceBarrierDesc* barriers, uint32_t count) = 0;
		virtual void BeginRendering(const RenderingDesc& desc) = 0;
		virtual void EndRendering() = 0;

		virtual void BindGraphicsPipeline(PipelineHandle pipeline) = 0;
		virtual void BindResourceSet(ResourceSetHandle resourceSet) = 0;
		virtual void BindVertexBuffer(uint32_t binding, BufferHandle buffer, uint32_t offset) = 0;
		virtual void BindIndexBuffer(BufferHandle buffer, Format format, uint32_t offset) = 0;
		virtual void SetInlineConstants(uint32_t offset, uint32_t size, const void* data) = 0;

		virtual void SetViewport(const Viewport& viewport) = 0;
		virtual void SetScissor(const Rect& rect) = 0;
		virtual void SetStencilReference(uint32_t reference) = 0;

		virtual void DrawInstanced(
			uint32_t vertexCount,
			uint32_t instanceCount,
			uint32_t startVertex,
			uint32_t startInstance) = 0;
		virtual void DrawIndexedInstanced(
			uint32_t indexCount,
			uint32_t instanceCount,
			uint32_t firstIndex,
			int32_t vertexOffset,
			uint32_t firstInstance) = 0;

		virtual void Close() = 0;

	protected:
		virtual ~ICommandList() = default;
	};
}
