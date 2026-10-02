#include "MetalDevice.h"
#include "MetalBuffer.h"
#include "MetalTexture.h"
#include "MetalPipeline.h"
#include "MetalCommandList.h"
#include "MetalResourceSet.h"
#include "MetalShader.h"
#include "dyf/RHI/Readback.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <set>
#include <vector>

#import <AppKit/AppKit.h>
#import <CoreGraphics/CoreGraphics.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <dispatch/dispatch.h>

namespace dyf::Backends
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

    }

    struct MetalDevice::Impl
    {
        struct Submission
        {
            uint64_t value = 0;
            std::vector<std::unique_ptr<MetalCommandList, MetalObjectDeleter>> commandLists;
            id<MTLCommandBuffer> presentCommandBuffer = nil;
            id<CAMetalDrawable> drawable = nil;
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

        std::vector<uint64_t> frameSlots;
        std::vector<std::unique_ptr<MetalCommandList, MetalObjectDeleter>> activeCommandLists;
		std::vector<std::unique_ptr<MetalBuffer, MetalObjectDeleter>> liveBuffers;
		std::vector<std::unique_ptr<MetalTexture, MetalObjectDeleter>> liveTextures;
		std::vector<std::unique_ptr<MetalShader, MetalObjectDeleter>> liveShaders;
		std::vector<std::unique_ptr<MetalPipeline, MetalObjectDeleter>> livePipelines;
		std::vector<std::unique_ptr<MetalResourceSet, MetalObjectDeleter>> liveResourceSets;
        uint32_t nextFrameSlot = 0;
        uint32_t activeFrameSlot = 0;
        bool frameActive = false;
        bool allowReadback = false;

        std::vector<Submission> submissions;
        uint64_t nextSubmissionValue = 1;
        uint64_t completedSubmissionValue = 0;
		uint64_t lastSubmittedValue = 0;
        uint64_t frameLastSubmissionValue = 0;
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
                        std::fprintf(stderr, "%s", [[NSString stringWithFormat:
                            @"Metal submission failed: %@\n", commandBuffer.error] UTF8String]);
                    }
                }
                if(submission.presentCommandBuffer.status ==
                    MTLCommandBufferStatusError)
                {
                    asyncWorkFailed = true;
                    std::fprintf(stderr, "%s", [[NSString stringWithFormat:
                        @"Metal presentation failed: %@\n", submission.presentCommandBuffer.error] UTF8String]);
                }

                completedSubmissionValue = submission.value;
                ReleaseSubmission(submission);
                submissions.erase(submissions.begin());
            }
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
            AbandonResources();
            m_impl = nullptr;
            return;
        }

        ReleaseResources();
        m_impl->activeCommandLists.clear();

        m_impl->ClearCurrentDrawable();
        for(Impl::Submission& submission : m_impl->submissions)
            Impl::ReleaseSubmission(submission);
        m_impl->submissions.clear();
		m_impl->liveResourceSets.clear();
		m_impl->livePipelines.clear();
		m_impl->liveShaders.clear();
		m_impl->liveTextures.clear();
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
        if(desc.maxFramesInFlight == 0 || desc.enableValidation)
            return -1;

        m_impl->windowHandle = windowHandle;
        NSArray<id<MTLDevice>>* adapters=MTLCopyAllDevices();
        if(desc.adapterIndex>=adapters.count) {
#if !__has_feature(objc_arc)
            [adapters release];
#endif
            return -1;
        }
        id<MTLDevice> device=adapters[desc.adapterIndex];
#if !__has_feature(objc_arc)
        [device retain];
        [adapters release];
#endif
        m_impl->device = device;

        m_impl->commandQueue = [device newCommandQueue];
        if(m_impl->commandQueue == nil) return -1;

        m_impl->frameSlots.resize(desc.maxFramesInFlight);
        m_impl->activeCommandLists.reserve(desc.maxFramesInFlight);
        m_impl->submissions.reserve(desc.maxFramesInFlight);
        return 0;
    }


