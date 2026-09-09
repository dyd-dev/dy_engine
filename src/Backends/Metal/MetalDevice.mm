#include "MetalDevice.h"
#include "MetalBuffer.h"
#include "MetalTexture.h"
#include "MetalPipeline.h"
#include "MetalCommandList.h"
#include "MetalResourceSet.h"
#include "MetalShader.h"
#include "RHI/Readback.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <set>
#include <vector>

#import <AppKit/AppKit.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <dispatch/dispatch.h>

namespace dy::Backends
{
    struct MetalObjectDeleter
    {
        template<typename Object>
        void operator()(Object* object) const
        {
            delete object;
        }
    };

    namespace
    {
        RHI::Format FromLayerPixelFormat(MTLPixelFormat format)
        {
            switch(format)
            {
            case MTLPixelFormatBGRA8Unorm: return RHI::Format::B8G8R8A8_UNORM;
            case MTLPixelFormatBGRA8Unorm_sRGB: return RHI::Format::B8G8R8A8_UNORM_SRGB;
            case MTLPixelFormatRGBA16Float: return RHI::Format::R16G16B16A16_FLOAT;
            default: return RHI::Format::Unknown;
            }
        }

        MTLPixelFormat ToLayerPixelFormat(RHI::Format format)
        {
            switch(format)
            {
            case RHI::Format::B8G8R8A8_UNORM: return MTLPixelFormatBGRA8Unorm;
            case RHI::Format::B8G8R8A8_UNORM_SRGB: return MTLPixelFormatBGRA8Unorm_sRGB;
            case RHI::Format::R16G16B16A16_FLOAT: return MTLPixelFormatRGBA16Float;
            default: return MTLPixelFormatInvalid;
            }
        }

        bool IsFinished(id<MTLCommandBuffer> commandBuffer)
        {
            if(commandBuffer == nil) return false;
            const MTLCommandBufferStatus status = commandBuffer.status;
            return status == MTLCommandBufferStatusCompleted ||
                status == MTLCommandBufferStatusError;
        }

		template<typename Object>
		struct RetiredObject
		{
			uint64_t completionValue = 0;
			std::unique_ptr<Object, MetalObjectDeleter> object;
		};

		template<typename Object, typename Interface>
		bool RetireObject(
			std::vector<std::unique_ptr<Object, MetalObjectDeleter>>& liveObjects,
			Interface* object,
			uint64_t completionValue,
			std::vector<RetiredObject<Object>>& retiredObjects)
		{
			const auto found = std::find_if(
				liveObjects.begin(), liveObjects.end(),
				[object](const std::unique_ptr<Object, MetalObjectDeleter>& candidate)
				{
					return static_cast<Interface*>(candidate.get()) == object;
				});
			if(found == liveObjects.end()) return false;
			retiredObjects.push_back({completionValue, nullptr});
			retiredObjects.back().object = std::move(*found);
			liveObjects.erase(found);
			return true;
		}

		template<typename Object>
		void ReclaimObjects(
			std::vector<RetiredObject<Object>>& objects,
			uint64_t completedValue)
		{
			objects.erase(
				std::remove_if(
					objects.begin(), objects.end(),
					[completedValue](const RetiredObject<Object>& object)
					{
						return object.completionValue <= completedValue;
					}),
				objects.end());
		}
    }

    struct MetalDevice::Impl
    {
        struct FrameSlot
        {
            uint64_t submissionValue = 0;
        };

        struct Submission
        {
            uint64_t value = 0;
            std::vector<std::unique_ptr<MetalCommandList, MetalObjectDeleter>> commandLists;
            id<MTLCommandBuffer> presentCommandBuffer = nil;
            id<CAMetalDrawable> drawable = nil;
            bool waitingForPresent = false;
        };

        id<MTLDevice> device = nil;
        id<MTLCommandQueue> commandQueue = nil;
        const void* windowHandle = nullptr;

        CAMetalLayer* metalLayer = nil;
        id<CAMetalDrawable> currentDrawable = nil;
        MetalTexture* backBufferTex = nullptr;

        dispatch_queue_t drawableQueue = nullptr;
        std::mutex drawableMutex;
        id<CAMetalDrawable> readyDrawable = nil;
        bool drawableRequestPending = false;
        bool stoppingDrawableAcquisition = false;

        std::vector<FrameSlot> frameSlots;
        std::vector<std::unique_ptr<MetalCommandList, MetalObjectDeleter>> activeCommandLists;
		std::vector<std::unique_ptr<MetalBuffer, MetalObjectDeleter>> liveBuffers;
		std::vector<std::unique_ptr<MetalTexture, MetalObjectDeleter>> liveTextures;
		std::vector<std::unique_ptr<MetalShader, MetalObjectDeleter>> liveShaders;
		std::vector<std::unique_ptr<MetalPipeline, MetalObjectDeleter>> livePipelines;
		std::vector<std::unique_ptr<MetalResourceSet, MetalObjectDeleter>> liveResourceSets;
		std::vector<RetiredObject<MetalBuffer>> retiredBuffers;
		std::vector<RetiredObject<MetalTexture>> retiredTextures;
		std::vector<RetiredObject<MetalShader>> retiredShaders;
		std::vector<RetiredObject<MetalPipeline>> retiredPipelines;
		std::vector<RetiredObject<MetalResourceSet>> retiredResourceSets;
        uint32_t nextFrameSlot = 0;
        uint32_t activeFrameSlot = 0;
        bool frameActive = false;
        bool allowReadback = false;

