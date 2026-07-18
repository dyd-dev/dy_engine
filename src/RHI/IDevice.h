#pragma once
#include <cstdint>

#include "Format.h"
#include "ShaderLayout.h"

namespace dy::RHI
{
	class ICommandList;

	class IBuffer;
	class ITexture;
	class IPipelineState;

	struct BufferDesc;
	struct TextureDesc;
	struct GraphicsPipelineDesc;

	struct DeviceDesc
	{
		// 스왑체인(백버퍼) 포맷. 백엔드는 이 값을 그대로 따르고 GetBackBuffer()->GetFormat() 로 보고한다.
		// UNORM = 셰이더 수동 감마, *_SRGB = 하드웨어 감마. 두 백엔드가 같은 값을 쓰므로 색이 일치한다.
		Format swapchainFormat = Format::R8G8B8A8_UNORM;
		uint32_t maxFramesInFlight = 2;
		uint32_t maxDrawsPerFrame = 128;
		uint32_t maxBindlessTextures = 128;
		uint64_t frameAcquireTimeoutNanoseconds = 16666667ull;
		ShaderLayoutDesc shaderLayout = {};
	};

	class IDevice
	{
	public:
		virtual ~IDevice() = default;
		[[nodiscard]] static IDevice* Create(const void* windowHandle, const DeviceDesc& desc = {});
		[[nodiscard]] const DeviceDesc& GetDesc() const { return m_desc; }

		virtual void BeginFrame() = 0;

		[[nodiscard]] virtual uint32_t GetCurrentFrameIndex() const = 0;
		[[nodiscard]] virtual RHI::ICommandList* AcquireCommandList()	= 0;

		virtual void Submit(ICommandList** cmdLists, uint32_t count) = 0;
		virtual void Present() = 0;

		[[nodiscard]] virtual ITexture* GetBackBuffer() = 0;

		[[nodiscard]] virtual IBuffer* CreateBuffer(const BufferDesc& desc) = 0;
		[[nodiscard]] virtual ITexture* CreateTexture(const TextureDesc& desc) = 0;
		[[nodiscard]] virtual IPipelineState* CreateGraphicsPipeline(const GraphicsPipelineDesc& desc) = 0;
		
		virtual void DestroyBuffer(IBuffer* buffer) = 0;
		virtual void DestroyTexture(ITexture* texture) = 0;
		virtual void DestroyPipelineState(IPipelineState* pipeline) = 0;

		virtual bool UpdateTexture(ITexture* texture, const void* data, uint32_t rowPitch) = 0;

		// (D3D12/Metal: false. Vulkan: true)
		[[nodiscard]] virtual bool RequiresClipSpaceYFlip() const { return false; }

		[[nodiscard]] virtual DescriptorIndex AllocateDescriptorSlot() { return INVALID_DESCRIPTOR_INDEX; }
		virtual void UpdateDescriptorSlot(DescriptorIndex index, ITexture* texture) { (void)index; (void)texture; }
		virtual void UpdateDescriptorSlot(DescriptorIndex index, IBuffer* buffer) { (void)index; (void)buffer; }

	protected:
		virtual int Initialize(const void* windowHandle, const DeviceDesc& desc) = 0;

	private:
		void SetDesc(const DeviceDesc& desc) { m_desc = desc; }

		DeviceDesc m_desc = {};
	};
}
