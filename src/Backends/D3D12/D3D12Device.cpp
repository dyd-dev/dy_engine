#include "D3D12Device.h"
#include "D3D12CommandList.h"
#include "D3D12Buffer.h"
#include "RHI/IPipelineState.h"
#include "D3D12PipelineState.h"
#include "D3D12Texture.h"
#include "d3dx12.h"
#include <d3dcompiler.h>

#pragma comment(lib, "d3dcompiler.lib")

#include <d3d12.h>
#include <dxgi1_6.h>
#include <iostream>
#include <vector>
#include <wrl.h>
#include <thread>

using Microsoft::WRL::ComPtr;

namespace dy::Backends
{
    namespace
    {
        constexpr uint32_t kGlobalDescriptorHeapSize = 1024;
        constexpr uint32_t kTransientDescriptorSlotCount = 1;
    }

    // 헤더에서 선언만 했던 구조체의 실제 정의
    struct D3D12InternalState
    {
        ComPtr<ID3D12Device> device;
        ComPtr<ID3D12InfoQueue> infoQueue; // 디버그 빌드: D3D12 검증 메시지 수집
        ComPtr<ID3D12CommandQueue> commandQueue;
        ComPtr<IDXGISwapChain3> swapChain;
        ComPtr<ID3D12DescriptorHeap> rtvHeap;
        ComPtr<ID3D12Resource> renderTargets[2];

        ComPtr<ID3D12Fence> fence;
        uint32_t fenceValue = 1;
        uint64_t frameFenceValues[2] = { 0, 0 };
        std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> frameUploadBuffers[2];
        std::vector<Microsoft::WRL::ComPtr<ID3D12CommandAllocator>> frameUploadAllocators[2];
        std::vector<Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList>> frameUploadCommandLists[2];
        HANDLE fenceEvent = nullptr;

        uint32_t frameIndex = 0;
        uint32_t rtvDescriptorSize = 0;

        ComPtr<ID3D12DescriptorHeap> globalDescriptorHeap;
        uint32_t descriptorSlotOffset = 0;
        uint32_t srvDescriptorSize = 0;

        // Depth Stencil 관련
        ComPtr<ID3D12DescriptorHeap> dsvHeap;
        ComPtr<ID3D12Resource> depthStencilBuffer;
        uint32_t dsvDescriptorSize = 0;

        D3D12CommandList* commandLists[2] = { nullptr, nullptr };
        D3D12Texture* backBufferTextures[2] = { nullptr, nullptr };
        
        ComPtr<ID3D12CommandAllocator> commandAllocators[2];
        ComPtr<ID3D12RootSignature> deviceRootSignature;
        ComPtr<ID3D12PipelineState> texturedTrianglePipeline;
    };

    // 누적된 D3D12 검증 메시지를 stdout 으로 덤프하고 디바이스 제거 사유를 확인한다.
    // (디버그 레이어가 켜진 디버그 빌드에서만 메시지가 쌓인다.)
    static void DumpInfoQueue(D3D12InternalState* internal, const char* where)
    {
        if (internal == nullptr || internal->infoQueue == nullptr) return;
        const UINT64 count = internal->infoQueue->GetNumStoredMessages();
        for (UINT64 i = 0; i < count; ++i) {
            SIZE_T len = 0;
            internal->infoQueue->GetMessage(i, nullptr, &len);
            std::vector<char> bytes(len);
            D3D12_MESSAGE* msg = reinterpret_cast<D3D12_MESSAGE*>(bytes.data());
            if (SUCCEEDED(internal->infoQueue->GetMessage(i, msg, &len)) && msg->pDescription) {
                std::cout << "[D3D12 " << where << " sev=" << static_cast<int>(msg->Severity)
                          << " id=" << static_cast<int>(msg->ID) << "] " << msg->pDescription << std::endl;
            }
        }
        internal->infoQueue->ClearStoredMessages();
        const HRESULT removedReason = internal->device->GetDeviceRemovedReason();
        if (FAILED(removedReason)) {
            std::cout << "[D3D12] DEVICE REMOVED reason=0x" << std::hex << static_cast<unsigned>(removedReason) << std::dec << std::endl;
        }
    }

    D3D12Device::D3D12Device()
    {
        m_internal = new D3D12InternalState();
    }

    D3D12Device::~D3D12Device()
    {
        for (int i = 0; i < 2; ++i) {
            delete m_internal->commandLists[i];
            delete m_internal->backBufferTextures[i];
        }
        delete m_internal;
    }

