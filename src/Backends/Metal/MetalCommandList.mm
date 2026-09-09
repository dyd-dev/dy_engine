#include "MetalCommandList.h"

#include "MetalBuffer.h"
#include "MetalPipeline.h"
#include "MetalResourceSet.h"
#include "MetalTexture.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <unordered_map>
#include <vector>

#import <Metal/Metal.h>

namespace dy::Backends
{
	namespace
	{
		[[nodiscard]] bool HasUsage(RHI::BufferUsage value, RHI::BufferUsage usage)
		{
			return (value & usage) != RHI::BufferUsage::None;
		}

		[[nodiscard]] bool HasUsage(RHI::TextureUsage value, RHI::TextureUsage usage)
		{
			return (value & usage) != RHI::TextureUsage::None;
		}

		[[nodiscard]] bool HasStage(
			RHI::ShaderStageFlags stages,
			RHI::ShaderStageFlags stage)
		{
			return (stages & stage) != RHI::ShaderStageFlags::None;
		}

		[[nodiscard]] uint32_t FormatSize(RHI::Format format)
		{
			switch(format)
			{
			case RHI::Format::R8G8B8A8_UNORM:
			case RHI::Format::B8G8R8A8_UNORM:
			case RHI::Format::R8G8B8A8_UNORM_SRGB:
			case RHI::Format::B8G8R8A8_UNORM_SRGB:
			case RHI::Format::D32_FLOAT:
			case RHI::Format::D24_UNORM_S8_UINT:
			case RHI::Format::R32_UINT:
				return 4;
			case RHI::Format::R16G16B16A16_FLOAT:
			case RHI::Format::R32G32_FLOAT:
				return 8;
			case RHI::Format::R32G32B32_FLOAT:
				return 12;
			case RHI::Format::R32G32B32A32_FLOAT:
				return 16;
			case RHI::Format::R16_UINT:
				return 2;
			default:
				return 0;
			}
		}

		[[nodiscard]] bool IsBufferStateAllowed(
			const RHI::BufferDesc& desc,
			RHI::ResourceState state)
		{
			switch(state)
			{
			case RHI::ResourceState::Undefined:
			case RHI::ResourceState::Common:
			case RHI::ResourceState::CopyDestination:
				return true;
			case RHI::ResourceState::VertexBuffer:
				return HasUsage(desc.usage, RHI::BufferUsage::Vertex);
			case RHI::ResourceState::IndexBuffer:
				return HasUsage(desc.usage, RHI::BufferUsage::Index);
			case RHI::ResourceState::ConstantBuffer:
				return HasUsage(desc.usage, RHI::BufferUsage::Constant);
			case RHI::ResourceState::ShaderResource:
			case RHI::ResourceState::UnorderedAccess:
				return HasUsage(desc.usage, RHI::BufferUsage::Storage);
			default:
				return false;
			}
		}

		[[nodiscard]] bool IsTextureStateAllowed(
			const RHI::TextureDesc& desc,
			RHI::ResourceState state)
		{
			switch(state)
			{
			case RHI::ResourceState::Undefined:
			case RHI::ResourceState::Common:
			case RHI::ResourceState::CopyDestination:
				return true;
			case RHI::ResourceState::ShaderResource:
				return HasUsage(desc.usage, RHI::TextureUsage::ShaderResource);
			case RHI::ResourceState::UnorderedAccess:
				return HasUsage(desc.usage, RHI::TextureUsage::Storage);
			case RHI::ResourceState::RenderTarget:
			case RHI::ResourceState::Present:
				return HasUsage(desc.usage, RHI::TextureUsage::RenderTarget);
			case RHI::ResourceState::DepthRead:
			case RHI::ResourceState::DepthWrite:
				return HasUsage(desc.usage, RHI::TextureUsage::DepthStencil);
			default:
				return false;
			}
		}

		[[nodiscard]] id<MTLBuffer> NativeBuffer(MetalBuffer* buffer)
		{
			return buffer == nullptr
				? nil : (__bridge id<MTLBuffer>)buffer->GetNativeBuffer();
		}

		[[nodiscard]] id<MTLTexture> NativeTexture(MetalTexture* texture)
		{
			return texture == nullptr
				? nil : (__bridge id<MTLTexture>)texture->GetNativeTexture();
		}

		[[nodiscard]] std::pair<MetalTexture*, uint32_t> TextureKey(
			MetalTexture* texture,
			uint32_t mipLevel,
			uint32_t arrayLayer)
		{
			return {texture, arrayLayer * texture->GetDesc().mipLevels + mipLevel};
		}

		[[nodiscard]] bool ResolveSubresources(
			MetalTexture* texture,
			const RHI::TextureSubresourceRange& range,
			uint32_t& firstMip,
			uint32_t& mipCount,
			uint32_t& firstLayer,
			uint32_t& layerCount)
		{
			firstMip = range.firstMipLevel;
			firstLayer = range.firstArrayLayer;
			if(firstMip >= texture->GetDesc().mipLevels ||
				firstLayer >= texture->GetDesc().depthOrArraySize)
			{
				return false;
			}
			mipCount = range.mipLevelCount == 0
				? texture->GetDesc().mipLevels - firstMip : range.mipLevelCount;
			layerCount = range.arrayLayerCount == 0
				? texture->GetDesc().depthOrArraySize - firstLayer : range.arrayLayerCount;
			return mipCount <= texture->GetDesc().mipLevels - firstMip &&
				layerCount <= texture->GetDesc().depthOrArraySize - firstLayer;
		}

		[[nodiscard]] MTLLoadAction ToLoadAction(RHI::LoadOp operation)
		{
			switch(operation)
			{
			case RHI::LoadOp::Load: return MTLLoadActionLoad;
			case RHI::LoadOp::Clear: return MTLLoadActionClear;
			case RHI::LoadOp::Discard: return MTLLoadActionDontCare;
			default: return MTLLoadActionDontCare;
			}
		}

		[[nodiscard]] MTLStoreAction ToStoreAction(RHI::StoreOp operation)
		{
			switch(operation)
			{
			case RHI::StoreOp::Store: return MTLStoreActionStore;
			case RHI::StoreOp::Discard: return MTLStoreActionDontCare;
			default: return MTLStoreActionDontCare;
			}
		}

		[[nodiscard]] const RHI::ResourceBindingLayout* FindLayoutBinding(
			const RHI::PipelineLayoutDesc& layout,
			uint32_t binding)
		{
			for(uint32_t index = 0; index < layout.bindingCount; ++index)
			{
				const RHI::ResourceBindingLayout& candidate = layout.bindings[index];
				if(candidate.binding == binding)
					return &candidate;
			}
			return nullptr;
		}

