#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <tuple>
#include "Binding.h"
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#include "Swapchain.h"
#include "Submission.h"
#include "Feature.h"
#include "Format.h"
#include "dyf/Platform/Profiler.h"
#include <stdexcept>

namespace dyf { class Renderer; class ModelRenderer; }

namespace dyf::Platform
{
    class FrameProfiler;
    struct LiveFrameKey;
    enum class LiveOutcome : std::uint8_t;
    enum class LiveObjectKind : std::uint8_t;
}

namespace dyf::RHI
{
	class IDevice;
	class ICommandList;
	struct BufferDesc;
	struct GraphicsPipelineDesc;
    struct PipelineLayoutDesc;
    struct SamplerDesc;
    struct ComputePipelineDesc;
    struct MeshPipelineDesc;
	struct ResourceSetDesc;
	struct ShaderDesc;
	struct TextureDesc;
	struct TextureReadback;

    // 장치가 실제로 제공하는 한도다. 바이트 단위와 정렬 조건은 값 이름에 따른다.
    enum class Limit : uint8_t
    {
        InlineConstantBytes, Texture2DDimension,
        UniformBufferOffsetAlignment, StorageBufferOffsetAlignment,
        UniformBufferBytes, StorageBufferBytes, SamplerAnisotropy, TessellationPatchControlPoints,
        TextureArrayLayers,
        // Required byte alignment for both UpdateBuffer offset and size; 1 permits byte ranges.
        BufferUpdateAlignment
    };

    enum class FormatFeature : uint8_t { LinearFiltering, CompressedSampling };

	struct DeviceDesc
	{
		uint32_t maxFramesInFlight = 2;
        uint32_t adapterIndex = 0;
        bool enableValidation = false;
	};

    // 결과는 ReadTimestamps로 읽는다. 쿼리 객체는 핸들로만 사용한다.
    struct TimestampQueryDesc { uint32_t count = 0; };
    enum class CpuTimestampDomain : uint8_t { Unsupported, QueryPerformanceCounter };
    struct TimestampCalibration
    {
        CpuTimestampDomain cpuDomain = CpuTimestampDomain::Unsupported;
        uint64_t cpuTicks = 0, cpuFrequency = 0, gpuTicks = 0;
        double gpuPeriodNanoseconds = 0;
        uint32_t gpuValidBits = 0;
        std::optional<uint64_t> maximumDeviationNanoseconds;
    };
    // Upload-page statistics for profiling; request bytes include row padding, not alignment gaps.
    struct UploadMemoryStatistics
    {
        uint64_t allocationCount=0, reuseCount=0, allocationBytes=0;
        uint64_t requestCount=0, requestedBytes=0;
        uint64_t cachedBytes=0, inUseBytes=0, peakInUseBytes=0;
    };

    struct ResourceCounter { uint64_t live = 0, created = 0, destroyed = 0; };
    struct ResourceAllocationCounters { ResourceCounter buffers, textures, pipelines; };

	class IDevice
	{
	public:
		// 생성·기록 시 네이티브 제약에 의한 거절/적용값 변경은 stderr로 알린다.
		// Supports/GetLimit 조회는 로그를 출력하지 않는다. 동등한 내부 표현은 공개 필드의 설명을 따른다.
		virtual ~IDevice();

