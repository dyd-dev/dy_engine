#pragma once
#include <cstdint>

#include "Format.h"

namespace dy::RHI
{
	enum class ResourceState : uint8_t;
	class ICommandList;

	class IBuffer;
	class IShader;
	class IResourceSet;
	class ISampler;
	class ITexture;
	class IPipelineState;

	struct BufferDesc;
	struct ResourceSetWrite;
	struct SamplerDesc;
	struct ShaderDesc;
	struct TextureDesc;
	struct GraphicsPipelineDesc;

	struct DeviceDesc
	{
		// 스왑체인(백버퍼) 포맷. Unknown이면 backend가 native 포맷을 선택한다.
		// 구체 포맷은 정확히 지원되어야 하며, 지원할 수 없으면 Device 생성이 실패한다.
		// 실제 선택값은 GetBackBuffer()->GetFormat()으로 확인한다.
		// UNORM = 셰이더 수동 감마, *_SRGB = 하드웨어 감마.
		Format swapchainFormat = Format::Unknown;
		// 동시에 완료되지 않을 수 있는 제출의 상한이자 순환 재사용할 frame context 수. 0은 유효하지 않다.
		// swapchain image 수와는 독립적이며 GetCurrentFrameIndex()는 이 범위의 context index를 반환한다.
		uint32_t maxFramesInFlight = 2;
		uint64_t frameAcquireTimeoutNanoseconds = 16666667ull;
	};

	class IDevice
	{
	public:
		virtual ~IDevice() = default;
		[[nodiscard]] static IDevice* Create(const void* windowHandle, const DeviceDesc& desc = {});
		[[nodiscard]] const DeviceDesc& GetDesc() const { return m_desc; }

		// frame context와 presentable image를 획득했을 때만 true를 반환한다.
		[[nodiscard]] virtual bool BeginFrame() = 0;

		// [0, GetDesc().maxFramesInFlight) 범위의 frame context index. swapchain image index가 아니다.
		[[nodiscard]] virtual uint32_t GetCurrentFrameIndex() const = 0;
		[[nodiscard]] virtual RHI::ICommandList* AcquireCommandList()	= 0;

		virtual void Submit(ICommandList** cmdLists, uint32_t count) = 0;
		virtual void Present() = 0;

		[[nodiscard]] virtual ITexture* GetBackBuffer() = 0;

		[[nodiscard]] virtual IBuffer* CreateBuffer(const BufferDesc& desc) = 0;
		[[nodiscard]] virtual IShader* CreateShader(const ShaderDesc& desc) = 0;
		[[nodiscard]] virtual ISampler* CreateSampler(const SamplerDesc& desc) = 0;
		[[nodiscard]] virtual ITexture* CreateTexture(const TextureDesc& desc) = 0;
		[[nodiscard]] virtual IPipelineState* CreateGraphicsPipeline(const GraphicsPipelineDesc& desc) = 0;
		// pipeline은 생성한 resource set보다 오래 살아야 한다. set에 기록한 자원과 sampler는 GPU 사용 완료까지 유효해야 한다.
		[[nodiscard]] virtual IResourceSet* CreateResourceSet(IPipelineState* pipeline) = 0;
		virtual bool UpdateResourceSet(IResourceSet* resourceSet, const ResourceSetWrite* writes, uint32_t writeCount) = 0;
		
		virtual void DestroyBuffer(IBuffer* buffer) = 0;
		virtual void DestroyShader(IShader* shader) = 0;
		virtual void DestroySampler(ISampler* sampler) = 0;
		virtual void DestroyTexture(ITexture* texture) = 0;
		virtual void DestroyPipelineState(IPipelineState* pipeline) = 0;
		virtual void DestroyResourceSet(IResourceSet* resourceSet) = 0;

		virtual bool UpdateTexture(ITexture* texture, const void* data, uint32_t rowPitch, ResourceState finalState) = 0;

	protected:
		virtual int Initialize(const void* windowHandle, const DeviceDesc& desc) = 0;

	private:
		void SetDesc(const DeviceDesc& desc) { m_desc = desc; }

		DeviceDesc m_desc = {};
	};
}
