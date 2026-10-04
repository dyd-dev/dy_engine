#pragma once

#include "../../RHI/UploadPool.h"

#include <d3d12.h>
#include <wrl.h>
#include <limits>
#include <memory>

namespace dyf::Backends
{
    struct D3D12UploadPage
    {
        uint64_t capacity = 0;
        uint8_t* mapped = nullptr;
        Microsoft::WRL::ComPtr<ID3D12Resource> resource;

        ~D3D12UploadPage()
        {
            if (mapped != nullptr) resource->Unmap(0, nullptr);
        }
    };

    using D3D12UploadPool = RHI::Detail::UploadPagePool<D3D12UploadPage>;

    inline std::shared_ptr<D3D12UploadPool> MakeD3D12UploadPool(ID3D12Device* device)
    {
        Microsoft::WRL::ComPtr<ID3D12Device> retainedDevice = device;
        return std::make_shared<D3D12UploadPool>(
            [retainedDevice](uint64_t capacity) -> std::unique_ptr<D3D12UploadPage>
            {
                if (!retainedDevice || capacity == 0 ||
                    capacity > std::numeric_limits<SIZE_T>::max()) return {};
                auto page = std::make_unique<D3D12UploadPage>();
                D3D12_HEAP_PROPERTIES heap{};
                heap.Type = D3D12_HEAP_TYPE_UPLOAD;
                D3D12_RESOURCE_DESC desc{};
                desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
                desc.Width = capacity;
                desc.Height = 1;
                desc.DepthOrArraySize = 1;
                desc.MipLevels = 1;
                desc.SampleDesc.Count = 1;
                desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
                if (FAILED(retainedDevice->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE,
                        &desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                        IID_PPV_ARGS(&page->resource)))) return {};
                const D3D12_RANGE noReads{0, 0};
                if (FAILED(page->resource->Map(0, &noReads,
                        reinterpret_cast<void**>(&page->mapped)))) return {};
                page->capacity = capacity;
                return page;
            });
    }
}
