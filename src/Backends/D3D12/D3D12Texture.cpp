#include "D3D12Texture.h"

#include "d3dx12.h"

#include <d3d12.h>
#include <wrl.h>

using Microsoft::WRL::ComPtr;

namespace dy::Backends
{
    struct D3D12TextureInternal
    {
        ComPtr<ID3D12Resource> resource;
        ComPtr<ID3D12DescriptorHeap> rtvHeap;
        ComPtr<ID3D12DescriptorHeap> dsvHeap;
        D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = {};
        D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle = {};
        D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
        bool hasRTV = false;
        bool hasDSV = false;
        uint32_t globalSrvIndex = 0xFFFFFFFFu;
    };

    namespace
    {
        bool HasUsage(RHI::TextureUsage usage, RHI::TextureUsage flag)
        {
            return (static_cast<uint32_t>(usage) & static_cast<uint32_t>(flag)) != 0u;
        }

        D3D12_RESOURCE_FLAGS ToResourceFlags(RHI::TextureUsage usage)
        {
            D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE;
            if (HasUsage(usage, RHI::TextureUsage::RenderTarget)) flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
            if (HasUsage(usage, RHI::TextureUsage::DepthStencil)) flags |= D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
            if (HasUsage(usage, RHI::TextureUsage::Storage)) flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            return flags;
        }

        DXGI_FORMAT ToResourceFormat(RHI::Format format, RHI::TextureUsage usage)
        {
            if (HasUsage(usage, RHI::TextureUsage::DepthStencil) &&
                HasUsage(usage, RHI::TextureUsage::ShaderResource))
            {
                if (format == RHI::Format::D32_FLOAT) return DXGI_FORMAT_R32_TYPELESS;
                if (format == RHI::Format::D24_UNORM_S8_UINT) return DXGI_FORMAT_R24G8_TYPELESS;
            }
            return static_cast<DXGI_FORMAT>(D3D12Texture::ToDxgiFormat(format));
        }

        D3D12_RESOURCE_STATES InitialState(RHI::TextureUsage usage)
        {
            if (HasUsage(usage, RHI::TextureUsage::DepthStencil)) return D3D12_RESOURCE_STATE_DEPTH_WRITE;
            if (HasUsage(usage, RHI::TextureUsage::RenderTarget)) return D3D12_RESOURCE_STATE_RENDER_TARGET;
            return D3D12_RESOURCE_STATE_COPY_DEST;
        }
    }

    uint32_t D3D12Texture::ToDxgiFormat(RHI::Format format)
    {
        switch (format)
        {
        case RHI::Format::R8G8B8A8_UNORM: return DXGI_FORMAT_R8G8B8A8_UNORM;
        case RHI::Format::B8G8R8A8_UNORM: return DXGI_FORMAT_B8G8R8A8_UNORM;
        case RHI::Format::R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        case RHI::Format::B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        case RHI::Format::R16G16B16A16_FLOAT: return DXGI_FORMAT_R16G16B16A16_FLOAT;
        case RHI::Format::R32G32B32A32_FLOAT: return DXGI_FORMAT_R32G32B32A32_FLOAT;
        case RHI::Format::D32_FLOAT: return DXGI_FORMAT_D32_FLOAT;
        case RHI::Format::D24_UNORM_S8_UINT: return DXGI_FORMAT_D24_UNORM_S8_UINT;
        case RHI::Format::R32_UINT: return DXGI_FORMAT_R32_UINT;
        case RHI::Format::R16_UINT: return DXGI_FORMAT_R16_UINT;
        case RHI::Format::Unknown:
        default: return DXGI_FORMAT_UNKNOWN;
        }
    }

    uint32_t D3D12Texture::ToDxgiShaderResourceFormat(RHI::Format format)
    {
        switch (format)
        {
        case RHI::Format::D32_FLOAT: return DXGI_FORMAT_R32_FLOAT;
        case RHI::Format::D24_UNORM_S8_UINT: return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
        default: return ToDxgiFormat(format);
        }
    }

    uint32_t D3D12Texture::ToDxgiDepthStencilFormat(RHI::Format format)
    {
        switch (format)
        {
        case RHI::Format::D32_FLOAT: return DXGI_FORMAT_D32_FLOAT;
        case RHI::Format::D24_UNORM_S8_UINT: return DXGI_FORMAT_D24_UNORM_S8_UINT;
        default: return ToDxgiFormat(format);
        }
    }

    D3D12Texture::D3D12Texture(ID3D12Device* device, const RHI::TextureDesc& desc)
        : RHI::ITexture(desc)
        , m_internal(new D3D12TextureInternal())
    {
        CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_DEFAULT);

