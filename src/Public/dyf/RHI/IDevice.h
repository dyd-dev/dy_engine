#pragma once

#include <cstdint>
#include <map>
#include <tuple>
#include "ResourceState.h"
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "ResourceHandles.h"
#include "Swapchain.h"
#include "Submission.h"
#include "Feature.h"
#include "Query.h"

namespace dyf::RHI
{
	class ICommandList;
	struct BufferDesc;
	struct GraphicsPipelineDesc;
    struct PipelineLayoutDesc;
    struct SamplerDesc;
    struct ComputePipelineDesc;
	struct ResourceSetDesc;
	struct ShaderDesc;
	struct TextureDesc;
	struct TextureReadback;

	struct DeviceDesc
	{
		uint32_t maxFramesInFlight = 2;
        uint32_t adapterIndex = 0;
        bool enableValidation = false;
	};

	class IDevice
	{
	public:
		// 생성·기록 시 네이티브 제약에 의한 거절/적용값 변경은 stderr로 알린다.
		// Supports/GetLimit 조회는 로그를 출력하지 않는다. 동등한 내부 표현은 공개 필드의 설명을 따른다.
		virtual ~IDevice();

		[[nodiscard]] static IDevice* Create(const DeviceDesc& desc);
		[[nodiscard]] bool Supports(Feature) const;
        // 설정의 공통 규칙과 장치 한도를 검사한다. 셰이더는 이 장치에서 생성한 핸들을 사용한다.
        // 조회는 자원을 생성하지 않으며, 이후 생성 시 메모리·셰이더 오류까지 보장하지는 않는다.
        [[nodiscard]] bool Supports(const PipelineLayoutDesc&) const;
        [[nodiscard]] bool Supports(const GraphicsPipelineDesc&) const;
        [[nodiscard]] bool Supports(const SamplerDesc&) const;
        [[nodiscard]] uint64_t GetLimit(Limit) const;
        [[nodiscard]] bool IsLost() const;
        [[nodiscard]] const DeviceDesc& GetDesc() const { return m_desc; }
		[[nodiscard]] bool CreateSwapchain(const SwapchainDesc& desc);
		[[nodiscard]] bool WaitIdle();
        [[nodiscard]] TimestampQueryHandle CreateTimestampQuery(const TimestampQueryDesc&);
        void DestroyTimestampQuery(TimestampQueryHandle);
        // Returns false until all requested writes are available. Values are raw GPU ticks.
        [[nodiscard]] bool ReadTimestamps(TimestampQueryHandle,uint32_t first,uint32_t count,uint64_t* ticks);
        [[nodiscard]] double GetTimestampPeriodNanoseconds() const {return GetTimestampPeriodNative();}
        [[nodiscard]] uint32_t GetTimestampValidBits() const {return GetTimestampValidBitsNative();}

        // 성공하면 Present까지 같은 backbuffer를 사용한다. 중간에 여러 번 Submit할 수 있다.
		[[nodiscard]] bool BeginFrame();
		[[nodiscard]] ICommandList* AcquireCommandList();
        [[nodiscard]] bool ResetCommandList(ICommandList*);
        void DestroyCommandList(ICommandList*);

		// 호출 순서대로 실행하며 프레임을 끝내지 않는다. 제출한 명령과 자원은 완료까지 보유한다.
        // GPU 작업 중인 목록은 Reset하거나 다시 Submit할 수 없다.
		[[nodiscard]] bool Submit(ICommandList** commandLists, uint32_t count);
        [[nodiscard]] bool Submit(const SubmitDesc&, FenceHandle& completion);
        [[nodiscard]] bool IsComplete(FenceHandle);
        [[nodiscard]] bool Wait(FenceHandle,uint64_t timeoutNanoseconds);
        // 제출 후 backbuffer가 Present 상태일 때 표시를 요청하고 프레임을 끝낸다.
        // resize로 표시를 생략하고 swapchain 재생성을 예약한 경우도 정상 프레임 종료로 true를 반환한다.
        // 상태 오류는 프레임을 유지한다. 네이티브 표시 요청의 실제 실패는 프레임을 마감하고 false와 stderr로 알린다.
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
		[[nodiscard]] ResourceSetHandle CreateResourceSet(const ResourceSetDesc& desc);

