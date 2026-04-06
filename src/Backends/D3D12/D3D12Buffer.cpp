#include "D3D12Buffer.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <d3d12.h>
#include <wrl.h>

using Microsoft::WRL::ComPtr;

namespace dy::Backends
{
    struct D3D12BufferInternal
    {
        ComPtr<ID3D12Resource> resource;
    };

    // 생성자에서 ID3D12Device를 받도록 수정해야 합니다 (Device에서 생성하기 위함)
    D3D12Buffer::D3D12Buffer(void* nativeDevice, const RHI::BufferDesc& desc)
        : m_size(desc.size)
    {
        m_internal = new D3D12BufferInternal();
        ID3D12Device* device = static_cast<ID3D12Device*>(nativeDevice);

        // CPU가 접근하기 쉬운 업로드 힙(Upload Heap) 사용
        D3D12_HEAP_PROPERTIES heapProps = {};
        heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;
        heapProps.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
        heapProps.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;

        D3D12_RESOURCE_DESC resourceDesc = {};
        resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        resourceDesc.Alignment = 0;
        resourceDesc.Width = desc.size;
        resourceDesc.Height = 1;
        resourceDesc.DepthOrArraySize = 1;
        resourceDesc.MipLevels = 1;
        resourceDesc.Format = DXGI_FORMAT_UNKNOWN;
        resourceDesc.SampleDesc.Count = 1;
        resourceDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        resourceDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

        device->CreateCommittedResource(
            &heapProps,
            D3D12_HEAP_FLAG_NONE,
            &resourceDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            IID_PPV_ARGS(&m_internal->resource)
        );
    }

    D3D12Buffer::~D3D12Buffer() { delete m_internal; }

    void* D3D12Buffer::Map(uint32_t offset, uint32_t size)
    {
        void* mappedData = nullptr;
        D3D12_RANGE readRange = { 0, 0 }; // CPU에서 읽지는 않음
        m_internal->resource->Map(0, &readRange, &mappedData);
        // offset을 더해서 포인터 반환
        return static_cast<uint8_t*>(mappedData) + offset;
    }

    void D3D12Buffer::Unmap()
    {
        m_internal->resource->Unmap(0, nullptr);
    }

    uint32_t D3D12Buffer::GetSize() const { return m_size; }

    // (선택) 커맨드 리스트가 내부 리소스를 훔쳐볼 수 있도록 하는 함수 추가
    void* D3D12Buffer::GetNativeResource() const { return m_internal->resource.Get(); }
}