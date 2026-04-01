#pragma once
#include "RHI/IDevice.h"

using dy::RHI::ICommandList;
using dy::RHI::IBuffer;
using dy::RHI::ITexture;
using dy::RHI::IPipelineState;
using dy::RHI::BufferDesc;
using dy::RHI::TextureDesc;
using dy::RHI::GraphicsPipelineDesc;

namespace dy::Backends
{
	class NullDevice : public RHI::IDevice
	{
	public:
		void BeginFrame() override {}
		
		uint32_t GetCurrentFrameIndex() const override { return 0; }
		ICommandList* AcquireCommandList() override { return nullptr; }

		void Submit(ICommandList** cmdLists, uint32_t count) override {}
		void Present() override {}

		IBuffer* CreateBuffer(const BufferDesc& desc) override { return nullptr; }
		ITexture* CreateTexture(const TextureDesc& desc) override { return nullptr; }
		IPipelineState* CreateGraphicsPipeline(const GraphicsPipelineDesc& desc) override { return nullptr; }
		// virtual IPipelineState* CreateComputePipelilne(const ComputePipelineDesc& desc) = 0;

		void DestroyBuffer(IBuffer* buffer) override {}
		void DestroyTexture(ITexture* texture) override {}
		void DestroyPipelineState(IPipelineState* pipeline) override {}

		// Warning: In a multi-threaded setup, worker threads should not call this directly
        // unless they are explicitly assigned the task of writing to the final swapchain image.
		ITexture* GetBackBuffer() override { return nullptr; }
		
	protected:
		int Initialize(const void *windowHandle) override { return 0; }
	};
}