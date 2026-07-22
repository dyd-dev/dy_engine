#include "MetalCommandList.h"
#include "MetalBuffer.h"
#include "MetalTexture.h"
#include "MetalPipeline.h"
#include <cstring>
#include <vector>
#import <Metal/Metal.h>

namespace dy::Backends
{
    namespace
    {
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

            auto* metalTexture = dynamic_cast<MetalTexture*>(texture);
            if(metalTexture == nullptr)
                return nil;
            return (__bridge id<MTLTexture>)metalTexture->GetNativeTexture();
        }
    }

    struct MetalCommandList::Impl
    {
        id<MTLCommandQueue>          commandQueue   = nil;
        id<MTLCommandBuffer>         commandBuffer  = nil;
        id<MTLRenderCommandEncoder>  encoder        = nil;
        MTLRenderPassDescriptor*     passDescriptor = nil;
        MetalPipeline*             activePipeline = nullptr;
        RHI::IBuffer*               indexBuffer    = nullptr;
        RHI::Format                 indexFormat    = RHI::Format::Unknown;
        uint32_t                    indexOffset    = 0;
        MTLPrimitiveType            primitiveType  = MTLPrimitiveTypeTriangle;
        std::vector<uint8_t>        inlineConstants;
        uint32_t                     renderWidth = 1;
        uint32_t                     renderHeight = 1;
        std::vector<RHI::ITexture*> activeColorTargets;
        RHI::ITexture*               activeDepthTarget = nullptr;
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

    void MetalCommandList::Begin()
    {
        m_impl->commandBuffer = [m_impl->commandQueue commandBuffer];
        m_impl->encoder       = nil;
        m_impl->passDescriptor = nil;
        m_impl->indexBuffer = nullptr;
        m_impl->indexFormat = RHI::Format::Unknown;
        m_impl->indexOffset = 0;
        m_impl->primitiveType = MTLPrimitiveTypeTriangle;
        m_impl->activePipeline = nullptr;
        m_impl->inlineConstants.clear();
        m_impl->renderWidth = 1;
        m_impl->renderHeight = 1;
        m_impl->activeColorTargets.clear();
        m_impl->activeDepthTarget = nullptr;
    }

    void MetalCommandList::SetRenderTargets(uint32_t numRenderTargets, RHI::ITexture** renderTargets, RHI::ITexture* depthStencil)
    {
        EndActivePass();

        if(numRenderTargets > 8u)
            return;
        if(numRenderTargets == 0 && depthStencil == nullptr)
            return;
        if(numRenderTargets > 0 && renderTargets == nullptr)
            return;

        std::vector<id<MTLTexture>> colorTextures;
        uint32_t width = 0;
        uint32_t height = 0;

        colorTextures.reserve(numRenderTargets);
        for(uint32_t attachmentIndex = 0; attachmentIndex < numRenderTargets; ++attachmentIndex)
        {
            RHI::ITexture* colorTarget = renderTargets[attachmentIndex];
            if(colorTarget == nullptr)
                return;

            if((colorTarget->GetUsage() & RHI::TextureUsage::RenderTarget) == RHI::TextureUsage::None ||
               colorTarget->GetWidth() == 0 || colorTarget->GetHeight() == 0)
            {
                return;
            }

            id<MTLTexture> colorTexture = GetMetalTexture(colorTarget);
            if(colorTexture == nil ||
               colorTexture.width != colorTarget->GetWidth() ||
               colorTexture.height != colorTarget->GetHeight())
            {
                return;
            }

            if(attachmentIndex == 0)
            {
                width = colorTarget->GetWidth();
                height = colorTarget->GetHeight();
            }
            else if(width != colorTarget->GetWidth() || height != colorTarget->GetHeight())
            {
                return;
            }
            colorTextures.push_back(colorTexture);
        }

        id<MTLTexture> depthTexture = nil;
        if(depthStencil != nullptr)
        {
            if((depthStencil->GetUsage() & RHI::TextureUsage::DepthStencil) == RHI::TextureUsage::None ||
               depthStencil->GetWidth() == 0 || depthStencil->GetHeight() == 0)
            {
                return;
            }

            depthTexture = GetMetalTexture(depthStencil);
            if(depthTexture == nil ||
               depthTexture.width != depthStencil->GetWidth() ||
               depthTexture.height != depthStencil->GetHeight())
            {
                return;
            }

            if(!colorTextures.empty() &&
               (depthStencil->GetWidth() != width || depthStencil->GetHeight() != height))
            {
                return;
            }

            if(colorTextures.empty())
            {
                width = depthStencil->GetWidth();
                height = depthStencil->GetHeight();
            }
        }

        MTLRenderPassDescriptor* passDesc = [MTLRenderPassDescriptor new];
        for(uint32_t attachmentIndex = 0; attachmentIndex < numRenderTargets; ++attachmentIndex)
        {
            passDesc.colorAttachments[attachmentIndex].texture = colorTextures[attachmentIndex];
            passDesc.colorAttachments[attachmentIndex].loadAction = MTLLoadActionLoad;
            passDesc.colorAttachments[attachmentIndex].storeAction = MTLStoreActionStore;
        }

        if(depthStencil != nullptr)
        {
            passDesc.depthAttachment.texture = depthTexture;
            passDesc.depthAttachment.loadAction = MTLLoadActionLoad;
            passDesc.depthAttachment.storeAction =
                (depthStencil->GetUsage() & RHI::TextureUsage::ShaderResource) != RHI::TextureUsage::None
                    ? MTLStoreActionStore
                    : MTLStoreActionDontCare;
            if(depthStencil->GetFormat() == RHI::Format::D24_UNORM_S8_UINT)
            {
                passDesc.stencilAttachment.texture = depthTexture;
                passDesc.stencilAttachment.loadAction = MTLLoadActionLoad;
                passDesc.stencilAttachment.storeAction = passDesc.depthAttachment.storeAction;
            }
        }

        m_impl->passDescriptor = passDesc;
        m_impl->renderWidth = width;
        m_impl->renderHeight = height;
        if(numRenderTargets > 0)
            m_impl->activeColorTargets.assign(renderTargets, renderTargets + numRenderTargets);
        m_impl->activeDepthTarget = depthStencil;
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
        if(m_impl->passDescriptor == nil || m_impl->encoder != nil ||
           renderTarget == nullptr)
            return;

        for(size_t attachmentIndex = 0; attachmentIndex < m_impl->activeColorTargets.size(); ++attachmentIndex)
        {
            if(m_impl->activeColorTargets[attachmentIndex] != renderTarget) continue;
            MTLRenderPassColorAttachmentDescriptor* attachment = m_impl->passDescriptor.colorAttachments[attachmentIndex];
            if(attachment.texture == nil) return;
            attachment.loadAction = MTLLoadActionClear;
            attachment.clearColor = MTLClearColorMake(r, g, b, a);
            return;
        }
    }

    void MetalCommandList::ClearDepth(RHI::ITexture* depthStencil, float depth)
    {
        if(m_impl->passDescriptor == nil || m_impl->encoder != nil ||
           depthStencil == nullptr || depthStencil != m_impl->activeDepthTarget)
            return;

        if(m_impl->passDescriptor.depthAttachment.texture != nil)
        {
            m_impl->passDescriptor.depthAttachment.loadAction = MTLLoadActionClear;
            m_impl->passDescriptor.depthAttachment.clearDepth = depth;
        }
    }

    void MetalCommandList::BindGraphicsPipeline(RHI::IPipelineState* pipelineState)
    {
        EnsureRenderEncoder();

        auto* pipeline = static_cast<MetalPipeline*>(pipelineState);
        auto* native   = (__bridge id<MTLRenderPipelineState>)pipeline->GetNativePipeline();
        auto* depth    = (__bridge id<MTLDepthStencilState>)pipeline->GetNativeDepthStencil();
        const RHI::RasterizationDesc& rasterization = pipeline->GetRasterization();

        [m_impl->encoder setRenderPipelineState:native];
        switch(pipeline->GetPrimitiveTopology())
        {
        case RHI::PrimitiveTopology::PointList: m_impl->primitiveType = MTLPrimitiveTypePoint; break;
        case RHI::PrimitiveTopology::LineList: m_impl->primitiveType = MTLPrimitiveTypeLine; break;
        case RHI::PrimitiveTopology::LineStrip: m_impl->primitiveType = MTLPrimitiveTypeLineStrip; break;
        case RHI::PrimitiveTopology::TriangleStrip: m_impl->primitiveType = MTLPrimitiveTypeTriangleStrip; break;
        case RHI::PrimitiveTopology::TriangleList: m_impl->primitiveType = MTLPrimitiveTypeTriangle; break;
        }
        switch(rasterization.fillMode)
        {
        case RHI::FillMode::Solid: [m_impl->encoder setTriangleFillMode:MTLTriangleFillModeFill]; break;
        case RHI::FillMode::Wireframe: [m_impl->encoder setTriangleFillMode:MTLTriangleFillModeLines]; break;
        }
        switch(rasterization.cullMode)
        {
        case RHI::CullMode::None: [m_impl->encoder setCullMode:MTLCullModeNone]; break;
        case RHI::CullMode::Front: [m_impl->encoder setCullMode:MTLCullModeFront]; break;
        case RHI::CullMode::Back: [m_impl->encoder setCullMode:MTLCullModeBack]; break;
        }
        switch(rasterization.frontFace)
        {
        case RHI::FrontFace::CounterClockwise: [m_impl->encoder setFrontFacingWinding:MTLWindingClockwise]; break;
        case RHI::FrontFace::Clockwise: [m_impl->encoder setFrontFacingWinding:MTLWindingCounterClockwise]; break;
        }
        [m_impl->encoder setDepthBias:static_cast<float>(rasterization.depthBias)
                             slopeScale:rasterization.depthBiasSlope
                                  clamp:rasterization.depthBiasClamp];
        [m_impl->encoder setDepthStencilState:depth];
        [m_impl->encoder setStencilReferenceValue:pipeline->GetStencilReference()];
        m_impl->activePipeline = pipeline;
    }

    void MetalCommandList::BindResourceSet(RHI::IResourceSet* resourceSet)
    {
        EnsureRenderEncoder();
        auto* metalSet = dynamic_cast<MetalResourceSet*>(resourceSet);
        if(metalSet != nullptr) metalSet->Bind((__bridge void*)m_impl->encoder, m_impl->activePipeline);
    }

    void MetalCommandList::BindVertexBuffer(uint32_t slot, RHI::IBuffer* buffer, uint32_t offset)
    {
        EnsureRenderEncoder();
        const uint32_t bufferIndex = MetalPipeline::GetNativeVertexBufferIndex(slot);
        if(bufferIndex < 31u)
            [m_impl->encoder setVertexBuffer:GetMetalBuffer(buffer) offset:offset atIndex:bufferIndex];
    }

    void MetalCommandList::BindIndexBuffer(RHI::IBuffer* buffer, RHI::Format format, uint32_t offset)
    {
        m_impl->indexBuffer = buffer;
        m_impl->indexFormat = format;
        m_impl->indexOffset = offset;
    }

    void MetalCommandList::SetInlineConstants(uint32_t offset, uint32_t size, const void* data)
    {
        EnsureRenderEncoder();
        if(data == nullptr || size == 0 || m_impl->activePipeline == nullptr) return;
        for(const RHI::InlineConstantRangeDesc& range : m_impl->activePipeline->GetInlineConstantRanges())
        {
            if(offset < range.offset || offset + size > range.offset + range.size) continue;
            m_impl->inlineConstants.resize(range.size, 0);
            std::memcpy(m_impl->inlineConstants.data() + offset - range.offset, data, size);
            if((range.stages & RHI::ShaderStageFlags::Vertex) != RHI::ShaderStageFlags::None)
                [m_impl->encoder setVertexBytes:m_impl->inlineConstants.data() length:range.size atIndex:range.binding];
            if((range.stages & RHI::ShaderStageFlags::Fragment) != RHI::ShaderStageFlags::None)
                [m_impl->encoder setFragmentBytes:m_impl->inlineConstants.data() length:range.size atIndex:range.binding];
            return;
        }
    }

    void MetalCommandList::DrawInstanced(uint32_t vertexCount, uint32_t instanceCount, uint32_t startVertex, uint32_t startInstance)
    {
        EnsureRenderEncoder();

        [m_impl->encoder drawPrimitives:m_impl->primitiveType
                            vertexStart:startVertex
                            vertexCount:vertexCount
                          instanceCount:instanceCount
                           baseInstance:startInstance];
    }

    void MetalCommandList::DrawIndexedInstanced(uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex, int32_t vertexOffset, uint32_t firstInstance)
    {
        EnsureRenderEncoder();
        if(m_impl->indexBuffer == nullptr ||
           (m_impl->indexFormat != RHI::Format::R16_UINT && m_impl->indexFormat != RHI::Format::R32_UINT)) return;

        const MTLIndexType indexType = m_impl->indexFormat == RHI::Format::R16_UINT
            ? MTLIndexTypeUInt16
            : MTLIndexTypeUInt32;
        const uint32_t indexSize = indexType == MTLIndexTypeUInt16 ? 2u : 4u;
        [m_impl->encoder drawIndexedPrimitives:m_impl->primitiveType
                                    indexCount:indexCount
                                     indexType:indexType
                                   indexBuffer:GetMetalBuffer(m_impl->indexBuffer)
                             indexBufferOffset:m_impl->indexOffset + firstIndex * indexSize
                                  instanceCount:instanceCount
                                     baseVertex:vertexOffset
                                   baseInstance:firstInstance];
    }

    void MetalCommandList::Close()
    {
        EndActivePass();
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

        if(m_impl->passDescriptor == nil || m_impl->commandBuffer == nil)
            return;

        m_impl->encoder = [m_impl->commandBuffer renderCommandEncoderWithDescriptor:m_impl->passDescriptor];
        if(m_impl->encoder == nil)
            return;

        MTLViewport viewport = { 0.0, 0.0, static_cast<double>(m_impl->renderWidth), static_cast<double>(m_impl->renderHeight), 0.0, 1.0 };
        MTLScissorRect scissor = { 0, 0, m_impl->renderWidth, m_impl->renderHeight };
        [m_impl->encoder setViewport:viewport];
        [m_impl->encoder setScissorRect:scissor];
    }

    void MetalCommandList::EndActivePass()
    {
        if(m_impl->passDescriptor != nil && m_impl->encoder == nil)
        {
            bool clearsColor = false;
            for(size_t attachmentIndex = 0; attachmentIndex < m_impl->activeColorTargets.size(); ++attachmentIndex)
            {
                if(m_impl->passDescriptor.colorAttachments[attachmentIndex].texture != nil &&
                   m_impl->passDescriptor.colorAttachments[attachmentIndex].loadAction == MTLLoadActionClear)
                {
                    clearsColor = true;
                    break;
                }
            }
            const bool clearsDepth =
                m_impl->passDescriptor.depthAttachment.texture != nil &&
                m_impl->passDescriptor.depthAttachment.loadAction == MTLLoadActionClear;
            if(clearsColor || clearsDepth)
                EnsureRenderEncoder();
        }

        if(m_impl->encoder != nil)
        {
            [m_impl->encoder endEncoding];
            m_impl->encoder = nil;
        }

        m_impl->passDescriptor = nil;
        m_impl->activeColorTargets.clear();
        m_impl->activeDepthTarget = nullptr;
        m_impl->renderWidth = 1;
        m_impl->renderHeight = 1;
    }

    void* MetalCommandList::GetNativeCommandBuffer() const
    {
        return (__bridge void*)m_impl->commandBuffer;
    }
}
