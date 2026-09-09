#include "MetalPipeline.h"

#include "MetalShader.h"

#include <cmath>
#include <set>

#import <Metal/Metal.h>

namespace dy::Backends
{
	namespace
	{
		[[nodiscard]] MTLPixelFormat ToPixelFormat(RHI::Format format)
		{
			switch(format)
			{
			case RHI::Format::R8G8B8A8_UNORM: return MTLPixelFormatRGBA8Unorm;
			case RHI::Format::B8G8R8A8_UNORM: return MTLPixelFormatBGRA8Unorm;
			case RHI::Format::R8G8B8A8_UNORM_SRGB: return MTLPixelFormatRGBA8Unorm_sRGB;
			case RHI::Format::B8G8R8A8_UNORM_SRGB: return MTLPixelFormatBGRA8Unorm_sRGB;
			case RHI::Format::R16G16B16A16_FLOAT: return MTLPixelFormatRGBA16Float;
			case RHI::Format::R32G32_FLOAT: return MTLPixelFormatRG32Float;
			case RHI::Format::R32G32B32A32_FLOAT: return MTLPixelFormatRGBA32Float;
			case RHI::Format::D32_FLOAT: return MTLPixelFormatDepth32Float;
			case RHI::Format::D24_UNORM_S8_UINT: return MTLPixelFormatDepth24Unorm_Stencil8;
			case RHI::Format::R32_UINT: return MTLPixelFormatR32Uint;
			case RHI::Format::R16_UINT: return MTLPixelFormatR16Uint;
			default: return MTLPixelFormatInvalid;
			}
		}

		[[nodiscard]] MTLVertexFormat ToVertexFormat(RHI::Format format)
		{
			switch(format)
			{
			case RHI::Format::R32G32_FLOAT: return MTLVertexFormatFloat2;
			case RHI::Format::R32G32B32_FLOAT: return MTLVertexFormatFloat3;
			case RHI::Format::R32G32B32A32_FLOAT: return MTLVertexFormatFloat4;
			case RHI::Format::R32_UINT: return MTLVertexFormatUInt;
			case RHI::Format::R16_UINT: return MTLVertexFormatUShort;
			default: return MTLVertexFormatInvalid;
			}
		}

		[[nodiscard]] uint32_t VertexFormatSize(RHI::Format format)
		{
			switch(format)
			{
			case RHI::Format::R32G32_FLOAT: return 8;
			case RHI::Format::R32G32B32_FLOAT: return 12;
			case RHI::Format::R32G32B32A32_FLOAT: return 16;
			case RHI::Format::R32_UINT: return 4;
			case RHI::Format::R16_UINT: return 2;
			default: return 0;
			}
		}

		[[nodiscard]] MTLCompareFunction ToCompareFunction(RHI::CompareOp operation)
		{
			switch(operation)
			{
			case RHI::CompareOp::Never: return MTLCompareFunctionNever;
			case RHI::CompareOp::Less: return MTLCompareFunctionLess;
			case RHI::CompareOp::Equal: return MTLCompareFunctionEqual;
			case RHI::CompareOp::LessEqual: return MTLCompareFunctionLessEqual;
			case RHI::CompareOp::Greater: return MTLCompareFunctionGreater;
			case RHI::CompareOp::NotEqual: return MTLCompareFunctionNotEqual;
			case RHI::CompareOp::GreaterEqual: return MTLCompareFunctionGreaterEqual;
			case RHI::CompareOp::Always: return MTLCompareFunctionAlways;
			default: return MTLCompareFunctionNever;
			}
		}

		[[nodiscard]] MTLStencilOperation ToStencilOperation(RHI::StencilOp operation)
		{
			switch(operation)
			{
			case RHI::StencilOp::Keep: return MTLStencilOperationKeep;
			case RHI::StencilOp::Zero: return MTLStencilOperationZero;
			case RHI::StencilOp::Replace: return MTLStencilOperationReplace;
			case RHI::StencilOp::IncrementClamp: return MTLStencilOperationIncrementClamp;
			case RHI::StencilOp::DecrementClamp: return MTLStencilOperationDecrementClamp;
			case RHI::StencilOp::Invert: return MTLStencilOperationInvert;
			case RHI::StencilOp::IncrementWrap: return MTLStencilOperationIncrementWrap;
			case RHI::StencilOp::DecrementWrap: return MTLStencilOperationDecrementWrap;
			default: return MTLStencilOperationKeep;
			}
		}

