#include "D3D12Device.h"
#include "D3D12CommandList.h"
#include "D3D12Buffer.h"
#include "RHI/GraphicsPipeline.h"
#include "RHI/IResourceSet.h"
#include "RHI/ISampler.h"
#include "D3D12PipelineState.h"
#include "D3D12Texture.h"
#include "d3dx12.h"

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
		class D3D12Shader final : public RHI::IShader
		{
		public:
			explicit D3D12Shader(const RHI::ShaderDesc& desc)
				: m_stage(desc.stage)
				, m_binary(
					static_cast<const uint8_t*>(desc.binary),
					static_cast<const uint8_t*>(desc.binary) + desc.binarySize)
			{
			}

			[[nodiscard]] RHI::ShaderStage GetStage() const { return m_stage; }
			[[nodiscard]] const void* GetBinary() const { return m_binary.data(); }
			[[nodiscard]] size_t GetBinarySize() const { return m_binary.size(); }

		private:
			RHI::ShaderStage m_stage = RHI::ShaderStage::Unknown;
			std::vector<uint8_t> m_binary;
		};
    }

    // 헤더에서 선언만 했던 구조체의 실제 정의
    struct D3D12InternalState
    {
        ComPtr<ID3D12Device> device;
        ComPtr<ID3D12InfoQueue> infoQueue; // 디버그 빌드: D3D12 검증 메시지 수집
        ComPtr<ID3D12CommandQueue> commandQueue;
        ComPtr<IDXGISwapChain3> swapChain;
        ComPtr<ID3D12DescriptorHeap> rtvHeap;
        std::vector<ComPtr<ID3D12Resource>> renderTargets;

        ComPtr<ID3D12Fence> fence;
        uint32_t fenceValue = 1;
        std::vector<uint64_t> frameFenceValues;
        std::vector<std::vector<ComPtr<ID3D12Resource>>> frameUploadBuffers;
        std::vector<std::vector<ComPtr<ID3D12CommandAllocator>>> frameUploadAllocators;
        std::vector<std::vector<ComPtr<ID3D12GraphicsCommandList>>> frameUploadCommandLists;
        HANDLE fenceEvent = nullptr;

        uint32_t maxFramesInFlight = 0;
        uint32_t frameIndex = 0;
        uint32_t backBufferIndex = 0;
        uint32_t rtvDescriptorSize = 0;

        std::vector<D3D12CommandList*> commandLists;
        std::vector<D3D12Texture*> backBufferTextures;

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
        for (D3D12CommandList* commandList : m_internal->commandLists) delete commandList;
        for (D3D12Texture* backBufferTexture : m_internal->backBufferTextures) delete backBufferTexture;
        delete m_internal;
    }

    int D3D12Device::Initialize(const void* windowHandle, const RHI::DeviceDesc& desc)
    {
        RHI::Format actualSwapchainFormat = desc.swapchainFormat;
        DXGI_FORMAT swapchainResourceFormat = DXGI_FORMAT_UNKNOWN;
        DXGI_FORMAT swapchainRtvFormat = DXGI_FORMAT_UNKNOWN;
        switch(desc.swapchainFormat)
        {
        case RHI::Format::Unknown:
            actualSwapchainFormat = RHI::Format::R8G8B8A8_UNORM;
            swapchainResourceFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
            swapchainRtvFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
            break;
        case RHI::Format::R8G8B8A8_UNORM:
            swapchainResourceFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
            swapchainRtvFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
            break;
        case RHI::Format::B8G8R8A8_UNORM:
            swapchainResourceFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
            swapchainRtvFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
            break;
        case RHI::Format::R8G8B8A8_UNORM_SRGB:
            swapchainResourceFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
            swapchainRtvFormat = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
            break;
        case RHI::Format::B8G8R8A8_UNORM_SRGB:
            swapchainResourceFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
            swapchainRtvFormat = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
            break;
        case RHI::Format::R16G16B16A16_FLOAT:
            swapchainResourceFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
            swapchainRtvFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
            break;
        case RHI::Format::R32G32B32A32_FLOAT:
        case RHI::Format::D32_FLOAT:
        case RHI::Format::D24_UNORM_S8_UINT:
        case RHI::Format::R32_UINT:
        case RHI::Format::R16_UINT:
        default:
            return -1;
        }

        HWND hwnd = static_cast<HWND>(const_cast<void*>(windowHandle));
        m_internal->maxFramesInFlight = desc.maxFramesInFlight;
        m_internal->frameFenceValues.assign(desc.maxFramesInFlight, 0);
        m_internal->frameUploadBuffers.resize(desc.maxFramesInFlight);
        m_internal->frameUploadAllocators.resize(desc.maxFramesInFlight);
        m_internal->frameUploadCommandLists.resize(desc.maxFramesInFlight);
        m_internal->commandLists.resize(desc.maxFramesInFlight, nullptr);
        m_internal->renderTargets.resize(2);
        m_internal->backBufferTextures.resize(m_internal->renderTargets.size(), nullptr);

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
        if(FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return -1;
        if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&m_internal->device)))) {
            return -1;
        }

        D3D12_FEATURE_DATA_FORMAT_SUPPORT rtvFormatSupport = {};
        rtvFormatSupport.Format = swapchainRtvFormat;
        if(FAILED(m_internal->device->CheckFeatureSupport(
            D3D12_FEATURE_FORMAT_SUPPORT, &rtvFormatSupport, sizeof(rtvFormatSupport))) ||
            (rtvFormatSupport.Support1 & D3D12_FORMAT_SUPPORT1_RENDER_TARGET) == 0)
        {
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
        if(FAILED(m_internal->device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&m_internal->commandQueue)))) return -1;

        // 3. 스왑체인 image 수는 frame context 수와 독립적으로 유지한다.
        RECT rect = {};
        GetClientRect(hwnd, &rect);
        uint32_t width = rect.right - rect.left;
        uint32_t height = rect.bottom - rect.top;
        if (width == 0 || height == 0) {
            width = 1280;
            height = 720;
        }

        DXGI_SWAP_CHAIN_DESC1 swapChainDesc = {};
        swapChainDesc.BufferCount = static_cast<UINT>(m_internal->renderTargets.size());
        swapChainDesc.Width = width;
        swapChainDesc.Height = height;
        swapChainDesc.Format = swapchainResourceFormat;
        swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        swapChainDesc.SampleDesc.Count = 1;

        ComPtr<IDXGISwapChain1> tempSwapChain;
        if(FAILED(factory->CreateSwapChainForHwnd(
            m_internal->commandQueue.Get(), hwnd, &swapChainDesc, nullptr, nullptr, &tempSwapChain))) return -1;
        if(FAILED(tempSwapChain.As(&m_internal->swapChain))) return -1;
        m_internal->backBufferIndex = m_internal->swapChain->GetCurrentBackBufferIndex();

        // 4. RTV(렌더 타겟 뷰) 힙 생성
        D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
        rtvHeapDesc.NumDescriptors = swapChainDesc.BufferCount;
        rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        rtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        if(FAILED(m_internal->device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&m_internal->rtvHeap)))) return -1;
        m_internal->rtvDescriptorSize = m_internal->device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

        // 5. 렌더 타겟 뷰(RTV) 연결
        D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = m_internal->rtvHeap->GetCPUDescriptorHandleForHeapStart();
        
        RHI::TextureDesc bbDesc = {};
        bbDesc.width = swapChainDesc.Width;
        bbDesc.height = swapChainDesc.Height;
        bbDesc.format = actualSwapchainFormat;
        bbDesc.usage = RHI::TextureUsage::RenderTarget;

        D3D12_RENDER_TARGET_VIEW_DESC swapchainRtvDesc = {};
        swapchainRtvDesc.Format = swapchainRtvFormat;
        swapchainRtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;

        for (UINT n = 0; n < swapChainDesc.BufferCount; n++)
        {
            if(FAILED(m_internal->swapChain->GetBuffer(n, IID_PPV_ARGS(&m_internal->renderTargets[n])))) return -1;
            m_internal->device->CreateRenderTargetView(m_internal->renderTargets[n].Get(), &swapchainRtvDesc, rtvHandle);
            
            m_internal->backBufferTextures[n] = new D3D12Texture(m_internal->renderTargets[n].Get(), bbDesc);
            
            rtvHandle.ptr += m_internal->rtvDescriptorSize;
        }

        // 6. 동기화용 펜스(Fence) 생성
        m_internal->device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_internal->fence));
        m_internal->fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);

        // 7. 커맨드 리스트 미리 할당
        for (uint32_t i = 0; i < m_internal->maxFramesInFlight; i++) {
            m_internal->commandLists[i] = new D3D12CommandList(m_internal->device.Get(), 0);
        }

        std::cout << "[D3D12Device] Initialization Complete!" << std::endl;
        return 0;
    }

    bool D3D12Device::BeginFrame() {
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_internal->rtvHeap->GetCPUDescriptorHandleForHeapStart();
        rtv.ptr += static_cast<SIZE_T>(m_internal->backBufferIndex) * m_internal->rtvDescriptorSize;

        D3D12CommandList* commandList = m_internal->commandLists[m_internal->frameIndex];
        commandList->SetBackBuffer(m_internal->backBufferTextures[m_internal->backBufferIndex], rtv.ptr);
        return commandList->Reset();
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

        // 3. 다음 swapchain image와 frame context를 서로 독립적으로 갱신한다.
        m_internal->backBufferIndex = m_internal->swapChain->GetCurrentBackBufferIndex();
        m_internal->frameIndex = (m_internal->frameIndex + 1) % m_internal->maxFramesInFlight;

        // 4. 다음 frame context를 재사용하기 전에 그 context의 이전 제출 완료를 기다린다.
        const uint64_t waitFenceValue = m_internal->frameFenceValues[m_internal->frameIndex];
        while (m_internal->fence->GetCompletedValue() < waitFenceValue) {
            std::this_thread::yield();
        }

        // 5. 재사용할 context에 속한 임시 업로드 리소스를 지연 해제한다.
        m_internal->frameUploadBuffers[m_internal->frameIndex].clear();
        m_internal->frameUploadAllocators[m_internal->frameIndex].clear();
        m_internal->frameUploadCommandLists[m_internal->frameIndex].clear();
    }

    RHI::IBuffer* D3D12Device::CreateBuffer(const RHI::BufferDesc& desc) { 
        return new D3D12Buffer(m_internal->device.Get(), desc);
    }

    RHI::IShader* D3D12Device::CreateShader(const RHI::ShaderDesc& desc) {
        if (desc.stage == RHI::ShaderStage::Unknown ||
            desc.binary == nullptr || desc.binarySize == 0 ||
            desc.entryPoint == nullptr || desc.entryPoint[0] == '\0') return nullptr;
        return new D3D12Shader(desc);
    }

    RHI::ISampler* D3D12Device::CreateSampler(const RHI::SamplerDesc& desc) {
        if (desc.minLod > desc.maxLod ||
            static_cast<uint32_t>(desc.minFilter) > static_cast<uint32_t>(RHI::SamplerFilter::Linear) ||
            static_cast<uint32_t>(desc.magFilter) > static_cast<uint32_t>(RHI::SamplerFilter::Linear) ||
            static_cast<uint32_t>(desc.mipFilter) > static_cast<uint32_t>(RHI::SamplerFilter::Linear) ||
            static_cast<uint32_t>(desc.addressU) > static_cast<uint32_t>(RHI::SamplerAddressMode::ClampToEdge) ||
            static_cast<uint32_t>(desc.addressV) > static_cast<uint32_t>(RHI::SamplerAddressMode::ClampToEdge) ||
            static_cast<uint32_t>(desc.addressW) > static_cast<uint32_t>(RHI::SamplerAddressMode::ClampToEdge)) return nullptr;
        return new D3D12Sampler(desc);
    }

    RHI::IPipelineState* D3D12Device::CreateGraphicsPipeline(const RHI::GraphicsPipelineDesc& desc) {
        const bool hasColorAttachment = desc.colorAttachmentCount > 0;
        const bool hasDepthAttachment = desc.depthStencilFormat != RHI::Format::Unknown;
        const bool hasFragmentShader = desc.fragmentShader != nullptr;
        const auto* vertexShader = dynamic_cast<const D3D12Shader*>(desc.vertexShader);
        const auto* fragmentShader = dynamic_cast<const D3D12Shader*>(desc.fragmentShader);
        if ((!hasColorAttachment && !hasDepthAttachment) ||
            ((desc.depthStencil.depthTestEnable || desc.depthStencil.depthWriteEnable || desc.depthStencil.stencilTestEnable) && !hasDepthAttachment) ||
            (desc.depthStencil.depthWriteEnable && !desc.depthStencil.depthTestEnable) ||
            (desc.depthStencil.stencilTestEnable && desc.depthStencilFormat != RHI::Format::D24_UNORM_S8_UINT)) return nullptr;
        if((desc.colorAttachmentCount > 0 && desc.colorAttachments == nullptr) ||
            desc.colorAttachmentCount > D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT) return nullptr;
        if (vertexShader == nullptr || vertexShader->GetStage() != RHI::ShaderStage::Vertex) return nullptr;
        if (hasFragmentShader &&
            (fragmentShader == nullptr || fragmentShader->GetStage() != RHI::ShaderStage::Fragment)) return nullptr;
        if ((desc.inputAssembly.vertexBindingCount > 0 && desc.inputAssembly.vertexBindings == nullptr) ||
            (desc.inputAssembly.vertexAttributeCount > 0 && desc.inputAssembly.vertexAttributes == nullptr)) return nullptr;
        for (uint32_t bindingIndex = 0; bindingIndex < desc.inputAssembly.vertexBindingCount; ++bindingIndex)
        {
            const RHI::VertexBindingDesc& binding = desc.inputAssembly.vertexBindings[bindingIndex];
            if (binding.stride == 0 || binding.slot >= D3D12_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT) return nullptr;
            for (uint32_t previous = 0; previous < bindingIndex; ++previous)
            {
                if (desc.inputAssembly.vertexBindings[previous].slot == binding.slot) return nullptr;
            }
        }

        // 1. Root Signature 1.1 지원 여부 확인
        D3D12_FEATURE_DATA_ROOT_SIGNATURE featureData = {};
        featureData.HighestVersion = D3D_ROOT_SIGNATURE_VERSION_1_1;
        if (FAILED(m_internal->device->CheckFeatureSupport(D3D12_FEATURE_ROOT_SIGNATURE, &featureData, sizeof(featureData)))) {
            featureData.HighestVersion = D3D_ROOT_SIGNATURE_VERSION_1_0;
        }

        if ((desc.pipelineLayout.resourceBindingCount > 0 && desc.pipelineLayout.resourceBindings == nullptr) ||
            (desc.pipelineLayout.inlineConstantRangeCount > 0 && desc.pipelineLayout.inlineConstantRanges == nullptr)) return nullptr;

        uint32_t textureBindingCount = 0;
        uint32_t setCount = 0;
        for(uint32_t index = 0; index < desc.pipelineLayout.resourceBindingCount; ++index)
        {
            const RHI::ResourceBindingDesc& binding = desc.pipelineLayout.resourceBindings[index];
            if(binding.type == RHI::ResourceType::TextureSampler) ++textureBindingCount;
            setCount = std::max(setCount, binding.set + 1u);
        }
        std::vector<CD3DX12_DESCRIPTOR_RANGE1> descriptorRanges(desc.pipelineLayout.resourceBindingCount + textureBindingCount);
        std::vector<CD3DX12_ROOT_PARAMETER1> rootParameters(desc.pipelineLayout.inlineConstantRangeCount + setCount * 2u);
        std::vector<D3D12ResourceBinding> resourceBindings;
        std::vector<D3D12InlineConstantRange> inlineConstantRanges;
        resourceBindings.reserve(desc.pipelineLayout.resourceBindingCount);
        inlineConstantRanges.reserve(desc.pipelineLayout.inlineConstantRangeCount);

        uint32_t rootParameterIndex = 0;
        for(uint32_t index = 0; index < desc.pipelineLayout.inlineConstantRangeCount; ++index)
        {
            const RHI::InlineConstantRangeDesc& range = desc.pipelineLayout.inlineConstantRanges[index];
            if(range.size == 0 || (range.size & 3u) != 0 || (range.offset & 3u) != 0 ||
                range.stages == RHI::ShaderStageFlags::None) return nullptr;
            D3D12_SHADER_VISIBILITY visibility = D3D12_SHADER_VISIBILITY_ALL;
            if(range.stages == RHI::ShaderStageFlags::Vertex) visibility = D3D12_SHADER_VISIBILITY_VERTEX;
            else if(range.stages == RHI::ShaderStageFlags::Fragment) visibility = D3D12_SHADER_VISIBILITY_PIXEL;
            rootParameters[rootParameterIndex].InitAsConstants(range.size / 4, range.binding, 0, visibility);
            inlineConstantRanges.push_back({ range, rootParameterIndex++ });
        }

        uint32_t resourceHeapOffset = 0;
        uint32_t samplerHeapOffset = 0;
        for(uint32_t index = 0; index < desc.pipelineLayout.resourceBindingCount; ++index)
        {
            const RHI::ResourceBindingDesc& binding = desc.pipelineLayout.resourceBindings[index];
            if(binding.arrayCount == 0 || binding.stages == RHI::ShaderStageFlags::None ||
                static_cast<uint32_t>(binding.type) > static_cast<uint32_t>(RHI::ResourceType::TextureSampler)) return nullptr;
            for(uint32_t previous = 0; previous < index; ++previous)
            {
                const RHI::ResourceBindingDesc& other = desc.pipelineLayout.resourceBindings[previous];
                if(other.set == binding.set && other.binding == binding.binding) return nullptr;
            }
            D3D12ResourceBinding nativeBinding = {};
            nativeBinding.desc = binding;
            nativeBinding.heapOffset = resourceHeapOffset;
            nativeBinding.samplerHeapOffset = samplerHeapOffset;
            resourceHeapOffset += binding.arrayCount;
            if(binding.type == RHI::ResourceType::TextureSampler) samplerHeapOffset += binding.arrayCount;
            resourceBindings.push_back(nativeBinding);
        }

        uint32_t rangeIndex = 0;
        for(uint32_t set = 0; set < setCount; ++set)
        {
            const uint32_t firstRange = rangeIndex;
            for(D3D12ResourceBinding& binding : resourceBindings)
            {
                if(binding.desc.set != set) continue;
                const D3D12_DESCRIPTOR_RANGE_TYPE rangeType = binding.desc.type == RHI::ResourceType::ConstantBuffer
                    ? D3D12_DESCRIPTOR_RANGE_TYPE_CBV
                    : D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
                descriptorRanges[rangeIndex++].Init(
                    rangeType,
                    binding.desc.arrayCount,
                    binding.desc.binding,
                    binding.desc.set,
                    D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE,
                    binding.heapOffset);
            }
            if(rangeIndex == firstRange) continue;
            rootParameters[rootParameterIndex].InitAsDescriptorTable(
                rangeIndex - firstRange,
                &descriptorRanges[firstRange],
                D3D12_SHADER_VISIBILITY_ALL);
            for(D3D12ResourceBinding& binding : resourceBindings)
                if(binding.desc.set == set) binding.resourceRootParameter = rootParameterIndex;
            ++rootParameterIndex;
        }
        for(uint32_t set = 0; set < setCount; ++set)
        {
            const uint32_t firstRange = rangeIndex;
            for(D3D12ResourceBinding& binding : resourceBindings)
            {
                if(binding.desc.set != set || binding.desc.type != RHI::ResourceType::TextureSampler) continue;
                descriptorRanges[rangeIndex++].Init(
                    D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER,
                    binding.desc.arrayCount,
                    binding.desc.binding,
                    binding.desc.set,
                    D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE,
                    binding.samplerHeapOffset);
            }
            if(rangeIndex == firstRange) continue;
            rootParameters[rootParameterIndex].InitAsDescriptorTable(
                rangeIndex - firstRange,
                &descriptorRanges[firstRange],
                D3D12_SHADER_VISIBILITY_ALL);
            for(D3D12ResourceBinding& binding : resourceBindings)
                if(binding.desc.set == set && binding.desc.type == RHI::ResourceType::TextureSampler)
                    binding.samplerRootParameter = rootParameterIndex;
            ++rootParameterIndex;
        }
        rootParameters.resize(rootParameterIndex);

        // 3. Versioned Root Signature 생성
        CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC rootSigDesc;
        rootSigDesc.Init_1_1(
            static_cast<uint32_t>(rootParameters.size()),
            rootParameters.data(),
            0,
            nullptr,
            D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

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
        std::vector<D3D12_INPUT_ELEMENT_DESC> inputElements;
        inputElements.reserve(desc.inputAssembly.vertexAttributeCount);
        for (uint32_t attributeIndex = 0; attributeIndex < desc.inputAssembly.vertexAttributeCount; ++attributeIndex)
        {
            const RHI::VertexAttributeDesc& attribute = desc.inputAssembly.vertexAttributes[attributeIndex];
            DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
            switch (attribute.format)
            {
            case RHI::Format::R32_FLOAT: format = DXGI_FORMAT_R32_FLOAT; break;
            case RHI::Format::R32G32_FLOAT: format = DXGI_FORMAT_R32G32_FLOAT; break;
            case RHI::Format::R32G32B32_FLOAT: format = DXGI_FORMAT_R32G32B32_FLOAT; break;
            case RHI::Format::R32G32B32A32_FLOAT: format = DXGI_FORMAT_R32G32B32A32_FLOAT; break;
            case RHI::Format::R8G8B8A8_UNORM: format = DXGI_FORMAT_R8G8B8A8_UNORM; break;
            default: return nullptr;
            }
            if (attribute.semanticName == nullptr || attribute.semanticName[0] == '\0') return nullptr;

            const RHI::VertexBindingDesc* binding = nullptr;
            for (uint32_t bindingIndex = 0; bindingIndex < desc.inputAssembly.vertexBindingCount; ++bindingIndex)
            {
                if (desc.inputAssembly.vertexBindings[bindingIndex].slot == attribute.binding)
                {
                    binding = &desc.inputAssembly.vertexBindings[bindingIndex];
                    break;
                }
            }
            if (binding == nullptr || binding->stride == 0) return nullptr;

            D3D12_INPUT_ELEMENT_DESC element = {};
            element.SemanticName = attribute.semanticName;
            element.SemanticIndex = attribute.semanticIndex;
            element.Format = format;
            element.InputSlot = attribute.binding;
            element.AlignedByteOffset = attribute.offset;
            element.InputSlotClass = binding->inputRate == RHI::VertexInputRate::PerInstance
                ? D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA
                : D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
            element.InstanceDataStepRate = binding->inputRate == RHI::VertexInputRate::PerInstance ? 1u : 0u;
            inputElements.push_back(element);
        }
        psoDesc.InputLayout = { inputElements.data(), static_cast<UINT>(inputElements.size()) };
        psoDesc.pRootSignature = pRootSignature.Get();

        psoDesc.VS = { vertexShader->GetBinary(), vertexShader->GetBinarySize() };
        if (hasFragmentShader) {
            psoDesc.PS = { fragmentShader->GetBinary(), fragmentShader->GetBinarySize() };
        }

        psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
        switch(desc.rasterization.fillMode)
        {
        case RHI::FillMode::Solid: psoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID; break;
        case RHI::FillMode::Wireframe: psoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_WIREFRAME; break;
        default: return nullptr;
        }
        switch(desc.rasterization.cullMode)
        {
        case RHI::CullMode::None: psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE; break;
        case RHI::CullMode::Front: psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_FRONT; break;
        case RHI::CullMode::Back: psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_BACK; break;
        default: return nullptr;
        }
        switch(desc.rasterization.frontFace)
        {
        case RHI::FrontFace::CounterClockwise: psoDesc.RasterizerState.FrontCounterClockwise = TRUE; break;
        case RHI::FrontFace::Clockwise: psoDesc.RasterizerState.FrontCounterClockwise = FALSE; break;
        default: return nullptr;
        }
        psoDesc.RasterizerState.DepthBias = desc.rasterization.depthBias;
        psoDesc.RasterizerState.SlopeScaledDepthBias = desc.rasterization.depthBiasSlope;
        psoDesc.RasterizerState.DepthBiasClamp = desc.rasterization.depthBiasClamp;
        psoDesc.BlendState = {};
        psoDesc.BlendState.IndependentBlendEnable = desc.colorAttachmentCount > 1 ? TRUE : FALSE;
        for(uint32_t attachmentIndex = 0; attachmentIndex < desc.colorAttachmentCount; ++attachmentIndex)
        {
            const RHI::ColorAttachmentDesc& source = desc.colorAttachments[attachmentIndex];
            if(source.format == RHI::Format::Unknown || (source.writeMask & ~RHI::ColorWriteAll) != 0) return nullptr;
            switch(source.format)
            {
            case RHI::Format::R8G8B8A8_UNORM:
            case RHI::Format::B8G8R8A8_UNORM:
            case RHI::Format::R8G8B8A8_UNORM_SRGB:
            case RHI::Format::B8G8R8A8_UNORM_SRGB:
            case RHI::Format::R16G16B16A16_FLOAT:
            case RHI::Format::R32G32B32A32_FLOAT:
                break;
            default:
                return nullptr;
            }
            D3D12_RENDER_TARGET_BLEND_DESC& target = psoDesc.BlendState.RenderTarget[attachmentIndex];
            target.BlendEnable = source.blendEnable ? TRUE : FALSE;
            target.LogicOpEnable = FALSE;
            const RHI::BlendFactor factors[] = {
                source.sourceColorFactor,
                source.destinationColorFactor,
                source.sourceAlphaFactor,
                source.destinationAlphaFactor
            };
            D3D12_BLEND* nativeFactors[] = {
                &target.SrcBlend,
                &target.DestBlend,
                &target.SrcBlendAlpha,
                &target.DestBlendAlpha
            };
            for(uint32_t factorIndex = 0; factorIndex < 4; ++factorIndex)
            {
                switch(factors[factorIndex])
                {
                case RHI::BlendFactor::Zero: *nativeFactors[factorIndex] = D3D12_BLEND_ZERO; break;
                case RHI::BlendFactor::One: *nativeFactors[factorIndex] = D3D12_BLEND_ONE; break;
                case RHI::BlendFactor::SourceColor: *nativeFactors[factorIndex] = D3D12_BLEND_SRC_COLOR; break;
                case RHI::BlendFactor::OneMinusSourceColor: *nativeFactors[factorIndex] = D3D12_BLEND_INV_SRC_COLOR; break;
                case RHI::BlendFactor::SourceAlpha: *nativeFactors[factorIndex] = D3D12_BLEND_SRC_ALPHA; break;
                case RHI::BlendFactor::OneMinusSourceAlpha: *nativeFactors[factorIndex] = D3D12_BLEND_INV_SRC_ALPHA; break;
                case RHI::BlendFactor::DestinationColor: *nativeFactors[factorIndex] = D3D12_BLEND_DEST_COLOR; break;
                case RHI::BlendFactor::OneMinusDestinationColor: *nativeFactors[factorIndex] = D3D12_BLEND_INV_DEST_COLOR; break;
                case RHI::BlendFactor::DestinationAlpha: *nativeFactors[factorIndex] = D3D12_BLEND_DEST_ALPHA; break;
                case RHI::BlendFactor::OneMinusDestinationAlpha: *nativeFactors[factorIndex] = D3D12_BLEND_INV_DEST_ALPHA; break;
                default: return nullptr;
                }
            }
            const RHI::BlendOp operations[] = { source.colorOp, source.alphaOp };
            D3D12_BLEND_OP* nativeOperations[] = { &target.BlendOp, &target.BlendOpAlpha };
            for(uint32_t operationIndex = 0; operationIndex < 2; ++operationIndex)
            {
                switch(operations[operationIndex])
                {
                case RHI::BlendOp::Add: *nativeOperations[operationIndex] = D3D12_BLEND_OP_ADD; break;
                case RHI::BlendOp::Subtract: *nativeOperations[operationIndex] = D3D12_BLEND_OP_SUBTRACT; break;
                case RHI::BlendOp::ReverseSubtract: *nativeOperations[operationIndex] = D3D12_BLEND_OP_REV_SUBTRACT; break;
                case RHI::BlendOp::Min: *nativeOperations[operationIndex] = D3D12_BLEND_OP_MIN; break;
                case RHI::BlendOp::Max: *nativeOperations[operationIndex] = D3D12_BLEND_OP_MAX; break;
                default: return nullptr;
                }
            }
            target.LogicOp = D3D12_LOGIC_OP_NOOP;
            target.RenderTargetWriteMask = source.writeMask;
            psoDesc.RTVFormats[attachmentIndex] = static_cast<DXGI_FORMAT>(D3D12Texture::ToDxgiFormat(source.format));
            if(psoDesc.RTVFormats[attachmentIndex] == DXGI_FORMAT_UNKNOWN) return nullptr;
        }
        psoDesc.DepthStencilState = {};
        psoDesc.DepthStencilState.DepthEnable = desc.depthStencil.depthTestEnable ? TRUE : FALSE;
        psoDesc.DepthStencilState.DepthWriteMask = desc.depthStencil.depthWriteEnable
            ? D3D12_DEPTH_WRITE_MASK_ALL
            : D3D12_DEPTH_WRITE_MASK_ZERO;
        switch(desc.depthStencil.depthCompareOp)
        {
        case RHI::CompareOp::Never: psoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_NEVER; break;
        case RHI::CompareOp::Less: psoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS; break;
        case RHI::CompareOp::Equal: psoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_EQUAL; break;
        case RHI::CompareOp::LessEqual: psoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL; break;
        case RHI::CompareOp::Greater: psoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER; break;
        case RHI::CompareOp::NotEqual: psoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_NOT_EQUAL; break;
        case RHI::CompareOp::GreaterEqual: psoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL; break;
        case RHI::CompareOp::Always: psoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS; break;
        default: return nullptr;
        }
        psoDesc.DepthStencilState.StencilEnable = desc.depthStencil.stencilTestEnable ? TRUE : FALSE;
        psoDesc.DepthStencilState.StencilReadMask = desc.depthStencil.stencilReadMask;
        psoDesc.DepthStencilState.StencilWriteMask = desc.depthStencil.stencilWriteMask;
        const RHI::StencilFaceDesc* stencilFaces[] = { &desc.depthStencil.frontFace, &desc.depthStencil.backFace };
        D3D12_DEPTH_STENCILOP_DESC* nativeStencilFaces[] = {
            &psoDesc.DepthStencilState.FrontFace,
            &psoDesc.DepthStencilState.BackFace
        };
        for(uint32_t faceIndex = 0; faceIndex < 2; ++faceIndex)
        {
            const RHI::StencilFaceDesc& source = *stencilFaces[faceIndex];
            D3D12_DEPTH_STENCILOP_DESC& target = *nativeStencilFaces[faceIndex];
            switch(source.compareOp)
            {
            case RHI::CompareOp::Never: target.StencilFunc = D3D12_COMPARISON_FUNC_NEVER; break;
            case RHI::CompareOp::Less: target.StencilFunc = D3D12_COMPARISON_FUNC_LESS; break;
            case RHI::CompareOp::Equal: target.StencilFunc = D3D12_COMPARISON_FUNC_EQUAL; break;
            case RHI::CompareOp::LessEqual: target.StencilFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL; break;
            case RHI::CompareOp::Greater: target.StencilFunc = D3D12_COMPARISON_FUNC_GREATER; break;
            case RHI::CompareOp::NotEqual: target.StencilFunc = D3D12_COMPARISON_FUNC_NOT_EQUAL; break;
            case RHI::CompareOp::GreaterEqual: target.StencilFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL; break;
            case RHI::CompareOp::Always: target.StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS; break;
            default: return nullptr;
            }
            const RHI::StencilOp operations[] = { source.failOp, source.depthFailOp, source.passOp };
            D3D12_STENCIL_OP* nativeOperations[] = { &target.StencilFailOp, &target.StencilDepthFailOp, &target.StencilPassOp };
            for(uint32_t operationIndex = 0; operationIndex < 3; ++operationIndex)
            {
                switch(operations[operationIndex])
                {
                case RHI::StencilOp::Keep: *nativeOperations[operationIndex] = D3D12_STENCIL_OP_KEEP; break;
                case RHI::StencilOp::Zero: *nativeOperations[operationIndex] = D3D12_STENCIL_OP_ZERO; break;
                case RHI::StencilOp::Replace: *nativeOperations[operationIndex] = D3D12_STENCIL_OP_REPLACE; break;
                case RHI::StencilOp::IncrementClamp: *nativeOperations[operationIndex] = D3D12_STENCIL_OP_INCR_SAT; break;
                case RHI::StencilOp::DecrementClamp: *nativeOperations[operationIndex] = D3D12_STENCIL_OP_DECR_SAT; break;
                case RHI::StencilOp::Invert: *nativeOperations[operationIndex] = D3D12_STENCIL_OP_INVERT; break;
                case RHI::StencilOp::IncrementWrap: *nativeOperations[operationIndex] = D3D12_STENCIL_OP_INCR; break;
                case RHI::StencilOp::DecrementWrap: *nativeOperations[operationIndex] = D3D12_STENCIL_OP_DECR; break;
                default: return nullptr;
                }
            }
        }
        psoDesc.DSVFormat = hasDepthAttachment
            ? static_cast<DXGI_FORMAT>(D3D12Texture::ToDxgiDepthStencilFormat(desc.depthStencilFormat))
            : DXGI_FORMAT_UNKNOWN;

        psoDesc.SampleMask = UINT_MAX;
        switch (desc.inputAssembly.topology)
        {
        case RHI::PrimitiveTopology::PointList:
            psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
            break;
        case RHI::PrimitiveTopology::LineList:
        case RHI::PrimitiveTopology::LineStrip:
            psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
            break;
        case RHI::PrimitiveTopology::TriangleList:
        case RHI::PrimitiveTopology::TriangleStrip:
            psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            break;
        }
        psoDesc.NumRenderTargets = desc.colorAttachmentCount;
        psoDesc.SampleDesc.Count = 1;

        // 5. PSO 생성
        Microsoft::WRL::ComPtr<ID3D12PipelineState> pPSO;
        if (FAILED(m_internal->device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&pPSO)))) {
            std::cout << "[D3D12] CreateGraphicsPipelineState FAILED (hasFragmentShader=" << hasFragmentShader << ")" << std::endl;
            DumpInfoQueue(m_internal, "CreateGraphicsPipelineState");
            return nullptr;
        }

        // 6. 래퍼 객체로 반환
        DumpInfoQueue(m_internal, "CreateGraphicsPipeline");
        return new D3D12PipelineState(
            pPSO.Get(),
            pRootSignature.Get(),
            desc.inputAssembly.topology,
            desc.inputAssembly.vertexBindings,
            desc.inputAssembly.vertexBindingCount,
            desc.depthStencil.stencilReference,
            std::move(resourceBindings),
            std::move(inlineConstantRanges));
    }

    RHI::IResourceSet* D3D12Device::CreateResourceSet(RHI::IPipelineState* pipeline) {
        auto* d3dPipeline = dynamic_cast<D3D12PipelineState*>(pipeline);
        if(d3dPipeline == nullptr) return nullptr;
        auto* resourceSet = new D3D12ResourceSet(m_internal->device.Get(), d3dPipeline);
        if(!resourceSet->IsValid()) {
            delete resourceSet;
            return nullptr;
        }
        return resourceSet;
    }

    bool D3D12Device::UpdateResourceSet(RHI::IResourceSet* resourceSet, const RHI::ResourceSetWrite* writes, uint32_t writeCount) {
        auto* d3dSet = dynamic_cast<D3D12ResourceSet*>(resourceSet);
        return d3dSet != nullptr && d3dSet->Update(m_internal->device.Get(), writes, writeCount);
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
    void D3D12Device::DestroyShader(RHI::IShader* shader) { delete shader; }
    void D3D12Device::DestroySampler(RHI::ISampler* sampler) { delete sampler; }
    void D3D12Device::DestroyResourceSet(RHI::IResourceSet* resourceSet) { delete resourceSet; }

    RHI::ITexture* D3D12Device::GetBackBuffer() {
        return m_internal->backBufferTextures[m_internal->backBufferIndex];
    }
}
