#include "D3D12Device.h"
#include "D3D12CommandList.h"
#include "D3D12Buffer.h"
#include "RHI/IPipelineState.h"
#include "D3D12PipelineState.h"
#include "D3D12Texture.h"
#include "d3dx12.h"


#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <d3d12.h>
#include <dxgi1_6.h>
#include <iostream>
#include <vector>
#include <wrl.h>

using Microsoft::WRL::ComPtr;

namespace dy::Backends
{
    // 헤더에서 선언만 했던 구조체의 실제 정의
    struct D3D12InternalState
    {
        ComPtr<ID3D12Device> device;
        ComPtr<ID3D12CommandQueue> commandQueue;
        ComPtr<IDXGISwapChain3> swapChain;
        ComPtr<ID3D12DescriptorHeap> rtvHeap;
        ComPtr<ID3D12Resource> renderTargets[2];

        ComPtr<ID3D12Fence> fence;
        uint32_t fenceValue = 1;
        HANDLE fenceEvent = nullptr;

        uint32_t frameIndex = 0;
        uint32_t rtvDescriptorSize = 0;
    };

    D3D12Device::D3D12Device()
    {
        m_internal = new D3D12InternalState();
    }

    D3D12Device::~D3D12Device()
    {
        delete m_internal;
    }