		[[nodiscard]] MTLBlendFactor ToBlendFactor(RHI::BlendFactor factor)
		{
			switch(factor)
			{
			case RHI::BlendFactor::Zero: return MTLBlendFactorZero;
			case RHI::BlendFactor::One: return MTLBlendFactorOne;
			case RHI::BlendFactor::SourceColor: return MTLBlendFactorSourceColor;
			case RHI::BlendFactor::OneMinusSourceColor: return MTLBlendFactorOneMinusSourceColor;
			case RHI::BlendFactor::DestinationColor: return MTLBlendFactorDestinationColor;
			case RHI::BlendFactor::OneMinusDestinationColor: return MTLBlendFactorOneMinusDestinationColor;
			case RHI::BlendFactor::SourceAlpha: return MTLBlendFactorSourceAlpha;
			case RHI::BlendFactor::OneMinusSourceAlpha: return MTLBlendFactorOneMinusSourceAlpha;
			case RHI::BlendFactor::DestinationAlpha: return MTLBlendFactorDestinationAlpha;
			case RHI::BlendFactor::OneMinusDestinationAlpha: return MTLBlendFactorOneMinusDestinationAlpha;
			default: return MTLBlendFactorZero;
			}
		}

		[[nodiscard]] MTLBlendOperation ToBlendOperation(RHI::BlendOp operation)
		{
			switch(operation)
			{
			case RHI::BlendOp::Add: return MTLBlendOperationAdd;
			case RHI::BlendOp::Subtract: return MTLBlendOperationSubtract;
			case RHI::BlendOp::ReverseSubtract: return MTLBlendOperationReverseSubtract;
			case RHI::BlendOp::Min: return MTLBlendOperationMin;
			case RHI::BlendOp::Max: return MTLBlendOperationMax;
			default: return MTLBlendOperationAdd;
			}
		}

		[[nodiscard]] MTLColorWriteMask ToColorWriteMask(RHI::ColorWriteMask mask)
		{
			MTLColorWriteMask result = MTLColorWriteMaskNone;
			const uint8_t bits = static_cast<uint8_t>(mask);
			if((bits & static_cast<uint8_t>(RHI::ColorWriteMask::Red)) != 0)
				result |= MTLColorWriteMaskRed;
			if((bits & static_cast<uint8_t>(RHI::ColorWriteMask::Green)) != 0)
				result |= MTLColorWriteMaskGreen;
			if((bits & static_cast<uint8_t>(RHI::ColorWriteMask::Blue)) != 0)
				result |= MTLColorWriteMaskBlue;
			if((bits & static_cast<uint8_t>(RHI::ColorWriteMask::Alpha)) != 0)
				result |= MTLColorWriteMaskAlpha;
			return result;
		}

		[[nodiscard]] MTLSamplerMinMagFilter ToMinMagFilter(RHI::SamplerFilter filter)
		{
			return filter == RHI::SamplerFilter::Linear
				? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
		}

		[[nodiscard]] MTLSamplerMipFilter ToMipFilter(RHI::SamplerFilter filter)
		{
			return filter == RHI::SamplerFilter::Linear
				? MTLSamplerMipFilterLinear : MTLSamplerMipFilterNearest;
		}

		[[nodiscard]] MTLSamplerAddressMode ToAddressMode(RHI::SamplerAddressMode mode)
		{
			switch(mode)
			{
			case RHI::SamplerAddressMode::Repeat: return MTLSamplerAddressModeRepeat;
			case RHI::SamplerAddressMode::MirroredRepeat: return MTLSamplerAddressModeMirrorRepeat;
			case RHI::SamplerAddressMode::ClampToEdge: return MTLSamplerAddressModeClampToEdge;
			case RHI::SamplerAddressMode::ClampToBorder: return MTLSamplerAddressModeClampToBorderColor;
			default: return MTLSamplerAddressModeClampToEdge;
			}
		}

