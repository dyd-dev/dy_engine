#include "MetalCommandList.h"
#include "MetalBuffer.h"
#include "MetalTexture.h"
#include "MetalPipeline.h"
#include <vector>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

namespace dy::Backends
{
    struct MetalCommandList::Impl
    {
        id<MTLCommandQueue>          commandQueue   = nil;
        id<MTLCommandBuffer>         commandBuffer  = nil;
        id<MTLRenderCommandEncoder>  encoder        = nil;
        MTLRenderPassDescriptor*     passDescriptor = nil;
        id<CAMetalDrawable>          drawable       = nil;
        std::vector<id<MTLTexture>>  textures;
    };

    MetalCommandList::MetalCommandList(void* commandQueue)
        : m_impl(new Impl())
    {
        m_impl->commandQueue = (__bridge id<MTLCommandQueue>)commandQueue;
    }

    MetalCommandList::~MetalCommandList()
    {
        delete m_impl;
    }

    void MetalCommandList::Begin(void* drawable)
    {
        m_impl->drawable      = (__bridge id<CAMetalDrawable>)drawable;
        m_impl->commandBuffer = [m_impl->commandQueue commandBuffer];
        m_impl->encoder       = nil;
        m_impl->passDescriptor = nil;
    }

    void MetalCommandList::SetRenderTargets(uint32_t numRenderTargets, RHI::ITexture** renderTargets, RHI::ITexture* depthStencil)
    {
        if(m_impl->encoder)
        {
            [m_impl->encoder endEncoding];
            m_impl->encoder = nil;
        }

        MTLRenderPassDescriptor* passDesc = [MTLRenderPassDescriptor new];

        if(numRenderTargets == 0 || renderTargets == nullptr)
        {
            passDesc.colorAttachments[0].texture     = m_impl->drawable.texture;
            passDesc.colorAttachments[0].loadAction  = MTLLoadActionClear;
            passDesc.colorAttachments[0].storeAction = MTLStoreActionStore;
            passDesc.colorAttachments[0].clearColor  = MTLClearColorMake(0.08, 0.10, 0.14, 1.0);
        }
        else
        {
            for(uint32_t i = 0; i < numRenderTargets; i++)
            {
                auto* tex = static_cast<MetalTexture*>(renderTargets[i]);
                id<MTLTexture> mtlTex = (__bridge id<MTLTexture>)tex->GetNativeTexture();

                if(mtlTex == nil)
                    mtlTex = m_impl->drawable.texture;

                passDesc.colorAttachments[i].texture     = mtlTex;
                passDesc.colorAttachments[i].loadAction  = MTLLoadActionClear;
                passDesc.colorAttachments[i].storeAction = MTLStoreActionStore;
                passDesc.colorAttachments[i].clearColor  = MTLClearColorMake(0.08, 0.10, 0.14, 1.0);
            }
        }

        if(depthStencil)
        {
            auto* depth = static_cast<MetalTexture*>(depthStencil);
            passDesc.depthAttachment.texture     = (__bridge id<MTLTexture>)depth->GetNativeTexture();
            passDesc.depthAttachment.loadAction  = MTLLoadActionClear;
            passDesc.depthAttachment.storeAction = MTLStoreActionDontCare;
            passDesc.depthAttachment.clearDepth  = 1.0;
        }

        m_impl->passDescriptor = passDesc;
    }

    void MetalCommandList::SetViewport(const RHI::Viewport&) {}
    void MetalCommandList::SetScissor(const RHI::Rect&) {}

    void MetalCommandList::ClearColor(RHI::ITexture* renderTarget, float r, float g, float b, float a)
    {
        if(m_impl->passDescriptor == nil || m_impl->encoder != nil)
            return;

        id<MTLTexture> targetTexture = nil;
        if(renderTarget != nullptr)
        {
            auto* metalTexture = static_cast<MetalTexture*>(renderTarget);
            targetTexture = (__bridge id<MTLTexture>)metalTexture->GetNativeTexture();
        }

        for(uint32_t i = 0; i < 8; ++i)
        {
            MTLRenderPassColorAttachmentDescriptor* attachment = m_impl->passDescriptor.colorAttachments[i];
            if(attachment.texture == nil)
                continue;

            if(targetTexture == nil || attachment.texture == targetTexture)
            {
                attachment.clearColor = MTLClearColorMake(r, g, b, a);
                if(targetTexture != nil)
                    break;
            }
        }
    }

    void MetalCommandList::ClearDepth(RHI::ITexture*, float depth)
    {
        if(m_impl->passDescriptor == nil || m_impl->encoder != nil)
            return;

        if(m_impl->passDescriptor.depthAttachment.texture != nil)
            m_impl->passDescriptor.depthAttachment.clearDepth = depth;
    }

    void MetalCommandList::BindGraphicsPipeline(RHI::IPipelineState* pipelineState)
    {
        EnsureRenderEncoder();

        auto* pipeline = static_cast<MetalPipeline*>(pipelineState);
        auto* native   = (__bridge id<MTLRenderPipelineState>)pipeline->GetNativePipeline();
        auto* depth    = (__bridge id<MTLDepthStencilState>)pipeline->GetNativeDepthStencil();

        [m_impl->encoder setRenderPipelineState:native];
        if(depth)
            [m_impl->encoder setDepthStencilState:depth];
    }

