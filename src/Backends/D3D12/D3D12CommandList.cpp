#include "D3D12CommandList.h"

#include "D3D12Buffer.h"
#include "D3D12PipelineState.h"
#include "D3D12Texture.h"

#include <d3d12.h>
#include <wrl.h>

using Microsoft::WRL::ComPtr;

namespace dy::Backends
{
    namespace
    {
        constexpr uint32_t kBaseColorTextureBinding = 0; // DY_RENDERER_BINDING_BASE_COLOR_TEXTURE
        constexpr uint32_t kShadowMapBinding = 2; // DY_RENDERER_BINDING_SHADOW_MAP
        constexpr uint32_t kMetallicRoughnessTextureBinding = 6;
        constexpr uint32_t kNormalTextureBinding = 7;
        constexpr uint32_t kOcclusionTextureBinding = 8;
        constexpr uint32_t kEmissiveTextureBinding = 9;
        constexpr uint32_t kBindlessTransformStorageBinding = 11;
        constexpr uint32_t kBaseColorTextureRootParameter = 1;
        constexpr uint32_t kShadowMapSrvRootParameter = 5;
        constexpr uint32_t kMetallicRoughnessTextureRootParameter = 6;
        constexpr uint32_t kNormalTextureRootParameter = 7;
        constexpr uint32_t kOcclusionTextureRootParameter = 8;
        constexpr uint32_t kEmissiveTextureRootParameter = 9;
        constexpr uint32_t kBindlessTransformStorageRootParameter = 4;

        void TransitionTexture(ID3D12GraphicsCommandList* commandList, D3D12Texture* texture, D3D12_RESOURCE_STATES after)
        {
            if (commandList == nullptr || texture == nullptr) return;
            ID3D12Resource* resource = static_cast<ID3D12Resource*>(texture->GetNativeResource());
            if (resource == nullptr) return;

            const D3D12_RESOURCE_STATES before = static_cast<D3D12_RESOURCE_STATES>(texture->GetResourceState());
            if (before == after) return;

            D3D12_RESOURCE_BARRIER barrier = {};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.pResource = resource;
            barrier.Transition.StateBefore = before;
            barrier.Transition.StateAfter = after;
            barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            commandList->ResourceBarrier(1, &barrier);
            texture->SetResourceState(after);
        }

        void BindTextureDescriptorTable(
            ID3D12GraphicsCommandList* commandList,
            ID3D12DescriptorHeap* descriptorHeap,
            uint32_t descriptorSize,
            uint32_t rootParameter,
            D3D12Texture* texture)
        {
            if (commandList == nullptr ||
                descriptorHeap == nullptr ||
                texture == nullptr ||
                texture->GetGlobalSrvIndex() == 0xFFFFFFFFu)
            {
                return;
            }

            D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle = descriptorHeap->GetGPUDescriptorHandleForHeapStart();
            gpuHandle.ptr += static_cast<UINT64>(texture->GetGlobalSrvIndex()) * descriptorSize;
            commandList->SetGraphicsRootDescriptorTable(rootParameter, gpuHandle);
        }
    }

    struct D3D12CommandListInternal
    {
        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12GraphicsCommandList> commandList;
        ComPtr<ID3D12Resource> backBuffer;
        D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = {};
        D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle = {};
        bool hasDSV = false;
        ID3D12Device* device = nullptr;
        ID3D12DescriptorHeap* globalDescriptorHeap = nullptr;
        uint32_t srvDescriptorSize = 0;
        D3D12Texture* backBufferTexture = nullptr; // 상태 추적기(GetBackBuffer 가 돌려주는 래퍼와 동일 리소스)
    };