		[[nodiscard]] MTLSamplerBorderColor ToBorderColor(RHI::SamplerBorderColor color)
		{
			switch(color)
			{
			case RHI::SamplerBorderColor::TransparentBlack:
				return MTLSamplerBorderColorTransparentBlack;
			case RHI::SamplerBorderColor::OpaqueBlack:
				return MTLSamplerBorderColorOpaqueBlack;
			case RHI::SamplerBorderColor::OpaqueWhite:
				return MTLSamplerBorderColorOpaqueWhite;
			default:
				return MTLSamplerBorderColorTransparentBlack;
			}
		}

		[[nodiscard]] bool UsesBorder(const RHI::SamplerDesc& desc)
		{
			return desc.addressU == RHI::SamplerAddressMode::ClampToBorder ||
				desc.addressV == RHI::SamplerAddressMode::ClampToBorder ||
				desc.addressW == RHI::SamplerAddressMode::ClampToBorder;
		}

		[[nodiscard]] bool IsValidSampler(const RHI::SamplerDesc& desc)
		{
			return desc.minFilter != RHI::SamplerFilter::Undefined &&
				desc.magFilter != RHI::SamplerFilter::Undefined &&
				desc.mipFilter != RHI::SamplerFilter::Undefined &&
				desc.addressU != RHI::SamplerAddressMode::Undefined &&
				desc.addressV != RHI::SamplerAddressMode::Undefined &&
				desc.addressW != RHI::SamplerAddressMode::Undefined &&
				(!UsesBorder(desc) || desc.borderColor != RHI::SamplerBorderColor::Undefined) &&
				desc.maxAnisotropy != 0 && std::isfinite(desc.mipLodBias) &&
				desc.mipLodBias == 0.0f && std::isfinite(desc.minLod) &&
				std::isfinite(desc.maxLod) && desc.minLod <= desc.maxLod;
		}

		[[nodiscard]] bool HasStage(
			RHI::ShaderStageFlags stages,
			RHI::ShaderStageFlags stage)
		{
			return (stages & stage) != RHI::ShaderStageFlags::None;
		}