		[[nodiscard]] static IDevice* Create(const DeviceDesc& desc);
		[[nodiscard]] bool Supports(Feature) const;
        [[nodiscard]] bool Supports(Format,FormatFeature) const noexcept;
        // Counts device-created resources, including resources retained by pending GPU work.
        // Swapchain-owned images are excluded.
        [[nodiscard]] ResourceAllocationCounters GetResourceAllocationCounters() const;
        [[nodiscard]] UploadMemoryStatistics GetUploadMemoryStatistics() const;
        // 설정의 공통 규칙과 장치 한도를 검사한다. 셰이더는 이 장치에서 생성한 핸들을 사용한다.
        // 조회는 자원을 생성하지 않으며, 이후 생성 시 메모리·셰이더 오류까지 보장하지는 않는다.
        [[nodiscard]] bool Supports(const PipelineLayoutDesc&) const;
        [[nodiscard]] bool Supports(const GraphicsPipelineDesc&) const;
        [[nodiscard]] bool Supports(const MeshPipelineDesc&) const;
        [[nodiscard]] bool Supports(const SamplerDesc&) const;
        [[nodiscard]] uint64_t GetLimit(Limit) const;
        [[nodiscard]] bool IsLost() const;
        [[nodiscard]] const DeviceDesc& GetDesc() const { return m_desc; }
		[[nodiscard]] bool CreateSwapchain(const SwapchainDesc& desc);
        // Vulkan waits may dispatch Win32 sent messages. Their callbacks must not
        // destroy/mutate this device; recursively waiting for the same Present returns false.
		[[nodiscard]] bool WaitIdle();
        [[nodiscard]] TimestampQueryHandle CreateTimestampQuery(const TimestampQueryDesc&);
        void DestroyTimestampQuery(TimestampQueryHandle);
        // Returns false until the latest submitted resets/writes for every requested query complete.
        // Recording alone does not invalidate completed results. Values are raw GPU ticks.
        [[nodiscard]] bool ReadTimestamps(TimestampQueryHandle,uint32_t first,uint32_t count,uint64_t* ticks);
        [[nodiscard]] double GetTimestampPeriodNanoseconds() const {return GetTimestampPeriodNative();}
        [[nodiscard]] uint32_t GetTimestampValidBits() const {return GetTimestampValidBitsNative();}
        // Returns a raw clock pair; failure clears the output, without waiting for GPU work.
        [[nodiscard]] bool GetTimestampCalibration(TimestampCalibration&) const;


        // 성공하면 Present까지 같은 backbuffer를 사용한다. 중간에 여러 번 Submit할 수 있다.
		[[nodiscard]] bool BeginFrame();
        // Owner-thread observation; false means no frame remains to present.
        [[nodiscard]] bool HasActiveFrame() const noexcept {return m_frameActive;}
		// Each acquired list is independent and may be recorded by one worker thread.
		// Join recording before Submit/Reset/Destroy. Keep shared resources alive and
		// immutable; serialize frame/swapchain lifetime changes with recording.
		[[nodiscard]] ICommandList* AcquireCommandList();
        // Translate closed lists into independent native command buffers before Submit.
        // Only native recording runs on workers; allocation, validation and submission
        // remain owned by the common RHI. Join before changing lists/device lifetime.
        // Resources and frame/swapchain state must remain unchanged during this call.
        // Workers are owned by the device. threadCount=0 selects hardware concurrency;
        // 1 records on the calling thread. All recording finishes before returning.
        [[nodiscard]] bool PrepareCommandLists(ICommandList* const* commandLists, uint32_t count,
            uint32_t threadCount = 1);
        [[nodiscard]] bool ResetCommandList(ICommandList*);
        // Destroy invalidates the caller's pointer immediately. Storage may be reused;
        // stale pointers are not generation-checked and must never be passed back.
        void DestroyCommandList(ICommandList*);

		// 호출 순서대로 실행하며 프레임을 끝내지 않는다. 제출한 명령과 자원은 완료까지 보유한다.
        // GPU 작업 중인 목록은 Reset하거나 다시 Submit할 수 없다.
		[[nodiscard]] bool Submit(ICommandList** commandLists, uint32_t count);
        [[nodiscard]] bool Submit(const SubmitDesc&, FenceHandle& completion);
        [[nodiscard]] bool IsComplete(FenceHandle);
        [[nodiscard]] bool Wait(FenceHandle,uint64_t timeoutNanoseconds);
        // 제출 후 backbuffer가 Present 상태일 때 표시를 요청하고 프레임을 끝낸다.
        // resize로 표시를 생략하고 swapchain 재생성을 예약한 경우도 정상 프레임 종료로 true를 반환한다.
        // 상태 오류는 프레임을 유지한다. 표시 요청 실패는 false와 stderr로 알린다.
        // 비동기 표시에서는 true가 요청 접수를 뜻하며, 이후 네이티브 실패는 다음 BeginFrame/Present/WaitIdle에서 보고한다.
        [[nodiscard]] bool Present();