        D3D12_RESOURCE_DESC resourceDesc = {};
        resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        resourceDesc.Width = desc.width;
        resourceDesc.Height = desc.height;
        resourceDesc.DepthOrArraySize = static_cast<UINT16>(desc.depthOrArraySize);
        resourceDesc.MipLevels = static_cast<UINT16>(desc.mipLevels);
        resourceDesc.Format = ToResourceFormat(desc.format, desc.usage);
        resourceDesc.SampleDesc.Count = 1;
        resourceDesc.SampleDesc.Quality = 0;
        resourceDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        resourceDesc.Flags = ToResourceFlags(desc.usage);

        D3D12_CLEAR_VALUE optimizedClearValue = {};
        D3D12_CLEAR_VALUE* optimizedClearValuePtr = nullptr;
        if (HasUsage(desc.usage, RHI::TextureUsage::DepthStencil))
        {
            optimizedClearValue.Format = static_cast<DXGI_FORMAT>(ToDxgiDepthStencilFormat(desc.format));
            optimizedClearValue.DepthStencil.Depth = 1.0f;
            optimizedClearValue.DepthStencil.Stencil = 0;
            optimizedClearValuePtr = &optimizedClearValue;
        }

        m_internal->state = InitialState(desc.usage);
        device->CreateCommittedResource(
            &heapProps,
            D3D12_HEAP_FLAG_NONE,
            &resourceDesc,
            m_internal->state,
            optimizedClearValuePtr,
            IID_PPV_ARGS(&m_internal->resource));

        if (m_internal->resource && HasUsage(desc.usage, RHI::TextureUsage::RenderTarget))
        {
            D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
            heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
            heapDesc.NumDescriptors = 1;
            device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&m_internal->rtvHeap));
            if (m_internal->rtvHeap)
            {
                m_internal->rtvHandle = m_internal->rtvHeap->GetCPUDescriptorHandleForHeapStart();
                device->CreateRenderTargetView(m_internal->resource.Get(), nullptr, m_internal->rtvHandle);
                m_internal->hasRTV = true;
            }
        }

        if (m_internal->resource && HasUsage(desc.usage, RHI::TextureUsage::DepthStencil))
        {
            D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
            heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
            heapDesc.NumDescriptors = 1;
            device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&m_internal->dsvHeap));
            if (m_internal->dsvHeap)
            {
                D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
                dsvDesc.Format = static_cast<DXGI_FORMAT>(ToDxgiDepthStencilFormat(desc.format));
                dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
                m_internal->dsvHandle = m_internal->dsvHeap->GetCPUDescriptorHandleForHeapStart();
                device->CreateDepthStencilView(m_internal->resource.Get(), &dsvDesc, m_internal->dsvHandle);
                m_internal->hasDSV = true;
            }
        }
    }

    D3D12Texture::D3D12Texture(ID3D12Resource* resource, const RHI::TextureDesc& desc)
        : RHI::ITexture(desc)
        , m_internal(new D3D12TextureInternal())
    {
        m_internal->resource = resource;
        m_internal->state = HasUsage(desc.usage, RHI::TextureUsage::RenderTarget)
            ? D3D12_RESOURCE_STATE_PRESENT
            : D3D12_RESOURCE_STATE_COMMON;
    }

    D3D12Texture::~D3D12Texture()
    {
        delete m_internal;
    }

    void* D3D12Texture::GetNativeResource() const
    {
        return m_internal->resource.Get();
    }

    bool D3D12Texture::HasRenderTargetView() const
    {
        return m_internal->hasRTV;
    }

    bool D3D12Texture::HasDepthStencilView() const
    {
        return m_internal->hasDSV;
    }

    size_t D3D12Texture::GetRenderTargetViewHandlePtr() const
    {
        return m_internal->rtvHandle.ptr;
    }

    size_t D3D12Texture::GetDepthStencilViewHandlePtr() const
    {
        return m_internal->dsvHandle.ptr;
    }

    uint32_t D3D12Texture::GetResourceState() const
    {
        return static_cast<uint32_t>(m_internal->state);
    }

    void D3D12Texture::SetResourceState(uint32_t state)
    {
        m_internal->state = static_cast<D3D12_RESOURCE_STATES>(state);
    }

    uint32_t D3D12Texture::GetGlobalSrvIndex() const
    {
        return m_internal->globalSrvIndex;
    }

    void D3D12Texture::SetGlobalSrvIndex(uint32_t index)
    {
        m_internal->globalSrvIndex = index;
    }
}
