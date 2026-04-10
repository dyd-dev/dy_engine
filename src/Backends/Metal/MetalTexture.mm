//
//  MetalTexture.mm
//  
//
//  Created by 정준혁 on 4/8/26.
//

#include "MetalTexture.h"
#import <Metal/Metal.h>

namespace dy::Backends
{
    struct MetalTexture::Impl
    {
        id<MTLTexture> texture = nil;
        uint32_t       width   = 0;
        uint32_t       height  = 0;
        RHI::Format    format  = RHI::Format::Unknown;
    };

    // RHI Format → MTLPixelFormat 변환
    static MTLPixelFormat ToMTLFormat(RHI::Format format)
    {
        switch(format)
        {
            case RHI::Format::R8G8B8A8_UNORM:        return MTLPixelFormatRGBA8Unorm;
            case RHI::Format::R16G16B16A16_FLOAT:    return MTLPixelFormatRGBA16Float;
            case RHI::Format::R32G32B32A32_FLOAT:    return MTLPixelFormatRGBA32Float;
            case RHI::Format::D32_FLOAT:             return MTLPixelFormatDepth32Float;
            case RHI::Format::D24_UNORM_S8_UINT:     return MTLPixelFormatDepth24Unorm_Stencil8;
            default:                                 return MTLPixelFormatInvalid;
        }
    }

    MetalTexture::MetalTexture(const RHI::TextureDesc& desc, void* device)
        : m_impl(new Impl())
    {
        id<MTLDevice> mtlDevice = (__bridge id<MTLDevice>)device;

        m_impl->width  = desc.width;
        m_impl->height = desc.height;
        m_impl->format = desc.format;

        MTLTextureDescriptor* texDesc = [MTLTextureDescriptor new];
        texDesc.width       = desc.width;
        texDesc.height      = desc.height;
        texDesc.pixelFormat = ToMTLFormat(desc.format);
        texDesc.mipmapLevelCount = desc.mipLevels;

        // TextureUsage → MTLTextureUsage 변환
        MTLTextureUsage mtlUsage = MTLTextureUsageUnknown;
        if((desc.usage & RHI::TextureUsage::ShaderResource) != RHI::TextureUsage::None)
            mtlUsage |= MTLTextureUsageShaderRead;
        if((desc.usage & RHI::TextureUsage::RenderTarget) != RHI::TextureUsage::None)
            mtlUsage |= MTLTextureUsageRenderTarget;
        if((desc.usage & RHI::TextureUsage::UnorderedAccess) != RHI::TextureUsage::None)
            mtlUsage |= MTLTextureUsageShaderWrite;

        texDesc.usage = mtlUsage;

        m_impl->texture = [mtlDevice newTextureWithDescriptor:texDesc];
    }

    MetalTexture::~MetalTexture()
    {
        delete m_impl;
    }

    uint32_t    MetalTexture::GetWidth()  const { return m_impl->width;  }
    uint32_t    MetalTexture::GetHeight() const { return m_impl->height; }
    RHI::Format MetalTexture::GetFormat() const { return m_impl->format; }

    void* MetalTexture::GetNativeTexture() const
    {
        return (__bridge void*)m_impl->texture;
    }
}