		// Swapchain 소유 handle이다. DestroyTexture에 전달하지 않는다.
		[[nodiscard]] TextureHandle GetBackBuffer();

		// Synchronous diagnostic readback of mip 0 / layer 0 in RGBA8 or BGRA8.
		// Submit all recorded work first. Resource state is preserved. A swapchain
		// image must have allowReadback enabled and be read after Submit, before Present.
		// Returns false for unsupported formats, invalid use, or a non-rasterizing backend.
		[[nodiscard]] bool ReadTexture(TextureHandle texture, TextureReadback& result);

		[[nodiscard]] BufferHandle CreateBuffer(const BufferDesc& desc);
		[[nodiscard]] TextureHandle CreateTexture(const TextureDesc& desc);
		[[nodiscard]] ShaderHandle CreateShader(const ShaderDesc& desc);
		[[nodiscard]] PipelineHandle CreateGraphicsPipeline(const GraphicsPipelineDesc& desc);
        [[nodiscard]] PipelineHandle CreateComputePipeline(const ComputePipelineDesc& desc);
        [[nodiscard]] PipelineHandle CreateMeshPipeline(const MeshPipelineDesc& desc);
		[[nodiscard]] ResourceSetHandle CreateResourceSet(const ResourceSetDesc& desc);

        // Destroy invalidates caller handles; recorded/submitted work retains its own leases.
        // All handles expire when this device is destroyed, regardless of address reuse.
		void DestroyBuffer(BufferHandle buffer);
		void DestroyTexture(TextureHandle texture);
		void DestroyShader(ShaderHandle shader);
		void DestroyPipeline(PipelineHandle pipeline);
		void DestroyResourceSet(ResourceSetHandle resourceSet);

		// 업로드 입력은 호출 중 복사하므로 반환 후 원본 메모리를 해제하거나 바꿀 수 있다.
		// GPU 복사는 명령 목록에 기록되며 Submit으로 실행한다. UpdateTexture도 같은 수명 규칙을 따른다.
        // offset/size must satisfy GetLimit(BufferUpdateAlignment); rejected here before payload copying.
        // Vulkan/D3D12/iOS permit byte ranges; Metal macOS requires multiples of 4.
		bool UpdateBuffer(
			ICommandList& commandList,
			BufferHandle buffer,
			uint32_t offset,
			const void* data,
			uint32_t size);
		// rowPitch는 행 간 바이트 간격, slicePitch는 rowPitch*mipHeight 이상이다.
		// dataSize는 rowPitch*(mipHeight-1)+마지막 행 픽셀 바이트 수 이상이면 된다.
		// 마지막 행 뒤 패딩은 필요 없다. 네이티브 정렬을 위한 내부 재포장은 픽셀 값을 보존한다.
        // BC1/BC3에서는 행이 4x4 블록 행이며 mipHeight 대신 ceil(mipHeight/4)를 사용한다.
        // 한 행은 ceil(mipWidth/4)*블록 바이트 이상이다. 지원하는 백엔드의 sampled texture에 한한다.
		bool UpdateTexture(
			ICommandList& commandList,
			TextureHandle texture,
			uint32_t mipLevel,
			uint32_t arrayLayer,
			const void* data,
			uint32_t dataSize,
			uint32_t rowPitch,
			uint32_t slicePitch);

