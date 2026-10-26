#include "MetalCommandList.h"

#include "MetalBuffer.h"
#include "MetalPipeline.h"
#include "MetalResourceSet.h"
#include "MetalTexture.h"
#include "MetalTessellation.h"
#include "MetalUpload.h"
#include "MetalValidation.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <unordered_map>
#include <vector>

#import <Metal/Metal.h>

namespace dyf::Backends
{
	static_assert(sizeof(RHI::DrawIndirectArguments) == sizeof(MTLDrawPrimitivesIndirectArguments));
	static_assert(sizeof(RHI::DrawIndexedIndirectArguments) == sizeof(MTLDrawIndexedPrimitivesIndirectArguments));
	static_assert(sizeof(RHI::DispatchIndirectArguments) == sizeof(MTLDispatchThreadgroupsIndirectArguments));

	namespace
	{
		[[nodiscard]] bool HasStage(
			RHI::ShaderStageFlags stages,
			RHI::ShaderStageFlags stage)
		{
			return (stages & stage) != RHI::ShaderStageFlags::None;
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

		enum class OperationKind : uint8_t
		{
			BufferBarrier,
			TextureBarrier
		};

		struct MetalOperation
		{
			OperationKind kind = OperationKind::BufferBarrier;
			MetalBuffer* buffer = nullptr;
			MetalTexture* texture = nullptr;
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
		MTLRenderPassDescriptor* resumePass = nil;
		std::array<MTLStoreAction, 8> finalColorStoreActions = {};
		MTLStoreAction finalDepthStoreAction = MTLStoreActionDontCare;
		MTLStoreAction finalStencilStoreAction = MTLStoreActionDontCare;
		NSMutableArray<id<MTLResource>>* retainedResources = nil;
		std::shared_ptr<MetalUploadPool> uploadPool;
		std::shared_ptr<MetalUploadArena> uploadArena;
		std::shared_ptr<std::shared_ptr<MetalUploadArena>> uploadCompletionLease;

		std::vector<MetalOperation> operations;
		std::unordered_map<uint32_t, VertexBinding> vertexBindings;
		std::vector<RHI::Format> colorFormats;
		std::vector<uint8_t> inlineConstants;

		MetalPipeline* pipeline = nullptr;
		MetalResourceSet* resourceSet = nullptr;
		MetalBuffer* indexBuffer = nullptr;
		MetalTexture* depthTexture = nullptr;
		RHI::Format indexFormat = RHI::Format::Unknown;
		RHI::Format depthFormat = RHI::Format::Unknown;
		uint32_t indexOffset = 0;
		uint32_t renderWidth = 0;
		uint32_t renderHeight = 0;
		RHI::Viewport viewport = {};
		RHI::Rect scissor = {};
		uint32_t stencilReference = 0;
		bool rendering = false;
		bool closed = false;
		bool valid = true;
		bool usesBackBuffer = false;
	};

	namespace
	{
		[[nodiscard]] bool IsNativePending(id<MTLCommandBuffer> buffer)
		{
			if(buffer == nil) return false;
			const auto status = buffer.status;
			return status != MTLCommandBufferStatusNotEnqueued &&
				status != MTLCommandBufferStatusCompleted && status != MTLCommandBufferStatusError;
		}

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
		void EndRenderEncoding(ImplType* impl, bool preserveAttachments = false)
		{
			if(impl->renderEncoder == nil) return;
			// Every encoder starts with Unknown so intermediate splits may Store,
			// while final, close, reset, and discard paths honor the declared action.
			for(uint32_t index = 0; index < impl->colorFormats.size(); ++index)
				[impl->renderEncoder setColorStoreAction:preserveAttachments
					? MTLStoreActionStore : impl->finalColorStoreActions[index] atIndex:index];
			if(impl->depthTexture != nullptr)
				[impl->renderEncoder setDepthStoreAction:preserveAttachments
					? MTLStoreActionStore : impl->finalDepthStoreAction];
			if(RHI::HasStencil(impl->depthFormat))
				[impl->renderEncoder setStencilStoreAction:preserveAttachments
					? MTLStoreActionStore : impl->finalStencilStoreAction];
			[impl->renderEncoder endEncoding];
#if !__has_feature(objc_arc)
			[impl->renderEncoder release];
#endif
			impl->renderEncoder = nil;
		}

		template<typename ImplType>
		void TrackSwapchainImage(ImplType* impl, MetalTexture* texture)
		{
			if(texture == nullptr || !texture->IsSwapchainImage()) return;
			impl->usesBackBuffer = true;
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
			if(resource == nil) return;
			if(impl->retainedResources == nil) impl->retainedResources = [NSMutableArray new];
			[impl->retainedResources addObject:resource];
		}

		template<typename ImplType>
		MetalUploadArena::Allocation UploadConstants(ImplType* impl, const void* data, size_t size)
		{
			if(!impl->uploadArena || !size) return {};
			// 256 covers the constant-buffer alignment of both Apple and Mac GPU families.
			// The command's completion lease keeps every page alive through GPU execution.
			const auto upload = impl->uploadArena->Allocate(size, 256);
			if(upload) std::memcpy(upload.data, data, size);
			return upload;
		}

	}

	MetalCommandList::MetalCommandList(void* commandQueue,
		std::shared_ptr<MetalUploadPool> uploadPool)
	{
		auto impl = std::make_unique<Impl>();
		impl->commandQueue = (__bridge id<MTLCommandQueue>)commandQueue;
		impl->uploadPool = uploadPool ? std::move(uploadPool) :
			MakeMetalUploadPool(impl->commandQueue.device);
		m_impl = impl.release();
	}

	MetalCommandList::~MetalCommandList()
	{
		if(IsNativePending(m_impl->commandBuffer))
		{
			// Begin registered the arena retention before any possible commit.
			// Even a direct native submission keeps its pages until completion.
			EndRenderEncoding(m_impl);
			EndBlitEncoding(m_impl);
#if !__has_feature(objc_arc)
			[m_impl->resumePass release];
			[m_impl->retainedResources release];
			[m_impl->commandBuffer release];
#endif
		}
		else Reset();
		delete m_impl;
	}

	bool MetalCommandList::Begin()
	{
		if(IsNativePending(m_impl->commandBuffer)) return false;
		Reset();
		m_impl->uploadArena = std::make_shared<MetalUploadArena>(m_impl->uploadPool);
		id<MTLCommandBuffer> commandBuffer = [m_impl->commandQueue commandBuffer];
		if(commandBuffer == nil) return false;
#if !__has_feature(objc_arc)
		[commandBuffer retain];
#endif
		m_impl->commandBuffer = commandBuffer;
		// Completion handlers must be registered before commit. The device's
		// normal retirement still owns the arena; this also covers direct use.
		m_impl->uploadCompletionLease =
			std::make_shared<std::shared_ptr<MetalUploadArena>>(m_impl->uploadArena);
		auto uploads = m_impl->uploadCompletionLease;
		[commandBuffer addCompletedHandler:^(id<MTLCommandBuffer> completed) {
			(void)completed;
			uploads->reset();
		}];
		m_impl->valid = true;
		return true;
	}

	void MetalCommandList::Reset()
	{
		if(m_impl == nullptr) return;
		if(IsNativePending(m_impl->commandBuffer)) return;
		// A discarded autoreleased command buffer may stay alive until its pool
		// drains. Cancel its unused completion lease so reuse can happen now.
		if(m_impl->uploadCompletionLease && (m_impl->commandBuffer == nil ||
			m_impl->commandBuffer.status == MTLCommandBufferStatusNotEnqueued))
			m_impl->uploadCompletionLease->reset();
		m_impl->uploadCompletionLease.reset();
		EndRenderEncoding(m_impl);
		EndBlitEncoding(m_impl);
#if !__has_feature(objc_arc)
		[m_impl->resumePass release];
		[m_impl->retainedResources release];
		[m_impl->commandBuffer release];
#endif
		m_impl->retainedResources = nil;
		m_impl->resumePass = nil;
		m_impl->commandBuffer = nil;
		m_impl->uploadArena.reset();
		m_impl->operations.clear();
		m_impl->vertexBindings.clear();
		m_impl->colorFormats.clear();
		m_impl->inlineConstants.clear();
		m_impl->pipeline = nullptr;
		m_impl->resourceSet = nullptr;
		m_impl->indexBuffer = nullptr;
		m_impl->depthTexture = nullptr;
		m_impl->indexFormat = RHI::Format::Unknown;
		m_impl->depthFormat = RHI::Format::Unknown;
		m_impl->indexOffset = 0;
		m_impl->renderWidth = 0;
		m_impl->renderHeight = 0;
		m_impl->rendering = false;
		m_impl->closed = false;
		m_impl->usesBackBuffer = false;
	}

    bool MetalCommandList::ReplayNative(const std::vector<std::function<bool(ICommandList&)>>& commands)
    {
        @autoreleasepool { return ICommandList::ReplayNative(commands); }
    }

    void MetalCommandList::GlobalBarrierNative()
    {
        // The public RHI places this outside rendering. All resources use tracked
        // hazards on one MTLCommandQueue; ending the current encoder creates the
        // native pass boundary, including resources outside the RenderGraph.
        EndBlitEncoding(m_impl);
    }

	void MetalCommandList::ResourceBarrierNative(
		const RHI::ResourceBarrierDesc* barriers,
		uint32_t count)
	{
		if(m_impl->closed || m_impl->rendering || (count != 0 && barriers == nullptr))
		{
			Invalidate(m_impl);
			return;
		}
		// Device-created resources use tracked hazards on this MTLCommandQueue.
		// End the blit pass so a following pass observes writes even without a state change.
		if(count != 0) EndBlitEncoding(m_impl);

		for(uint32_t index = 0; index < count; ++index)
		{
			const RHI::ResourceBarrierDesc& barrier = barriers[index];
			if((barrier.buffer == nullptr) == (barrier.texture == nullptr) ||
				barrier.after == RHI::ResourceState::Undefined)
			{
				Invalidate(m_impl);
				continue;
			}

			if(barrier.buffer != nullptr)
			{
				auto* buffer = dynamic_cast<MetalBuffer*>(barrier.buffer);
				if(buffer == nullptr || NativeBuffer(buffer) == nil)
				{
					Invalidate(m_impl);
					continue;
				}
				m_impl->operations.push_back({
					OperationKind::BufferBarrier,
					buffer,
					nullptr,
					barrier.after});
				continue;
			}

			auto* texture = dynamic_cast<MetalTexture*>(barrier.texture);
			uint32_t firstMip = 0;
			uint32_t mipCount = 0;
			uint32_t firstLayer = 0;
			uint32_t layerCount = 0;
			if(texture == nullptr || NativeTexture(texture) == nil ||
				((barrier.before == RHI::ResourceState::Present ||
					barrier.after == RHI::ResourceState::Present) &&
					!texture->IsSwapchainImage()) ||
				!MetalValidation::ResolveSubresources(
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
					MetalOperation operation = {};
					operation.kind = OperationKind::TextureBarrier;
					operation.texture = texture;
					operation.after = barrier.after;
					operation.mipLevel = mip;
					operation.arrayLayer = layer;
					m_impl->operations.push_back(operation);
				}
			}
		}
	}

	void MetalCommandList::BeginRenderingNative(const RHI::RenderingDesc& desc)
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
				!MetalValidation::HasUsage(texture->GetDesc().usage, RHI::TextureUsage::RenderTarget) ||
				attachment.mipLevel >= texture->GetDesc().mipLevels ||
				attachment.arrayLayer >= texture->GetDesc().depthOrArraySize ||
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
		if(valid && desc.depthStencilAttachment != nullptr)
		{
			const RHI::DepthStencilAttachment& attachment = *desc.depthStencilAttachment;
			auto* texture = dynamic_cast<MetalTexture*>(attachment.texture);
			id<MTLTexture> native = NativeTexture(texture);
			const bool subresourceValid = texture != nullptr &&
				attachment.mipLevel < texture->GetDesc().mipLevels &&
				attachment.arrayLayer < texture->GetDesc().depthOrArraySize;
			const bool hasStencil = texture != nullptr &&
				RHI::HasStencil(texture->GetDesc().format);
			const bool declaredStateValid =
				attachment.state == RHI::ResourceState::DepthRead ||
				attachment.state == RHI::ResourceState::DepthWrite;
			const bool requiresWrite = attachment.depthLoadOp == RHI::LoadOp::Clear ||
				(hasStencil && attachment.stencilLoadOp == RHI::LoadOp::Clear);
			const bool stateValid = subresourceValid && declaredStateValid &&
				(!requiresWrite || attachment.state == RHI::ResourceState::DepthWrite);
			const bool depthClearValid = attachment.depthLoadOp != RHI::LoadOp::Clear ||
				(std::isfinite(attachment.clearDepth) && attachment.clearDepth >= 0.0f &&
					attachment.clearDepth <= 1.0f);
			const bool stencilClearValid = attachment.stencilLoadOp != RHI::LoadOp::Clear ||
				attachment.clearStencil <= std::numeric_limits<uint8_t>::max();
			if(texture == nullptr || native == nil ||
				!MetalValidation::HasUsage(texture->GetDesc().usage, RHI::TextureUsage::DepthStencil) ||
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
				}
				m_impl->depthFormat = texture->GetDesc().format;
				m_impl->depthTexture = texture;
			}
		}

