#include "MetalCommandList.h"
#include "MetalBuffer.h"
#include "MetalTexture.h"
#include "MetalPipeline.h"
#include "RHI/ShaderLayout.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <vector>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

namespace dy::Backends
{
    namespace
    {
        constexpr uint32_t kMaxDescriptorBindings = 16u;
        constexpr uint32_t kMaxPushConstantBytes = 256u;
        constexpr uint32_t kVertexStorageBinding = RHI::ShaderLayoutDesc{}.vertexStorageBinding;
        constexpr uint32_t kIndexStorageBinding = RHI::ShaderLayoutDesc{}.indexStorageBinding;

        id<MTLBuffer> GetMetalBuffer(RHI::IBuffer* buffer)
        {
            if(buffer == nullptr)
                return nil;

            auto* metalBuffer = static_cast<MetalBuffer*>(buffer);
            return (__bridge id<MTLBuffer>)metalBuffer->GetNativeBuffer();
        }

        id<MTLTexture> GetMetalTexture(RHI::ITexture* texture)
        {
            if(texture == nullptr)
                return nil;

            auto* metalTexture = static_cast<MetalTexture*>(texture);
            return (__bridge id<MTLTexture>)metalTexture->GetNativeTexture();
        }
    }

    struct MetalCommandList::Impl
    {
        id<MTLCommandQueue>          commandQueue   = nil;
        id<MTLCommandBuffer>         commandBuffer  = nil;
        id<MTLRenderCommandEncoder>  encoder        = nil;
        MTLRenderPassDescriptor*     passDescriptor = nil;
        id<CAMetalDrawable>          drawable       = nil;
        std::vector<id<MTLTexture>>  textures;
        RHI::GeometryBinding         geometry       = {};
        std::array<uint8_t, kMaxPushConstantBytes> inlineConstants = {};
        uint32_t                     inlineConstantSize = 0;
        uint32_t                     renderWidth = 1;
        uint32_t                     renderHeight = 1;
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
        m_impl->geometry = {};
        m_impl->inlineConstants = {};
        m_impl->inlineConstantSize = 0;
        m_impl->renderWidth = 1;
        m_impl->renderHeight = 1;
    }

    void MetalCommandList::SetRenderTargets(uint32_t numRenderTargets, RHI::ITexture** renderTargets, RHI::ITexture* depthStencil)
    {
        if(m_impl->encoder)
        {
            [m_impl->encoder endEncoding];
            m_impl->encoder = nil;
        }

        MTLRenderPassDescriptor* passDesc = [MTLRenderPassDescriptor new];
        uint32_t width = 1;
        uint32_t height = 1;

        if(numRenderTargets == 0 || renderTargets == nullptr)
        {
            if(depthStencil == nullptr && m_impl->drawable != nil)
            {
                passDesc.colorAttachments[0].texture     = m_impl->drawable.texture;
                passDesc.colorAttachments[0].loadAction  = MTLLoadActionClear;
                passDesc.colorAttachments[0].storeAction = MTLStoreActionStore;
                passDesc.colorAttachments[0].clearColor  = MTLClearColorMake(0.08, 0.10, 0.14, 1.0);
                width = static_cast<uint32_t>(m_impl->drawable.texture.width);
                height = static_cast<uint32_t>(m_impl->drawable.texture.height);
            }
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
                if(i == 0 && mtlTex != nil)
                {
                    width = static_cast<uint32_t>(mtlTex.width);
                    height = static_cast<uint32_t>(mtlTex.height);
                }
            }
        }

        if(depthStencil)
        {
            auto* depth = static_cast<MetalTexture*>(depthStencil);
            passDesc.depthAttachment.texture     = (__bridge id<MTLTexture>)depth->GetNativeTexture();
            passDesc.depthAttachment.loadAction  = MTLLoadActionClear;
            passDesc.depthAttachment.storeAction = MTLStoreActionDontCare;
            passDesc.depthAttachment.clearDepth  = 1.0;
            width = depthStencil->GetWidth();
            height = depthStencil->GetHeight();
        }

