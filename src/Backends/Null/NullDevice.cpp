#include "Backends/Null/NullDevice.h"

#include "RHI/IBuffer.h"
#include "RHI/ICommandList.h"
#include "RHI/IPipelineState.h"
#include "RHI/IShader.h"
#include "RHI/ITexture.h"

#include <memory>

namespace dy::Backends
{
	namespace
	{
		class NullBuffer final : public RHI::IBuffer
		{
		public:
			explicit NullBuffer(const RHI::BufferDesc& desc)
				: RHI::IBuffer(desc)
			{
			}

			void* Map(uint32_t) override { return nullptr; }
			void Unmap() override {}
		};

		class NullTexture final : public RHI::ITexture
		{
		public:
			explicit NullTexture(const RHI::TextureDesc& desc)
				: RHI::ITexture(desc)
			{
			}
		};

		class NullPipelineState final : public RHI::IPipelineState
		{
		public:
			explicit NullPipelineState(const RHI::GraphicsPipelineDesc&) {}
		};

		class NullShader final : public RHI::IShader
		{
		public:
			explicit NullShader(const RHI::ShaderDesc& desc)
				: m_stage(desc.stage)
			{
			}

			[[nodiscard]] RHI::ShaderStage GetStage() const { return m_stage; }

		private:
			RHI::ShaderStage m_stage = RHI::ShaderStage::Unknown;
		};

		class NullCommandList final : public RHI::ICommandList
		{
		public:
			void BindGraphicsPipeline(RHI::IPipelineState*) override {}
			void BindGlobalDescriptors() override {}
			void BindGeometry(const RHI::GeometryBinding&) override {}
			void BindConstantBuffer(uint32_t, RHI::IBuffer*, uint32_t, uint32_t) override {}
			void BindTexture(uint32_t, RHI::ITexture*) override {}
			void SetInlineConstants(uint32_t, const void*) override {}
			void SetRenderTargets(uint32_t, RHI::ITexture**, RHI::ITexture*) override {}
			void SetViewport(const RHI::Viewport&) override {}
			void SetScissor(const RHI::Rect&) override {}
			void ClearColor(RHI::ITexture*, float, float, float, float) override {}
			void ClearDepth(RHI::ITexture*, float) override {}
			void BindVertexBuffer(RHI::IBuffer*, uint32_t, uint32_t) override {}
			void BindIndexBuffer(RHI::IBuffer*, RHI::Format, uint32_t) override {}
			void DrawInstanced(uint32_t, uint32_t, uint32_t, uint32_t) override {}
			void DrawIndexedInstanced(uint32_t, uint32_t, uint32_t, int32_t, uint32_t) override {}
			void Close() override {}
		};
	}

	struct NullDevice::Impl
	{
		NullCommandList commandList;
		std::unique_ptr<NullTexture> backBuffer;
		RHI::DescriptorIndex nextDescriptorIndex = 0;
		uint32_t frameIndex = 0;
	};

	NullDevice::NullDevice()
		: m_impl(new Impl())
	{
	}

	NullDevice::~NullDevice()
	{
		delete m_impl;
	}

	bool NullDevice::BeginFrame() { return true; }

	uint32_t NullDevice::GetCurrentFrameIndex() const
	{
		return m_impl->frameIndex;
	}

	RHI::ICommandList* NullDevice::AcquireCommandList()
	{
		return &m_impl->commandList;
	}

	void NullDevice::Submit(RHI::ICommandList**, uint32_t) {}

	void NullDevice::Present()
	{
		m_impl->frameIndex = (m_impl->frameIndex + 1) % GetDesc().maxFramesInFlight;
	}

	RHI::IBuffer* NullDevice::CreateBuffer(const RHI::BufferDesc& desc)
	{
		return new NullBuffer(desc);
	}

	RHI::ITexture* NullDevice::CreateTexture(const RHI::TextureDesc& desc)
	{
		return new NullTexture(desc);
	}

	RHI::IShader* NullDevice::CreateShader(const RHI::ShaderDesc& desc)
	{
		if(desc.stage == RHI::ShaderStage::Unknown ||
			desc.binary == nullptr || desc.binarySize == 0 ||
			desc.entryPoint == nullptr || desc.entryPoint[0] == '\0') return nullptr;
		return new NullShader(desc);
	}

	bool NullDevice::UpdateTexture(RHI::ITexture*, const void*, uint32_t)
	{
		return true;
	}

	RHI::IPipelineState* NullDevice::CreateGraphicsPipeline(const RHI::GraphicsPipelineDesc& desc)
	{
		const bool hasColorAttachment = desc.renderTargetFormat != RHI::Format::Unknown;
		const bool hasDepthAttachment = desc.depthStencilFormat != RHI::Format::Unknown;
		const bool hasFragmentShader = desc.fragmentShader != nullptr;
		if((!hasColorAttachment && !hasDepthAttachment) || (desc.depthEnable && !hasDepthAttachment))
		{
			return nullptr;
		}
		const auto* vertexShader = dynamic_cast<const NullShader*>(desc.vertexShader);
		const auto* fragmentShader = dynamic_cast<const NullShader*>(desc.fragmentShader);
		if(vertexShader == nullptr || vertexShader->GetStage() != RHI::ShaderStage::Vertex) return nullptr;
		if(hasFragmentShader &&
			(fragmentShader == nullptr || fragmentShader->GetStage() != RHI::ShaderStage::Fragment)) return nullptr;

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

	void NullDevice::DestroyShader(RHI::IShader* shader)
	{
		delete shader;
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
		return m_impl->backBuffer.get();
	}

	int NullDevice::Initialize(const void*, const RHI::DeviceDesc& desc)
	{
		const RHI::Format actualFormat = desc.swapchainFormat == RHI::Format::Unknown
			? RHI::Format::R8G8B8A8_UNORM
			: desc.swapchainFormat;
		switch(actualFormat)
		{
		case RHI::Format::R8G8B8A8_UNORM:
		case RHI::Format::B8G8R8A8_UNORM:
		case RHI::Format::R8G8B8A8_UNORM_SRGB:
		case RHI::Format::B8G8R8A8_UNORM_SRGB:
		case RHI::Format::R16G16B16A16_FLOAT:
		case RHI::Format::R32G32B32A32_FLOAT:
			break;
		case RHI::Format::Unknown:
		case RHI::Format::D32_FLOAT:
		case RHI::Format::D24_UNORM_S8_UINT:
		case RHI::Format::R32_UINT:
		case RHI::Format::R16_UINT:
		default:
			return -1;
		}

		m_impl->backBuffer = std::make_unique<NullTexture>(RHI::TextureDesc{
			1,
			1,
			1,
			1,
			actualFormat,
			RHI::TextureUsage::RenderTarget
		});
		return 0;
	}
}
