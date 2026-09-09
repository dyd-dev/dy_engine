#pragma once

#include "RHI/ResourceSet.h"

#include <cstdint>
#include <vector>

struct ID3D12DescriptorHeap;
struct ID3D12Resource;

namespace dy::Backends
{
    struct D3D12ObjectDeleter;
    struct D3D12ResourceSetInternal;

    class D3D12ResourceSet final : public RHI::ResourceSet
    {
    public:
        D3D12ResourceSet(
            const RHI::ResourceSetDesc& desc,
            ID3D12DescriptorHeap* descriptorHeap,
            uint32_t descriptorSize,
            const std::vector<ID3D12Resource*>& resources);

        ID3D12DescriptorHeap* GetNativeDescriptorHeap() const;
        uint32_t GetDescriptorSize() const;
        uint32_t GetNativeResourceCount() const;
        ID3D12Resource* GetNativeResource(uint32_t index) const;

    private:
        friend struct D3D12ObjectDeleter;

        ~D3D12ResourceSet() override;

        D3D12ResourceSetInternal* m_internal = nullptr;
    };
}
