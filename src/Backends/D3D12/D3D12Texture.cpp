#include "D3D12Texture.h"
#include "RHI/Enums.h" // Enums.h 인클루드 추가

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <d3d12.h>
#include <wrl.h>

using Microsoft::WRL::ComPtr;

namespace dy::Backends
{
    struct D3D12TextureInternal
    {
        ComPtr<ID3D12Resource> resource;
    };

    // 1. RHI 포맷 -> DXGI 포맷 완벽 매핑
    static DXGI_FORMAT ToDxgiFormat(RHI::Format format)
    {
        switch (format)
        {
        case RHI::Format::R8G8B8A8_UNORM:       return DXGI_FORMAT_R8G8B8A8_UNORM;
        case RHI::Format::R16G16B16A16_FLOAT:   return DXGI_FORMAT_R16G16B16A16_FLOAT;
        case RHI::Format::R32G32B32A32_FLOAT:   return DXGI_FORMAT_R32G32B32A32_FLOAT;
        case RHI::Format::D32_FLOAT:            return DXGI_FORMAT_D32_FLOAT;
        case RHI::Format::D24_UNORM_S8_UINT:    return DXGI_FORMAT_D24_UNORM_S8_UINT;
        case RHI::Format::Unknown:
        default:                                return DXGI_FORMAT_UNKNOWN;
        }
    }

    // 2. RHI Usage(비트마스크) -> D3D12 Flags 매핑
    static D3D12_RESOURCE_FLAGS ToDxgiResourceFlags(RHI::TextureUsage usage)
    {
        D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE;

        // 비트 연산으로 여러 속성이 겹쳐있을 경우를 모두 처리
        if (static_cast<uint32_t>(usage) & static_cast<uint32_t>(RHI::TextureUsage::RenderTarget))
            flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

        if (static_cast<uint32_t>(usage) & static_cast<uint32_t>(RHI::TextureUsage::DepthStencil))
            flags |= D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

        if (static_cast<uint32_t>(usage) & static_cast<uint32_t>(RHI::TextureUsage::UnorderedAccess))
            flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

        // ShaderResource는 기본적으로 허용되므로 D3D12에서는 특별한 플래그를 추가하지 않습니다.
        return flags;
    }

    D3D12Texture::D3D12Texture(void* nativeDevice, const RHI::TextureDesc& desc)
        : m_desc(desc)
    {
        m_internal = new D3D12TextureInternal();
        ID3D12Device* device = static_cast<ID3D12Device*>(nativeDevice);

        D3D12_HEAP_PROPERTIES heapProps = {};
        heapProps.Type = D3D12_HEAP_TYPE_DEFAULT; // 텍스처는 GPU 전용 메모리(Default Heap)에 생성

        D3D12_RESOURCE_DESC resourceDesc = {};
        resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        resourceDesc.Alignment = 0;
        resourceDesc.Width = desc.width;
        resourceDesc.Height = desc.height;
        resourceDesc.DepthOrArraySize = static_cast<UINT16>(desc.depthOrArraySize);
        resourceDesc.MipLevels = static_cast<UINT16>(desc.mipLevels);
        resourceDesc.Format = ToDxgiFormat(desc.format); // 헬퍼 함수 적용
        resourceDesc.SampleDesc.Count = 1;
        resourceDesc.SampleDesc.Quality = 0;
        resourceDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        resourceDesc.Flags = ToDxgiResourceFlags(desc.usage); // 헬퍼 함수 적용

        // GPU 메모리에 할당
        device->CreateCommittedResource(
            &heapProps,
            D3D12_HEAP_FLAG_NONE,
            &resourceDesc,
            D3D12_RESOURCE_STATE_COPY_DEST, // 나중에 데이터를 복사해 넣기 위해 COPY_DEST 상태로 세팅
            nullptr,
            IID_PPV_ARGS(&m_internal->resource)
        );
    }

    D3D12Texture::~D3D12Texture()
    {
        delete m_internal;
    }

    uint32_t D3D12Texture::GetWidth() const { return m_desc.width; }
    uint32_t D3D12Texture::GetHeight() const { return m_desc.height; }
    RHI::Format D3D12Texture::GetFormat() const { return m_desc.format; }

    void* D3D12Texture::GetNativeResource() const
    {
        return m_internal->resource.Get();
    }
}