	protected:
		// Native hooks receive common-validated descriptors, device-owned resources
		// and closed, distinct replay lists. Common RHI pins their lifetimes through
		// completion. Adapters translate API limits, layouts, synchronization and errors;
		// they do not search a second ownership registry or repeat common validation.
        IDevice();
        void StopLiveProfiling() noexcept;
        virtual UploadMemoryStatistics GetUploadMemoryStatisticsNative() const;
        virtual bool SetUploadMemoryPolicyNative(bool,bool) { return false; }
        enum class DiagnosticSeverity : uint8_t { Info, Warning, Error };
        void ReportDiagnostic(DiagnosticSeverity severity, const char* message) const;
		// Release common references while the native device is still alive.
		void ReleaseResources();
		void AbandonResources();
        // Reserve before retiring native swapchain resources. Only one replacement
        // may await reclamation; false defers another replacement without losing
        // the device. Complete only after native completion has been confirmed
        // and the retired resources have been released.
        [[nodiscard]] bool TryBeginSwapchainRetirement();
        void CompleteSwapchainRetirement();
		virtual bool CreateSwapchainNative(const SwapchainDesc&) = 0;
		virtual void DestroySwapchainNative() = 0;
		virtual bool WaitIdleNative() = 0;
        virtual TimestampQueryHandle CreateTimestampQueryNative(const TimestampQueryDesc&) {return nullptr;}
        virtual void DestroyTimestampQueryNative(TimestampQueryHandle) {}
        virtual bool ReadTimestampsNative(TimestampQueryHandle,uint32_t,uint32_t,uint64_t*) {return false;}
        virtual double GetTimestampPeriodNative() const {return 0;}
        virtual uint32_t GetTimestampValidBitsNative() const {return 0;}
        virtual bool GetTimestampCalibrationNative(TimestampCalibration&) const {return false;}
        virtual bool IsLostNative() const = 0;
        virtual bool SupportsNative(Feature) const = 0;
        virtual bool SupportsFormatNative(Format,FormatFeature) const noexcept { return false; }
    virtual uint64_t GetLimitNative(Limit) const = 0;
    virtual bool SupportsSamplerNative(const SamplerDesc&) const = 0;
        virtual bool SupportsPipelineLayoutNative(const PipelineLayoutDesc&) const = 0;
        virtual bool SupportsGraphicsPipelineNative(const GraphicsPipelineDesc&) const = 0;
        virtual bool SupportsMeshPipelineNative(const MeshPipelineDesc&) const { return false; }
        virtual uint64_t GetCompletedSubmissionNative() = 0;
        virtual uint64_t GetLastSubmissionNative() const = 0;
		virtual bool BeginFrameNative() = 0;
		virtual bool PresentNative() = 0;
		virtual bool SubmitNative(ICommandList** commandLists, uint32_t count) = 0;
		virtual ICommandList* AcquireCommandListNative() = 0;
        virtual ICommandList* AcquireCommandListNative(bool,bool) { return AcquireCommandListNative(); }
        virtual void DiscardCommandListNative(ICommandList*) = 0;
		virtual TextureHandle GetBackBufferNative() = 0;
		virtual BufferHandle CreateBufferNative(const BufferDesc& desc) = 0;
		virtual TextureHandle CreateTextureNative(const TextureDesc& desc) = 0;
		virtual ShaderHandle CreateShaderNative(const ShaderDesc& desc) = 0;
		virtual PipelineHandle CreateGraphicsPipelineNative(const GraphicsPipelineDesc& desc) = 0;
        virtual PipelineHandle CreateComputePipelineNative(const ComputePipelineDesc&) {return nullptr;}
        virtual PipelineHandle CreateMeshPipelineNative(const MeshPipelineDesc&) {return nullptr;}
		virtual ResourceSetHandle CreateResourceSetNative(const ResourceSetDesc& desc) = 0;
		virtual void DestroyBufferNative(BufferHandle buffer) = 0;
		virtual void DestroyTextureNative(TextureHandle texture) = 0;
		virtual void DestroyShaderNative(ShaderHandle shader) = 0;
		virtual void DestroyPipelineNative(PipelineHandle pipeline) = 0;
		virtual void DestroyResourceSetNative(ResourceSetHandle resourceSet) = 0;
		virtual bool UpdateBufferNative(
			ICommandList& commandList,
			BufferHandle buffer,
			uint32_t offset,
			const void* data,
			uint32_t size) = 0;
		virtual bool UpdateTextureNative(
			ICommandList& commandList,
			TextureHandle texture,
			uint32_t mipLevel,
			uint32_t arrayLayer,
			const void* data,
			uint32_t dataSize,
			uint32_t rowPitch,
			uint32_t slicePitch) = 0;
		virtual bool ReadTextureNative(TextureHandle texture, TextureReadback& result) = 0;
		virtual int Initialize(const void* windowHandle, const DeviceDesc& desc) = 0;

