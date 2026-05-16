#pragma once
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

	struct DeviceDesc
	{
	};

	class IDevice
	{
	public:
		virtual ~IDevice() = default;
		static IDevice* Create(const void* windowHandle, const DeviceDesc& desc = {});
		virtual void BeginFrame() = 0;
		virtual uint32_t GetCurrentFrameIndex() const = 0;
		virtual ICommandList* AcquireCommandList() = 0;
		virtual void Submit(ICommandList** cmdLists, uint32_t count) = 0;
		virtual void Present() = 0;
		virtual IBuffer* CreateBuffer(const BufferDesc& desc) = 0;
		virtual ITexture* CreateTexture(const TextureDesc& desc) = 0;
		virtual bool UpdateTexture(ITexture* texture, const void* rgba8Pixels, uint32_t rowPitch) = 0;
		virtual IPipelineState* CreateGraphicsPipeline(const GraphicsPipelineDesc& desc) = 0;
		[[nodiscard]] virtual DescriptorIndex AllocateDescriptorSlot() = 0;
		virtual void UpdateDescriptorSlot(DescriptorIndex index, ITexture* texture) = 0;
		virtual void DestroyBuffer(IBuffer* buffer) = 0;
		virtual void DestroyTexture(ITexture* texture) = 0;
		virtual void DestroyPipelineState(IPipelineState* pipeline) = 0;
		virtual ITexture* GetBackBuffer() = 0;
		
	protected:
		virtual int Initialize(const void* windowHandle, const DeviceDesc& desc) = 0;
	};
}
