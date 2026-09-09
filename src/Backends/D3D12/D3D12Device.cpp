#include "D3D12Device.h"
#include "D3D12Buffer.h"
#include "D3D12CommandList.h"
#include "D3D12PipelineState.h"
#include "D3D12ResourceSet.h"
#include "D3D12Shader.h"
#include "D3D12Texture.h"
#include "RHI/ResourceSet.h"
#include "RHI/Shader.h"
#include "RHI/Pipeline.h"
#include "RHI/Readback.h"
#include "d3dx12.h"

#include <d3d12.h>
#include <dxgi1_6.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <memory>
#include <utility>
#include <vector>
#include <wrl.h>

using Microsoft::WRL::ComPtr;

namespace dy::Backends
{
    struct D3D12ObjectDeleter
    {
        template<typename Object>
        void operator()(Object* object) const
        {
            delete object;
        }
    };

    struct D3D12FrameSlot
    {
        uint64_t completionValue = 0;
    };

    struct D3D12SubmissionRecord
    {
        uint64_t completionValue = 0;
        std::vector<std::unique_ptr<D3D12CommandList, D3D12ObjectDeleter>> commandLists;
    };

    template<typename Object>
    struct D3D12RetiredObject
    {
        uint64_t completionValue = 0;
        std::unique_ptr<Object, D3D12ObjectDeleter> object;
    };

    template<typename Object, typename Interface>
    bool OwnsObject(
        const std::vector<std::unique_ptr<Object, D3D12ObjectDeleter>>& liveObjects,
        const Interface* object)
    {
        return std::find_if(
            liveObjects.begin(),
            liveObjects.end(),
            [object](const std::unique_ptr<Object, D3D12ObjectDeleter>& candidate)
            {
                return static_cast<const Interface*>(candidate.get()) == object;
            }) != liveObjects.end();
    }

    template<typename Object, typename Interface>
    bool RetireObject(
        std::vector<std::unique_ptr<Object, D3D12ObjectDeleter>>& liveObjects,
        Interface* object,
        uint64_t completionValue,
        std::vector<D3D12RetiredObject<Object>>& retiredObjects)
    {
        const auto found = std::find_if(
            liveObjects.begin(),
            liveObjects.end(),
            [object](const std::unique_ptr<Object, D3D12ObjectDeleter>& candidate)
            {
                return static_cast<Interface*>(candidate.get()) == object;
            });
        if (found == liveObjects.end()) return false;
        retiredObjects.push_back({completionValue, nullptr});
        retiredObjects.back().object = std::move(*found);
        liveObjects.erase(found);
        return true;
    }

    template<typename Object>
    void ReclaimObjects(
        std::vector<D3D12RetiredObject<Object>>& objects,
        uint64_t completedValue)
    {
        objects.erase(
            std::remove_if(
                objects.begin(),
                objects.end(),
                [completedValue](const D3D12RetiredObject<Object>& object)
                {
                    return object.completionValue <= completedValue;
                }),
            objects.end());
    }

    // 헤더에서 선언만 했던 구조체의 실제 정의
    struct D3D12InternalState
    {
        ComPtr<ID3D12Device> device;
        ComPtr<ID3D12InfoQueue> infoQueue; // 디버그 빌드: D3D12 검증 메시지 수집
        ComPtr<ID3D12CommandQueue> commandQueue;
        HWND windowHandle = nullptr;
        ComPtr<IDXGISwapChain3> swapChain;
        ComPtr<ID3D12DescriptorHeap> rtvHeap;
        std::vector<std::unique_ptr<D3D12Texture, D3D12ObjectDeleter>> backBufferTextures;
        std::vector<uint64_t> imageCompletionValues;

        ComPtr<ID3D12Fence> fence;
        uint64_t nextCompletionValue = 1;
        HANDLE fenceEvent = nullptr;
        std::vector<D3D12FrameSlot> frames;
        std::vector<D3D12SubmissionRecord> submissions;
        std::vector<std::unique_ptr<D3D12CommandList, D3D12ObjectDeleter>> activeCommandLists;
        // Keep diagnostic work alive if native submission succeeds but its
        // completion signal fails. Device teardown drains the queue.
        std::vector<ComPtr<ID3D12Object>> readbackResources;
        std::vector<std::unique_ptr<D3D12Buffer, D3D12ObjectDeleter>> liveBuffers;
        std::vector<std::unique_ptr<D3D12Texture, D3D12ObjectDeleter>> liveTextures;
        std::vector<std::unique_ptr<D3D12Shader, D3D12ObjectDeleter>> liveShaders;
        std::vector<std::unique_ptr<D3D12PipelineState, D3D12ObjectDeleter>> livePipelines;
        std::vector<std::unique_ptr<D3D12ResourceSet, D3D12ObjectDeleter>> liveResourceSets;
        std::vector<D3D12RetiredObject<D3D12Buffer>> retiredBuffers;
        std::vector<D3D12RetiredObject<D3D12Texture>> retiredTextures;
        std::vector<D3D12RetiredObject<D3D12Shader>> retiredShaders;
        std::vector<D3D12RetiredObject<D3D12PipelineState>> retiredPipelines;
        std::vector<D3D12RetiredObject<D3D12ResourceSet>> retiredResourceSets;
        uint64_t lastSubmittedValue = 0;

        uint32_t nextFrameIndex = 0;
        uint32_t activeFrameIndex = 0;
        uint32_t activeImageIndex = 0;
        UINT presentSyncInterval = 1;
        UINT presentFlags = 0;
        UINT swapchainFlags = 0;
        DXGI_FORMAT swapchainResourceFormat = DXGI_FORMAT_UNKNOWN;
        DXGI_FORMAT swapchainRtvFormat = DXGI_FORMAT_UNKNOWN;
        RHI::Format swapchainFormat = RHI::Format::Unknown;
        RHI::SwapchainDesc swapchainDesc = {};
        bool swapchainReady = false;
        bool frameReady = false;
        bool frameSubmitted = false;
        bool submissionFaulted = false;

        bool CollectCompletedWork(uint64_t& completedValue)
        {
            if (fence == nullptr) return false;
            completedValue = fence->GetCompletedValue();
            if (completedValue == std::numeric_limits<uint64_t>::max())
            {
                submissionFaulted = true;
                return false;
            }
            submissions.erase(
                std::remove_if(
                    submissions.begin(),
                    submissions.end(),
                    [completedValue](const D3D12SubmissionRecord& submission)
                    {
                        return submission.completionValue <= completedValue;
                    }),
                submissions.end());
            ReclaimObjects(retiredResourceSets, completedValue);
            ReclaimObjects(retiredPipelines, completedValue);
            ReclaimObjects(retiredShaders, completedValue);
            ReclaimObjects(retiredTextures, completedValue);
            ReclaimObjects(retiredBuffers, completedValue);
            return true;
        }

        bool CollectCompletedWork()
        {
            uint64_t completedValue = 0;
            return CollectCompletedWork(completedValue);
        }
    };