		enum class OperationKind : uint8_t
		{
			BufferBarrier,
			TextureBarrier,
			BufferRequirement,
			TextureRequirement,
			BufferWrite,
			TextureWrite
		};

		struct MetalOperation
		{
			OperationKind kind = OperationKind::BufferBarrier;
			MetalBuffer* buffer = nullptr;
			MetalTexture* texture = nullptr;
			RHI::ResourceState before = RHI::ResourceState::Undefined;
			RHI::ResourceState after = RHI::ResourceState::Undefined;
			uint32_t mipLevel = 0;
			uint32_t arrayLayer = 0;
		};

		struct VertexBinding
		{
			MetalBuffer* buffer = nullptr;
			uint32_t offset = 0;
		};
	}

	struct MetalCommandList::Impl
	{
		id<MTLCommandQueue> commandQueue = nil;
		id<MTLCommandBuffer> commandBuffer = nil;
		id<MTLRenderCommandEncoder> renderEncoder = nil;
		id<MTLBlitCommandEncoder> blitEncoder = nil;
		NSMutableArray<id<MTLResource>>* retainedResources = nil;

		std::vector<MetalOperation> operations;
		std::unordered_map<MetalBuffer*, RHI::ResourceState> bufferStates;
		std::map<std::pair<MetalTexture*, uint32_t>, RHI::ResourceState> textureStates;
		std::unordered_map<uint32_t, VertexBinding> vertexBindings;
		std::vector<MetalTexture*> referencedSwapchainImages;
		std::vector<RHI::Format> colorFormats;
		std::vector<uint8_t> inlineConstants;
		std::vector<uint8_t> inlineConstantCoverage;

		MetalPipeline* pipeline = nullptr;
		MetalResourceSet* resourceSet = nullptr;
		MetalBuffer* indexBuffer = nullptr;
		MetalTexture* depthTexture = nullptr;
		RHI::Format indexFormat = RHI::Format::Unknown;
		RHI::Format depthFormat = RHI::Format::Unknown;
		uint32_t depthMipLevel = 0;
		uint32_t depthArrayLayer = 0;
		uint32_t indexOffset = 0;
		uint32_t renderWidth = 0;
		uint32_t renderHeight = 0;
		bool rendering = false;
		bool stencilConfigured = false;
		bool viewportSet = false;
		bool scissorSet = false;
		bool stencilReferenceSet = false;
		bool closed = false;
		bool valid = true;
		bool usesBackBuffer = false;
	};

	namespace
	{
		template<typename ImplType>
		void Invalidate(ImplType* impl)
		{
			impl->valid = false;
		}

		template<typename ImplType>
		void EndBlitEncoding(ImplType* impl)
		{
			if(impl->blitEncoder == nil) return;
			[impl->blitEncoder endEncoding];
#if !__has_feature(objc_arc)
			[impl->blitEncoder release];
#endif
			impl->blitEncoder = nil;
		}

		template<typename ImplType>
		void EndRenderEncoding(ImplType* impl)
		{
			if(impl->renderEncoder == nil) return;
			[impl->renderEncoder endEncoding];
#if !__has_feature(objc_arc)
			[impl->renderEncoder release];
#endif
			impl->renderEncoder = nil;
		}

		template<typename ImplType>
		[[nodiscard]] bool RequireBufferState(
			ImplType* impl,
			MetalBuffer* buffer,
			RHI::ResourceState first,
			RHI::ResourceState second = RHI::ResourceState::Undefined)
		{
			const auto found = impl->bufferStates.find(buffer);
			if(found != impl->bufferStates.end() &&
				found->second != first &&
				(second == RHI::ResourceState::Undefined || found->second != second))
			{
				return false;
			}
			MetalOperation operation = {};
			operation.kind = OperationKind::BufferRequirement;
			operation.buffer = buffer;
			operation.before = first;
			operation.after = second;
			impl->operations.push_back(operation);
			return true;
		}

		template<typename ImplType>
		[[nodiscard]] bool RequireTextureState(
			ImplType* impl,
			MetalTexture* texture,
			uint32_t mipLevel,
			uint32_t arrayLayer,
			RHI::ResourceState first,
			RHI::ResourceState second = RHI::ResourceState::Undefined)
		{
			const auto key = TextureKey(texture, mipLevel, arrayLayer);
			const auto found = impl->textureStates.find(key);
			if(found != impl->textureStates.end() &&
				found->second != first &&
				(second == RHI::ResourceState::Undefined || found->second != second))
			{
				return false;
			}
			MetalOperation operation = {};
			operation.kind = OperationKind::TextureRequirement;
			operation.texture = texture;
			operation.mipLevel = mipLevel;
			operation.arrayLayer = arrayLayer;
			operation.before = first;
			operation.after = second;
			impl->operations.push_back(operation);
			return true;
		}

		template<typename ImplType>
		void TrackSwapchainImage(ImplType* impl, MetalTexture* texture)
		{
			if(texture == nullptr || !texture->IsSwapchainImage()) return;
			impl->usesBackBuffer = true;
			if(std::find(impl->referencedSwapchainImages.begin(),
				impl->referencedSwapchainImages.end(), texture) ==
				impl->referencedSwapchainImages.end())
			{
				impl->referencedSwapchainImages.push_back(texture);
			}
		}

		template<typename ImplType>
		[[nodiscard]] bool EnsureBlitEncoder(ImplType* impl)
		{
			if(impl->rendering || impl->commandBuffer == nil) return false;
			if(impl->blitEncoder != nil) return true;
			id<MTLBlitCommandEncoder> encoder = [impl->commandBuffer blitCommandEncoder];
			if(encoder == nil) return false;
#if !__has_feature(objc_arc)
			[encoder retain];
#endif
			impl->blitEncoder = encoder;
			return true;
		}

		template<typename ImplType>
		void RetainResource(ImplType* impl, id<MTLResource> resource)
		{
			if(resource != nil) [impl->retainedResources addObject:resource];
		}

		template<typename ImplType>
		[[nodiscard]] bool RequireTextureSubresourcesInState(
			ImplType* impl,
			MetalTexture* texture,
			uint32_t firstMipLevel,
			uint32_t mipLevelCount,
			uint32_t firstArrayLayer,
			uint32_t arrayLayerCount,
			RHI::ResourceState state)
		{
			for(uint32_t layer = firstArrayLayer;
				layer < firstArrayLayer + arrayLayerCount; ++layer)
			{
				for(uint32_t mip = firstMipLevel;
					mip < firstMipLevel + mipLevelCount; ++mip)
				{
					if(!RequireTextureState(
						impl, texture, mip, layer, state)) return false;
				}
			}
			return true;
		}
	}

