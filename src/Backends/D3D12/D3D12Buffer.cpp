#include "D3D12Buffer.h"

#include <d3d12.h>
#include <wrl.h>

using Microsoft::WRL::ComPtr;

namespace dy::Backends
{
    namespace
    {
        bool HasUsage(RHI::BufferUsage usage, RHI::BufferUsage flag)
        {
            return (static_cast<uint32_t>(usage) & static_cast<uint32_t>(flag)) != 0;
        }

        D3D12_RESOURCE_STATES ToNativeState(RHI::ResourceState state)
        {
            switch (state)
            {
            case RHI::ResourceState::CopyDestination: return D3D12_RESOURCE_STATE_COPY_DEST;
            case RHI::ResourceState::VertexBuffer:
            case RHI::ResourceState::ConstantBuffer:
                return D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
            case RHI::ResourceState::IndexBuffer: return D3D12_RESOURCE_STATE_INDEX_BUFFER;
            case RHI::ResourceState::ShaderResource:
                return D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            case RHI::ResourceState::UnorderedAccess: return D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            case RHI::ResourceState::Undefined:
            case RHI::ResourceState::Common:
            default:
                return D3D12_RESOURCE_STATE_COMMON;
            }
        }
    }

    struct D3D12BufferInternal
    {
        ComPtr<ID3D12Resource> resource;
    };

    D3D12Buffer::D3D12Buffer(void* nativeDevice, const RHI::BufferDesc& desc)
        : RHI::Buffer(desc)
        , m_internal(new D3D12BufferInternal())
        , m_state(desc.initialState)
    {
        auto* device = static_cast<ID3D12Device*>(nativeDevice);
        if (device == nullptr || desc.size == 0) return;

        D3D12_HEAP_PROPERTIES heapProperties = {};
        heapProperties.Type = D3D12_HEAP_TYPE_DEFAULT;

        D3D12_RESOURCE_DESC resourceDesc = {};
        resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        resourceDesc.Width = HasUsage(desc.usage, RHI::BufferUsage::Constant)
            ? (static_cast<uint64_t>(desc.size) +
                D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT - 1) &
                ~(static_cast<uint64_t>(
                    D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT) - 1)
            : desc.size;
        resourceDesc.Height = 1;
        resourceDesc.DepthOrArraySize = 1;
        resourceDesc.MipLevels = 1;
        resourceDesc.Format = DXGI_FORMAT_UNKNOWN;
        resourceDesc.SampleDesc.Count = 1;
        resourceDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        resourceDesc.Flags = HasUsage(desc.usage, RHI::BufferUsage::Storage)
            ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
            : D3D12_RESOURCE_FLAG_NONE;

        device->CreateCommittedResource(
            &heapProperties,
            D3D12_HEAP_FLAG_NONE,
            &resourceDesc,
            ToNativeState(desc.initialState),
            nullptr,
            IID_PPV_ARGS(&m_internal->resource));
    }

    D3D12Buffer::~D3D12Buffer()
    {
        delete m_internal;
    }

    void* D3D12Buffer::GetNativeResource() const
    {
        return m_internal == nullptr ? nullptr : m_internal->resource.Get();
    }

    RHI::ResourceState D3D12Buffer::GetState() const
    {
        return m_state;
    }

    void D3D12Buffer::SetState(RHI::ResourceState state)
    {
        m_state = state;
    }
}
