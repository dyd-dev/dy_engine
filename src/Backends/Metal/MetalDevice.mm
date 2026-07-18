#include "MetalDevice.h"
#include "MetalBuffer.h"
#include "MetalTexture.h"
#include "MetalPipeline.h"
#include "MetalCommandList.h"
#include <vector>

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
        MetalTexture*       backBufferTex   = nullptr;  // cached; not owned by Renderer

        RHI::DescriptorIndex        nextDescriptorIndex = 0;
        std::vector<RHI::ITexture*> textures;
    };

    MetalDevice::MetalDevice()
        : m_impl(new Impl()) {}

    MetalDevice::~MetalDevice()
    {
        delete m_impl->backBufferTex;
        delete m_impl->commandList;
        delete m_impl;
    }

    int MetalDevice::Initialize(const void* windowHandle, const RHI::DeviceDesc&)
    {
        m_impl->device = MTLCreateSystemDefaultDevice();
        if(!m_impl->device) return -1;

        m_impl->commandQueue = [m_impl->device newCommandQueue];

        NSWindow* window = (__bridge NSWindow*)windowHandle;
        m_impl->metalLayer = [CAMetalLayer layer];
        m_impl->metalLayer.device      = m_impl->device;
        m_impl->metalLayer.pixelFormat = MTLPixelFormatRGBA8Unorm;
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

        for(uint32_t i = 0; i < m_impl->textures.size(); i++)
        {
            if(m_impl->textures[i] != nullptr)
            {
                auto* metalTex = static_cast<MetalTexture*>(m_impl->textures[i]);
                m_impl->commandList->SetNativeTexture(metalTex->GetNativeTexture(), i);
            }
        }
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
        const bool hasColorAttachment = desc.renderTargetFormat != RHI::Format::Unknown;
        const bool hasDepthAttachment = desc.depthStencilFormat != RHI::Format::Unknown;
        if((!hasColorAttachment && !hasDepthAttachment) || (desc.depthEnable && !hasDepthAttachment))
            return nullptr;

        auto* pipeline = new MetalPipeline(desc, (__bridge void*)m_impl->device);
        if(!pipeline->IsValid())
        {
            delete pipeline;
            return nullptr;
        }
        return pipeline;
    }

    void MetalDevice::DestroyBuffer(RHI::IBuffer* buffer)                 { delete buffer; }
    void MetalDevice::DestroyTexture(RHI::ITexture* texture)
    {
        if(texture == nullptr) return;
        for(uint32_t i = 0; i < m_impl->textures.size(); ++i)
        {
            if(m_impl->textures[i] != texture) continue;
            m_impl->textures[i] = nullptr;
            m_impl->commandList->SetNativeTexture(nullptr, i);
        }
        delete texture;
    }
    void MetalDevice::DestroyPipelineState(RHI::IPipelineState* pipeline) { delete pipeline; }

    bool MetalDevice::UpdateTexture(RHI::ITexture* texture, const void* data, uint32_t rowPitch)
    {
        if(texture == nullptr || data == nullptr || rowPitch == 0)
            return false;

        auto* metalTexture = static_cast<MetalTexture*>(texture);
        id<MTLTexture> nativeTexture = (__bridge id<MTLTexture>)metalTexture->GetNativeTexture();
        if(nativeTexture == nil)
            return false;

        MTLRegion region = MTLRegionMake2D(0, 0, texture->GetWidth(), texture->GetHeight());
        [nativeTexture replaceRegion:region
                          mipmapLevel:0
                            withBytes:data
                          bytesPerRow:rowPitch];
        return true;
    }

    RHI::DescriptorIndex MetalDevice::AllocateDescriptorSlot()
    {
        return m_impl->nextDescriptorIndex++;
    }

    void MetalDevice::UpdateDescriptorSlot(RHI::DescriptorIndex index, RHI::ITexture* texture)
    {
        if(index >= m_impl->textures.size())
            m_impl->textures.resize(index + 1, nullptr);

        m_impl->textures[index] = texture;

        auto* metalTex = static_cast<MetalTexture*>(texture);
        m_impl->commandList->SetNativeTexture(metalTex->GetNativeTexture(), index);
    }

    RHI::ITexture* MetalDevice::GetBackBuffer()
    {
        if(!m_impl->currentDrawable) return nullptr;

        if(!m_impl->backBufferTex)
        {
            RHI::TextureDesc desc{};
            desc.width  = static_cast<uint32_t>(m_impl->metalLayer.drawableSize.width);
            desc.height = static_cast<uint32_t>(m_impl->metalLayer.drawableSize.height);
            desc.format = RHI::Format::R8G8B8A8_UNORM;
            desc.usage  = RHI::TextureUsage::RenderTarget;
            m_impl->backBufferTex = new MetalTexture(desc, (__bridge void*)m_impl->device);
        }

        m_impl->backBufferTex->SetNativeTexture((__bridge void*)m_impl->currentDrawable.texture);
        return m_impl->backBufferTex;
    }
}