    int D3D12Device::Initialize(const void* windowHandle, const RHI::DeviceDesc& desc)
    {
        HWND hwnd = static_cast<HWND>(const_cast<void*>(windowHandle));

#if defined(_DEBUG)
        // 디버그 레이어 활성화(디바이스 생성 전 필수). 이래야 InfoQueue 에 검증 메시지가 쌓인다.
        // ("그래픽 도구" 선택 기능 미설치 시 D3D12GetDebugInterface 가 실패하므로 조건부 처리.)
        {
            ComPtr<ID3D12Debug> debugController;
            if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController)))) {
                debugController->EnableDebugLayer();
            }
        }
#endif

        // 1. 디바이스 생성
        ComPtr<IDXGIFactory4> factory;
        CreateDXGIFactory1(IID_PPV_ARGS(&factory));
        if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&m_internal->device)))) {
            return -1;
        }
        
#if defined(_DEBUG)
        if (SUCCEEDED(m_internal->device.As(&m_internal->infoQueue))) {
            // break 하지 않고 메시지를 모아 DumpInfoQueue 가 stdout 으로 덤프한다.
            m_internal->infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, FALSE);
            m_internal->infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, FALSE);
            m_internal->infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_WARNING, FALSE);
        }
#endif

        // 2. 커맨드 큐 생성
        D3D12_COMMAND_QUEUE_DESC queueDesc = {};
        queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        m_internal->device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&m_internal->commandQueue));

        // 3. 스왑체인(화면 버퍼 2개) 생성
        RECT rect = {};
        GetClientRect(hwnd, &rect);
        uint32_t width = rect.right - rect.left;
        uint32_t height = rect.bottom - rect.top;
        if (width == 0 || height == 0) {
            width = 1280;
            height = 720;
        }

        // DeviceDesc 의 포맷을 그대로 따른다(기본 R8G8B8A8_UNORM).
        // 주의: FLIP 스왑체인은 sRGB 버퍼 포맷을 직접 허용하지 않으므로, sRGB 가 필요하면
        // 버퍼는 UNORM 으로 만들고 RTV 만 sRGB 로 두는 분리가 필요(현재 기본 UNORM 이라 무관).
        const DXGI_FORMAT swapchainDxgiFormat = static_cast<DXGI_FORMAT>(D3D12Texture::ToDxgiFormat(desc.swapchainFormat));

        DXGI_SWAP_CHAIN_DESC1 swapChainDesc = {};
        swapChainDesc.BufferCount = 2;
        swapChainDesc.Width = width;
        swapChainDesc.Height = height;
        swapChainDesc.Format = swapchainDxgiFormat;
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

        // 4-1. 커맨드 할당자(Command Allocator) 생성
        for (int i = 0; i < 2; i++) {
            m_internal->device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_internal->commandAllocators[i]));
        }

        // 5. 렌더 타겟 뷰(RTV) 연결
        D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = m_internal->rtvHeap->GetCPUDescriptorHandleForHeapStart();
        
        RHI::TextureDesc bbDesc = {};
        bbDesc.width = swapChainDesc.Width;
        bbDesc.height = swapChainDesc.Height;
        bbDesc.format = desc.swapchainFormat;
        bbDesc.usage = RHI::TextureUsage::RenderTarget;

        for (UINT n = 0; n < 2; n++)
        {
            m_internal->swapChain->GetBuffer(n, IID_PPV_ARGS(&m_internal->renderTargets[n]));
            m_internal->device->CreateRenderTargetView(m_internal->renderTargets[n].Get(), nullptr, rtvHandle);
            
            m_internal->backBufferTextures[n] = new D3D12Texture(m_internal->renderTargets[n].Get(), bbDesc);
            
            rtvHandle.ptr += m_internal->rtvDescriptorSize;
        }

        // 6. 동기화용 펜스(Fence) 생성
        m_internal->device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_internal->fence));
        m_internal->fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);

        // 7. 글로벌 디스크립터 힙 생성
        D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc = {};
        srvHeapDesc.NumDescriptors = kGlobalDescriptorHeapSize;
        srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        
        if(FAILED(m_internal->device->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(&m_internal->globalDescriptorHeap))))
        {
            std::cout << "Failed to create descriptor heap!" << std::endl;
            return -1;
        }
        m_internal->srvDescriptorSize = m_internal->device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

        // 8. 깊이 버퍼(Depth Buffer) 생성
        D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc = {};
        dsvHeapDesc.NumDescriptors = 1;
        dsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        dsvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        m_internal->device->CreateDescriptorHeap(&dsvHeapDesc, IID_PPV_ARGS(&m_internal->dsvHeap));
        m_internal->dsvDescriptorSize = m_internal->device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);

        D3D12_RESOURCE_DESC depthDesc = {};
        depthDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        depthDesc.Alignment = 0;
        depthDesc.Width = swapChainDesc.Width;
        depthDesc.Height = swapChainDesc.Height;
        depthDesc.DepthOrArraySize = 1;
        depthDesc.MipLevels = 1;
        depthDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
        depthDesc.SampleDesc.Count = 1;
        depthDesc.SampleDesc.Quality = 0;
        depthDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        depthDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

        D3D12_CLEAR_VALUE depthOptimizedClearValue = {};
        depthOptimizedClearValue.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
        depthOptimizedClearValue.DepthStencil.Depth = 1.0f;
        depthOptimizedClearValue.DepthStencil.Stencil = 0;

        CD3DX12_HEAP_PROPERTIES depthHeapProps(D3D12_HEAP_TYPE_DEFAULT);
        m_internal->device->CreateCommittedResource(
            &depthHeapProps,
            D3D12_HEAP_FLAG_NONE,
            &depthDesc,
            D3D12_RESOURCE_STATE_DEPTH_WRITE,
            &depthOptimizedClearValue,
            IID_PPV_ARGS(&m_internal->depthStencilBuffer)
        );

        m_internal->device->CreateDepthStencilView(m_internal->depthStencilBuffer.Get(), nullptr, m_internal->dsvHeap->GetCPUDescriptorHandleForHeapStart());

        // 9. 커맨드 리스트 미리 할당
        D3D12_CPU_DESCRIPTOR_HANDLE currentRtv = m_internal->rtvHeap->GetCPUDescriptorHandleForHeapStart();
        D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle = m_internal->dsvHeap->GetCPUDescriptorHandleForHeapStart();
        for (int i = 0; i < 2; i++) {
            m_internal->commandLists[i] = new D3D12CommandList(m_internal->device.Get(), m_internal->renderTargets[i].Get(), currentRtv.ptr, m_internal->globalDescriptorHeap.Get(), m_internal->srvDescriptorSize);
            m_internal->commandLists[i]->SetDepthStencilView(dsvHandle.ptr); // DSV 등록
            m_internal->commandLists[i]->SetBackBufferTexture(m_internal->backBufferTextures[i]); // 백버퍼 상태 추적기 연결
            currentRtv.ptr += m_internal->rtvDescriptorSize;
        }

        std::cout << "[D3D12Device] Initialization Complete (with Depth Buffer)!" << std::endl;
        return 0;
    }

    void D3D12Device::BeginFrame() { 
        m_internal->commandLists[m_internal->frameIndex]->Reset();
    }
    uint32_t D3D12Device::GetCurrentFrameIndex() const { return m_internal->frameIndex; }

    RHI::ICommandList* D3D12Device::AcquireCommandList() { 
        return m_internal->commandLists[m_internal->frameIndex];
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
        DumpInfoQueue(m_internal, "Submit");
    }
    void D3D12Device::Present() {
        DumpInfoQueue(m_internal, "Present");

        // 1. 현재 프레임이 끝났음을 나타내기 위해 Fence를 Signal
        const uint64_t currentFenceValue = m_internal->fenceValue;
        m_internal->commandQueue->Signal(m_internal->fence.Get(), currentFenceValue);
        m_internal->frameFenceValues[m_internal->frameIndex] = currentFenceValue;
        m_internal->fenceValue++;

        // 2. 화면 표시 (Present)
        m_internal->swapChain->Present(1, 0);

        // 3. 다음 프레임 버퍼 인덱스 갱신
        m_internal->frameIndex = m_internal->swapChain->GetCurrentBackBufferIndex();

        // 4. 다음 프레임이 사용할 백버퍼의 이전 GPU 작업이 완료되었는지 확인하고 대기 (Busy-wait)
        const uint64_t waitFenceValue = m_internal->frameFenceValues[m_internal->frameIndex];
        while (m_internal->fence->GetCompletedValue() < waitFenceValue) {
            std::this_thread::yield();
        }

        // 5. 대기가 완료되었으므로, 해당 프레임에 사용했던 임시 업로드 리소스를 지연 해제
        m_internal->frameUploadBuffers[m_internal->frameIndex].clear();
        m_internal->frameUploadAllocators[m_internal->frameIndex].clear();
        m_internal->frameUploadCommandLists[m_internal->frameIndex].clear();
    }

    RHI::IBuffer* D3D12Device::CreateBuffer(const RHI::BufferDesc& desc) { 
        return new D3D12Buffer(m_internal->device.Get(), desc);
    }

    RHI::IPipelineState* D3D12Device::CreateGraphicsPipeline(const RHI::GraphicsPipelineDesc& desc) {
        const bool hasColorAttachment = desc.renderTargetFormat != RHI::Format::Unknown;
        const bool hasDepthAttachment = desc.depthStencilFormat != RHI::Format::Unknown;
        if ((!hasColorAttachment && !hasDepthAttachment) || (desc.depthEnable && !hasDepthAttachment)) return nullptr;

        // 1. Root Signature 1.1 지원 여부 확인
        D3D12_FEATURE_DATA_ROOT_SIGNATURE featureData = {};
        featureData.HighestVersion = D3D_ROOT_SIGNATURE_VERSION_1_1;
        if (FAILED(m_internal->device->CheckFeatureSupport(D3D12_FEATURE_ROOT_SIGNATURE, &featureData, sizeof(featureData)))) {
            featureData.HighestVersion = D3D_ROOT_SIGNATURE_VERSION_1_0;
        }

        // 2. Root Parameter 정의
        // 머티리얼 텍스처 테이블: 글로벌 디스크립터 힙을 덮는 unbounded SRV 배열(register t0, space0).
        //  - non-bindless: 셰이더가 인덱스 0(=트랜지언트 슬롯)만 읽음.
        //  - bindless    : 셰이더가 per-draw 디스크립터 인덱스로 BindlessTextures[idx] 를 읽음.
        // DESCRIPTORS_VOLATILE 라 접근하지 않는 슬롯은 미초기화여도 무방.
        CD3DX12_DESCRIPTOR_RANGE1 textureSrvRange;
        textureSrvRange.Init(
            D3D12_DESCRIPTOR_RANGE_TYPE_SRV,
            UINT_MAX, // unbounded
            0,
            0,
            D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE,
            0);

        // 그림자 맵 SRV: bindless 텍스처 힙과 겹치지 않도록 register(t0, space4) 에 단독 배치.
        CD3DX12_DESCRIPTOR_RANGE1 shadowSrvRange;
        shadowSrvRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 4, D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE, 0);

        CD3DX12_DESCRIPTOR_RANGE1 metallicRoughnessSrvRange;
        metallicRoughnessSrvRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 1, 1, D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE, 0);
        CD3DX12_DESCRIPTOR_RANGE1 normalSrvRange;
        normalSrvRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 2, 1, D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE, 0);
        CD3DX12_DESCRIPTOR_RANGE1 occlusionSrvRange;
        occlusionSrvRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 3, 1, D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE, 0);
        CD3DX12_DESCRIPTOR_RANGE1 emissiveSrvRange;
        emissiveSrvRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 4, 1, D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE, 0);

        CD3DX12_ROOT_PARAMETER1 rootParameters[10] = {};
        rootParameters[0].InitAsConstants(52, 0); // register(b0): DrawConstants 208 bytes = 52 DWORDs
        rootParameters[1].InitAsDescriptorTable(1, &textureSrvRange, D3D12_SHADER_VISIBILITY_PIXEL); // register(t0, space0)
        rootParameters[2].InitAsConstantBufferView(1); // register(b1): RendererLighting
        rootParameters[3].InitAsConstantBufferView(3); // register(b3): ShadowMatrix
        // bindless storage SRV 는 space0~2 의 무한(unbounded) 범위(param 1)와 겹치면 안 되므로 space3 에 둔다.
        rootParameters[4].InitAsShaderResourceView(11, 3); // register(t11, space3): instance transforms
        rootParameters[5].InitAsDescriptorTable(1, &shadowSrvRange, D3D12_SHADER_VISIBILITY_PIXEL); // register(t0, space4): shadow map
        rootParameters[6].InitAsDescriptorTable(1, &metallicRoughnessSrvRange, D3D12_SHADER_VISIBILITY_PIXEL); // register(t1, space1)
        rootParameters[7].InitAsDescriptorTable(1, &normalSrvRange, D3D12_SHADER_VISIBILITY_PIXEL); // register(t2, space1)
        rootParameters[8].InitAsDescriptorTable(1, &occlusionSrvRange, D3D12_SHADER_VISIBILITY_PIXEL); // register(t3, space1)
        rootParameters[9].InitAsDescriptorTable(1, &emissiveSrvRange, D3D12_SHADER_VISIBILITY_PIXEL); // register(t4, space1)

        CD3DX12_STATIC_SAMPLER_DESC samplerDesc(0, D3D12_FILTER_MIN_MAG_MIP_LINEAR);

        // 3. Versioned Root Signature 생성
        CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC rootSigDesc;
        rootSigDesc.Init_1_1(10, rootParameters, 1, &samplerDesc, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

        Microsoft::WRL::ComPtr<ID3DBlob> signature;
        Microsoft::WRL::ComPtr<ID3DBlob> error;

        // SerializeVersionedRootSignature를 사용하면 기기 지원 버전에 맞게 자동으로 1.1 또는 1.0으로 다운그레이드 직렬화 해줌
        if (FAILED(D3DX12SerializeVersionedRootSignature(&rootSigDesc, featureData.HighestVersion, &signature, &error))) {
            std::cout << "[D3D12] RootSignature serialize FAILED";
            if (error) std::cout << ": " << static_cast<const char*>(error->GetBufferPointer());
            std::cout << std::endl;
            return nullptr;
        }

        Microsoft::WRL::ComPtr<ID3D12RootSignature> pRootSignature;
        if (FAILED(m_internal->device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(), IID_PPV_ARGS(&pRootSignature)))) {
            std::cout << "[D3D12] CreateRootSignature FAILED" << std::endl;
            DumpInfoQueue(m_internal, "CreateRootSignature");
            return nullptr;
        }

        // 4. PSO 설정 (CD3DX12 헬퍼로 대폭 축소!)
        D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
        static const D3D12_INPUT_ELEMENT_DESC kInputLayout[] = {
            { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT,    0, 0,  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT,    0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,       0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "TANGENT",  0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 32, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        };
        psoDesc.InputLayout = { kInputLayout, static_cast<UINT>(sizeof(kInputLayout) / sizeof(kInputLayout[0])) };
        psoDesc.pRootSignature = pRootSignature.Get();

        // 셰이더 컴파일
        ComPtr<ID3DBlob> vsBlob;
        ComPtr<ID3DBlob> psBlob;
        ComPtr<ID3DBlob> errorBlob;

        UINT compileFlags = D3DCOMPILE_ENABLE_UNBOUNDED_DESCRIPTOR_TABLES;
#if defined(_DEBUG) || defined(DEBUG)
        compileFlags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif

        const bool hasPixelShader = desc.pixelShader != nullptr && desc.pixelShaderSize > 0u;

        HRESULT hr = D3DCompile(desc.vertexShader, desc.vertexShaderSize, nullptr, nullptr, nullptr, "main", "vs_5_1", compileFlags, 0, &vsBlob, &errorBlob);
        if (FAILED(hr)) {
            if (errorBlob) std::cout << "VS Compile Error: " << (char*)errorBlob->GetBufferPointer() << std::endl;
            return nullptr;
        }

        if (hasPixelShader) {
            hr = D3DCompile(desc.pixelShader, desc.pixelShaderSize, nullptr, nullptr, nullptr, "main", "ps_5_1", compileFlags, 0, &psBlob, &errorBlob);
            if (FAILED(hr)) {
                if (errorBlob) std::cout << "PS Compile Error: " << (char*)errorBlob->GetBufferPointer() << std::endl;
                return nullptr;
            }
        }

        psoDesc.VS = CD3DX12_SHADER_BYTECODE(vsBlob.Get());
        if (hasPixelShader) {
            psoDesc.PS = CD3DX12_SHADER_BYTECODE(psBlob.Get());
        }

        psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
        psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
        psoDesc.RasterizerState.FrontCounterClockwise = TRUE; // 엔진 메시/Vulkan과 동일하게 CCW를 앞면으로
        if (!hasColorAttachment) psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        psoDesc.RasterizerState.DepthBias = desc.depthBias;
        psoDesc.RasterizerState.SlopeScaledDepthBias = desc.depthBiasSlope;
        psoDesc.RasterizerState.DepthBiasClamp = desc.depthBiasClamp;
        psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
        psoDesc.BlendState.RenderTarget[0].BlendEnable = TRUE;
        psoDesc.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
        psoDesc.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
        psoDesc.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
        psoDesc.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
        psoDesc.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ZERO;
        psoDesc.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
        psoDesc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        psoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
        psoDesc.DepthStencilState.DepthEnable = desc.depthEnable ? TRUE : FALSE;
        psoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
        psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        psoDesc.DSVFormat = hasDepthAttachment
            ? static_cast<DXGI_FORMAT>(D3D12Texture::ToDxgiDepthStencilFormat(desc.depthStencilFormat))
            : DXGI_FORMAT_UNKNOWN;

        psoDesc.SampleMask = UINT_MAX;
        psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        psoDesc.NumRenderTargets = hasColorAttachment ? 1u : 0u;
        if (hasColorAttachment) {
            psoDesc.RTVFormats[0] = static_cast<DXGI_FORMAT>(D3D12Texture::ToDxgiFormat(desc.renderTargetFormat));
        }
        psoDesc.SampleDesc.Count = 1;

        // 5. PSO 생성
        Microsoft::WRL::ComPtr<ID3D12PipelineState> pPSO;
        if (FAILED(m_internal->device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&pPSO)))) {
            std::cout << "[D3D12] CreateGraphicsPipelineState FAILED (hasPixelShader=" << hasPixelShader << ")" << std::endl;
            DumpInfoQueue(m_internal, "CreateGraphicsPipelineState");
            return nullptr;
        }

        // 6. 래퍼 객체로 반환
        DumpInfoQueue(m_internal, "CreateGraphicsPipeline");
        return new D3D12PipelineState(pPSO.Get(), pRootSignature.Get());
    }

    RHI::DescriptorIndex D3D12Device::AllocateDescriptorSlot() {
        if(m_internal->descriptorSlotOffset >= kGlobalDescriptorHeapSize - kTransientDescriptorSlotCount) return RHI::INVALID_DESCRIPTOR_INDEX;
        return m_internal->descriptorSlotOffset++;
    }

    void D3D12Device::UpdateDescriptorSlot(RHI::DescriptorIndex index, RHI::IBuffer* buffer) {
        if(index == RHI::INVALID_DESCRIPTOR_INDEX || buffer == nullptr) return;
        D3D12Buffer* dxBuffer = static_cast<D3D12Buffer*>(buffer);

        CD3DX12_CPU_DESCRIPTOR_HANDLE srvHandle(m_internal->globalDescriptorHeap->GetCPUDescriptorHandleForHeapStart());
        srvHandle.Offset(index, m_internal->srvDescriptorSize);

        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Format = DXGI_FORMAT_UNKNOWN;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Buffer.FirstElement = 0;
        srvDesc.Buffer.NumElements = dxBuffer->GetSize() / dxBuffer->GetStride();
        srvDesc.Buffer.StructureByteStride = dxBuffer->GetStride();
        srvDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;

        m_internal->device->CreateShaderResourceView(
            static_cast<ID3D12Resource*>(dxBuffer->GetNativeResource()),
            &srvDesc,
            srvHandle
        );
    }

    void D3D12Device::UpdateDescriptorSlot(RHI::DescriptorIndex index, RHI::ITexture* texture) {
        if(index == RHI::INVALID_DESCRIPTOR_INDEX || texture == nullptr) return;
        // ITexture를 D3D12Texture로 다운캐스팅
        D3D12Texture* dxTexture = static_cast<D3D12Texture*>(texture);

        // 1. 글로벌 힙의 시작 주소 가져오기
        CD3DX12_CPU_DESCRIPTOR_HANDLE srvHandle(m_internal->globalDescriptorHeap->GetCPUDescriptorHandleForHeapStart());
        
        // 2. 인덱스(슬롯 번호)만큼 주소 이동
        srvHandle.Offset(index, m_internal->srvDescriptorSize);

        // 3. SRV(Shader Resource View) 생성
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        
        srvDesc.Format = static_cast<DXGI_FORMAT>(D3D12Texture::ToDxgiShaderResourceFormat(dxTexture->GetFormat()));
        
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Texture2D.MipLevels = 1;

        // 실제 리소스(ID3D12Resource)를 가져와서 뷰 생성
        m_internal->device->CreateShaderResourceView(
            static_cast<ID3D12Resource*>(dxTexture->GetNativeResource()),
            &srvDesc,
            srvHandle
        );

        // 커맨드 리스트가 SRV 디스크립터 테이블을 바인딩할 때 GPU 핸들을 계산할 수 있도록 슬롯 기억.
        dxTexture->SetGlobalSrvIndex(index);
    }

    RHI::ITexture* D3D12Device::CreateTexture(const RHI::TextureDesc& desc) {
        return new D3D12Texture(m_internal->device.Get(), desc);
    }

    bool D3D12Device::UpdateTexture(RHI::ITexture* texture, const void* data, uint32_t rowPitch) {
        auto d3dTexture = static_cast<D3D12Texture*>(texture);
        ID3D12Resource* destResource = static_cast<ID3D12Resource*>(d3dTexture->GetNativeResource());

        D3D12_RESOURCE_DESC desc = destResource->GetDesc();
        UINT64 requiredSize = 0;
        m_internal->device->GetCopyableFootprints(&desc, 0, 1, 0, nullptr, nullptr, nullptr, &requiredSize);

        std::cout << "[D3D12Device] UpdateTexture started. requiredSize=" << requiredSize << std::endl;

        // Upload Heap 버퍼 생성
        ComPtr<ID3D12Resource> uploadBuffer;
        CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_UPLOAD);
        CD3DX12_RESOURCE_DESC bufferDesc = CD3DX12_RESOURCE_DESC::Buffer(requiredSize);
        HRESULT hr = m_internal->device->CreateCommittedResource(
            &heapProps, D3D12_HEAP_FLAG_NONE, &bufferDesc, 
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&uploadBuffer));
        
        if (FAILED(hr)) {
            std::cout << "[D3D12Device] Failed to create uploadBuffer! HR=" << std::hex << hr << std::endl;
        }

        // 임시 커맨드 리스트 생성
        ComPtr<ID3D12CommandAllocator> alloc;
        m_internal->device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc));
        ComPtr<ID3D12GraphicsCommandList> cmdList;
        m_internal->device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr, IID_PPV_ARGS(&cmdList));

        // d3dx12.h 헬퍼를 사용해 완벽한 복사 수행
        D3D12_SUBRESOURCE_DATA subresourceData = {};
        subresourceData.pData = data;
        subresourceData.RowPitch = rowPitch;
        subresourceData.SlicePitch = rowPitch * desc.Height;

        UINT64 bytesCopied = UpdateSubresources(cmdList.Get(), destResource, uploadBuffer.Get(), 0, 0, 1, &subresourceData);
        if (bytesCopied == 0) {
            std::cout << "[D3D12Device] UpdateSubresources FAILED (0 bytes copied)!" << std::endl;
        }
        if (bytesCopied == 0) {
            std::cout << "[D3D12Device] UpdateSubresources FAILED (0 bytes copied)!" << std::endl;
        }

        // 텍스처 상태 변경: COPY_DEST -> PIXEL_SHADER_RESOURCE
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = destResource;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cmdList->ResourceBarrier(1, &barrier);

        cmdList->Close();

        // 커맨드 실행
        ID3D12CommandList* lists[] = { cmdList.Get() };
        m_internal->commandQueue->ExecuteCommandLists(1, lists);

        // 임시 업로드 버퍼와 커맨드 리스트/할당자가 즉시 파괴되는 것을 막기 위해 지연 소멸 큐에 등록합니다.
        // 다음 프레임 버퍼가 다 돌아와 대기가 완료될 때 안전하게 해제됩니다.
        m_internal->frameUploadBuffers[m_internal->frameIndex].push_back(uploadBuffer);
        m_internal->frameUploadAllocators[m_internal->frameIndex].push_back(alloc);
        m_internal->frameUploadCommandLists[m_internal->frameIndex].push_back(cmdList);
        
        d3dTexture->SetResourceState(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        return true;
    }

    void D3D12Device::DestroyBuffer(RHI::IBuffer* buffer) { delete buffer; }

    void D3D12Device::DestroyTexture(RHI::ITexture* texture) {
        delete texture;
    }

    void D3D12Device::DestroyPipelineState(RHI::IPipelineState* pipeline) { delete pipeline; }

    RHI::ITexture* D3D12Device::GetBackBuffer() { 
        return m_internal->backBufferTextures[m_internal->frameIndex]; 
    }
}
