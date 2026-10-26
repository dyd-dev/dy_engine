#include "D3D12CommandList.h"

#include "D3D12Buffer.h"
#include "D3D12PipelineState.h"
#include "D3D12ResourceSet.h"
#include "D3D12Texture.h"
#include "D3D12Query.h"
#include "D3D12Validation.h"
#ifdef DY_RENDERDOC_ENABLED
#include "PixEvents.h"
#endif
#include "d3dx12.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <d3d12.h>
#include <limits>
#include <wrl.h>

using Microsoft::WRL::ComPtr;

namespace dyf::Backends
{
    struct D3D12CommandStorage
    {
        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12GraphicsCommandList> commands;
    };

    struct D3D12CommandPoolCache::Impl
    {
        struct Slot {std::atomic<D3D12CommandStorage*> storage{nullptr};};
        static_assert(std::atomic<D3D12CommandStorage*>::is_always_lock_free,
            "D3D12 command storage requires lock-free ownership atomics.");
        ComPtr<ID3D12Device> device;
        std::array<Slot,64> available;
        ~Impl()
        {for(auto& slot:available)delete slot.storage.load(std::memory_order_relaxed);}
    };

    D3D12CommandPoolCache::D3D12CommandPoolCache(void* device) :m_impl(std::make_unique<Impl>())
    {m_impl->device=static_cast<ID3D12Device*>(device);}
    D3D12CommandPoolCache::~D3D12CommandPoolCache()=default;

