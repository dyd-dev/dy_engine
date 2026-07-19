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
        : RHI::ITexture(desc)
        , m_impl(new Impl())
    {
        id<MTLDevice> mtlDevice = (__bridge id<MTLDevice>)device;

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
        if((desc.usage & RHI::TextureUsage::DepthStencil) != RHI::TextureUsage::None)
            mtlUsage |= MTLTextureUsageRenderTarget;
        if((desc.usage & RHI::TextureUsage::Storage) != RHI::TextureUsage::None)
            mtlUsage |= MTLTextureUsageShaderWrite;

        texDesc.usage = mtlUsage;

        m_impl->texture = [mtlDevice newTextureWithDescriptor:texDesc];
        // 텍스처 초기 데이터는 IDevice::UpdateTexture 단일 경로로 업로드한다(생성-시 초기화 경로 폐기).
    }

    MetalTexture::~MetalTexture()
    {
        delete m_impl;
    }

    void* MetalTexture::GetNativeTexture() const
    {
        return (__bridge void*)m_impl->texture;
    }
    void MetalTexture::SetNativeTexture(void* texture)
    {
        m_impl->texture = (__bridge id<MTLTexture>)texture;
        if(m_impl->texture != nil)
        {
            RHI::TextureDesc desc = GetDesc();
            desc.width = static_cast<uint32_t>(m_impl->texture.width);
            desc.height = static_cast<uint32_t>(m_impl->texture.height);
            SetDesc(desc);
        }
    }
}

