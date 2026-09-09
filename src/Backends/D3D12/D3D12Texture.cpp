#include "D3D12Texture.h"

#include <d3d12.h>
#include <limits>
#include <wrl.h>

using Microsoft::WRL::ComPtr;

namespace dy::Backends
{
    namespace
    {
        bool HasUsage(RHI::TextureUsage usage, RHI::TextureUsage flag)
        {
            return (static_cast<uint32_t>(usage) & static_cast<uint32_t>(flag)) != 0;
        }

        D3D12_RESOURCE_FLAGS ToResourceFlags(RHI::TextureUsage usage)
        {
            D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE;
            if (HasUsage(usage, RHI::TextureUsage::RenderTarget))
                flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
            if (HasUsage(usage, RHI::TextureUsage::DepthStencil))
                flags |= D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
            if (HasUsage(usage, RHI::TextureUsage::Storage))
                flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            return flags;
        }

        D3D12_RESOURCE_STATES ToNativeState(RHI::ResourceState state)
        {
            switch (state)
            {
            case RHI::ResourceState::CopyDestination: return D3D12_RESOURCE_STATE_COPY_DEST;
            case RHI::ResourceState::ShaderResource:
                return D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            case RHI::ResourceState::UnorderedAccess: return D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            case RHI::ResourceState::RenderTarget: return D3D12_RESOURCE_STATE_RENDER_TARGET;
            case RHI::ResourceState::DepthRead: return D3D12_RESOURCE_STATE_DEPTH_READ;
            case RHI::ResourceState::DepthWrite: return D3D12_RESOURCE_STATE_DEPTH_WRITE;
            case RHI::ResourceState::Present: return D3D12_RESOURCE_STATE_PRESENT;
            case RHI::ResourceState::Undefined:
            case RHI::ResourceState::Common:
            default:
                return D3D12_RESOURCE_STATE_COMMON;
            }
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
    }

    struct D3D12TextureInternal
    {
        ComPtr<ID3D12Resource> resource;
        ComPtr<ID3D12DescriptorHeap> rtvHeap;
        ComPtr<ID3D12DescriptorHeap> dsvHeap;
        D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = {};
        D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle = {};
        uint32_t rtvDescriptorSize = 0;
        uint32_t dsvDescriptorSize = 0;
        bool swapchainImage = false;
    };

