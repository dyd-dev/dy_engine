#include "D3D12Device.h"
#include "D3D12Buffer.h"
#include "D3D12CommandList.h"
#include "D3D12PipelineState.h"
#include "D3D12ResourceSet.h"
#include "D3D12Shader.h"
#include "D3D12Texture.h"
#include "D3D12Query.h"
#include "dyf/RHI/ResourceSet.h"
#include "dyf/RHI/Shader.h"
#include "dyf/RHI/Pipeline.h"
#include "dyf/RHI/Readback.h"
#include "d3dx12.h"

#include <d3d12.h>
#include <dxgi1_6.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <limits>
#include <memory>
#include <utility>
#include <vector>
#include <wrl.h>

using Microsoft::WRL::ComPtr;

namespace dyf::Backends
{
    struct D3D12ObjectDeleter
    {
        template<typename Object>
        void operator()(Object* object) const
        {
            delete object;
        }
    };

    struct D3D12SubmissionRecord
    {
        uint64_t completionValue = 0;
        std::vector<std::unique_ptr<D3D12CommandList, D3D12ObjectDeleter>> commandLists;
    };

    // 헤더에서 선언만 했던 구조체의 실제 정의
    struct D3D12InternalState
    {
        ComPtr<ID3D12Device> device;
        D3D12_RESOURCE_BINDING_TIER resourceBindingTier = D3D12_RESOURCE_BINDING_TIER_1;
        D3D_FEATURE_LEVEL resourceBindingFeatureLevel = D3D_FEATURE_LEVEL_11_0;
        ComPtr<ID3D12InfoQueue> infoQueue; // 디버그 빌드: D3D12 검증 메시지 수집
        ComPtr<ID3D12CommandQueue> commandQueue;
        uint64_t timestampFrequency = 0;
        HWND windowHandle = nullptr;
        ComPtr<IDXGISwapChain3> swapChain;
        ComPtr<ID3D12DescriptorHeap> rtvHeap;
        std::vector<std::unique_ptr<D3D12Texture, D3D12ObjectDeleter>> backBufferTextures;
        std::vector<uint64_t> imageCompletionValues;

        ComPtr<ID3D12Fence> fence;
        uint64_t nextCompletionValue = 1;
        HANDLE fenceEvent = nullptr;
        std::vector<uint64_t> frames;
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
            return true;
        }

        bool CollectCompletedWork()
        {
            uint64_t completedValue = 0;
            return CollectCompletedWork(completedValue);
        }
    };

    bool D3D12Device::ReadTextureNative(RHI::TextureHandle texture, RHI::TextureReadback& result)
    {
        if(!m_internal || !texture || m_internal->submissionFaulted ||
            !m_internal->activeCommandLists.empty()) return false;
        const bool backBuffer = texture == GetBackBuffer();
        if(backBuffer)
        {
            if(!m_internal->swapchainDesc.allowReadback || !m_internal->frameSubmitted) return false;
        }
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
        D3D12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(resource, before, D3D12_RESOURCE_STATE_COPY_SOURCE, 0);
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

    // 누적된 D3D12 검증 오류는 stderr에, 나머지 메시지는 stdout에 출력한다.
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
                auto& output = msg->Severity <= D3D12_MESSAGE_SEVERITY_ERROR ? std::cerr : std::cout;
                output << "[D3D12 " << where << " sev=" << static_cast<int>(msg->Severity)
                       << " id=" << static_cast<int>(msg->ID) << "] " << msg->pDescription << std::endl;
            }
        }
        internal->infoQueue->ClearStoredMessages();
        const HRESULT removedReason = internal->device->GetDeviceRemovedReason();
        if (FAILED(removedReason)) {
            std::cerr << "[D3D12] DEVICE REMOVED reason=0x" << std::hex << static_cast<unsigned>(removedReason) << std::dec << std::endl;
        }
    }

    D3D12Device::D3D12Device()
    {
        m_internal = new D3D12InternalState();
    }

    D3D12Device::~D3D12Device()
    {
        if (m_internal == nullptr) return;

        const bool queueDrained = WaitIdleNative();
        const bool deviceRemoved = m_internal->device != nullptr &&
            FAILED(m_internal->device->GetDeviceRemovedReason());
        if (!queueDrained && !deviceRemoved &&
            (m_internal->lastSubmittedValue != 0 || m_internal->submissionFaulted))
        {
            // 제출/표시 완료 신호 실패는 실제 장치 제거와 다르다. 제출 없는 Present도 포함한다.
            // 완료가 불확실한 큐가 사용하는 자원은 장치와 함께 보존한다.
            AbandonResources();
            m_internal = nullptr;
            return;
        }

        ReleaseResources();
        if (m_internal->fenceEvent != nullptr) CloseHandle(m_internal->fenceEvent);
        delete m_internal;
    }

    int D3D12Device::Initialize(const void* windowHandle, const RHI::DeviceDesc& desc)
    {
        if (desc.maxFramesInFlight == 0) return -1;
        m_internal->windowHandle = static_cast<HWND>(const_cast<void*>(windowHandle));

        if(desc.enableValidation)
        {
            ComPtr<ID3D12Debug> debugController;
            if(FAILED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController)))) return -1;
            debugController->EnableDebugLayer();
        }

        // 1. 디바이스 생성
        ComPtr<IDXGIFactory1> adapterFactory;
        ComPtr<IDXGIAdapter1> adapter;
        if(FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&adapterFactory))) ||
            FAILED(adapterFactory->EnumAdapters1(desc.adapterIndex,&adapter))) return -1;
        if (FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&m_internal->device)))) {
            return -1;
        }

        D3D12_FEATURE_DATA_D3D12_OPTIONS options = {};
        const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
        D3D12_FEATURE_DATA_FEATURE_LEVELS featureLevels = {};
        featureLevels.NumFeatureLevels = 2;
        featureLevels.pFeatureLevelsRequested = levels;
        if (FAILED(m_internal->device->CheckFeatureSupport(
                D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options))) ||
            FAILED(m_internal->device->CheckFeatureSupport(
                D3D12_FEATURE_FEATURE_LEVELS, &featureLevels, sizeof(featureLevels))))
        {
            return -1;
        }
        m_internal->resourceBindingTier = options.ResourceBindingTier;
        m_internal->resourceBindingFeatureLevel = featureLevels.MaxSupportedFeatureLevel;
        
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
        if(FAILED(m_internal->commandQueue->GetTimestampFrequency(&m_internal->timestampFrequency)))
            m_internal->timestampFrequency = 0;
        return 0;
    }


