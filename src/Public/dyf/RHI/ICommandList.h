#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <tuple>
#include <memory>
#include <vector>

#include "Barrier.h"
#include "Format.h"
#include "Rendering.h"
#include "ResourceHandles.h"
#include "Query.h"

namespace dyf::RHI
{
	class IDevice;
	class ICommandList
	{
	public:
        void ResetTimestamps(TimestampQueryHandle,uint32_t first,uint32_t count);
        void WriteTimestamp(TimestampQueryHandle,uint32_t index);
		void ResourceBarrier(const ResourceBarrierDesc* barriers, uint32_t count);
		void BeginRendering(const RenderingDesc& desc);
		void EndRendering();

		void BindGraphicsPipeline(PipelineHandle pipeline);
        void BindComputePipeline(PipelineHandle pipeline);
        void Dispatch(uint32_t x,uint32_t y,uint32_t z);
		// ResourceSet은 현재 pipeline용이어야 한다. pipeline 교체 후에는 다시 바인딩한다.
		// graphics의 VB/IB/ResourceSet은 rendering 구간 안에서 설정하고 EndRendering에서 해제된다.
		// compute의 ResourceSet은 BindComputePipeline 뒤, rendering 구간 밖에서 설정한다.
		void BindResourceSet(ResourceSetHandle resourceSet);
		void BindVertexBuffer(uint32_t binding, BufferHandle buffer, uint32_t offset);
		void BindIndexBuffer(BufferHandle buffer, Format format, uint32_t offset);
        // offset과 size는 4바이트 배수이며 파이프라인이 선언한 범위 안이어야 한다.
        // 입력 바이트는 기록할 때 복사하므로 호출 뒤 원본 저장소를 해제해도 된다.
		void SetInlineConstants(uint32_t offset, uint32_t size, const void* data);

		void SetViewport(const Viewport& viewport);
		void SetScissor(const Rect& rect);
		// D24S8의 8비트 스텐실 값(0..255). 초과 비트를 버리지 않고 기록에 실패한다.
		void SetStencilReference(uint32_t reference);

		void DrawInstanced(
			uint32_t vertexCount,
			uint32_t instanceCount,
			uint32_t startVertex,
			uint32_t startInstance);
		void DrawIndexedInstanced(
			uint32_t indexCount,
			uint32_t instanceCount,
			uint32_t firstIndex,
			int32_t vertexOffset,
			uint32_t firstInstance);

		bool Close();

	protected:
        virtual void ResetTimestampsNative(TimestampQueryHandle,uint32_t,uint32_t) {m_recordingFailed=true;}
        virtual void WriteTimestampNative(TimestampQueryHandle,uint32_t) {m_recordingFailed=true;}
		virtual ~ICommandList();
		virtual void ResourceBarrierNative(const ResourceBarrierDesc* barriers, uint32_t count) = 0;
		virtual void BeginRenderingNative(const RenderingDesc& desc) = 0;
		virtual void EndRenderingNative() = 0;
		virtual void BindGraphicsPipelineNative(PipelineHandle pipeline) = 0;
        virtual void BindComputePipelineNative(PipelineHandle) {m_recordingFailed=true;}
        virtual void DispatchNative(uint32_t,uint32_t,uint32_t) {m_recordingFailed=true;}
		virtual void BindResourceSetNative(ResourceSetHandle resourceSet) = 0;
		virtual void BindVertexBufferNative(uint32_t binding, BufferHandle buffer, uint32_t offset) = 0;
		virtual void BindIndexBufferNative(BufferHandle buffer, Format format, uint32_t offset) = 0;
		virtual void SetInlineConstantsNative(uint32_t offset, uint32_t size, const void* data) = 0;
		virtual void SetViewportNative(const Viewport& viewport) = 0;
		virtual void SetScissorNative(const Rect& rect) = 0;
		virtual void SetStencilReferenceNative(uint32_t reference) = 0;
		virtual void DrawInstancedNative(
			uint32_t vertexCount,
			uint32_t instanceCount,
			uint32_t startVertex,
			uint32_t startInstance) = 0;
		virtual void DrawIndexedInstancedNative(
			uint32_t indexCount,
			uint32_t instanceCount,
			uint32_t firstIndex,
			int32_t vertexOffset,
			uint32_t firstInstance) = 0;
		virtual bool CloseNative() = 0;

	private:
		friend class IDevice;
        friend class RecordedCommandList;
        static ICommandList* CreateRecorded();
        using StateMap=std::map<std::tuple<uintptr_t,uint32_t,uint32_t>,ResourceState>;
        std::vector<std::function<bool(StateMap&)>> m_stateOperations;
        void RequireState(const void*,uint32_t mip,uint32_t layer,ResourceState);
        std::vector<std::function<bool(ICommandList&)>> m_commands;
        uint64_t m_completion=0;
        uint64_t m_imageGeneration=0;
        bool m_rendering=false, m_viewport=false, m_scissor=false;
        PipelineHandle m_pipeline=nullptr;
        BufferHandle m_indexBuffer=nullptr;
        ResourceSetHandle m_resourceSet=nullptr;
        std::map<uint32_t,BufferHandle> m_vertexBuffers;
        std::vector<Format> m_colorFormats;
        Format m_depthStencilFormat=Format::Unknown;
        bool ValidateBindings(bool indexed, bool compute);
        bool RequireResourceSetStates(ResourceSetHandle);
		bool Track(const void* handle);
		bool CanRecordCommands();
		IDevice* m_owner = nullptr;
		bool m_recordingClosed = false;
		bool m_recordingFailed = false;
		std::vector<std::shared_ptr<void>> m_references;
	};
}
