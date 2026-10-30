#pragma once
#include "dyf/Platform/Profiler.h"

#include <cstdint>
#include <functional>
#include <map>
#include <tuple>
#include <memory>
#include <unordered_set>
#include <vector>

#include "Barrier.h"
#include "Format.h"
#include "Indirect.h"
#include "Rendering.h"
#include "Binding.h"
#include "Backend/StateTable.h"

namespace dyf { class ModelRenderer; }

namespace dyf::Platform
{
    class FrameProfiler;
    struct LiveCommandRef;
    struct LiveContext;
    struct LiveEvent;
    enum class LiveCommandKind : std::uint8_t;
    enum class LiveEventKind : std::uint8_t;
    enum class LiveOutcome : std::uint8_t;
}

namespace dyf::RHI
{
    struct CommandStateSummary
    {
            // A transition cycle can restore final without changing the initial requirement.
            ResourceState initial, final;
            bool writes = false;
            constexpr explicit CommandStateSummary(ResourceState state) : initial(state), final(state) {}
            constexpr bool Append(ResourceState before, ResourceState after, bool barrier)
            {
                if(final != before) return false;
                if(barrier) { final = after; writes = true; }
                return true;
            }
    };

	struct DebugLabelColor { float r = 0.3f, g = 0.6f, b = 1.0f, a = 1.0f; };
	class IDevice;
	class ICommandList
	{
	public:
        ICommandList(const ICommandList&) = delete;
        ICommandList& operator=(const ICommandList&) = delete;
        // A recording exception invalidates this list until IDevice::ResetCommandList.
        // The original exception is propagated; Close/Prepare/Submit reject the list.
        // Labels are copied when recorded. Unsupported native capture tools treat them as no-ops.
        void BeginDebugEvent(const char* name, const DebugLabelColor& color = {});
        void EndDebugEvent();
        void InsertDebugMarker(const char* name, const DebugLabelColor& color = {});
        void ResetTimestamps(TimestampQueryHandle,uint32_t first,uint32_t count);
        void WriteTimestamp(TimestampQueryHandle,uint32_t index);
		void ResourceBarrier(const ResourceBarrierDesc* barriers, uint32_t count);
        // Outside rendering: order earlier resource accesses before subsequent ones
        // on this device's queue, including resources not imported into a graph.
        // Preserves states/layouts; transitions and cross-queue/CPU waits are separate.
        void GlobalBarrier();
        // Outside rendering. Distinct buffers must be in CopySource and
        // CopyDestination; offsets and nonzero size are 4-byte aligned and in bounds.
        // Whole-buffer states do not permit copying within the same buffer.
        void CopyBuffer(BufferHandle source, uint32_t sourceOffset,
            BufferHandle destination, uint32_t destinationOffset, uint32_t size);
		void BeginRendering(const RenderingDesc& desc);
		void EndRendering();

		void BindGraphicsPipeline(PipelineHandle pipeline);
        void BindComputePipeline(PipelineHandle pipeline);
        void Dispatch(uint32_t x,uint32_t y,uint32_t z);
        // Requires Feature::IndirectDispatch and ResourceState::IndirectArgument.
        // The argument buffer supplies one DispatchIndirectArguments structure.
        void DispatchIndirect(BufferHandle arguments, uint32_t offset = 0);
		// ResourceSet은 현재 pipeline용이어야 한다. pipeline 교체 후에는 다시 바인딩한다.
		// graphics의 VB/IB/ResourceSet은 rendering 구간 안에서 설정하고 EndRendering에서 해제된다.
		// compute의 ResourceSet은 BindComputePipeline 뒤, rendering 구간 밖에서 설정한다.
		void BindResourceSet(ResourceSetHandle resourceSet);
		void BindVertexBuffer(uint32_t binding, BufferHandle buffer, uint32_t offset);
		void BindIndexBuffer(BufferHandle buffer, Format format, uint32_t offset);
        // offset과 size는 4바이트 배수이며 파이프라인이 선언한 범위 안이어야 한다.
        // 입력 바이트는 기록할 때 복사하므로 호출 뒤 원본 저장소를 해제해도 된다.
        // Graphics 상수는 BeginRendering/EndRendering 안에서, compute 상수는 그 밖에서 기록한다.
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

