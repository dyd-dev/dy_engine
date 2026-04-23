#include "D3D12CommandList.h"
#include "D3D12Buffer.h"
#include "D3D12PipelineState.h"
#include "D3D12Texture.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <d3d12.h>
#include <wrl.h>

using Microsoft::WRL::ComPtr;

namespace dy::Backends
{
    struct D3D12CommandListInternal
    {
        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12GraphicsCommandList> commandList;
        ComPtr<ID3D12Resource> backBuffer;
        D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = {};
        ID3D12DescriptorHeap* globalDescriptorHeap = nullptr;
    };

    D3D12CommandList::D3D12CommandList(void* nativeDevice, void* nativeBackBuffer, size_t rtvHandlePtr, void* globalDescriptorHeap)
    {
        m_internal = new D3D12CommandListInternal();
        ID3D12Device* device = static_cast<ID3D12Device*>(nativeDevice);

        m_internal->backBuffer = static_cast<ID3D12Resource*>(nativeBackBuffer);
        m_internal->rtvHandle.ptr = rtvHandlePtr;
        m_internal->globalDescriptorHeap = static_cast<ID3D12DescriptorHeap*>(globalDescriptorHeap);

        device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_internal->allocator));
        device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_internal->allocator.Get(), nullptr, IID_PPV_ARGS(&m_internal->commandList));
        m_internal->commandList->Close();
    }

    D3D12CommandList::~D3D12CommandList() { delete m_internal; }

    void* D3D12CommandList::GetNativeList() { return m_internal->commandList.Get(); }

    void D3D12CommandList::Reset() {
        m_internal->allocator->Reset();
        m_internal->commandList->Reset(m_internal->allocator.Get(), nullptr);
    }

    void D3D12CommandList::Close() { 
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = m_internal->backBuffer.Get();
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        m_internal->commandList->ResourceBarrier(1, &barrier);

        m_internal->commandList->Close();
    }

    // 화면을 지우는 함수
    void D3D12CommandList::ClearColor(RHI::ITexture* renderTarget, float r, float g, float b, float a)
    {
        // 렌더 타겟 상태로 변환 (배리어)
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = m_internal->backBuffer.Get();
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        m_internal->commandList->ResourceBarrier(1, &barrier);

        m_internal->commandList->OMSetRenderTargets(1, &m_internal->rtvHandle, FALSE, nullptr);

        // 색상 칠하기
        const float color[] = { r, g, b, a };
        m_internal->commandList->ClearRenderTargetView(m_internal->rtvHandle, color, 0, nullptr);

    }

    void D3D12CommandList::DrawInstanced(uint32_t vertexCount, uint32_t instanceCount, uint32_t startVertex, uint32_t startInstance) {
        m_internal->commandList->DrawInstanced(vertexCount, instanceCount, startVertex, startInstance);
    }

    void D3D12CommandList::BindGlobalDescriptorHeap() {
        if (m_internal->globalDescriptorHeap) {
            ID3D12DescriptorHeap* heaps[] = { m_internal->globalDescriptorHeap };
            m_internal->commandList->SetDescriptorHeaps(1, heaps);
            m_internal->commandList->SetGraphicsRootDescriptorTable(1, m_internal->globalDescriptorHeap->GetGPUDescriptorHandleForHeapStart());
        }
    }

    void D3D12CommandList::BindVertexBuffer(RHI::IBuffer* buffer) {
        auto* d3d12Buffer = static_cast<D3D12Buffer*>(buffer);

		D3D12_VERTEX_BUFFER_VIEW vbView = {};
        vbView.BufferLocation = ((ID3D12Resource*)d3d12Buffer->GetNativeResource())->GetGPUVirtualAddress();
        vbView.SizeInBytes = d3d12Buffer->GetSize();
        vbView.StrideInBytes = d3d12Buffer->GetStride(); // 20 bytes (float3 + float2)

        // GPU에게 버퍼 위치를 알려줌
        m_internal->commandList->IASetVertexBuffers(0, 1, &vbView);
    }

    void D3D12CommandList::BindGraphicsPipeline(RHI::IPipelineState* pipelineState) {
        // 1. RHI 인터페이스를 D3D12용 실제 클래스로 형변환
        auto* d3d12PSO = static_cast<D3D12PipelineState*>(pipelineState);

        // 2. GPU에게 "이제부터 이 공식(PSO)으로 그려!"라고 명령
        m_internal->commandList->SetPipelineState(d3d12PSO->GetNativePSO());

        // 3. GPU에게 "데이터 통로는 이 규칙(Root Signature)을 따라!"라고 명령
        m_internal->commandList->SetGraphicsRootSignature(d3d12PSO->GetNativeRootSignature());

        // 삼각형으로 그리게 설정
        m_internal->commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    }

    // 인덱스 버퍼 바인딩 구현
    void D3D12CommandList::BindIndexBuffer(RHI::IBuffer* buffer, RHI::Format format, uint32_t offset) {
        auto* d3d12Buffer = static_cast<D3D12Buffer*>(buffer);

        D3D12_INDEX_BUFFER_VIEW ibView = {};
        ibView.BufferLocation = ((ID3D12Resource*)d3d12Buffer->GetNativeResource())->GetGPUVirtualAddress() + offset;
        ibView.SizeInBytes = d3d12Buffer->GetSize() - offset;

        // RHI의 Format을 DXGI 형식으로 변환하여 사용
        ibView.Format = static_cast<DXGI_FORMAT>(D3D12Texture::ToDxgiFormat(format));

        m_internal->commandList->IASetIndexBuffer(&ibView);
    }

    // 인덱스 그리기 구현
    void D3D12CommandList::DrawIndexedInstanced(uint32_t indexCount, uint32_t instanceCount, uint32_t startIndex, int32_t baseVertex, uint32_t startInstance) {
        m_internal->commandList->DrawIndexedInstanced(indexCount, instanceCount, startIndex, baseVertex, startInstance);
    }

    void D3D12CommandList::SetPushConstants(uint32_t size, const void* data) {
        m_internal->commandList->SetGraphicsRoot32BitConstants(0, size / 4, data, 0);
    }
    void D3D12CommandList::SetRenderTargets(uint32_t numRenderTargets, RHI::ITexture** renderTargets, RHI::ITexture* depthStencil) {
        if (numRenderTargets > 0 && renderTargets[0] != nullptr) {
            m_internal->commandList->OMSetRenderTargets(1, &m_internal->rtvHandle, FALSE, nullptr);
            
            // RHI 인터페이스에서 Viewport 설정이 빠졌으므로, 렌더타겟 크기에 맞춰 자동으로 설정합니다.
            uint32_t width = renderTargets[0]->GetWidth();
            uint32_t height = renderTargets[0]->GetHeight();
            D3D12_VIEWPORT viewport = {0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f};
            D3D12_RECT scissorRect = { 0, 0, static_cast<LONG>(width), static_cast<LONG>(height) };
            
            m_internal->commandList->RSSetViewports(1, &viewport);
            m_internal->commandList->RSSetScissorRects(1, &scissorRect);
        }
    }
    void D3D12CommandList::ClearDepth(RHI::ITexture* depthStencil, float depth) {}
    void D3D12CommandList::ResourceBarrier(RHI::IBuffer* buffer, RHI::ResourceState before, RHI::ResourceState after) {}
    void D3D12CommandList::ResourceBarrier(RHI::ITexture* texture, RHI::ResourceState before, RHI::ResourceState after) {}
}