		if(valid)
		{
#if !__has_feature(objc_arc)
			[m_impl->resumePass release];
#endif
			m_impl->resumePass = nil;
			for(uint32_t index = 0; index < desc.colorAttachmentCount; ++index)
			{
				m_impl->finalColorStoreActions[index] = pass.colorAttachments[index].storeAction;
				pass.colorAttachments[index].storeAction = MTLStoreActionUnknown;
			}
			if(desc.depthStencilAttachment != nullptr)
			{
				m_impl->finalDepthStoreAction = pass.depthAttachment.storeAction;
				pass.depthAttachment.storeAction = MTLStoreActionUnknown;
				if(pass.stencilAttachment.texture != nil)
				{
					m_impl->finalStencilStoreAction = pass.stencilAttachment.storeAction;
					pass.stencilAttachment.storeAction = MTLStoreActionUnknown;
				}
			}
			m_impl->resumePass = [pass copy];
			for(uint32_t index = 0; index < desc.colorAttachmentCount; ++index)
				m_impl->resumePass.colorAttachments[index].loadAction = MTLLoadActionLoad;
			if(pass.depthAttachment.texture != nil)
				m_impl->resumePass.depthAttachment.loadAction = MTLLoadActionLoad;
			if(pass.stencilAttachment.texture != nil)
				m_impl->resumePass.stencilAttachment.loadAction = MTLLoadActionLoad;
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

	void MetalCommandList::EndRenderingNative()
	{
		if(m_impl->closed || !m_impl->rendering)
		{
			Invalidate(m_impl);
			return;
		}
		EndRenderEncoding(m_impl);
		m_impl->rendering = false;
	}

	void MetalCommandList::BindComputePipelineNative(RHI::PipelineHandle pipelineState)
	{
		auto* pipeline = static_cast<MetalPipeline*>(pipelineState);
		if(!m_impl->valid || m_impl->closed || m_impl->rendering ||
			pipeline->GetNativePipeline() == nullptr)
		{
			Invalidate(m_impl);
			return;
		}
		m_impl->pipeline = pipeline;
		m_impl->resourceSet = nullptr;
		m_impl->inlineConstants.assign(pipeline->GetLayout().inlineConstantSize, 0);
	}

	void MetalCommandList::DispatchNative(uint32_t x, uint32_t y, uint32_t z)
	{
		DispatchCompute(x, y, z, nullptr, 0);
	}

	void MetalCommandList::DispatchIndirectNative(RHI::BufferHandle arguments, uint32_t offset)
	{
		auto* buffer = static_cast<MetalBuffer*>(arguments);
		if(!m_impl->valid || m_impl->closed || NativeBuffer(buffer) == nil)
		{
			Invalidate(m_impl);
			return;
		}
		RetainResource(m_impl, NativeBuffer(buffer));
		DispatchCompute(0, 0, 0, buffer, offset);
	}

	void MetalCommandList::DispatchCompute(uint32_t x, uint32_t y, uint32_t z,
		MetalBuffer* arguments, uint32_t offset)
	{
		auto* pipeline = m_impl->pipeline;
		if(!m_impl->valid || m_impl->closed || m_impl->rendering || !pipeline)
		{
			Invalidate(m_impl);
			return;
		}
		EndBlitEncoding(m_impl);
		id<MTLComputeCommandEncoder> encoder = [m_impl->commandBuffer computeCommandEncoder];
		if(encoder == nil) { Invalidate(m_impl); return; }
		[encoder setComputePipelineState:(__bridge id<MTLComputePipelineState>)pipeline->GetNativePipeline()];
		const auto& layout = pipeline->GetLayout();
		if(m_impl->resourceSet)
			for(uint32_t i = 0; i < m_impl->resourceSet->GetBindingCount(); ++i)
			{
				const auto& binding = m_impl->resourceSet->GetBindings()[i];
				auto* buffer = static_cast<MetalBuffer*>(binding.buffer);
				if(NativeBuffer(buffer) == nil) { Invalidate(m_impl); break; }
				[encoder setBuffer:NativeBuffer(buffer) offset:binding.offset atIndex:binding.binding + binding.arrayElement];
			}
		if(!m_impl->inlineConstants.empty())
		{
			const auto constants = UploadConstants(m_impl, m_impl->inlineConstants.data(), m_impl->inlineConstants.size());
			if(!constants) Invalidate(m_impl);
			else
			{
				[encoder setBuffer:constants.page->buffer offset:static_cast<NSUInteger>(constants.offset)
					atIndex:layout.inlineConstantBinding];
			}
		}
		const auto* size = pipeline->GetThreadGroupSize();
		if(m_impl->valid)
		{
			if(arguments != nullptr)
				[encoder dispatchThreadgroupsWithIndirectBuffer:NativeBuffer(arguments)
					indirectBufferOffset:offset threadsPerThreadgroup:MTLSizeMake(size[0], size[1], size[2])];
			else
				[encoder dispatchThreadgroups:MTLSizeMake(x, y, z)
					threadsPerThreadgroup:MTLSizeMake(size[0], size[1], size[2])];
		}
		// Tracked buffer hazards synchronize this pass with subsequent compute/render/blit passes.
		[encoder endEncoding];
	}

	void MetalCommandList::BindGraphicsPipelineNative(RHI::PipelineHandle pipelineState)
	{
		auto* pipeline = static_cast<MetalPipeline*>(pipelineState);
		if(!m_impl->valid || m_impl->closed || !m_impl->rendering ||
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
		for(uint32_t index = 0; index < pipeline->GetDesc().vertexBufferCount; ++index)
		{
			const uint32_t binding = pipeline->GetDesc().vertexBuffers[index].binding;
			const auto found = m_impl->vertexBindings.find(binding);
			if(found == m_impl->vertexBindings.end()) continue;
			id<MTLBuffer> native = NativeBuffer(found->second.buffer);
			[encoder setVertexBuffer:native offset:found->second.offset atIndex:binding];
			[encoder useResource:native usage:MTLResourceUsageRead];
		}
		for(const MetalStaticSamplerBinding& binding :
			pipeline->GetStaticSamplerBindings())
		{
			id<MTLSamplerState> sampler =
				(__bridge id<MTLSamplerState>)binding.sampler;
			if(HasStage(binding.stages, RHI::ShaderStageFlags::Vertex) ||
				HasStage(binding.stages, RHI::ShaderStageFlags::Domain))
				[encoder setVertexSamplerState:sampler atIndex:binding.index];
			if(HasStage(binding.stages, RHI::ShaderStageFlags::Mesh))
				if(@available(macOS 13.0, iOS 16.0, *))
					[encoder setMeshSamplerState:sampler atIndex:binding.index];
			if(HasStage(binding.stages, RHI::ShaderStageFlags::Fragment))
				[encoder setFragmentSamplerState:sampler atIndex:binding.index];
		}
	}

	void MetalCommandList::BindResourceSetNative(RHI::ResourceSetHandle resourceSet)
	{
		auto* set = static_cast<MetalResourceSet*>(resourceSet);
		if(!m_impl->valid || m_impl->closed || m_impl->pipeline == nullptr ||
			(!m_impl->rendering && !m_impl->pipeline->IsCompute()))
		{
			Invalidate(m_impl);
			return;
		}

		if(m_impl->pipeline->IsCompute()) { m_impl->resourceSet = set; return; }

		const RHI::PipelineLayoutDesc& layout = m_impl->pipeline->GetLayout();
		id<MTLRenderCommandEncoder> encoder = m_impl->renderEncoder;
		for(uint32_t index = 0; index < set->GetBindingCount(); ++index)
		{
			const RHI::ResourceBinding& binding = set->GetBindings()[index];
			if(binding.buffer == nullptr) continue;
			const RHI::ResourceBindingLayout* declaration =
				MetalValidation::FindLayoutBinding(layout, binding.binding);
			const uint32_t nativeIndex = binding.binding + binding.arrayElement;
			if(declaration->type == RHI::ResourceBindingType::ConstantBuffer ||
				declaration->type == RHI::ResourceBindingType::ReadOnlyStorageBuffer ||
				declaration->type == RHI::ResourceBindingType::ReadWriteStorageBuffer)
			{
				auto* buffer = static_cast<MetalBuffer*>(binding.buffer);
				id<MTLBuffer> native = NativeBuffer(buffer);
				if(HasStage(declaration->stages, RHI::ShaderStageFlags::Vertex) ||
					HasStage(declaration->stages, RHI::ShaderStageFlags::Domain))
					[encoder setVertexBuffer:native offset:binding.offset atIndex:nativeIndex];
				if(HasStage(declaration->stages, RHI::ShaderStageFlags::Mesh))
					if(@available(macOS 13.0, iOS 16.0, *))
						[encoder setMeshBuffer:native offset:binding.offset atIndex:nativeIndex];
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
			if(binding.nativeTexture == nullptr)
			{
				Invalidate(m_impl);
				return;
			}
			id<MTLTexture> native = (__bridge id<MTLTexture>)binding.nativeTexture;
			if(HasStage(binding.stages, RHI::ShaderStageFlags::Vertex) ||
				HasStage(binding.stages, RHI::ShaderStageFlags::Domain))
				[encoder setVertexTexture:native atIndex:binding.index];
			if(HasStage(binding.stages, RHI::ShaderStageFlags::Mesh))
				if(@available(macOS 13.0, iOS 16.0, *))
					[encoder setMeshTexture:native atIndex:binding.index];
			if(HasStage(binding.stages, RHI::ShaderStageFlags::Fragment))
				[encoder setFragmentTexture:native atIndex:binding.index];
			const MTLResourceUsage usage =
				binding.requiredState == RHI::ResourceState::UnorderedAccess
				? (MTLResourceUsageRead | MTLResourceUsageWrite) : MTLResourceUsageRead;
			[encoder useResource:native usage:usage];
		}

		m_impl->resourceSet = set;
	}

	void MetalCommandList::BindVertexBufferNative(
		uint32_t binding,
		RHI::BufferHandle buffer,
		uint32_t offset)
	{
		auto* metalBuffer = static_cast<MetalBuffer*>(buffer);
		if(!m_impl->valid || m_impl->closed || !m_impl->rendering ||
			m_impl->pipeline == nullptr || NativeBuffer(metalBuffer) == nil)
		{
			Invalidate(m_impl);
			return;
		}
		m_impl->vertexBindings[binding] = {metalBuffer, offset};
		id<MTLBuffer> native = NativeBuffer(metalBuffer);
		[m_impl->renderEncoder setVertexBuffer:native offset:offset atIndex:binding];
		[m_impl->renderEncoder useResource:native usage:MTLResourceUsageRead];
	}

	void MetalCommandList::BindIndexBufferNative(
		RHI::BufferHandle buffer,
		RHI::Format format,
		uint32_t offset)
	{
		auto* metalBuffer = static_cast<MetalBuffer*>(buffer);
		if(!m_impl->valid || m_impl->closed || !m_impl->rendering || NativeBuffer(metalBuffer) == nil)
		{
			Invalidate(m_impl);
			return;
		}
		m_impl->indexBuffer = metalBuffer;
		m_impl->indexFormat = format;
		m_impl->indexOffset = offset;
		[m_impl->renderEncoder useResource:NativeBuffer(metalBuffer) usage:MTLResourceUsageRead];
	}

	void MetalCommandList::SetInlineConstantsNative(
		uint32_t offset,
		uint32_t size,
		const void* data)
	{
		if(m_impl->closed || m_impl->pipeline == nullptr ||
			(!m_impl->rendering && !m_impl->pipeline->IsCompute()))
		{
			Invalidate(m_impl);
			return;
		}
		std::memcpy(m_impl->inlineConstants.data() + offset, data, size);
		if(m_impl->pipeline->IsCompute()) return;
		const auto constants = UploadConstants(m_impl, m_impl->inlineConstants.data(), m_impl->inlineConstants.size());
		if(!constants)
		{
			Invalidate(m_impl);
			return;
		}
		const RHI::PipelineLayoutDesc& layout = m_impl->pipeline->GetLayout();
		if(HasStage(layout.inlineConstantStages, RHI::ShaderStageFlags::Vertex) ||
			HasStage(layout.inlineConstantStages, RHI::ShaderStageFlags::Domain))
			[m_impl->renderEncoder setVertexBuffer:constants.page->buffer
				offset:static_cast<NSUInteger>(constants.offset) atIndex:layout.inlineConstantBinding];
		if(HasStage(layout.inlineConstantStages, RHI::ShaderStageFlags::Mesh))
			if(@available(macOS 13.0, iOS 16.0, *))
				[m_impl->renderEncoder setMeshBuffer:constants.page->buffer
					offset:static_cast<NSUInteger>(constants.offset) atIndex:layout.inlineConstantBinding];
		if(HasStage(layout.inlineConstantStages, RHI::ShaderStageFlags::Fragment))
			[m_impl->renderEncoder setFragmentBuffer:constants.page->buffer
				offset:static_cast<NSUInteger>(constants.offset) atIndex:layout.inlineConstantBinding];
		[m_impl->renderEncoder useResource:constants.page->buffer usage:MTLResourceUsageRead];
	}

	void MetalCommandList::SetViewportNative(const RHI::Viewport& viewport)
	{
		if(!m_impl->valid || m_impl->closed || !m_impl->rendering || m_impl->renderEncoder == nil)
		{
			Invalidate(m_impl);
			return;
		}
		MTLViewport native = {
			viewport.x, viewport.y, viewport.width, viewport.height,
			viewport.minDepth, viewport.maxDepth};
		[m_impl->renderEncoder setViewport:native];
		m_impl->viewport = viewport;
	}

	void MetalCommandList::SetScissorNative(const RHI::Rect& rect)
	{
		if(!m_impl->valid || m_impl->closed || !m_impl->rendering || m_impl->renderEncoder == nil ||
			rect.x < 0 || rect.y < 0 ||
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
		m_impl->scissor = rect;
	}

	void MetalCommandList::SetStencilReferenceNative(uint32_t reference)
	{
		if(!m_impl->valid || m_impl->closed || !m_impl->rendering || m_impl->renderEncoder == nil)
		{
			Invalidate(m_impl);
			return;
		}
		[m_impl->renderEncoder setStencilReferenceValue:reference];
		m_impl->stencilReference = reference;
	}

	namespace
	{
		template<typename ImplType>
		[[nodiscard]] bool ValidateDrawState(const ImplType* impl)
		{
			return impl->valid && impl->rendering && impl->pipeline != nullptr &&
				impl->renderEncoder != nil;
		}
	}

	void MetalCommandList::DrawInstancedNative(
		uint32_t vertexCount,
		uint32_t instanceCount,
		uint32_t startVertex,
		uint32_t startInstance)
	{
		if(m_impl->closed || !ValidateDrawState(m_impl))
		{
			Invalidate(m_impl);
			return;
		}
		if(m_impl->pipeline->IsTessellated())
		{
			const auto& desc = m_impl->pipeline->GetDesc();
			if(desc.patchControlPoints == 0 || vertexCount % desc.patchControlPoints != 0 ||
				startVertex % desc.patchControlPoints != 0)
			{
				Invalidate(m_impl);
				return;
			}
			const uint32_t patchCount = vertexCount / desc.patchControlPoints;
			const uint32_t factorCount = desc.tessellation.domain == RHI::TessellationDomain::Quad ? 6u : 4u;
			const uint32_t factorStride = factorCount * sizeof(uint16_t);
			uint64_t maxFactorBytes = std::numeric_limits<NSUInteger>::max();
			if(@available(macOS 10.14, *))
				maxFactorBytes = m_impl->commandQueue.device.maxBufferLength;
			MetalTessellationFactorRange factorRange;
			if(!GetMetalTessellationFactorRange(factorStride, patchCount, instanceCount,
				startVertex / desc.patchControlPoints, startInstance, maxFactorBytes, factorRange) ||
				m_impl->resumePass == nil)
			{
				Invalidate(m_impl);
				return;
			}
			id<MTLBuffer> factorBuffer = [m_impl->commandQueue.device
				newBufferWithLength:static_cast<NSUInteger>(factorRange.bytes)
				options:MTLResourceStorageModeShared];
			if(factorBuffer == nil)
			{
				Invalidate(m_impl);
				return;
			}
			RetainResource(m_impl, factorBuffer);

			MetalPipeline* pipeline = m_impl->pipeline;
			MetalResourceSet* resourceSet = m_impl->resourceSet;
			const auto inlineConstants = m_impl->inlineConstants;
			EndRenderEncoding(m_impl, true);

			id<MTLComputeCommandEncoder> compute = [m_impl->commandBuffer computeCommandEncoder];
			id<MTLComputePipelineState> hull =
				(__bridge id<MTLComputePipelineState>)pipeline->GetNativeHullPipeline();
			if(compute == nil || hull == nil)
			{
				if(compute != nil) [compute endEncoding];
				Invalidate(m_impl);
#if !__has_feature(objc_arc)
				[factorBuffer release];
#endif
				return;
			}
			[compute setComputePipelineState:hull];
			for(uint32_t index = 0; index < desc.vertexBufferCount; ++index)
			{
				const uint32_t binding = desc.vertexBuffers[index].binding;
				const auto& value = m_impl->vertexBindings.at(binding);
				[compute setBuffer:NativeBuffer(value.buffer) offset:value.offset atIndex:binding];
			}
			if(resourceSet != nullptr)
			{
				const auto& layout = pipeline->GetLayout();
				for(uint32_t index = 0; index < resourceSet->GetBindingCount(); ++index)
				{
					const auto& binding = resourceSet->GetBindings()[index];
					const auto* declaration = MetalValidation::FindLayoutBinding(layout, binding.binding);
					if(declaration == nullptr ||
						(!HasStage(declaration->stages, RHI::ShaderStageFlags::Vertex) &&
						 !HasStage(declaration->stages, RHI::ShaderStageFlags::Hull))) continue;
					if(binding.buffer != nullptr)
						[compute setBuffer:NativeBuffer(dynamic_cast<MetalBuffer*>(binding.buffer))
							offset:binding.offset atIndex:binding.binding + binding.arrayElement];
				}
				for(const MetalTextureBinding& binding : resourceSet->GetTextureBindings())
					if(HasStage(binding.stages, RHI::ShaderStageFlags::Vertex) ||
						HasStage(binding.stages, RHI::ShaderStageFlags::Hull))
						[compute setTexture:(__bridge id<MTLTexture>)binding.nativeTexture atIndex:binding.index];
			}
			for(const MetalStaticSamplerBinding& binding : pipeline->GetStaticSamplerBindings())
				if(HasStage(binding.stages, RHI::ShaderStageFlags::Vertex) ||
					HasStage(binding.stages, RHI::ShaderStageFlags::Hull))
					[compute setSamplerState:(__bridge id<MTLSamplerState>)binding.sampler atIndex:binding.index];
			if(!inlineConstants.empty() &&
				(HasStage(pipeline->GetLayout().inlineConstantStages, RHI::ShaderStageFlags::Vertex) ||
				 HasStage(pipeline->GetLayout().inlineConstantStages, RHI::ShaderStageFlags::Hull)))
			{
				const auto constants = UploadConstants(m_impl, inlineConstants.data(), inlineConstants.size());
				if(!constants)
				{
					[compute endEncoding];
					Invalidate(m_impl);
#if !__has_feature(objc_arc)
					[factorBuffer release];
#endif
					return;
				}
				[compute setBuffer:constants.page->buffer offset:static_cast<NSUInteger>(constants.offset)
					atIndex:pipeline->GetLayout().inlineConstantBinding];
			}
			[compute setBuffer:factorBuffer offset:static_cast<NSUInteger>(factorRange.writeOffset)
				atIndex:MetalPipeline::TessellationFactorBufferIndex];
			const NSUInteger threadCount = static_cast<NSUInteger>(patchCount) * instanceCount;
			const NSUInteger width = std::min(threadCount, hull.threadExecutionWidth);
			[compute dispatchThreads:MTLSizeMake(threadCount, 1, 1)
				threadsPerThreadgroup:MTLSizeMake(width, 1, 1)];
			[compute endEncoding];

			id<MTLRenderCommandEncoder> resumed =
				[m_impl->commandBuffer renderCommandEncoderWithDescriptor:m_impl->resumePass];
			if(resumed == nil)
			{
				Invalidate(m_impl);
#if !__has_feature(objc_arc)
				[factorBuffer release];
#endif
				return;
			}
#if !__has_feature(objc_arc)
			[resumed retain];
#endif
			m_impl->renderEncoder = resumed;
			BindGraphicsPipelineNative(pipeline);
			if(resourceSet != nullptr) BindResourceSetNative(resourceSet);
			if(!inlineConstants.empty())
				SetInlineConstantsNative(0, static_cast<uint32_t>(inlineConstants.size()), inlineConstants.data());
			SetViewportNative(m_impl->viewport);
			SetScissorNative(m_impl->scissor);
			if(desc.depthStencil.stencilEnabled) SetStencilReferenceNative(m_impl->stencilReference);
			if(!m_impl->valid)
			{
#if !__has_feature(objc_arc)
				[factorBuffer release];
#endif
				return;
			}
			[m_impl->renderEncoder setTessellationFactorBuffer:factorBuffer
				offset:0 instanceStride:static_cast<NSUInteger>(factorRange.instanceStride)];
			[m_impl->renderEncoder
				drawPatches:desc.patchControlPoints
				patchStart:startVertex / desc.patchControlPoints
				patchCount:patchCount
				patchIndexBuffer:nil
				patchIndexBufferOffset:0
				instanceCount:instanceCount
				baseInstance:startInstance];
#if !__has_feature(objc_arc)
			[factorBuffer release];
#endif
			return;
		}
		[m_impl->renderEncoder
			drawPrimitives:static_cast<MTLPrimitiveType>(m_impl->pipeline->GetNativePrimitiveType())
			vertexStart:startVertex
			vertexCount:vertexCount
			instanceCount:instanceCount
			baseInstance:startInstance];
	}

	void MetalCommandList::DrawIndexedInstancedNative(
		uint32_t indexCount,
		uint32_t instanceCount,
		uint32_t firstIndex,
		int32_t vertexOffset,
		uint32_t firstInstance)
	{
		if(m_impl->pipeline != nullptr && m_impl->pipeline->IsTessellated())
		{
			// Metal의 control-point index 형식은 파이프라인 상태이므로 현재 공통 API는
			// 비인덱스 패치만 허용한다. D3D12/Vulkan의 인덱스 패치는 그대로 지원된다.
			Invalidate(m_impl);
			return;
		}
		const uint32_t indexSize = MetalValidation::FormatSize(m_impl->indexFormat);
		const uint64_t nativeOffset = static_cast<uint64_t>(m_impl->indexOffset) +
			static_cast<uint64_t>(firstIndex) * indexSize;
		if(m_impl->closed || !ValidateDrawState(m_impl))
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

	void MetalCommandList::DrawIndirectNative(RHI::BufferHandle arguments, uint32_t offset)
	{
		auto* buffer = static_cast<MetalBuffer*>(arguments);
		if(m_impl->closed || !ValidateDrawState(m_impl) ||
			m_impl->pipeline->IsTessellated() || NativeBuffer(buffer) == nil)
		{
			Invalidate(m_impl);
			return;
		}
		RetainResource(m_impl, NativeBuffer(buffer));
		[m_impl->renderEncoder
			drawPrimitives:static_cast<MTLPrimitiveType>(m_impl->pipeline->GetNativePrimitiveType())
			indirectBuffer:NativeBuffer(buffer) indirectBufferOffset:offset];
	}

	void MetalCommandList::DrawIndexedIndirectNative(RHI::BufferHandle arguments, uint32_t offset)
	{
		auto* buffer = static_cast<MetalBuffer*>(arguments);
		if(m_impl->closed || !ValidateDrawState(m_impl) ||
			m_impl->pipeline->IsTessellated() ||
			NativeBuffer(m_impl->indexBuffer) == nil || NativeBuffer(buffer) == nil)
		{
			Invalidate(m_impl);
			return;
		}
		RetainResource(m_impl, NativeBuffer(buffer));
		const MTLIndexType indexType = m_impl->indexFormat == RHI::Format::R16_UINT
			? MTLIndexTypeUInt16 : MTLIndexTypeUInt32;
		[m_impl->renderEncoder
			drawIndexedPrimitives:static_cast<MTLPrimitiveType>(m_impl->pipeline->GetNativePrimitiveType())
			indexType:indexType indexBuffer:NativeBuffer(m_impl->indexBuffer)
			indexBufferOffset:m_impl->indexOffset
			indirectBuffer:NativeBuffer(buffer) indirectBufferOffset:offset];
	}

	void MetalCommandList::DispatchMeshNative(
		uint32_t threadGroupCountX,
		uint32_t threadGroupCountY,
		uint32_t threadGroupCountZ)
	{
		if(m_impl->closed || !ValidateDrawState(m_impl))
		{
			Invalidate(m_impl);
			return;
		}
		if(@available(macOS 13.0, iOS 16.0, *))
		{
			if(m_impl->pipeline == nullptr || m_impl->pipeline->GetNativePipeline() == nullptr)
			{
				Invalidate(m_impl);
				return;
			}
			id<MTLRenderPipelineState> pipelineState =
				(__bridge id<MTLRenderPipelineState>)m_impl->pipeline->GetNativePipeline();
			NSUInteger meshThreads = pipelineState.maxTotalThreadsPerMeshThreadgroup;
			if(meshThreads == 0)
			{
				Invalidate(m_impl);
				return;
			}
			NSUInteger objectThreads = pipelineState.maxTotalThreadsPerObjectThreadgroup;
			if(objectThreads == 0)
			{
				objectThreads = 1;
			}
			MTLSize threadgroups = MTLSizeMake(threadGroupCountX, threadGroupCountY, threadGroupCountZ);
			MTLSize threadsPerObjectThreadgroup = MTLSizeMake(objectThreads, 1, 1);
			MTLSize threadsPerMeshThreadgroup = MTLSizeMake(meshThreads, 1, 1);
			[m_impl->renderEncoder
				drawMeshThreadgroups:threadgroups
				threadsPerObjectThreadgroup:threadsPerObjectThreadgroup
				threadsPerMeshThreadgroup:threadsPerMeshThreadgroup];
		}
		else
		{
			Invalidate(m_impl);
		}
	}

	bool MetalCommandList::CloseNative()
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

        return m_impl->valid;
    }

	bool MetalCommandList::RecordBufferUpdate(
		MetalBuffer* buffer,
		uint32_t offset,
		const void* data,
		uint32_t size)
	{
		if(m_impl->closed || m_impl->rendering || NativeBuffer(buffer) == nil)
		{
			return false;
		}
		if(offset % 4u != 0 || size % 4u != 0)
		{
			std::fprintf(stderr,
				"dyf::RHI [error]: Metal buffer copy requires 4-byte offset and size alignment (offset=%u, size=%u); the native copy was not issued.\n",
				offset, size);
			return false;
		}
		if(!m_impl->uploadArena) return false;
		const auto upload = m_impl->uploadArena->Allocate(size, 16);
		if(!upload) return false;
		std::memcpy(upload.data, data, size);
		if(!EnsureBlitEncoder(m_impl)) return false;
		[m_impl->blitEncoder copyFromBuffer:upload.page->buffer
			sourceOffset:static_cast<NSUInteger>(upload.offset)
			toBuffer:NativeBuffer(buffer)
			destinationOffset:offset
			size:size];
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
		(void)dataSize; (void)slicePitch; // Source bounds were checked by common RHI.
		if(m_impl->closed || m_impl->rendering || NativeTexture(texture) == nil)
		{
			return false;
		}
		const auto format = texture->GetDesc().format;
		const bool compressed = RHI::IsBlockCompressed(format);
		const uint32_t elementSize = compressed ? RHI::CompressedBlockBytes(format) : MetalValidation::FormatSize(format);
		const uint32_t mipWidth = std::max(1u, texture->GetDesc().width >> mipLevel);
		const uint32_t mipHeight = std::max(1u, texture->GetDesc().height >> mipLevel);
		const uint32_t columns = compressed ? mipWidth / 4u + (mipWidth % 4u != 0) : mipWidth;
		const uint32_t rows = compressed ? mipHeight / 4u + (mipHeight % 4u != 0) : mipHeight;
		if(elementSize == 0 || columns > std::numeric_limits<uint32_t>::max() / elementSize)
			return false;
		const uint32_t tightRowPitch = columns * elementSize;

		if(RHI::HasStencil(texture->GetDesc().format) ||
			(elementSize & (elementSize - 1u)) != 0) return false;

		id<MTLTexture> nativeTexture = NativeTexture(texture);
		const uint64_t stagingSize64 = static_cast<uint64_t>(tightRowPitch) * rows;
		if(stagingSize64 > std::numeric_limits<NSUInteger>::max())
		{
			return false;
		}
		if(!m_impl->uploadArena) return false;
		// For BC, pitches count 4x4 block rows, including the final 1x1/2x2 mip.
		// Sixteen-byte offsets cover both pixel sizes and BC1/BC3 block sizes.
		const auto upload = m_impl->uploadArena->Allocate(stagingSize64, 16);
		if(!upload) return false;
		auto* destination = upload.data;
		const auto* source = static_cast<const uint8_t*>(data);
		// 입력 행 사이의 패딩만 건너뛴다. 마지막 행 뒤의 패딩은 요구하거나 읽지 않는다.
		for(uint32_t row = 0; row < rows; ++row)
			std::memcpy(destination + static_cast<size_t>(row) * tightRowPitch,
				source + static_cast<size_t>(row) * rowPitch, tightRowPitch);

		if(!EnsureBlitEncoder(m_impl)) return false;
		TrackSwapchainImage(m_impl, texture);
		[m_impl->blitEncoder
			copyFromBuffer:upload.page->buffer
			sourceOffset:static_cast<NSUInteger>(upload.offset)
			sourceBytesPerRow:tightRowPitch
			sourceBytesPerImage:0
			sourceSize:MTLSizeMake(mipWidth, mipHeight, 1)
			toTexture:nativeTexture
			destinationSlice:arrayLayer
			destinationLevel:mipLevel
			destinationOrigin:MTLOriginMake(0, 0, 0)];
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