		[[nodiscard]] bool ValidateMetalLayout(
			const RHI::GraphicsPipelineDesc& desc)
		{
			constexpr uint32_t nativeBufferBindingCount = 31;
			constexpr uint32_t nativeTextureBindingCount = 128;
			constexpr uint32_t nativeSamplerBindingCount = 16;
			std::set<uint32_t> vertexBufferBindings;
			for(uint32_t index = 0; index < desc.vertexBufferCount; ++index)
			{
				const RHI::VertexBufferLayout& layout = desc.vertexBuffers[index];
				if(layout.binding >= nativeBufferBindingCount || layout.stride == 0 ||
					layout.stepMode == RHI::VertexStepMode::Undefined ||
					!vertexBufferBindings.insert(layout.binding).second)
				{
					return false;
				}
			}

			std::set<uint32_t> locations;
			for(uint32_t index = 0; index < desc.vertexAttributeCount; ++index)
			{
				const RHI::VertexAttribute& attribute = desc.vertexAttributes[index];
				const RHI::VertexBufferLayout* layout = nullptr;
				for(uint32_t layoutIndex = 0;
					layoutIndex < desc.vertexBufferCount; ++layoutIndex)
				{
					if(desc.vertexBuffers[layoutIndex].binding == attribute.binding)
					{
						layout = &desc.vertexBuffers[layoutIndex];
						break;
					}
				}
				const uint32_t attributeSize = VertexFormatSize(attribute.format);
				if(layout == nullptr ||
					ToVertexFormat(attribute.format) == MTLVertexFormatInvalid || attributeSize == 0 ||
					attribute.location >= 31 || !locations.insert(attribute.location).second ||
					attribute.offset > layout->stride ||
					attributeSize > layout->stride - attribute.offset)
				{
					return false;
				}
			}

			std::set<uint32_t> declarations;
			std::set<uint32_t> vertexBufferSlots = vertexBufferBindings;
			std::set<uint32_t> fragmentBufferSlots;
			std::set<uint32_t> vertexTextureSlots;
			std::set<uint32_t> fragmentTextureSlots;
			std::set<uint32_t> vertexSamplerSlots;
			std::set<uint32_t> fragmentSamplerSlots;
			constexpr auto graphicsStages = RHI::ShaderStageFlags::Vertex |
				RHI::ShaderStageFlags::Fragment;
			for(uint32_t index = 0; index < desc.layout.bindingCount; ++index)
			{
				const RHI::ResourceBindingLayout& binding = desc.layout.bindings[index];
				if(binding.type == RHI::ResourceBindingType::Undefined ||
					binding.count == 0 ||
					binding.stages == RHI::ShaderStageFlags::None ||
					(binding.stages & graphicsStages) != binding.stages ||
					!declarations.insert(binding.binding).second)
				{
					return false;
				}
				uint32_t limit = nativeBufferBindingCount;
				if(binding.type == RHI::ResourceBindingType::SampledTexture ||
					binding.type == RHI::ResourceBindingType::StorageTexture)
					limit = nativeTextureBindingCount;
				else if(binding.type == RHI::ResourceBindingType::StaticSampler)
					limit = nativeSamplerBindingCount;
				if(binding.binding >= limit || binding.count > limit - binding.binding)
					return false;
				if(binding.type == RHI::ResourceBindingType::StaticSampler &&
					!IsValidSampler(binding.staticSampler))
				{
					return false;
				}

				for(uint32_t element = 0; element < binding.count; ++element)
				{
					const uint32_t slot = binding.binding + element;
					std::set<uint32_t>* vertexSlots = &vertexBufferSlots;
					std::set<uint32_t>* fragmentSlots = &fragmentBufferSlots;
					if(binding.type == RHI::ResourceBindingType::SampledTexture ||
						binding.type == RHI::ResourceBindingType::StorageTexture)
					{
						vertexSlots = &vertexTextureSlots;
						fragmentSlots = &fragmentTextureSlots;
					}
					else if(binding.type == RHI::ResourceBindingType::StaticSampler)
					{
						vertexSlots = &vertexSamplerSlots;
						fragmentSlots = &fragmentSamplerSlots;
					}
					if(HasStage(binding.stages, RHI::ShaderStageFlags::Vertex) &&
						!vertexSlots->insert(slot).second)
					{
						return false;
					}
					if(HasStage(binding.stages, RHI::ShaderStageFlags::Fragment) &&
						!fragmentSlots->insert(slot).second)
					{
						return false;
					}
				}
			}

			if(desc.layout.inlineConstantSize != 0)
			{
				if(desc.layout.inlineConstantBinding >= nativeBufferBindingCount ||
					desc.layout.inlineConstantStages == RHI::ShaderStageFlags::None ||
					(desc.layout.inlineConstantSize % sizeof(uint32_t)) != 0)
				{
					return false;
				}
				if((desc.layout.inlineConstantStages & graphicsStages) !=
					desc.layout.inlineConstantStages)
				{
					return false;
				}
				if(HasStage(desc.layout.inlineConstantStages, RHI::ShaderStageFlags::Vertex) &&
					!vertexBufferSlots.insert(desc.layout.inlineConstantBinding).second)
				{
					return false;
				}
				if(HasStage(desc.layout.inlineConstantStages, RHI::ShaderStageFlags::Fragment) &&
					!fragmentBufferSlots.insert(desc.layout.inlineConstantBinding).second)
					return false;
			}
			return true;
		}
	}

