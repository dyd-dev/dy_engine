#pragma once
#include "RHI/IDevice.h"

namespace dy::Backends
{
	class MetalDevice final : public RHI::IDevice
	{
	public:
		void BeginFrame() override {}
		uint32_t GetCurrentFrameIndex() const override { return 0; }
		RHI::ICommandList* AcquireCommandList() override { return nullptr; }
		void Submit(RHI::ICommandList**, uint32_t) override {}
		void Present() override {}
		RHI::IBuffer* CreateBuffer(const RHI::BufferDesc&) override { return nullptr; }
		RHI::ITexture* CreateTexture(const RHI::TextureDesc&) override { return nullptr; }
		bool UpdateTexture(RHI::ITexture*, const void*, uint32_t) override { return false; }
		RHI::IPipelineState* CreateGraphicsPipeline(const RHI::GraphicsPipelineDesc&) override { return nullptr; }
		RHI::DescriptorIndex AllocateDescriptorSlot() override { return RHI::INVALID_DESCRIPTOR_INDEX; }
		void UpdateDescriptorSlot(RHI::DescriptorIndex, RHI::ITexture*) override {}
		void DestroyBuffer(RHI::IBuffer*) override {}
		void DestroyTexture(RHI::ITexture*) override {}
		void DestroyPipelineState(RHI::IPipelineState*) override {}
		RHI::ITexture* GetBackBuffer() override { return nullptr; }

	protected:
		int Initialize(const void*, const RHI::DeviceDesc&) override { return -1; }
	};
}

namespace dy::RHI
{
	using MetalDevice = dy::Backends::MetalDevice;
}