    uint32_t D3D12Texture::ToDxgiFormat(RHI::Format format)
    {
        switch (format)
        {
        case RHI::Format::R8G8B8A8_UNORM: return DXGI_FORMAT_R8G8B8A8_UNORM;
        case RHI::Format::B8G8R8A8_UNORM: return DXGI_FORMAT_B8G8R8A8_UNORM;
        case RHI::Format::R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        case RHI::Format::B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        case RHI::Format::R16G16B16A16_FLOAT: return DXGI_FORMAT_R16G16B16A16_FLOAT;
        case RHI::Format::R32G32_FLOAT: return DXGI_FORMAT_R32G32_FLOAT;
        case RHI::Format::R32G32B32_FLOAT: return DXGI_FORMAT_R32G32B32_FLOAT;
        case RHI::Format::R32G32B32A32_FLOAT: return DXGI_FORMAT_R32G32B32A32_FLOAT;
        case RHI::Format::D32_FLOAT: return DXGI_FORMAT_D32_FLOAT;
        case RHI::Format::D24_UNORM_S8_UINT: return DXGI_FORMAT_D24_UNORM_S8_UINT;
        case RHI::Format::R32_UINT: return DXGI_FORMAT_R32_UINT;
        case RHI::Format::R16_UINT: return DXGI_FORMAT_R16_UINT;
        case RHI::Format::Unknown:
        default:
            return DXGI_FORMAT_UNKNOWN;
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
        : RHI::Texture(desc)
        , m_internal(new D3D12TextureInternal())
        , m_states(
            static_cast<std::size_t>(desc.mipLevels) * desc.depthOrArraySize,
            RHI::ResourceState::Undefined)
    {
        if (device == nullptr || desc.width == 0 || desc.height == 0 ||
            desc.depthOrArraySize == 0 || desc.mipLevels == 0)
        {
            return;
        }

        D3D12_HEAP_PROPERTIES heapProperties = {};
        heapProperties.Type = D3D12_HEAP_TYPE_DEFAULT;

        D3D12_RESOURCE_DESC resourceDesc = {};
        resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        resourceDesc.Width = desc.width;
        resourceDesc.Height = desc.height;
        resourceDesc.DepthOrArraySize = static_cast<UINT16>(desc.depthOrArraySize);
        resourceDesc.MipLevels = static_cast<UINT16>(desc.mipLevels);
        resourceDesc.Format = ToResourceFormat(desc.format, desc.usage);
        resourceDesc.SampleDesc.Count = 1;
        resourceDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        resourceDesc.Flags = ToResourceFlags(desc.usage);
        if (resourceDesc.Format == DXGI_FORMAT_UNKNOWN) return;

        if (FAILED(device->CreateCommittedResource(
                &heapProperties,
                D3D12_HEAP_FLAG_NONE,
                &resourceDesc,
                ToNativeState(RHI::ResourceState::Undefined),
                nullptr,
                IID_PPV_ARGS(&m_internal->resource))))
        {
            return;
        }

        if (HasUsage(desc.usage, RHI::TextureUsage::RenderTarget))
        {
            const uint64_t viewCount = static_cast<uint64_t>(desc.mipLevels) *
                desc.depthOrArraySize;
            if (viewCount > std::numeric_limits<UINT>::max()) return;
            D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
            heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
            heapDesc.NumDescriptors = static_cast<UINT>(viewCount);
            if (SUCCEEDED(device->CreateDescriptorHeap(
                    &heapDesc, IID_PPV_ARGS(&m_internal->rtvHeap))))
            {
                m_internal->rtvHandle =
                    m_internal->rtvHeap->GetCPUDescriptorHandleForHeapStart();
                m_internal->rtvDescriptorSize =
                    device->GetDescriptorHandleIncrementSize(
                        D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
                D3D12_CPU_DESCRIPTOR_HANDLE handle = m_internal->rtvHandle;
                for (uint32_t layer = 0; layer < desc.depthOrArraySize; ++layer)
                {
                    for (uint32_t mip = 0; mip < desc.mipLevels; ++mip)
                    {
                        D3D12_RENDER_TARGET_VIEW_DESC view = {};
                        view.Format = static_cast<DXGI_FORMAT>(ToDxgiFormat(desc.format));
                        if (desc.depthOrArraySize > 1)
                        {
                            view.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
                            view.Texture2DArray.MipSlice = mip;
                            view.Texture2DArray.FirstArraySlice = layer;
                            view.Texture2DArray.ArraySize = 1;
                        }
                        else
                        {
                            view.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
                            view.Texture2D.MipSlice = mip;
                        }
                        device->CreateRenderTargetView(
                            m_internal->resource.Get(), &view, handle);
                        handle.ptr += m_internal->rtvDescriptorSize;
                    }
                }
            }
        }

        if (HasUsage(desc.usage, RHI::TextureUsage::DepthStencil))
        {
            const uint64_t viewCount = static_cast<uint64_t>(desc.mipLevels) *
                desc.depthOrArraySize;
            if (viewCount > std::numeric_limits<UINT>::max() / 2u) return;
            D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
            heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
            heapDesc.NumDescriptors = static_cast<UINT>(viewCount * 2u);
            if (SUCCEEDED(device->CreateDescriptorHeap(
                    &heapDesc, IID_PPV_ARGS(&m_internal->dsvHeap))))
            {
                m_internal->dsvHandle =
                    m_internal->dsvHeap->GetCPUDescriptorHandleForHeapStart();
                m_internal->dsvDescriptorSize =
                    device->GetDescriptorHandleIncrementSize(
                        D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
                for (uint32_t readOnlyIndex = 0; readOnlyIndex < 2; ++readOnlyIndex)
                {
                    D3D12_CPU_DESCRIPTOR_HANDLE handle = m_internal->dsvHandle;
                    handle.ptr += static_cast<SIZE_T>(readOnlyIndex) *
                        static_cast<SIZE_T>(viewCount) *
                        m_internal->dsvDescriptorSize;
                    for (uint32_t layer = 0; layer < desc.depthOrArraySize; ++layer)
                    {
                        for (uint32_t mip = 0; mip < desc.mipLevels; ++mip)
                        {
                            D3D12_DEPTH_STENCIL_VIEW_DESC view = {};
                            view.Format = static_cast<DXGI_FORMAT>(
                                ToDxgiDepthStencilFormat(desc.format));
                            if (readOnlyIndex != 0)
                            {
                                view.Flags = D3D12_DSV_FLAG_READ_ONLY_DEPTH;
                                if (desc.format == RHI::Format::D24_UNORM_S8_UINT)
                                {
                                    view.Flags = static_cast<D3D12_DSV_FLAGS>(
                                        view.Flags |
                                        D3D12_DSV_FLAG_READ_ONLY_STENCIL);
                                }
                            }
                            if (desc.depthOrArraySize > 1)
                            {
                                view.ViewDimension =
                                    D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
                                view.Texture2DArray.MipSlice = mip;
                                view.Texture2DArray.FirstArraySlice = layer;
                                view.Texture2DArray.ArraySize = 1;
                            }
                            else
                            {
                                view.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
                                view.Texture2D.MipSlice = mip;
                            }
                            device->CreateDepthStencilView(
                                m_internal->resource.Get(), &view, handle);
                            handle.ptr += m_internal->dsvDescriptorSize;
                        }
                    }
                }
            }
        }
    }

    D3D12Texture::D3D12Texture(
        ID3D12Resource* resource,
        const RHI::TextureDesc& desc,
        std::size_t rtvHandle,
        bool swapchainImage)
        : RHI::Texture(desc)
        , m_internal(new D3D12TextureInternal())
        , m_states(
            static_cast<std::size_t>(desc.mipLevels) * desc.depthOrArraySize,
            swapchainImage
                ? RHI::ResourceState::Present
                : RHI::ResourceState::Undefined)
    {
        m_internal->resource = resource;
        m_internal->rtvHandle.ptr = rtvHandle;
        m_internal->swapchainImage = swapchainImage;
    }

    D3D12Texture::~D3D12Texture()
    {
        delete m_internal;
    }

    void* D3D12Texture::GetNativeResource() const
    {
        return m_internal == nullptr ? nullptr : m_internal->resource.Get();
    }

    void* D3D12Texture::GetRenderTargetViewHeap() const
    {
        return m_internal == nullptr ? nullptr : m_internal->rtvHeap.Get();
    }

    void* D3D12Texture::GetDepthStencilViewHeap() const
    {
        return m_internal == nullptr ? nullptr : m_internal->dsvHeap.Get();
    }

    std::size_t D3D12Texture::GetRenderTargetViewHandle(
        uint32_t mipLevel,
        uint32_t arrayLayer) const
    {
        if (m_internal == nullptr || mipLevel >= GetDesc().mipLevels ||
            arrayLayer >= GetDesc().depthOrArraySize ||
            m_internal->rtvHandle.ptr == 0)
        {
            return 0;
        }
        const uint64_t index = static_cast<uint64_t>(arrayLayer) *
            GetDesc().mipLevels + mipLevel;
        return m_internal->rtvHandle.ptr +
            index * m_internal->rtvDescriptorSize;
    }

    std::size_t D3D12Texture::GetDepthStencilViewHandle(
        uint32_t mipLevel,
        uint32_t arrayLayer,
        bool readOnly) const
    {
        if (m_internal == nullptr || mipLevel >= GetDesc().mipLevels ||
            arrayLayer >= GetDesc().depthOrArraySize ||
            m_internal->dsvHandle.ptr == 0)
        {
            return 0;
        }
        const uint64_t index = static_cast<uint64_t>(arrayLayer) *
            GetDesc().mipLevels + mipLevel;
        const uint64_t viewCount = static_cast<uint64_t>(GetDesc().mipLevels) *
            GetDesc().depthOrArraySize;
        return m_internal->dsvHandle.ptr +
            (index + (readOnly ? viewCount : 0u)) *
                m_internal->dsvDescriptorSize;
    }

    RHI::ResourceState D3D12Texture::GetState(
        uint32_t mipLevel,
        uint32_t arrayLayer) const
    {
        if (mipLevel >= GetDesc().mipLevels ||
            arrayLayer >= GetDesc().depthOrArraySize)
        {
            return RHI::ResourceState::Undefined;
        }
        return m_states[static_cast<std::size_t>(arrayLayer) *
            GetDesc().mipLevels + mipLevel];
    }

    void D3D12Texture::SetState(
        uint32_t mipLevel,
        uint32_t arrayLayer,
        RHI::ResourceState state)
    {
        if (mipLevel >= GetDesc().mipLevels ||
            arrayLayer >= GetDesc().depthOrArraySize)
        {
            return;
        }
        m_states[static_cast<std::size_t>(arrayLayer) *
            GetDesc().mipLevels + mipLevel] = state;
    }

    bool D3D12Texture::IsSwapchainImage() const
    {
        return m_internal != nullptr && m_internal->swapchainImage;
    }
}