uint64_t D3D12Device::GetLastSubmissionNative() const {return m_internal->lastSubmittedValue;}
uint64_t D3D12Device::GetCompletedSubmissionNative() {uint64_t value=0; return m_internal->CollectCompletedWork(value)?value:0;}
void D3D12Device::DiscardCommandListNative(RHI::ICommandList* list) {auto& active=m_internal->activeCommandLists; active.erase(std::remove_if(active.begin(),active.end(),[list](const auto& value){return value.get()==list;}),active.end());}

RHI::TimestampQueryHandle D3D12Device::CreateTimestampQueryNative(const RHI::TimestampQueryDesc& desc)
{
    auto query = std::make_unique<D3D12TimestampQuery>(desc.count, m_internal->fence.Get());
    D3D12_QUERY_HEAP_DESC heap{};
    heap.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    heap.Count = desc.count;
    if(FAILED(m_internal->device->CreateQueryHeap(&heap, IID_PPV_ARGS(&query->heap)))) return nullptr;
    D3D12_HEAP_PROPERTIES properties{};
    properties.Type = D3D12_HEAP_TYPE_READBACK;
    properties.CreationNodeMask = properties.VisibleNodeMask = 1;
    D3D12_RESOURCE_DESC buffer{};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = uint64_t(desc.count) * sizeof(uint64_t);
    buffer.Height = 1;
    buffer.DepthOrArraySize = buffer.MipLevels = 1;
    buffer.SampleDesc.Count = 1;
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if(FAILED(m_internal->device->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE,
        &buffer, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&query->readback)))) return nullptr;
    return query.release();
}
void D3D12Device::DestroyTimestampQueryNative(RHI::TimestampQueryHandle query)
{
    delete static_cast<D3D12TimestampQuery*>(query);
}
bool D3D12Device::ReadTimestampsNative(RHI::TimestampQueryHandle handle, uint32_t first,
    uint32_t count, uint64_t* ticks)
{
    if(m_internal->submissionFaulted || IsLostNative()) return false;
    auto& query = *static_cast<D3D12TimestampQuery*>(handle);
    for(uint32_t index = first; index < first + count; ++index)
        if(!query.completions[index] || query.InFlight(index)) return false;
    const D3D12_RANGE readRange{SIZE_T(first) * sizeof(uint64_t), SIZE_T(first + count) * sizeof(uint64_t)};
    void* mapped = nullptr;
    if(FAILED(query.readback->Map(0, &readRange, &mapped))) return false;
    std::memcpy(ticks, static_cast<const uint64_t*>(mapped) + first, count * sizeof(uint64_t));
    const D3D12_RANGE noWrites{0, 0};
    query.readback->Unmap(0, &noWrites);
    return true;
}
double D3D12Device::GetTimestampPeriodNative() const
{
    return m_internal->timestampFrequency ? 1e9 / double(m_internal->timestampFrequency) : 0;
}
uint32_t D3D12Device::GetTimestampValidBitsNative() const
{
    return m_internal->timestampFrequency ? 64 : 0;
}
bool D3D12Device::SupportsNative(RHI::Feature feature) const
{
    if(feature == RHI::Feature::TimestampQuery) return m_internal->timestampFrequency != 0;
    switch(feature)
    {
    case RHI::Feature::Rasterization:
    case RHI::Feature::DescriptorIndexing:
    case RHI::Feature::SamplerLodBias:
    case RHI::Feature::Wireframe:
    case RHI::Feature::DepthBiasClamp:
        return true;
    case RHI::Feature::MeshShader:
    {
        if (!m_internal || !m_internal->device) return false;
        D3D12_FEATURE_DATA_D3D12_OPTIONS7 options7{};
        if (SUCCEEDED(m_internal->device->CheckFeatureSupport(
            D3D12_FEATURE_D3D12_OPTIONS7, &options7, sizeof(options7))))
        {
            return options7.MeshShaderTier >= D3D12_MESH_SHADER_TIER_1;
        }
        return false;
    }
    case RHI::Feature::TaskShader:
        return false;
    default:
        return false;
    }
}

uint64_t D3D12Device::GetLimitNative(RHI::Limit limit) const
{
    switch(limit)
    {
    case RHI::Limit::InlineConstantBytes:
        return D3D12_MAX_ROOT_COST * sizeof(uint32_t);
    case RHI::Limit::Texture2DDimension:
        return D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION;
    case RHI::Limit::UniformBufferOffsetAlignment:
        return D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT;
    case RHI::Limit::StorageBufferOffsetAlignment:
        // 정형 버퍼는 이 공통 정렬 외에 자신의 stride 배수여야 한다.
        return sizeof(uint32_t);
    case RHI::Limit::UniformBufferBytes:
        return D3D12_REQ_CONSTANT_BUFFER_ELEMENT_COUNT * 4u * sizeof(float);
    case RHI::Limit::StorageBufferBytes:
        return std::numeric_limits<uint32_t>::max();
    case RHI::Limit::SamplerAnisotropy:
        return D3D12_MAX_MAXANISOTROPY;
    }
    return 0;
}