	MetalCommandList::MetalCommandList(void* commandQueue)
		: m_impl(new Impl())
	{
		m_impl->commandQueue = (__bridge id<MTLCommandQueue>)commandQueue;
	}

	MetalCommandList::~MetalCommandList()
	{
		Reset();
		delete m_impl;
	}

	bool MetalCommandList::Begin()
	{
		Reset();
		id<MTLCommandBuffer> commandBuffer = [m_impl->commandQueue commandBuffer];
		if(commandBuffer == nil) return false;
#if !__has_feature(objc_arc)
		[commandBuffer retain];
#endif
		m_impl->commandBuffer = commandBuffer;
		m_impl->retainedResources = [NSMutableArray new];
		m_impl->valid = true;
		return true;
	}

	void MetalCommandList::Reset()
	{
		if(m_impl == nullptr) return;
		EndRenderEncoding(m_impl);
		EndBlitEncoding(m_impl);
#if !__has_feature(objc_arc)
		[m_impl->retainedResources release];
		[m_impl->commandBuffer release];
#endif
		m_impl->retainedResources = nil;
		m_impl->commandBuffer = nil;
		m_impl->operations.clear();
		m_impl->bufferStates.clear();
		m_impl->textureStates.clear();
		m_impl->vertexBindings.clear();
		m_impl->referencedSwapchainImages.clear();
		m_impl->colorFormats.clear();
		m_impl->inlineConstants.clear();
		m_impl->inlineConstantCoverage.clear();
		m_impl->pipeline = nullptr;
		m_impl->resourceSet = nullptr;
		m_impl->indexBuffer = nullptr;
		m_impl->depthTexture = nullptr;
		m_impl->indexFormat = RHI::Format::Unknown;
		m_impl->depthFormat = RHI::Format::Unknown;
		m_impl->depthMipLevel = 0;
		m_impl->depthArrayLayer = 0;
		m_impl->indexOffset = 0;
		m_impl->renderWidth = 0;
		m_impl->renderHeight = 0;
		m_impl->rendering = false;
		m_impl->stencilConfigured = false;
		m_impl->viewportSet = false;
		m_impl->scissorSet = false;
		m_impl->stencilReferenceSet = false;
		m_impl->closed = false;
		m_impl->usesBackBuffer = false;
	}

	void MetalCommandList::ResourceBarrier(
		const RHI::ResourceBarrierDesc* barriers,
		uint32_t count)
	{
		if(m_impl->closed || m_impl->rendering || (count != 0 && barriers == nullptr))
		{
			Invalidate(m_impl);
			return;
		}
		if(count != 0) EndBlitEncoding(m_impl);

		for(uint32_t index = 0; index < count; ++index)
		{
			const RHI::ResourceBarrierDesc& barrier = barriers[index];
			if((barrier.buffer == nullptr) == (barrier.texture == nullptr) ||
				barrier.after == RHI::ResourceState::Undefined ||
				(barrier.before == barrier.after &&
					barrier.before != RHI::ResourceState::UnorderedAccess))
			{
				Invalidate(m_impl);
				continue;
			}

			if(barrier.buffer != nullptr)
			{
				auto* buffer = dynamic_cast<MetalBuffer*>(barrier.buffer);
				if(buffer == nullptr || NativeBuffer(buffer) == nil ||
					!IsBufferStateAllowed(buffer->GetDesc(), barrier.after) ||
					(m_impl->bufferStates.find(buffer) != m_impl->bufferStates.end() &&
						m_impl->bufferStates[buffer] != barrier.before))
				{
					Invalidate(m_impl);
					continue;
				}
				m_impl->operations.push_back({
					OperationKind::BufferBarrier,
					buffer,
					nullptr,
					barrier.before,
					barrier.after});
				m_impl->bufferStates[buffer] = barrier.after;
				continue;
			}

			auto* texture = dynamic_cast<MetalTexture*>(barrier.texture);
			uint32_t firstMip = 0;
			uint32_t mipCount = 0;
			uint32_t firstLayer = 0;
			uint32_t layerCount = 0;
			if(texture == nullptr || NativeTexture(texture) == nil ||
				!IsTextureStateAllowed(texture->GetDesc(), barrier.after) ||
				((barrier.before == RHI::ResourceState::Present ||
					barrier.after == RHI::ResourceState::Present) &&
					!texture->IsSwapchainImage()) ||
				!ResolveSubresources(
					texture, barrier.subresources,
					firstMip, mipCount, firstLayer, layerCount))
			{
				Invalidate(m_impl);
				continue;
			}
			TrackSwapchainImage(m_impl, texture);

			for(uint32_t layer = firstLayer; layer < firstLayer + layerCount; ++layer)
			{
				for(uint32_t mip = firstMip; mip < firstMip + mipCount; ++mip)
				{
					const auto key = TextureKey(texture, mip, layer);
					const auto prior = m_impl->textureStates.find(key);
					if(prior != m_impl->textureStates.end() && prior->second != barrier.before)
					{
						Invalidate(m_impl);
						continue;
					}
					MetalOperation operation = {};
					operation.kind = OperationKind::TextureBarrier;
					operation.texture = texture;
					operation.before = barrier.before;
					operation.after = barrier.after;
					operation.mipLevel = mip;
					operation.arrayLayer = layer;
					m_impl->operations.push_back(operation);
					m_impl->textureStates[key] = barrier.after;
				}
			}
		}
	}