        std::vector<Submission> submissions;
        uint64_t nextSubmissionValue = 1;
        uint64_t completedSubmissionValue = 0;
		uint64_t lastSubmittedValue = 0;
        uint64_t pendingPresentValue = 0;
        bool asyncWorkFailed = false;

        void RequestDrawable()
        {
            CAMetalLayer* layerToRequest = nil;
            dispatch_queue_t queue = nullptr;
            {
                std::unique_lock<std::mutex> lock(
                    drawableMutex, std::try_to_lock);
                if(!lock.owns_lock()) return;
                if(stoppingDrawableAcquisition || drawableRequestPending ||
                    readyDrawable != nil || drawableQueue == nullptr || metalLayer == nil)
                {
                    return;
                }

                drawableRequestPending = true;
                layerToRequest = metalLayer;
                queue = drawableQueue;
#if !__has_feature(objc_arc)
                [layerToRequest retain];
#endif
            }

            dispatch_async(queue, ^{
                @autoreleasepool
                {
                    id<CAMetalDrawable> drawable = nil;
                    {
                        std::lock_guard<std::mutex> lock(drawableMutex);
                        if(!stoppingDrawableAcquisition)
                            drawable = [layerToRequest nextDrawable];
                        if(!stoppingDrawableAcquisition && drawable != nil && readyDrawable == nil)
                        {
#if !__has_feature(objc_arc)
                            [drawable retain];
#endif
                            readyDrawable = drawable;
                        }
                        drawableRequestPending = false;
                    }
#if !__has_feature(objc_arc)
                    [layerToRequest release];
#endif
                }
            });
        }

        void StopDrawableAcquisition()
        {
            {
                std::lock_guard<std::mutex> lock(drawableMutex);
                stoppingDrawableAcquisition = true;
            }

            if(drawableQueue != nullptr)
                dispatch_sync(drawableQueue, ^{});

            std::lock_guard<std::mutex> lock(drawableMutex);
#if !__has_feature(objc_arc)
            [readyDrawable release];
#endif
            readyDrawable = nil;
            drawableRequestPending = false;
        }

        void ClearCurrentDrawable()
        {
#if !__has_feature(objc_arc)
            [currentDrawable release];
#endif
            currentDrawable = nil;
        }

        static void ReleaseSubmission(Submission& submission)
        {
#if !__has_feature(objc_arc)
            [submission.presentCommandBuffer release];
            [submission.drawable release];
#endif
            submission.presentCommandBuffer = nil;
            submission.drawable = nil;
            submission.commandLists.clear();
        }

        void CollectCompletedSubmissions()
        {
            while(!submissions.empty())
            {
                Submission& submission = submissions.front();
                if(submission.waitingForPresent)
                    break;

                id<MTLCommandBuffer> completion = submission.presentCommandBuffer;
                if(completion == nil && !submission.commandLists.empty())
                {
                    completion = (__bridge id<MTLCommandBuffer>)
                        submission.commandLists.back()->GetNativeCommandBuffer();
                }
                if(!IsFinished(completion))
                    break;

                for(const std::unique_ptr<MetalCommandList, MetalObjectDeleter>& commandList :
                    submission.commandLists)
                {
                    id<MTLCommandBuffer> commandBuffer =
                        (__bridge id<MTLCommandBuffer>)
                            commandList->GetNativeCommandBuffer();
                    if(commandBuffer.status == MTLCommandBufferStatusError)
                    {
                        asyncWorkFailed = true;
                        NSLog(@"Metal submission failed: %@", commandBuffer.error);
                    }
                }
                if(submission.presentCommandBuffer.status ==
                    MTLCommandBufferStatusError)
                {
                    asyncWorkFailed = true;
                    NSLog(@"Metal presentation failed: %@",
                        submission.presentCommandBuffer.error);
                }

                completedSubmissionValue = submission.value;
                ReleaseSubmission(submission);
                submissions.erase(submissions.begin());
            }
			CollectRetiredObjects();
        }

		void CollectRetiredObjects()
		{
			ReclaimObjects(retiredResourceSets, completedSubmissionValue);
			ReclaimObjects(retiredPipelines, completedSubmissionValue);
			ReclaimObjects(retiredShaders, completedSubmissionValue);
			ReclaimObjects(retiredTextures, completedSubmissionValue);
			ReclaimObjects(retiredBuffers, completedSubmissionValue);
		}

        Submission* FindSubmission(uint64_t value)
        {
            const auto it = std::find_if(
                submissions.begin(), submissions.end(),
                [value](const Submission& submission)
                {
                    return submission.value == value;
                });
            return it == submissions.end() ? nullptr : &*it;
        }

        bool DrainGpuForShutdown()
        {
            if(commandQueue == nil) return submissions.empty();

            id<MTLCommandBuffer> marker = [commandQueue commandBuffer];
            if(marker == nil) return false;
            [marker commit];
            [marker waitUntilCompleted];
            return marker.status == MTLCommandBufferStatusCompleted;
        }
    };

