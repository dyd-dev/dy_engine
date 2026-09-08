#pragma once
#include <cstdint>
#include "Format.h"

namespace dy::RHI
{
	class IBuffer;
	class ITexture;
	class IPipelineState;

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

	struct GeometryBinding
	{
		IBuffer* vertexBuffer = nullptr;
		uint32_t vertexStride = 0;
		uint32_t vertexOffset = 0;
		IBuffer* indexBuffer = nullptr;
		Format indexFormat = Format::Unknown;
		uint32_t indexOffset = 0;
	};

	enum class BufferAccess : uint8_t
	{
		ComputeShaderWrite,
		VertexShaderRead
	};

	struct DebugLabelColor
	{
		float r = 1.0f;
		float g = 1.0f;
		float b = 1.0f;
		float a = 1.0f;
	};

	class ICommandList
	{
	public:
		virtual ~ICommandList() = default;

		// pipeline
		virtual void BindGraphicsPipeline(IPipelineState* pipelineState) = 0;
		virtual void BindComputePipeline(IPipelineState* pipelineState) { (void)pipelineState; }
		virtual void BindGlobalDescriptors() = 0;

		// geometry binding / Input Assembly
		virtual void BindGeometry(const GeometryBinding& geometry) = 0;
		virtual void BindVertexBuffer(IBuffer* buffer, uint32_t stride, uint32_t offset) = 0;
		virtual void BindIndexBuffer(IBuffer* buffer, Format format, uint32_t offset) = 0;

		// descriptor binding
		virtual void BindConstantBuffer(uint32_t binding, IBuffer* buffer, uint32_t offset, uint32_t size) { (void)binding; (void)buffer; (void)offset; (void)size; }
		virtual void BindTexture(uint32_t binding, ITexture* texture) { (void)binding; (void)texture; }
		virtual void BindStorageBuffer(uint32_t binding, IBuffer* buffer, uint32_t offset, uint32_t size)
		{
			(void)binding; (void)buffer; (void)offset; (void)size;
		}

		// shader constants
		virtual void SetInlineConstants(uint32_t size, const void* data) = 0;

		// output state
		virtual void SetRenderTargets(uint32_t numRenderTargets, ITexture** renderTargets, ITexture* depthStencil) = 0;
		virtual void SetViewport(const Viewport& viewport) = 0;
		virtual void SetScissor(const Rect& rect) = 0;
		virtual void ClearColor(ITexture* renderTarget, float r, float g, float b, float a) = 0;
		virtual void ClearDepth(ITexture* depthStencil, float depth) = 0;

		// draw
		virtual void DrawInstanced(uint32_t vertexCount, uint32_t instanceCount, uint32_t startVertex, uint32_t startInstance) = 0;
		virtual void DrawIndexedInstanced(uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex, int32_t vertexOffset, uint32_t firstInstance) = 0;
		// Remaining draw slots for optional work; backends without a fixed limit return UINT32_MAX.
		[[nodiscard]] virtual uint32_t GetRemainingDrawCapacity() const { return UINT32_MAX; }

		// compute
		virtual void Dispatch(uint32_t threadGroupCountX, uint32_t threadGroupCountY, uint32_t threadGroupCountZ)
		{
			(void)threadGroupCountX; (void)threadGroupCountY; (void)threadGroupCountZ;
		}
		virtual void BufferMemoryBarrier(
			IBuffer* buffer,
			BufferAccess sourceAccess,
			BufferAccess destinationAccess,
			uint32_t offset,
			uint32_t size)
		{
			(void)buffer; (void)sourceAccess; (void)destinationAccess; (void)offset; (void)size;
		}
		// API-neutral GPU command annotations. Backends translate these to PIX events,
		// Vulkan debug labels, Metal debug groups, or validation-only Null events.
		virtual void BeginDebugEvent(const char* name, const DebugLabelColor& color = {}) { (void)name; (void)color; }
		virtual void EndDebugEvent() {}
		virtual void InsertDebugMarker(const char* name, const DebugLabelColor& color = {}) { (void)name; (void)color; }

		// Records a named GPU timestamp pair. Unsupported backends return false and
		// the RAII wrapper below becomes a no-op. Results are exposed by IDevice only
		// after the frame that owns the queries has completed on the GPU.
		[[nodiscard]] virtual bool BeginGpuTimestamp(const char* name) { (void)name; return false; }
		virtual void EndGpuTimestamp() {}

		// lifecycle
		virtual void Close() = 0;
	};

	class CommandGpuTimestampScope final
	{
	public:
		CommandGpuTimestampScope(ICommandList* commandList, const char* name)
			: m_commandList(commandList)
		{
			if(m_commandList == nullptr || !m_commandList->BeginGpuTimestamp(name))
			{
				m_commandList = nullptr;
			}
		}

		~CommandGpuTimestampScope()
		{
			End();
		}

		CommandGpuTimestampScope(const CommandGpuTimestampScope&) = delete;
		CommandGpuTimestampScope& operator=(const CommandGpuTimestampScope&) = delete;

		CommandGpuTimestampScope(CommandGpuTimestampScope&& other) noexcept
			: m_commandList(other.m_commandList)
		{
			other.m_commandList = nullptr;
		}

		void End()
		{
			if(m_commandList == nullptr) return;
			m_commandList->EndGpuTimestamp();
			m_commandList = nullptr;
		}

	private:
		ICommandList* m_commandList = nullptr;
	};

	class CommandDebugEventScope final
	{
	public:
		CommandDebugEventScope(ICommandList* commandList, const char* name, const DebugLabelColor& color = {})
			: m_commandList(commandList)
		{
			if(m_commandList != nullptr) m_commandList->BeginDebugEvent(name, color);
		}

		~CommandDebugEventScope()
		{
			End();
		}

		CommandDebugEventScope(const CommandDebugEventScope&) = delete;
		CommandDebugEventScope& operator=(const CommandDebugEventScope&) = delete;

		CommandDebugEventScope(CommandDebugEventScope&& other) noexcept
			: m_commandList(other.m_commandList)
		{
			other.m_commandList = nullptr;
		}

		void End()
		{
			if(m_commandList == nullptr) return;
			m_commandList->EndDebugEvent();
			m_commandList = nullptr;
		}

	private:
		ICommandList* m_commandList = nullptr;
	};
}
