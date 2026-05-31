#include "Backends/Null/NullDevice.h"

#include "RHI/IBuffer.h"
#include "RHI/ICommandList.h"
#include "RHI/IPipelineState.h"
#include "RHI/ITexture.h"

namespace dy::Backends
{
	namespace
	{
		class NullBuffer final : public RHI::IBuffer
		{
		public:
			explicit NullBuffer(const RHI::BufferDesc& desc)
				: m_size(desc.size), m_stride(desc.stride)
			{
			}

			void* Map(uint32_t, uint32_t) override { return nullptr; }
			void Unmap() override {}
			uint32_t GetSize() const override { return m_size; }
			uint32_t GetStride() const override { return m_stride; }

		private:
			uint32_t m_size = 0;
			uint32_t m_stride = 0;
		};

		class NullTexture final : public RHI::ITexture
		{
		public:
			explicit NullTexture(const RHI::TextureDesc& desc)
				: m_width(desc.width), m_height(desc.height), m_format(desc.format)
			{
			}

			uint32_t GetWidth() const override { return m_width; }
			uint32_t GetHeight() const override { return m_height; }
			RHI::Format GetFormat() const override { return m_format; }

		private:
			uint32_t m_width = 0;
			uint32_t m_height = 0;
			RHI::Format m_format = RHI::Format::Unknown;
		};

		class NullPipelineState final : public RHI::IPipelineState
		{
		public:
			explicit NullPipelineState(const RHI::GraphicsPipelineDesc&) {}
		};

		class NullCommandList final : public RHI::ICommandList
		{
		public:
			void BindGraphicsPipeline(RHI::IPipelineState*) override {}
			void BindGlobalDescriptorHeap() override {}
			void SetPushConstants(uint32_t, const void*) override {}
			void SetRenderTargets(uint32_t, RHI::ITexture**, RHI::ITexture*) override {}
			void SetViewport(const RHI::Viewport&) override {}
			void SetScissor(const RHI::Rect&) override {}
			void ClearColor(RHI::ITexture*, float, float, float, float) override {}
			void ClearDepth(RHI::ITexture*, float) override {}
			void DrawInstanced(uint32_t, uint32_t, uint32_t, uint32_t) override {}
			void ResourceBarrier(RHI::IBuffer*, RHI::ResourceState, RHI::ResourceState) override {}
			void ResourceBarrier(RHI::ITexture*, RHI::ResourceState, RHI::ResourceState) override {}
			void Close() override {}
		};
	}

	struct NullDevice::Impl
	{
		NullCommandList commandList;
		NullTexture backBuffer = NullTexture({
			1,
			1,
			1,
			1,
			RHI::Format::R8G8B8A8_UNORM,
			RHI::TextureUsage::RenderTarget
		});
		RHI::DescriptorIndex nextDescriptorIndex = 0;
	};

	NullDevice::NullDevice()
		: m_impl(new Impl())
	{
	}

	NullDevice::~NullDevice()
	{
		delete m_impl;
	}

	void NullDevice::BeginFrame() {}

	uint32_t NullDevice::GetCurrentFrameIndex() const
	{
		return 0;
	}

	RHI::ICommandList* NullDevice::AcquireCommandList()
	{
		return &m_impl->commandList;
	}

	void NullDevice::Submit(RHI::ICommandList**, uint32_t) {}

	void NullDevice::Present() {}

	RHI::IBuffer* NullDevice::CreateBuffer(const RHI::BufferDesc& desc)
	{
		return new NullBuffer(desc);
	}

	RHI::ITexture* NullDevice::CreateTexture(const RHI::TextureDesc& desc)
	{
		return new NullTexture(desc);
	}

	void NullDevice::UpdateTexture(RHI::ITexture*, const void*, uint32_t)
	{
	}

	RHI::IPipelineState* NullDevice::CreateGraphicsPipeline(const RHI::GraphicsPipelineDesc& desc)
	{
		return new NullPipelineState(desc);
	}

	RHI::DescriptorIndex NullDevice::AllocateDescriptorSlot()
	{
		return m_impl->nextDescriptorIndex++;
	}

	void NullDevice::UpdateDescriptorSlot(RHI::DescriptorIndex, RHI::ITexture*) {}
	void NullDevice::UpdateDescriptorSlot(RHI::DescriptorIndex, RHI::IBuffer*) {}

	void NullDevice::DestroyBuffer(RHI::IBuffer* buffer)
	{
		delete buffer;
	}

	void NullDevice::DestroyTexture(RHI::ITexture* texture)
	{
		delete texture;
	}

	void NullDevice::DestroyPipelineState(RHI::IPipelineState* pipeline)
	{
		delete pipeline;
	}

	RHI::ITexture* NullDevice::GetBackBuffer()
	{
		return &m_impl->backBuffer;
	}

	int NullDevice::Initialize(const void*)
	{
		return 0;
	}
}