    MetalDevice::MetalDevice()
        : m_impl(new Impl())
    {
    }

    MetalDevice::~MetalDevice()
    {
        if(m_impl == nullptr) return;

        m_impl->StopDrawableAcquisition();
        if(!m_impl->submissions.empty() && !m_impl->DrainGpuForShutdown())
        {
            m_impl = nullptr;
            return;
        }

        m_impl->activeCommandLists.clear();

        m_impl->ClearCurrentDrawable();
        for(Impl::Submission& submission : m_impl->submissions)
            Impl::ReleaseSubmission(submission);
        m_impl->submissions.clear();
		m_impl->retiredResourceSets.clear();
		m_impl->liveResourceSets.clear();
		m_impl->retiredPipelines.clear();
		m_impl->livePipelines.clear();
		m_impl->retiredShaders.clear();
		m_impl->liveShaders.clear();
		m_impl->retiredTextures.clear();
		m_impl->liveTextures.clear();
		m_impl->retiredBuffers.clear();
		m_impl->liveBuffers.clear();

        if(m_impl->backBufferTex != nullptr)
        {
            RHI::TextureDesc desc = m_impl->backBufferTex->GetDesc();
            m_impl->backBufferTex->SetBackBuffer(nullptr, desc);
        }
        delete m_impl->backBufferTex;
        m_impl->backBufferTex = nullptr;

        if(m_impl->metalLayer != nil)
            m_impl->metalLayer.device = nil;
#if !__has_feature(objc_arc)
        [m_impl->metalLayer release];
        if(m_impl->drawableQueue != nullptr)
            dispatch_release(m_impl->drawableQueue);
        [m_impl->commandQueue release];
        [m_impl->device release];
#endif
        m_impl->metalLayer = nil;
        m_impl->drawableQueue = nullptr;
        m_impl->commandQueue = nil;
        m_impl->device = nil;
        delete m_impl;
    }

    int MetalDevice::Initialize(const void* windowHandle, const RHI::DeviceDesc& desc)
    {
        if(windowHandle == nullptr || desc.maxFramesInFlight == 0)
            return -1;

        m_impl->windowHandle = windowHandle;
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if(device == nil) return -1;
        m_impl->device = device;

        m_impl->commandQueue = [device newCommandQueue];
        if(m_impl->commandQueue == nil) return -1;

        m_impl->frameSlots.resize(desc.maxFramesInFlight);
        m_impl->activeCommandLists.reserve(desc.maxFramesInFlight);
        m_impl->submissions.reserve(desc.maxFramesInFlight);
        return 0;
    }

    bool MetalDevice::CreateSwapchain(const RHI::SwapchainDesc& desc)
    {
        if(m_impl->device == nil || m_impl->commandQueue == nil ||
            m_impl->windowHandle == nullptr || m_impl->metalLayer != nil ||
            m_impl->backBufferTex != nullptr || ![NSThread isMainThread] ||
            desc.minimumImageCount == 0)
        {
            return false;
        }

        NSWindow* window = (__bridge NSWindow*)m_impl->windowHandle;
        NSView* contentView = window.contentView;
        if(contentView == nil) return false;

        CAMetalLayer* layer = [CAMetalLayer layer];
        if(layer == nil) return false;
        layer.device = m_impl->device;

        if(desc.format != RHI::Format::Unknown)
        {
            const MTLPixelFormat requestedFormat = ToLayerPixelFormat(desc.format);
            if(requestedFormat == MTLPixelFormatInvalid) return false;
            layer.pixelFormat = requestedFormat;
            if(layer.pixelFormat != requestedFormat) return false;
        }

        if(@available(macOS 10.13.2, *))
        {
            layer.allowsNextDrawableTimeout = YES;
            switch(desc.presentMode)
            {
            case RHI::PresentMode::Fifo:
                layer.displaySyncEnabled = YES;
                break;
            case RHI::PresentMode::Immediate:
                layer.displaySyncEnabled = NO;
                break;
            case RHI::PresentMode::Mailbox:
            default:
                return false;
            }
            if((desc.presentMode == RHI::PresentMode::Fifo) !=
                (layer.displaySyncEnabled != NO))
            {
                return false;
            }

            if(layer.maximumDrawableCount < desc.minimumImageCount)
                layer.maximumDrawableCount = desc.minimumImageCount;
            if(layer.maximumDrawableCount < desc.minimumImageCount) return false;
        }
        else
        {
            return false;
        }

        const CGFloat scale = window.backingScaleFactor;
        layer.contentsScale = scale;
        layer.frame = contentView.bounds;
        layer.drawableSize = CGSizeMake(
            contentView.bounds.size.width * scale,
            contentView.bounds.size.height * scale);

        const RHI::Format actualFormat = FromLayerPixelFormat(layer.pixelFormat);
        if((desc.initialWidth && desc.initialWidth != layer.drawableSize.width) ||
            (desc.initialHeight && desc.initialHeight != layer.drawableSize.height)) return false;
        if(actualFormat == RHI::Format::Unknown) return false;

        RHI::TextureDesc backBufferDesc = {};
        backBufferDesc.width = static_cast<uint32_t>(layer.drawableSize.width);
        backBufferDesc.height = static_cast<uint32_t>(layer.drawableSize.height);
        backBufferDesc.depthOrArraySize = 1;
        backBufferDesc.mipLevels = 1;
        backBufferDesc.format = actualFormat;
        backBufferDesc.usage = RHI::TextureUsage::RenderTarget;

        auto backBuffer = std::unique_ptr<MetalTexture, MetalObjectDeleter>(
            new MetalTexture(backBufferDesc));
        dispatch_queue_t drawableQueue = dispatch_queue_create(
            "dy.engine.metal.drawable", DISPATCH_QUEUE_SERIAL);
        if(drawableQueue == nullptr) return false;

        [contentView setWantsLayer:YES];
        [contentView setLayer:layer];
#if !__has_feature(objc_arc)
        [layer retain];
#endif
        m_impl->metalLayer = layer;
        layer.framebufferOnly = desc.allowReadback ? NO : YES;
        m_impl->allowReadback = desc.allowReadback;
        m_impl->backBufferTex = backBuffer.release();
        m_impl->drawableQueue = drawableQueue;
        m_impl->RequestDrawable();
        return true;
    }

