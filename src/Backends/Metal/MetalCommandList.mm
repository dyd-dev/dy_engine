//
//  MetalCommandList.mm
//  
//
//  Created by 정준혁 on 4/8/26.
//

#include "MetalCommandList.h"
#include "MetalBuffer.h"
#include "MetalTexture.h"
#include "MetalPipeline.h"
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

namespace dy::Backends
{
    struct MetalCommandList::Impl
    {
        id<MTLCommandQueue>          commandQueue   = nil;
        id<MTLCommandBuffer>         commandBuffer  = nil;
        id<MTLRenderCommandEncoder>  encoder        = nil;
        id<CAMetalDrawable>          drawable       = nil;
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
    }

    void MetalCommandList::SetRenderTargets(uint32_t numRenderTargets, RHI::ITexture** renderTargets, RHI::ITexture* depthStencil)
    {
        MTLRenderPassDescriptor* passDesc = [MTLRenderPassDescriptor new];

        // RenderTarget이 없으면 SwapChain 백버퍼 사용
        if(numRenderTargets == 0 || renderTargets == nullptr)
        {
            passDesc.colorAttachments[0].texture    = m_impl->drawable.texture;
            passDesc.colorAttachments[0].loadAction = MTLLoadActionClear;
            passDesc.colorAttachments[0].storeAction = MTLStoreActionStore;
            passDesc.colorAttachments[0].clearColor = MTLClearColorMake(0.1, 0.1, 0.2, 1.0);
        }
        else
        {
            for(uint32_t i = 0; i < numRenderTargets; i++)
            {
                auto* tex = static_cast<MetalTexture*>(renderTargets[i]);
                passDesc.colorAttachments[i].texture     = (__bridge id<MTLTexture>)tex->GetNativeTexture();
                passDesc.colorAttachments[i].loadAction  = MTLLoadActionClear;
                passDesc.colorAttachments[i].storeAction = MTLStoreActionStore;
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

        m_impl->encoder = [m_impl->commandBuffer renderCommandEncoderWithDescriptor:passDesc];
    }

    void MetalCommandList::ClearColor(RHI::ITexture* renderTarget, float r, float g, float b, float a)
    {
        // Metal은 RenderPassDescriptor에서 clearColor로 처리 → SetRenderTargets에서 이미 처리됨
    }

    void MetalCommandList::ClearDepth(RHI::ITexture* depthStencil, float depth)
    {
        // Metal은 RenderPassDescriptor에서 clearDepth로 처리 → SetRenderTargets에서 이미 처리됨
    }

    void MetalCommandList::BindGraphicsPipeline(RHI::IPipelineState* pipelineState)
    {
        auto* pipeline = static_cast<MetalPipeline*>(pipelineState);
        auto* native   = (__bridge id<MTLRenderPipelineState>)pipeline->GetNativePipeline();
        auto* depth    = (__bridge id<MTLDepthStencilState>)pipeline->GetNativeDepthStencil();

        [m_impl->encoder setRenderPipelineState:native];
        if(depth)
            [m_impl->encoder setDepthStencilState:depth];
    }

    void MetalCommandList::BindIndexBuffer(RHI::IBuffer* buffer, RHI::Format format, uint32_t offset)
    {
        // Metal은 drawIndexedPrimitives 호출 시 직접 넘김 → 여기선 저장만
        // 필요 시 Impl에 캐싱 추가
    }

    void MetalCommandList::SetPushConstants(uint32_t size, const void* data)
    {
        // vertex: buffer(0), fragment: buffer(0)
        [m_impl->encoder setVertexBytes:data length:size atIndex:0];
        [m_impl->encoder setFragmentBytes:data length:size atIndex:0];
    }

    void MetalCommandList::DrawInstanced(uint32_t vertexCount, uint32_t instanceCount, uint32_t startVertex, uint32_t startInstance)
    {
        [m_impl->encoder drawPrimitives:MTLPrimitiveTypeTriangle
                            vertexStart:startVertex
                            vertexCount:vertexCount
                          instanceCount:instanceCount
                           baseInstance:startInstance];
    }

    void MetalCommandList::ResourceBarrier(RHI::IBuffer* buffer, RHI::ResourceState before, RHI::ResourceState after)
    {
        // Metal은 자동으로 리소스 동기화 처리 → 명시적 배리어 불필요
    }

    void MetalCommandList::ResourceBarrier(RHI::ITexture* texture, RHI::ResourceState before, RHI::ResourceState after)
    {
        // Metal은 자동으로 리소스 동기화 처리 → 명시적 배리어 불필요
    }

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
    id<MTLRenderPipelineState> pipeline =
        (__bridge id<MTLRenderPipelineState>)pipelineState;
    [m_impl->encoder setRenderPipelineState:pipeline];
}

void MetalCommandList::SetNativeVertexBuffer(RHI::IBuffer* buffer, uint32_t index)
{
    auto* metalBuffer = static_cast<MetalBuffer*>(buffer);
    id<MTLBuffer> mtlBuffer = (__bridge id<MTLBuffer>)metalBuffer->GetNativeBuffer();
    [m_impl->encoder setVertexBuffer:mtlBuffer offset:0 atIndex:index];
}

void MetalCommandList::DrawIndexed(RHI::IBuffer* indexBuffer, uint32_t indexCount)
{
    auto* metalBuffer = static_cast<MetalBuffer*>(indexBuffer);
    id<MTLBuffer> mtlBuffer = (__bridge id<MTLBuffer>)metalBuffer->GetNativeBuffer();
    [m_impl->encoder drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                                indexCount:indexCount
                                 indexType:MTLIndexTypeUInt16
                               indexBuffer:mtlBuffer
                         indexBufferOffset:0];
}
void MetalCommandList::SetNativeTexture(void* texture, uint32_t index)
{
    id<MTLTexture> mtlTexture = (__bridge id<MTLTexture>)texture;
    [m_impl->encoder setFragmentTexture:mtlTexture atIndex:index];
}


    void* MetalCommandList::GetNativeCommandBuffer() const
    {
        return (__bridge void*)m_impl->commandBuffer;
    }
}