uint64_t MetalDevice::GetLastSubmissionNative() const {return m_impl->lastSubmittedValue;}
uint64_t MetalDevice::GetCompletedSubmissionNative()
{
    // Present의 마무리 커맨드도 동일 제출열에서 완료 여부를 판정한다.
    m_impl->CollectCompletedSubmissions();
    return m_impl->asyncWorkFailed ? 0 : m_impl->completedSubmissionValue;
}
void MetalDevice::DiscardCommandListNative(RHI::ICommandList* list) {auto& active=m_impl->activeCommandLists; active.erase(std::remove_if(active.begin(),active.end(),[list](const auto& value){return value.get()==list;}),active.end());}

bool MetalDevice::SupportsNative(RHI::Feature feature) const
{
    if(!m_impl || m_impl->device == nil) return false;
    switch(feature)
    {
    case RHI::Feature::Rasterization:
    case RHI::Feature::Tessellation:
    case RHI::Feature::DescriptorIndexing:
    case RHI::Feature::FractionalDepthBias:
    case RHI::Feature::Wireframe:
    case RHI::Feature::DepthBiasClamp:
        return true;
    // 이 구현은 shader bias 인자를 추가하지 않으므로 sampler LOD bias는 지원하지 않는다.
    default: return false;
    }
}

uint64_t MetalDevice::GetLimitNative(RHI::Limit limit) const
{
    if(!m_impl || m_impl->device == nil) return 0;
    id<MTLDevice> device = m_impl->device;
    switch(limit)
    {
    case RHI::Limit::InlineConstantBytes:
    case RHI::Limit::UniformBufferBytes:
    case RHI::Limit::StorageBufferBytes:
        // inline constants도 setBytes가 아닌 MTLBuffer로 전달하므로 setBytes의 4KB 제한은 적용하지 않는다.
        if(@available(macOS 10.14, *))
            return static_cast<uint64_t>(device.maxBufferLength);
        // 조회 API 이전 OS의 한도는 과거 feature table을 보존한 Dawn 구현과 대조했다.
        // https://dawn.googlesource.com/dawn/+/41e4d9a34c1d9dcb2eef3ff39ff9c1f987bfa02a/src/dawn/native/metal/BufferMTL.mm
        if(@available(macOS 10.12, *)) return 1024ull * 1024 * 1024;
        return 256ull * 1024 * 1024;
    case RHI::Limit::Texture2DDimension:
        // Apple의 GPU family 표: macOS GPU 및 Apple3~9는 16384, Apple10은 32768이다.
        // https://developer.apple.com/metal/Metal-Feature-Set-Tables.pdf
#if defined(__MAC_OS_X_VERSION_MAX_ALLOWED) && __MAC_OS_X_VERSION_MAX_ALLOWED >= 260000
        if(@available(macOS 26.0, *))
            if([device supportsFamily:MTLGPUFamilyApple10]) return 32768;
#endif
        return 16384;
    case RHI::Limit::UniformBufferOffsetAlignment:
        // Apple GPU의 constant 정렬은 4B, Mac2의 정렬은 32B다. 장치 family를 확인한다.
        if(@available(macOS 11.0, *))
            if([device supportsFamily:MTLGPUFamilyApple1]) return 4;
        if(@available(macOS 10.15, *))
            if([device supportsFamily:MTLGPUFamilyMac2]) return 32;
        // Mac1은 과거 Apple feature table을 보존한 Dawn의 family 표에서 256B로 확인된다.
        // https://dawn.googlesource.com/dawn/+/4a845f198685328fc4881288fc6c8ea570c7176d/src/dawn/native/metal/PhysicalDeviceMTL.mm
        return 256;
    case RHI::Limit::StorageBufferOffsetAlignment:
        // device 주소공간은 고정 장치 정렬이 없다. shader 데이터 타입의 ABI 정렬은 호출자가 맞춘다.
        return 1;
    case RHI::Limit::SamplerAnisotropy:
        // MTLSamplerDescriptor.maxAnisotropy의 명시 범위는 1~16이다.
        return 16;
    case RHI::Limit::TessellationPatchControlPoints:
        return 32;
    default: return 0;
    }
}

