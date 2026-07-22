#pragma once
#include <cstdint>
#include "Format.h"
#include "Rendering.h"
#include "ResourceBarrier.h"

namespace dy::RHI
{
	class IBuffer;
	class IPipelineState;
	class IResourceSet;
	class ICommandList
	{
	public:
		virtual ~ICommandList() = default;

		// pipeline
		virtual void BindGraphicsPipeline(IPipelineState* pipelineState) = 0;
		virtual void BindResourceSet(IResourceSet* resourceSet) = 0;

		// Input Assembly
		virtual void BindVertexBuffer(uint32_t slot, IBuffer* buffer, uint32_t offset) = 0;
		virtual void BindIndexBuffer(IBuffer* buffer, Format format, uint32_t offset) = 0;

		// shader constants
		virtual void SetInlineConstants(uint32_t offset, uint32_t size, const void* data) = 0;
		virtual void Barrier(const TextureBarrier* barriers, uint32_t barrierCount) = 0;

		// rendering scope
		virtual void BeginRendering(const RenderingInfo& renderingInfo) = 0;
		virtual void EndRendering() = 0;
		virtual void SetViewport(const Viewport& viewport) = 0;
		virtual void SetScissor(const Rect& rect) = 0;

		// draw
		virtual void DrawInstanced(uint32_t vertexCount, uint32_t instanceCount, uint32_t startVertex, uint32_t startInstance) = 0;
		virtual void DrawIndexedInstanced(uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex, int32_t vertexOffset, uint32_t firstInstance) = 0;

		// lifecycle
		virtual void Close() = 0;

		// Compute Commands
		// virtual void DispatchCompute(uint32_t threadGroupCountX, uint32_t threadGroupCountY, uint32_t threadGroupCountZ) = 0;
	};
}
