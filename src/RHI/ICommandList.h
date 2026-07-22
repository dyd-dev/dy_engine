#pragma once
#include <cstdint>
#include "Format.h"

namespace dy::RHI
{
	class IBuffer;
	class ITexture;
	class IPipelineState;
	class IResourceSet;

	struct Viewport
	{
		float x = 0.0f;
		float y = 0.0f;
		float width = 0.0f;
		float height = 0.0f;
		float minDepth = 0.0f;
		float maxDepth = 1.0f;
	};

	struct Rect
	{
		int32_t x = 0;
		int32_t y = 0;
		uint32_t width = 0;
		uint32_t height = 0;
	};

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

		// output state
		virtual void SetRenderTargets(uint32_t numRenderTargets, ITexture** renderTargets, ITexture* depthStencil) = 0;
		virtual void SetViewport(const Viewport& viewport) = 0;
		virtual void SetScissor(const Rect& rect) = 0;
		virtual void ClearColor(ITexture* renderTarget, float r, float g, float b, float a) = 0;
		virtual void ClearDepth(ITexture* depthStencil, float depth) = 0;

		// draw
		virtual void DrawInstanced(uint32_t vertexCount, uint32_t instanceCount, uint32_t startVertex, uint32_t startInstance) = 0;
		virtual void DrawIndexedInstanced(uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex, int32_t vertexOffset, uint32_t firstInstance) = 0;

		// lifecycle
		virtual void Close() = 0;

		// Compute Commands
		// virtual void DispatchCompute(uint32_t threadGroupCountX, uint32_t threadGroupCountY, uint32_t threadGroupCountZ) = 0;
	};
}