		void DestroyBuffer(BufferHandle buffer);
		void DestroyTexture(TextureHandle texture);
		void DestroyShader(ShaderHandle shader);
		void DestroyPipeline(PipelineHandle pipeline);
		void DestroyResourceSet(ResourceSetHandle resourceSet);

		// 업로드 입력은 호출 중 복사하므로 반환 후 원본 메모리를 해제하거나 바꿀 수 있다.
		// GPU 복사는 명령 목록에 기록되며 Submit으로 실행한다. UpdateTexture도 같은 수명 규칙을 따른다.
		bool UpdateBuffer(
			ICommandList& commandList,
			BufferHandle buffer,
			uint32_t offset,
			const void* data,
			uint32_t size);
		// rowPitch는 행 간 바이트 간격, slicePitch는 rowPitch*mipHeight 이상이다.
		// dataSize는 rowPitch*(mipHeight-1)+마지막 행 픽셀 바이트 수 이상이면 된다.
		// 마지막 행 뒤 패딩은 필요 없다. 네이티브 정렬을 위한 내부 재포장은 픽셀 값을 보존한다.
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
        enum class DiagnosticSeverity : uint8_t { Info, Warning, Error };
        void ReportDiagnostic(DiagnosticSeverity severity, const char* message) const;
		// Release common references while the native device is still alive.
		void ReleaseResources();
		void AbandonResources();
		virtual bool CreateSwapchainNative(const SwapchainDesc&) = 0;
		virtual void DestroySwapchainNative() = 0;
		virtual bool WaitIdleNative() = 0;
        virtual TimestampQueryHandle CreateTimestampQueryNative(const TimestampQueryDesc&) {return nullptr;}
        virtual void DestroyTimestampQueryNative(TimestampQueryHandle) {}
        virtual bool ReadTimestampsNative(TimestampQueryHandle,uint32_t,uint32_t,uint64_t*) {return false;}
        virtual double GetTimestampPeriodNative() const {return 0;}
        virtual uint32_t GetTimestampValidBitsNative() const {return 0;}
        virtual bool IsLostNative() const = 0;
        virtual bool SupportsNative(Feature) const = 0;
    virtual uint64_t GetLimitNative(Limit) const = 0;
    virtual bool SupportsSamplerNative(const SamplerDesc&) const = 0;
        virtual bool SupportsPipelineLayoutNative(const PipelineLayoutDesc&) const = 0;
        virtual bool SupportsGraphicsPipelineNative(const GraphicsPipelineDesc&) const = 0;
        virtual uint64_t GetCompletedSubmissionNative() = 0;
        virtual uint64_t GetLastSubmissionNative() const = 0;
		virtual bool BeginFrameNative() = 0;
		virtual bool PresentNative() = 0;
		virtual bool SubmitNative(ICommandList** commandLists, uint32_t count) = 0;
		virtual ICommandList* AcquireCommandListNative() = 0;
        virtual void DiscardCommandListNative(ICommandList*) = 0;
		virtual TextureHandle GetBackBufferNative() = 0;
		virtual BufferHandle CreateBufferNative(const BufferDesc& desc) = 0;
		virtual TextureHandle CreateTextureNative(const TextureDesc& desc) = 0;
		virtual ShaderHandle CreateShaderNative(const ShaderDesc& desc) = 0;
		virtual PipelineHandle CreateGraphicsPipelineNative(const GraphicsPipelineDesc& desc) = 0;
        virtual PipelineHandle CreateComputePipelineNative(const ComputePipelineDesc&) {return nullptr;}
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
		friend class ICommandList;
		mutable std::recursive_mutex m_resourceMutex;
		bool m_nativeResourcesAlive = true;
		std::unordered_set<ICommandList*> m_recordedCommands;
        std::unordered_set<ICommandList*> m_userCommands;
        std::map<std::tuple<uintptr_t,uint32_t,uint32_t>,ResourceState> m_resourceStates;
        uint64_t m_imageGeneration = 1;
		std::unordered_set<const void*> m_borrowedTextures;
		std::shared_ptr<void> Reference(const void* handle) const;
		void Track(void* handle, void (*destroy)(IDevice&, void*), std::vector<std::shared_ptr<void>> dependencies = {});
		std::unordered_map<const void*, std::shared_ptr<void>> m_resources;

		DeviceDesc m_desc = {};
		SwapchainDesc m_swapchainDesc = {};
		bool m_hasSwapchain = false;
		bool m_frameActive = false;
		void InvalidateBackBuffers();
	};
}