    D3D12CommandList::D3D12CommandList(void* nativeDevice, void* nativeBackBuffer, size_t rtvHandlePtr, void* globalDescriptorHeap, uint32_t srvDescriptorSize)
    {
        m_internal = new D3D12CommandListInternal();
        ID3D12Device* device = static_cast<ID3D12Device*>(nativeDevice);

        m_internal->backBuffer = static_cast<ID3D12Resource*>(nativeBackBuffer);
        m_internal->rtvHandle.ptr = rtvHandlePtr;
        m_internal->device = device;
        m_internal->globalDescriptorHeap = static_cast<ID3D12DescriptorHeap*>(globalDescriptorHeap);
        m_internal->srvDescriptorSize = srvDescriptorSize;

        device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_internal->allocator));
        device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_internal->allocator.Get(), nullptr, IID_PPV_ARGS(&m_internal->commandList));
        m_internal->commandList->Close();
    }

    D3D12CommandList::~D3D12CommandList()
    {
        delete m_internal;
    }

    void* D3D12CommandList::GetNativeList()
    {
        return m_internal->commandList.Get();
    }

    void D3D12CommandList::Reset()
    {
        m_internal->allocator->Reset();
        m_internal->commandList->Reset(m_internal->allocator.Get(), nullptr);
    }

    void D3D12CommandList::Close()
    {
        // 백버퍼를 PRESENT 로 전이. 추적기(D3D12Texture)를 거쳐 상태를 갱신해야
        // 다음 프레임 재사용 시 SetRenderTargets 가 올바른 before-state 로 배리어를 친다.
        if (m_internal->backBufferTexture != nullptr)
        {
            TransitionTexture(m_internal->commandList.Get(), m_internal->backBufferTexture, D3D12_RESOURCE_STATE_PRESENT);
        }
        else
        {
            D3D12_RESOURCE_BARRIER barrier = {};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.pResource = m_internal->backBuffer.Get();
            barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
            barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
            barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            m_internal->commandList->ResourceBarrier(1, &barrier);
        }
        m_internal->commandList->Close();
    }

    void D3D12CommandList::SetBackBufferTexture(RHI::ITexture* texture)
    {
        m_internal->backBufferTexture = static_cast<D3D12Texture*>(texture);
    }

    void D3D12CommandList::ClearColor(RHI::ITexture* renderTarget, float r, float g, float b, float a)
    {
        D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = m_internal->rtvHandle;
        if (renderTarget != nullptr)
        {
            auto* d3dTarget = static_cast<D3D12Texture*>(renderTarget);
            TransitionTexture(m_internal->commandList.Get(), d3dTarget, D3D12_RESOURCE_STATE_RENDER_TARGET);
            if (d3dTarget->HasRenderTargetView())
            {
                rtvHandle.ptr = d3dTarget->GetRenderTargetViewHandlePtr();
            }
        }

        const float color[] = { r, g, b, a };
        m_internal->commandList->ClearRenderTargetView(rtvHandle, color, 0, nullptr);
    }

    void D3D12CommandList::ClearDepth(RHI::ITexture* depthStencil, float depth)
    {
        D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle = m_internal->dsvHandle;
        bool hasDSV = m_internal->hasDSV;

        if (depthStencil != nullptr)
        {
            auto* d3dDepth = static_cast<D3D12Texture*>(depthStencil);
            TransitionTexture(m_internal->commandList.Get(), d3dDepth, D3D12_RESOURCE_STATE_DEPTH_WRITE);
            if (d3dDepth->HasDepthStencilView())
            {
                dsvHandle.ptr = d3dDepth->GetDepthStencilViewHandlePtr();
                hasDSV = true;
            }
        }

        if (hasDSV)
        {
            m_internal->commandList->ClearDepthStencilView(dsvHandle, D3D12_CLEAR_FLAG_DEPTH, depth, 0, 0, nullptr);
        }
    }

    void D3D12CommandList::SetDepthStencilView(size_t dsvHandlePtr)
    {
        m_internal->dsvHandle.ptr = dsvHandlePtr;
        m_internal->hasDSV = true;
    }

    void D3D12CommandList::DrawInstanced(uint32_t vertexCount, uint32_t instanceCount, uint32_t startVertex, uint32_t startInstance)
    {
        m_internal->commandList->DrawInstanced(vertexCount, instanceCount, startVertex, startInstance);
    }

    void D3D12CommandList::BindVertexBuffer(RHI::IBuffer* buffer, uint32_t stride, uint32_t offset)
    {
        auto* d3d12Buffer = static_cast<D3D12Buffer*>(buffer);
        if (d3d12Buffer == nullptr) return;

        D3D12_VERTEX_BUFFER_VIEW view = {};
        view.BufferLocation = static_cast<ID3D12Resource*>(d3d12Buffer->GetNativeResource())->GetGPUVirtualAddress() + offset;
        view.SizeInBytes = d3d12Buffer->GetSize() - offset;
        view.StrideInBytes = stride;
        m_internal->commandList->IASetVertexBuffers(0, 1, &view);
    }

    void D3D12CommandList::BindIndexBuffer(RHI::IBuffer* buffer, RHI::Format format, uint32_t offset)
    {
        auto* d3d12Buffer = static_cast<D3D12Buffer*>(buffer);
        if (d3d12Buffer == nullptr) return;

        D3D12_INDEX_BUFFER_VIEW view = {};
        view.BufferLocation = static_cast<ID3D12Resource*>(d3d12Buffer->GetNativeResource())->GetGPUVirtualAddress() + offset;
        view.SizeInBytes = d3d12Buffer->GetSize() - offset;
        view.Format = format == RHI::Format::R16_UINT ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT;
        m_internal->commandList->IASetIndexBuffer(&view);
    }

    void D3D12CommandList::DrawIndexedInstanced(uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex, int32_t vertexOffset, uint32_t firstInstance)
    {
        m_internal->commandList->DrawIndexedInstanced(indexCount, instanceCount, firstIndex, vertexOffset, firstInstance);
    }

    void D3D12CommandList::BindGlobalDescriptors()
    {
        if (m_internal->globalDescriptorHeap == nullptr) return;
        ID3D12DescriptorHeap* heaps[] = { m_internal->globalDescriptorHeap };
        m_internal->commandList->SetDescriptorHeaps(1, heaps);
        m_internal->commandList->SetGraphicsRootDescriptorTable(1, m_internal->globalDescriptorHeap->GetGPUDescriptorHandleForHeapStart());
    }

    void D3D12CommandList::BindGraphicsPipeline(RHI::IPipelineState* pipelineState)
    {
        auto* d3d12PSO = static_cast<D3D12PipelineState*>(pipelineState);
        if (d3d12PSO == nullptr) return;

        m_internal->commandList->SetPipelineState(d3d12PSO->GetNativePSO());
        m_internal->commandList->SetGraphicsRootSignature(d3d12PSO->GetNativeRootSignature());
        m_internal->commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    }

    void D3D12CommandList::BindGeometry(const RHI::GeometryBinding& geometry)
    {
        BindVertexBuffer(geometry.vertexBuffer, geometry.vertexStride, geometry.vertexOffset);
        if (geometry.indexBuffer != nullptr)
        {
            BindIndexBuffer(geometry.indexBuffer, geometry.indexFormat, geometry.indexOffset);
        }
    }

    void D3D12CommandList::BindConstantBuffer(uint32_t binding, RHI::IBuffer* buffer, uint32_t offset, uint32_t size)
    {
        (void)size;
        if (buffer == nullptr) return;

        uint32_t rootParameterIndex = UINT32_MAX;
        if (binding == 1)
        {
            rootParameterIndex = 2;
        }
        else if (binding == 3)
        {
            rootParameterIndex = 3;
        }

        if (rootParameterIndex == UINT32_MAX) return;

        auto* d3d12Buffer = static_cast<D3D12Buffer*>(buffer);
        ID3D12Resource* resource = static_cast<ID3D12Resource*>(d3d12Buffer->GetNativeResource());
        if (resource == nullptr) return;

        m_internal->commandList->SetGraphicsRootConstantBufferView(rootParameterIndex, resource->GetGPUVirtualAddress() + offset);
    }

    void D3D12CommandList::BindTexture(uint32_t binding, RHI::ITexture* texture)
    {
        if (texture == nullptr) return;
        auto* d3dTexture = static_cast<D3D12Texture*>(texture);
        TransitionTexture(m_internal->commandList.Get(), d3dTexture, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

        // 그림자 맵: 전용 SRV 디스크립터 테이블(루트 파라미터 7)에 바인딩.
        // 머티리얼 텍스처는 bindless 글로벌 힙(루트 1)로 가므로 상태 전환만 한다.
        if (binding == kBaseColorTextureBinding)
        {
            // per-draw 베이스컬러: 텍스처의 글로벌 SRV 슬롯에 디스크립터 테이블을 직접 바인딩.
            // (과거의 단일 공유 슬롯 1023 복사 방식은 CopyDescriptorsSimple 이 CPU 즉시 실행이라
            //  한 프레임의 모든 드로우가 같은 슬롯을 덮어써 GPU 실행 시 마지막 텍스처만 샘플되는 버그가 있었음.
            //  루트 범위가 unbounded 라 텍스처별 글로벌 슬롯을 테이블 시작점으로 직접 쓰면 드로우마다 올바른 텍스처가 샘플된다.)
            BindTextureDescriptorTable(
                m_internal->commandList.Get(),
                m_internal->globalDescriptorHeap,
                m_internal->srvDescriptorSize,
                kBaseColorTextureRootParameter,
                d3dTexture);
            return;
        }

        if (binding == kMetallicRoughnessTextureBinding)
        {
            BindTextureDescriptorTable(
                m_internal->commandList.Get(),
                m_internal->globalDescriptorHeap,
                m_internal->srvDescriptorSize,
                kMetallicRoughnessTextureRootParameter,
                d3dTexture);
            return;
        }

        if (binding == kNormalTextureBinding)
        {
            BindTextureDescriptorTable(
                m_internal->commandList.Get(),
                m_internal->globalDescriptorHeap,
                m_internal->srvDescriptorSize,
                kNormalTextureRootParameter,
                d3dTexture);
            return;
        }

        if (binding == kOcclusionTextureBinding)
        {
            BindTextureDescriptorTable(
                m_internal->commandList.Get(),
                m_internal->globalDescriptorHeap,
                m_internal->srvDescriptorSize,
                kOcclusionTextureRootParameter,
                d3dTexture);
            return;
        }

        if (binding == kEmissiveTextureBinding)
        {
            BindTextureDescriptorTable(
                m_internal->commandList.Get(),
                m_internal->globalDescriptorHeap,
                m_internal->srvDescriptorSize,
                kEmissiveTextureRootParameter,
                d3dTexture);
            return;
        }

        if (binding == kShadowMapBinding)
        {
            BindTextureDescriptorTable(
                m_internal->commandList.Get(),
                m_internal->globalDescriptorHeap,
                m_internal->srvDescriptorSize,
                kShadowMapSrvRootParameter,
                d3dTexture);
        }
    }

    void D3D12CommandList::BindStorageBuffer(uint32_t binding, RHI::IBuffer* buffer, uint32_t offset, uint32_t size)
    {
        (void)size;
        if (buffer == nullptr) return;

        uint32_t rootParameterIndex = UINT32_MAX;
        if (binding == kBindlessTransformStorageBinding)
        {
            rootParameterIndex = kBindlessTransformStorageRootParameter;
        }

        if (rootParameterIndex == UINT32_MAX) return;

        auto* d3d12Buffer = static_cast<D3D12Buffer*>(buffer);
        ID3D12Resource* resource = static_cast<ID3D12Resource*>(d3d12Buffer->GetNativeResource());
        if (resource == nullptr) return;

        m_internal->commandList->SetGraphicsRootShaderResourceView(rootParameterIndex, resource->GetGPUVirtualAddress() + offset);
    }

    void D3D12CommandList::SetInlineConstants(uint32_t size, const void* data)
    {
        m_internal->commandList->SetGraphicsRoot32BitConstants(0, size / 4, data, 0);
    }

    void D3D12CommandList::SetRenderTargets(uint32_t numRenderTargets, RHI::ITexture** renderTargets, RHI::ITexture* depthStencil)
    {
        D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = m_internal->rtvHandle;
        D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle = {};
        D3D12_CPU_DESCRIPTOR_HANDLE* rtvPtr = nullptr;
        D3D12_CPU_DESCRIPTOR_HANDLE* dsvPtr = nullptr;
        uint32_t width = 1;
        uint32_t height = 1;

        if (numRenderTargets > 0 && renderTargets != nullptr && renderTargets[0] != nullptr)
        {
            width = renderTargets[0]->GetWidth();
            height = renderTargets[0]->GetHeight();
            auto* d3dTarget = static_cast<D3D12Texture*>(renderTargets[0]);
            TransitionTexture(m_internal->commandList.Get(), d3dTarget, D3D12_RESOURCE_STATE_RENDER_TARGET);
            if (d3dTarget->HasRenderTargetView())
            {
                rtvHandle.ptr = d3dTarget->GetRenderTargetViewHandlePtr();
            }
            rtvPtr = &rtvHandle;
        }

        if (depthStencil != nullptr)
        {
            width = depthStencil->GetWidth();
            height = depthStencil->GetHeight();
            auto* d3dDepth = static_cast<D3D12Texture*>(depthStencil);
            TransitionTexture(m_internal->commandList.Get(), d3dDepth, D3D12_RESOURCE_STATE_DEPTH_WRITE);
            if (d3dDepth->HasDepthStencilView())
            {
                dsvHandle.ptr = d3dDepth->GetDepthStencilViewHandlePtr();
                dsvPtr = &dsvHandle;
            }
        }

        m_internal->commandList->OMSetRenderTargets(rtvPtr != nullptr ? 1u : 0u, rtvPtr, FALSE, dsvPtr);

        D3D12_VIEWPORT viewport = { 0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f };
        D3D12_RECT scissorRect = { 0, 0, static_cast<LONG>(width), static_cast<LONG>(height) };
        m_internal->commandList->RSSetViewports(1, &viewport);
        m_internal->commandList->RSSetScissorRects(1, &scissorRect);
    }

    void D3D12CommandList::SetViewport(const RHI::Viewport& viewport)
    {
        D3D12_VIEWPORT d3dViewport = { viewport.x, viewport.y, viewport.width, viewport.height, viewport.minDepth, viewport.maxDepth };
        m_internal->commandList->RSSetViewports(1, &d3dViewport);
    }

    void D3D12CommandList::SetScissor(const RHI::Rect& rect)
    {
        D3D12_RECT d3dRect = { rect.x, rect.y, rect.x + static_cast<LONG>(rect.width), rect.y + static_cast<LONG>(rect.height) };
        m_internal->commandList->RSSetScissorRects(1, &d3dRect);
    }
}
