#include "MetalDevice.h"
#include "MetalBuffer.h"
#include "MetalTexture.h"
#include "MetalPipeline.h"
#include "MetalCommandList.h"

#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <AppKit/AppKit.h>

namespace dy::Backends
{
    struct MetalDevice::Impl
    {
        id<MTLDevice>       device          = nil;
        id<MTLCommandQueue> commandQueue    = nil;
        CAMetalLayer*       metalLayer      = nil;
        id<CAMetalDrawable> currentDrawable = nil;
        uint32_t            frameIndex      = 0;

        MetalCommandList*   commandList     = nullptr;
    };

    MetalDevice::MetalDevice()
        : m_impl(new Impl()) {}

    MetalDevice::~MetalDevice()
    {
        delete m_impl->commandList;
        delete m_impl;
    }

    int MetalDevice::Initialize(const void* windowHandle)
    {
        m_impl->device = MTLCreateSystemDefaultDevice();
        if(!m_impl->device) return -1;

        m_impl->commandQueue = [m_impl->device newCommandQueue];

        NSWindow* window = (__bridge NSWindow*)windowHandle;
        m_impl->metalLayer = [CAMetalLayer layer];
        m_impl->metalLayer.device      = m_impl->device;
        m_impl->metalLayer.pixelFormat = MTLPixelFormatBGRA8Unorm;
        m_impl->metalLayer.frame       = window.contentView.bounds;
        [window.contentView setWantsLayer:YES];
        [window.contentView setLayer:m_impl->metalLayer];

        m_impl->commandList = new MetalCommandList(
            (__bridge void*)m_impl->commandQueue
        );

        return 0;
    }

    void MetalDevice::BeginFrame()
    {
        m_impl->currentDrawable = [m_impl->metalLayer nextDrawable];
        m_impl->frameIndex      = (m_impl->frameIndex + 1) % 2;

        m_impl->commandList->Begin(
            (__bridge void*)m_impl->currentDrawable
        );
    }

    uint32_t MetalDevice::GetCurrentFrameIndex() const
    {
        return m_impl->frameIndex;
    }

    RHI::ICommandList* MetalDevice::AcquireCommandList()
    {
        return m_impl->commandList;
    }

    void MetalDevice::Submit(RHI::ICommandList** cmdLists, uint32_t count)
    {
        for(uint32_t i = 0; i < count; i++)
        {
            auto* metalCmdList = static_cast<MetalCommandList*>(cmdLists[i]);
            id<MTLCommandBuffer> cmdBuffer = (__bridge id<MTLCommandBuffer>)
                metalCmdList->GetNativeCommandBuffer();
            [cmdBuffer commit];
        }
    }

    void MetalDevice::Present()
    {
        if(m_impl->currentDrawable)
        {
            id<MTLCommandBuffer> cmdBuffer = [m_impl->commandQueue commandBuffer];
            [cmdBuffer presentDrawable:m_impl->currentDrawable];
            [cmdBuffer commit];
        }
    }

    RHI::IBuffer* MetalDevice::CreateBuffer(const RHI::BufferDesc& desc)
    {
        return new MetalBuffer(desc, (__bridge void*)m_impl->device);
    }

    RHI::ITexture* MetalDevice::CreateTexture(const RHI::TextureDesc& desc)
    {
        return new MetalTexture(desc, (__bridge void*)m_impl->device);
    }

    RHI::IPipelineState* MetalDevice::CreateGraphicsPipeline(const RHI::GraphicsPipelineDesc& desc)
    {
        return new MetalPipeline(desc, (__bridge void*)m_impl->device);
    }

    void MetalDevice::DestroyBuffer(RHI::IBuffer* buffer)                 { delete buffer; }
    void MetalDevice::DestroyTexture(RHI::ITexture* texture)              { delete texture; }
    void MetalDevice::DestroyPipelineState(RHI::IPipelineState* pipeline) { delete pipeline; }

    RHI::ITexture* MetalDevice::GetBackBuffer()
    {
        RHI::TextureDesc desc{};
        desc.width  = static_cast<uint32_t>(m_impl->metalLayer.drawableSize.width);
        desc.height = static_cast<uint32_t>(m_impl->metalLayer.drawableSize.height);
        desc.format = RHI::Format::R8G8B8A8_UNORM;
        desc.usage  = RHI::TextureUsage::RenderTarget;

        return new MetalTexture(desc, (__bridge void*)m_impl->device);
    }
}