    bool MetalDevice::BeginFrame()
    {
        m_impl->CollectCompletedSubmissions();
        if(m_impl->asyncWorkFailed ||
            m_impl->pendingPresentValue != 0 || m_impl->metalLayer == nil ||
            m_impl->backBufferTex == nullptr || m_impl->frameSlots.empty() ||
            m_impl->windowHandle == nullptr || ![NSThread isMainThread])
        {
            return false;
        }
		if(m_impl->frameActive)
		{
			return m_impl->currentDrawable != nil &&
				m_impl->backBufferTex->GetNativeTexture() != nullptr;
		}

        NSWindow* window = (__bridge NSWindow*)m_impl->windowHandle;
        NSView* contentView = window.contentView;
        if(contentView == nil) return false;

        Impl::FrameSlot& slot = m_impl->frameSlots[m_impl->nextFrameSlot];
        if(slot.submissionValue > m_impl->completedSubmissionValue)
            return false;

        id<CAMetalDrawable> drawable = nil;
        uint32_t expectedWidth = 0;
        uint32_t expectedHeight = 0;
        {
            std::unique_lock<std::mutex> lock(
                m_impl->drawableMutex, std::try_to_lock);
            if(!lock.owns_lock()) return false;

            const CGFloat scale = window.backingScaleFactor;
            const CGRect bounds = contentView.bounds;
            const CGSize drawableSize = CGSizeMake(
                bounds.size.width * scale,
                bounds.size.height * scale);
            m_impl->metalLayer.contentsScale = scale;
            m_impl->metalLayer.frame = bounds;
            m_impl->metalLayer.drawableSize = drawableSize;
            if(m_impl->metalLayer.drawableSize.width <= 0.0 ||
                m_impl->metalLayer.drawableSize.height <= 0.0)
            {
                return false;
            }

            expectedWidth = static_cast<uint32_t>(
                m_impl->metalLayer.drawableSize.width);
            expectedHeight = static_cast<uint32_t>(
                m_impl->metalLayer.drawableSize.height);
            drawable = m_impl->readyDrawable;
            m_impl->readyDrawable = nil;
        }
        m_impl->RequestDrawable();
        if(drawable == nil)
        {
            return false;
        }

        m_impl->currentDrawable = drawable;

        id<MTLTexture> texture = drawable.texture;
        const RHI::Format actualFormat = texture == nil
            ? RHI::Format::Unknown
            : FromLayerPixelFormat(texture.pixelFormat);
        if(texture == nil || texture.width == 0 || texture.height == 0 ||
            texture.width != expectedWidth || texture.height != expectedHeight ||
            actualFormat == RHI::Format::Unknown ||
            actualFormat != m_impl->backBufferTex->GetDesc().format)
        {
            m_impl->ClearCurrentDrawable();
            return false;
        }

        RHI::TextureDesc backBufferDesc = m_impl->backBufferTex->GetDesc();
        backBufferDesc.width = static_cast<uint32_t>(texture.width);
        backBufferDesc.height = static_cast<uint32_t>(texture.height);
        backBufferDesc.format = actualFormat;
        m_impl->backBufferTex->SetBackBuffer(
            (__bridge void*)texture, backBufferDesc);

        m_impl->activeFrameSlot = m_impl->nextFrameSlot;
        m_impl->frameActive = true;
        return true;
    }

    RHI::ICommandList* MetalDevice::AcquireCommandList()
    {
        m_impl->CollectCompletedSubmissions();
        if(m_impl->device == nil || m_impl->commandQueue == nil ||
            m_impl->asyncWorkFailed)
        {
            return nullptr;
        }

        auto commandList = std::unique_ptr<MetalCommandList, MetalObjectDeleter>(
            new MetalCommandList((__bridge void*)m_impl->commandQueue));
        if(!commandList->Begin()) return nullptr;

        MetalCommandList* result = commandList.get();
        m_impl->activeCommandLists.push_back(std::move(commandList));
        return result;
    }