    std::unique_ptr<D3D12CommandStorage> D3D12CommandPoolCache::Acquire()
    {
        if(!m_impl->device)return {};
        std::unique_ptr<D3D12CommandStorage> storage;
        for(auto& slot:m_impl->available)
        {
            if(!slot.storage.load(std::memory_order_relaxed))continue;
            storage.reset(slot.storage.exchange(nullptr,std::memory_order_acquire));
            if(storage)break;
        }
        if(storage)
        {
            if(FAILED(storage->allocator->Reset()) || FAILED(storage->commands->Reset(storage->allocator.Get(),nullptr)))return {};
        }
        else
        {
            storage=std::make_unique<D3D12CommandStorage>();
            if(FAILED(m_impl->device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&storage->allocator))) ||
                FAILED(m_impl->device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,storage->allocator.Get(),nullptr,
                    IID_PPV_ARGS(&storage->commands))))return {};
        }
        return storage;
    }

    void D3D12CommandPoolCache::Return(std::unique_ptr<D3D12CommandStorage> storage) noexcept
    {
        if(!storage)return;
        auto* owned=storage.release();
        for(auto& slot:m_impl->available)
        {
            D3D12CommandStorage* empty=nullptr;
            if(slot.storage.compare_exchange_strong(empty,owned,std::memory_order_release,
                std::memory_order_relaxed))return;
        }
        delete owned; // The existing 64-entry cache bound remains unchanged.
    }

    namespace
    {
        D3D12_RESOURCE_STATES ToNativeState(RHI::ResourceState state)
        {
            switch (state)
            {
            case RHI::ResourceState::CopyDestination: return D3D12_RESOURCE_STATE_COPY_DEST;
            case RHI::ResourceState::VertexBuffer:
            case RHI::ResourceState::ConstantBuffer:
                return D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
            case RHI::ResourceState::IndexBuffer: return D3D12_RESOURCE_STATE_INDEX_BUFFER;
            case RHI::ResourceState::IndirectArgument: return D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT;
            case RHI::ResourceState::ShaderResource:
                return D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            case RHI::ResourceState::UnorderedAccess: return D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            case RHI::ResourceState::RenderTarget: return D3D12_RESOURCE_STATE_RENDER_TARGET;
            case RHI::ResourceState::DepthRead: return D3D12_RESOURCE_STATE_DEPTH_READ;
            case RHI::ResourceState::DepthWrite: return D3D12_RESOURCE_STATE_DEPTH_WRITE;
            case RHI::ResourceState::Present: return D3D12_RESOURCE_STATE_PRESENT;
            case RHI::ResourceState::Undefined:
            case RHI::ResourceState::Common:
            default:
                return D3D12_RESOURCE_STATE_COMMON;
            }
        }

        void TrackSwapchainImage(
            std::vector<D3D12Texture*>& images,
            D3D12Texture* texture)
        {
            if (texture == nullptr || !texture->IsSwapchainImage()) return;
            if (std::find(images.begin(), images.end(), texture) == images.end())
                images.push_back(texture);
        }

        using RetainedObjects = std::unordered_map<ID3D12Object*, ComPtr<ID3D12Object>>;

        void RetainObject(RetainedObjects& objects, ID3D12Object* object)
        {
            if (object != nullptr) objects.try_emplace(object, object);
        }

        bool IsBufferStateValid(
            const D3D12Buffer& buffer,
            RHI::ResourceState state)
        {
            return D3D12Validation::IsBufferStateAllowed(buffer.GetDesc(), state);
        }

        bool IsTextureStateValid(
            const D3D12Texture& texture,
            RHI::ResourceState state)
        {
            switch (state)
            {
            case RHI::ResourceState::Undefined:
            case RHI::ResourceState::Common:
            case RHI::ResourceState::CopyDestination:
                return true;
            case RHI::ResourceState::ShaderResource:
                return D3D12Validation::HasUsage(
                    texture.GetDesc().usage, RHI::TextureUsage::ShaderResource);
            case RHI::ResourceState::UnorderedAccess:
                return D3D12Validation::HasUsage(texture.GetDesc().usage, RHI::TextureUsage::Storage);
            case RHI::ResourceState::RenderTarget:
                return D3D12Validation::HasUsage(texture.GetDesc().usage, RHI::TextureUsage::RenderTarget);
            case RHI::ResourceState::DepthRead:
            case RHI::ResourceState::DepthWrite:
                return D3D12Validation::HasUsage(texture.GetDesc().usage, RHI::TextureUsage::DepthStencil);
            case RHI::ResourceState::Present:
                return texture.IsSwapchainImage();
            default:
                return false;
            }
        }

        bool ResolveTextureSubresourceRange(
            const D3D12Texture& texture,
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
            return mipLevelCount <=
                    texture.GetDesc().mipLevels - range.firstMipLevel &&
                arrayLayerCount <=
                    texture.GetDesc().depthOrArraySize - range.firstArrayLayer;
        }

        enum class OperationKind : uint8_t
        {
            BufferBarrier,
            TextureBarrier
        };

        struct D3D12Operation
        {
            OperationKind kind = OperationKind::BufferBarrier;
            D3D12Buffer* buffer = nullptr;
            D3D12Texture* texture = nullptr;
            RHI::ResourceState after = RHI::ResourceState::Undefined;
            uint32_t mipLevel = 0;
            uint32_t arrayLayer = 0;
        };
    }

    struct D3D12CommandListInternal
    {
        D3D12CommandListInternal(std::shared_ptr<D3D12UploadPool> pool,RHI::Detail::UploadPolicy policy)
            : uploads(std::move(pool),policy) {}

        struct DiscardRequest
        {
            ComPtr<ID3D12Resource> resource;
            UINT subresource = 0;
        };

        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12GraphicsCommandList> commandList;
        std::shared_ptr<D3D12CommandPoolCache> commandPool;
        std::unique_ptr<D3D12CommandStorage> commandStorage;
        bool reuseAllowed=false;
        ComPtr<ID3D12CommandSignature> drawSignature;
        ComPtr<ID3D12CommandSignature> drawIndexedSignature;
        ComPtr<ID3D12CommandSignature> dispatchSignature;
        // Staging leases survive until retirement; native recording storage is cached separately.
        D3D12Detail::UploadArena<D3D12UploadPage> uploads;
        ID3D12Device* device = nullptr;
        D3D12PipelineState* pipeline = nullptr;
        std::vector<D3D12Texture*> referencedSwapchainImages;
        RetainedObjects retainedObjects;
        std::vector<DiscardRequest> storeDiscardResources;
        std::vector<D3D12Operation> operations;
        std::map<uint32_t, D3D12_VERTEX_BUFFER_VIEW> vertexBufferViews;
        bool rendering = false;
        bool closed = false;
        bool recordingFailed = false;
        struct TimestampOperation { D3D12TimestampQuery* query; uint32_t index; bool write; };
        std::vector<TimestampOperation> timestamps;
    };

    namespace
    {
        bool CanRecord(const D3D12CommandListInternal* internal)
        {
            return internal != nullptr && !internal->closed &&
                !internal->recordingFailed && internal->commandList != nullptr;
        }

        void RejectRecording(D3D12CommandListInternal* internal)
        {
            if (internal != nullptr) internal->recordingFailed = true;
        }

    }

    D3D12CommandList::D3D12CommandList(void* nativeDevice,
        std::shared_ptr<D3D12UploadPool> uploadPool,
        void* drawSignature, void* drawIndexedSignature, void* dispatchSignature, RHI::Detail::UploadPolicy policy,
        std::shared_ptr<D3D12CommandPoolCache> commandPool)
        : m_internal(new D3D12CommandListInternal(uploadPool ? std::move(uploadPool)
            : MakeD3D12UploadPool(static_cast<ID3D12Device*>(nativeDevice)),policy))
    {
        auto* device = static_cast<ID3D12Device*>(nativeDevice);
        if (device == nullptr)
        {
            m_internal->recordingFailed = true;
            return;
        }
        m_internal->device = device;
        m_internal->drawSignature = static_cast<ID3D12CommandSignature*>(drawSignature);
        m_internal->drawIndexedSignature = static_cast<ID3D12CommandSignature*>(drawIndexedSignature);
        m_internal->dispatchSignature = static_cast<ID3D12CommandSignature*>(dispatchSignature);
        if(commandPool)
        {
            m_internal->commandPool=std::move(commandPool);
            try {m_internal->commandStorage=m_internal->commandPool->Acquire();}
            catch(...) {delete m_internal;m_internal=nullptr;throw;}
            if(!m_internal->commandStorage) {m_internal->recordingFailed=true;return;}
            m_internal->allocator=m_internal->commandStorage->allocator;
            m_internal->commandList=m_internal->commandStorage->commands;
            return;
        }
        if (FAILED(device->CreateCommandAllocator(
                D3D12_COMMAND_LIST_TYPE_DIRECT,
                IID_PPV_ARGS(&m_internal->allocator))))
        {
            m_internal->recordingFailed = true;
            return;
        }
        if (FAILED(device->CreateCommandList(
                0,
                D3D12_COMMAND_LIST_TYPE_DIRECT,
                m_internal->allocator.Get(),
                nullptr,
                IID_PPV_ARGS(&m_internal->commandList))))
        {
            m_internal->allocator.Reset();
            m_internal->recordingFailed = true;
        }
    }

    D3D12CommandList::~D3D12CommandList()
    {
        if(m_internal && m_internal->reuseAllowed && IsClosed() && m_internal->commandStorage)
            m_internal->commandPool->Return(std::move(m_internal->commandStorage));
        delete m_internal;
    }

    void D3D12CommandList::AllowPoolReuse() {m_internal->reuseAllowed=true;}

    void D3D12CommandList::ResetTimestampsNative(RHI::TimestampQueryHandle handle, uint32_t first, uint32_t count)
    {
        auto* query = dynamic_cast<D3D12TimestampQuery*>(handle);
        if(!CanRecord(m_internal) || m_internal->rendering || !query)
        { RejectRecording(m_internal); return; }
        for(uint32_t index = first; index < first + count; ++index)
        {
            if(query->InFlight(index)) { RejectRecording(m_internal); return; }
            m_internal->timestamps.push_back({query, index, false});
        }
    }
    void D3D12CommandList::WriteTimestampNative(RHI::TimestampQueryHandle handle, uint32_t index)
    {
        auto* query = dynamic_cast<D3D12TimestampQuery*>(handle);
        if(!CanRecord(m_internal) || !query || query->InFlight(index))
        { RejectRecording(m_internal); return; }
        m_internal->commandList->EndQuery(query->heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, index);
        // Resolve only a query that was actually written, never unwritten slots in the heap.
        m_internal->commandList->ResolveQueryData(query->heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP,
            index, 1, query->readback.Get(), uint64_t(index) * sizeof(uint64_t));
        m_internal->timestamps.push_back({query, index, true});
    }
    void D3D12CommandList::MarkTimestampsSubmitted(uint64_t completion)
    {
        for(const auto& operation : m_internal->timestamps)
            operation.query->completions[operation.index] = operation.write ? completion : 0;
    }
    void D3D12CommandList::BeginDebugEventNative(const char* name, const RHI::DebugLabelColor& color)
    {
        if(!CanRecord(m_internal)) { RejectRecording(m_internal); return; }
#ifdef DY_RENDERDOC_ENABLED
        BeginPixEvent(m_internal->commandList.Get(),name,color);
#else
        (void)name;(void)color;
#endif
    }
    void D3D12CommandList::EndDebugEventNative()
    {
        if(!CanRecord(m_internal)) { RejectRecording(m_internal); return; }
#ifdef DY_RENDERDOC_ENABLED
        EndPixEvent(m_internal->commandList.Get());
#endif
    }
    void D3D12CommandList::InsertDebugMarkerNative(const char* name, const RHI::DebugLabelColor& color)
    {
        if(!CanRecord(m_internal)) { RejectRecording(m_internal); return; }
#ifdef DY_RENDERDOC_ENABLED
        InsertPixMarker(m_internal->commandList.Get(),name,color);
#else
        (void)name;(void)color;
#endif
    }
    void D3D12CommandList::GlobalBarrierNative()
    {
        const auto barrier = CD3DX12_RESOURCE_BARRIER::UAV(nullptr);
        m_internal->commandList->ResourceBarrier(1, &barrier);
    }

    void D3D12CommandList::ResourceBarrierNative(
        const RHI::ResourceBarrierDesc* barriers,
        uint32_t count)
    {
        if (count == 0) return;
        if (!CanRecord(m_internal) || m_internal->rendering ||
            barriers == nullptr)
        {
            RejectRecording(m_internal);
            return;
        }

        std::vector<D3D12_RESOURCE_BARRIER> nativeBarriers;
        for (uint32_t index = 0; index < count; ++index)
        {
            const RHI::ResourceBarrierDesc& barrier = barriers[index];
            if ((barrier.buffer == nullptr) == (barrier.texture == nullptr) ||
                barrier.after == RHI::ResourceState::Undefined)
            {
                RejectRecording(m_internal);
                return;
            }

            ID3D12Resource* resource = nullptr;
            if (barrier.buffer != nullptr)
            {
                auto* buffer = dynamic_cast<D3D12Buffer*>(barrier.buffer);
                if (buffer == nullptr ||
                    !IsBufferStateValid(*buffer, barrier.before) ||
                    !IsBufferStateValid(*buffer, barrier.after))
                {
                    RejectRecording(m_internal);
                    return;
                }
                resource = static_cast<ID3D12Resource*>(buffer->GetNativeResource());
            }
            else
            {
                auto* texture = dynamic_cast<D3D12Texture*>(barrier.texture);
                if (texture == nullptr ||
                    !IsTextureStateValid(*texture, barrier.before) ||
                    !IsTextureStateValid(*texture, barrier.after))
                {
                    RejectRecording(m_internal);
                    return;
                }
                resource = static_cast<ID3D12Resource*>(texture->GetNativeResource());
                TrackSwapchainImage(m_internal->referencedSwapchainImages, texture);
            }
            if (resource == nullptr)
            {
                RejectRecording(m_internal);
                return;
            }
            RetainObject(m_internal->retainedObjects, resource);

            const D3D12_RESOURCE_STATES before = ToNativeState(barrier.before);
            const D3D12_RESOURCE_STATES after = ToNativeState(barrier.after);
            const bool sameState = barrier.before == barrier.after;

            if (barrier.texture == nullptr)
            {
                if (barrier.subresources.firstMipLevel != 0 ||
                    barrier.subresources.mipLevelCount != 0 ||
                    barrier.subresources.firstArrayLayer != 0 ||
                    barrier.subresources.arrayLayerCount != 0)
                {
                    RejectRecording(m_internal);
                    return;
                }
                auto* buffer = static_cast<D3D12Buffer*>(barrier.buffer);
                D3D12Operation operation = {};
                operation.kind = OperationKind::BufferBarrier;
                operation.buffer = buffer;
                operation.after = barrier.after;
                m_internal->operations.push_back(operation);
                if (sameState)
                {
                    if (barrier.before != RHI::ResourceState::UnorderedAccess)
                    {
                        // Legacy D3D12 copies are synchronous; repeated read states need no barrier.
                        continue;
                    }
                    nativeBarriers.push_back(CD3DX12_RESOURCE_BARRIER::UAV(resource));
                    continue;
                }
                if (before == after) continue;
                nativeBarriers.push_back(
                    CD3DX12_RESOURCE_BARRIER::Transition(resource, before, after));
                continue;
            }

            auto* texture = static_cast<D3D12Texture*>(barrier.texture);
            const RHI::TextureDesc& textureDesc = texture->GetDesc();
            uint32_t mipCount = 0;
            uint32_t layerCount = 0;
            if (!ResolveTextureSubresourceRange(
                    *texture,
                    barrier.subresources,
                    mipCount,
                    layerCount))
            {
                RejectRecording(m_internal);
                return;
            }
            for (uint32_t layer = barrier.subresources.firstArrayLayer;
                layer < barrier.subresources.firstArrayLayer + layerCount;
                ++layer)
            {
                for (uint32_t mip = barrier.subresources.firstMipLevel;
                    mip < barrier.subresources.firstMipLevel + mipCount;
                    ++mip)
                {
                    D3D12Operation operation = {};
                    operation.kind = OperationKind::TextureBarrier;
                    operation.texture = texture;
                    operation.after = barrier.after;
                    operation.mipLevel = mip;
                    operation.arrayLayer = layer;
                    m_internal->operations.push_back(operation);
                }
            }
            if (sameState)
            {
                if (barrier.before != RHI::ResourceState::UnorderedAccess)
                {
                    // Legacy copy and output-merger accesses already preserve dependent ordering.
                    // Read-only states likewise need no native transition to the same state.
                    continue;
                }
                nativeBarriers.push_back(CD3DX12_RESOURCE_BARRIER::UAV(resource));
                continue;
            }
            if (before == after) continue;

            if (barrier.subresources.firstMipLevel == 0 &&
                barrier.subresources.firstArrayLayer == 0 &&
                mipCount == textureDesc.mipLevels &&
                layerCount == textureDesc.depthOrArraySize)
            {
                nativeBarriers.push_back(
                    CD3DX12_RESOURCE_BARRIER::Transition(resource, before, after));
                continue;
            }

            const DXGI_FORMAT format = resource->GetDesc().Format;
            const uint32_t planeCount = m_internal->device != nullptr
                ? D3D12GetFormatPlaneCount(m_internal->device, format)
                : 0;
            if (planeCount == 0)
            {
                RejectRecording(m_internal);
                return;
            }
            for (uint32_t plane = 0; plane < planeCount; ++plane)
            {
                for (uint32_t layer = 0; layer < layerCount; ++layer)
                {
                    for (uint32_t mip = 0; mip < mipCount; ++mip)
                    {
                        const UINT subresource = D3D12CalcSubresource(
                            barrier.subresources.firstMipLevel + mip,
                            barrier.subresources.firstArrayLayer + layer,
                            plane,
                            textureDesc.mipLevels,
                            textureDesc.depthOrArraySize);
                        nativeBarriers.push_back(CD3DX12_RESOURCE_BARRIER::Transition(
                            resource, before, after, subresource));
                    }
                }
            }
        }

        if (!nativeBarriers.empty())
        {
            if (nativeBarriers.size() > std::numeric_limits<UINT>::max())
            {
                RejectRecording(m_internal);
                return;
            }
            m_internal->commandList->ResourceBarrier(
                static_cast<UINT>(nativeBarriers.size()), nativeBarriers.data());
        }
    }

    void D3D12CommandList::BeginRenderingNative(const RHI::RenderingDesc& desc)
    {
        if (!CanRecord(m_internal) || m_internal->rendering ||
            desc.colorAttachmentCount > D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT)
        {
            RejectRecording(m_internal);
            return;
        }

        for (uint32_t index = 0; index < desc.colorAttachmentCount; ++index)
        {
            const RHI::ColorAttachment& attachment = desc.colorAttachments[index];
            auto* texture = static_cast<D3D12Texture*>(attachment.texture);
            if (texture->GetRenderTargetViewHandle(
                    attachment.mipLevel, attachment.arrayLayer) == 0 ||
                texture->GetNativeResource() == nullptr)
            {
                RejectRecording(m_internal);
                return;
            }
        }
        if (desc.depthStencilAttachment != nullptr)
        {
            const RHI::DepthStencilAttachment& attachment =
                *desc.depthStencilAttachment;
            auto* texture = static_cast<D3D12Texture*>(attachment.texture);
            if (texture->GetDepthStencilViewHandle(
                    attachment.mipLevel,
                    attachment.arrayLayer,
                    attachment.state == RHI::ResourceState::DepthRead) == 0 ||
                texture->GetNativeResource() == nullptr)
            {
                RejectRecording(m_internal);
                return;
            }
        }

        std::array<D3D12_CPU_DESCRIPTOR_HANDLE,D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT> colorHandles={};
        for (uint32_t index = 0; index < desc.colorAttachmentCount; ++index)
        {
            const RHI::ColorAttachment& attachment = desc.colorAttachments[index];
            auto* texture = static_cast<D3D12Texture*>(attachment.texture);
            auto* resource = static_cast<ID3D12Resource*>(texture->GetNativeResource());

            D3D12_CPU_DESCRIPTOR_HANDLE handle = {};
            handle.ptr = texture->GetRenderTargetViewHandle(
                attachment.mipLevel, attachment.arrayLayer);
            colorHandles[index]=handle;
            TrackSwapchainImage(m_internal->referencedSwapchainImages, texture);
            RetainObject(m_internal->retainedObjects, resource);
            RetainObject(
                m_internal->retainedObjects,
                static_cast<ID3D12DescriptorHeap*>(
                    texture->GetRenderTargetViewHeap()));
            const UINT subresource = D3D12CalcSubresource(
                attachment.mipLevel,
                attachment.arrayLayer,
                0,
                texture->GetDesc().mipLevels,
                texture->GetDesc().depthOrArraySize);

            if (attachment.loadOp == RHI::LoadOp::Clear)
            {
                m_internal->commandList->ClearRenderTargetView(
                    handle, attachment.clearColor, 0, nullptr);
            }
            else if (attachment.loadOp == RHI::LoadOp::Discard)
            {
                D3D12_DISCARD_REGION region = {};
                region.FirstSubresource = subresource;
                region.NumSubresources = 1;
                m_internal->commandList->DiscardResource(resource, &region);
            }
            if (attachment.storeOp == RHI::StoreOp::Discard)
            {
                m_internal->storeDiscardResources.push_back({ resource, subresource });
            }
        }

        D3D12_CPU_DESCRIPTOR_HANDLE depthHandle = {};
        D3D12_CPU_DESCRIPTOR_HANDLE* depthHandlePointer = nullptr;
        if (desc.depthStencilAttachment != nullptr)
        {
            const RHI::DepthStencilAttachment& attachment =
                *desc.depthStencilAttachment;
            auto* texture = static_cast<D3D12Texture*>(attachment.texture);
            auto* resource = static_cast<ID3D12Resource*>(texture->GetNativeResource());
            depthHandle.ptr = texture->GetDepthStencilViewHandle(
                attachment.mipLevel,
                attachment.arrayLayer,
                attachment.state == RHI::ResourceState::DepthRead);
            depthHandlePointer = &depthHandle;
            RetainObject(m_internal->retainedObjects, resource);
            RetainObject(
                m_internal->retainedObjects,
                static_cast<ID3D12DescriptorHeap*>(
                    texture->GetDepthStencilViewHeap()));

            D3D12_CLEAR_FLAGS clearFlags = static_cast<D3D12_CLEAR_FLAGS>(0);
            if (attachment.depthLoadOp == RHI::LoadOp::Clear)
                clearFlags |= D3D12_CLEAR_FLAG_DEPTH;
            if (attachment.stencilLoadOp == RHI::LoadOp::Clear)
                clearFlags |= D3D12_CLEAR_FLAG_STENCIL;
            if (clearFlags != 0)
            {
                m_internal->commandList->ClearDepthStencilView(
                    depthHandle,
                    clearFlags,
                    attachment.clearDepth,
                    static_cast<UINT8>(attachment.clearStencil),
                    0,
                    nullptr);
            }
            if (attachment.state == RHI::ResourceState::DepthWrite &&
                attachment.depthLoadOp == RHI::LoadOp::Discard)
            {
                D3D12_DISCARD_REGION region = {};
                region.FirstSubresource = D3D12CalcSubresource(
                    attachment.mipLevel,
                    attachment.arrayLayer,
                    0,
                    texture->GetDesc().mipLevels,
                    texture->GetDesc().depthOrArraySize);
                region.NumSubresources = 1;
                m_internal->commandList->DiscardResource(resource, &region);
            }
            if (attachment.state == RHI::ResourceState::DepthWrite &&
                attachment.stencilLoadOp == RHI::LoadOp::Discard &&
                texture->GetDesc().format == RHI::Format::D24_UNORM_S8_UINT)
            {
                D3D12_DISCARD_REGION region = {};
                region.FirstSubresource = D3D12CalcSubresource(
                    attachment.mipLevel,
                    attachment.arrayLayer,
                    1,
                    texture->GetDesc().mipLevels,
                    texture->GetDesc().depthOrArraySize);
                region.NumSubresources = 1;
                m_internal->commandList->DiscardResource(resource, &region);
            }
            if (attachment.state == RHI::ResourceState::DepthWrite &&
                attachment.depthStoreOp == RHI::StoreOp::Discard)
            {
                m_internal->storeDiscardResources.push_back({
                    resource,
                    D3D12CalcSubresource(
                        attachment.mipLevel,
                        attachment.arrayLayer,
                        0,
                        texture->GetDesc().mipLevels,
                        texture->GetDesc().depthOrArraySize)
                });
            }
            if (attachment.state == RHI::ResourceState::DepthWrite &&
                attachment.stencilStoreOp == RHI::StoreOp::Discard &&
                texture->GetDesc().format == RHI::Format::D24_UNORM_S8_UINT)
            {
                m_internal->storeDiscardResources.push_back({
                    resource,
                    D3D12CalcSubresource(
                        attachment.mipLevel,
                        attachment.arrayLayer,
                        1,
                        texture->GetDesc().mipLevels,
                        texture->GetDesc().depthOrArraySize)
                });
            }
        }

        m_internal->commandList->OMSetRenderTargets(
            desc.colorAttachmentCount,
            desc.colorAttachmentCount ? colorHandles.data() : nullptr,
            FALSE,
            depthHandlePointer);
        m_internal->pipeline = nullptr;
        m_internal->rendering = true;
    }

    void D3D12CommandList::EndRenderingNative()
    {
        if (!CanRecord(m_internal) || !m_internal->rendering)
        {
            RejectRecording(m_internal);
            return;
        }
        for (const D3D12CommandListInternal::DiscardRequest& discard :
            m_internal->storeDiscardResources)
        {
            D3D12_DISCARD_REGION region = {};
            region.FirstSubresource = discard.subresource;
            region.NumSubresources = 1;
            m_internal->commandList->DiscardResource(
                discard.resource.Get(), &region);
        }
        m_internal->storeDiscardResources.clear();
        m_internal->rendering = false;
    }

    void D3D12CommandList::BindGraphicsPipelineNative(RHI::PipelineHandle pipelineState)
    {
        if (!CanRecord(m_internal))
        {
            RejectRecording(m_internal);
            return;
        }
        auto* pipeline = static_cast<D3D12PipelineState*>(pipelineState);
        if (pipeline->GetNativePipelineState() == nullptr ||
            pipeline->GetNativeRootSignature() == nullptr)
        {
            RejectRecording(m_internal);
            return;
        }
        m_internal->pipeline = pipeline;
        m_internal->commandList->SetPipelineState(pipeline->GetNativePipelineState());
        m_internal->commandList->SetGraphicsRootSignature(
            pipeline->GetNativeRootSignature());
        if (!pipeline->IsMesh())
        {
            m_internal->commandList->IASetPrimitiveTopology(
                static_cast<D3D12_PRIMITIVE_TOPOLOGY>(pipeline->GetPrimitiveTopology()));
            // RHI의 stride는 파이프라인 상태다. D3D12는 VB view에 보관하므로
            // 같은 버퍼를 유지한 채 PSO를 바꿔도 새 레이아웃을 적용한다.
            for (auto& [binding, view] : m_internal->vertexBufferViews)
            {
                const uint32_t stride = pipeline->GetVertexStride(binding);
                if (stride != 0 && view.StrideInBytes != stride)
                {
                    view.StrideInBytes = stride;
                    m_internal->commandList->IASetVertexBuffers(binding, 1, &view);
                }
            }
        }
        RetainObject(m_internal->retainedObjects, pipeline->GetNativePipelineState());
        RetainObject(m_internal->retainedObjects, pipeline->GetNativeRootSignature());
    }

    void D3D12CommandList::BindComputePipelineNative(RHI::PipelineHandle pipelineState)
    {
        if (!CanRecord(m_internal))
        {
            RejectRecording(m_internal);
            return;
        }
        auto* pipeline = static_cast<D3D12PipelineState*>(pipelineState);
        m_internal->pipeline = pipeline;
        m_internal->commandList->SetPipelineState(pipeline->GetNativePipelineState());
        m_internal->commandList->SetComputeRootSignature(
            pipeline->GetNativeRootSignature());
    }

    void D3D12CommandList::DispatchNative(uint32_t x, uint32_t y, uint32_t z)
    {
        if (!CanRecord(m_internal))
        {
            RejectRecording(m_internal);
            return;
        }
        m_internal->commandList->Dispatch(x, y, z);
    }

    void D3D12CommandList::DispatchIndirectNative(RHI::BufferHandle arguments, uint32_t offset)
    {
        auto* buffer = static_cast<D3D12Buffer*>(arguments);
        if (!CanRecord(m_internal) || m_internal->dispatchSignature == nullptr ||
            buffer->GetNativeResource() == nullptr)
        {
            RejectRecording(m_internal);
            return;
        }
        auto* resource = static_cast<ID3D12Resource*>(buffer->GetNativeResource());
        RetainObject(m_internal->retainedObjects, resource);
        m_internal->commandList->ExecuteIndirect(
            m_internal->dispatchSignature.Get(), 1, resource, offset, nullptr, 0);
    }

    void D3D12CommandList::BindResourceSetNative(RHI::ResourceSetHandle resourceSet)
    {
        if (!CanRecord(m_internal))
        {
            RejectRecording(m_internal);
            return;
        }
        auto* set = static_cast<D3D12ResourceSet*>(resourceSet);
        ID3D12DescriptorHeap* heap = set->GetNativeDescriptorHeap();
        if (m_internal->pipeline->GetDescriptorCount() != 0 && heap == nullptr)
        {
            RejectRecording(m_internal);
            return;
        }
        // Borrowed swapchain images still participate in native WSI retirement.
        for (const auto& requirement : set->GetStateRequirements())
        {
            if (requirement.texture)
            {
                auto* texture = static_cast<D3D12Texture*>(requirement.texture);
                TrackSwapchainImage(
                    m_internal->referencedSwapchainImages, texture);
            }
        }
        if (heap != nullptr)
        {
            m_internal->commandList->SetDescriptorHeaps(1, &heap);
            const D3D12_GPU_DESCRIPTOR_HANDLE start =
                heap->GetGPUDescriptorHandleForHeapStart();
            for (const D3D12PipelineBinding& binding :
                m_internal->pipeline->GetBindings())
            {
                D3D12_GPU_DESCRIPTOR_HANDLE handle = start;
                handle.ptr += static_cast<UINT64>(binding.descriptorOffset) *
                    set->GetDescriptorSize();
                if (m_internal->pipeline->IsCompute())
                {
                    m_internal->commandList->SetComputeRootDescriptorTable(
                        binding.rootParameter, handle);
                }
                else
                {
                    m_internal->commandList->SetGraphicsRootDescriptorTable(
                        binding.rootParameter, handle);
                }
            }
            RetainObject(m_internal->retainedObjects, heap);
        }
        for (uint32_t index = 0; index < set->GetNativeResourceCount(); ++index)
            RetainObject(m_internal->retainedObjects, set->GetNativeResource(index));
    }

    void D3D12CommandList::BindVertexBufferNative(
        uint32_t binding,
        RHI::BufferHandle buffer,
        uint32_t offset)
    {
        if (!CanRecord(m_internal) ||
            binding >= D3D12_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT)
        {
            RejectRecording(m_internal);
            return;
        }
        const uint32_t stride = m_internal->pipeline->GetVertexStride(binding);
        if (stride == 0)
        {
            RejectRecording(m_internal);
            return;
        }
        auto* d3dBuffer = static_cast<D3D12Buffer*>(buffer);
        auto* resource = static_cast<ID3D12Resource*>(d3dBuffer->GetNativeResource());
        if (resource == nullptr)
        {
            RejectRecording(m_internal);
            return;
        }

        D3D12_VERTEX_BUFFER_VIEW view = {};
        view.BufferLocation = resource->GetGPUVirtualAddress() + offset;
        view.SizeInBytes = buffer->GetDesc().size - offset;
        view.StrideInBytes = stride;
        m_internal->commandList->IASetVertexBuffers(binding, 1, &view);
        m_internal->vertexBufferViews[binding] = view;
        RetainObject(m_internal->retainedObjects, resource);
    }

    void D3D12CommandList::BindIndexBufferNative(
        RHI::BufferHandle buffer,
        RHI::Format format,
        uint32_t offset)
    {
        if (!CanRecord(m_internal))
        {
            RejectRecording(m_internal);
            return;
        }
        auto* d3dBuffer = static_cast<D3D12Buffer*>(buffer);
        auto* resource = static_cast<ID3D12Resource*>(d3dBuffer->GetNativeResource());
        if (resource == nullptr)
        {
            RejectRecording(m_internal);
            return;
        }

        D3D12_INDEX_BUFFER_VIEW view = {};
        view.BufferLocation = resource->GetGPUVirtualAddress() + offset;
        view.SizeInBytes = buffer->GetDesc().size - offset;
        view.Format = format == RHI::Format::R16_UINT
            ? DXGI_FORMAT_R16_UINT
            : DXGI_FORMAT_R32_UINT;
        m_internal->commandList->IASetIndexBuffer(&view);
        RetainObject(m_internal->retainedObjects, resource);
    }

    void D3D12CommandList::SetInlineConstantsNative(
        uint32_t offset,
        uint32_t size,
        const void* data)
    {
        if (!CanRecord(m_internal))
        {
            RejectRecording(m_internal);
            return;
        }
        const uint32_t rootParameter =
            m_internal->pipeline->GetInlineConstantRootParameter();
        if (rootParameter == std::numeric_limits<uint32_t>::max())
        {
            RejectRecording(m_internal);
            return;
        }
        if (m_internal->pipeline->IsCompute())
        {
            m_internal->commandList->SetComputeRoot32BitConstants(
                rootParameter, size / 4, data, offset / 4);
        }
        else
        {
            m_internal->commandList->SetGraphicsRoot32BitConstants(
                rootParameter, size / 4, data, offset / 4);
        }
    }

    void D3D12CommandList::SetStencilReferenceNative(uint32_t reference)
    {
        if (!CanRecord(m_internal))
        {
            RejectRecording(m_internal);
            return;
        }
        m_internal->commandList->OMSetStencilRef(reference);
    }

    void D3D12CommandList::SetViewportNative(const RHI::Viewport& viewport)
    {
        if (!CanRecord(m_internal))
        {
            RejectRecording(m_internal);
            return;
        }
        D3D12_VIEWPORT nativeViewport = {
            viewport.x,
            viewport.y,
            viewport.width,
            viewport.height,
            viewport.minDepth,
            viewport.maxDepth
        };
        m_internal->commandList->RSSetViewports(1, &nativeViewport);
    }

    void D3D12CommandList::SetScissorNative(const RHI::Rect& rect)
    {
        const int64_t right = static_cast<int64_t>(rect.x) + rect.width;
        const int64_t bottom = static_cast<int64_t>(rect.y) + rect.height;
        if (!CanRecord(m_internal) ||
            right > std::numeric_limits<LONG>::max() ||
            bottom > std::numeric_limits<LONG>::max())
        {
            RejectRecording(m_internal);
            return;
        }
        D3D12_RECT nativeRect = {
            rect.x,
            rect.y,
            static_cast<LONG>(right),
            static_cast<LONG>(bottom)
        };
        m_internal->commandList->RSSetScissorRects(1, &nativeRect);
    }

    void D3D12CommandList::DrawInstancedNative(
        uint32_t vertexCount,
        uint32_t instanceCount,
        uint32_t startVertex,
        uint32_t startInstance)
    {
        if (!CanRecord(m_internal))
        {
            RejectRecording(m_internal);
            return;
        }
        m_internal->commandList->DrawInstanced(
            vertexCount, instanceCount, startVertex, startInstance);
    }

    void D3D12CommandList::DrawIndexedInstancedNative(
        uint32_t indexCount,
        uint32_t instanceCount,
        uint32_t firstIndex,
        int32_t vertexOffset,
        uint32_t firstInstance)
    {
        if (!CanRecord(m_internal))
        {
            RejectRecording(m_internal);
            return;
        }
        m_internal->commandList->DrawIndexedInstanced(
            indexCount, instanceCount, firstIndex, vertexOffset, firstInstance);
    }

    void D3D12CommandList::DrawIndirectNative(RHI::BufferHandle arguments, uint32_t offset)
    {
        auto* buffer = static_cast<D3D12Buffer*>(arguments);
        if (!CanRecord(m_internal) || m_internal->drawSignature == nullptr ||
            buffer->GetNativeResource() == nullptr)
        {
            RejectRecording(m_internal);
            return;
        }
        auto* resource = static_cast<ID3D12Resource*>(buffer->GetNativeResource());
        RetainObject(m_internal->retainedObjects, resource);
        m_internal->commandList->ExecuteIndirect(
            m_internal->drawSignature.Get(), 1, resource, offset, nullptr, 0);
    }

    void D3D12CommandList::DrawIndexedIndirectNative(RHI::BufferHandle arguments, uint32_t offset)
    {
        auto* buffer = static_cast<D3D12Buffer*>(arguments);
        if (!CanRecord(m_internal) || m_internal->drawIndexedSignature == nullptr ||
            buffer->GetNativeResource() == nullptr)
        {
            RejectRecording(m_internal);
            return;
        }
        auto* resource = static_cast<ID3D12Resource*>(buffer->GetNativeResource());
        RetainObject(m_internal->retainedObjects, resource);
        m_internal->commandList->ExecuteIndirect(
            m_internal->drawIndexedSignature.Get(), 1, resource, offset, nullptr, 0);
    }

    void D3D12CommandList::DispatchMeshNative(uint32_t x, uint32_t y, uint32_t z)
    {
        if (!CanRecord(m_internal))
        {
            RejectRecording(m_internal);
            return;
        }
        ComPtr<ID3D12GraphicsCommandList6> cmdList6;
        if (FAILED(m_internal->commandList.As(&cmdList6)) || cmdList6 == nullptr)
        {
            RejectRecording(m_internal);
            return;
        }
        cmdList6->DispatchMesh(x, y, z);
    }

    bool D3D12CommandList::CloseNative()
    {
        if (m_internal == nullptr || m_internal->closed) return IsClosed();
        if (m_internal->rendering || m_internal->commandList == nullptr ||
            FAILED(m_internal->commandList->Close()))
        {
            RejectRecording(m_internal);
            return IsClosed();
        }
        m_internal->closed = true;

        return IsClosed();
    }

    void* D3D12CommandList::GetNativeList()
    {
        return m_internal == nullptr ? nullptr : m_internal->commandList.Get();
    }

    bool D3D12CommandList::IsClosed() const
    {
        return m_internal != nullptr && m_internal->closed &&
            !m_internal->recordingFailed;
    }

    const std::vector<D3D12Texture*>&
    D3D12CommandList::GetReferencedSwapchainImages() const
    {
        return m_internal->referencedSwapchainImages;
    }

    bool D3D12CommandList::RecordBufferUpload(
        D3D12Buffer* buffer,
        uint32_t offset,
        const void* data,
        uint32_t size)
    {
        if (!CanRecord(m_internal) || m_internal->rendering ||
            m_internal->device == nullptr)
        {
            return false;
        }
        auto* destination = static_cast<ID3D12Resource*>(buffer->GetNativeResource());
        if (destination == nullptr) return false;

        const auto upload = m_internal->uploads.Allocate(size);
        if (!upload) return false;
        std::memcpy(upload.data, data, size);

        // Finish fallible bookkeeping before emitting a native command.
        RetainObject(m_internal->retainedObjects, destination);
        m_internal->commandList->CopyBufferRegion(
            destination, offset, upload.page->resource.Get(), upload.offset, size);
        return true;
    }

    bool D3D12CommandList::RecordTextureUpload(
        D3D12Texture* texture,
        uint32_t mipLevel,
        uint32_t arrayLayer,
        const void* data,
        uint32_t dataSize,
        uint32_t rowPitch,
        uint32_t slicePitch)
    {
        if (!CanRecord(m_internal) || m_internal->rendering ||
            m_internal->device == nullptr ||
            texture->GetDesc().format == RHI::Format::D32_FLOAT ||
            texture->GetDesc().format == RHI::Format::D24_UNORM_S8_UINT)
        {
            return false;
        }
        auto* destination = static_cast<ID3D12Resource*>(texture->GetNativeResource());
        if (destination == nullptr) return false;
        const UINT subresource = D3D12CalcSubresource(
            mipLevel,
            arrayLayer,
            0,
            texture->GetDesc().mipLevels,
            texture->GetDesc().depthOrArraySize);
        const D3D12_RESOURCE_DESC destinationDesc = destination->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
        UINT rowCount = 0;
        UINT64 rowSize = 0;
        UINT64 uploadSize = 0;
        m_internal->device->GetCopyableFootprints(
            &destinationDesc,
            subresource,
            1,
            0,
            &footprint,
            &rowCount,
            &rowSize,
            &uploadSize);
        if (rowCount == 0 || rowSize == 0 || uploadSize == 0 || uploadSize == UINT64_MAX ||
            rowSize > rowPitch ||
            rowSize > footprint.Footprint.RowPitch ||
            rowCount > (std::numeric_limits<uint32_t>::max() / rowPitch))
        {
            return false;
        }
        if(RHI::IsBlockCompressed(texture->GetDesc().format))
        {
            const uint32_t blockBytes = RHI::CompressedBlockBytes(texture->GetDesc().format);
            const uint32_t width = (std::max)(1u, texture->GetDesc().width >> mipLevel);
            const uint32_t height = (std::max)(1u, texture->GetDesc().height >> mipLevel);
            const uint32_t blockColumns = width / 4u + (width % 4u != 0);
            const uint32_t blockRows = height / 4u + (height % 4u != 0);
            // GetCopyableFootprints supplies padded GPU pitches and block-row counts.
            // Validate the caller's packed BC rows before copying into that footprint.
            if(rowPitch % blockBytes != 0 || rowCount != blockRows ||
                rowSize != static_cast<uint64_t>(blockColumns) * blockBytes)
                return false;
        }
        const uint64_t requiredSourceSize =
            static_cast<uint64_t>(rowCount - 1) * rowPitch + rowSize;
        if (requiredSourceSize > slicePitch || requiredSourceSize > dataSize)
            return false;

        const auto upload = m_internal->uploads.Allocate(
            uploadSize, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
        if (!upload) return false;
        const auto* source = static_cast<const uint8_t*>(data);
        for (UINT row = 0; row < rowCount; ++row)
        {
            std::memcpy(
                upload.data + footprint.Offset +
                    static_cast<std::size_t>(row) * footprint.Footprint.RowPitch,
                source + static_cast<std::size_t>(row) * rowPitch,
                static_cast<std::size_t>(rowSize));
        }
        footprint.Offset += upload.offset;
        TrackSwapchainImage(m_internal->referencedSwapchainImages, texture);
        RetainObject(m_internal->retainedObjects, destination);
        const CD3DX12_TEXTURE_COPY_LOCATION sourceLocation(upload.page->resource.Get(), footprint);
        const CD3DX12_TEXTURE_COPY_LOCATION destinationLocation(destination, subresource);
        m_internal->commandList->CopyTextureRegion(
            &destinationLocation, 0, 0, 0, &sourceLocation, nullptr);
        return true;
    }

    void D3D12CommandList::CommitResourceStates()
    {
        if (m_internal == nullptr) return;
        for (const D3D12Operation& operation : m_internal->operations)
        {
            if (operation.kind == OperationKind::BufferBarrier)
            {
                operation.buffer->SetState(operation.after);
            }
            else if (operation.kind == OperationKind::TextureBarrier)
            {
                operation.texture->SetState(
                    operation.mipLevel,
                    operation.arrayLayer,
                    operation.after);
            }
        }
    }
}
