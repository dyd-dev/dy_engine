#include "D3D12ResourceSet.h"

#include <d3d12.h>
#include <wrl.h>

using Microsoft::WRL::ComPtr;

namespace dy::Backends
{
    struct D3D12ResourceSetInternal
    {
        ComPtr<ID3D12DescriptorHeap> descriptorHeap;
        std::vector<ComPtr<ID3D12Resource>> resources;
        uint32_t descriptorSize = 0;
    };

    D3D12ResourceSet::D3D12ResourceSet(
        const RHI::ResourceSetDesc& desc,
        ID3D12DescriptorHeap* descriptorHeap,
        uint32_t descriptorSize,
        const std::vector<ID3D12Resource*>& resources)
        : RHI::ResourceSet(desc)
        , m_internal(new D3D12ResourceSetInternal())
    {
        m_internal->descriptorHeap = descriptorHeap;
        m_internal->descriptorSize = descriptorSize;
        m_internal->resources.reserve(resources.size());
        for (ID3D12Resource* resource : resources)
        {
            m_internal->resources.emplace_back(resource);
        }
    }

    D3D12ResourceSet::~D3D12ResourceSet()
    {
        delete m_internal;
    }

    ID3D12DescriptorHeap* D3D12ResourceSet::GetNativeDescriptorHeap() const
    {
        return m_internal->descriptorHeap.Get();
    }

    uint32_t D3D12ResourceSet::GetDescriptorSize() const
    {
        return m_internal->descriptorSize;
    }

    uint32_t D3D12ResourceSet::GetNativeResourceCount() const
    {
        return static_cast<uint32_t>(m_internal->resources.size());
    }

    ID3D12Resource* D3D12ResourceSet::GetNativeResource(uint32_t index) const
    {
        return index < m_internal->resources.size()
            ? m_internal->resources[index].Get()
            : nullptr;
    }
}