	void MetalCommandList::BeginRendering(const RHI::RenderingDesc& desc)
	{
		if(m_impl->closed || m_impl->rendering ||
			(desc.colorAttachmentCount != 0 && desc.colorAttachments == nullptr) ||
			(desc.colorAttachmentCount == 0 && desc.depthStencilAttachment == nullptr) ||
			desc.colorAttachmentCount > 8)
		{
			Invalidate(m_impl);
			return;
		}
		EndBlitEncoding(m_impl);
		m_impl->pipeline = nullptr;
		m_impl->resourceSet = nullptr;
		m_impl->vertexBindings.clear();
		m_impl->indexBuffer = nullptr;
		m_impl->indexFormat = RHI::Format::Unknown;
		m_impl->indexOffset = 0;
		m_impl->inlineConstants.clear();
		m_impl->inlineConstantCoverage.clear();
		m_impl->viewportSet = false;
		m_impl->scissorSet = false;
		m_impl->stencilReferenceSet = false;

		MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor new];
		m_impl->colorFormats.clear();
		m_impl->renderWidth = 0;
		m_impl->renderHeight = 0;
		bool valid = true;
		for(uint32_t index = 0; index < desc.colorAttachmentCount; ++index)
		{
			const RHI::ColorAttachment& attachment = desc.colorAttachments[index];
			auto* texture = dynamic_cast<MetalTexture*>(attachment.texture);
			id<MTLTexture> native = NativeTexture(texture);
			bool clearValueValid = true;
			if(attachment.loadOp == RHI::LoadOp::Clear)
			{
				for(float component : attachment.clearColor)
					clearValueValid = clearValueValid && std::isfinite(component);
			}
			if(texture == nullptr || native == nil ||
				!HasUsage(texture->GetDesc().usage, RHI::TextureUsage::RenderTarget) ||
				attachment.mipLevel >= texture->GetDesc().mipLevels ||
				attachment.arrayLayer >= texture->GetDesc().depthOrArraySize ||
				!RequireTextureState(
					m_impl, texture, attachment.mipLevel, attachment.arrayLayer,
					RHI::ResourceState::RenderTarget) ||
				attachment.loadOp == RHI::LoadOp::Undefined || !clearValueValid ||
				attachment.storeOp == RHI::StoreOp::Undefined)
			{
				valid = false;
				break;
			}
			const uint32_t attachmentWidth = std::max(
				1u, static_cast<uint32_t>(native.width) >> attachment.mipLevel);
			const uint32_t attachmentHeight = std::max(
				1u, static_cast<uint32_t>(native.height) >> attachment.mipLevel);
			if(m_impl->renderWidth == 0)
			{
				m_impl->renderWidth = attachmentWidth;
				m_impl->renderHeight = attachmentHeight;
			}
			else if(m_impl->renderWidth != attachmentWidth ||
				m_impl->renderHeight != attachmentHeight)
			{
				valid = false;
				break;
			}
			MTLRenderPassColorAttachmentDescriptor* target = pass.colorAttachments[index];
			target.texture = native;
			target.level = attachment.mipLevel;
			target.slice = attachment.arrayLayer;
			target.loadAction = ToLoadAction(attachment.loadOp);
			target.storeAction = ToStoreAction(attachment.storeOp);
			target.clearColor = MTLClearColorMake(
				attachment.clearColor[0], attachment.clearColor[1],
				attachment.clearColor[2], attachment.clearColor[3]);
			m_impl->colorFormats.push_back(texture->GetDesc().format);
			TrackSwapchainImage(m_impl, texture);
		}

		m_impl->depthFormat = RHI::Format::Unknown;
		m_impl->depthTexture = nullptr;
		m_impl->depthMipLevel = 0;
		m_impl->depthArrayLayer = 0;
		m_impl->stencilConfigured = false;
		if(valid && desc.depthStencilAttachment != nullptr)
		{
			const RHI::DepthStencilAttachment& attachment = *desc.depthStencilAttachment;
			auto* texture = dynamic_cast<MetalTexture*>(attachment.texture);
			id<MTLTexture> native = NativeTexture(texture);
			const bool subresourceValid = texture != nullptr &&
				attachment.mipLevel < texture->GetDesc().mipLevels &&
				attachment.arrayLayer < texture->GetDesc().depthOrArraySize;
			const bool hasStencil = texture != nullptr &&
				texture->GetDesc().format == RHI::Format::D24_UNORM_S8_UINT;
			const bool declaredStateValid =
				attachment.state == RHI::ResourceState::DepthRead ||
				attachment.state == RHI::ResourceState::DepthWrite;
			const bool requiresWrite = attachment.depthLoadOp == RHI::LoadOp::Clear ||
				(hasStencil && attachment.stencilLoadOp == RHI::LoadOp::Clear);
			const bool stateValid = subresourceValid && declaredStateValid &&
				(!requiresWrite || attachment.state == RHI::ResourceState::DepthWrite) &&
				RequireTextureState(
					m_impl, texture, attachment.mipLevel, attachment.arrayLayer,
					attachment.state);
			const bool depthClearValid = attachment.depthLoadOp != RHI::LoadOp::Clear ||
				(std::isfinite(attachment.clearDepth) && attachment.clearDepth >= 0.0f &&
					attachment.clearDepth <= 1.0f);
			const bool stencilClearValid = attachment.stencilLoadOp != RHI::LoadOp::Clear ||
				attachment.clearStencil <= std::numeric_limits<uint8_t>::max();
			if(texture == nullptr || native == nil ||
				!HasUsage(texture->GetDesc().usage, RHI::TextureUsage::DepthStencil) ||
				!subresourceValid || !stateValid || !depthClearValid || !stencilClearValid ||
				attachment.depthLoadOp == RHI::LoadOp::Undefined ||
				attachment.depthStoreOp == RHI::StoreOp::Undefined ||
				(!hasStencil &&
					(attachment.stencilLoadOp != RHI::LoadOp::Undefined ||
						attachment.stencilStoreOp != RHI::StoreOp::Undefined)))
			{
				valid = false;
			}
			else
			{
				const uint32_t attachmentWidth = std::max(
					1u, static_cast<uint32_t>(native.width) >> attachment.mipLevel);
				const uint32_t attachmentHeight = std::max(
					1u, static_cast<uint32_t>(native.height) >> attachment.mipLevel);
				if(m_impl->renderWidth == 0)
				{
					m_impl->renderWidth = attachmentWidth;
					m_impl->renderHeight = attachmentHeight;
				}
				else if(m_impl->renderWidth != attachmentWidth ||
					m_impl->renderHeight != attachmentHeight)
				{
					valid = false;
				}
				pass.depthAttachment.texture = native;
				pass.depthAttachment.level = attachment.mipLevel;
				pass.depthAttachment.slice = attachment.arrayLayer;
				pass.depthAttachment.loadAction = ToLoadAction(attachment.depthLoadOp);
				pass.depthAttachment.storeAction = ToStoreAction(attachment.depthStoreOp);
				pass.depthAttachment.clearDepth = attachment.clearDepth;
				if(hasStencil)
				{
					const bool hasStencilLoad =
						attachment.stencilLoadOp != RHI::LoadOp::Undefined;
					const bool hasStencilStore =
						attachment.stencilStoreOp != RHI::StoreOp::Undefined;
					if(hasStencilLoad != hasStencilStore)
					{
						valid = false;
					}
					pass.stencilAttachment.texture = native;
					pass.stencilAttachment.level = attachment.mipLevel;
					pass.stencilAttachment.slice = attachment.arrayLayer;
					pass.stencilAttachment.loadAction = ToLoadAction(attachment.stencilLoadOp);
					pass.stencilAttachment.storeAction = ToStoreAction(attachment.stencilStoreOp);
					pass.stencilAttachment.clearStencil = attachment.clearStencil;
					m_impl->stencilConfigured = hasStencilLoad;
				}
				m_impl->depthFormat = texture->GetDesc().format;
				m_impl->depthTexture = texture;
				m_impl->depthMipLevel = attachment.mipLevel;
				m_impl->depthArrayLayer = attachment.arrayLayer;
			}
		}