	struct MetalPipeline::Impl
	{
		id<MTLRenderPipelineState> pipelineState = nil;
		id<MTLDepthStencilState> depthStencilState = nil;
		MTLPrimitiveType primitiveType = MTLPrimitiveTypeTriangle;
		MTLCullMode cullMode = MTLCullModeNone;
		MTLWinding frontFace = MTLWindingCounterClockwise;
		MTLTriangleFillMode fillMode = MTLTriangleFillModeFill;
		std::vector<MetalStaticSamplerBinding> staticSamplers;
		std::vector<void*> ownedStaticSamplers;
	};

	MetalPipeline::MetalPipeline(
		const RHI::GraphicsPipelineDesc& desc,
		void* device)
		: RHI::Pipeline(desc.layout)
		, m_impl(new Impl())
		, m_desc(desc)
	{
		if(desc.vertexBuffers != nullptr && desc.vertexBufferCount != 0)
			m_vertexBuffers.assign(
				desc.vertexBuffers, desc.vertexBuffers + desc.vertexBufferCount);
		if(desc.vertexAttributes != nullptr && desc.vertexAttributeCount != 0)
			m_vertexAttributes.assign(
				desc.vertexAttributes, desc.vertexAttributes + desc.vertexAttributeCount);
		if(desc.colorAttachments != nullptr && desc.colorAttachmentCount != 0)
			m_colorAttachments.assign(
				desc.colorAttachments, desc.colorAttachments + desc.colorAttachmentCount);
		m_desc.vertexBuffers = m_vertexBuffers.empty() ? nullptr : m_vertexBuffers.data();
		m_desc.vertexAttributes = m_vertexAttributes.empty() ? nullptr : m_vertexAttributes.data();
		m_desc.colorAttachments = m_colorAttachments.empty() ? nullptr : m_colorAttachments.data();
		m_desc.layout = GetLayout();

		id<MTLDevice> metalDevice = (__bridge id<MTLDevice>)device;
		auto* vertexShader = dynamic_cast<MetalShader*>(desc.vertexShader);
		auto* fragmentShader = dynamic_cast<MetalShader*>(desc.fragmentShader);
		if(metalDevice == nil || vertexShader == nullptr ||
			vertexShader->GetNativeFunction() == nullptr ||
			vertexShader->GetStage() != RHI::ShaderStage::Vertex ||
			(desc.fragmentShader != nullptr &&
				(fragmentShader == nullptr || fragmentShader->GetNativeFunction() == nullptr ||
					fragmentShader->GetStage() != RHI::ShaderStage::Fragment)) ||
			(desc.colorAttachmentCount != 0 && fragmentShader == nullptr) ||
			(desc.vertexBufferCount != 0 && desc.vertexBuffers == nullptr) ||
			(desc.vertexAttributeCount != 0 && desc.vertexAttributes == nullptr) ||
			(desc.colorAttachmentCount != 0 && desc.colorAttachments == nullptr) ||
			desc.colorAttachmentCount > 8 ||
			(desc.depthStencil.format == RHI::Format::Unknown &&
				(desc.depthStencil.depthTestEnabled ||
					desc.depthStencil.depthWriteEnabled ||
					desc.depthStencil.stencilEnabled)) ||
			!std::isfinite(desc.raster.depthBiasConstant) ||
			!std::isfinite(desc.raster.depthBiasSlope) ||
			!std::isfinite(desc.raster.depthBiasClamp) ||
			(desc.layout.bindingCount != 0 && desc.layout.bindings == nullptr) ||
			!ValidateMetalLayout(desc))
		{
			return;
		}

		switch(desc.topology)
		{
		case RHI::PrimitiveTopology::PointList: m_impl->primitiveType = MTLPrimitiveTypePoint; break;
		case RHI::PrimitiveTopology::LineList: m_impl->primitiveType = MTLPrimitiveTypeLine; break;
		case RHI::PrimitiveTopology::TriangleList: m_impl->primitiveType = MTLPrimitiveTypeTriangle; break;
		case RHI::PrimitiveTopology::TriangleStrip: m_impl->primitiveType = MTLPrimitiveTypeTriangleStrip; break;
		default: return;
		}

		switch(desc.raster.cullMode)
		{
		case RHI::CullMode::None: m_impl->cullMode = MTLCullModeNone; break;
		case RHI::CullMode::Front: m_impl->cullMode = MTLCullModeFront; break;
		case RHI::CullMode::Back: m_impl->cullMode = MTLCullModeBack; break;
		default: return;
		}
		switch(desc.raster.frontFace)
		{
		case RHI::FrontFace::CounterClockwise: m_impl->frontFace = MTLWindingCounterClockwise; break;
		case RHI::FrontFace::Clockwise: m_impl->frontFace = MTLWindingClockwise; break;
		default: return;
		}
		switch(desc.raster.fillMode)
		{
		case RHI::FillMode::Solid: m_impl->fillMode = MTLTriangleFillModeFill; break;
		case RHI::FillMode::Wireframe: m_impl->fillMode = MTLTriangleFillModeLines; break;
		default: return;
		}

		for(uint32_t index = 0; index < desc.layout.bindingCount; ++index)
		{
			const RHI::ResourceBindingLayout& binding = desc.layout.bindings[index];
			if(binding.type != RHI::ResourceBindingType::StaticSampler) continue;
			MTLSamplerDescriptor* samplerDesc = [MTLSamplerDescriptor new];
			samplerDesc.minFilter = ToMinMagFilter(binding.staticSampler.minFilter);
			samplerDesc.magFilter = ToMinMagFilter(binding.staticSampler.magFilter);
			samplerDesc.mipFilter = ToMipFilter(binding.staticSampler.mipFilter);
			samplerDesc.sAddressMode = ToAddressMode(binding.staticSampler.addressU);
			samplerDesc.tAddressMode = ToAddressMode(binding.staticSampler.addressV);
			samplerDesc.rAddressMode = ToAddressMode(binding.staticSampler.addressW);
			samplerDesc.maxAnisotropy = binding.staticSampler.maxAnisotropy;
			samplerDesc.lodMinClamp = binding.staticSampler.minLod;
			samplerDesc.lodMaxClamp = binding.staticSampler.maxLod;
			if(UsesBorder(binding.staticSampler))
				samplerDesc.borderColor = ToBorderColor(binding.staticSampler.borderColor);
			id<MTLSamplerState> sampler =
				[metalDevice newSamplerStateWithDescriptor:samplerDesc];
#if !__has_feature(objc_arc)
			[samplerDesc release];
#endif
			if(sampler == nil) return;
			void* storedSampler = nullptr;
#if __has_feature(objc_arc)
			storedSampler = (__bridge_retained void*)sampler;
#else
			storedSampler = (__bridge void*)sampler;
#endif
			m_impl->ownedStaticSamplers.push_back(storedSampler);
			for(uint32_t element = 0; element < binding.count; ++element)
			{
				m_impl->staticSamplers.push_back({
					binding.binding + element,
					binding.stages,
					storedSampler});
			}
		}

		MTLRenderPipelineDescriptor* pipelineDesc = [MTLRenderPipelineDescriptor new];
		pipelineDesc.vertexFunction =
			(__bridge id<MTLFunction>)vertexShader->GetNativeFunction();
		pipelineDesc.fragmentFunction = fragmentShader == nullptr
			? nil : (__bridge id<MTLFunction>)fragmentShader->GetNativeFunction();

		if(!m_vertexAttributes.empty())
		{
			MTLVertexDescriptor* vertexDesc = [MTLVertexDescriptor vertexDescriptor];
			for(const RHI::VertexBufferLayout& layout : m_vertexBuffers)
			{
				vertexDesc.layouts[layout.binding].stride = layout.stride;
				vertexDesc.layouts[layout.binding].stepRate = 1;
				vertexDesc.layouts[layout.binding].stepFunction =
					layout.stepMode == RHI::VertexStepMode::Instance
					? MTLVertexStepFunctionPerInstance : MTLVertexStepFunctionPerVertex;
			}
			for(const RHI::VertexAttribute& attribute : m_vertexAttributes)
			{
				vertexDesc.attributes[attribute.location].format = ToVertexFormat(attribute.format);
				vertexDesc.attributes[attribute.location].offset = attribute.offset;
				vertexDesc.attributes[attribute.location].bufferIndex = attribute.binding;
			}
			pipelineDesc.vertexDescriptor = vertexDesc;
		}

		bool descriptorValid = true;
		for(uint32_t index = 0; index < desc.colorAttachmentCount; ++index)
		{
			const RHI::ColorAttachmentDesc& attachment = desc.colorAttachments[index];
			MTLRenderPipelineColorAttachmentDescriptor* native =
				pipelineDesc.colorAttachments[index];
			native.pixelFormat = ToPixelFormat(attachment.format);
			if(native.pixelFormat == MTLPixelFormatInvalid)
			{
				descriptorValid = false;
				break;
			}
			native.writeMask = ToColorWriteMask(attachment.writeMask);
			native.blendingEnabled = attachment.blend.enabled;
			if(attachment.blend.enabled)
			{
				if(attachment.blend.sourceColor == RHI::BlendFactor::Undefined ||
					attachment.blend.destinationColor == RHI::BlendFactor::Undefined ||
					attachment.blend.colorOp == RHI::BlendOp::Undefined ||
					attachment.blend.sourceAlpha == RHI::BlendFactor::Undefined ||
					attachment.blend.destinationAlpha == RHI::BlendFactor::Undefined ||
					attachment.blend.alphaOp == RHI::BlendOp::Undefined)
				{
					descriptorValid = false;
					break;
				}
				native.sourceRGBBlendFactor = ToBlendFactor(attachment.blend.sourceColor);
				native.destinationRGBBlendFactor = ToBlendFactor(attachment.blend.destinationColor);
				native.rgbBlendOperation = ToBlendOperation(attachment.blend.colorOp);
				native.sourceAlphaBlendFactor = ToBlendFactor(attachment.blend.sourceAlpha);
				native.destinationAlphaBlendFactor = ToBlendFactor(attachment.blend.destinationAlpha);
				native.alphaBlendOperation = ToBlendOperation(attachment.blend.alphaOp);
			}
		}

		const MTLPixelFormat depthStencilFormat = ToPixelFormat(desc.depthStencil.format);
		if(desc.depthStencil.format != RHI::Format::Unknown)
		{
			if(depthStencilFormat != MTLPixelFormatDepth32Float &&
				depthStencilFormat != MTLPixelFormatDepth24Unorm_Stencil8)
			{
				descriptorValid = false;
			}
			pipelineDesc.depthAttachmentPixelFormat = depthStencilFormat;
			if(desc.depthStencil.format == RHI::Format::D24_UNORM_S8_UINT)
				pipelineDesc.stencilAttachmentPixelFormat = depthStencilFormat;
			else if(desc.depthStencil.stencilEnabled)
				descriptorValid = false;
		}

		NSError* error = nil;
		if(descriptorValid)
			m_impl->pipelineState =
				[metalDevice newRenderPipelineStateWithDescriptor:pipelineDesc error:&error];
#if !__has_feature(objc_arc)
		[pipelineDesc release];
#endif
		if(m_impl->pipelineState == nil) return;

		if(desc.depthStencil.format != RHI::Format::Unknown)
		{
			if(desc.depthStencil.depthTestEnabled &&
				desc.depthStencil.depthCompareOp == RHI::CompareOp::Undefined)
			{
				return;
			}
			MTLDepthStencilDescriptor* depthDesc = [MTLDepthStencilDescriptor new];
			depthDesc.depthCompareFunction = desc.depthStencil.depthTestEnabled
				? ToCompareFunction(desc.depthStencil.depthCompareOp)
				: MTLCompareFunctionAlways;
			depthDesc.depthWriteEnabled = desc.depthStencil.depthWriteEnabled;
			if(desc.depthStencil.stencilEnabled)
			{
				const auto makeStencil = [](
					const RHI::StencilFaceState& face) -> MTLStencilDescriptor*
				{
					if(face.failOp == RHI::StencilOp::Undefined ||
						face.depthFailOp == RHI::StencilOp::Undefined ||
						face.passOp == RHI::StencilOp::Undefined ||
						face.compareOp == RHI::CompareOp::Undefined)
					{
						return nil;
					}
					MTLStencilDescriptor* result = [MTLStencilDescriptor new];
					result.stencilFailureOperation = ToStencilOperation(face.failOp);
					result.depthFailureOperation = ToStencilOperation(face.depthFailOp);
					result.depthStencilPassOperation = ToStencilOperation(face.passOp);
					result.stencilCompareFunction = ToCompareFunction(face.compareOp);
					return result;
				};
				MTLStencilDescriptor* front = makeStencil(desc.depthStencil.front);
				MTLStencilDescriptor* back = makeStencil(desc.depthStencil.back);
				if(front == nil || back == nil)
				{
#if !__has_feature(objc_arc)
					[front release];
					[back release];
					[depthDesc release];
#endif
					return;
				}
				front.readMask = desc.depthStencil.stencilReadMask;
				front.writeMask = desc.depthStencil.stencilWriteMask;
				back.readMask = desc.depthStencil.stencilReadMask;
				back.writeMask = desc.depthStencil.stencilWriteMask;
				depthDesc.frontFaceStencil = front;
				depthDesc.backFaceStencil = back;
#if !__has_feature(objc_arc)
				[front release];
				[back release];
#endif
			}
			m_impl->depthStencilState =
				[metalDevice newDepthStencilStateWithDescriptor:depthDesc];
#if !__has_feature(objc_arc)
			[depthDesc release];
#endif
			if(m_impl->depthStencilState == nil) return;
		}
	}