bool D3D12Device::SupportsPipelineLayoutNative(const RHI::PipelineLayoutDesc& desc) const
{
    if(!m_internal || !m_internal->device) return false;
    const auto graphicsStages = static_cast<uint32_t>(RHI::ShaderStageFlags::Vertex |
        RHI::ShaderStageFlags::Fragment | RHI::ShaderStageFlags::Mesh);
    if(desc.inlineConstantSize &&
        (static_cast<uint32_t>(desc.inlineConstantStages) & ~graphicsStages)) return false;
    uint64_t rootCost = desc.inlineConstantSize / sizeof(uint32_t);
    uint64_t descriptorCount = 0;
    uint64_t samplerCount = 0;
    uint64_t constantBuffers[3] = {};
    uint64_t shaderResources[3] = {};
    uint64_t unorderedAccessViews = 0;
    for(uint32_t index = 0; index < desc.bindingCount; ++index)
    {
        const auto& binding = desc.bindings[index];
        if(static_cast<uint32_t>(binding.stages) & ~graphicsStages) return false;
        if(binding.type == RHI::ResourceBindingType::StaticSampler)
            samplerCount += binding.count;
        else
        {
            // 현재 번역은 binding마다 descriptor table 하나를 사용한다.
            ++rootCost;
            descriptorCount += binding.count;
            constexpr RHI::ShaderStageFlags stageFlags[] = {
                RHI::ShaderStageFlags::Vertex, RHI::ShaderStageFlags::Fragment, RHI::ShaderStageFlags::Mesh
            };
            for (uint32_t stage = 0; stage < 3; ++stage)
            {
                const auto flag = stageFlags[stage];
                if ((binding.stages & flag) == RHI::ShaderStageFlags::None) continue;
                if (binding.type == RHI::ResourceBindingType::ConstantBuffer)
                    constantBuffers[stage] += binding.count;
                else if (binding.type == RHI::ResourceBindingType::SampledTexture ||
                    binding.type == RHI::ResourceBindingType::ReadOnlyStorageBuffer)
                    shaderResources[stage] += binding.count;
            }
            if (binding.type == RHI::ResourceBindingType::ReadWriteStorageBuffer ||
                binding.type == RHI::ResourceBindingType::StorageTexture)
                unorderedAccessViews += binding.count;
        }
    }
    // heap의 총 크기와 실제 단계별 바인딩 한도는 다르다.
    if (m_internal->resourceBindingTier < D3D12_RESOURCE_BINDING_TIER_3)
    {
        for (uint64_t count : constantBuffers)
            if (count > D3D12_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT) return false;
        const uint64_t uavLimit = m_internal->resourceBindingTier == D3D12_RESOURCE_BINDING_TIER_1 &&
            m_internal->resourceBindingFeatureLevel < D3D_FEATURE_LEVEL_11_1
            ? D3D12_PS_CS_UAV_REGISTER_COUNT : D3D12_UAV_SLOT_COUNT;
        if (unorderedAccessViews > uavLimit) return false;
    }
    if (m_internal->resourceBindingTier == D3D12_RESOURCE_BINDING_TIER_1)
        for (uint64_t count : shaderResources)
            if (count > D3D12_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT) return false;
    return rootCost <= D3D12_MAX_ROOT_COST &&
        descriptorCount <= D3D12_MAX_SHADER_VISIBLE_DESCRIPTOR_HEAP_SIZE_TIER_1 &&
        samplerCount <= D3D12_MAX_SHADER_VISIBLE_SAMPLER_HEAP_SIZE;
}
bool D3D12Device::IsLostNative() const {return m_internal && (m_internal->submissionFaulted || (m_internal->device && FAILED(m_internal->device->GetDeviceRemovedReason())));}
bool D3D12Device::WaitIdleNative()
{
    if(!m_internal || !m_internal->commandQueue || !m_internal->fence || !m_internal->fenceEvent || m_internal->submissionFaulted) return false;
    const uint64_t completion = m_internal->nextCompletionValue++;
    if(FAILED(m_internal->commandQueue->Signal(m_internal->fence.Get(), completion))) return false;
    if(m_internal->fence->GetCompletedValue() < completion)
    {
        if(FAILED(m_internal->fence->SetEventOnCompletion(completion, m_internal->fenceEvent)) ||
            WaitForSingleObject(m_internal->fenceEvent, INFINITE) != WAIT_OBJECT_0) return false;
    }
    return m_internal->CollectCompletedWork();
}
void D3D12Device::DestroySwapchainNative()
{
    m_internal->backBufferTextures.clear();
    m_internal->rtvHeap.Reset();
    m_internal->swapChain.Reset();
    m_internal->imageCompletionValues.clear();
    m_internal->swapchainReady = m_internal->frameReady = m_internal->frameSubmitted = false;
}

    bool D3D12Device::CreateSwapchainNative(const RHI::SwapchainDesc& desc)
    {
        m_internal->windowHandle=static_cast<HWND>(const_cast<void*>(desc.window));
        if (m_internal == nullptr || m_internal->device == nullptr ||
            m_internal->commandQueue == nullptr || m_internal->windowHandle == nullptr ||
            m_internal->swapchainReady ||
            desc.minimumImageCount > DXGI_MAX_SWAP_CHAIN_BUFFERS ||
            desc.compositeAlpha != RHI::CompositeAlpha::Opaque)
        {
            return false;
        }

        DXGI_COLOR_SPACE_TYPE colorSpace;
        switch(desc.colorSpace)
        {
        case RHI::ColorSpace::Srgb:
            colorSpace = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
            break;
        case RHI::ColorSpace::LinearSrgb:
            colorSpace = DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709;
            break;
        default:
            return false;
        }

        RHI::Format actualFormat = desc.format;
        DXGI_FORMAT resourceFormat = DXGI_FORMAT_UNKNOWN;
        DXGI_FORMAT rtvFormat = DXGI_FORMAT_UNKNOWN;
        switch (desc.format)
        {
        case RHI::Format::Unknown: return false;
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
        // HWND 출력은 불투명 합성만 지원한다. 요청한 알파 방식을 몰래 대체하지 않는다.
        requestedDesc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;

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
        // 자동 exclusive fullscreen 전환은 Immediate의 ALLOW_TEARING 계약과
        // 충돌한다. 창 모드 변경은 호출자가 명시적으로 관리한다.
        if (FAILED(factory->MakeWindowAssociation(m_internal->windowHandle, DXGI_MWA_NO_ALT_ENTER)))
            return false;

        UINT colorSpaceSupport = 0;
        if(FAILED(swapchain3->CheckColorSpaceSupport(colorSpace, &colorSpaceSupport)) ||
            !(colorSpaceSupport & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT) ||
            FAILED(swapchain3->SetColorSpace1(colorSpace))) return false;

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
        if (m_internal->swapchainReady && actualDesc.BufferCount > desc.minimumImageCount)
        {
            char message[160];
            std::snprintf(message, sizeof(message),
                "D3D12: Swapchain minimum image count %u uses %u images to satisfy native requirements.",
                desc.minimumImageCount, actualDesc.BufferCount);
		ReportDiagnostic(DiagnosticSeverity::Info, message);
        }
        return m_internal->swapchainReady;
    }

    bool D3D12Device::BeginFrameNative()
    {
        if (m_internal == nullptr || !m_internal->swapchainReady ||
            m_internal->submissionFaulted ||
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
            for (uint64_t completionValue : m_internal->frames)
            {
                if (completionValue > completedValue) return false;
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
            m_internal->frames[m_internal->nextFrameIndex] > completedValue ||
            m_internal->imageCompletionValues[imageIndex] > completedValue)
        {
            return false;
        }

        m_internal->activeFrameIndex = m_internal->nextFrameIndex;
        m_internal->activeImageIndex = imageIndex;
        m_internal->frameReady = true;
        return true;
    }

    RHI::ICommandList* D3D12Device::AcquireCommandListNative()
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

    bool D3D12Device::SubmitNative(RHI::ICommandList** cmdLists, uint32_t count)
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
            for(const auto& commandList : submission.commandLists)
                commandList->MarkTimestampsSubmitted(UINT64_MAX);
            m_internal->lastSubmittedValue = submission.completionValue;
            m_internal->submissionFaulted = true;
            if (frameSubmission) m_internal->frameReady = false;
            return false;
        }

        for (const std::unique_ptr<D3D12CommandList, D3D12ObjectDeleter>& commandList :
            submission.commandLists)
        {
            commandList->MarkTimestampsSubmitted(submission.completionValue);
            commandList->CommitResourceStates();
        }
        m_internal->lastSubmittedValue = submission.completionValue;

        // 프레임을 구성하는 여러 제출의 마지막 완료 값을 유지한다. 마감은 Present가 한다.
        if (m_internal->frameReady)
        {
            m_internal->frames[m_internal->activeFrameIndex] =
                submission.completionValue;
        }
        if (frameSubmission)
        {
            m_internal->imageCompletionValues[m_internal->activeImageIndex] =
                submission.completionValue;
            m_internal->frameSubmitted = true;
        }
        DumpInfoQueue(m_internal, "Submit");
        return true;
    }

    bool D3D12Device::PresentNative()
    {
        if (m_internal == nullptr || m_internal->submissionFaulted ||
            !m_internal->frameReady)
        {
            return false;
        }

        const HRESULT result = m_internal->swapChain->Present(
            m_internal->presentSyncInterval,
            m_internal->presentFlags);
        if (SUCCEEDED(result))
        {
            // Present도 queue에 backbuffer를 사용하는 작업을 추가한다. draw 제출의
            // fence만 기다리면 resize가 표시 중인 버퍼를 먼저 해제할 수 있다.
            const uint64_t completion = m_internal->nextCompletionValue++;
            if (FAILED(m_internal->commandQueue->Signal(m_internal->fence.Get(), completion)))
            {
                m_internal->submissionFaulted = true;
                m_internal->frameReady = false;
                m_internal->frameSubmitted = false;
                ReportDiagnostic(DiagnosticSeverity::Error,
                    "D3D12: Failed to signal presentation completion; swapchain images cannot be safely reused or resized.");
                return false;
            }
            m_internal->frames[m_internal->activeFrameIndex] = completion;
            m_internal->imageCompletionValues[m_internal->activeImageIndex] = completion;
            m_internal->lastSubmittedValue = completion;
        }
        // 제출이 없는 프레임도 Present에서 정상적으로 마감한다.
        m_internal->nextFrameIndex =
            (m_internal->activeFrameIndex + 1) %
            static_cast<uint32_t>(m_internal->frames.size());
        m_internal->frameReady = false;
        m_internal->frameSubmitted = false;
        if (FAILED(result)) m_internal->submissionFaulted = true;
        DumpInfoQueue(m_internal, "Present");
        return SUCCEEDED(result);
    }

    namespace
    {
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

        bool HasStage(RHI::ShaderStageFlags stages, RHI::ShaderStageFlags stage)
        {
            return (static_cast<uint32_t>(stages) & static_cast<uint32_t>(stage)) != 0;
        }

        D3D12_SHADER_VISIBILITY ToShaderVisibility(RHI::ShaderStageFlags stages)
        {
            const bool vertex = HasStage(stages, RHI::ShaderStageFlags::Vertex);
            const bool fragment = HasStage(stages, RHI::ShaderStageFlags::Fragment);
            const bool mesh = HasStage(stages, RHI::ShaderStageFlags::Mesh);
            const uint32_t count = static_cast<uint32_t>(vertex) + static_cast<uint32_t>(fragment) + static_cast<uint32_t>(mesh);
            if (count == 1 && vertex) return D3D12_SHADER_VISIBILITY_VERTEX;
            if (count == 1 && fragment) return D3D12_SHADER_VISIBILITY_PIXEL;
            if (count == 1 && mesh) return D3D12_SHADER_VISIBILITY_MESH;
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

        D3D12_BLEND ToAlphaBlendFactor(RHI::BlendFactor factor)
        {
            // 색상 계수의 alpha 성분은 해당 alpha 계수와 같다. D3D12의
            // alpha 항에는 *_COLOR enum을 직접 전달할 수 없다.
            switch (factor)
            {
            case RHI::BlendFactor::SourceColor: return D3D12_BLEND_SRC_ALPHA;
            case RHI::BlendFactor::OneMinusSourceColor: return D3D12_BLEND_INV_SRC_ALPHA;
            case RHI::BlendFactor::DestinationColor: return D3D12_BLEND_DEST_ALPHA;
            case RHI::BlendFactor::OneMinusDestinationColor: return D3D12_BLEND_INV_DEST_ALPHA;
            default: return ToBlendFactor(factor);
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
            if (desc.maxAnisotropy > D3D12_MAX_MAXANISOTROPY ||
                desc.mipLodBias < D3D12_MIP_LOD_BIAS_MIN ||
                desc.mipLodBias > D3D12_MIP_LOD_BIAS_MAX)
            {
                return false;
            }
            if (desc.maxAnisotropy > 1)
            {
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

    bool D3D12Device::SupportsSamplerNative(const RHI::SamplerDesc& desc) const
    {
        // 지원 조회와 실제 sampler 변환에 같은 범위 검사를 적용한다. 범위 밖 값은 보정하지 않는다.
        D3D12_FILTER filter;
        return ToSamplerFilter(desc, filter);
    }

    bool D3D12Device::SupportsGraphicsPipelineNative(const RHI::GraphicsPipelineDesc& desc) const
    {
        if(!m_internal || !m_internal->device ||
            desc.colorAttachmentCount > D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT ||
            desc.vertexAttributeCount > D3D12_IA_VERTEX_INPUT_STRUCTURE_ELEMENT_COUNT) return false;
        INT depthBias = 0;
        if(!ToDepthBias(desc.raster.depthBiasConstant, depthBias)) return false;
        const auto supportsFormat = [&](DXGI_FORMAT format, D3D12_FORMAT_SUPPORT1 required) {
            D3D12_FEATURE_DATA_FORMAT_SUPPORT support = {};
            support.Format = format;
            return format != DXGI_FORMAT_UNKNOWN &&
                SUCCEEDED(m_internal->device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT,
                    &support, sizeof(support))) && (support.Support1 & required) == required;
        };
        for(uint32_t index = 0; index < desc.vertexBufferCount; ++index)
            if(desc.vertexBuffers[index].binding >= D3D12_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT ||
                desc.vertexBuffers[index].stride > D3D12_REQ_MULTI_ELEMENT_STRUCTURE_SIZE_IN_BYTES) return false;
        for(uint32_t index = 0; index < desc.vertexAttributeCount; ++index)
            if(!supportsFormat(ToVertexFormat(desc.vertexAttributes[index].format),
                D3D12_FORMAT_SUPPORT1_IA_VERTEX_BUFFER)) return false;
        for(uint32_t index = 0; index < desc.colorAttachmentCount; ++index)
        {
            const auto& attachment = desc.colorAttachments[index];
            auto required = D3D12_FORMAT_SUPPORT1_RENDER_TARGET;
            if(attachment.blend.enabled)
                required = static_cast<D3D12_FORMAT_SUPPORT1>(required | D3D12_FORMAT_SUPPORT1_BLENDABLE);
            if(!supportsFormat(static_cast<DXGI_FORMAT>(D3D12Texture::ToDxgiFormat(attachment.format)), required)) return false;
        }
        if(desc.depthStencil.format != RHI::Format::Unknown &&
            !supportsFormat(static_cast<DXGI_FORMAT>(D3D12Texture::ToDxgiFormat(desc.depthStencil.format)),
                D3D12_FORMAT_SUPPORT1_DEPTH_STENCIL)) return false;
        return true;
    }

    RHI::BufferHandle D3D12Device::CreateBufferNative(const RHI::BufferDesc& desc)
    {
        if (m_internal == nullptr || m_internal->device == nullptr)
            return nullptr;

        auto buffer = std::unique_ptr<D3D12Buffer, D3D12ObjectDeleter>(
            new D3D12Buffer(m_internal->device.Get(), desc));
        if (buffer->GetNativeResource() == nullptr) return nullptr;
        D3D12Buffer* result = buffer.get();
        m_internal->liveBuffers.push_back(std::move(buffer));
        return result;
    }

    RHI::TextureHandle D3D12Device::CreateTextureNative(const RHI::TextureDesc& desc)
    {
        if (m_internal == nullptr || m_internal->device == nullptr ||
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

    RHI::ShaderHandle D3D12Device::CreateShaderNative(const RHI::ShaderDesc& desc)
    {
        if (desc.stage != RHI::ShaderStage::Vertex &&
            desc.stage != RHI::ShaderStage::Fragment &&
            desc.stage != RHI::ShaderStage::Mesh)
        {
            return nullptr;
        }
        auto shader = std::unique_ptr<D3D12Shader, D3D12ObjectDeleter>(
            new D3D12Shader(desc));
        D3D12Shader* result = shader.get();
        m_internal->liveShaders.push_back(std::move(shader));
        return result;
    }

    RHI::PipelineHandle D3D12Device::CreateGraphicsPipelineNative(
        const RHI::GraphicsPipelineDesc& desc)
    {
        if (m_internal == nullptr || m_internal->device == nullptr ||
            !SupportsPipelineLayoutNative(desc.layout) ||
            !SupportsGraphicsPipelineNative(desc))
        {
            return nullptr;
        }

        auto* vertexShader = dynamic_cast<D3D12Shader*>(desc.vertexShader);
        auto* fragmentShader = dynamic_cast<D3D12Shader*>(desc.fragmentShader);
        if (vertexShader == nullptr ||
            vertexShader->GetBinarySize() == 0 ||
            (desc.fragmentShader != nullptr &&
                (fragmentShader == nullptr ||
                    fragmentShader->GetBinarySize() == 0)))
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
                if (!ToSamplerFilter(binding.staticSampler, filter))
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
                std::cerr << "[D3D12] root signature: "
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
            if (layout.binding >= D3D12_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT)
            {
                return nullptr;
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
            if (format == DXGI_FORMAT_UNKNOWN)
            {
                return nullptr;
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
        bool alphaFactorsTranslated = false;
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
                blend.SrcBlend = ToBlendFactor(attachment.blend.sourceColor);
                blend.DestBlend =
                    ToBlendFactor(attachment.blend.destinationColor);
                blend.BlendOp = ToBlendOp(attachment.blend.colorOp);
                blend.SrcBlendAlpha =
                    ToAlphaBlendFactor(attachment.blend.sourceAlpha);
                blend.DestBlendAlpha =
                    ToAlphaBlendFactor(attachment.blend.destinationAlpha);
                alphaFactorsTranslated = alphaFactorsTranslated ||
                    blend.SrcBlendAlpha != ToBlendFactor(attachment.blend.sourceAlpha) ||
                    blend.DestBlendAlpha != ToBlendFactor(attachment.blend.destinationAlpha);
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
            D3D12Texture::ToDxgiFormat(
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
        if (alphaFactorsTranslated)
		ReportDiagnostic(DiagnosticSeverity::Info,
                "D3D12: Alpha blend color factors were translated to equivalent alpha factors.");
        return result;
    }

    bool D3D12Device::SupportsMeshPipelineNative(const RHI::MeshPipelineDesc& desc) const
    {
        if (m_internal == nullptr || m_internal->device == nullptr ||
            !Supports(RHI::Feature::MeshShader))
        {
            return false;
        }
        if (!SupportsPipelineLayoutNative(desc.layout)) return false;
        for (uint32_t i = 0; i < desc.colorAttachmentCount; ++i)
        {
            if (D3D12Texture::ToDxgiFormat(desc.colorAttachments[i].format) == DXGI_FORMAT_UNKNOWN)
                return false;
        }
        if (desc.depthStencil.format != RHI::Format::Unknown &&
            D3D12Texture::ToDxgiFormat(desc.depthStencil.format) == DXGI_FORMAT_UNKNOWN)
            return false;
        return true;
    }

    RHI::PipelineHandle D3D12Device::CreateMeshPipelineNative(
        const RHI::MeshPipelineDesc& desc)
    {
        if (m_internal == nullptr || m_internal->device == nullptr ||
            !Supports(RHI::Feature::MeshShader) ||
            !SupportsPipelineLayoutNative(desc.layout) ||
            !SupportsMeshPipelineNative(desc))
        {
            return nullptr;
        }

        auto* meshShader = dynamic_cast<D3D12Shader*>(desc.meshShader);
        auto* fragmentShader = dynamic_cast<D3D12Shader*>(desc.fragmentShader);
        if (meshShader == nullptr ||
            meshShader->GetBinarySize() == 0 ||
            (desc.fragmentShader != nullptr &&
                (fragmentShader == nullptr ||
                    fragmentShader->GetBinarySize() == 0)))
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
                if (binding.count > std::numeric_limits<uint32_t>::max() - staticSamplerCount)
                    return nullptr;
                staticSamplerCount += binding.count;
            }
            else
            {
                if (descriptorCount > std::numeric_limits<uint32_t>::max() - binding.count)
                    return nullptr;
                descriptorCount += binding.count;
                ++tableCount;
            }
        }
        const uint32_t rootConstantDwords = desc.layout.inlineConstantSize / 4;
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
                if (!ToSamplerFilter(binding.staticSampler, filter)) return nullptr;
                const D3D12_STATIC_BORDER_COLOR borderColor =
                    binding.staticSampler.borderColor == RHI::SamplerBorderColor::Undefined
                    ? D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK
                    : ToBorderColor(binding.staticSampler.borderColor);
                if (static_cast<int>(borderColor) < 0) return nullptr;

                for (uint32_t arrayIndex = 0; arrayIndex < binding.count; ++arrayIndex)
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
            pipelineBinding.rootParameter = static_cast<uint32_t>(rootParameters.size() - 1);
            pipelineBinding.descriptorOffset = descriptorOffset;
            pipelineBindings.push_back(pipelineBinding);
            descriptorOffset += binding.count;
        }

        uint32_t inlineConstantRootParameter = std::numeric_limits<uint32_t>::max();
        if (rootConstantDwords != 0)
        {
            inlineConstantRootParameter = static_cast<uint32_t>(rootParameters.size());
            rootParameters.emplace_back();
            rootParameters.back().InitAsConstants(
                rootConstantDwords,
                desc.layout.inlineConstantBinding,
                0,
                ToShaderVisibility(desc.layout.inlineConstantStages));
        }

        CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC rootSignatureDesc = {};
        rootSignatureDesc.Init_1_1(
            static_cast<UINT>(rootParameters.size()),
            rootParameters.empty() ? nullptr : rootParameters.data(),
            static_cast<UINT>(staticSamplers.size()),
            staticSamplers.empty() ? nullptr : staticSamplers.data(),
            D3D12_ROOT_SIGNATURE_FLAG_NONE);

        ComPtr<ID3DBlob> serializedRootSignature;
        ComPtr<ID3DBlob> rootSignatureErrors;
        if (FAILED(D3D12SerializeVersionedRootSignature(
                &rootSignatureDesc,
                &serializedRootSignature,
                &rootSignatureErrors)))
        {
            if (rootSignatureErrors != nullptr &&
                rootSignatureErrors->GetBufferPointer() != nullptr)
            {
                DumpInfoQueue(m_internal, "SerializeRootSignature");
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

#ifndef D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS
#define D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS static_cast<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE>(25)
#endif

        struct alignas(void*) MeshPipelineStateStream
        {
            CD3DX12_PIPELINE_STATE_STREAM_ROOT_SIGNATURE RootSignature;
            CD3DX12_PIPELINE_STATE_STREAM_SUBOBJECT<D3D12_SHADER_BYTECODE, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS> MS;
            CD3DX12_PIPELINE_STATE_STREAM_PS PS;
            CD3DX12_PIPELINE_STATE_STREAM_RASTERIZER RasterizerState;
            CD3DX12_PIPELINE_STATE_STREAM_BLEND_DESC BlendState;
            CD3DX12_PIPELINE_STATE_STREAM_DEPTH_STENCIL DepthStencilState;
            CD3DX12_PIPELINE_STATE_STREAM_DEPTH_STENCIL_FORMAT DSVFormat;
            CD3DX12_PIPELINE_STATE_STREAM_RENDER_TARGET_FORMATS RTVFormats;
            CD3DX12_PIPELINE_STATE_STREAM_SAMPLE_MASK SampleMask;
            CD3DX12_PIPELINE_STATE_STREAM_SAMPLE_DESC SampleDesc;
        } stream{};

        stream.RootSignature = rootSignature.Get();
        stream.MS = { meshShader->GetBinary(), meshShader->GetBinarySize() };
        if (fragmentShader != nullptr)
        {
            stream.PS = { fragmentShader->GetBinary(), fragmentShader->GetBinarySize() };
        }

        D3D12_RASTERIZER_DESC rasterDesc = {};
        rasterDesc.FillMode = ToFillMode(desc.raster.fillMode);
        rasterDesc.CullMode = ToCullMode(desc.raster.cullMode);
        rasterDesc.FrontCounterClockwise = desc.raster.frontFace == RHI::FrontFace::CounterClockwise;
        if (!ToDepthBias(desc.raster.depthBiasConstant, rasterDesc.DepthBias)) return nullptr;
        rasterDesc.DepthBiasClamp = desc.raster.depthBiasClamp;
        rasterDesc.SlopeScaledDepthBias = desc.raster.depthBiasSlope;
        rasterDesc.DepthClipEnable = TRUE;
        stream.RasterizerState = CD3DX12_RASTERIZER_DESC(rasterDesc);

        D3D12_BLEND_DESC blendDesc = {};
        blendDesc.AlphaToCoverageEnable = FALSE;
        blendDesc.IndependentBlendEnable = desc.colorAttachmentCount > 1;
        D3D12_RT_FORMAT_ARRAY rtvFormats = {};
        rtvFormats.NumRenderTargets = desc.colorAttachmentCount;
        bool alphaFactorsTranslated = false;
        for (uint32_t index = 0; index < desc.colorAttachmentCount; ++index)
        {
            const RHI::ColorAttachmentDesc& attachment = desc.colorAttachments[index];
            const DXGI_FORMAT format = static_cast<DXGI_FORMAT>(D3D12Texture::ToDxgiFormat(attachment.format));
            if (format == DXGI_FORMAT_UNKNOWN) return nullptr;
            rtvFormats.RTFormats[index] = format;

            D3D12_RENDER_TARGET_BLEND_DESC& blend = blendDesc.RenderTarget[index];
            blend.BlendEnable = attachment.blend.enabled;
            blend.LogicOpEnable = FALSE;
            blend.LogicOp = D3D12_LOGIC_OP_NOOP;
            blend.RenderTargetWriteMask = static_cast<UINT8>(attachment.writeMask);
            if (attachment.blend.enabled)
            {
                blend.SrcBlend = ToBlendFactor(attachment.blend.sourceColor);
                blend.DestBlend = ToBlendFactor(attachment.blend.destinationColor);
                blend.BlendOp = ToBlendOp(attachment.blend.colorOp);
                blend.SrcBlendAlpha = ToAlphaBlendFactor(attachment.blend.sourceAlpha);
                blend.DestBlendAlpha = ToAlphaBlendFactor(attachment.blend.destinationAlpha);
                if (blend.SrcBlendAlpha != ToBlendFactor(attachment.blend.sourceAlpha) ||
                    blend.DestBlendAlpha != ToBlendFactor(attachment.blend.destinationAlpha))
                {
                    alphaFactorsTranslated = true;
                }
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
        stream.BlendState = CD3DX12_BLEND_DESC(blendDesc);
        stream.RTVFormats = rtvFormats;

        D3D12_DEPTH_STENCIL_DESC depthStencilDesc = {};
        depthStencilDesc.DepthEnable = desc.depthStencil.depthTestEnabled || desc.depthStencil.depthWriteEnabled;
        depthStencilDesc.DepthWriteMask = desc.depthStencil.depthWriteEnabled ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
        depthStencilDesc.DepthFunc = desc.depthStencil.depthTestEnabled ? ToCompareOp(desc.depthStencil.depthCompareOp) : D3D12_COMPARISON_FUNC_ALWAYS;
        depthStencilDesc.StencilEnable = desc.depthStencil.stencilEnabled;
        depthStencilDesc.StencilReadMask = desc.depthStencil.stencilReadMask;
        depthStencilDesc.StencilWriteMask = desc.depthStencil.stencilWriteMask;
        if (desc.depthStencil.stencilEnabled)
        {
            depthStencilDesc.FrontFace.StencilFailOp = ToStencilOp(desc.depthStencil.front.failOp);
            depthStencilDesc.FrontFace.StencilDepthFailOp = ToStencilOp(desc.depthStencil.front.depthFailOp);
            depthStencilDesc.FrontFace.StencilPassOp = ToStencilOp(desc.depthStencil.front.passOp);
            depthStencilDesc.FrontFace.StencilFunc = ToCompareOp(desc.depthStencil.front.compareOp);
            depthStencilDesc.BackFace.StencilFailOp = ToStencilOp(desc.depthStencil.back.failOp);
            depthStencilDesc.BackFace.StencilDepthFailOp = ToStencilOp(desc.depthStencil.back.depthFailOp);
            depthStencilDesc.BackFace.StencilPassOp = ToStencilOp(desc.depthStencil.back.passOp);
            depthStencilDesc.BackFace.StencilFunc = ToCompareOp(desc.depthStencil.back.compareOp);
        }
        else
        {
            depthStencilDesc.FrontFace = { D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_COMPARISON_FUNC_ALWAYS };
            depthStencilDesc.BackFace = depthStencilDesc.FrontFace;
        }
        stream.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(depthStencilDesc);

        const DXGI_FORMAT dsvFormat = static_cast<DXGI_FORMAT>(D3D12Texture::ToDxgiFormat(desc.depthStencil.format));
        if (desc.depthStencil.format != RHI::Format::Unknown && dsvFormat == DXGI_FORMAT_UNKNOWN) return nullptr;
        stream.DSVFormat = dsvFormat;

        stream.SampleMask = UINT_MAX;
        DXGI_SAMPLE_DESC sampleDesc = { 1, 0 };
        stream.SampleDesc = sampleDesc;

        ComPtr<ID3D12Device2> device2;
        if (FAILED(m_internal->device.As(&device2)) || device2 == nullptr)
        {
            return nullptr;
        }

        D3D12_PIPELINE_STATE_STREAM_DESC streamDesc = {
            sizeof(stream),
            &stream
        };

        ComPtr<ID3D12PipelineState> pipelineState;
        if (FAILED(device2->CreatePipelineState(&streamDesc, IID_PPV_ARGS(&pipelineState))))
        {
            DumpInfoQueue(m_internal, "CreateMeshPipeline");
            return nullptr;
        }

        auto pipeline = std::unique_ptr<D3D12PipelineState, D3D12ObjectDeleter>(
            new D3D12PipelineState(
                desc.layout,
                pipelineState.Get(),
                rootSignature.Get(),
                std::move(pipelineBindings),
                {},
                inlineConstantRootParameter,
                descriptorCount,
                D3D_PRIMITIVE_TOPOLOGY_UNDEFINED,
                desc.depthStencil.stencilEnabled,
                desc.depthStencil.depthWriteEnabled || desc.depthStencil.stencilEnabled,
                true));
        D3D12PipelineState* result = pipeline.get();
        m_internal->livePipelines.push_back(std::move(pipeline));
        if (alphaFactorsTranslated)
            ReportDiagnostic(DiagnosticSeverity::Info,
                "D3D12: Alpha blend color factors were translated to equivalent alpha factors.");
        return result;
    }

    RHI::ResourceSetHandle D3D12Device::CreateResourceSetNative(
        const RHI::ResourceSetDesc& desc)
    {
        if (m_internal == nullptr || m_internal->device == nullptr)
        {
            return nullptr;
        }
        auto* pipeline = dynamic_cast<D3D12PipelineState*>(desc.pipeline);
        if (pipeline == nullptr ||
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
            if (slot == pipeline->GetBindings().end())
            {
                return nullptr;
            }
            const uint32_t descriptorIndex =
                slot->descriptorOffset + binding.arrayElement;

            D3D12_CPU_DESCRIPTOR_HANDLE handle =
                descriptorHeap->GetCPUDescriptorHandleForHeapStart();
            handle.ptr += static_cast<SIZE_T>(descriptorIndex) * descriptorSize;

            if (slot->layout.type == RHI::ResourceBindingType::ConstantBuffer)
            {
                auto* buffer = dynamic_cast<D3D12Buffer*>(binding.buffer);
                if (buffer == nullptr ||
                    !IsDefaultSubresourceRange(binding.subresources) ||
                    (binding.offset % D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT) != 0)
                {
                    return nullptr;
                }
                const uint64_t alignedSize =
                    (static_cast<uint64_t>(binding.size) +
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
                if (buffer == nullptr || !IsDefaultSubresourceRange(binding.subresources))
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
                        (binding.size % buffer->GetDesc().stride) != 0)
                    {
                        return nullptr;
                    }
                    view.Format = DXGI_FORMAT_UNKNOWN;
                    view.Buffer.FirstElement =
                        binding.offset / buffer->GetDesc().stride;
                    view.Buffer.NumElements =
                        binding.size / buffer->GetDesc().stride;
                    view.Buffer.StructureByteStride = buffer->GetDesc().stride;
                }
                else
                {
                    if ((binding.offset % 4) != 0 ||
                        (binding.size % 4) != 0)
                    {
                        return nullptr;
                    }
                    view.Format = DXGI_FORMAT_R32_TYPELESS;
                    view.Buffer.FirstElement = binding.offset / 4;
                    view.Buffer.NumElements = binding.size / 4;
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
                if (buffer == nullptr || !IsDefaultSubresourceRange(binding.subresources))
                {
                    return nullptr;
                }
                D3D12_UNORDERED_ACCESS_VIEW_DESC view = {};
                view.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
                if (buffer->GetDesc().stride != 0)
                {
                    if ((binding.offset % buffer->GetDesc().stride) != 0 ||
                        (binding.size % buffer->GetDesc().stride) != 0)
                    {
                        return nullptr;
                    }
                    view.Format = DXGI_FORMAT_UNKNOWN;
                    view.Buffer.FirstElement =
                        binding.offset / buffer->GetDesc().stride;
                    view.Buffer.NumElements =
                        binding.size / buffer->GetDesc().stride;
                    view.Buffer.StructureByteStride = buffer->GetDesc().stride;
                }
                else
                {
                    if ((binding.offset % 4) != 0 ||
                        (binding.size % 4) != 0)
                    {
                        return nullptr;
                    }
                    view.Format = DXGI_FORMAT_R32_TYPELESS;
                    view.Buffer.FirstElement = binding.offset / 4;
                    view.Buffer.NumElements = binding.size / 4;
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
                if (texture == nullptr || binding.offset != 0 || binding.size != 0)
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
                const uint32_t mipLevelCount = binding.subresources.mipLevelCount == 0
                    ? texture->GetDesc().mipLevels - binding.subresources.firstMipLevel
                    : binding.subresources.mipLevelCount;
                const uint32_t arrayLayerCount = binding.subresources.arrayLayerCount == 0
                    ? texture->GetDesc().depthOrArraySize - binding.subresources.firstArrayLayer
                    : binding.subresources.arrayLayerCount;
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
                if (texture == nullptr ||
                    binding.offset != 0 || binding.size != 0 ||
                    RHI::IsSrgbFormat(texture->GetDesc().format) ||
                    texture->GetDesc().format == RHI::Format::D32_FLOAT ||
                    texture->GetDesc().format == RHI::Format::D24_UNORM_S8_UINT)
                {
                    return nullptr;
                }
                const uint32_t arrayLayerCount = binding.subresources.arrayLayerCount == 0
                    ? texture->GetDesc().depthOrArraySize - binding.subresources.firstArrayLayer
                    : binding.subresources.arrayLayerCount;

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

    void D3D12Device::DestroyBufferNative(RHI::BufferHandle buffer)
    {
        if(!m_internal) return;
        auto& objects = m_internal->liveBuffers;
        objects.erase(std::remove_if(objects.begin(), objects.end(),
            [buffer](const auto& object) { return object.get() == buffer; }), objects.end());
    }

    void D3D12Device::DestroyTextureNative(RHI::TextureHandle texture)
    {
        if(!m_internal) return;
        auto& objects = m_internal->liveTextures;
        objects.erase(std::remove_if(objects.begin(), objects.end(),
            [texture](const auto& object) { return object.get() == texture; }), objects.end());
    }

    void D3D12Device::DestroyShaderNative(RHI::ShaderHandle shader)
    {
        if(!m_internal) return;
        auto& objects = m_internal->liveShaders;
        objects.erase(std::remove_if(objects.begin(), objects.end(),
            [shader](const auto& object) { return object.get() == shader; }), objects.end());
    }

    void D3D12Device::DestroyPipelineNative(RHI::PipelineHandle pipeline)
    {
        if(!m_internal) return;
        auto& objects = m_internal->livePipelines;
        objects.erase(std::remove_if(objects.begin(), objects.end(),
            [pipeline](const auto& object) { return object.get() == pipeline; }), objects.end());
    }

    void D3D12Device::DestroyResourceSetNative(RHI::ResourceSetHandle resourceSet)
    {
        if(!m_internal) return;
        auto& objects = m_internal->liveResourceSets;
        objects.erase(std::remove_if(objects.begin(), objects.end(),
            [resourceSet](const auto& object) { return object.get() == resourceSet; }), objects.end());
    }

    bool D3D12Device::UpdateBufferNative(
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
            d3dCommandList->RecordBufferUpload(
                d3dBuffer, offset, data, size);
    }

    bool D3D12Device::UpdateTextureNative(
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
            d3dCommandList->RecordTextureUpload(
                d3dTexture,
                mipLevel,
                arrayLayer,
                data,
                dataSize,
                rowPitch,
                slicePitch);
    }

    RHI::TextureHandle D3D12Device::GetBackBufferNative() {
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