    bool MetalDevice::Submit(RHI::ICommandList** cmdLists, uint32_t count)
    {
        if(cmdLists == nullptr || count == 0)
        {
            return false;
        }

        std::vector<MetalCommandList*> submittedCommandLists;
        submittedCommandLists.reserve(count);
        for(uint32_t index = 0; index < count; ++index)
        {
            if(cmdLists[index] == nullptr) return false;
            for(uint32_t previous = 0; previous < index; ++previous)
            {
                if(cmdLists[previous] == cmdLists[index]) return false;
            }

            const auto owned = std::find_if(
                m_impl->activeCommandLists.begin(),
                m_impl->activeCommandLists.end(),
                [command = cmdLists[index]](
                    const std::unique_ptr<MetalCommandList, MetalObjectDeleter>& candidate)
                {
                    return candidate.get() == command;
                });
            if(owned == m_impl->activeCommandLists.end()) return false;

            MetalCommandList* commandList = owned->get();
			if(!commandList->IsClosed()) return false;
            submittedCommandLists.push_back(commandList);
        }

		std::vector<std::unique_ptr<MetalCommandList, MetalObjectDeleter>> consumedCommandLists;
		consumedCommandLists.reserve(count);
		for(MetalCommandList* commandList : submittedCommandLists)
		{
			const auto owned = std::find_if(
				m_impl->activeCommandLists.begin(),
				m_impl->activeCommandLists.end(),
				[commandList](
					const std::unique_ptr<MetalCommandList, MetalObjectDeleter>& candidate)
				{
					return candidate.get() == commandList;
				});
			consumedCommandLists.push_back(std::move(*owned));
			m_impl->activeCommandLists.erase(owned);
		}

		bool usesBackBuffer = false;
		for(MetalCommandList* commandList : submittedCommandLists)
		{
			if(!commandList->IsValid() ||
				commandList->GetNativeCommandBuffer() == nullptr)
			{
				return false;
			}
			usesBackBuffer = usesBackBuffer || commandList->UsesBackBuffer();
		}
		if(m_impl->asyncWorkFailed) return false;

        if(usesBackBuffer &&
            (!m_impl->frameActive || m_impl->currentDrawable == nil ||
                m_impl->backBufferTex == nullptr || m_impl->frameSlots.empty()))
        {
            return false;
        }

		MetalSubmissionState resourceStates = {};
		for(MetalCommandList* commandList : submittedCommandLists)
		{
			if(!commandList->ValidateForSubmit(resourceStates)) return false;
		}
		if(usesBackBuffer)
		{
			const auto key = std::make_pair(m_impl->backBufferTex, 0u);
			const auto found = resourceStates.textureSubresources.find(key);
			const RHI::ResourceState finalState =
				found == resourceStates.textureSubresources.end()
				? m_impl->backBufferTex->GetState(0, 0) : found->second;
			if(finalState != RHI::ResourceState::Present) return false;
		}

        m_impl->submissions.emplace_back();
        Impl::Submission& submission = m_impl->submissions.back();
        submission.value = m_impl->nextSubmissionValue++;
		submission.commandLists = std::move(consumedCommandLists);

        for(const std::unique_ptr<MetalCommandList, MetalObjectDeleter>& commandList :
            submission.commandLists)
        {
			commandList->CommitResourceStates();
            id<MTLCommandBuffer> commandBuffer =
                (__bridge id<MTLCommandBuffer>)
                    commandList->GetNativeCommandBuffer();
            [commandBuffer commit];
        }
		m_impl->lastSubmittedValue = submission.value;

        if(usesBackBuffer)
        {
            submission.drawable = m_impl->currentDrawable;
            submission.waitingForPresent = true;
            m_impl->currentDrawable = nil;

            m_impl->frameSlots[m_impl->activeFrameSlot].submissionValue =
                submission.value;
            m_impl->pendingPresentValue = submission.value;
            m_impl->nextFrameSlot = static_cast<uint32_t>(
                (m_impl->activeFrameSlot + 1) % m_impl->frameSlots.size());
            m_impl->frameActive = false;

            const RHI::TextureDesc backBufferDesc =
                m_impl->backBufferTex->GetDesc();
            m_impl->backBufferTex->SetBackBuffer(nullptr, backBufferDesc);
        }
        return true;
    }

    void MetalDevice::Present()
    {
        if(m_impl->pendingPresentValue == 0) return;

        Impl::Submission* submission = m_impl->FindSubmission(
            m_impl->pendingPresentValue);
        m_impl->pendingPresentValue = 0;
        if(submission == nullptr)
        {
            m_impl->asyncWorkFailed = true;
            return;
        }
        if(submission->drawable == nil)
        {
            submission->waitingForPresent = false;
            m_impl->asyncWorkFailed = true;
            return;
        }

        id<MTLCommandBuffer> commandBuffer =
            [m_impl->commandQueue commandBuffer];
        if(commandBuffer == nil)
        {
            submission->waitingForPresent = false;
            m_impl->asyncWorkFailed = true;
            return;
        }
#if !__has_feature(objc_arc)
        [commandBuffer retain];
#endif
        submission->presentCommandBuffer = commandBuffer;
        [commandBuffer presentDrawable:submission->drawable];
        [commandBuffer commit];
        submission->waitingForPresent = false;
    }

    RHI::BufferHandle MetalDevice::CreateBuffer(const RHI::BufferDesc& desc)
    {
		auto buffer = std::unique_ptr<MetalBuffer, MetalObjectDeleter>(
			new MetalBuffer(desc, (__bridge void*)m_impl->device));
		if(buffer->GetNativeBuffer() == nullptr) return nullptr;
		MetalBuffer* result = buffer.get();
		m_impl->liveBuffers.push_back(std::move(buffer));
		return result;
    }

