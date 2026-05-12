#pragma once
/* Device.h
*
* GPU와 통신하는 최상위 클래스입니다.
* 물리/논리 디바이스 초기화, 메모리 할당자 역할을 합니다.
*
* SwapChain은 OS와 1:1 결합되는 객체이므로 Backend Device 내부로 가져가 불필요한 BoilerPlate Code를 숨깁니다.
*/
#include <cstdint>

namespace dy::RHI
{
	class ICommandList;

	class IBuffer;
	class ITexture;
	class IPipelineState;

	struct BufferDesc;
	struct TextureDesc;
	struct GraphicsPipelineDesc;

	using DescriptorIndex = uint32_t;
	constexpr DescriptorIndex INVALID_DESCRIPTOR_INDEX = 0xFFFFFFFF;

	class IDevice
	{
	public:
		virtual ~IDevice() = default;
		static IDevice* Create(const void *windowHandle);

		// Global synchronization: Acquires the next swapchain image
		// Must be called once per frame by the "main render thread"
		virtual void BeginFrame() = 0;

		// Returns the current frame index in the ring buffer (e.g., 0, 1, or 2)
		// Upper layers MUST use this to index into their own multiple-buffered resources.
		virtual uint32_t GetCurrentFrameIndex() const = 0;

		// Rename from CreateCommandList to AcquireCommandList to enforce pre-allocation semantics
		// The backend uses GetCurrentFrameIndex() and thread_local internally to return the correct list.
		virtual ICommandList* AcquireCommandList() = 0;
		
		// Submits multiple command lists recorded by various threads simultaneously
		virtual void Submit(ICommandList** cmdLists, uint32_t count) = 0;
		virtual void Present() = 0;

		virtual IBuffer* CreateBuffer(const BufferDesc& desc) = 0;
		virtual ITexture* CreateTexture(const TextureDesc& desc) = 0;
		virtual void UpdateTexture(ITexture* texture, const void* data, uint32_t rowPitch) = 0;
		virtual IPipelineState* CreateGraphicsPipeline(const GraphicsPipelineDesc& desc) = 0;
		// virtual IPipelineState* CreateComputePipelilne(const ComputePipelineDesc& desc) = 0;

		// Allocates a slot in the GPU's Global Descriptor Heap.
		[[nodiscard]] virtual DescriptorIndex AllocateDescriptorSlot() = 0;

		// Binds an ITexture (SRV) to the allocated slot in the Global Heap.
		virtual void UpdateDescriptorSlot(DescriptorIndex index, ITexture* texture) = 0;
		virtual void UpdateDescriptorSlot(DescriptorIndex index, IBuffer* buffer) = 0;

		virtual void DestroyBuffer(IBuffer* buffer) = 0;
		virtual void DestroyTexture(ITexture* texture) = 0;
		virtual void DestroyPipelineState(IPipelineState* pipeline) = 0;

		// Warning: In a multi-threaded setup, worker threads should not call this directly
        // unless they are explicitly assigned the task of writing to the final swapchain image.
		virtual ITexture* GetBackBuffer() = 0;
		
	protected:
		virtual int Initialize(const void *windowHandle) = 0;
	};
}