    bool D3D12Device::ReadTexture(RHI::TextureHandle texture, RHI::TextureReadback& result)
    {
        if(!m_internal || !texture || m_internal->submissionFaulted ||
            !m_internal->activeCommandLists.empty()) return false;
        const bool backBuffer = texture == GetBackBuffer();
        if(backBuffer)
        {
            if(!m_internal->swapchainDesc.allowReadback || !m_internal->frameSubmitted) return false;
        }
        else if(!OwnsObject(m_internal->liveTextures, texture)) return false;
        const auto& desc = texture->GetDesc();
        if(!RHI::IsReadbackFormat(desc.format) || !desc.width || !desc.height ||
            desc.width > UINT32_MAX / 4u) return false;
        const uint64_t bytes = static_cast<uint64_t>(desc.width) * desc.height * 4u;
        if(bytes > std::numeric_limits<size_t>::max()) return false;
        auto* image = static_cast<D3D12Texture*>(texture);
        D3D12_RESOURCE_STATES before;
        switch(image->GetState(0, 0))
        {
        case RHI::ResourceState::Common: before = D3D12_RESOURCE_STATE_COMMON; break;
        case RHI::ResourceState::Present: before = D3D12_RESOURCE_STATE_PRESENT; break;
        case RHI::ResourceState::CopyDestination: before = D3D12_RESOURCE_STATE_COPY_DEST; break;
        case RHI::ResourceState::RenderTarget: before = D3D12_RESOURCE_STATE_RENDER_TARGET; break;
        case RHI::ResourceState::ShaderResource:
            before = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE; break;
        case RHI::ResourceState::UnorderedAccess: before = D3D12_RESOURCE_STATE_UNORDERED_ACCESS; break;
        default: return false;
        }
        auto* resource = static_cast<ID3D12Resource*>(image->GetNativeResource());
        if(!resource) return false;
        const auto nativeDesc = resource->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
        UINT64 storageBytes = 0;
        m_internal->device->GetCopyableFootprints(&nativeDesc, 0, 1, 0, &footprint, nullptr, nullptr, &storageBytes);
        if(storageBytes > std::numeric_limits<size_t>::max()) return false;
        const auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_READBACK);
        const auto bufferDesc = CD3DX12_RESOURCE_DESC::Buffer(storageBytes);
        ComPtr<ID3D12Resource> buffer;
        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12GraphicsCommandList> commands;
        ComPtr<ID3D12Fence> fence;
        if(FAILED(m_internal->device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE,
                &bufferDesc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&buffer))) ||
            FAILED(m_internal->device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
            FAILED(m_internal->device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                allocator.Get(), nullptr, IID_PPV_ARGS(&commands))) ||
            FAILED(m_internal->device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) return false;
        struct Event
        {
            HANDLE handle = CreateEvent(nullptr, FALSE, FALSE, nullptr);
            ~Event() { if(handle) CloseHandle(handle); }
        } event;
        if(!event.handle) return false;
        RHI::TextureReadback output;
        output.width = desc.width; output.height = desc.height;
        output.rowPitch = desc.width * 4u; output.format = desc.format;
        output.pixels.resize(static_cast<size_t>(bytes));
        auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(resource, before, D3D12_RESOURCE_STATE_COPY_SOURCE, 0);
        commands->ResourceBarrier(1, &barrier);
        D3D12_TEXTURE_COPY_LOCATION source{};
        source.pResource = resource; source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION destination{};
        destination.pResource = buffer.Get(); destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        destination.PlacedFootprint = footprint;
        commands->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
        std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
        commands->ResourceBarrier(1, &barrier);
        if(FAILED(commands->Close())) return false;
        const size_t retirementBase = m_internal->readbackResources.size();
        m_internal->readbackResources.reserve(retirementBase + 4);
        m_internal->readbackResources.emplace_back(buffer.Get());
        m_internal->readbackResources.emplace_back(allocator.Get());
        m_internal->readbackResources.emplace_back(commands.Get());
        m_internal->readbackResources.emplace_back(fence.Get());
        ID3D12CommandList* native = commands.Get();
        m_internal->commandQueue->ExecuteCommandLists(1, &native);
        if(FAILED(m_internal->commandQueue->Signal(fence.Get(), 1)) ||
            FAILED(fence->SetEventOnCompletion(1, event.handle)) ||
            WaitForSingleObject(event.handle, INFINITE) != WAIT_OBJECT_0 ||
            fence->GetCompletedValue() == UINT64_MAX)
        {
            m_internal->submissionFaulted = true;
            return false;
        }
        m_internal->readbackResources.resize(retirementBase);
        void* mapped = nullptr;
        const D3D12_RANGE range{0, static_cast<SIZE_T>(storageBytes)};
        if(FAILED(buffer->Map(0, &range, &mapped))) return false;
        for(uint32_t row = 0; row < desc.height; ++row)
            std::memcpy(output.pixels.data() + static_cast<size_t>(row) * output.rowPitch,
                static_cast<const uint8_t*>(mapped) + footprint.Offset +
                    static_cast<size_t>(row) * footprint.Footprint.RowPitch, output.rowPitch);
        const D3D12_RANGE written{0, 0};
        buffer->Unmap(0, &written);
        result = std::move(output);
        return true;
    }

    static bool CreateBackBufferViews(
        D3D12InternalState* internal,
        IDXGISwapChain3* swapchain,
        RHI::Format format,
        DXGI_FORMAT resourceFormat,
        DXGI_FORMAT rtvFormat,
        ComPtr<ID3D12DescriptorHeap>& rtvHeap,
        std::vector<std::unique_ptr<D3D12Texture, D3D12ObjectDeleter>>& textures)
    {
        if (internal == nullptr || internal->device == nullptr || swapchain == nullptr)
        {
            return false;
        }

        DXGI_SWAP_CHAIN_DESC1 actualDesc = {};
        if (FAILED(swapchain->GetDesc1(&actualDesc)) || actualDesc.BufferCount == 0 ||
            actualDesc.Format != resourceFormat)
        {
            return false;
        }

        D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
        heapDesc.NumDescriptors = actualDesc.BufferCount;
        heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        ComPtr<ID3D12DescriptorHeap> newRtvHeap;
        if (FAILED(internal->device->CreateDescriptorHeap(
                &heapDesc, IID_PPV_ARGS(&newRtvHeap))))
        {
            return false;
        }

        const uint32_t descriptorSize =
            internal->device->GetDescriptorHandleIncrementSize(
                D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle =
            newRtvHeap->GetCPUDescriptorHandleForHeapStart();
        D3D12_RENDER_TARGET_VIEW_DESC viewDesc = {};
        viewDesc.Format = rtvFormat;
        viewDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;

        std::vector<std::unique_ptr<D3D12Texture, D3D12ObjectDeleter>> newTextures;
        newTextures.reserve(actualDesc.BufferCount);
        for (UINT imageIndex = 0; imageIndex < actualDesc.BufferCount; ++imageIndex)
        {
            ComPtr<ID3D12Resource> resource;
            if (FAILED(swapchain->GetBuffer(imageIndex, IID_PPV_ARGS(&resource))))
            {
                return false;
            }

            internal->device->CreateRenderTargetView(
                resource.Get(), &viewDesc, rtvHandle);
            const D3D12_RESOURCE_DESC nativeDesc = resource->GetDesc();
            RHI::TextureDesc textureDesc = {};
            textureDesc.width = static_cast<uint32_t>(nativeDesc.Width);
            textureDesc.height = nativeDesc.Height;
            textureDesc.format = format;
            textureDesc.usage = RHI::TextureUsage::RenderTarget;
            newTextures.push_back(
                std::unique_ptr<D3D12Texture, D3D12ObjectDeleter>(
                    new D3D12Texture(
                        resource.Get(), textureDesc, rtvHandle.ptr, true)));
            rtvHandle.ptr += descriptorSize;
        }

        rtvHeap = std::move(newRtvHeap);
        textures = std::move(newTextures);
        return true;
    }

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
        if (m_internal == nullptr) return;

        if (m_internal->commandQueue != nullptr &&
            m_internal->fence != nullptr &&
            m_internal->fenceEvent != nullptr)
        {
            const uint64_t completionValue = m_internal->nextCompletionValue++;
            if (SUCCEEDED(m_internal->commandQueue->Signal(
                    m_internal->fence.Get(), completionValue)) &&
                m_internal->fence->GetCompletedValue() < completionValue &&
                SUCCEEDED(m_internal->fence->SetEventOnCompletion(
                    completionValue, m_internal->fenceEvent)))
            {
                WaitForSingleObject(m_internal->fenceEvent, INFINITE);
            }
        }

        if (m_internal->fenceEvent != nullptr) CloseHandle(m_internal->fenceEvent);
        delete m_internal;
    }

    int D3D12Device::Initialize(const void* windowHandle, const RHI::DeviceDesc& desc)
    {
        if (windowHandle == nullptr || desc.maxFramesInFlight == 0) return -1;
        m_internal->windowHandle = static_cast<HWND>(const_cast<void*>(windowHandle));

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
        if (FAILED(m_internal->device->CreateCommandQueue(
                &queueDesc, IID_PPV_ARGS(&m_internal->commandQueue))))
        {
            return -1;
        }

        // 6. 동기화용 펜스(Fence) 생성
        if (FAILED(m_internal->device->CreateFence(
                0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_internal->fence))))
        {
            return -1;
        }
        m_internal->fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        if (m_internal->fenceEvent == nullptr) return -1;

        m_internal->frames.resize(desc.maxFramesInFlight);
        return 0;
    }

    bool D3D12Device::CreateSwapchain(const RHI::SwapchainDesc& desc)
    {
        if (m_internal == nullptr || m_internal->device == nullptr ||
            m_internal->commandQueue == nullptr || m_internal->windowHandle == nullptr ||
            m_internal->swapchainReady || desc.minimumImageCount == 0 ||
            desc.minimumImageCount > DXGI_MAX_SWAP_CHAIN_BUFFERS)
        {
            return false;
        }

        RHI::Format actualFormat = desc.format;
        DXGI_FORMAT resourceFormat = DXGI_FORMAT_UNKNOWN;
        DXGI_FORMAT rtvFormat = DXGI_FORMAT_UNKNOWN;
        switch (desc.format)
        {
        case RHI::Format::Unknown:
            actualFormat = RHI::Format::R8G8B8A8_UNORM;
            resourceFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
            rtvFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
            break;
        case RHI::Format::R8G8B8A8_UNORM:
            resourceFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
            rtvFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
            break;
        case RHI::Format::B8G8R8A8_UNORM:
            resourceFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
            rtvFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
            break;
        case RHI::Format::R8G8B8A8_UNORM_SRGB:
            resourceFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
            rtvFormat = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
            break;
        case RHI::Format::B8G8R8A8_UNORM_SRGB:
            resourceFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
            rtvFormat = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
            break;
        case RHI::Format::R16G16B16A16_FLOAT:
            resourceFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
            rtvFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
            break;
        default:
            return false;
        }

        ComPtr<IDXGIFactory4> factory;
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return false;

        UINT presentSyncInterval = 1;
        UINT presentFlags = 0;
        UINT swapchainFlags = 0;
        switch (desc.presentMode)
        {
        case RHI::PresentMode::Fifo:
            break;
        case RHI::PresentMode::Immediate:
        {
            ComPtr<IDXGIFactory5> factory5;
            BOOL tearingSupported = FALSE;
            if (FAILED(factory.As(&factory5)) || factory5 == nullptr ||
                FAILED(factory5->CheckFeatureSupport(
                    DXGI_FEATURE_PRESENT_ALLOW_TEARING,
                    &tearingSupported,
                    sizeof(tearingSupported))) ||
                tearingSupported == FALSE)
            {
                return false;
            }
            presentSyncInterval = 0;
            presentFlags = DXGI_PRESENT_ALLOW_TEARING;
            swapchainFlags = DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
            break;
        }
        case RHI::PresentMode::Mailbox:
        default:
            return false;
        }

        RECT clientRect = {};
        if (!GetClientRect(m_internal->windowHandle, &clientRect) ||
            clientRect.right <= clientRect.left ||
            clientRect.bottom <= clientRect.top)
        {
            return false;
        }

        DXGI_SWAP_CHAIN_DESC1 requestedDesc = {};
        requestedDesc.BufferCount = std::max(desc.minimumImageCount, 2u);
        requestedDesc.Width = static_cast<UINT>(clientRect.right - clientRect.left);
        requestedDesc.Height = static_cast<UINT>(clientRect.bottom - clientRect.top);
        requestedDesc.Format = resourceFormat;
        requestedDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        requestedDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        requestedDesc.SampleDesc.Count = 1;
        requestedDesc.Flags = swapchainFlags;

        ComPtr<IDXGISwapChain1> swapchain1;
        ComPtr<IDXGISwapChain3> swapchain3;
        if (FAILED(factory->CreateSwapChainForHwnd(
                m_internal->commandQueue.Get(),
                m_internal->windowHandle,
                &requestedDesc,
                nullptr,
                nullptr,
                &swapchain1)) ||
            FAILED(swapchain1.As(&swapchain3)))
        {
            return false;
        }

        DXGI_SWAP_CHAIN_DESC1 actualDesc = {};
        if (FAILED(swapchain3->GetDesc1(&actualDesc)) ||
            actualDesc.BufferCount < desc.minimumImageCount ||
            actualDesc.Format != resourceFormat)
        {
            return false;
        }

        ComPtr<ID3D12DescriptorHeap> rtvHeap;
        std::vector<std::unique_ptr<D3D12Texture, D3D12ObjectDeleter>> backBufferTextures;
        if (!CreateBackBufferViews(
                m_internal,
                swapchain3.Get(),
                actualFormat,
                resourceFormat,
                rtvFormat,
                rtvHeap,
                backBufferTextures))
        {
            return false;
        }

        m_internal->swapChain = std::move(swapchain3);
        if((desc.initialWidth && desc.initialWidth != actualDesc.Width) ||
            (desc.initialHeight && desc.initialHeight != actualDesc.Height))
        {
            m_internal->swapChain.Reset();
            return false;
        }
        m_internal->rtvHeap = std::move(rtvHeap);
        m_internal->backBufferTextures = std::move(backBufferTextures);
        m_internal->imageCompletionValues.assign(actualDesc.BufferCount, 0);
        m_internal->presentSyncInterval = presentSyncInterval;
        m_internal->presentFlags = presentFlags;
        m_internal->swapchainFlags = swapchainFlags;
        m_internal->swapchainResourceFormat = resourceFormat;
        m_internal->swapchainRtvFormat = rtvFormat;
        m_internal->swapchainFormat = actualFormat;
        m_internal->swapchainDesc = desc;
        m_internal->activeImageIndex = m_internal->swapChain->GetCurrentBackBufferIndex();
        m_internal->swapchainReady =
            m_internal->activeImageIndex < m_internal->backBufferTextures.size();
        return m_internal->swapchainReady;
    }

    bool D3D12Device::BeginFrame()
    {
        if (m_internal == nullptr || !m_internal->swapchainReady ||
            m_internal->submissionFaulted || m_internal->frameSubmitted ||
            m_internal->frames.empty())
        {
            return false;
        }

        uint64_t completedValue = 0;
        if (!m_internal->CollectCompletedWork(completedValue)) return false;
        if (m_internal->frameReady)
        {
            return m_internal->activeFrameIndex == m_internal->nextFrameIndex &&
                m_internal->activeFrameIndex < m_internal->frames.size() &&
                m_internal->activeImageIndex <
                    m_internal->backBufferTextures.size() &&
                m_internal->activeImageIndex ==
                    m_internal->swapChain->GetCurrentBackBufferIndex();
        }

        RECT clientRect = {};
        if (!GetClientRect(m_internal->windowHandle, &clientRect) ||
            clientRect.right <= clientRect.left ||
            clientRect.bottom <= clientRect.top)
        {
            return false;
        }
        const uint32_t width = static_cast<uint32_t>(
            clientRect.right - clientRect.left);
        const uint32_t height = static_cast<uint32_t>(
            clientRect.bottom - clientRect.top);

        if (m_internal->backBufferTextures.empty())
        {
            ComPtr<ID3D12DescriptorHeap> rtvHeap;
            std::vector<std::unique_ptr<D3D12Texture, D3D12ObjectDeleter>> backBufferTextures;
            if (!CreateBackBufferViews(
                    m_internal,
                    m_internal->swapChain.Get(),
                    m_internal->swapchainFormat,
                    m_internal->swapchainResourceFormat,
                    m_internal->swapchainRtvFormat,
                    rtvHeap,
                    backBufferTextures))
            {
                return false;
            }
            m_internal->rtvHeap = std::move(rtvHeap);
            m_internal->backBufferTextures = std::move(backBufferTextures);
            m_internal->imageCompletionValues.assign(
                m_internal->backBufferTextures.size(), 0);
        }

        const RHI::TextureDesc& backBufferDesc =
            m_internal->backBufferTextures.front()->GetDesc();
        if (backBufferDesc.width != width || backBufferDesc.height != height)
        {
            for (const D3D12FrameSlot& frame : m_internal->frames)
            {
                if (frame.completionValue > completedValue) return false;
            }
            for (uint64_t completionValue : m_internal->imageCompletionValues)
            {
                if (completionValue > completedValue) return false;
            }
            for (const std::unique_ptr<D3D12CommandList, D3D12ObjectDeleter>& commandList :
                m_internal->activeCommandLists)
            {
                if (!commandList->GetReferencedSwapchainImages().empty()) return false;
            }

            const UINT imageCount = static_cast<UINT>(
                m_internal->imageCompletionValues.size());
            if (imageCount < m_internal->swapchainDesc.minimumImageCount)
            {
                return false;
            }

            m_internal->backBufferTextures.clear();
            m_internal->rtvHeap.Reset();
            if (FAILED(m_internal->swapChain->ResizeBuffers(
                    imageCount,
                    width,
                    height,
                    m_internal->swapchainResourceFormat,
                    m_internal->swapchainFlags)))
            {
                ComPtr<ID3D12DescriptorHeap> rtvHeap;
                std::vector<std::unique_ptr<D3D12Texture, D3D12ObjectDeleter>> backBufferTextures;
                if (CreateBackBufferViews(
                        m_internal,
                        m_internal->swapChain.Get(),
                        m_internal->swapchainFormat,
                        m_internal->swapchainResourceFormat,
                        m_internal->swapchainRtvFormat,
                        rtvHeap,
                        backBufferTextures))
                {
                    m_internal->rtvHeap = std::move(rtvHeap);
                    m_internal->backBufferTextures = std::move(backBufferTextures);
                    m_internal->imageCompletionValues.assign(
                        m_internal->backBufferTextures.size(), 0);
                }
                return false;
            }

            ComPtr<ID3D12DescriptorHeap> rtvHeap;
            std::vector<std::unique_ptr<D3D12Texture, D3D12ObjectDeleter>> backBufferTextures;
            const RHI::Format format =
                m_internal->swapchainDesc.format == RHI::Format::Unknown
                    ? m_internal->swapchainFormat
                    : m_internal->swapchainDesc.format;
            if (!CreateBackBufferViews(
                    m_internal,
                    m_internal->swapChain.Get(),
                    format,
                    m_internal->swapchainResourceFormat,
                    m_internal->swapchainRtvFormat,
                    rtvHeap,
                    backBufferTextures))
            {
                return false;
            }
            m_internal->rtvHeap = std::move(rtvHeap);
            m_internal->backBufferTextures = std::move(backBufferTextures);
            m_internal->imageCompletionValues.assign(
                m_internal->backBufferTextures.size(), 0);
        }

        const uint32_t imageIndex = m_internal->swapChain->GetCurrentBackBufferIndex();
        if (m_internal->nextFrameIndex >= m_internal->frames.size() ||
            imageIndex >= m_internal->imageCompletionValues.size() ||
            m_internal->frames[m_internal->nextFrameIndex].completionValue > completedValue ||
            m_internal->imageCompletionValues[imageIndex] > completedValue)
        {
            return false;
        }

        m_internal->activeFrameIndex = m_internal->nextFrameIndex;
        m_internal->activeImageIndex = imageIndex;
        m_internal->frameReady = true;
        return true;
    }

    RHI::ICommandList* D3D12Device::AcquireCommandList()
    {
        if (m_internal == nullptr || m_internal->device == nullptr ||
            m_internal->submissionFaulted)
        {
            return nullptr;
        }

        if (!m_internal->CollectCompletedWork()) return nullptr;

        auto commandList = std::unique_ptr<D3D12CommandList, D3D12ObjectDeleter>(
            new D3D12CommandList(m_internal->device.Get()));
        if (commandList->GetNativeList() == nullptr) return nullptr;
        D3D12CommandList* result = commandList.get();
        m_internal->activeCommandLists.push_back(std::move(commandList));
        return result;
    }

    bool D3D12Device::Submit(RHI::ICommandList** cmdLists, uint32_t count)
    {
        if (m_internal == nullptr || cmdLists == nullptr || count == 0)
        {
            return false;
        }

        D3D12Texture* activeBackBuffer = nullptr;
        if (m_internal->frameReady &&
            m_internal->activeImageIndex < m_internal->backBufferTextures.size())
        {
            activeBackBuffer =
                m_internal->backBufferTextures[m_internal->activeImageIndex].get();
        }

        std::vector<D3D12CommandList*> submittedCommandLists;
        submittedCommandLists.reserve(count);
        std::vector<ID3D12CommandList*> nativeCommandLists;
        nativeCommandLists.reserve(count);
        bool frameSubmission = false;
        bool submissionValid = !m_internal->submissionFaulted &&
            m_internal->commandQueue != nullptr && m_internal->fence != nullptr;
        for (uint32_t index = 0; index < count; ++index)
        {
            if (cmdLists[index] == nullptr) return false;
            for (uint32_t previous = 0; previous < index; ++previous)
            {
                if (cmdLists[previous] == cmdLists[index]) return false;
            }

            const auto owned = std::find_if(
                m_internal->activeCommandLists.begin(),
                m_internal->activeCommandLists.end(),
                [command = cmdLists[index]](
                    const std::unique_ptr<D3D12CommandList, D3D12ObjectDeleter>& candidate)
                {
                    return candidate.get() == command;
                });
            if (owned == m_internal->activeCommandLists.end()) return false;

            D3D12CommandList* commandList = owned->get();
            if (!commandList->IsClosed() ||
                commandList->GetNativeList() == nullptr)
            {
                return false;
            }

            for (D3D12Texture* image : commandList->GetReferencedSwapchainImages())
            {
                frameSubmission = true;
                if (activeBackBuffer == nullptr || image != activeBackBuffer)
                {
                    submissionValid = false;
                }
            }

            submittedCommandLists.push_back(commandList);
            nativeCommandLists.push_back(
                static_cast<ID3D12CommandList*>(commandList->GetNativeList()));
        }

        D3D12SubmissionState resourceStates = {};
        if (submissionValid)
        {
            for (D3D12CommandList* commandList : submittedCommandLists)
            {
                if (!commandList->ValidateForSubmit(resourceStates))
                {
                    submissionValid = false;
                    break;
                }
            }
        }
        if (submissionValid && frameSubmission)
        {
            const auto key = std::make_pair(activeBackBuffer, 0u);
            const auto found = resourceStates.textureSubresources.find(key);
            const RHI::ResourceState finalState =
                found == resourceStates.textureSubresources.end()
                ? activeBackBuffer->GetState(0, 0)
                : found->second;
            if (finalState != RHI::ResourceState::Present)
                submissionValid = false;
        }
        if (!submissionValid)
        {
            for (D3D12CommandList* commandList : submittedCommandLists)
            {
                const auto owned = std::find_if(
                    m_internal->activeCommandLists.begin(),
                    m_internal->activeCommandLists.end(),
                    [commandList](
                        const std::unique_ptr<D3D12CommandList, D3D12ObjectDeleter>& candidate)
                    {
                        return candidate.get() == commandList;
                    });
                if (owned != m_internal->activeCommandLists.end())
                    m_internal->activeCommandLists.erase(owned);
            }
            return false;
        }

        m_internal->submissions.emplace_back();
        D3D12SubmissionRecord& submission = m_internal->submissions.back();
        submission.completionValue = m_internal->nextCompletionValue++;
        submission.commandLists.reserve(count);
        for (D3D12CommandList* commandList : submittedCommandLists)
        {
            const auto owned = std::find_if(
                m_internal->activeCommandLists.begin(),
                m_internal->activeCommandLists.end(),
                [commandList](
                    const std::unique_ptr<D3D12CommandList, D3D12ObjectDeleter>& candidate)
                {
                    return candidate.get() == commandList;
                });
            submission.commandLists.push_back(std::move(*owned));
            m_internal->activeCommandLists.erase(owned);
        }

        m_internal->commandQueue->ExecuteCommandLists(
            count, nativeCommandLists.data());
        if (FAILED(m_internal->commandQueue->Signal(
                m_internal->fence.Get(), submission.completionValue)))
        {
            submission.completionValue = std::numeric_limits<uint64_t>::max();
            m_internal->lastSubmittedValue = submission.completionValue;
            m_internal->submissionFaulted = true;
            if (frameSubmission) m_internal->frameReady = false;
            return false;
        }

        for (const std::unique_ptr<D3D12CommandList, D3D12ObjectDeleter>& commandList :
            submission.commandLists)
        {
            commandList->CommitResourceStates();
        }
        m_internal->lastSubmittedValue = submission.completionValue;

        if (frameSubmission)
        {
            m_internal->frames[m_internal->activeFrameIndex].completionValue =
                submission.completionValue;
            m_internal->imageCompletionValues[m_internal->activeImageIndex] =
                submission.completionValue;
            m_internal->nextFrameIndex =
                (m_internal->activeFrameIndex + 1) %
                static_cast<uint32_t>(m_internal->frames.size());
            m_internal->frameReady = false;
            m_internal->frameSubmitted = true;
        }
        DumpInfoQueue(m_internal, "Submit");
        return true;
    }

    void D3D12Device::Present()
    {
        if (m_internal == nullptr || m_internal->submissionFaulted ||
            !m_internal->frameSubmitted)
        {
            return;
        }

        const HRESULT result = m_internal->swapChain->Present(
            m_internal->presentSyncInterval,
            m_internal->presentFlags);
        m_internal->frameSubmitted = false;
        if (FAILED(result)) m_internal->submissionFaulted = true;
        DumpInfoQueue(m_internal, "Present");
    }

    namespace
    {
        bool HasUsage(RHI::BufferUsage usage, RHI::BufferUsage flag)
        {
            return (static_cast<uint32_t>(usage) & static_cast<uint32_t>(flag)) != 0;
        }

        bool HasUsage(RHI::TextureUsage usage, RHI::TextureUsage flag)
        {
            return (static_cast<uint32_t>(usage) & static_cast<uint32_t>(flag)) != 0;
        }

        bool IsDefaultSubresourceRange(
            const RHI::TextureSubresourceRange& range)
        {
            return range.firstMipLevel == 0 && range.mipLevelCount == 0 &&
                range.firstArrayLayer == 0 && range.arrayLayerCount == 0;
        }

        bool ResolveTextureSubresourceRange(
            const RHI::Texture& texture,
            const RHI::TextureSubresourceRange& range,
            uint32_t& mipLevelCount,
            uint32_t& arrayLayerCount)
        {
            if (range.firstMipLevel >= texture.GetDesc().mipLevels ||
                range.firstArrayLayer >= texture.GetDesc().depthOrArraySize)
            {
                return false;
            }
            mipLevelCount = range.mipLevelCount == 0
                ? texture.GetDesc().mipLevels - range.firstMipLevel
                : range.mipLevelCount;
            arrayLayerCount = range.arrayLayerCount == 0
                ? texture.GetDesc().depthOrArraySize - range.firstArrayLayer
                : range.arrayLayerCount;
            return mipLevelCount <= texture.GetDesc().mipLevels - range.firstMipLevel &&
                arrayLayerCount <=
                    texture.GetDesc().depthOrArraySize - range.firstArrayLayer;
        }

        bool HasStage(RHI::ShaderStageFlags stages, RHI::ShaderStageFlags stage)
        {
            return (static_cast<uint32_t>(stages) & static_cast<uint32_t>(stage)) != 0;
        }

        D3D12_SHADER_VISIBILITY ToShaderVisibility(RHI::ShaderStageFlags stages)
        {
            const bool vertex = HasStage(stages, RHI::ShaderStageFlags::Vertex);
            const bool fragment = HasStage(stages, RHI::ShaderStageFlags::Fragment);
            if (vertex && !fragment) return D3D12_SHADER_VISIBILITY_VERTEX;
            if (fragment && !vertex) return D3D12_SHADER_VISIBILITY_PIXEL;
            return D3D12_SHADER_VISIBILITY_ALL;
        }

        DXGI_FORMAT ToVertexFormat(RHI::Format format)
        {
            switch (format)
            {
            case RHI::Format::R8G8B8A8_UNORM: return DXGI_FORMAT_R8G8B8A8_UNORM;
            case RHI::Format::R16G16B16A16_FLOAT: return DXGI_FORMAT_R16G16B16A16_FLOAT;
            case RHI::Format::R32G32_FLOAT: return DXGI_FORMAT_R32G32_FLOAT;
            case RHI::Format::R32G32B32_FLOAT: return DXGI_FORMAT_R32G32B32_FLOAT;
            case RHI::Format::R32G32B32A32_FLOAT: return DXGI_FORMAT_R32G32B32A32_FLOAT;
            case RHI::Format::R32_UINT: return DXGI_FORMAT_R32_UINT;
            case RHI::Format::R16_UINT: return DXGI_FORMAT_R16_UINT;
            default: return DXGI_FORMAT_UNKNOWN;
            }
        }

        D3D12_PRIMITIVE_TOPOLOGY_TYPE ToTopologyType(RHI::PrimitiveTopology topology)
        {
            switch (topology)
            {
            case RHI::PrimitiveTopology::PointList: return D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
            case RHI::PrimitiveTopology::LineList: return D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
            case RHI::PrimitiveTopology::TriangleList:
            case RHI::PrimitiveTopology::TriangleStrip:
                return D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            default: return D3D12_PRIMITIVE_TOPOLOGY_TYPE_UNDEFINED;
            }
        }

        D3D12_PRIMITIVE_TOPOLOGY ToPrimitiveTopology(RHI::PrimitiveTopology topology)
        {
            switch (topology)
            {
            case RHI::PrimitiveTopology::PointList: return D3D_PRIMITIVE_TOPOLOGY_POINTLIST;
            case RHI::PrimitiveTopology::LineList: return D3D_PRIMITIVE_TOPOLOGY_LINELIST;
            case RHI::PrimitiveTopology::TriangleList: return D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
            case RHI::PrimitiveTopology::TriangleStrip: return D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
            default: return D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
            }
        }

        D3D12_FILL_MODE ToFillMode(RHI::FillMode mode)
        {
            switch (mode)
            {
            case RHI::FillMode::Solid: return D3D12_FILL_MODE_SOLID;
            case RHI::FillMode::Wireframe: return D3D12_FILL_MODE_WIREFRAME;
            default: return static_cast<D3D12_FILL_MODE>(0);
            }
        }

        D3D12_CULL_MODE ToCullMode(RHI::CullMode mode)
        {
            switch (mode)
            {
            case RHI::CullMode::None: return D3D12_CULL_MODE_NONE;
            case RHI::CullMode::Front: return D3D12_CULL_MODE_FRONT;
            case RHI::CullMode::Back: return D3D12_CULL_MODE_BACK;
            default: return static_cast<D3D12_CULL_MODE>(0);
            }
        }

        D3D12_COMPARISON_FUNC ToCompareOp(RHI::CompareOp op)
        {
            switch (op)
            {
            case RHI::CompareOp::Never: return D3D12_COMPARISON_FUNC_NEVER;
            case RHI::CompareOp::Less: return D3D12_COMPARISON_FUNC_LESS;
            case RHI::CompareOp::Equal: return D3D12_COMPARISON_FUNC_EQUAL;
            case RHI::CompareOp::LessEqual: return D3D12_COMPARISON_FUNC_LESS_EQUAL;
            case RHI::CompareOp::Greater: return D3D12_COMPARISON_FUNC_GREATER;
            case RHI::CompareOp::NotEqual: return D3D12_COMPARISON_FUNC_NOT_EQUAL;
            case RHI::CompareOp::GreaterEqual: return D3D12_COMPARISON_FUNC_GREATER_EQUAL;
            case RHI::CompareOp::Always: return D3D12_COMPARISON_FUNC_ALWAYS;
            default: return static_cast<D3D12_COMPARISON_FUNC>(0);
            }
        }

        D3D12_STENCIL_OP ToStencilOp(RHI::StencilOp op)
        {
            switch (op)
            {
            case RHI::StencilOp::Keep: return D3D12_STENCIL_OP_KEEP;
            case RHI::StencilOp::Zero: return D3D12_STENCIL_OP_ZERO;
            case RHI::StencilOp::Replace: return D3D12_STENCIL_OP_REPLACE;
            case RHI::StencilOp::IncrementClamp: return D3D12_STENCIL_OP_INCR_SAT;
            case RHI::StencilOp::DecrementClamp: return D3D12_STENCIL_OP_DECR_SAT;
            case RHI::StencilOp::Invert: return D3D12_STENCIL_OP_INVERT;
            case RHI::StencilOp::IncrementWrap: return D3D12_STENCIL_OP_INCR;
            case RHI::StencilOp::DecrementWrap: return D3D12_STENCIL_OP_DECR;
            default: return static_cast<D3D12_STENCIL_OP>(0);
            }
        }

        D3D12_BLEND ToBlendFactor(RHI::BlendFactor factor)
        {
            switch (factor)
            {
            case RHI::BlendFactor::Zero: return D3D12_BLEND_ZERO;
            case RHI::BlendFactor::One: return D3D12_BLEND_ONE;
            case RHI::BlendFactor::SourceColor: return D3D12_BLEND_SRC_COLOR;
            case RHI::BlendFactor::OneMinusSourceColor: return D3D12_BLEND_INV_SRC_COLOR;
            case RHI::BlendFactor::DestinationColor: return D3D12_BLEND_DEST_COLOR;
            case RHI::BlendFactor::OneMinusDestinationColor: return D3D12_BLEND_INV_DEST_COLOR;
            case RHI::BlendFactor::SourceAlpha: return D3D12_BLEND_SRC_ALPHA;
            case RHI::BlendFactor::OneMinusSourceAlpha: return D3D12_BLEND_INV_SRC_ALPHA;
            case RHI::BlendFactor::DestinationAlpha: return D3D12_BLEND_DEST_ALPHA;
            case RHI::BlendFactor::OneMinusDestinationAlpha: return D3D12_BLEND_INV_DEST_ALPHA;
            default: return static_cast<D3D12_BLEND>(0);
            }
        }

        D3D12_BLEND_OP ToBlendOp(RHI::BlendOp op)
        {
            switch (op)
            {
            case RHI::BlendOp::Add: return D3D12_BLEND_OP_ADD;
            case RHI::BlendOp::Subtract: return D3D12_BLEND_OP_SUBTRACT;
            case RHI::BlendOp::ReverseSubtract: return D3D12_BLEND_OP_REV_SUBTRACT;
            case RHI::BlendOp::Min: return D3D12_BLEND_OP_MIN;
            case RHI::BlendOp::Max: return D3D12_BLEND_OP_MAX;
            default: return static_cast<D3D12_BLEND_OP>(0);
            }
        }

        D3D12_TEXTURE_ADDRESS_MODE ToAddressMode(RHI::SamplerAddressMode mode)
        {
            switch (mode)
            {
            case RHI::SamplerAddressMode::Repeat: return D3D12_TEXTURE_ADDRESS_MODE_WRAP;
            case RHI::SamplerAddressMode::MirroredRepeat: return D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
            case RHI::SamplerAddressMode::ClampToEdge: return D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            case RHI::SamplerAddressMode::ClampToBorder: return D3D12_TEXTURE_ADDRESS_MODE_BORDER;
            default: return static_cast<D3D12_TEXTURE_ADDRESS_MODE>(0);
            }
        }

        D3D12_STATIC_BORDER_COLOR ToBorderColor(RHI::SamplerBorderColor color)
        {
            switch (color)
            {
            case RHI::SamplerBorderColor::TransparentBlack:
                return D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
            case RHI::SamplerBorderColor::OpaqueBlack:
                return D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK;
            case RHI::SamplerBorderColor::OpaqueWhite:
                return D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
            default:
                return static_cast<D3D12_STATIC_BORDER_COLOR>(-1);
            }
        }

        bool ToSamplerFilter(const RHI::SamplerDesc& desc, D3D12_FILTER& filter)
        {
            if (desc.minFilter == RHI::SamplerFilter::Undefined ||
                desc.magFilter == RHI::SamplerFilter::Undefined ||
                desc.mipFilter == RHI::SamplerFilter::Undefined ||
                desc.maxAnisotropy == 0 ||
                desc.maxAnisotropy > D3D12_MAX_MAXANISOTROPY ||
                !std::isfinite(desc.mipLodBias) ||
                !std::isfinite(desc.minLod) ||
                !std::isfinite(desc.maxLod) ||
                desc.minLod > desc.maxLod)
            {
                return false;
            }
            if (desc.maxAnisotropy > 1)
            {
                if (desc.minFilter != RHI::SamplerFilter::Linear ||
                    desc.magFilter != RHI::SamplerFilter::Linear ||
                    desc.mipFilter != RHI::SamplerFilter::Linear)
                {
                    return false;
                }
                filter = D3D12_FILTER_ANISOTROPIC;
                return true;
            }
            const D3D12_FILTER_TYPE minFilter = desc.minFilter == RHI::SamplerFilter::Linear
                ? D3D12_FILTER_TYPE_LINEAR
                : D3D12_FILTER_TYPE_POINT;
            const D3D12_FILTER_TYPE magFilter = desc.magFilter == RHI::SamplerFilter::Linear
                ? D3D12_FILTER_TYPE_LINEAR
                : D3D12_FILTER_TYPE_POINT;
            const D3D12_FILTER_TYPE mipFilter = desc.mipFilter == RHI::SamplerFilter::Linear
                ? D3D12_FILTER_TYPE_LINEAR
                : D3D12_FILTER_TYPE_POINT;
            filter = D3D12_ENCODE_BASIC_FILTER(
                minFilter, magFilter, mipFilter, D3D12_FILTER_REDUCTION_TYPE_STANDARD);
            return true;
        }

        uint32_t RegisterNamespace(RHI::ResourceBindingType type)
        {
            switch (type)
            {
            case RHI::ResourceBindingType::ConstantBuffer: return 0;
            case RHI::ResourceBindingType::ReadOnlyStorageBuffer:
            case RHI::ResourceBindingType::SampledTexture:
                return 1;
            case RHI::ResourceBindingType::ReadWriteStorageBuffer:
            case RHI::ResourceBindingType::StorageTexture:
                return 2;
            case RHI::ResourceBindingType::StaticSampler: return 3;
            default: return std::numeric_limits<uint32_t>::max();
            }
        }

        bool RangesOverlap(uint32_t firstA, uint32_t countA, uint32_t firstB, uint32_t countB)
        {
            const uint64_t endA = static_cast<uint64_t>(firstA) + countA;
            const uint64_t endB = static_cast<uint64_t>(firstB) + countB;
            return static_cast<uint64_t>(firstA) < endB &&
                static_cast<uint64_t>(firstB) < endA;
        }

        D3D12_DESCRIPTOR_RANGE_TYPE ToDescriptorRangeType(RHI::ResourceBindingType type)
        {
            switch (type)
            {
            case RHI::ResourceBindingType::ConstantBuffer:
                return D3D12_DESCRIPTOR_RANGE_TYPE_CBV;
            case RHI::ResourceBindingType::ReadOnlyStorageBuffer:
            case RHI::ResourceBindingType::SampledTexture:
                return D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
            case RHI::ResourceBindingType::ReadWriteStorageBuffer:
            case RHI::ResourceBindingType::StorageTexture:
                return D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
            default:
                return static_cast<D3D12_DESCRIPTOR_RANGE_TYPE>(-1);
            }
        }

        bool ToDepthBias(
            float constant,
            INT& nativeBias)
        {
            if (!std::isfinite(constant)) return false;
            const double value = static_cast<double>(constant);
            if (std::trunc(value) != value ||
                value < static_cast<double>(std::numeric_limits<INT>::min()) ||
                value > static_cast<double>(std::numeric_limits<INT>::max()))
            {
                return false;
            }
            nativeBias = static_cast<INT>(constant);
            return true;
        }
    }

    RHI::BufferHandle D3D12Device::CreateBuffer(const RHI::BufferDesc& desc)
    {
        if (m_internal == nullptr || m_internal->device == nullptr || desc.size == 0)
            return nullptr;
        if ((desc.initialState == RHI::ResourceState::VertexBuffer &&
                !HasUsage(desc.usage, RHI::BufferUsage::Vertex)) ||
            (desc.initialState == RHI::ResourceState::IndexBuffer &&
                !HasUsage(desc.usage, RHI::BufferUsage::Index)) ||
            (desc.initialState == RHI::ResourceState::ConstantBuffer &&
                !HasUsage(desc.usage, RHI::BufferUsage::Constant)) ||
            ((desc.initialState == RHI::ResourceState::ShaderResource ||
                desc.initialState == RHI::ResourceState::UnorderedAccess) &&
                !HasUsage(desc.usage, RHI::BufferUsage::Storage)))
        {
            return nullptr;
        }
        switch (desc.initialState)
        {
        case RHI::ResourceState::Undefined:
        case RHI::ResourceState::Common:
        case RHI::ResourceState::CopyDestination:
        case RHI::ResourceState::VertexBuffer:
        case RHI::ResourceState::IndexBuffer:
        case RHI::ResourceState::ConstantBuffer:
        case RHI::ResourceState::ShaderResource:
        case RHI::ResourceState::UnorderedAccess:
            break;
        default:
            return nullptr;
        }

        auto buffer = std::unique_ptr<D3D12Buffer, D3D12ObjectDeleter>(
            new D3D12Buffer(m_internal->device.Get(), desc));
        if (buffer->GetNativeResource() == nullptr) return nullptr;
        D3D12Buffer* result = buffer.get();
        m_internal->liveBuffers.push_back(std::move(buffer));
        return result;
    }

    RHI::TextureHandle D3D12Device::CreateTexture(const RHI::TextureDesc& desc)
    {
        if (m_internal == nullptr || m_internal->device == nullptr ||
            desc.width == 0 || desc.height == 0 ||
            desc.depthOrArraySize == 0 || desc.mipLevels == 0 ||
            desc.depthOrArraySize > std::numeric_limits<UINT16>::max() ||
            desc.mipLevels > std::numeric_limits<UINT16>::max())
        {
            return nullptr;
        }
        const bool depthFormat = desc.format == RHI::Format::D32_FLOAT ||
            desc.format == RHI::Format::D24_UNORM_S8_UINT;
        if (depthFormat != HasUsage(
                desc.usage, RHI::TextureUsage::DepthStencil) ||
            (depthFormat &&
                (HasUsage(desc.usage, RHI::TextureUsage::RenderTarget) ||
                    HasUsage(desc.usage, RHI::TextureUsage::Storage))) ||
            (RHI::IsSrgbFormat(desc.format) &&
                HasUsage(desc.usage, RHI::TextureUsage::Storage)))
        {
            return nullptr;
        }
        auto texture = std::unique_ptr<D3D12Texture, D3D12ObjectDeleter>(
            new D3D12Texture(m_internal->device.Get(), desc));
        if (texture->GetNativeResource() == nullptr ||
            (HasUsage(desc.usage, RHI::TextureUsage::RenderTarget) &&
                texture->GetRenderTargetViewHandle(0, 0) == 0) ||
            (HasUsage(desc.usage, RHI::TextureUsage::DepthStencil) &&
                (texture->GetDepthStencilViewHandle(0, 0, false) == 0 ||
                    texture->GetDepthStencilViewHandle(0, 0, true) == 0)))
        {
            return nullptr;
        }
        D3D12Texture* result = texture.get();
        m_internal->liveTextures.push_back(std::move(texture));
        return result;
    }

    RHI::ShaderHandle D3D12Device::CreateShader(const RHI::ShaderDesc& desc)
    {
        if ((desc.stage != RHI::ShaderStage::Vertex &&
                desc.stage != RHI::ShaderStage::Fragment) ||
            desc.entryPoint == nullptr || desc.entryPoint[0] == '\0' ||
            desc.binary == nullptr || desc.binarySize == 0)
        {
            return nullptr;
        }
        auto shader = std::unique_ptr<D3D12Shader, D3D12ObjectDeleter>(
            new D3D12Shader(desc));
        D3D12Shader* result = shader.get();
        m_internal->liveShaders.push_back(std::move(shader));
        return result;
    }

    RHI::PipelineHandle D3D12Device::CreateGraphicsPipeline(
        const RHI::GraphicsPipelineDesc& desc)
    {
        if (m_internal == nullptr || m_internal->device == nullptr ||
            desc.vertexShader == nullptr ||
            desc.topology == RHI::PrimitiveTopology::Undefined ||
            desc.raster.fillMode == RHI::FillMode::Undefined ||
            desc.raster.cullMode == RHI::CullMode::Undefined ||
            desc.raster.frontFace == RHI::FrontFace::Undefined ||
            (desc.vertexBufferCount != 0 && desc.vertexBuffers == nullptr) ||
            (desc.vertexAttributeCount != 0 && desc.vertexAttributes == nullptr) ||
            (desc.colorAttachmentCount != 0 && desc.colorAttachments == nullptr) ||
            (desc.layout.bindingCount != 0 && desc.layout.bindings == nullptr) ||
            (desc.vertexAttributeCount != 0 && desc.vertexBufferCount == 0) ||
            !std::isfinite(desc.raster.depthBiasSlope) ||
            !std::isfinite(desc.raster.depthBiasClamp) ||
            desc.colorAttachmentCount > D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT)
        {
            return nullptr;
        }

        auto* vertexShader = dynamic_cast<D3D12Shader*>(desc.vertexShader);
        auto* fragmentShader = dynamic_cast<D3D12Shader*>(desc.fragmentShader);
        if (vertexShader == nullptr ||
            !OwnsObject(m_internal->liveShaders, desc.vertexShader) ||
            vertexShader->GetStage() != RHI::ShaderStage::Vertex ||
            vertexShader->GetBinarySize() == 0 ||
            (desc.fragmentShader != nullptr &&
                (!OwnsObject(
                        m_internal->liveShaders, desc.fragmentShader) ||
                    fragmentShader == nullptr ||
                    fragmentShader->GetStage() != RHI::ShaderStage::Fragment ||
                    fragmentShader->GetBinarySize() == 0)))
        {
            return nullptr;
        }

        if ((desc.depthStencil.depthTestEnabled ||
                desc.depthStencil.depthWriteEnabled ||
                desc.depthStencil.stencilEnabled) &&
            (desc.depthStencil.format != RHI::Format::D32_FLOAT &&
                desc.depthStencil.format != RHI::Format::D24_UNORM_S8_UINT))
        {
            return nullptr;
        }
        if (desc.depthStencil.depthTestEnabled &&
            desc.depthStencil.depthCompareOp == RHI::CompareOp::Undefined)
        {
            return nullptr;
        }
        if (desc.depthStencil.stencilEnabled &&
            (desc.depthStencil.format != RHI::Format::D24_UNORM_S8_UINT ||
                desc.depthStencil.front.failOp == RHI::StencilOp::Undefined ||
                desc.depthStencil.front.depthFailOp == RHI::StencilOp::Undefined ||
                desc.depthStencil.front.passOp == RHI::StencilOp::Undefined ||
                desc.depthStencil.front.compareOp == RHI::CompareOp::Undefined ||
                desc.depthStencil.back.failOp == RHI::StencilOp::Undefined ||
                desc.depthStencil.back.depthFailOp == RHI::StencilOp::Undefined ||
                desc.depthStencil.back.passOp == RHI::StencilOp::Undefined ||
                desc.depthStencil.back.compareOp == RHI::CompareOp::Undefined))
        {
            return nullptr;
        }

        for (uint32_t index = 0; index < desc.layout.bindingCount; ++index)
        {
            const RHI::ResourceBindingLayout& binding = desc.layout.bindings[index];
            const uint32_t stages = static_cast<uint32_t>(binding.stages);
            if (binding.type == RHI::ResourceBindingType::Undefined ||
                binding.count == 0 ||
                binding.stages == RHI::ShaderStageFlags::None ||
                (stages & ~static_cast<uint32_t>(
                    RHI::ShaderStageFlags::Vertex |
                    RHI::ShaderStageFlags::Fragment)) != 0 ||
                static_cast<uint64_t>(binding.binding) + binding.count >
                    static_cast<uint64_t>(std::numeric_limits<uint32_t>::max()) + 1)
            {
                return nullptr;
            }
            for (uint32_t previous = 0; previous < index; ++previous)
            {
                const RHI::ResourceBindingLayout& other =
                    desc.layout.bindings[previous];
                if ((binding.binding == other.binding) ||
                        (RegisterNamespace(binding.type) ==
                            RegisterNamespace(other.type) &&
                            RangesOverlap(
                                binding.binding,
                                binding.count,
                                other.binding,
                                other.count)))
                {
                    return nullptr;
                }
            }
            if (binding.type == RHI::ResourceBindingType::ConstantBuffer &&
                desc.layout.inlineConstantSize != 0 &&
                RangesOverlap(
                    binding.binding,
                    binding.count,
                    desc.layout.inlineConstantBinding,
                    1))
            {
                return nullptr;
            }
        }

        if (desc.layout.inlineConstantSize != 0 &&
            (desc.layout.inlineConstantStages == RHI::ShaderStageFlags::None ||
                (static_cast<uint32_t>(desc.layout.inlineConstantStages) &
                    ~static_cast<uint32_t>(
                        RHI::ShaderStageFlags::Vertex |
                        RHI::ShaderStageFlags::Fragment)) != 0 ||
                (desc.layout.inlineConstantSize % 4) != 0))
        {
            return nullptr;
        }

        uint32_t tableCount = 0;
        uint32_t descriptorCount = 0;
        uint32_t staticSamplerCount = 0;
        for (uint32_t index = 0; index < desc.layout.bindingCount; ++index)
        {
            const RHI::ResourceBindingLayout& binding = desc.layout.bindings[index];
            if (binding.type == RHI::ResourceBindingType::StaticSampler)
            {
                if (binding.count > std::numeric_limits<uint32_t>::max() -
                        staticSamplerCount)
                {
                    return nullptr;
                }
                staticSamplerCount += binding.count;
            }
            else
            {
                if (descriptorCount > std::numeric_limits<uint32_t>::max() -
                        binding.count)
                {
                    return nullptr;
                }
                descriptorCount += binding.count;
                ++tableCount;
            }
        }
        const uint32_t rootConstantDwords = desc.layout.inlineConstantSize / 4;
        if (static_cast<uint64_t>(tableCount) + rootConstantDwords >
            D3D12_MAX_ROOT_COST)
        {
            return nullptr;
        }

        std::vector<CD3DX12_DESCRIPTOR_RANGE1> descriptorRanges;
        std::vector<CD3DX12_ROOT_PARAMETER1> rootParameters;
        std::vector<D3D12PipelineBinding> pipelineBindings;
        std::vector<D3D12_STATIC_SAMPLER_DESC> staticSamplers;
        descriptorRanges.reserve(tableCount);
        rootParameters.reserve(tableCount + (rootConstantDwords != 0 ? 1u : 0u));
        pipelineBindings.reserve(tableCount);
        staticSamplers.reserve(staticSamplerCount);

        uint32_t descriptorOffset = 0;
        for (uint32_t index = 0; index < desc.layout.bindingCount; ++index)
        {
            const RHI::ResourceBindingLayout& binding = desc.layout.bindings[index];
            if (binding.type == RHI::ResourceBindingType::StaticSampler)
            {
                D3D12_FILTER filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
                if (!ToSamplerFilter(binding.staticSampler, filter) ||
                    binding.staticSampler.addressU == RHI::SamplerAddressMode::Undefined ||
                    binding.staticSampler.addressV == RHI::SamplerAddressMode::Undefined ||
                    binding.staticSampler.addressW == RHI::SamplerAddressMode::Undefined)
                {
                    return nullptr;
                }
                const bool usesBorder =
                    binding.staticSampler.addressU == RHI::SamplerAddressMode::ClampToBorder ||
                    binding.staticSampler.addressV == RHI::SamplerAddressMode::ClampToBorder ||
                    binding.staticSampler.addressW == RHI::SamplerAddressMode::ClampToBorder;
                if (usesBorder &&
                    binding.staticSampler.borderColor ==
                        RHI::SamplerBorderColor::Undefined)
                {
                    return nullptr;
                }
                const D3D12_STATIC_BORDER_COLOR borderColor =
                    binding.staticSampler.borderColor ==
                        RHI::SamplerBorderColor::Undefined
                    ? D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK
                    : ToBorderColor(binding.staticSampler.borderColor);
                if (static_cast<int>(borderColor) < 0) return nullptr;

                for (uint32_t arrayIndex = 0;
                    arrayIndex < binding.count;
                    ++arrayIndex)
                {
                    D3D12_STATIC_SAMPLER_DESC sampler = {};
                    sampler.Filter = filter;
                    sampler.AddressU = ToAddressMode(binding.staticSampler.addressU);
                    sampler.AddressV = ToAddressMode(binding.staticSampler.addressV);
                    sampler.AddressW = ToAddressMode(binding.staticSampler.addressW);
                    sampler.MaxAnisotropy = binding.staticSampler.maxAnisotropy;
                    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
                    sampler.BorderColor = borderColor;
                    sampler.MipLODBias = binding.staticSampler.mipLodBias;
                    sampler.MinLOD = binding.staticSampler.minLod;
                    sampler.MaxLOD = binding.staticSampler.maxLod;
                    sampler.ShaderRegister = binding.binding + arrayIndex;
                    sampler.RegisterSpace = 0;
                    sampler.ShaderVisibility = ToShaderVisibility(binding.stages);
                    staticSamplers.push_back(sampler);
                }
                continue;
            }

            descriptorRanges.emplace_back();
            descriptorRanges.back().Init(
                ToDescriptorRangeType(binding.type),
                binding.count,
                binding.binding,
                0,
                D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE |
                    D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE,
                0);
            rootParameters.emplace_back();
            rootParameters.back().InitAsDescriptorTable(
                1,
                &descriptorRanges.back(),
                ToShaderVisibility(binding.stages));

            D3D12PipelineBinding pipelineBinding = {};
            pipelineBinding.layout = binding;
            pipelineBinding.rootParameter =
                static_cast<uint32_t>(rootParameters.size() - 1);
            pipelineBinding.descriptorOffset = descriptorOffset;
            pipelineBindings.push_back(pipelineBinding);
            descriptorOffset += binding.count;
        }

        uint32_t inlineConstantRootParameter =
            std::numeric_limits<uint32_t>::max();
        if (rootConstantDwords != 0)
        {
            inlineConstantRootParameter =
                static_cast<uint32_t>(rootParameters.size());
            rootParameters.emplace_back();
            rootParameters.back().InitAsConstants(
                rootConstantDwords,
                desc.layout.inlineConstantBinding,
                0,
                ToShaderVisibility(desc.layout.inlineConstantStages));
        }

        D3D12_FEATURE_DATA_ROOT_SIGNATURE rootSignatureFeature = {};
        rootSignatureFeature.HighestVersion = D3D_ROOT_SIGNATURE_VERSION_1_1;
        if (FAILED(m_internal->device->CheckFeatureSupport(
                D3D12_FEATURE_ROOT_SIGNATURE,
                &rootSignatureFeature,
                sizeof(rootSignatureFeature))))
        {
            rootSignatureFeature.HighestVersion = D3D_ROOT_SIGNATURE_VERSION_1_0;
        }

        CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC rootSignatureDesc;
        rootSignatureDesc.Init_1_1(
            static_cast<UINT>(rootParameters.size()),
            rootParameters.empty() ? nullptr : rootParameters.data(),
            static_cast<UINT>(staticSamplers.size()),
            staticSamplers.empty() ? nullptr : staticSamplers.data(),
            D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);
        ComPtr<ID3DBlob> serializedRootSignature;
        ComPtr<ID3DBlob> rootSignatureError;
        if (FAILED(D3DX12SerializeVersionedRootSignature(
                &rootSignatureDesc,
                rootSignatureFeature.HighestVersion,
                &serializedRootSignature,
                &rootSignatureError)))
        {
            if (rootSignatureError != nullptr)
            {
                std::cout << "[D3D12] root signature: "
                    << static_cast<const char*>(
                        rootSignatureError->GetBufferPointer())
                    << std::endl;
            }
            return nullptr;
        }

        ComPtr<ID3D12RootSignature> rootSignature;
        if (FAILED(m_internal->device->CreateRootSignature(
                0,
                serializedRootSignature->GetBufferPointer(),
                serializedRootSignature->GetBufferSize(),
                IID_PPV_ARGS(&rootSignature))))
        {
            return nullptr;
        }

        std::vector<D3D12VertexBinding> vertexBindings;
        vertexBindings.reserve(desc.vertexBufferCount);
        for (uint32_t index = 0; index < desc.vertexBufferCount; ++index)
        {
            const RHI::VertexBufferLayout& layout = desc.vertexBuffers[index];
            if (layout.stride == 0 ||
                layout.stepMode == RHI::VertexStepMode::Undefined ||
                layout.binding >= D3D12_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT)
            {
                return nullptr;
            }
            for (const D3D12VertexBinding& previous : vertexBindings)
            {
                if (previous.binding == layout.binding) return nullptr;
            }
            vertexBindings.push_back({ layout.binding, layout.stride });
        }

        std::vector<D3D12_INPUT_ELEMENT_DESC> inputElements;
        inputElements.reserve(desc.vertexAttributeCount);
        for (uint32_t index = 0; index < desc.vertexAttributeCount; ++index)
        {
            const RHI::VertexAttribute& attribute = desc.vertexAttributes[index];
            const DXGI_FORMAT format = ToVertexFormat(attribute.format);
            const auto vertexBinding = std::find_if(
                desc.vertexBuffers,
                desc.vertexBuffers + desc.vertexBufferCount,
                [&attribute](const RHI::VertexBufferLayout& layout)
                {
                    return layout.binding == attribute.binding;
                });
            if (format == DXGI_FORMAT_UNKNOWN ||
                vertexBinding == desc.vertexBuffers + desc.vertexBufferCount)
            {
                return nullptr;
            }
            for (const D3D12_INPUT_ELEMENT_DESC& previous : inputElements)
            {
                if (previous.SemanticIndex == attribute.location) return nullptr;
            }

            D3D12_INPUT_ELEMENT_DESC element = {};
            element.SemanticName = "TEXCOORD";
            element.SemanticIndex = attribute.location;
            element.Format = format;
            element.InputSlot = attribute.binding;
            element.AlignedByteOffset = attribute.offset;
            element.InputSlotClass =
                vertexBinding->stepMode == RHI::VertexStepMode::Instance
                ? D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA
                : D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
            element.InstanceDataStepRate =
                vertexBinding->stepMode == RHI::VertexStepMode::Instance ? 1u : 0u;
            inputElements.push_back(element);
        }

        D3D12_GRAPHICS_PIPELINE_STATE_DESC pipelineDesc = {};
        pipelineDesc.pRootSignature = rootSignature.Get();
        pipelineDesc.VS = {
            vertexShader->GetBinary(),
            vertexShader->GetBinarySize()
        };
        if (fragmentShader != nullptr)
        {
            pipelineDesc.PS = {
                fragmentShader->GetBinary(),
                fragmentShader->GetBinarySize()
            };
        }
        pipelineDesc.InputLayout = {
            inputElements.empty() ? nullptr : inputElements.data(),
            static_cast<UINT>(inputElements.size())
        };
        pipelineDesc.PrimitiveTopologyType = ToTopologyType(desc.topology);
        if (pipelineDesc.PrimitiveTopologyType ==
            D3D12_PRIMITIVE_TOPOLOGY_TYPE_UNDEFINED)
        {
            return nullptr;
        }

        pipelineDesc.RasterizerState.FillMode = ToFillMode(desc.raster.fillMode);
        pipelineDesc.RasterizerState.CullMode = ToCullMode(desc.raster.cullMode);
        pipelineDesc.RasterizerState.FrontCounterClockwise =
            desc.raster.frontFace == RHI::FrontFace::CounterClockwise;
        if (!ToDepthBias(
                desc.raster.depthBiasConstant,
                pipelineDesc.RasterizerState.DepthBias))
        {
            return nullptr;
        }
        pipelineDesc.RasterizerState.DepthBiasClamp = desc.raster.depthBiasClamp;
        pipelineDesc.RasterizerState.SlopeScaledDepthBias =
            desc.raster.depthBiasSlope;
        pipelineDesc.RasterizerState.DepthClipEnable = TRUE;

        pipelineDesc.BlendState.AlphaToCoverageEnable = FALSE;
        pipelineDesc.BlendState.IndependentBlendEnable =
            desc.colorAttachmentCount > 1;
        pipelineDesc.NumRenderTargets = desc.colorAttachmentCount;
        for (uint32_t index = 0; index < desc.colorAttachmentCount; ++index)
        {
            const RHI::ColorAttachmentDesc& attachment =
                desc.colorAttachments[index];
            const DXGI_FORMAT format = static_cast<DXGI_FORMAT>(
                D3D12Texture::ToDxgiFormat(attachment.format));
            if (format == DXGI_FORMAT_UNKNOWN) return nullptr;
            pipelineDesc.RTVFormats[index] = format;

            D3D12_RENDER_TARGET_BLEND_DESC& blend =
                pipelineDesc.BlendState.RenderTarget[index];
            blend.BlendEnable = attachment.blend.enabled;
            blend.LogicOpEnable = FALSE;
            blend.LogicOp = D3D12_LOGIC_OP_NOOP;
            blend.RenderTargetWriteMask =
                static_cast<UINT8>(attachment.writeMask);
            if (attachment.blend.enabled)
            {
                if (attachment.blend.sourceColor == RHI::BlendFactor::Undefined ||
                    attachment.blend.destinationColor == RHI::BlendFactor::Undefined ||
                    attachment.blend.colorOp == RHI::BlendOp::Undefined ||
                    attachment.blend.sourceAlpha == RHI::BlendFactor::Undefined ||
                    attachment.blend.destinationAlpha == RHI::BlendFactor::Undefined ||
                    attachment.blend.alphaOp == RHI::BlendOp::Undefined)
                {
                    return nullptr;
                }
                blend.SrcBlend = ToBlendFactor(attachment.blend.sourceColor);
                blend.DestBlend =
                    ToBlendFactor(attachment.blend.destinationColor);
                blend.BlendOp = ToBlendOp(attachment.blend.colorOp);
                blend.SrcBlendAlpha =
                    ToBlendFactor(attachment.blend.sourceAlpha);
                blend.DestBlendAlpha =
                    ToBlendFactor(attachment.blend.destinationAlpha);
                blend.BlendOpAlpha = ToBlendOp(attachment.blend.alphaOp);
            }
            else
            {
                blend.SrcBlend = D3D12_BLEND_ONE;
                blend.DestBlend = D3D12_BLEND_ZERO;
                blend.BlendOp = D3D12_BLEND_OP_ADD;
                blend.SrcBlendAlpha = D3D12_BLEND_ONE;
                blend.DestBlendAlpha = D3D12_BLEND_ZERO;
                blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
            }
        }

        pipelineDesc.DepthStencilState.DepthEnable =
            desc.depthStencil.depthTestEnabled ||
            desc.depthStencil.depthWriteEnabled;
        pipelineDesc.DepthStencilState.DepthWriteMask =
            desc.depthStencil.depthWriteEnabled
            ? D3D12_DEPTH_WRITE_MASK_ALL
            : D3D12_DEPTH_WRITE_MASK_ZERO;
        pipelineDesc.DepthStencilState.DepthFunc =
            desc.depthStencil.depthTestEnabled
            ? ToCompareOp(desc.depthStencil.depthCompareOp)
            : D3D12_COMPARISON_FUNC_ALWAYS;
        pipelineDesc.DepthStencilState.StencilEnable =
            desc.depthStencil.stencilEnabled;
        pipelineDesc.DepthStencilState.StencilReadMask =
            desc.depthStencil.stencilReadMask;
        pipelineDesc.DepthStencilState.StencilWriteMask =
            desc.depthStencil.stencilWriteMask;
        if (desc.depthStencil.stencilEnabled)
        {
            pipelineDesc.DepthStencilState.FrontFace.StencilFailOp =
                ToStencilOp(desc.depthStencil.front.failOp);
            pipelineDesc.DepthStencilState.FrontFace.StencilDepthFailOp =
                ToStencilOp(desc.depthStencil.front.depthFailOp);
            pipelineDesc.DepthStencilState.FrontFace.StencilPassOp =
                ToStencilOp(desc.depthStencil.front.passOp);
            pipelineDesc.DepthStencilState.FrontFace.StencilFunc =
                ToCompareOp(desc.depthStencil.front.compareOp);
            pipelineDesc.DepthStencilState.BackFace.StencilFailOp =
                ToStencilOp(desc.depthStencil.back.failOp);
            pipelineDesc.DepthStencilState.BackFace.StencilDepthFailOp =
                ToStencilOp(desc.depthStencil.back.depthFailOp);
            pipelineDesc.DepthStencilState.BackFace.StencilPassOp =
                ToStencilOp(desc.depthStencil.back.passOp);
            pipelineDesc.DepthStencilState.BackFace.StencilFunc =
                ToCompareOp(desc.depthStencil.back.compareOp);
        }
        else
        {
            pipelineDesc.DepthStencilState.FrontFace = {
                D3D12_STENCIL_OP_KEEP,
                D3D12_STENCIL_OP_KEEP,
                D3D12_STENCIL_OP_KEEP,
                D3D12_COMPARISON_FUNC_ALWAYS
            };
            pipelineDesc.DepthStencilState.BackFace =
                pipelineDesc.DepthStencilState.FrontFace;
        }
        pipelineDesc.DSVFormat = static_cast<DXGI_FORMAT>(
            D3D12Texture::ToDxgiDepthStencilFormat(
                desc.depthStencil.format));
        if (desc.depthStencil.format != RHI::Format::Unknown &&
            pipelineDesc.DSVFormat == DXGI_FORMAT_UNKNOWN)
        {
            return nullptr;
        }

        pipelineDesc.SampleMask = UINT_MAX;
        pipelineDesc.SampleDesc.Count = 1;

        ComPtr<ID3D12PipelineState> pipelineState;
        if (FAILED(m_internal->device->CreateGraphicsPipelineState(
                &pipelineDesc, IID_PPV_ARGS(&pipelineState))))
        {
            DumpInfoQueue(m_internal, "CreateGraphicsPipeline");
            return nullptr;
        }

        auto pipeline = std::unique_ptr<D3D12PipelineState, D3D12ObjectDeleter>(
            new D3D12PipelineState(
                desc.layout,
                pipelineState.Get(),
                rootSignature.Get(),
                std::move(pipelineBindings),
                std::move(vertexBindings),
                inlineConstantRootParameter,
                descriptorCount,
                static_cast<uint32_t>(ToPrimitiveTopology(desc.topology)),
                desc.depthStencil.stencilEnabled,
                desc.depthStencil.depthWriteEnabled ||
                    desc.depthStencil.stencilEnabled));
        D3D12PipelineState* result = pipeline.get();
        m_internal->livePipelines.push_back(std::move(pipeline));
        return result;
    }

    RHI::ResourceSetHandle D3D12Device::CreateResourceSet(
        const RHI::ResourceSetDesc& desc)
    {
        if (m_internal == nullptr || m_internal->device == nullptr ||
            desc.pipeline == nullptr ||
            (desc.bindingCount != 0 && desc.bindings == nullptr))
        {
            return nullptr;
        }
        auto* pipeline = dynamic_cast<D3D12PipelineState*>(desc.pipeline);
        if (pipeline == nullptr ||
            !OwnsObject(m_internal->livePipelines, desc.pipeline) ||
            desc.bindingCount != pipeline->GetDescriptorCount())
        {
            return nullptr;
        }

        ComPtr<ID3D12DescriptorHeap> descriptorHeap;
        const uint32_t descriptorSize =
            m_internal->device->GetDescriptorHandleIncrementSize(
                D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        if (pipeline->GetDescriptorCount() != 0)
        {
            D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
            heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
            heapDesc.NumDescriptors = pipeline->GetDescriptorCount();
            heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
            if (FAILED(m_internal->device->CreateDescriptorHeap(
                    &heapDesc, IID_PPV_ARGS(&descriptorHeap))))
            {
                return nullptr;
            }
        }

        std::vector<uint8_t> written(pipeline->GetDescriptorCount(), 0);
        std::vector<ID3D12Resource*> resources;
        resources.reserve(desc.bindingCount);
        for (uint32_t index = 0; index < desc.bindingCount; ++index)
        {
            const RHI::ResourceBinding& binding = desc.bindings[index];
            const auto slot = std::find_if(
                pipeline->GetBindings().begin(),
                pipeline->GetBindings().end(),
                [&binding](const D3D12PipelineBinding& candidate)
                {
                    return candidate.layout.binding == binding.binding;
                });
            if (slot == pipeline->GetBindings().end() ||
                binding.arrayElement >= slot->layout.count)
            {
                return nullptr;
            }
            const uint32_t descriptorIndex =
                slot->descriptorOffset + binding.arrayElement;
            if (written[descriptorIndex] != 0) return nullptr;
            written[descriptorIndex] = 1;

            D3D12_CPU_DESCRIPTOR_HANDLE handle =
                descriptorHeap->GetCPUDescriptorHandleForHeapStart();
            handle.ptr += static_cast<SIZE_T>(descriptorIndex) * descriptorSize;

            if (slot->layout.type == RHI::ResourceBindingType::ConstantBuffer)
            {
                auto* buffer = dynamic_cast<D3D12Buffer*>(binding.buffer);
                if (buffer == nullptr || binding.texture != nullptr ||
                    !OwnsObject(m_internal->liveBuffers, binding.buffer) ||
                    !IsDefaultSubresourceRange(binding.subresources) ||
                    !HasUsage(buffer->GetDesc().usage, RHI::BufferUsage::Constant) ||
                    (binding.offset % D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT) != 0 ||
                    binding.offset >= buffer->GetDesc().size)
                {
                    return nullptr;
                }
                const uint32_t requestedSize = binding.size == 0
                    ? buffer->GetDesc().size - binding.offset
                    : binding.size;
                if (requestedSize == 0 ||
                    requestedSize > buffer->GetDesc().size - binding.offset)
                {
                    return nullptr;
                }
                const uint64_t alignedSize =
                    (static_cast<uint64_t>(requestedSize) +
                        D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT - 1) &
                    ~(static_cast<uint64_t>(
                        D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT) - 1);
                if (alignedSize > std::numeric_limits<UINT>::max())
                    return nullptr;

                auto* resource = static_cast<ID3D12Resource*>(
                    buffer->GetNativeResource());
                D3D12_CONSTANT_BUFFER_VIEW_DESC view = {};
                view.BufferLocation =
                    resource->GetGPUVirtualAddress() + binding.offset;
                view.SizeInBytes = static_cast<UINT>(alignedSize);
                m_internal->device->CreateConstantBufferView(&view, handle);
                resources.push_back(resource);
            }
            else if (slot->layout.type ==
                RHI::ResourceBindingType::ReadOnlyStorageBuffer)
            {
                auto* buffer = dynamic_cast<D3D12Buffer*>(binding.buffer);
                if (buffer == nullptr || binding.texture != nullptr ||
                    !OwnsObject(m_internal->liveBuffers, binding.buffer) ||
                    !IsDefaultSubresourceRange(binding.subresources) ||
                    !HasUsage(buffer->GetDesc().usage, RHI::BufferUsage::Storage) ||
                    binding.offset >= buffer->GetDesc().size)
                {
                    return nullptr;
                }
                const uint32_t requestedSize = binding.size == 0
                    ? buffer->GetDesc().size - binding.offset
                    : binding.size;
                if (requestedSize == 0 ||
                    requestedSize > buffer->GetDesc().size - binding.offset)
                {
                    return nullptr;
                }

                D3D12_SHADER_RESOURCE_VIEW_DESC view = {};
                view.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
                view.Shader4ComponentMapping =
                    D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                if (buffer->GetDesc().stride != 0)
                {
                    if ((binding.offset % buffer->GetDesc().stride) != 0 ||
                        (requestedSize % buffer->GetDesc().stride) != 0)
                    {
                        return nullptr;
                    }
                    view.Format = DXGI_FORMAT_UNKNOWN;
                    view.Buffer.FirstElement =
                        binding.offset / buffer->GetDesc().stride;
                    view.Buffer.NumElements =
                        requestedSize / buffer->GetDesc().stride;
                    view.Buffer.StructureByteStride = buffer->GetDesc().stride;
                }
                else
                {
                    if ((binding.offset % 4) != 0 ||
                        (requestedSize % 4) != 0)
                    {
                        return nullptr;
                    }
                    view.Format = DXGI_FORMAT_R32_TYPELESS;
                    view.Buffer.FirstElement = binding.offset / 4;
                    view.Buffer.NumElements = requestedSize / 4;
                    view.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
                }

                auto* resource = static_cast<ID3D12Resource*>(
                    buffer->GetNativeResource());
                m_internal->device->CreateShaderResourceView(
                    resource, &view, handle);
                resources.push_back(resource);
            }
            else if (slot->layout.type ==
                RHI::ResourceBindingType::ReadWriteStorageBuffer)
            {
                auto* buffer = dynamic_cast<D3D12Buffer*>(binding.buffer);
                if (buffer == nullptr || binding.texture != nullptr ||
                    !OwnsObject(m_internal->liveBuffers, binding.buffer) ||
                    !IsDefaultSubresourceRange(binding.subresources) ||
                    !HasUsage(buffer->GetDesc().usage, RHI::BufferUsage::Storage) ||
                    binding.offset >= buffer->GetDesc().size)
                {
                    return nullptr;
                }
                const uint32_t requestedSize = binding.size == 0
                    ? buffer->GetDesc().size - binding.offset
                    : binding.size;
                if (requestedSize == 0 ||
                    requestedSize > buffer->GetDesc().size - binding.offset)
                {
                    return nullptr;
                }

                D3D12_UNORDERED_ACCESS_VIEW_DESC view = {};
                view.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
                if (buffer->GetDesc().stride != 0)
                {
                    if ((binding.offset % buffer->GetDesc().stride) != 0 ||
                        (requestedSize % buffer->GetDesc().stride) != 0)
                    {
                        return nullptr;
                    }
                    view.Format = DXGI_FORMAT_UNKNOWN;
                    view.Buffer.FirstElement =
                        binding.offset / buffer->GetDesc().stride;
                    view.Buffer.NumElements =
                        requestedSize / buffer->GetDesc().stride;
                    view.Buffer.StructureByteStride = buffer->GetDesc().stride;
                }
                else
                {
                    if ((binding.offset % 4) != 0 ||
                        (requestedSize % 4) != 0)
                    {
                        return nullptr;
                    }
                    view.Format = DXGI_FORMAT_R32_TYPELESS;
                    view.Buffer.FirstElement = binding.offset / 4;
                    view.Buffer.NumElements = requestedSize / 4;
                    view.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
                }

                auto* resource = static_cast<ID3D12Resource*>(
                    buffer->GetNativeResource());
                m_internal->device->CreateUnorderedAccessView(
                    resource, nullptr, &view, handle);
                resources.push_back(resource);
            }
            else if (slot->layout.type ==
                RHI::ResourceBindingType::SampledTexture)
            {
                auto* texture = dynamic_cast<D3D12Texture*>(binding.texture);
                if (texture == nullptr || binding.buffer != nullptr ||
                    (!OwnsObject(
                            m_internal->liveTextures, binding.texture) &&
                        !OwnsObject(
                            m_internal->backBufferTextures,
                            binding.texture)) ||
                    binding.offset != 0 || binding.size != 0 ||
                    !HasUsage(
                        texture->GetDesc().usage,
                        RHI::TextureUsage::ShaderResource))
                {
                    return nullptr;
                }
                auto* resource = static_cast<ID3D12Resource*>(
                    texture->GetNativeResource());
                D3D12_SHADER_RESOURCE_VIEW_DESC view = {};
                view.Format = static_cast<DXGI_FORMAT>(
                    D3D12Texture::ToDxgiShaderResourceFormat(
                        texture->GetDesc().format));
                view.Shader4ComponentMapping =
                    D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                if (view.Format == DXGI_FORMAT_UNKNOWN) return nullptr;
                uint32_t mipLevelCount = 0;
                uint32_t arrayLayerCount = 0;
                if (!ResolveTextureSubresourceRange(
                        *texture,
                        binding.subresources,
                        mipLevelCount,
                        arrayLayerCount))
                {
                    return nullptr;
                }
                if (texture->GetDesc().depthOrArraySize > 1)
                {
                    view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
                    view.Texture2DArray.MostDetailedMip =
                        binding.subresources.firstMipLevel;
                    view.Texture2DArray.MipLevels = mipLevelCount;
                    view.Texture2DArray.FirstArraySlice =
                        binding.subresources.firstArrayLayer;
                    view.Texture2DArray.ArraySize = arrayLayerCount;
                }
                else
                {
                    view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                    view.Texture2D.MostDetailedMip =
                        binding.subresources.firstMipLevel;
                    view.Texture2D.MipLevels = mipLevelCount;
                }
                m_internal->device->CreateShaderResourceView(
                    resource, &view, handle);
                resources.push_back(resource);
            }
            else if (slot->layout.type ==
                RHI::ResourceBindingType::StorageTexture)
            {
                auto* texture = dynamic_cast<D3D12Texture*>(binding.texture);
                if (texture == nullptr || binding.buffer != nullptr ||
                    (!OwnsObject(
                            m_internal->liveTextures, binding.texture) &&
                        !OwnsObject(
                            m_internal->backBufferTextures,
                            binding.texture)) ||
                    binding.offset != 0 || binding.size != 0 ||
                    binding.subresources.mipLevelCount != 1 ||
                    !HasUsage(texture->GetDesc().usage, RHI::TextureUsage::Storage) ||
                    RHI::IsSrgbFormat(texture->GetDesc().format) ||
                    texture->GetDesc().format == RHI::Format::D32_FLOAT ||
                    texture->GetDesc().format == RHI::Format::D24_UNORM_S8_UINT)
                {
                    return nullptr;
                }
                uint32_t mipLevelCount = 0;
                uint32_t arrayLayerCount = 0;
                if (!ResolveTextureSubresourceRange(
                        *texture,
                        binding.subresources,
                        mipLevelCount,
                        arrayLayerCount) ||
                    mipLevelCount != 1)
                {
                    return nullptr;
                }

                auto* resource = static_cast<ID3D12Resource*>(
                    texture->GetNativeResource());
                D3D12_UNORDERED_ACCESS_VIEW_DESC view = {};
                view.Format = static_cast<DXGI_FORMAT>(
                    D3D12Texture::ToDxgiFormat(texture->GetDesc().format));
                if (view.Format == DXGI_FORMAT_UNKNOWN) return nullptr;
                if (texture->GetDesc().depthOrArraySize > 1)
                {
                    view.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
                    view.Texture2DArray.MipSlice =
                        binding.subresources.firstMipLevel;
                    view.Texture2DArray.FirstArraySlice =
                        binding.subresources.firstArrayLayer;
                    view.Texture2DArray.ArraySize = arrayLayerCount;
                }
                else
                {
                    view.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
                    view.Texture2D.MipSlice =
                        binding.subresources.firstMipLevel;
                }
                m_internal->device->CreateUnorderedAccessView(
                    resource, nullptr, &view, handle);
                resources.push_back(resource);
            }
            else
            {
                return nullptr;
            }
        }
        if (std::find(written.begin(), written.end(), 0) != written.end())
            return nullptr;

        auto resourceSet = std::unique_ptr<D3D12ResourceSet, D3D12ObjectDeleter>(
            new D3D12ResourceSet(
                desc,
                descriptorHeap.Get(),
                descriptorSize,
                resources));
        D3D12ResourceSet* result = resourceSet.get();
        m_internal->liveResourceSets.push_back(std::move(resourceSet));
        return result;
    }

    void D3D12Device::DestroyBuffer(RHI::BufferHandle buffer)
    {
        if (m_internal != nullptr && RetireObject(
                m_internal->liveBuffers,
                buffer,
                m_internal->lastSubmittedValue,
                m_internal->retiredBuffers))
        {
            m_internal->CollectCompletedWork();
        }
    }

    void D3D12Device::DestroyTexture(RHI::TextureHandle texture)
    {
        if (m_internal != nullptr && RetireObject(
                m_internal->liveTextures,
                texture,
                m_internal->lastSubmittedValue,
                m_internal->retiredTextures))
        {
            m_internal->CollectCompletedWork();
        }
    }

    void D3D12Device::DestroyShader(RHI::ShaderHandle shader)
    {
        if (m_internal != nullptr && RetireObject(
                m_internal->liveShaders,
                shader,
                m_internal->lastSubmittedValue,
                m_internal->retiredShaders))
        {
            m_internal->CollectCompletedWork();
        }
    }

    void D3D12Device::DestroyPipeline(RHI::PipelineHandle pipeline)
    {
        if (m_internal != nullptr && RetireObject(
                m_internal->livePipelines,
                pipeline,
                m_internal->lastSubmittedValue,
                m_internal->retiredPipelines))
        {
            m_internal->CollectCompletedWork();
        }
    }

    void D3D12Device::DestroyResourceSet(RHI::ResourceSetHandle resourceSet)
    {
        if (m_internal != nullptr && RetireObject(
                m_internal->liveResourceSets,
                resourceSet,
                m_internal->lastSubmittedValue,
                m_internal->retiredResourceSets))
        {
            m_internal->CollectCompletedWork();
        }
    }

    bool D3D12Device::UpdateBuffer(
        RHI::ICommandList& commandList,
        RHI::BufferHandle buffer,
        uint32_t offset,
        const void* data,
        uint32_t size)
    {
        if (m_internal == nullptr) return false;
        auto* d3dCommandList = dynamic_cast<D3D12CommandList*>(&commandList);
        auto* d3dBuffer = dynamic_cast<D3D12Buffer*>(buffer);
        const auto owned = std::find_if(
            m_internal->activeCommandLists.begin(),
            m_internal->activeCommandLists.end(),
            [d3dCommandList](
                const std::unique_ptr<D3D12CommandList, D3D12ObjectDeleter>& candidate)
            {
                return candidate.get() == d3dCommandList;
            });
        return d3dCommandList != nullptr && d3dBuffer != nullptr &&
            owned != m_internal->activeCommandLists.end() &&
            OwnsObject(m_internal->liveBuffers, buffer) &&
            d3dCommandList->RecordBufferUpload(
                d3dBuffer, offset, data, size);
    }

    bool D3D12Device::UpdateTexture(
        RHI::ICommandList& commandList,
        RHI::TextureHandle texture,
        uint32_t mipLevel,
        uint32_t arrayLayer,
        const void* data,
        uint32_t dataSize,
        uint32_t rowPitch,
        uint32_t slicePitch)
    {
        if (m_internal == nullptr) return false;
        auto* d3dCommandList = dynamic_cast<D3D12CommandList*>(&commandList);
        auto* d3dTexture = dynamic_cast<D3D12Texture*>(texture);
        const auto owned = std::find_if(
            m_internal->activeCommandLists.begin(),
            m_internal->activeCommandLists.end(),
            [d3dCommandList](
                const std::unique_ptr<D3D12CommandList, D3D12ObjectDeleter>& candidate)
            {
                return candidate.get() == d3dCommandList;
            });
        return d3dCommandList != nullptr && d3dTexture != nullptr &&
            owned != m_internal->activeCommandLists.end() &&
            (OwnsObject(m_internal->liveTextures, texture) ||
                OwnsObject(m_internal->backBufferTextures, texture)) &&
            d3dCommandList->RecordTextureUpload(
                d3dTexture,
                mipLevel,
                arrayLayer,
                data,
                dataSize,
                rowPitch,
                slicePitch);
    }

    RHI::TextureHandle D3D12Device::GetBackBuffer() {
        if (m_internal == nullptr || !m_internal->swapchainReady ||
            m_internal->backBufferTextures.empty())
        {
            return nullptr;
        }
        const uint32_t imageIndex = (m_internal->frameReady || m_internal->frameSubmitted)
            ? m_internal->activeImageIndex
            : m_internal->swapChain->GetCurrentBackBufferIndex();
        return imageIndex < m_internal->backBufferTextures.size()
            ? m_internal->backBufferTextures[imageIndex].get()
            : nullptr;
    }
}