bool MetalDevice::SupportsPipelineLayoutNative(const RHI::PipelineLayoutDesc& desc) const
{
    return m_impl && MetalPipeline::SupportsLayout(desc, (__bridge void*)m_impl->device);
}

bool MetalDevice::SupportsSamplerNative(const RHI::SamplerDesc& desc) const
{
    return m_impl && m_impl->device != nil && MetalPipeline::SupportsSampler(desc);
}

bool MetalDevice::SupportsGraphicsPipelineNative(const RHI::GraphicsPipelineDesc& desc) const
{
    return m_impl && MetalPipeline::SupportsGraphics(desc, (__bridge void*)m_impl->device);
}
bool MetalDevice::IsLostNative() const {return !m_impl || m_impl->asyncWorkFailed;}
bool MetalDevice::WaitIdleNative()
{
    if(!m_impl || !m_impl->DrainGpuForShutdown()) return false;
    m_impl->CollectCompletedSubmissions();
    return !m_impl->asyncWorkFailed;
}
void MetalDevice::DestroySwapchainNative()
{
    m_impl->StopDrawableAcquisition();
    m_impl->ClearCurrentDrawable();
    delete m_impl->backBufferTex;
    m_impl->backBufferTex = nullptr;
    if(m_impl->metalLayer != nil) m_impl->metalLayer.device = nil;
#if !__has_feature(objc_arc)
    [m_impl->metalLayer release];
    if(m_impl->drawableQueue != nullptr) dispatch_release(m_impl->drawableQueue);
#endif
    m_impl->metalLayer = nil;
    m_impl->drawableQueue = nullptr;
    m_impl->stoppingDrawableAcquisition = false;
    m_impl->frameActive = false;
    m_impl->frameLastSubmissionValue = 0;
}

    bool MetalDevice::CreateSwapchainNative(const RHI::SwapchainDesc& desc)
    {
        // CAMetalLayer는 2/3개만 지원한다. 범위 밖 값을 setter에 넘기면 예외를 유발할 수 있다.
        if(desc.minimumImageCount > 3)
        {
            ReportDiagnostic(DiagnosticSeverity::Error,
                "Metal swapchains support at most 3 drawable images; minimumImageCount exceeds 3.");
            return false;
        }
        m_impl->windowHandle=desc.window;
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

        // 창 합성 의미를 요청값으로 정한다. Core Animation은 투명 layer를 premultiplied로 합성한다.
        switch(desc.compositeAlpha)
        {
        case RHI::CompositeAlpha::Opaque: layer.opaque = YES; break;
        case RHI::CompositeAlpha::Premultiplied: layer.opaque = NO; break;
        default: return false;
        }
        CFStringRef colorSpaceName = nullptr;
        switch(desc.colorSpace)
        {
        case RHI::ColorSpace::Srgb: colorSpaceName = kCGColorSpaceSRGB; break;
        case RHI::ColorSpace::LinearSrgb: colorSpaceName = kCGColorSpaceLinearSRGB; break;
        default: return false;
        }
        CGColorSpaceRef colorSpace = CGColorSpaceCreateWithName(colorSpaceName);
        if(colorSpace == nullptr) return false;
        layer.colorspace = colorSpace;
        const bool colorSpaceAccepted = layer.colorspace != nullptr && CFEqual(layer.colorspace, colorSpace);
        CGColorSpaceRelease(colorSpace);
        if(!colorSpaceAccepted) return false;

        if(desc.format == RHI::Format::Unknown) return false;
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
        if(layer.maximumDrawableCount > desc.minimumImageCount)
        {
            char message[160];
            std::snprintf(message, sizeof(message),
                "Metal swapchain uses %llu drawable images for requested minimum %u.",
                static_cast<unsigned long long>(layer.maximumDrawableCount), desc.minimumImageCount);
            ReportDiagnostic(DiagnosticSeverity::Info, message);
        }
        return true;
    }

    bool MetalDevice::BeginFrameNative()
    {
        m_impl->CollectCompletedSubmissions();
        if(m_impl->asyncWorkFailed ||
            m_impl->metalLayer == nil ||
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

        if(m_impl->frameSlots[m_impl->nextFrameSlot] > m_impl->completedSubmissionValue)
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
        m_impl->frameLastSubmissionValue = 0;
        m_impl->frameActive = true;
        return true;
    }

    RHI::ICommandList* MetalDevice::AcquireCommandListNative()
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

    bool MetalDevice::SubmitNative(RHI::ICommandList** cmdLists, uint32_t count)
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

        // 같은 drawable을 여러 제출에서 사용할 수 있다. 프레임 종료는 Present가 맡는다.
        if(usesBackBuffer)
            m_impl->frameLastSubmissionValue = submission.value;
        return true;
    }

    bool MetalDevice::PresentNative()
    {
        if(!m_impl->frameActive || m_impl->currentDrawable == nil || m_impl->asyncWorkFailed)
            return false;

        // 이미 commit된 렌더 커맨드는 수정하지 않는다. 같은 queue 뒤에 표시 전용 커맨드를 제출한다.
        id<MTLCommandBuffer> commandBuffer = [m_impl->commandQueue commandBuffer];
        if(commandBuffer == nil)
        {
            m_impl->asyncWorkFailed = true;
            return false;
        }
        m_impl->submissions.emplace_back();
        Impl::Submission& submission = m_impl->submissions.back();
        submission.value = m_impl->nextSubmissionValue++;
        submission.presentCommandBuffer = commandBuffer;
        submission.drawable = m_impl->currentDrawable;
#if !__has_feature(objc_arc)
        [commandBuffer retain];
#endif
        [commandBuffer presentDrawable:submission.drawable];
        [commandBuffer commit];
        m_impl->lastSubmittedValue = submission.value;

        // drawable의 소유권은 표시 제출로 옮기고, frame slot은 표시 커맨드 완료까지 재사용하지 않는다.
        m_impl->currentDrawable = nil;
        m_impl->frameSlots[m_impl->activeFrameSlot] = submission.value;
        m_impl->nextFrameSlot = static_cast<uint32_t>(
            (m_impl->activeFrameSlot + 1) % m_impl->frameSlots.size());
        m_impl->frameActive = false;
        m_impl->frameLastSubmissionValue = 0;
        const RHI::TextureDesc backBufferDesc = m_impl->backBufferTex->GetDesc();
        m_impl->backBufferTex->SetBackBuffer(nullptr, backBufferDesc);
        return true;
    }

    RHI::BufferHandle MetalDevice::CreateBufferNative(const RHI::BufferDesc& desc)
    {
		auto buffer = std::unique_ptr<MetalBuffer, MetalObjectDeleter>(
			new MetalBuffer(desc, (__bridge void*)m_impl->device));
		if(buffer->GetNativeBuffer() == nullptr) return nullptr;
		MetalBuffer* result = buffer.get();
		m_impl->liveBuffers.push_back(std::move(buffer));
		return result;
    }

    RHI::TextureHandle MetalDevice::CreateTextureNative(const RHI::TextureDesc& desc)
    {
		auto texture = std::unique_ptr<MetalTexture, MetalObjectDeleter>(
			new MetalTexture(desc, (__bridge void*)m_impl->device));
		if(texture->GetNativeTexture() == nullptr) return nullptr;
		MetalTexture* result = texture.get();
		m_impl->liveTextures.push_back(std::move(texture));
		return result;
    }

	RHI::ShaderHandle MetalDevice::CreateShaderNative(const RHI::ShaderDesc& desc)
	{
		auto shader = std::unique_ptr<MetalShader, MetalObjectDeleter>(
			new MetalShader(desc, (__bridge void*)m_impl->device));
		if(shader->GetNativeFunction() == nullptr) return nullptr;
		MetalShader* result = shader.get();
		m_impl->liveShaders.push_back(std::move(shader));
		return result;
	}

    RHI::PipelineHandle MetalDevice::CreateGraphicsPipelineNative(
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
			(desc.hullShader != nullptr && !ownsShader(desc.hullShader)) ||
			(desc.domainShader != nullptr && !ownsShader(desc.domainShader)) ||
			(desc.fragmentShader != nullptr && !ownsShader(desc.fragmentShader)))
		{
			return nullptr;
		}
        auto pipeline = std::unique_ptr<MetalPipeline, MetalObjectDeleter>(
            new MetalPipeline(desc, (__bridge void*)m_impl->device));
		if(pipeline->GetNativePipeline() == nullptr ||
			(pipeline->IsTessellated() && pipeline->GetNativeHullPipeline() == nullptr) ||
			(desc.depthStencil.format != RHI::Format::Unknown &&
				pipeline->GetNativeDepthStencil() == nullptr)) return nullptr;
		MetalPipeline* result = pipeline.get();
		m_impl->livePipelines.push_back(std::move(pipeline));
		return result;
    }

	RHI::ResourceSetHandle MetalDevice::CreateResourceSetNative(
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

    void MetalDevice::DestroyBufferNative(RHI::BufferHandle buffer)
    {
        if(!m_impl) return;
        auto& objects = m_impl->liveBuffers;
        objects.erase(std::remove_if(objects.begin(), objects.end(),
            [buffer](const auto& object) { return object.get() == buffer; }), objects.end());
    }

    void MetalDevice::DestroyTextureNative(RHI::TextureHandle texture)
    {
        if(!m_impl) return;
        auto& objects = m_impl->liveTextures;
        objects.erase(std::remove_if(objects.begin(), objects.end(),
            [texture](const auto& object) { return object.get() == texture; }), objects.end());
    }

	void MetalDevice::DestroyShaderNative(RHI::ShaderHandle shader)
	{
        if(!m_impl) return;
        auto& objects = m_impl->liveShaders;
        objects.erase(std::remove_if(objects.begin(), objects.end(),
            [shader](const auto& object) { return object.get() == shader; }), objects.end());
    }

    void MetalDevice::DestroyPipelineNative(RHI::PipelineHandle pipeline)
    {
        if(!m_impl) return;
        auto& objects = m_impl->livePipelines;
        objects.erase(std::remove_if(objects.begin(), objects.end(),
            [pipeline](const auto& object) { return object.get() == pipeline; }), objects.end());
    }

	void MetalDevice::DestroyResourceSetNative(RHI::ResourceSetHandle resourceSet)
	{
        if(!m_impl) return;
        auto& objects = m_impl->liveResourceSets;
        objects.erase(std::remove_if(objects.begin(), objects.end(),
            [resourceSet](const auto& object) { return object.get() == resourceSet; }), objects.end());
    }

	bool MetalDevice::UpdateBufferNative(
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

	bool MetalDevice::UpdateTextureNative(
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

    RHI::TextureHandle MetalDevice::GetBackBufferNative()
    {
        return m_impl->metalLayer == nil ? nullptr : m_impl->backBufferTex;
    }

    bool MetalDevice::ReadTextureNative(RHI::TextureHandle texture, RHI::TextureReadback& result)
    {
        if(!m_impl || !texture || m_impl->asyncWorkFailed || !m_impl->activeCommandLists.empty()) return false;
        const bool backBuffer = texture == m_impl->backBufferTex;
        id<MTLTexture> image = nil;
        if(backBuffer)
        {
            if(!m_impl->allowReadback || !m_impl->frameActive ||
                !m_impl->frameLastSubmissionValue || m_impl->currentDrawable == nil) return false;
            // Submit 뒤에도 현재 drawable을 보존하므로 Present 전의 진단 readback이 가능하다.
            image = m_impl->currentDrawable.texture;
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