    RHI::TextureHandle MetalDevice::CreateTexture(const RHI::TextureDesc& desc)
    {
		auto texture = std::unique_ptr<MetalTexture, MetalObjectDeleter>(
			new MetalTexture(desc, (__bridge void*)m_impl->device));
		if(texture->GetNativeTexture() == nullptr) return nullptr;
		MetalTexture* result = texture.get();
		m_impl->liveTextures.push_back(std::move(texture));
		return result;
    }

	RHI::ShaderHandle MetalDevice::CreateShader(const RHI::ShaderDesc& desc)
	{
		auto shader = std::unique_ptr<MetalShader, MetalObjectDeleter>(
			new MetalShader(desc, (__bridge void*)m_impl->device));
		if(shader->GetNativeFunction() == nullptr) return nullptr;
		MetalShader* result = shader.get();
		m_impl->liveShaders.push_back(std::move(shader));
		return result;
	}

    RHI::PipelineHandle MetalDevice::CreateGraphicsPipeline(
        const RHI::GraphicsPipelineDesc& desc)
    {
		const auto ownsShader = [this](RHI::ShaderHandle shader)
		{
			return std::find_if(
				m_impl->liveShaders.begin(), m_impl->liveShaders.end(),
				[shader](const std::unique_ptr<MetalShader, MetalObjectDeleter>& candidate)
				{
					return static_cast<RHI::ShaderHandle>(candidate.get()) == shader;
				}) != m_impl->liveShaders.end();
		};
		if(!ownsShader(desc.vertexShader) ||
			(desc.fragmentShader != nullptr && !ownsShader(desc.fragmentShader)))
		{
			return nullptr;
		}
        auto pipeline = std::unique_ptr<MetalPipeline, MetalObjectDeleter>(
            new MetalPipeline(desc, (__bridge void*)m_impl->device));
		if(pipeline->GetNativePipeline() == nullptr ||
			(desc.depthStencil.format != RHI::Format::Unknown &&
				pipeline->GetNativeDepthStencil() == nullptr)) return nullptr;
		MetalPipeline* result = pipeline.get();
		m_impl->livePipelines.push_back(std::move(pipeline));
		return result;
    }

	RHI::ResourceSetHandle MetalDevice::CreateResourceSet(
		const RHI::ResourceSetDesc& desc)
	{
		const auto pipelineIt = std::find_if(
			m_impl->livePipelines.begin(), m_impl->livePipelines.end(),
			[requested = desc.pipeline](
				const std::unique_ptr<MetalPipeline, MetalObjectDeleter>& candidate)
			{
				return static_cast<RHI::PipelineHandle>(candidate.get()) == requested;
			});
		if(pipelineIt == m_impl->livePipelines.end() ||
			(desc.bindingCount != 0 && desc.bindings == nullptr))
		{
			return nullptr;
		}

		const RHI::PipelineLayoutDesc& layout = (*pipelineIt)->GetLayout();
		std::set<std::pair<uint32_t, uint32_t>> populated;
		uint32_t textureBindingCount = 0;
		for(uint32_t index = 0; index < desc.bindingCount; ++index)
		{
			const RHI::ResourceBinding& binding = desc.bindings[index];
			const RHI::ResourceBindingLayout* declaration = nullptr;
			for(uint32_t layoutIndex = 0; layoutIndex < layout.bindingCount; ++layoutIndex)
			{
				if(layout.bindings[layoutIndex].binding == binding.binding)
				{
					declaration = &layout.bindings[layoutIndex];
					break;
				}
			}
			if(declaration == nullptr ||
				declaration->type == RHI::ResourceBindingType::StaticSampler ||
				binding.arrayElement >= declaration->count ||
				!populated.emplace(binding.binding, binding.arrayElement).second)
			{
				return nullptr;
			}

			switch(declaration->type)
			{
			case RHI::ResourceBindingType::ConstantBuffer:
			case RHI::ResourceBindingType::ReadOnlyStorageBuffer:
			case RHI::ResourceBindingType::ReadWriteStorageBuffer:
			{
				const auto bufferIt = std::find_if(
					m_impl->liveBuffers.begin(), m_impl->liveBuffers.end(),
					[requested = binding.buffer](
						const std::unique_ptr<MetalBuffer, MetalObjectDeleter>& candidate)
					{
						return static_cast<RHI::BufferHandle>(candidate.get()) == requested;
					});
				if(bufferIt == m_impl->liveBuffers.end() || binding.texture != nullptr)
					return nullptr;
				MetalBuffer* buffer = bufferIt->get();
				const RHI::BufferUsage required =
					declaration->type == RHI::ResourceBindingType::ConstantBuffer
					? RHI::BufferUsage::Constant : RHI::BufferUsage::Storage;
				if(buffer->GetNativeBuffer() == nullptr || binding.size == 0 ||
					(buffer->GetDesc().usage & required) == RHI::BufferUsage::None ||
					binding.offset > buffer->GetDesc().size ||
					binding.size > buffer->GetDesc().size - binding.offset)
				{
					return nullptr;
				}
				break;
			}
			case RHI::ResourceBindingType::SampledTexture:
			case RHI::ResourceBindingType::StorageTexture:
			{
				const auto textureIt = std::find_if(
					m_impl->liveTextures.begin(), m_impl->liveTextures.end(),
					[requested = binding.texture](
						const std::unique_ptr<MetalTexture, MetalObjectDeleter>& candidate)
					{
						return static_cast<RHI::TextureHandle>(candidate.get()) == requested;
					});
				if(textureIt == m_impl->liveTextures.end() || binding.buffer != nullptr)
					return nullptr;
				MetalTexture* texture = textureIt->get();
				const RHI::TextureUsage required =
					declaration->type == RHI::ResourceBindingType::SampledTexture
					? RHI::TextureUsage::ShaderResource : RHI::TextureUsage::Storage;
				const RHI::TextureDesc& textureDesc = texture->GetDesc();
				if(texture->GetNativeTexture() == nullptr ||
					(textureDesc.usage & required) == RHI::TextureUsage::None ||
					binding.subresources.firstMipLevel >= textureDesc.mipLevels ||
					binding.subresources.firstArrayLayer >= textureDesc.depthOrArraySize)
				{
					return nullptr;
				}
				const uint32_t mipCount = binding.subresources.mipLevelCount == 0
					? textureDesc.mipLevels - binding.subresources.firstMipLevel
					: binding.subresources.mipLevelCount;
				const uint32_t layerCount = binding.subresources.arrayLayerCount == 0
					? textureDesc.depthOrArraySize - binding.subresources.firstArrayLayer
					: binding.subresources.arrayLayerCount;
				if(mipCount > textureDesc.mipLevels - binding.subresources.firstMipLevel ||
					layerCount > textureDesc.depthOrArraySize - binding.subresources.firstArrayLayer ||
					(declaration->type == RHI::ResourceBindingType::StorageTexture && mipCount != 1))
				{
					return nullptr;
				}
				++textureBindingCount;
				break;
			}
			default:
				return nullptr;
			}
		}

		for(uint32_t index = 0; index < layout.bindingCount; ++index)
		{
			const RHI::ResourceBindingLayout& declaration = layout.bindings[index];
			if(declaration.type == RHI::ResourceBindingType::StaticSampler) continue;
			for(uint32_t element = 0; element < declaration.count; ++element)
			{
				if(populated.find({declaration.binding, element}) == populated.end())
					return nullptr;
			}
		}
		auto resourceSet = std::unique_ptr<MetalResourceSet, MetalObjectDeleter>(
			new MetalResourceSet(desc));
		if(resourceSet->GetTextureBindings().size() != textureBindingCount) return nullptr;
		MetalResourceSet* result = resourceSet.get();
		m_impl->liveResourceSets.push_back(std::move(resourceSet));
		return result;
	}