	private:
        friend struct ProfilerAccess;
        friend class dyf::Renderer;
        friend class dyf::ModelRenderer;
        friend class Platform::FrameProfiler;
        // Pending tokens and up to 4096 completed tokens (10 seconds); expired/omitted tokens return 0.
        [[nodiscard]] std::uint64_t GetLiveSubmissionId(FenceHandle) const noexcept;
        // Call at the source's first preparation boundary, before uploads/recording.
        // Use its actual frame key; already-recorded lists retain their upload policy.
        void PrepareLiveControls(Platform::LiveFrameKey) noexcept;
        void ReleaseLiveControls(std::uint64_t source) noexcept;

		friend class ICommandList;
        friend class RenderGraph;
        struct LiveState;
        std::unique_ptr<LiveState> m_live;
        LiveState* GetLiveState() noexcept;
        void PublishLiveResources() noexcept;
        void CaptureLiveUploadPolicy(ICommandList&) noexcept;
        void BeginLiveRecording(ICommandList&) noexcept;
        bool CaptureLiveGraphParallel(Platform::LiveFrameKey) noexcept;
        struct LiveExecutionScope;
        void BeginLiveExecution(ICommandList* const*,uint32_t,LiveExecutionScope&) noexcept;
        void EndLiveExecution(LiveExecutionScope&) noexcept;
        bool ReadTimestampsInternal(TimestampQueryHandle,uint32_t,uint32_t,uint64_t*,bool observe);
        void PumpLiveWork() noexcept;
        uint32_t ReserveLiveQuery(ICommandList*) noexcept;
        void DiscardLivePrepared(ICommandList*,Platform::LiveOutcome) noexcept;
        void SubmitLiveWork(ICommandList* const*,uint32_t,bool,uint64_t) noexcept;
        struct RecordingState;
        struct ResourceRecord
        {
            explicit ResourceRecord(std::shared_ptr<void> resource) : owner(std::move(resource)) {}
            // Snapshot holders extend storage lifetime, while this flag only
            // controls whether a recorder may acquire its first reference.
            const std::shared_ptr<void> owner;
            std::atomic<bool> registered{true};
        };
        using RecordingResources=std::unordered_map<const void*,std::shared_ptr<ResourceRecord>>;
        // General resources remain stable across frames. Borrowed backbuffers
        // stay on the generation-checked registry path instead of invalidating
        // this cache at every BeginFrame.
        std::shared_ptr<const RecordingResources> CaptureRecordingResources();
        void CaptureRecordingLookup(ICommandList&);
        struct RecordingResourceScope
        {
            RecordingResourceScope(const IDevice&,const RecordingResources*) noexcept;
            ~RecordingResourceScope();
            RecordingResourceScope(const RecordingResourceScope&)=delete;
            RecordingResourceScope& operator=(const RecordingResourceScope&)=delete;
            const IDevice* previousDevice;
            const RecordingResources* previousResources;
        };
        static thread_local const IDevice* s_recordingDevice;
        static thread_local const RecordingResources* s_recordingResources;
        void RecordParallel(uint32_t count, uint32_t threadCount,
            const std::function<void(uint32_t)>& record);
        // Owner-only device state; independent command lists remain worker-local.
        void RequireOwnerThread() const;
        bool IsOwnerThread() const noexcept;
        void DrainRetiredResources() noexcept;
        ResourceAllocationCounters m_allocationCounters;
		bool m_nativeResourcesAlive = true;
		std::unordered_set<ICommandList*> m_recordedCommands;
        std::unordered_set<ICommandList*> m_userCommands;
        std::map<std::tuple<uintptr_t,uint32_t,uint32_t>,ResourceState> m_resourceStates;
        uint64_t m_imageGeneration = 1;
		std::unordered_set<const void*> m_borrowedTextures;
		std::shared_ptr<void> Reference(const void* handle) const;
		void Track(void* handle, void (*destroy)(IDevice&, void*), std::vector<std::shared_ptr<void>> dependencies = {});
		void Track(void* handle, void (*destroy)(IDevice&, void*), std::vector<std::shared_ptr<void>> dependencies,
            Platform::LiveObjectKind kind,uint64_t elements=0,uint64_t bytes=0,
            const char* label="RHI resource");
        RecordingResources m_resources;
        std::shared_ptr<const RecordingResources> m_recordingResources;
        void UnregisterResource(const void* handle);