    int D3D12Device::Initialize(const void* windowHandle)
    {
        HWND hwnd = static_cast<HWND>(const_cast<void*>(windowHandle));
        
        // 1. 디바이스 생성
        ComPtr<IDXGIFactory4> factory;
        CreateDXGIFactory1(IID_PPV_ARGS(&factory));
        D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&m_internal->device));

        // 2. 커맨드 큐 생성
        D3D12_COMMAND_QUEUE_DESC queueDesc = {};
        queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        m_internal->device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&m_internal->commandQueue));

        // 3. 스왑체인(화면 버퍼 2개) 생성
        DXGI_SWAP_CHAIN_DESC1 swapChainDesc = {};
        swapChainDesc.BufferCount = 2;
        swapChainDesc.Width = 800;
        swapChainDesc.Height = 600;
        swapChainDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        swapChainDesc.SampleDesc.Count = 1;

        ComPtr<IDXGISwapChain1> tempSwapChain;
        factory->CreateSwapChainForHwnd(m_internal->commandQueue.Get(), hwnd, &swapChainDesc, nullptr, nullptr, &tempSwapChain);
        tempSwapChain.As(&m_internal->swapChain);
        m_internal->frameIndex = m_internal->swapChain->GetCurrentBackBufferIndex();

        // 4. RTV(렌더 타겟 뷰) 힙 생성
        D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
        rtvHeapDesc.NumDescriptors = 2;
        rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        rtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        m_internal->device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&m_internal->rtvHeap));
        m_internal->rtvDescriptorSize = m_internal->device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

        // 5. 렌더 타겟 뷰(RTV) 연결
        D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = m_internal->rtvHeap->GetCPUDescriptorHandleForHeapStart();
        for (UINT n = 0; n < 2; n++)
        {
            m_internal->swapChain->GetBuffer(n, IID_PPV_ARGS(&m_internal->renderTargets[n]));
            m_internal->device->CreateRenderTargetView(m_internal->renderTargets[n].Get(), nullptr, rtvHandle);
            rtvHandle.ptr += m_internal->rtvDescriptorSize;
        }

        // 6. 동기화용 펜스(Fence) 생성
        m_internal->device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_internal->fence));
        m_internal->fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);

        std::cout << "[D3D12Device] Initialization Complete!" << std::endl;
        return 0;
    }

    void D3D12Device::BeginFrame() { /* TODO */ }
    uint32_t D3D12Device::GetCurrentFrameIndex() const { return m_internal->frameIndex; }

    RHI::ICommandList* D3D12Device::AcquireCommandList() { 
        D3D12_CPU_DESCRIPTOR_HANDLE currentRtv = m_internal->rtvHeap->GetCPUDescriptorHandleForHeapStart();
        currentRtv.ptr += (m_internal->frameIndex * m_internal->rtvDescriptorSize);

        return new D3D12CommandList(m_internal->device.Get(), m_internal->renderTargets[m_internal->frameIndex].Get(), currentRtv.ptr);
    }
    void D3D12Device::Submit(RHI::ICommandList** cmdLists, uint32_t count) { 
		if (count == 0) return;
        // 1. 실행할 DX12 커맨드 리스트들을 모을 배열
        std::vector<ID3D12CommandList*> ppCommandLists;
        ppCommandLists.reserve(count);

        for (uint32_t i = 0; i < count; ++i) {
            D3D12CommandList* d3d12List = static_cast<D3D12CommandList*>(cmdLists[i]);
            ppCommandLists.push_back(reinterpret_cast<ID3D12CommandList*>(d3d12List->GetNativeList()));
        }

        // 2. 한 번에 묶어서 GPU에 제출 (성능 상 훨씬 유리)
        m_internal->commandQueue->ExecuteCommandLists(count, ppCommandLists.data());
    }
    void D3D12Device::Present() { 
        m_internal->swapChain->Present(1, 0);

        const uint64_t fence = m_internal->fenceValue;
        m_internal->commandQueue->Signal(m_internal->fence.Get(), fence);
        m_internal->fenceValue++;

        if (m_internal->fence->GetCompletedValue() < fence) {
            m_internal->fence->SetEventOnCompletion(fence, m_internal->fenceEvent);
            WaitForSingleObject(m_internal->fenceEvent, INFINITE);
        }
        m_internal->frameIndex = m_internal->swapChain->GetCurrentBackBufferIndex();
    }

    RHI::IBuffer* D3D12Device::CreateBuffer(const RHI::BufferDesc& desc) { 
        return new D3D12Buffer(m_internal->device.Get(), desc);
    }

    RHI::IPipelineState* D3D12Device::CreateGraphicsPipeline(const RHI::GraphicsPipelineDesc& desc) {
        // 1. Root Parameter 정의 (b0 레지스터를 쓰겠다고 선언)
        CD3DX12_ROOT_PARAMETER rootParameters[1] = {};
        rootParameters[0].InitAsConstantBufferView(0); // register(b0)

        // 2. Root Signature 생성 (d3dx12 헬퍼 사용)
        CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc;
        rootSigDesc.Init(1, rootParameters, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

        Microsoft::WRL::ComPtr<ID3DBlob> signature;
        Microsoft::WRL::ComPtr<ID3DBlob> error;

        // SerializeRootSignature
        if (FAILED(D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &error))) {
            return nullptr;
        }

        Microsoft::WRL::ComPtr<ID3D12RootSignature> pRootSignature;
        m_internal->device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(), IID_PPV_ARGS(&pRootSignature));

        // 3. Input Layout (Vertex 구조체: float3 Pos, float2 UV)
        D3D12_INPUT_ELEMENT_DESC inputLayout[] = {
            { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
        };

        // 4. PSO 설정 (CD3DX12 헬퍼로 대폭 축소!)
        D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
        psoDesc.InputLayout = { inputLayout, _countof(inputLayout) };
        psoDesc.pRootSignature = pRootSignature.Get();

        // 셰이더 바이트코드 연결
        psoDesc.VS = CD3DX12_SHADER_BYTECODE(desc.vertexShader, desc.vertexShaderSize);
        psoDesc.PS = CD3DX12_SHADER_BYTECODE(desc.pixelShader, desc.pixelShaderSize);

        psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
        psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
        psoDesc.DepthStencilState.DepthEnable = FALSE; // 일단 깊이 테스트는 끔
        psoDesc.DepthStencilState.StencilEnable = FALSE;

        psoDesc.SampleMask = UINT_MAX;
        psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        psoDesc.NumRenderTargets = 1;
        psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        psoDesc.SampleDesc.Count = 1;

        // 5. PSO 생성
        Microsoft::WRL::ComPtr<ID3D12PipelineState> pPSO;
        if (FAILED(m_internal->device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&pPSO)))) {
            return nullptr;
        }

        // 6. 래퍼 객체로 반환
        return new D3D12PipelineState(pPSO.Get(), pRootSignature.Get());
    }

    RHI::ITexture* D3D12Device::CreateTexture(const RHI::TextureDesc& desc) {
        return new D3D12Texture(m_internal->device.Get(), desc);
    }
    
    void D3D12Device::DestroyBuffer(RHI::IBuffer* buffer) { /* TODO */ }

    void D3D12Device::DestroyTexture(RHI::ITexture* texture) {
        delete texture;
    }

    void D3D12Device::DestroyPipelineState(RHI::IPipelineState* pipeline) { /* TODO */ }

    RHI::ITexture* D3D12Device::GetBackBuffer() { return nullptr; /* TODO */ }
}