        // Requires Feature::IndirectDraw and ResourceState::IndirectArgument.
        // GPU argument count/instanceCount zero is a no-op.
        // firstInstance must be zero unless Feature::IndirectFirstInstance is supported.
        // PatchList pipelines additionally require Feature::IndirectTessellation.
        void DrawIndirect(BufferHandle arguments, uint32_t offset = 0);
        // Indexed draws share current bindings/constants. stride is 4-byte aligned
        // and at least sizeof(DrawIndexedIndirectArguments); every argument must fit.
        // drawCount=0 still validates bindings, offset, stride and buffer state,
        // retains the buffer, consumes no argument bytes and emits no native draw.
        void DrawIndexedIndirect(BufferHandle arguments, uint32_t offset = 0,
            uint32_t drawCount = 1, uint32_t stride = sizeof(DrawIndexedIndirectArguments));

		void DispatchMesh(
			uint32_t threadGroupCountX,
			uint32_t threadGroupCountY,
			uint32_t threadGroupCountZ);

		bool Close();

	protected:
        // Only the common recorder replays these hooks. Resource ownership, ranges,
        // binding compatibility and inline-constant completeness are checked there.
        // Native implementations retain API-specific encoding and state/layout tracking.
        ICommandList();
        virtual bool ReplayNative(const std::vector<std::function<bool(ICommandList&)>>& commands);
        virtual void BeginDebugEventNative(const char*, const DebugLabelColor&) {}
        virtual void EndDebugEventNative() {}
        virtual void InsertDebugMarkerNative(const char*, const DebugLabelColor&) {}
        virtual void ResetTimestampsNative(TimestampQueryHandle,uint32_t,uint32_t) {m_recordingFailed=true;}
        virtual void WriteTimestampNative(TimestampQueryHandle,uint32_t) {m_recordingFailed=true;}
		virtual ~ICommandList();
		virtual void ResourceBarrierNative(const ResourceBarrierDesc* barriers, uint32_t count) = 0;
        virtual void GlobalBarrierNative() = 0;
        virtual void CopyBufferNative(BufferHandle,uint32_t,BufferHandle,uint32_t,uint32_t) {m_recordingFailed=true;}
		virtual void BeginRenderingNative(const RenderingDesc& desc) = 0;
		virtual void EndRenderingNative() = 0;
		virtual void BindGraphicsPipelineNative(PipelineHandle pipeline) = 0;
        virtual void BindComputePipelineNative(PipelineHandle) {m_recordingFailed=true;}
        virtual void DispatchNative(uint32_t,uint32_t,uint32_t) {m_recordingFailed=true;}
        virtual void DispatchIndirectNative(BufferHandle,uint32_t) {m_recordingFailed=true;}
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
        virtual void DrawIndirectNative(BufferHandle,uint32_t) {m_recordingFailed=true;}
        virtual void DrawIndexedIndirectNative(BufferHandle,uint32_t,uint32_t,uint32_t) {m_recordingFailed=true;}
		virtual void DispatchMeshNative(uint32_t x, uint32_t y, uint32_t z) { (void)x; (void)y; (void)z; m_recordingFailed = true; }
		virtual bool CloseNative() = 0;

