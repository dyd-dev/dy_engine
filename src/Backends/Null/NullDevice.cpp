#include "Backends/Null/NullDevice.h"

#include "RHI/IBuffer.h"
#include "RHI/ICommandList.h"
#include "RHI/GraphicsPipeline.h"
#include "RHI/IResourceSet.h"
#include "RHI/IShader.h"
#include "RHI/ISampler.h"
#include "RHI/ITexture.h"

#include <memory>
#include <vector>

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
			explicit NullPipelineState(const RHI::GraphicsPipelineDesc& desc)
			{
				if(desc.pipelineLayout.resourceBindingCount > 0)
					bindings.assign(desc.pipelineLayout.resourceBindings, desc.pipelineLayout.resourceBindings + desc.pipelineLayout.resourceBindingCount);
			}

			std::vector<RHI::ResourceBindingDesc> bindings;
		};

		class NullSampler final : public RHI::ISampler
		{
		public:
			explicit NullSampler(const RHI::SamplerDesc& desc) : RHI::ISampler(desc) {}
		};

		class NullResourceSet final : public RHI::IResourceSet
		{
		public:
			explicit NullResourceSet(NullPipelineState* pipelineState) : pipeline(pipelineState) {}
			NullPipelineState* pipeline = nullptr;
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
			void BindResourceSet(RHI::IResourceSet*) override {}
			void SetInlineConstants(uint32_t, uint32_t, const void*) override {}
			void BeginRendering(const RHI::RenderingInfo&) override {}
			void EndRendering() override {}
			void SetViewport(const RHI::Viewport&) override {}
			void SetScissor(const RHI::Rect&) override {}
			void BindVertexBuffer(uint32_t, RHI::IBuffer*, uint32_t) override {}
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

	RHI::ISampler* NullDevice::CreateSampler(const RHI::SamplerDesc& desc)
	{
		if(static_cast<uint32_t>(desc.minFilter) > static_cast<uint32_t>(RHI::SamplerFilter::Linear) ||
			static_cast<uint32_t>(desc.magFilter) > static_cast<uint32_t>(RHI::SamplerFilter::Linear) ||
			static_cast<uint32_t>(desc.mipFilter) > static_cast<uint32_t>(RHI::SamplerFilter::Linear) ||
			static_cast<uint32_t>(desc.addressU) > static_cast<uint32_t>(RHI::SamplerAddressMode::ClampToEdge) ||
			static_cast<uint32_t>(desc.addressV) > static_cast<uint32_t>(RHI::SamplerAddressMode::ClampToEdge) ||
			static_cast<uint32_t>(desc.addressW) > static_cast<uint32_t>(RHI::SamplerAddressMode::ClampToEdge) ||
			desc.minLod > desc.maxLod) return nullptr;
		return new NullSampler(desc);
	}

	bool NullDevice::UpdateTexture(RHI::ITexture*, const void*, uint32_t)
	{
		return true;
	}

	RHI::IPipelineState* NullDevice::CreateGraphicsPipeline(const RHI::GraphicsPipelineDesc& desc)
	{
		const bool hasColorAttachment = desc.colorAttachmentCount > 0;
		const bool hasDepthAttachment = desc.depthStencilFormat != RHI::Format::Unknown;
		const bool hasFragmentShader = desc.fragmentShader != nullptr;
		if((!hasColorAttachment && !hasDepthAttachment) ||
			((desc.depthStencil.depthTestEnable || desc.depthStencil.depthWriteEnable || desc.depthStencil.stencilTestEnable) && !hasDepthAttachment) ||
			(desc.depthStencil.depthWriteEnable && !desc.depthStencil.depthTestEnable) ||
			(desc.depthStencil.stencilTestEnable && desc.depthStencilFormat != RHI::Format::D24_UNORM_S8_UINT))
		{
			return nullptr;
		}
		if(desc.colorAttachmentCount > 0 && desc.colorAttachments == nullptr) return nullptr;
		for(uint32_t attachmentIndex = 0; attachmentIndex < desc.colorAttachmentCount; ++attachmentIndex)
		{
			const RHI::ColorAttachmentDesc& attachment = desc.colorAttachments[attachmentIndex];
			switch(attachment.format)
			{
			case RHI::Format::R8G8B8A8_UNORM:
			case RHI::Format::B8G8R8A8_UNORM:
			case RHI::Format::R8G8B8A8_UNORM_SRGB:
			case RHI::Format::B8G8R8A8_UNORM_SRGB:
			case RHI::Format::R16G16B16A16_FLOAT:
			case RHI::Format::R32G32B32A32_FLOAT:
				break;
			default:
				return nullptr;
			}
			if(attachment.format == RHI::Format::Unknown ||
				(attachment.writeMask & ~RHI::ColorWriteAll) != 0 ||
				static_cast<uint32_t>(attachment.sourceColorFactor) > static_cast<uint32_t>(RHI::BlendFactor::OneMinusDestinationAlpha) ||
				static_cast<uint32_t>(attachment.destinationColorFactor) > static_cast<uint32_t>(RHI::BlendFactor::OneMinusDestinationAlpha) ||
				static_cast<uint32_t>(attachment.sourceAlphaFactor) > static_cast<uint32_t>(RHI::BlendFactor::OneMinusDestinationAlpha) ||
				static_cast<uint32_t>(attachment.destinationAlphaFactor) > static_cast<uint32_t>(RHI::BlendFactor::OneMinusDestinationAlpha) ||
				static_cast<uint32_t>(attachment.colorOp) > static_cast<uint32_t>(RHI::BlendOp::Max) ||
				static_cast<uint32_t>(attachment.alphaOp) > static_cast<uint32_t>(RHI::BlendOp::Max)) return nullptr;
		}
		if((desc.rasterization.fillMode != RHI::FillMode::Solid && desc.rasterization.fillMode != RHI::FillMode::Wireframe) ||
			(desc.rasterization.cullMode != RHI::CullMode::None && desc.rasterization.cullMode != RHI::CullMode::Front && desc.rasterization.cullMode != RHI::CullMode::Back) ||
			(desc.rasterization.frontFace != RHI::FrontFace::CounterClockwise && desc.rasterization.frontFace != RHI::FrontFace::Clockwise) ||
			static_cast<uint32_t>(desc.depthStencil.depthCompareOp) > static_cast<uint32_t>(RHI::CompareOp::Always)) return nullptr;
		const RHI::StencilFaceDesc* stencilFaces[] = { &desc.depthStencil.frontFace, &desc.depthStencil.backFace };
		for(const RHI::StencilFaceDesc* face : stencilFaces)
		{
			if(static_cast<uint32_t>(face->compareOp) > static_cast<uint32_t>(RHI::CompareOp::Always) ||
				static_cast<uint32_t>(face->failOp) > static_cast<uint32_t>(RHI::StencilOp::DecrementWrap) ||
				static_cast<uint32_t>(face->depthFailOp) > static_cast<uint32_t>(RHI::StencilOp::DecrementWrap) ||
				static_cast<uint32_t>(face->passOp) > static_cast<uint32_t>(RHI::StencilOp::DecrementWrap)) return nullptr;
		}
		const auto* vertexShader = dynamic_cast<const NullShader*>(desc.vertexShader);
		const auto* fragmentShader = dynamic_cast<const NullShader*>(desc.fragmentShader);
		if(vertexShader == nullptr || vertexShader->GetStage() != RHI::ShaderStage::Vertex) return nullptr;
		if(hasFragmentShader &&
			(fragmentShader == nullptr || fragmentShader->GetStage() != RHI::ShaderStage::Fragment)) return nullptr;
		if((desc.inputAssembly.vertexBindingCount > 0 && desc.inputAssembly.vertexBindings == nullptr) ||
			(desc.inputAssembly.vertexAttributeCount > 0 && desc.inputAssembly.vertexAttributes == nullptr)) return nullptr;
		for(uint32_t bindingIndex = 0; bindingIndex < desc.inputAssembly.vertexBindingCount; ++bindingIndex)
		{
			const RHI::VertexBindingDesc& binding = desc.inputAssembly.vertexBindings[bindingIndex];
			if(binding.stride == 0) return nullptr;
			for(uint32_t previous = 0; previous < bindingIndex; ++previous)
			{
				if(desc.inputAssembly.vertexBindings[previous].slot == binding.slot) return nullptr;
			}
		}
		for(uint32_t attributeIndex = 0; attributeIndex < desc.inputAssembly.vertexAttributeCount; ++attributeIndex)
		{
			const RHI::VertexAttributeDesc& attribute = desc.inputAssembly.vertexAttributes[attributeIndex];
			if(attribute.semanticName == nullptr || attribute.semanticName[0] == '\0') return nullptr;
			bool hasBinding = false;
			for(uint32_t bindingIndex = 0; bindingIndex < desc.inputAssembly.vertexBindingCount; ++bindingIndex)
			{
				if(desc.inputAssembly.vertexBindings[bindingIndex].slot == attribute.binding)
				{
					hasBinding = true;
					break;
				}
			}
			if(!hasBinding) return nullptr;
			switch(attribute.format)
			{
			case RHI::Format::R32_FLOAT:
			case RHI::Format::R32G32_FLOAT:
			case RHI::Format::R32G32B32_FLOAT:
			case RHI::Format::R32G32B32A32_FLOAT:
			case RHI::Format::R8G8B8A8_UNORM:
				break;
			default:
				return nullptr;
			}
		}

		if((desc.pipelineLayout.resourceBindingCount > 0 && desc.pipelineLayout.resourceBindings == nullptr) ||
			(desc.pipelineLayout.inlineConstantRangeCount > 0 && desc.pipelineLayout.inlineConstantRanges == nullptr)) return nullptr;
		for(uint32_t bindingIndex = 0; bindingIndex < desc.pipelineLayout.resourceBindingCount; ++bindingIndex)
		{
			const RHI::ResourceBindingDesc& binding = desc.pipelineLayout.resourceBindings[bindingIndex];
			if(binding.arrayCount == 0 || binding.stages == RHI::ShaderStageFlags::None ||
				static_cast<uint32_t>(binding.type) > static_cast<uint32_t>(RHI::ResourceType::TextureSampler)) return nullptr;
			for(uint32_t previous = 0; previous < bindingIndex; ++previous)
			{
				const RHI::ResourceBindingDesc& other = desc.pipelineLayout.resourceBindings[previous];
				if(other.set == binding.set && other.binding == binding.binding) return nullptr;
			}
		}
		for(uint32_t rangeIndex = 0; rangeIndex < desc.pipelineLayout.inlineConstantRangeCount; ++rangeIndex)
		{
			const RHI::InlineConstantRangeDesc& range = desc.pipelineLayout.inlineConstantRanges[rangeIndex];
			if(range.size == 0 || range.stages == RHI::ShaderStageFlags::None) return nullptr;
		}

		return new NullPipelineState(desc);
	}

	RHI::IResourceSet* NullDevice::CreateResourceSet(RHI::IPipelineState* pipeline)
	{
		auto* nullPipeline = dynamic_cast<NullPipelineState*>(pipeline);
		return nullPipeline == nullptr ? nullptr : new NullResourceSet(nullPipeline);
	}

	bool NullDevice::UpdateResourceSet(RHI::IResourceSet* resourceSet, const RHI::ResourceSetWrite* writes, uint32_t writeCount)
	{
		auto* nullSet = dynamic_cast<NullResourceSet*>(resourceSet);
		if(nullSet == nullptr || (writeCount > 0 && writes == nullptr)) return false;
		for(uint32_t writeIndex = 0; writeIndex < writeCount; ++writeIndex)
		{
			const RHI::ResourceSetWrite& write = writes[writeIndex];
			const RHI::ResourceBindingDesc* declared = nullptr;
			for(const RHI::ResourceBindingDesc& binding : nullSet->pipeline->bindings)
			{
				if(binding.set == write.set && binding.binding == write.binding)
				{
					declared = &binding;
					break;
				}
			}
			if(declared == nullptr || write.arrayElement >= declared->arrayCount) return false;
			switch(declared->type)
			{
			case RHI::ResourceType::ConstantBuffer:
			case RHI::ResourceType::StorageBuffer:
				if(write.buffer == nullptr || write.texture != nullptr || write.sampler != nullptr) return false;
				break;
			case RHI::ResourceType::TextureSampler:
				if(write.buffer != nullptr || write.texture == nullptr || write.sampler == nullptr) return false;
				break;
			default:
				return false;
			}
		}
		return true;
	}

	void NullDevice::DestroyBuffer(RHI::IBuffer* buffer)
	{
		delete buffer;
	}

	void NullDevice::DestroyShader(RHI::IShader* shader)
	{
		delete shader;
	}

	void NullDevice::DestroySampler(RHI::ISampler* sampler)
	{
		delete sampler;
	}

	void NullDevice::DestroyTexture(RHI::ITexture* texture)
	{
		delete texture;
	}

	void NullDevice::DestroyPipelineState(RHI::IPipelineState* pipeline)
	{
		delete pipeline;
	}

	void NullDevice::DestroyResourceSet(RHI::IResourceSet* resourceSet)
	{
		delete resourceSet;
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
