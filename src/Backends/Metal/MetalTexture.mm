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
	namespace
	{
		[[nodiscard]] bool HasUsage(RHI::TextureUsage value, RHI::TextureUsage usage)
		{
			return (value & usage) != RHI::TextureUsage::None;
		}

		[[nodiscard]] bool IsDepthFormat(RHI::Format format)
		{
			return format == RHI::Format::D32_FLOAT ||
				format == RHI::Format::D24_UNORM_S8_UINT;
		}

		[[nodiscard]] uint32_t MaximumMipCount(uint32_t width, uint32_t height)
		{
			uint32_t dimension = width > height ? width : height;
			uint32_t result = 0;
			while(dimension != 0)
			{
				++result;
				dimension >>= 1u;
			}
			return result;
		}
	}

    struct MetalTexture::Impl
    {
		id<MTLTexture> ownedTexture = nil;
		__unsafe_unretained id<MTLTexture> texture = nil;
        bool swapchainImage = false;
    };

    // RHI Format → MTLPixelFormat 변환
    static MTLPixelFormat ToMTLFormat(RHI::Format format)
    {
        switch(format)
        {
            case RHI::Format::R8G8B8A8_UNORM:        return MTLPixelFormatRGBA8Unorm;
			case RHI::Format::B8G8R8A8_UNORM:        return MTLPixelFormatBGRA8Unorm;
			case RHI::Format::R8G8B8A8_UNORM_SRGB:   return MTLPixelFormatRGBA8Unorm_sRGB;
			case RHI::Format::B8G8R8A8_UNORM_SRGB:   return MTLPixelFormatBGRA8Unorm_sRGB;
            case RHI::Format::R16G16B16A16_FLOAT:    return MTLPixelFormatRGBA16Float;
			case RHI::Format::R32G32_FLOAT:          return MTLPixelFormatRG32Float;
            case RHI::Format::R32G32B32A32_FLOAT:    return MTLPixelFormatRGBA32Float;
            case RHI::Format::D32_FLOAT:             return MTLPixelFormatDepth32Float;
            case RHI::Format::D24_UNORM_S8_UINT:     return MTLPixelFormatDepth24Unorm_Stencil8;
			case RHI::Format::R32_UINT:              return MTLPixelFormatR32Uint;
			case RHI::Format::R16_UINT:              return MTLPixelFormatR16Uint;
            default:                                 return MTLPixelFormatInvalid;
        }
    }

    MetalTexture::MetalTexture(const RHI::TextureDesc& desc)
        : RHI::Texture(desc)
        , m_impl(new Impl())
		, m_states(
			static_cast<size_t>(desc.mipLevels) * desc.depthOrArraySize,
			RHI::ResourceState::Present)
    {
        m_impl->swapchainImage = true;
    }

    MetalTexture::MetalTexture(const RHI::TextureDesc& desc, void* device)
		: RHI::Texture(desc)
		, m_impl(new Impl())
		, m_states(
			static_cast<size_t>(desc.mipLevels) * desc.depthOrArraySize,
			RHI::ResourceState::Undefined)
    {
        m_impl->swapchainImage = false;
        id<MTLDevice> mtlDevice = (__bridge id<MTLDevice>)device;
		const MTLPixelFormat pixelFormat = ToMTLFormat(desc.format);
		if(mtlDevice == nil || desc.width == 0 || desc.height == 0 ||
			desc.depthOrArraySize == 0 || desc.mipLevels == 0 ||
			desc.mipLevels > MaximumMipCount(desc.width, desc.height) ||
			desc.usage == RHI::TextureUsage::None || pixelFormat == MTLPixelFormatInvalid ||
			(IsDepthFormat(desc.format) && HasUsage(desc.usage, RHI::TextureUsage::RenderTarget)) ||
			(!IsDepthFormat(desc.format) && HasUsage(desc.usage, RHI::TextureUsage::DepthStencil)))
		{
			return;
		}

        MTLTextureDescriptor* texDesc = [MTLTextureDescriptor new];
        texDesc.width       = desc.width;
        texDesc.height      = desc.height;
		texDesc.depth       = 1;
		texDesc.arrayLength = desc.depthOrArraySize;
		texDesc.textureType = desc.depthOrArraySize == 1
			? MTLTextureType2D : MTLTextureType2DArray;
        texDesc.pixelFormat = pixelFormat;
        texDesc.mipmapLevelCount = desc.mipLevels;
		texDesc.storageMode = MTLStorageModePrivate;

        // TextureUsage → MTLTextureUsage 변환
        MTLTextureUsage mtlUsage = MTLTextureUsageUnknown;
        if((desc.usage & RHI::TextureUsage::ShaderResource) != RHI::TextureUsage::None)
            mtlUsage |= MTLTextureUsageShaderRead;
        if((desc.usage & RHI::TextureUsage::RenderTarget) != RHI::TextureUsage::None)
            mtlUsage |= MTLTextureUsageRenderTarget;
        if((desc.usage & RHI::TextureUsage::DepthStencil) != RHI::TextureUsage::None)
            mtlUsage |= MTLTextureUsageRenderTarget;
        if((desc.usage & RHI::TextureUsage::Storage) != RHI::TextureUsage::None)
			mtlUsage |= MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;

        texDesc.usage = mtlUsage;
		texDesc.usage |= MTLTextureUsagePixelFormatView;

		m_impl->ownedTexture = [mtlDevice newTextureWithDescriptor:texDesc];
		m_impl->texture = m_impl->ownedTexture;
#if !__has_feature(objc_arc)
        [texDesc release];
#endif
        // 텍스처 초기 데이터는 IDevice::UpdateTexture 단일 경로로 업로드한다(생성-시 초기화 경로 폐기).
    }

    MetalTexture::~MetalTexture()
    {
#if !__has_feature(objc_arc)
		[m_impl->ownedTexture release];
#endif
		m_impl->ownedTexture = nil;
        m_impl->texture = nil;
        delete m_impl;
    }

    void* MetalTexture::GetNativeTexture() const
    {
		return m_impl == nullptr ? nullptr : (__bridge void*)m_impl->texture;
    }

    bool MetalTexture::IsSwapchainImage() const
    {
        return m_impl->swapchainImage;
    }

	RHI::ResourceState MetalTexture::GetState(
		uint32_t mipLevel,
		uint32_t arrayLayer) const
	{
		if(mipLevel >= GetDesc().mipLevels || arrayLayer >= GetDesc().depthOrArraySize)
			return RHI::ResourceState::Undefined;
		return m_states[static_cast<size_t>(arrayLayer) * GetDesc().mipLevels + mipLevel];
	}

	void MetalTexture::SetState(
		uint32_t mipLevel,
		uint32_t arrayLayer,
		RHI::ResourceState state)
	{
		if(mipLevel >= GetDesc().mipLevels || arrayLayer >= GetDesc().depthOrArraySize) return;
		m_states[static_cast<size_t>(arrayLayer) * GetDesc().mipLevels + mipLevel] = state;
	}

    void MetalTexture::SetBackBuffer(void* texture, const RHI::TextureDesc& desc)
    {
#if !__has_feature(objc_arc)
		[m_impl->ownedTexture release];
#endif
		m_impl->ownedTexture = nil;
        m_impl->swapchainImage = true;
        m_impl->texture = (__bridge id<MTLTexture>)texture;
        SetDesc(desc);
    }
}