	private:
        friend struct ProfilerAccess;
        friend class dyf::ModelRenderer;
        friend class Platform::FrameProfiler;
        [[nodiscard]] Platform::LiveCommandRef GetLiveCommandPosition() const noexcept;
        struct LiveDeferred;
        std::unique_ptr<LiveDeferred> m_liveDeferred;
        void DeferLiveEvent(const Platform::LiveEvent&) noexcept;
        void FlushLiveDeferred(Platform::LiveContext) noexcept;
        void DiscardLiveDeferred() noexcept;
        void RecordLiveCommand(Platform::LiveCommandKind,std::array<int64_t,5> arguments={},
            const void* resource=nullptr,uint64_t elements=0,uint64_t bytes=0,uint64_t operations=1) noexcept;
        void RecordLiveCommand(Platform::LiveCommandKind,std::array<int64_t,5> arguments,
            const void* resource,uint64_t elements,uint64_t bytes,uint64_t operations,
            Platform::LiveEventKind kind,bool fullUpload=false,const char* label=nullptr) noexcept;
        void RecordLiveResourceLink(std::uint64_t,std::uint64_t) noexcept;
        void RecordLiveInfo(Platform::LiveEvent) noexcept;
        void FinishLiveRecording(Platform::LiveOutcome,bool terminal=false) noexcept;
        void RecordLiveBindings(std::uint64_t,Platform::LiveCommandKind) noexcept;
		friend class IDevice;
        friend class RenderGraph;
        friend class RenderGraphPass;
        friend class RecordedCommandList;
        static ICommandList* CreateRecorded();
        using StateMap=std::map<std::tuple<uintptr_t,uint32_t,uint32_t>,ResourceState>;
        Backend::StateTable<StateMap::key_type,CommandStateSummary,Backend::SubresourceHash> m_closedStates;
        bool m_stateSummaryValid = true;
        std::unordered_set<ResourceSetHandle> m_requiredResourceSets;
        void RequireState(const void*,uint32_t mip,uint32_t layer,ResourceState);
        std::vector<std::function<bool(ICommandList&)>> m_commands;
        // Recorded payloads own their bytes until this list is reset or destroyed.
        std::vector<uint8_t> m_commandBytes;
        ICommandList* m_preparedNative = nullptr;
        bool m_uploadPageReuse=true,m_uploadSuballocation=true;
        uint64_t m_completion=0;
        uint64_t m_imageGeneration=0;
        bool m_rendering=false, m_viewport=false, m_scissor=false;
        Rect m_renderArea = {};
        bool m_stencilAttachment=false,m_stencilReference=false;
        uint32_t m_debugEventDepth = 0;
        PipelineHandle m_pipeline=nullptr;
        BufferHandle m_indexBuffer=nullptr;
        ResourceSetHandle m_resourceSet=nullptr;
        struct VertexBinding { BufferHandle buffer; uint32_t offset; };
        std::map<uint32_t,VertexBinding> m_vertexBuffers;
        Format m_indexFormat=Format::Unknown;
        uint32_t m_indexOffset=0;
        std::vector<Format> m_colorFormats;
        Format m_depthStencilFormat=Format::Unknown;
        ResourceState m_depthStencilState=ResourceState::Undefined;
        bool ValidateBindings(bool indexed,bool compute,uint32_t count=0,uint32_t instances=0,uint32_t first=0,uint32_t firstInstance=0);
        bool ValidateMeshBindings();
        bool ValidateIndirectArguments(BufferHandle, uint32_t offset, uint32_t argumentSize,
            uint32_t drawCount, uint32_t stride);
        bool RequireResourceSetStates(ResourceSetHandle);
		bool Track(const void* handle);
		bool CanRecordCommands();
		IDevice* m_owner = nullptr;
		bool m_recordingClosed = false;
		bool m_recordingFailed = false;
		std::vector<std::shared_ptr<void>> m_references;
        std::unordered_set<const void*> m_referenceHandles;
        // 그래프 전체 참조는 한 번만 보유하고 각 목록은 추가 참조만 따로 기록한다.
        struct ReferenceSnapshot
        {
            std::vector<std::shared_ptr<void>> resources;
            std::unordered_set<const void*> handles;
        };
        std::shared_ptr<const ReferenceSnapshot> m_sharedReferences;
	};
}