		DeviceDesc m_desc = {};
		SwapchainDesc m_swapchainDesc = {};
		bool m_hasSwapchain = false;
        bool m_swapchainRetirementPending = false;
		bool m_frameActive = false;
        std::shared_ptr<RecordingState> m_recording;
		void InvalidateBackBuffers();
	};
// 명시적으로 등록한 자원만 소유하며 장치는 이 객체보다 오래 유지해야 한다.
// 기록된 네이티브 명령은 GPU 작업이 끝날 때까지 참조 자원을 유지한다.
// 자원 생성·설정·업로드·배리어·제출은 호출자가 담당한다.
class ResourceScope
{
public:
    explicit ResourceScope(IDevice& device) : m_device(device) {}
    ResourceScope(const ResourceScope&) = delete;
    ResourceScope& operator=(const ResourceScope&) = delete;

    ~ResourceScope()
    {
        for(auto* query:m_queries) m_device.DestroyTimestampQuery(query);
        for(auto* commands:m_commands) m_device.DestroyCommandList(commands);
        for (auto it = m_sets.rbegin(); it != m_sets.rend(); ++it) m_device.DestroyResourceSet(*it);
        for (auto it = m_pipelines.rbegin(); it != m_pipelines.rend(); ++it) m_device.DestroyPipeline(*it);
        for (auto it = m_shaders.rbegin(); it != m_shaders.rend(); ++it) m_device.DestroyShader(*it);
        for (auto it = m_textures.rbegin(); it != m_textures.rend(); ++it) m_device.DestroyTexture(*it);
        for (auto it = m_buffers.rbegin(); it != m_buffers.rend(); ++it) m_device.DestroyBuffer(*it);
    }

    // 소유권은 한 번만 등록하고 빈 핸들은 생성 실패로 처리한다.
    // 스왑체인이 소유한 텍스처는 등록하지 않는다.
    ICommandList* Keep(ICommandList* value) { return Keep(value,m_commands,&IDevice::DestroyCommandList); }
    TimestampQueryHandle Keep(TimestampQueryHandle value) {return Keep(value,m_queries,&IDevice::DestroyTimestampQuery);}
    BufferHandle Keep(BufferHandle value) { return Keep(value, m_buffers, &IDevice::DestroyBuffer); }
    TextureHandle Keep(TextureHandle value) { return Keep(value, m_textures, &IDevice::DestroyTexture); }
    ShaderHandle Keep(ShaderHandle value) { return Keep(value, m_shaders, &IDevice::DestroyShader); }
    PipelineHandle Keep(PipelineHandle value) { return Keep(value, m_pipelines, &IDevice::DestroyPipeline); }
    ResourceSetHandle Keep(ResourceSetHandle value) { return Keep(value, m_sets, &IDevice::DestroyResourceSet); }

private:
    template<class Handle>
    Handle Keep(Handle value, std::vector<Handle>& handles, void (IDevice::*destroy)(Handle))
    {
        if (!value) throw std::runtime_error("RHI resource creation failed.");
        try { handles.push_back(value); }
        catch (...) { (m_device.*destroy)(value); throw; }
        return value;
    }

    IDevice& m_device;
    std::vector<TimestampQueryHandle> m_queries;
    std::vector<ICommandList*> m_commands;
    std::vector<BufferHandle> m_buffers;
    std::vector<TextureHandle> m_textures;
    std::vector<ShaderHandle> m_shaders;
    std::vector<PipelineHandle> m_pipelines;
    std::vector<ResourceSetHandle> m_sets;
};
}