	MetalPipeline::~MetalPipeline()
	{
		if(m_impl == nullptr) return;
#if __has_feature(objc_arc)
		for(void* sampler : m_impl->ownedStaticSamplers)
		{
			id<MTLSamplerState> released =
				(__bridge_transfer id<MTLSamplerState>)sampler;
			(void)released;
		}
#else
		for(void* sampler : m_impl->ownedStaticSamplers)
			[(__bridge id<MTLSamplerState>)sampler release];
#endif
		m_impl->ownedStaticSamplers.clear();
		m_impl->staticSamplers.clear();
#if !__has_feature(objc_arc)
		[m_impl->depthStencilState release];
		[m_impl->pipelineState release];
#endif
		m_impl->depthStencilState = nil;
		m_impl->pipelineState = nil;
		delete m_impl;
	}

	const RHI::GraphicsPipelineDesc& MetalPipeline::GetDesc() const { return m_desc; }
	void* MetalPipeline::GetNativePipeline() const
	{
		return m_impl == nullptr ? nullptr : (__bridge void*)m_impl->pipelineState;
	}
	void* MetalPipeline::GetNativeDepthStencil() const
	{
		return m_impl == nullptr ? nullptr : (__bridge void*)m_impl->depthStencilState;
	}
	uint32_t MetalPipeline::GetNativePrimitiveType() const
	{
		return m_impl == nullptr ? 0 : static_cast<uint32_t>(m_impl->primitiveType);
	}
	uint32_t MetalPipeline::GetNativeCullMode() const
	{
		return m_impl == nullptr ? 0 : static_cast<uint32_t>(m_impl->cullMode);
	}
	uint32_t MetalPipeline::GetNativeFrontFace() const
	{
		return m_impl == nullptr ? 0 : static_cast<uint32_t>(m_impl->frontFace);
	}
	uint32_t MetalPipeline::GetNativeFillMode() const
	{
		return m_impl == nullptr ? 0 : static_cast<uint32_t>(m_impl->fillMode);
	}

	const std::vector<MetalStaticSamplerBinding>&
	MetalPipeline::GetStaticSamplerBindings() const
	{
		return m_impl->staticSamplers;
	}
}