    void MetalDevice::DestroyBuffer(RHI::BufferHandle buffer)
    {
		if(RetireObject(
			m_impl->liveBuffers,
			buffer,
			m_impl->lastSubmittedValue,
			m_impl->retiredBuffers))
		{
			m_impl->CollectCompletedSubmissions();
		}
    }

    void MetalDevice::DestroyTexture(RHI::TextureHandle texture)
    {
		if(RetireObject(
			m_impl->liveTextures,
			texture,
			m_impl->lastSubmittedValue,
			m_impl->retiredTextures))
		{
			m_impl->CollectCompletedSubmissions();
		}
    }

	void MetalDevice::DestroyShader(RHI::ShaderHandle shader)
	{
		if(RetireObject(
			m_impl->liveShaders,
			shader,
			m_impl->lastSubmittedValue,
			m_impl->retiredShaders))
		{
			m_impl->CollectCompletedSubmissions();
		}
	}

    void MetalDevice::DestroyPipeline(RHI::PipelineHandle pipeline)
    {
		if(RetireObject(
			m_impl->livePipelines,
			pipeline,
			m_impl->lastSubmittedValue,
			m_impl->retiredPipelines))
		{
			m_impl->CollectCompletedSubmissions();
		}
    }

	void MetalDevice::DestroyResourceSet(RHI::ResourceSetHandle resourceSet)
	{
		if(RetireObject(
			m_impl->liveResourceSets,
			resourceSet,
			m_impl->lastSubmittedValue,
			m_impl->retiredResourceSets))
		{
			m_impl->CollectCompletedSubmissions();
		}
	}

	bool MetalDevice::UpdateBuffer(
		RHI::ICommandList& commandList,
		RHI::BufferHandle buffer,
		uint32_t offset,
		const void* data,
		uint32_t size)
    {
		const auto ownedCommandList = std::find_if(
			m_impl->activeCommandLists.begin(), m_impl->activeCommandLists.end(),
			[requested = &commandList](
				const std::unique_ptr<MetalCommandList, MetalObjectDeleter>& candidate)
			{
				return static_cast<RHI::ICommandList*>(candidate.get()) == requested;
			});
		const auto ownedBuffer = std::find_if(
			m_impl->liveBuffers.begin(), m_impl->liveBuffers.end(),
			[buffer](const std::unique_ptr<MetalBuffer, MetalObjectDeleter>& candidate)
			{
				return static_cast<RHI::BufferHandle>(candidate.get()) == buffer;
			});
		return ownedCommandList != m_impl->activeCommandLists.end() &&
			ownedBuffer != m_impl->liveBuffers.end() &&
			(*ownedCommandList)->RecordBufferUpdate(
				ownedBuffer->get(), offset, data, size);
    }