    void MetalCommandList::BindGlobalDescriptorHeap() {}
    void MetalCommandList::BindVertexBuffer(RHI::IBuffer*, uint32_t, uint32_t) {}
    void MetalCommandList::BindIndexBuffer(RHI::IBuffer*, RHI::Format, uint32_t) {}

    void MetalCommandList::SetPushConstants(uint32_t size, const void* data)
    {
        EnsureRenderEncoder();

        [m_impl->encoder setVertexBytes:data length:size atIndex:0];
        [m_impl->encoder setFragmentBytes:data length:size atIndex:0];

        // DrawConstants 레이아웃:
        //   [0..15]  float4x4 worldMatrix  (64 bytes)
        //   [16..19] float4   baseColor    (16 bytes)
        //   [20]     uint32_t baseColorTextureIndex (4 bytes)  ← byte offset 80
        static constexpr uint32_t kTextureIndexOffset = 20; // 80 bytes / 4
        static constexpr uint32_t kMinRequiredSize    = (kTextureIndexOffset + 1) * sizeof(uint32_t);

        if(size < kMinRequiredSize)
            return;

        const uint32_t* constants  = static_cast<const uint32_t*>(data);
        const uint32_t  textureIdx = constants[kTextureIndexOffset];

        if(textureIdx < m_impl->textures.size() && m_impl->textures[textureIdx] != nil)
            [m_impl->encoder setFragmentTexture:m_impl->textures[textureIdx] atIndex:0];
    }

    void MetalCommandList::DrawInstanced(uint32_t vertexCount, uint32_t instanceCount, uint32_t startVertex, uint32_t startInstance)
    {
        EnsureRenderEncoder();

        [m_impl->encoder drawPrimitives:MTLPrimitiveTypeTriangle
                            vertexStart:startVertex
                            vertexCount:vertexCount
                          instanceCount:instanceCount
                           baseInstance:startInstance];
    }

    void MetalCommandList::DrawIndexedInstanced(uint32_t, uint32_t, uint32_t, int32_t, uint32_t) {}

    void MetalCommandList::ResourceBarrier(RHI::IBuffer*, RHI::ResourceState, RHI::ResourceState) {}
    void MetalCommandList::ResourceBarrier(RHI::ITexture*, RHI::ResourceState, RHI::ResourceState) {}

    void MetalCommandList::Close()
    {
        if(m_impl->encoder)
        {
            [m_impl->encoder endEncoding];
            m_impl->encoder = nil;
        }
    }

    void MetalCommandList::SetNativePipelineState(void* pipelineState)
    {
        EnsureRenderEncoder();

        id<MTLRenderPipelineState> pipeline =
            (__bridge id<MTLRenderPipelineState>)pipelineState;
        [m_impl->encoder setRenderPipelineState:pipeline];
    }

    void MetalCommandList::SetNativeVertexBuffer(RHI::IBuffer* buffer, uint32_t index)
    {
        EnsureRenderEncoder();

        auto* metalBuffer = static_cast<MetalBuffer*>(buffer);
        id<MTLBuffer> mtlBuffer = (__bridge id<MTLBuffer>)metalBuffer->GetNativeBuffer();
        [m_impl->encoder setVertexBuffer:mtlBuffer offset:0 atIndex:index];
    }

    void MetalCommandList::SetNativeTexture(void* texture, uint32_t index)
    {
        id<MTLTexture> mtlTexture = (__bridge id<MTLTexture>)texture;
        if(index >= m_impl->textures.size())
            m_impl->textures.resize(index + 1, nil);
        m_impl->textures[index] = mtlTexture;
    }

    void MetalCommandList::DrawIndexed(RHI::IBuffer* indexBuffer, uint32_t indexCount)
    {
        EnsureRenderEncoder();

        auto* metalBuffer = static_cast<MetalBuffer*>(indexBuffer);
        id<MTLBuffer> mtlBuffer = (__bridge id<MTLBuffer>)metalBuffer->GetNativeBuffer();
        [m_impl->encoder drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                                    indexCount:indexCount
                                     indexType:MTLIndexTypeUInt16
                                   indexBuffer:mtlBuffer
                             indexBufferOffset:0];
    }

    void MetalCommandList::EnsureRenderEncoder()
    {
        if(m_impl->encoder != nil)
            return;

        if(m_impl->passDescriptor == nil)
        {
            MTLRenderPassDescriptor* passDesc = [MTLRenderPassDescriptor new];
            passDesc.colorAttachments[0].texture     = m_impl->drawable.texture;
            passDesc.colorAttachments[0].loadAction  = MTLLoadActionClear;
            passDesc.colorAttachments[0].storeAction = MTLStoreActionStore;
            passDesc.colorAttachments[0].clearColor  = MTLClearColorMake(0.08, 0.10, 0.14, 1.0);
            m_impl->passDescriptor = passDesc;
        }

        m_impl->encoder = [m_impl->commandBuffer renderCommandEncoderWithDescriptor:m_impl->passDescriptor];
    }

    void* MetalCommandList::GetNativeCommandBuffer() const
    {
        return (__bridge void*)m_impl->commandBuffer;
    }
}
