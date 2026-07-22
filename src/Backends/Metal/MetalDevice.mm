#include "MetalDevice.h"
#include "MetalBuffer.h"
#include "MetalTexture.h"
#include "MetalPipeline.h"
#include "MetalCommandList.h"
#include "RHI/IResourceSet.h"
#include "RHI/ISampler.h"
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
        NSMutableArray*     frameCompletionCommandBuffers = nil;
        uint32_t            maxFramesInFlight = 0;
        uint32_t            frameIndex      = 0;
        RHI::Format         swapchainFormat = RHI::Format::Unknown;

        MetalCommandList*   commandList     = nullptr;
        MetalTexture*       backBufferTex   = nullptr;  // cached; not owned by Renderer

    };

    MetalDevice::MetalDevice()
        : m_impl(new Impl()) {}

    MetalDevice::~MetalDevice()
    {
        for(id completion in m_impl->frameCompletionCommandBuffers)
        {
            if(completion != [NSNull null])
                [completion waitUntilCompleted];
        }
        [m_impl->frameCompletionCommandBuffers release];
        delete m_impl->backBufferTex;
        delete m_impl->commandList;
        delete m_impl;
    }

    int MetalDevice::Initialize(const void* windowHandle, const RHI::DeviceDesc& desc)
    {
        MTLPixelFormat layerPixelFormat = MTLPixelFormatInvalid;
        switch(desc.swapchainFormat)
        {
        case RHI::Format::Unknown:
            m_impl->swapchainFormat = RHI::Format::B8G8R8A8_UNORM;
            layerPixelFormat = MTLPixelFormatBGRA8Unorm;
            break;
        case RHI::Format::B8G8R8A8_UNORM:
            m_impl->swapchainFormat = desc.swapchainFormat;
            layerPixelFormat = MTLPixelFormatBGRA8Unorm;
            break;
        case RHI::Format::B8G8R8A8_UNORM_SRGB:
            m_impl->swapchainFormat = desc.swapchainFormat;
            layerPixelFormat = MTLPixelFormatBGRA8Unorm_sRGB;
            break;
        case RHI::Format::R16G16B16A16_FLOAT:
            m_impl->swapchainFormat = desc.swapchainFormat;
            layerPixelFormat = MTLPixelFormatRGBA16Float;
            break;
        case RHI::Format::R8G8B8A8_UNORM:
        case RHI::Format::R8G8B8A8_UNORM_SRGB:
        case RHI::Format::R32G32B32A32_FLOAT:
        case RHI::Format::D32_FLOAT:
        case RHI::Format::D24_UNORM_S8_UINT:
        case RHI::Format::R32_UINT:
        case RHI::Format::R16_UINT:
        default:
            return -1;
        }

        m_impl->maxFramesInFlight = desc.maxFramesInFlight;
        m_impl->frameCompletionCommandBuffers = [[NSMutableArray alloc] initWithCapacity:desc.maxFramesInFlight];
        for(uint32_t i = 0; i < desc.maxFramesInFlight; ++i)
            [m_impl->frameCompletionCommandBuffers addObject:[NSNull null]];

        m_impl->device = MTLCreateSystemDefaultDevice();
        if(!m_impl->device) return -1;

        m_impl->commandQueue = [m_impl->device newCommandQueue];

        NSWindow* window = (__bridge NSWindow*)windowHandle;
        m_impl->metalLayer = [CAMetalLayer layer];
        m_impl->metalLayer.device      = m_impl->device;
        m_impl->metalLayer.pixelFormat = layerPixelFormat;
        m_impl->metalLayer.frame       = window.contentView.bounds;
        [window.contentView setWantsLayer:YES];
        [window.contentView setLayer:m_impl->metalLayer];

        RHI::TextureDesc backBufferDesc{};
        backBufferDesc.width = static_cast<uint32_t>(m_impl->metalLayer.drawableSize.width);
        backBufferDesc.height = static_cast<uint32_t>(m_impl->metalLayer.drawableSize.height);
        backBufferDesc.format = m_impl->swapchainFormat;
        backBufferDesc.usage = RHI::TextureUsage::RenderTarget;
        m_impl->backBufferTex = new MetalTexture(backBufferDesc);

        m_impl->commandList = new MetalCommandList(
            (__bridge void*)m_impl->commandQueue
        );

        return 0;
    }

    bool MetalDevice::BeginFrame()
    {
        id previousCompletion = [m_impl->frameCompletionCommandBuffers objectAtIndex:m_impl->frameIndex];
        if(previousCompletion != [NSNull null])
        {
            [previousCompletion waitUntilCompleted];
            [m_impl->frameCompletionCommandBuffers replaceObjectAtIndex:m_impl->frameIndex withObject:[NSNull null]];
        }

        m_impl->currentDrawable = [m_impl->metalLayer nextDrawable];
        if(m_impl->currentDrawable == nil)
            return false;

        m_impl->backBufferTex->SetNativeTexture((__bridge void*)m_impl->currentDrawable.texture);

        m_impl->commandList->Begin();

        return true;
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
        // 같은 queue의 마지막 marker가 완료되면 이 frame의 모든 제출과 present도 완료된 것이다.
        id<MTLCommandBuffer> cmdBuffer = [m_impl->commandQueue commandBuffer];
        if(cmdBuffer != nil)
        {
            if(m_impl->currentDrawable)
            {
                [cmdBuffer presentDrawable:m_impl->currentDrawable];
            }
            [m_impl->frameCompletionCommandBuffers replaceObjectAtIndex:m_impl->frameIndex withObject:cmdBuffer];
            [cmdBuffer commit];
        }

        m_impl->currentDrawable = nil;
        m_impl->backBufferTex->SetNativeTexture(nullptr);
        m_impl->frameIndex = (m_impl->frameIndex + 1) % m_impl->maxFramesInFlight;
    }

    RHI::IBuffer* MetalDevice::CreateBuffer(const RHI::BufferDesc& desc)
    {
        return new MetalBuffer(desc, (__bridge void*)m_impl->device);
    }

    RHI::ITexture* MetalDevice::CreateTexture(const RHI::TextureDesc& desc)
    {
        return new MetalTexture(desc, (__bridge void*)m_impl->device);
    }

    RHI::IShader* MetalDevice::CreateShader(const RHI::ShaderDesc& desc)
    {
        if(desc.stage == RHI::ShaderStage::Unknown ||
           desc.binary == nullptr || desc.binarySize == 0 ||
           desc.entryPoint == nullptr || desc.entryPoint[0] == '\0') return nullptr;

        auto* shader = new MetalShader(desc, (__bridge void*)m_impl->device);
        if(!shader->IsValid())
        {
            delete shader;
            return nullptr;
        }
        return shader;
    }

    RHI::ISampler* MetalDevice::CreateSampler(const RHI::SamplerDesc& desc)
    {
        if(desc.minLod > desc.maxLod ||
           static_cast<uint32_t>(desc.minFilter) > static_cast<uint32_t>(RHI::SamplerFilter::Linear) ||
           static_cast<uint32_t>(desc.magFilter) > static_cast<uint32_t>(RHI::SamplerFilter::Linear) ||
           static_cast<uint32_t>(desc.mipFilter) > static_cast<uint32_t>(RHI::SamplerFilter::Linear) ||
           static_cast<uint32_t>(desc.addressU) > static_cast<uint32_t>(RHI::SamplerAddressMode::ClampToEdge) ||
           static_cast<uint32_t>(desc.addressV) > static_cast<uint32_t>(RHI::SamplerAddressMode::ClampToEdge) ||
           static_cast<uint32_t>(desc.addressW) > static_cast<uint32_t>(RHI::SamplerAddressMode::ClampToEdge)) return nullptr;
        auto* sampler = new MetalSampler(desc, (__bridge void*)m_impl->device);
        if(!sampler->IsValid())
        {
            delete sampler;
            return nullptr;
        }
        return sampler;
    }

    RHI::IPipelineState* MetalDevice::CreateGraphicsPipeline(const RHI::GraphicsPipelineDesc& desc)
    {
        const bool hasColorAttachment = desc.colorAttachmentCount > 0;
        const bool hasDepthAttachment = desc.depthStencilFormat != RHI::Format::Unknown;
        if((!hasColorAttachment && !hasDepthAttachment) ||
           ((desc.depthStencil.depthTestEnable || desc.depthStencil.depthWriteEnable || desc.depthStencil.stencilTestEnable) && !hasDepthAttachment) ||
           (desc.depthStencil.depthWriteEnable && !desc.depthStencil.depthTestEnable) ||
           (desc.depthStencil.stencilTestEnable && desc.depthStencilFormat != RHI::Format::D24_UNORM_S8_UINT))
            return nullptr;
        if(desc.colorAttachmentCount > 8u || (desc.colorAttachmentCount > 0 && desc.colorAttachments == nullptr))
            return nullptr;
        if((desc.inputAssembly.vertexBindingCount > 0 && desc.inputAssembly.vertexBindings == nullptr) ||
           (desc.inputAssembly.vertexAttributeCount > 0 && desc.inputAssembly.vertexAttributes == nullptr))
            return nullptr;

        auto* pipeline = new MetalPipeline(desc, (__bridge void*)m_impl->device);
        if(!pipeline->IsValid())
        {
            delete pipeline;
            return nullptr;
        }
        return pipeline;
    }

    RHI::IResourceSet* MetalDevice::CreateResourceSet(RHI::IPipelineState* pipeline)
    {
        auto* metalPipeline = dynamic_cast<MetalPipeline*>(pipeline);
        return metalPipeline == nullptr ? nullptr : new MetalResourceSet(metalPipeline);
    }

    bool MetalDevice::UpdateResourceSet(RHI::IResourceSet* resourceSet, const RHI::ResourceSetWrite* writes, uint32_t writeCount)
    {
        auto* metalSet = dynamic_cast<MetalResourceSet*>(resourceSet);
        return metalSet != nullptr && metalSet->Update(writes, writeCount);
    }

    void MetalDevice::DestroyBuffer(RHI::IBuffer* buffer)                 { delete buffer; }
    void MetalDevice::DestroyShader(RHI::IShader* shader)                 { delete shader; }
    void MetalDevice::DestroySampler(RHI::ISampler* sampler)              { delete sampler; }
    void MetalDevice::DestroyTexture(RHI::ITexture* texture)              { delete texture; }
    void MetalDevice::DestroyPipelineState(RHI::IPipelineState* pipeline) { delete pipeline; }
    void MetalDevice::DestroyResourceSet(RHI::IResourceSet* resourceSet)  { delete resourceSet; }

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

    RHI::ITexture* MetalDevice::GetBackBuffer()
    {
        return m_impl->backBufferTex;
    }
}