	bool MetalDevice::UpdateTexture(
		RHI::ICommandList& commandList,
		RHI::TextureHandle texture,
		uint32_t mipLevel,
		uint32_t arrayLayer,
		const void* data,
		uint32_t dataSize,
		uint32_t rowPitch,
		uint32_t slicePitch)
    {
		const auto ownedCommandList = std::find_if(
			m_impl->activeCommandLists.begin(), m_impl->activeCommandLists.end(),
			[requested = &commandList](
				const std::unique_ptr<MetalCommandList, MetalObjectDeleter>& candidate)
			{
				return static_cast<RHI::ICommandList*>(candidate.get()) == requested;
			});
		const auto ownedTexture = std::find_if(
			m_impl->liveTextures.begin(), m_impl->liveTextures.end(),
			[texture](const std::unique_ptr<MetalTexture, MetalObjectDeleter>& candidate)
			{
				return static_cast<RHI::TextureHandle>(candidate.get()) == texture;
			});
		return ownedCommandList != m_impl->activeCommandLists.end() &&
			ownedTexture != m_impl->liveTextures.end() &&
			(*ownedCommandList)->RecordTextureUpdate(
				ownedTexture->get(),
				mipLevel,
				arrayLayer,
				data,
				dataSize,
				rowPitch,
				slicePitch);
    }

    RHI::TextureHandle MetalDevice::GetBackBuffer()
    {
        return m_impl->metalLayer == nil ? nullptr : m_impl->backBufferTex;
    }

    bool MetalDevice::ReadTexture(RHI::TextureHandle texture, RHI::TextureReadback& result)
    {
        if(!m_impl || !texture || m_impl->asyncWorkFailed || !m_impl->activeCommandLists.empty()) return false;
        const bool backBuffer = texture == m_impl->backBufferTex;
        id<MTLTexture> image = nil;
        if(backBuffer)
        {
            if(!m_impl->allowReadback || !m_impl->pendingPresentValue) return false;
            const auto* submission = m_impl->FindSubmission(m_impl->pendingPresentValue);
            if(!submission || !submission->waitingForPresent || submission->drawable == nil) return false;
            image = submission->drawable.texture;
        }
        else
        {
            const auto found = std::find_if(m_impl->liveTextures.begin(), m_impl->liveTextures.end(),
                [texture](const auto& candidate) { return candidate.get() == texture; });
            if(found == m_impl->liveTextures.end() ||
                (*found)->GetState(0, 0) == RHI::ResourceState::Undefined) return false;
            image = (__bridge id<MTLTexture>)(*found)->GetNativeTexture();
        }
        const auto& desc = texture->GetDesc();
        if(image == nil || !RHI::IsReadbackFormat(desc.format) || !desc.width || !desc.height ||
            desc.width > UINT32_MAX / 4u) return false;
        const uint64_t bytes = static_cast<uint64_t>(desc.width) * desc.height * 4u;
        // Metal's texture-to-buffer blit requires a 256-byte row alignment.
        const uint64_t pitch = (static_cast<uint64_t>(desc.width) * 4u + 255u) & ~uint64_t(255u);
        if(bytes > std::numeric_limits<size_t>::max() ||
            pitch > std::numeric_limits<NSUInteger>::max() / desc.height) return false;
        RHI::TextureReadback output;
        output.width = desc.width; output.height = desc.height;
        output.rowPitch = desc.width * 4u; output.format = desc.format;
        output.pixels.resize(static_cast<size_t>(bytes));
        @autoreleasepool
        {
            id<MTLBuffer> buffer = [m_impl->device newBufferWithLength:static_cast<NSUInteger>(pitch * desc.height)
                options:MTLResourceStorageModeShared];
            if(buffer == nil) return false;
            id<MTLCommandBuffer> command = [m_impl->commandQueue commandBuffer];
            id<MTLBlitCommandEncoder> blit = [command blitCommandEncoder];
            if(command == nil || blit == nil)
            {
#if !__has_feature(objc_arc)
                [buffer release];
#endif
                return false;
            }
            [blit copyFromTexture:image sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
                sourceSize:MTLSizeMake(desc.width, desc.height, 1) toBuffer:buffer destinationOffset:0
                destinationBytesPerRow:static_cast<NSUInteger>(pitch)
                destinationBytesPerImage:static_cast<NSUInteger>(pitch * desc.height)];
            [blit endEncoding];
            [command commit];
            [command waitUntilCompleted];
            const bool succeeded = command.status == MTLCommandBufferStatusCompleted && buffer.contents != nullptr;
            if(succeeded)
                for(uint32_t row = 0; row < desc.height; ++row)
                    std::memcpy(output.pixels.data() + static_cast<size_t>(row) * output.rowPitch,
                        static_cast<const uint8_t*>(buffer.contents) + static_cast<size_t>(row * pitch), output.rowPitch);
#if !__has_feature(objc_arc)
            [buffer release];
#endif
            if(!succeeded) return false;
        }
        result = std::move(output);
        return true;
    }
}