		id<MTLRenderCommandEncoder> encoder = valid
			? [m_impl->commandBuffer renderCommandEncoderWithDescriptor:pass] : nil;
#if !__has_feature(objc_arc)
		[pass release];
#endif
		if(encoder == nil)
		{
			Invalidate(m_impl);
			return;
		}
#if !__has_feature(objc_arc)
		[encoder retain];
#endif
		m_impl->renderEncoder = encoder;
		m_impl->rendering = true;
	}

	void MetalCommandList::EndRendering()
	{
		if(m_impl->closed || !m_impl->rendering)
		{
			Invalidate(m_impl);
			return;
		}
		EndRenderEncoding(m_impl);
		m_impl->rendering = false;
	}

	void MetalCommandList::BindGraphicsPipeline(RHI::PipelineHandle pipelineState)
	{
		auto* pipeline = dynamic_cast<MetalPipeline*>(pipelineState);
		if(m_impl->closed || !m_impl->rendering || pipeline == nullptr ||
			pipeline->GetNativePipeline() == nullptr ||
			(pipeline->GetDesc().depthStencil.format != RHI::Format::Unknown &&
				pipeline->GetNativeDepthStencil() == nullptr))
		{
			Invalidate(m_impl);
			return;
		}
		m_impl->pipeline = pipeline;
		m_impl->resourceSet = nullptr;
		m_impl->inlineConstants.assign(pipeline->GetLayout().inlineConstantSize, 0);
		m_impl->inlineConstantCoverage.assign(
			pipeline->GetLayout().inlineConstantSize, 0);
		if((pipeline->GetDesc().depthStencil.depthWriteEnabled ||
			pipeline->GetDesc().depthStencil.stencilEnabled) &&
			m_impl->depthTexture != nullptr &&
			!RequireTextureState(
				m_impl,
				m_impl->depthTexture,
				m_impl->depthMipLevel,
				m_impl->depthArrayLayer,
				RHI::ResourceState::DepthWrite))
		{
			Invalidate(m_impl);
			return;
		}

		id<MTLRenderCommandEncoder> encoder = m_impl->renderEncoder;
		[encoder setRenderPipelineState:
			(__bridge id<MTLRenderPipelineState>)pipeline->GetNativePipeline()];
		id<MTLDepthStencilState> depth =
			(__bridge id<MTLDepthStencilState>)pipeline->GetNativeDepthStencil();
		[encoder setDepthStencilState:depth];
		[encoder setCullMode:static_cast<MTLCullMode>(pipeline->GetNativeCullMode())];
		[encoder setFrontFacingWinding:static_cast<MTLWinding>(pipeline->GetNativeFrontFace())];
		[encoder setTriangleFillMode:static_cast<MTLTriangleFillMode>(pipeline->GetNativeFillMode())];
		const RHI::RasterState& raster = pipeline->GetDesc().raster;
		[encoder setDepthBias:raster.depthBiasConstant
			slopeScale:raster.depthBiasSlope
			clamp:raster.depthBiasClamp];
		for(const MetalStaticSamplerBinding& binding :
			pipeline->GetStaticSamplerBindings())
		{
			id<MTLSamplerState> sampler =
				(__bridge id<MTLSamplerState>)binding.sampler;
			if(HasStage(binding.stages, RHI::ShaderStageFlags::Vertex))
				[encoder setVertexSamplerState:sampler atIndex:binding.index];
			if(HasStage(binding.stages, RHI::ShaderStageFlags::Fragment))
				[encoder setFragmentSamplerState:sampler atIndex:binding.index];
		}
	}

	void MetalCommandList::BindResourceSet(RHI::ResourceSetHandle resourceSet)
	{
		auto* set = dynamic_cast<MetalResourceSet*>(resourceSet);
		if(m_impl->closed || !m_impl->rendering || m_impl->pipeline == nullptr ||
			set == nullptr || set->GetPipeline() != m_impl->pipeline)
		{
			Invalidate(m_impl);
			return;
		}

		const RHI::PipelineLayoutDesc& layout = m_impl->pipeline->GetLayout();
		id<MTLRenderCommandEncoder> encoder = m_impl->renderEncoder;
		for(uint32_t index = 0; index < set->GetBindingCount(); ++index)
		{
			const RHI::ResourceBinding& binding = set->GetBindings()[index];
			const RHI::ResourceBindingLayout* declaration =
				FindLayoutBinding(layout, binding.binding);
			if(declaration == nullptr)
			{
				Invalidate(m_impl);
				return;
			}
			const uint32_t nativeIndex = binding.binding + binding.arrayElement;
			if(declaration->type == RHI::ResourceBindingType::ConstantBuffer ||
				declaration->type == RHI::ResourceBindingType::ReadOnlyStorageBuffer ||
				declaration->type == RHI::ResourceBindingType::ReadWriteStorageBuffer)
			{
				auto* buffer = dynamic_cast<MetalBuffer*>(binding.buffer);
				const RHI::ResourceState requiredState =
					declaration->type == RHI::ResourceBindingType::ConstantBuffer
					? RHI::ResourceState::ConstantBuffer
					: declaration->type == RHI::ResourceBindingType::ReadOnlyStorageBuffer
						? RHI::ResourceState::ShaderResource
						: RHI::ResourceState::UnorderedAccess;
				if(buffer == nullptr || !RequireBufferState(
					m_impl, buffer, requiredState))
				{
					Invalidate(m_impl);
					return;
				}
				id<MTLBuffer> native = NativeBuffer(buffer);
				if(HasStage(declaration->stages, RHI::ShaderStageFlags::Vertex))
					[encoder setVertexBuffer:native offset:binding.offset atIndex:nativeIndex];
				if(HasStage(declaration->stages, RHI::ShaderStageFlags::Fragment))
					[encoder setFragmentBuffer:native offset:binding.offset atIndex:nativeIndex];
				const MTLResourceUsage usage =
					declaration->type == RHI::ResourceBindingType::ReadWriteStorageBuffer
					? (MTLResourceUsageRead | MTLResourceUsageWrite) : MTLResourceUsageRead;
				[encoder useResource:native usage:usage];
			}
		}

		for(const MetalTextureBinding& binding : set->GetTextureBindings())
		{
			if(binding.texture == nullptr || binding.nativeTexture == nullptr ||
				!RequireTextureSubresourcesInState(
					m_impl,
					binding.texture,
					binding.firstMipLevel,
					binding.mipLevelCount,
					binding.firstArrayLayer,
					binding.arrayLayerCount,
					binding.requiredState))
			{
				Invalidate(m_impl);
				return;
			}
			id<MTLTexture> native = (__bridge id<MTLTexture>)binding.nativeTexture;
			if(HasStage(binding.stages, RHI::ShaderStageFlags::Vertex))
				[encoder setVertexTexture:native atIndex:binding.index];
			if(HasStage(binding.stages, RHI::ShaderStageFlags::Fragment))
				[encoder setFragmentTexture:native atIndex:binding.index];
			const MTLResourceUsage usage =
				binding.requiredState == RHI::ResourceState::UnorderedAccess
				? (MTLResourceUsageRead | MTLResourceUsageWrite) : MTLResourceUsageRead;
			[encoder useResource:native usage:usage];
		}

		m_impl->resourceSet = set;
	}

	void MetalCommandList::BindVertexBuffer(
		uint32_t binding,
		RHI::BufferHandle buffer,
		uint32_t offset)
	{
		auto* metalBuffer = dynamic_cast<MetalBuffer*>(buffer);
		const RHI::GraphicsPipelineDesc* pipelineDesc = m_impl->pipeline == nullptr
			? nullptr : &m_impl->pipeline->GetDesc();
		const RHI::VertexBufferLayout* layout = nullptr;
		if(pipelineDesc != nullptr)
		{
			for(uint32_t index = 0; index < pipelineDesc->vertexBufferCount; ++index)
			{
				if(pipelineDesc->vertexBuffers[index].binding == binding)
				{
					layout = &pipelineDesc->vertexBuffers[index];
					break;
				}
			}
		}
		if(m_impl->closed || !m_impl->rendering || pipelineDesc == nullptr ||
			layout == nullptr || metalBuffer == nullptr ||
			offset >= metalBuffer->GetDesc().size ||
			!HasUsage(metalBuffer->GetDesc().usage, RHI::BufferUsage::Vertex) ||
			!RequireBufferState(
				m_impl, metalBuffer, RHI::ResourceState::VertexBuffer))
		{
			Invalidate(m_impl);
			return;
		}
		m_impl->vertexBindings[binding] = {metalBuffer, offset};
		id<MTLBuffer> native = NativeBuffer(metalBuffer);
		[m_impl->renderEncoder setVertexBuffer:native offset:offset atIndex:binding];
		[m_impl->renderEncoder useResource:native usage:MTLResourceUsageRead];
	}

	void MetalCommandList::BindIndexBuffer(
		RHI::BufferHandle buffer,
		RHI::Format format,
		uint32_t offset)
	{
		auto* metalBuffer = dynamic_cast<MetalBuffer*>(buffer);
		const uint32_t indexSize = FormatSize(format);
		if(m_impl->closed || !m_impl->rendering || metalBuffer == nullptr ||
			offset >= metalBuffer->GetDesc().size ||
			!HasUsage(metalBuffer->GetDesc().usage, RHI::BufferUsage::Index) ||
			!RequireBufferState(
				m_impl, metalBuffer, RHI::ResourceState::IndexBuffer) ||
			(format != RHI::Format::R16_UINT && format != RHI::Format::R32_UINT) ||
			indexSize == 0 || offset % indexSize != 0 ||
			metalBuffer->GetDesc().size - offset < indexSize)
		{
			Invalidate(m_impl);
			return;
		}
		m_impl->indexBuffer = metalBuffer;
		m_impl->indexFormat = format;
		m_impl->indexOffset = offset;
		[m_impl->renderEncoder useResource:NativeBuffer(metalBuffer) usage:MTLResourceUsageRead];
	}

	void MetalCommandList::SetInlineConstants(
		uint32_t offset,
		uint32_t size,
		const void* data)
	{
		if(m_impl->closed || !m_impl->rendering || m_impl->pipeline == nullptr ||
			data == nullptr || size == 0 ||
			(offset % sizeof(uint32_t)) != 0 || (size % sizeof(uint32_t)) != 0 ||
			offset > m_impl->inlineConstants.size() ||
			size > m_impl->inlineConstants.size() - offset)
		{
			Invalidate(m_impl);
			return;
		}
		std::memcpy(m_impl->inlineConstants.data() + offset, data, size);
		std::fill(
			m_impl->inlineConstantCoverage.begin() + offset,
			m_impl->inlineConstantCoverage.begin() + offset + size,
			1);
		id<MTLDevice> device = m_impl->commandQueue.device;
		id<MTLBuffer> constants = [device newBufferWithBytes:m_impl->inlineConstants.data()
			length:m_impl->inlineConstants.size()
			options:MTLResourceStorageModeShared];
		if(constants == nil)
		{
			Invalidate(m_impl);
			return;
		}
		RetainResource(m_impl, constants);
		const RHI::PipelineLayoutDesc& layout = m_impl->pipeline->GetLayout();
		if(HasStage(layout.inlineConstantStages, RHI::ShaderStageFlags::Vertex))
			[m_impl->renderEncoder setVertexBuffer:constants
				offset:0 atIndex:layout.inlineConstantBinding];
		if(HasStage(layout.inlineConstantStages, RHI::ShaderStageFlags::Fragment))
			[m_impl->renderEncoder setFragmentBuffer:constants
				offset:0 atIndex:layout.inlineConstantBinding];
		[m_impl->renderEncoder useResource:constants usage:MTLResourceUsageRead];
#if !__has_feature(objc_arc)
		[constants release];
#endif
	}

	void MetalCommandList::SetViewport(const RHI::Viewport& viewport)
	{
		if(m_impl->closed || !m_impl->rendering ||
			!std::isfinite(viewport.x) || !std::isfinite(viewport.y) ||
			!std::isfinite(viewport.width) || !std::isfinite(viewport.height) ||
			!std::isfinite(viewport.minDepth) || !std::isfinite(viewport.maxDepth) ||
			viewport.width <= 0.0f ||
			viewport.height <= 0.0f || viewport.minDepth < 0.0f ||
			viewport.maxDepth > 1.0f || viewport.minDepth > viewport.maxDepth)
		{
			Invalidate(m_impl);
			return;
		}
		MTLViewport native = {
			viewport.x, viewport.y, viewport.width, viewport.height,
			viewport.minDepth, viewport.maxDepth};
		[m_impl->renderEncoder setViewport:native];
		m_impl->viewportSet = true;
	}

	void MetalCommandList::SetScissor(const RHI::Rect& rect)
	{
		if(m_impl->closed || !m_impl->rendering || rect.x < 0 || rect.y < 0 ||
			rect.width == 0 || rect.height == 0 ||
			static_cast<uint32_t>(rect.x) > m_impl->renderWidth ||
			rect.width > m_impl->renderWidth - static_cast<uint32_t>(rect.x) ||
			static_cast<uint32_t>(rect.y) > m_impl->renderHeight ||
			rect.height > m_impl->renderHeight - static_cast<uint32_t>(rect.y))
		{
			Invalidate(m_impl);
			return;
		}
		MTLScissorRect native = {
			static_cast<NSUInteger>(rect.x), static_cast<NSUInteger>(rect.y),
			rect.width, rect.height};
		[m_impl->renderEncoder setScissorRect:native];
		m_impl->scissorSet = true;
	}

	void MetalCommandList::SetStencilReference(uint32_t reference)
	{
		if(m_impl->closed || !m_impl->rendering ||
			reference > std::numeric_limits<uint8_t>::max())
		{
			Invalidate(m_impl);
			return;
		}
		[m_impl->renderEncoder setStencilReferenceValue:reference];
		m_impl->stencilReferenceSet = true;
	}

	namespace
	{
		template<typename ImplType>
		[[nodiscard]] bool ValidateDrawState(const ImplType* impl)
		{
			if(!impl->rendering || impl->pipeline == nullptr ||
				!impl->viewportSet || !impl->scissorSet)
			{
				return false;
			}
			const RHI::GraphicsPipelineDesc& desc = impl->pipeline->GetDesc();
			if(desc.colorAttachmentCount != impl->colorFormats.size() ||
				desc.depthStencil.format != impl->depthFormat)
			{
				return false;
			}
			if(desc.depthStencil.stencilEnabled &&
				(!impl->stencilConfigured || !impl->stencilReferenceSet))
			{
				return false;
			}
			for(uint32_t index = 0; index < desc.colorAttachmentCount; ++index)
				if(desc.colorAttachments[index].format != impl->colorFormats[index]) return false;
			for(uint32_t index = 0; index < desc.vertexBufferCount; ++index)
				if(impl->vertexBindings.find(desc.vertexBuffers[index].binding) ==
					impl->vertexBindings.end()) return false;
			bool resourceSetRequired = false;
			for(uint32_t index = 0; index < desc.layout.bindingCount; ++index)
			{
				if(desc.layout.bindings[index].type !=
					RHI::ResourceBindingType::StaticSampler)
				{
					resourceSetRequired = true;
					break;
				}
			}
			if(resourceSetRequired && impl->resourceSet == nullptr) return false;
			if(desc.layout.inlineConstantSize != 0 &&
				(impl->inlineConstantCoverage.size() != desc.layout.inlineConstantSize ||
					std::find(impl->inlineConstantCoverage.begin(),
						impl->inlineConstantCoverage.end(), 0) !=
						impl->inlineConstantCoverage.end()))
			{
				return false;
			}
			return true;
		}
	}

	void MetalCommandList::DrawInstanced(
		uint32_t vertexCount,
		uint32_t instanceCount,
		uint32_t startVertex,
		uint32_t startInstance)
	{
		if(m_impl->closed || vertexCount == 0 || instanceCount == 0 ||
			!ValidateDrawState(m_impl))
		{
			Invalidate(m_impl);
			return;
		}
		[m_impl->renderEncoder
			drawPrimitives:static_cast<MTLPrimitiveType>(m_impl->pipeline->GetNativePrimitiveType())
			vertexStart:startVertex
			vertexCount:vertexCount
			instanceCount:instanceCount
			baseInstance:startInstance];
	}

	void MetalCommandList::DrawIndexedInstanced(
		uint32_t indexCount,
		uint32_t instanceCount,
		uint32_t firstIndex,
		int32_t vertexOffset,
		uint32_t firstInstance)
	{
		const uint32_t indexSize = FormatSize(m_impl->indexFormat);
		const uint64_t nativeOffset = static_cast<uint64_t>(m_impl->indexOffset) +
			static_cast<uint64_t>(firstIndex) * indexSize;
		const uint64_t end = nativeOffset + static_cast<uint64_t>(indexCount) * indexSize;
		if(m_impl->closed || indexCount == 0 || instanceCount == 0 ||
			!ValidateDrawState(m_impl) || m_impl->indexBuffer == nullptr ||
			end > m_impl->indexBuffer->GetDesc().size)
		{
			Invalidate(m_impl);
			return;
		}
		const MTLIndexType indexType = m_impl->indexFormat == RHI::Format::R16_UINT
			? MTLIndexTypeUInt16 : MTLIndexTypeUInt32;
		[m_impl->renderEncoder
			drawIndexedPrimitives:static_cast<MTLPrimitiveType>(m_impl->pipeline->GetNativePrimitiveType())
			indexCount:indexCount
			indexType:indexType
			indexBuffer:NativeBuffer(m_impl->indexBuffer)
			indexBufferOffset:static_cast<NSUInteger>(nativeOffset)
			instanceCount:instanceCount
			baseVertex:vertexOffset
			baseInstance:firstInstance];
	}

	void MetalCommandList::Close()
	{
		if(m_impl->closed) Invalidate(m_impl);
		if(m_impl->rendering)
		{
			Invalidate(m_impl);
			EndRenderEncoding(m_impl);
			m_impl->rendering = false;
		}
		EndBlitEncoding(m_impl);
		m_impl->closed = true;
	}

	bool MetalCommandList::RecordBufferUpdate(
		MetalBuffer* buffer,
		uint32_t offset,
		const void* data,
		uint32_t size)
	{
		if(m_impl->closed || m_impl->rendering || buffer == nullptr ||
			NativeBuffer(buffer) == nil || data == nullptr ||
			size == 0 || offset > buffer->GetDesc().size || size > buffer->GetDesc().size - offset ||
			!EnsureBlitEncoder(m_impl))
		{
			return false;
		}

		id<MTLBuffer> staging = [m_impl->commandQueue.device newBufferWithBytes:data
			length:size options:MTLResourceStorageModeShared];
		if(staging == nil) return false;
		RetainResource(m_impl, staging);
		[m_impl->blitEncoder copyFromBuffer:staging
			sourceOffset:0
			toBuffer:NativeBuffer(buffer)
			destinationOffset:offset
			size:size];
#if !__has_feature(objc_arc)
		[staging release];
#endif
		MetalOperation operation = {};
		operation.kind = OperationKind::BufferWrite;
		operation.buffer = buffer;
		m_impl->operations.push_back(operation);
		return true;
	}

	bool MetalCommandList::RecordTextureUpdate(
		MetalTexture* texture,
		uint32_t mipLevel,
		uint32_t arrayLayer,
		const void* data,
		uint32_t dataSize,
		uint32_t rowPitch,
		uint32_t slicePitch)
	{
		if(m_impl->closed || m_impl->rendering || texture == nullptr ||
			NativeTexture(texture) == nil || data == nullptr ||
			mipLevel >= texture->GetDesc().mipLevels ||
			arrayLayer >= texture->GetDesc().depthOrArraySize)
		{
			return false;
		}
		const uint32_t pixelSize = FormatSize(texture->GetDesc().format);
		const uint32_t mipWidth = std::max(1u, texture->GetDesc().width >> mipLevel);
		const uint32_t mipHeight = std::max(1u, texture->GetDesc().height >> mipLevel);
		if(pixelSize == 0 || mipWidth > std::numeric_limits<uint32_t>::max() / pixelSize)
			return false;
		const uint32_t tightRowPitch = mipWidth * pixelSize;
		if(rowPitch < tightRowPitch || mipHeight > std::numeric_limits<uint32_t>::max() / rowPitch ||
			slicePitch < rowPitch * mipHeight || dataSize < slicePitch)
		{
			return false;
		}

		if(texture->GetDesc().format == RHI::Format::D24_UNORM_S8_UINT) return false;

		id<MTLTexture> nativeTexture = NativeTexture(texture);
		id<MTLDevice> device = m_impl->commandQueue.device;
		const uint64_t stagingSize64 = static_cast<uint64_t>(tightRowPitch) * mipHeight;
		if(stagingSize64 > std::numeric_limits<NSUInteger>::max())
		{
			return false;
		}
		const NSUInteger stagingSize = static_cast<NSUInteger>(stagingSize64);
		id<MTLBuffer> staging = [device newBufferWithLength:stagingSize
			options:MTLResourceStorageModeShared];
		if(staging == nil) return false;
		auto* destination = static_cast<uint8_t*>(staging.contents);
		const auto* source = static_cast<const uint8_t*>(data);
		for(uint32_t row = 0; row < mipHeight; ++row)
			std::memcpy(destination + static_cast<size_t>(row) * tightRowPitch,
				source + static_cast<size_t>(row) * rowPitch, tightRowPitch);

		if(!EnsureBlitEncoder(m_impl))
		{
#if !__has_feature(objc_arc)
			[staging release];
#endif
			return false;
		}
		RetainResource(m_impl, staging);
		[m_impl->blitEncoder
			copyFromBuffer:staging
			sourceOffset:0
			sourceBytesPerRow:tightRowPitch
			sourceBytesPerImage:0
			sourceSize:MTLSizeMake(mipWidth, mipHeight, 1)
			toTexture:nativeTexture
			destinationSlice:arrayLayer
			destinationLevel:mipLevel
			destinationOrigin:MTLOriginMake(0, 0, 0)];
#if !__has_feature(objc_arc)
		[staging release];
#endif
		MetalOperation operation = {};
		operation.kind = OperationKind::TextureWrite;
		operation.texture = texture;
		operation.mipLevel = mipLevel;
		operation.arrayLayer = arrayLayer;
		m_impl->operations.push_back(operation);
		TrackSwapchainImage(m_impl, texture);
		return true;
	}

	bool MetalCommandList::ValidateForSubmit(MetalSubmissionState& state) const
	{
		for(const MetalOperation& operation : m_impl->operations)
		{
			switch(operation.kind)
			{
			case OperationKind::BufferBarrier:
			{
				const auto found = state.buffers.find(operation.buffer);
				const RHI::ResourceState current = found == state.buffers.end()
					? operation.buffer->GetState() : found->second;
				if(current != operation.before) return false;
				state.buffers[operation.buffer] = operation.after;
				break;
			}
			case OperationKind::TextureBarrier:
			{
				const auto key = TextureKey(
					operation.texture, operation.mipLevel, operation.arrayLayer);
				const auto found = state.textureSubresources.find(key);
				const RHI::ResourceState current = found == state.textureSubresources.end()
					? operation.texture->GetState(operation.mipLevel, operation.arrayLayer)
					: found->second;
				if(current != operation.before) return false;
				state.textureSubresources[key] = operation.after;
				break;
			}
			case OperationKind::BufferRequirement:
			{
				const auto found = state.buffers.find(operation.buffer);
				const RHI::ResourceState current = found == state.buffers.end()
					? operation.buffer->GetState() : found->second;
				if(current != operation.before &&
					(operation.after == RHI::ResourceState::Undefined ||
						current != operation.after)) return false;
				break;
			}
			case OperationKind::TextureRequirement:
			{
				const auto key = TextureKey(
					operation.texture, operation.mipLevel, operation.arrayLayer);
				const auto found = state.textureSubresources.find(key);
				const RHI::ResourceState current = found == state.textureSubresources.end()
					? operation.texture->GetState(operation.mipLevel, operation.arrayLayer)
					: found->second;
				if(current != operation.before &&
					(operation.after == RHI::ResourceState::Undefined ||
						current != operation.after)) return false;
				break;
			}
			case OperationKind::BufferWrite:
			{
				const auto found = state.buffers.find(operation.buffer);
				const RHI::ResourceState current = found == state.buffers.end()
					? operation.buffer->GetState() : found->second;
				if(current != RHI::ResourceState::CopyDestination) return false;
				break;
			}
			case OperationKind::TextureWrite:
			{
				const auto key = TextureKey(
					operation.texture, operation.mipLevel, operation.arrayLayer);
				const auto found = state.textureSubresources.find(key);
				const RHI::ResourceState current = found == state.textureSubresources.end()
					? operation.texture->GetState(operation.mipLevel, operation.arrayLayer)
					: found->second;
				if(current != RHI::ResourceState::CopyDestination) return false;
				break;
			}
			}
		}
		return true;
	}

	void MetalCommandList::CommitResourceStates()
	{
		for(const MetalOperation& operation : m_impl->operations)
		{
			if(operation.kind == OperationKind::BufferBarrier)
				operation.buffer->SetState(operation.after);
			else if(operation.kind == OperationKind::TextureBarrier)
				operation.texture->SetState(
					operation.mipLevel, operation.arrayLayer, operation.after);
		}
	}

	bool MetalCommandList::IsClosed() const { return m_impl->closed; }
	bool MetalCommandList::IsValid() const { return m_impl->valid; }
	bool MetalCommandList::UsesBackBuffer() const { return m_impl->usesBackBuffer; }
	void* MetalCommandList::GetNativeCommandBuffer() const
	{
		return (__bridge void*)m_impl->commandBuffer;
	}
}
