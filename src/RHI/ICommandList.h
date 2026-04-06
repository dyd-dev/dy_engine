#pragma once
/* CommandList.h
* 
* 그리기 명령, 파이프라인 배리어를 기록하는 객체입니다.
* CPU에서 명령을 작성하고 GPU가 이를 실행합니다.
*
* CommandList Allocator에 접근할 때 thread_local을 사용하십시오.
* Mutex Lock이 필요 없으며 Thread-Safe Code를 작성할 수 있습니다.
*/
#include <cstdint>
#include "Enums.h"

namespace dy::RHI
{
	class IBuffer;
	class ITexture;
	class IPipelineState;

	class ICommandList
	{
	public:
		virtual ~ICommandList() = default;

		// Pipeline and Start Setup
		// 현대 API : PSO 사용 (Pipeline State Object)
		// 구형 API : SetBlendState, SetDepth, SetShader ...
		// Bind the global state.
		virtual void BindGraphicsPipeline(IPipelineState* pipelineState) = 0;

		// Binds the single Global Descriptor Heap containing ALL textures and buffers.
		// Must be called once per pass before Draw.
		virtual void BindGlobalDescriptorHeap() = 0;

		virtual void BindIndexBuffer(IBuffer* buffer, Format format, uint32_t offset) = 0;
		virtual void BindVertexBuffer(IBuffer* buffer) = 0;
		virtual void SetViewport(float width, float height) = 0;
		virtual void SetConstantBuffer(uint32_t slot, IBuffer* buffer) = 0;

		// Modern DOD Approach: Inject tiny data (e.g., Transform Index, Material Index) directly.
		// Replaces ALL BindBuffer and BindTexture calls.
		virtual void SetPushConstants(uint32_t size, const void* data) = 0;

		// Render Targets & Clears
		virtual void SetRenderTargets(uint32_t numRenderTargets, ITexture** renderTargets, ITexture* depthStencil) = 0;
		virtual void ClearColor(ITexture* renderTarget, float r, float g, float b, float a) = 0;
		virtual void ClearDepth(ITexture* depthStencil, float depth) = 0;

		// Draw Commands
		virtual void DrawInstanced(uint32_t vertexCount, uint32_t instanceCount, uint32_t startVertex, uint32_t startInstance) = 0;
		// virtual void DrawIndexedInstancedIndirect(IBuffer* argumentBuffer, uint32_t alignedByteOffset) = 0;

		// Synchronization
		// GPU 내 교통 정리 함수. (write 중 read 불가 등
		virtual void ResourceBarrier(IBuffer* buffer, ResourceState before, ResourceState after) = 0;
		virtual void ResourceBarrier(ITexture* texture, ResourceState before, ResourceState after) = 0;

		// Must be called after all commands are recorded for this thread's workload.
		// Cannot submit a command list that is not closed.
		virtual void Close() = 0;

		// Compute Commands
		// GPGPU Thread Group을 깨우는 명령
		// Frustum Culling, Physics, Ray Tracing 등등 GPU의 병렬 수학 계산 능력을 활용.
		// virtual void DispatchCompute(uint32_t threadGroupCountX, uint32_t threadGroupCountY, uint32_t threadGroupCountZ) = 0;
	};
}