#pragma once
#include "RHI/IDevice.h"
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl.h>

namespace dy::Backends {
    class D3D12Device : public dy::RHI::IDevice {
    public:
        D3D12Device() = default;
        virtual ~D3D12Device();

        void BeginFrame() override;
        void EndFrame() override;
        void Present() override;
        void SubmitCommandList(RHI::ICommandList* cmd) override;
		virtual dy::RHI::ICommandList* GetCommandList() override;

    protected :
        int Initialize(const void* windowHandle) override;

    private:
        void WaitForPreviousFrame();

        Microsoft::WRL::ComPtr<ID3D12Device> m_device;
        Microsoft::WRL::ComPtr<ID3D12CommandQueue> m_commandQueue;
        Microsoft::WRL::ComPtr<IDXGISwapChain4> m_swapChain;
        Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_rtvHeap;
        Microsoft::WRL::ComPtr<ID3D12Resource> m_renderTargets[2];
        Microsoft::WRL::ComPtr<ID3D12CommandAllocator> m_commandAllocator;
        Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> m_commandList;
        Microsoft::WRL::ComPtr<ID3D12RootSignature> m_rootSignature;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> m_pso;
        Microsoft::WRL::ComPtr<ID3D12Resource> m_vertexBuffer;
        D3D12_VERTEX_BUFFER_VIEW m_vertexBufferView;

        UINT m_rtvDescriptorSize = 0;
        UINT m_frameIndex = 0;
        HANDLE m_fenceEvent = nullptr;
        Microsoft::WRL::ComPtr<ID3D12Fence> m_fence;
        UINT32 m_fenceValue = 0;

        Microsoft::WRL::ComPtr<ID3D12Resource> m_texture;           // 진짜 GPU 텍스처
        Microsoft::WRL::ComPtr<ID3D12Resource> m_textureUploadHeap; // CPU -> GPU 전송용 임시 정거장
        Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_srvHeap;     // 텍스처 뷰(SRV)를 담을 상자
    };
}