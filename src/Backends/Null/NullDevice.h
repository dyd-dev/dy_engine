#pragma once

#include "RHI/IDevice.h"

namespace dy::Backends
{
	class NullDevice final : public RHI::IDevice
	{
	public:
		NullDevice();
		~NullDevice() override;

		bool BeginFrame() override;
		uint32_t GetCurrentFrameIndex() const override;
		RHI::ICommandList* AcquireCommandList() override;

		void Submit(RHI::ICommandList** cmdLists, uint32_t count) override;
		void Present() override;

		RHI::IBuffer* CreateBuffer(const RHI::BufferDesc& desc) override;
		RHI::ITexture* CreateTexture(const RHI::TextureDesc& desc) override;
		bool UpdateTexture(RHI::ITexture* texture, const void* data, uint32_t rowPitch) override;
		RHI::IPipelineState* CreateGraphicsPipeline(const RHI::GraphicsPipelineDesc& desc) override;

		[[nodiscard]] RHI::DescriptorIndex AllocateDescriptorSlot() override;
		void UpdateDescriptorSlot(RHI::DescriptorIndex index, RHI::ITexture* texture) override;
		void UpdateDescriptorSlot(RHI::DescriptorIndex index, RHI::IBuffer* buffer) override;

		void DestroyBuffer(RHI::IBuffer* buffer) override;
		void DestroyTexture(RHI::ITexture* texture) override;
		void DestroyPipelineState(RHI::IPipelineState* pipeline) override;
		RHI::ITexture* GetBackBuffer() override;

	protected:
		int Initialize(const void* windowHandle, const RHI::DeviceDesc& desc) override;

	private:
		struct Impl;
		Impl* m_impl = nullptr;
	};
}