        m_impl->passDescriptor = passDesc;
        m_impl->renderWidth = width;
        m_impl->renderHeight = height;
    }

    void MetalCommandList::SetViewport(const RHI::Viewport& viewport)
    {
        EnsureRenderEncoder();
        MTLViewport mtlViewport = {
            static_cast<double>(viewport.x),
            static_cast<double>(viewport.y),
            static_cast<double>(viewport.width),
            static_cast<double>(viewport.height),
            static_cast<double>(viewport.minDepth),
            static_cast<double>(viewport.maxDepth)
        };
        [m_impl->encoder setViewport:mtlViewport];
    }

    void MetalCommandList::SetScissor(const RHI::Rect& rect)
    {
        EnsureRenderEncoder();
        MTLScissorRect scissor = {
            static_cast<NSUInteger>(rect.x < 0 ? 0 : rect.x),
            static_cast<NSUInteger>(rect.y < 0 ? 0 : rect.y),
            static_cast<NSUInteger>(rect.width),
            static_cast<NSUInteger>(rect.height)
        };
        [m_impl->encoder setScissorRect:scissor];
    }

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

    void MetalCommandList::BindGlobalDescriptors()
    {
        EnsureRenderEncoder();
        for(uint32_t i = 0; i < m_impl->textures.size(); ++i)
        {
            if(m_impl->textures[i] != nil)
                [m_impl->encoder setFragmentTexture:m_impl->textures[i] atIndex:i];
        }
    }

    void MetalCommandList::BindVertexBuffer(RHI::IBuffer* buffer, uint32_t stride, uint32_t offset)
    {
        m_impl->geometry.vertexBuffer = buffer;
        m_impl->geometry.vertexStride = stride;
        m_impl->geometry.vertexOffset = offset;

        EnsureRenderEncoder();
        [m_impl->encoder setVertexBuffer:GetMetalBuffer(buffer) offset:offset atIndex:kVertexStorageBinding];
    }

    void MetalCommandList::BindIndexBuffer(RHI::IBuffer* buffer, RHI::Format format, uint32_t offset)
    {
        m_impl->geometry.indexBuffer = buffer;
        m_impl->geometry.indexFormat = format;
        m_impl->geometry.indexOffset = offset;

        EnsureRenderEncoder();
        [m_impl->encoder setVertexBuffer:GetMetalBuffer(buffer) offset:offset atIndex:kIndexStorageBinding];
    }

    void MetalCommandList::BindGeometry(const RHI::GeometryBinding& geometry) {
        BindVertexBuffer(geometry.vertexBuffer, geometry.vertexStride, geometry.vertexOffset);
        if (geometry.indexBuffer != nullptr) {
            BindIndexBuffer(geometry.indexBuffer, geometry.indexFormat, geometry.indexOffset);
        }
    }

    void MetalCommandList::BindConstantBuffer(uint32_t binding, RHI::IBuffer* buffer, uint32_t offset, uint32_t size)
    {
        (void)size;
        if(binding >= kMaxDescriptorBindings)
            return;

        EnsureRenderEncoder();
        id<MTLBuffer> mtlBuffer = GetMetalBuffer(buffer);
        [m_impl->encoder setVertexBuffer:mtlBuffer offset:offset atIndex:binding];
        [m_impl->encoder setFragmentBuffer:mtlBuffer offset:offset atIndex:binding];
    }

    void MetalCommandList::BindStorageBuffer(uint32_t binding, RHI::IBuffer* buffer, uint32_t offset, uint32_t size)
    {
        (void)size;
        if(binding >= kMaxDescriptorBindings)
            return;

        EnsureRenderEncoder();
        id<MTLBuffer> mtlBuffer = GetMetalBuffer(buffer);
        [m_impl->encoder setVertexBuffer:mtlBuffer offset:offset atIndex:binding];
        [m_impl->encoder setFragmentBuffer:mtlBuffer offset:offset atIndex:binding];
    }

    void MetalCommandList::BindTexture(uint32_t binding, RHI::ITexture* texture)
    {
        EnsureRenderEncoder();
        [m_impl->encoder setFragmentTexture:GetMetalTexture(texture) atIndex:binding];
    }

    void MetalCommandList::SetInlineConstants(uint32_t size, const void* data)
    {
        EnsureRenderEncoder();

        if(data == nullptr || size == 0)
        {
            m_impl->inlineConstantSize = 0;
            return;
        }

        m_impl->inlineConstantSize = std::min<uint32_t>(size, static_cast<uint32_t>(m_impl->inlineConstants.size()));
        std::memcpy(m_impl->inlineConstants.data(), data, m_impl->inlineConstantSize);

        [m_impl->encoder setVertexBytes:m_impl->inlineConstants.data() length:m_impl->inlineConstantSize atIndex:0];
        [m_impl->encoder setFragmentBytes:m_impl->inlineConstants.data() length:m_impl->inlineConstantSize atIndex:0];
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

    void MetalCommandList::DrawIndexedInstanced(uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex, int32_t vertexOffset, uint32_t firstInstance)
    {
        (void)firstIndex;
        (void)vertexOffset;
        EnsureRenderEncoder();

        [m_impl->encoder drawPrimitives:MTLPrimitiveTypeTriangle
                            vertexStart:0
                            vertexCount:indexCount
                          instanceCount:instanceCount
                           baseInstance:firstInstance];
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
        EnsureRenderEncoder();

        id<MTLRenderPipelineState> pipeline =
            (__bridge id<MTLRenderPipelineState>)pipelineState;
        [m_impl->encoder setRenderPipelineState:pipeline];
    }

    void MetalCommandList::SetNativeVertexBuffer(RHI::IBuffer* buffer, uint32_t index)
    {
        EnsureRenderEncoder();

        [m_impl->encoder setVertexBuffer:GetMetalBuffer(buffer) offset:0 atIndex:index];
    }

    void MetalCommandList::SetNativeTexture(void* texture, uint32_t index)
    {
        id<MTLTexture> mtlTexture = (__bridge id<MTLTexture>)texture;
        if(index >= m_impl->textures.size())
            m_impl->textures.resize(index + 1, nil);
        m_impl->textures[index] = mtlTexture;

        if(m_impl->encoder != nil)
            [m_impl->encoder setFragmentTexture:mtlTexture atIndex:index];
    }

    void MetalCommandList::DrawIndexed(RHI::IBuffer* indexBuffer, uint32_t indexCount)
    {
        EnsureRenderEncoder();

        id<MTLBuffer> mtlBuffer = GetMetalBuffer(indexBuffer);
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
            if(m_impl->drawable != nil)
            {
                m_impl->renderWidth = static_cast<uint32_t>(m_impl->drawable.texture.width);
                m_impl->renderHeight = static_cast<uint32_t>(m_impl->drawable.texture.height);
            }
        }

        m_impl->encoder = [m_impl->commandBuffer renderCommandEncoderWithDescriptor:m_impl->passDescriptor];
        MTLViewport viewport = { 0.0, 0.0, static_cast<double>(m_impl->renderWidth), static_cast<double>(m_impl->renderHeight), 0.0, 1.0 };
        MTLScissorRect scissor = { 0, 0, m_impl->renderWidth, m_impl->renderHeight };
        [m_impl->encoder setViewport:viewport];
        [m_impl->encoder setScissorRect:scissor];
    }

    void* MetalCommandList::GetNativeCommandBuffer() const
    {
        return (__bridge void*)m_impl->commandBuffer;
    }
}
