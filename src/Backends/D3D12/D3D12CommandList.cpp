#include "D3D12CommandList.h"

#include "D3D12Buffer.h"
#include "D3D12PipelineState.h"
#include "D3D12ResourceSet.h"
#include "D3D12Texture.h"
#include "d3dx12.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <d3d12.h>
#include <limits>
#include <wrl.h>

using Microsoft::WRL::ComPtr;

namespace dy::Backends
{
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

        void RetainObject(
            std::vector<ComPtr<ID3D12Object>>& objects,
            ID3D12Object* object)
        {
            if (object == nullptr) return;
            for (const ComPtr<ID3D12Object>& retained : objects)
            {
                if (retained.Get() == object) return;
            }
            objects.emplace_back(object);
        }

        bool HasUsage(RHI::BufferUsage usage, RHI::BufferUsage flag)
        {
            return (static_cast<uint32_t>(usage) & static_cast<uint32_t>(flag)) != 0;
        }

        bool HasUsage(RHI::TextureUsage usage, RHI::TextureUsage flag)
        {
            return (static_cast<uint32_t>(usage) & static_cast<uint32_t>(flag)) != 0;
        }

        bool IsBufferStateValid(
            const D3D12Buffer& buffer,
            RHI::ResourceState state)
        {
            switch (state)
            {
            case RHI::ResourceState::Undefined:
            case RHI::ResourceState::Common:
            case RHI::ResourceState::CopyDestination:
                return true;
            case RHI::ResourceState::VertexBuffer:
                return HasUsage(buffer.GetDesc().usage, RHI::BufferUsage::Vertex);
            case RHI::ResourceState::IndexBuffer:
                return HasUsage(buffer.GetDesc().usage, RHI::BufferUsage::Index);
            case RHI::ResourceState::ConstantBuffer:
                return HasUsage(buffer.GetDesc().usage, RHI::BufferUsage::Constant);
            case RHI::ResourceState::ShaderResource:
            case RHI::ResourceState::UnorderedAccess:
                return HasUsage(buffer.GetDesc().usage, RHI::BufferUsage::Storage);
            default:
                return false;
            }
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
                return HasUsage(
                    texture.GetDesc().usage, RHI::TextureUsage::ShaderResource);
            case RHI::ResourceState::UnorderedAccess:
                return HasUsage(texture.GetDesc().usage, RHI::TextureUsage::Storage);
            case RHI::ResourceState::RenderTarget:
                return HasUsage(texture.GetDesc().usage, RHI::TextureUsage::RenderTarget);
            case RHI::ResourceState::DepthRead:
            case RHI::ResourceState::DepthWrite:
                return HasUsage(texture.GetDesc().usage, RHI::TextureUsage::DepthStencil);
            case RHI::ResourceState::Present:
                return texture.IsSwapchainImage();
            default:
                return false;
            }
        }

        std::pair<D3D12Texture*, uint32_t> TextureKey(
            D3D12Texture* texture,
            uint32_t mipLevel,
            uint32_t arrayLayer)
        {
            return {
                texture,
                arrayLayer * texture->GetDesc().mipLevels + mipLevel
            };
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

        const RHI::ResourceBindingLayout* FindLayoutBinding(
            const RHI::PipelineLayoutDesc& layout,
            uint32_t binding)
        {
            for (uint32_t index = 0; index < layout.bindingCount; ++index)
            {
                const RHI::ResourceBindingLayout& candidate =
                    layout.bindings[index];
                if (candidate.binding == binding)
                    return &candidate;
            }
            return nullptr;
        }

        enum class OperationKind : uint8_t
        {
            BufferBarrier,
            TextureBarrier,
            BufferRequirement,
            TextureRequirement,
            BufferWrite,
            TextureWrite
        };

        struct D3D12Operation
        {
            OperationKind kind = OperationKind::BufferBarrier;
            D3D12Buffer* buffer = nullptr;
            D3D12Texture* texture = nullptr;
            RHI::ResourceState before = RHI::ResourceState::Undefined;
            RHI::ResourceState after = RHI::ResourceState::Undefined;
            uint32_t mipLevel = 0;
            uint32_t arrayLayer = 0;
        };
    }

    struct D3D12CommandListInternal
    {
        struct DiscardRequest
        {
            ComPtr<ID3D12Resource> resource;
            UINT subresource = 0;
        };

        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12GraphicsCommandList> commandList;
        ID3D12Device* device = nullptr;
        D3D12PipelineState* pipeline = nullptr;
        D3D12Texture* depthTexture = nullptr;
        uint32_t depthMipLevel = 0;
        uint32_t depthArrayLayer = 0;
        std::vector<D3D12Texture*> referencedSwapchainImages;
        std::vector<ComPtr<ID3D12Object>> retainedObjects;
        std::vector<DiscardRequest> storeDiscardResources;
        std::vector<D3D12Operation> operations;
        std::unordered_map<D3D12Buffer*, RHI::ResourceState> bufferStates;
        std::map<std::pair<D3D12Texture*, uint32_t>, RHI::ResourceState>
            textureStates;
        std::vector<uint8_t> inlineConstantCoverage;
        bool rendering = false;
        bool closed = false;
        bool recordingFailed = false;
        bool viewportSet = false;
        bool scissorSet = false;
        bool stencilReferenceSet = false;
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

        bool RequireBufferState(
            D3D12CommandListInternal* internal,
            D3D12Buffer* buffer,
            RHI::ResourceState state)
        {
            const auto found = internal->bufferStates.find(buffer);
            if (found != internal->bufferStates.end() &&
                found->second != state)
            {
                return false;
            }
            D3D12Operation operation = {};
            operation.kind = OperationKind::BufferRequirement;
            operation.buffer = buffer;
            operation.before = state;
            internal->operations.push_back(operation);
            return true;
        }

        bool RequireTextureState(
            D3D12CommandListInternal* internal,
            D3D12Texture* texture,
            uint32_t mipLevel,
            uint32_t arrayLayer,
            RHI::ResourceState state)
        {
            const auto key = TextureKey(texture, mipLevel, arrayLayer);
            const auto found = internal->textureStates.find(key);
            if (found != internal->textureStates.end() &&
                found->second != state)
            {
                return false;
            }
            D3D12Operation operation = {};
            operation.kind = OperationKind::TextureRequirement;
            operation.texture = texture;
            operation.mipLevel = mipLevel;
            operation.arrayLayer = arrayLayer;
            operation.before = state;
            internal->operations.push_back(operation);
            return true;
        }

        bool RequireTextureSubresourcesInState(
            D3D12CommandListInternal* internal,
            D3D12Texture* texture,
            const RHI::TextureSubresourceRange& range,
            RHI::ResourceState state)
        {
            uint32_t mipLevelCount = 0;
            uint32_t arrayLayerCount = 0;
            if (!ResolveTextureSubresourceRange(
                    *texture, range, mipLevelCount, arrayLayerCount))
            {
                return false;
            }
            for (uint32_t layer = range.firstArrayLayer;
                layer < range.firstArrayLayer + arrayLayerCount; ++layer)
            {
                for (uint32_t mip = range.firstMipLevel;
                    mip < range.firstMipLevel + mipLevelCount; ++mip)
                {
                    if (!RequireTextureState(
                            internal, texture, mip, layer, state))
                    {
                        return false;
                    }
                }
            }
            return true;
        }

        bool HasCompleteDrawState(const D3D12CommandListInternal* internal)
        {
            return internal != nullptr && internal->pipeline != nullptr &&
                internal->viewportSet && internal->scissorSet &&
                (!internal->pipeline->IsStencilEnabled() ||
                    internal->stencilReferenceSet) &&
                std::find(
                    internal->inlineConstantCoverage.begin(),
                    internal->inlineConstantCoverage.end(),
                    0) == internal->inlineConstantCoverage.end();
        }
    }

    D3D12CommandList::D3D12CommandList(void* nativeDevice)
        : m_internal(new D3D12CommandListInternal())
    {
        auto* device = static_cast<ID3D12Device*>(nativeDevice);
        if (device == nullptr)
        {
            m_internal->recordingFailed = true;
            return;
        }
        m_internal->device = device;
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
        delete m_internal;
    }

    void D3D12CommandList::ResourceBarrier(
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
                const auto prior = m_internal->bufferStates.find(buffer);
                if (prior != m_internal->bufferStates.end() &&
                    prior->second != barrier.before)
                {
                    RejectRecording(m_internal);
                    return;
                }
                D3D12Operation operation = {};
                operation.kind = OperationKind::BufferBarrier;
                operation.buffer = buffer;
                operation.before = barrier.before;
                operation.after = barrier.after;
                m_internal->operations.push_back(operation);
                m_internal->bufferStates[buffer] = barrier.after;
                if (sameState)
                {
                    if (barrier.before != RHI::ResourceState::UnorderedAccess)
                    {
                        RejectRecording(m_internal);
                        return;
                    }
                    D3D12_RESOURCE_BARRIER nativeBarrier = {};
                    nativeBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
                    nativeBarrier.UAV.pResource = resource;
                    nativeBarriers.push_back(nativeBarrier);
                    continue;
                }
                if (before == after) continue;
                D3D12_RESOURCE_BARRIER nativeBarrier = {};
                nativeBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                nativeBarrier.Transition.pResource = resource;
                nativeBarrier.Transition.StateBefore = before;
                nativeBarrier.Transition.StateAfter = after;
                nativeBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                nativeBarriers.push_back(nativeBarrier);
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
                    const auto prior = m_internal->textureStates.find(
                        TextureKey(texture, mip, layer));
                    if (prior != m_internal->textureStates.end() &&
                        prior->second != barrier.before)
                    {
                        RejectRecording(m_internal);
                        return;
                    }
                }
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
                    operation.before = barrier.before;
                    operation.after = barrier.after;
                    operation.mipLevel = mip;
                    operation.arrayLayer = layer;
                    m_internal->operations.push_back(operation);
                    m_internal->textureStates[
                        TextureKey(texture, mip, layer)] = barrier.after;
                }
            }
            if (sameState)
            {
                if (barrier.before != RHI::ResourceState::UnorderedAccess)
                {
                    RejectRecording(m_internal);
                    return;
                }
                D3D12_RESOURCE_BARRIER nativeBarrier = {};
                nativeBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
                nativeBarrier.UAV.pResource = resource;
                nativeBarriers.push_back(nativeBarrier);
                continue;
            }
            if (before == after) continue;

            if (barrier.subresources.firstMipLevel == 0 &&
                barrier.subresources.firstArrayLayer == 0 &&
                mipCount == textureDesc.mipLevels &&
                layerCount == textureDesc.depthOrArraySize)
            {
                D3D12_RESOURCE_BARRIER nativeBarrier = {};
                nativeBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                nativeBarrier.Transition.pResource = resource;
                nativeBarrier.Transition.StateBefore = before;
                nativeBarrier.Transition.StateAfter = after;
                nativeBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                nativeBarriers.push_back(nativeBarrier);
                continue;
            }

            D3D12_FEATURE_DATA_FORMAT_INFO formatInfo = {};
            formatInfo.Format = resource->GetDesc().Format;
            if (m_internal->device == nullptr ||
                FAILED(m_internal->device->CheckFeatureSupport(
                    D3D12_FEATURE_FORMAT_INFO, &formatInfo, sizeof(formatInfo))) ||
                formatInfo.PlaneCount == 0)
            {
                RejectRecording(m_internal);
                return;
            }
            const uint32_t planeCount = formatInfo.PlaneCount;
            for (uint32_t plane = 0; plane < planeCount; ++plane)
            {
                for (uint32_t layer = 0; layer < layerCount; ++layer)
                {
                    for (uint32_t mip = 0; mip < mipCount; ++mip)
                    {
                        D3D12_RESOURCE_BARRIER nativeBarrier = {};
                        nativeBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                        nativeBarrier.Transition.pResource = resource;
                        nativeBarrier.Transition.StateBefore = before;
                        nativeBarrier.Transition.StateAfter = after;
                        nativeBarrier.Transition.Subresource = D3D12CalcSubresource(
                            barrier.subresources.firstMipLevel + mip,
                            barrier.subresources.firstArrayLayer + layer,
                            plane,
                            textureDesc.mipLevels,
                            textureDesc.depthOrArraySize);
                        nativeBarriers.push_back(nativeBarrier);
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

    void D3D12CommandList::BeginRendering(const RHI::RenderingDesc& desc)
    {
        if (!CanRecord(m_internal) || m_internal->rendering ||
            (desc.colorAttachmentCount != 0 && desc.colorAttachments == nullptr) ||
            desc.colorAttachmentCount > D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT)
        {
            RejectRecording(m_internal);
            return;
        }

        for (uint32_t index = 0; index < desc.colorAttachmentCount; ++index)
        {
            const RHI::ColorAttachment& attachment = desc.colorAttachments[index];
            auto* texture = dynamic_cast<D3D12Texture*>(attachment.texture);
            if (texture == nullptr ||
                texture->GetRenderTargetViewHandle(
                    attachment.mipLevel, attachment.arrayLayer) == 0 ||
                texture->GetNativeResource() == nullptr ||
                !RequireTextureState(
                    m_internal,
                    texture,
                    attachment.mipLevel,
                    attachment.arrayLayer,
                    RHI::ResourceState::RenderTarget) ||
                attachment.loadOp == RHI::LoadOp::Undefined ||
                attachment.storeOp == RHI::StoreOp::Undefined)
            {
                RejectRecording(m_internal);
                return;
            }
            if (attachment.loadOp == RHI::LoadOp::Clear)
            {
                for (float component : attachment.clearColor)
                {
                    if (!std::isfinite(component))
                    {
                        RejectRecording(m_internal);
                        return;
                    }
                }
            }
        }
        if (desc.depthStencilAttachment != nullptr)
        {
            const RHI::DepthStencilAttachment& attachment =
                *desc.depthStencilAttachment;
            auto* texture = dynamic_cast<D3D12Texture*>(attachment.texture);
            const bool hasStencil = texture != nullptr &&
                texture->GetDesc().format == RHI::Format::D24_UNORM_S8_UINT;
            const bool declaredStateValid =
                attachment.state == RHI::ResourceState::DepthRead ||
                attachment.state == RHI::ResourceState::DepthWrite;
            const bool requiresWrite =
                attachment.depthLoadOp == RHI::LoadOp::Clear ||
                (hasStencil &&
                    attachment.stencilLoadOp == RHI::LoadOp::Clear);
            if (texture == nullptr ||
                texture->GetDepthStencilViewHandle(
                    attachment.mipLevel,
                    attachment.arrayLayer,
                    attachment.state == RHI::ResourceState::DepthRead) == 0 ||
                texture->GetNativeResource() == nullptr ||
                !declaredStateValid ||
                (requiresWrite &&
                    attachment.state != RHI::ResourceState::DepthWrite) ||
                !RequireTextureState(
                    m_internal,
                    texture,
                    attachment.mipLevel,
                    attachment.arrayLayer,
                    attachment.state) ||
                attachment.depthLoadOp == RHI::LoadOp::Undefined ||
                attachment.depthStoreOp == RHI::StoreOp::Undefined ||
                (hasStencil &&
                    (attachment.stencilLoadOp == RHI::LoadOp::Undefined ||
                        attachment.stencilStoreOp == RHI::StoreOp::Undefined)) ||
                (!hasStencil &&
                    (attachment.stencilLoadOp != RHI::LoadOp::Undefined ||
                        attachment.stencilStoreOp != RHI::StoreOp::Undefined)) ||
                (attachment.depthLoadOp == RHI::LoadOp::Clear &&
                    (!std::isfinite(attachment.clearDepth) ||
                        attachment.clearDepth < 0.0f ||
                        attachment.clearDepth > 1.0f)) ||
                (attachment.stencilLoadOp == RHI::LoadOp::Clear &&
                    attachment.clearStencil > std::numeric_limits<UINT8>::max()))
            {
                RejectRecording(m_internal);
                return;
            }
        }

        std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> colorHandles;
        colorHandles.reserve(desc.colorAttachmentCount);
        for (uint32_t index = 0; index < desc.colorAttachmentCount; ++index)
        {
            const RHI::ColorAttachment& attachment = desc.colorAttachments[index];
            auto* texture = static_cast<D3D12Texture*>(attachment.texture);
            auto* resource = static_cast<ID3D12Resource*>(texture->GetNativeResource());

            D3D12_CPU_DESCRIPTOR_HANDLE handle = {};
            handle.ptr = texture->GetRenderTargetViewHandle(
                attachment.mipLevel, attachment.arrayLayer);
            colorHandles.push_back(handle);
            TrackSwapchainImage(m_internal->referencedSwapchainImages, texture);
            RetainObject(m_internal->retainedObjects, resource);
            RetainObject(
                m_internal->retainedObjects,
                static_cast<ID3D12DescriptorHeap*>(
                    texture->GetRenderTargetViewHeap()));

            if (attachment.loadOp == RHI::LoadOp::Clear)
            {
                m_internal->commandList->ClearRenderTargetView(
                    handle, attachment.clearColor, 0, nullptr);
            }
            else if (attachment.loadOp == RHI::LoadOp::Discard)
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
            if (attachment.storeOp == RHI::StoreOp::Discard)
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
        }

        D3D12_CPU_DESCRIPTOR_HANDLE depthHandle = {};
        D3D12_CPU_DESCRIPTOR_HANDLE* depthHandlePointer = nullptr;
        m_internal->depthTexture = nullptr;
        m_internal->depthMipLevel = 0;
        m_internal->depthArrayLayer = 0;
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
            m_internal->depthTexture = texture;
            m_internal->depthMipLevel = attachment.mipLevel;
            m_internal->depthArrayLayer = attachment.arrayLayer;
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
            static_cast<UINT>(colorHandles.size()),
            colorHandles.empty() ? nullptr : colorHandles.data(),
            FALSE,
            depthHandlePointer);
        m_internal->viewportSet = false;
        m_internal->scissorSet = false;
        m_internal->stencilReferenceSet = false;
        m_internal->pipeline = nullptr;
        m_internal->inlineConstantCoverage.clear();
        m_internal->rendering = true;
    }

    void D3D12CommandList::EndRendering()
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

    void D3D12CommandList::BindGraphicsPipeline(RHI::PipelineHandle pipelineState)
    {
        if (!CanRecord(m_internal) || !m_internal->rendering ||
            pipelineState == nullptr)
        {
            RejectRecording(m_internal);
            return;
        }
        auto* pipeline = dynamic_cast<D3D12PipelineState*>(pipelineState);
        if (pipeline == nullptr)
        {
            RejectRecording(m_internal);
            return;
        }
        if (pipeline->GetNativePipelineState() == nullptr ||
            pipeline->GetNativeRootSignature() == nullptr)
        {
            RejectRecording(m_internal);
            return;
        }
        if (pipeline->RequiresDepthWrite() &&
            (m_internal->depthTexture == nullptr ||
                !RequireTextureState(
                    m_internal,
                    m_internal->depthTexture,
                    m_internal->depthMipLevel,
                    m_internal->depthArrayLayer,
                    RHI::ResourceState::DepthWrite)))
        {
            RejectRecording(m_internal);
            return;
        }
        m_internal->inlineConstantCoverage.assign(
            pipeline->GetLayout().inlineConstantSize, 0);
        m_internal->pipeline = pipeline;
        m_internal->commandList->SetPipelineState(pipeline->GetNativePipelineState());
        m_internal->commandList->SetGraphicsRootSignature(
            pipeline->GetNativeRootSignature());
        m_internal->commandList->IASetPrimitiveTopology(
            static_cast<D3D12_PRIMITIVE_TOPOLOGY>(pipeline->GetPrimitiveTopology()));
        RetainObject(m_internal->retainedObjects, pipeline->GetNativePipelineState());
        RetainObject(m_internal->retainedObjects, pipeline->GetNativeRootSignature());
    }

    void D3D12CommandList::BindResourceSet(RHI::ResourceSetHandle resourceSet)
    {
        if (!CanRecord(m_internal) || !m_internal->rendering ||
            resourceSet == nullptr ||
            m_internal->pipeline == nullptr ||
            resourceSet->GetPipeline() != m_internal->pipeline)
        {
            RejectRecording(m_internal);
            return;
        }
        auto* set = dynamic_cast<D3D12ResourceSet*>(resourceSet);
        if (set == nullptr)
        {
            RejectRecording(m_internal);
            return;
        }
        ID3D12DescriptorHeap* heap = set->GetNativeDescriptorHeap();
        if (m_internal->pipeline->GetDescriptorCount() != 0 && heap == nullptr)
        {
            RejectRecording(m_internal);
            return;
        }
        const RHI::PipelineLayoutDesc& layout =
            m_internal->pipeline->GetLayout();
        for (uint32_t index = 0; index < set->GetBindingCount(); ++index)
        {
            const RHI::ResourceBinding& binding = set->GetBindings()[index];
            const RHI::ResourceBindingLayout* declaration = FindLayoutBinding(
                layout, binding.binding);
            if (declaration == nullptr)
            {
                RejectRecording(m_internal);
                return;
            }
            if (declaration->type == RHI::ResourceBindingType::ConstantBuffer ||
                declaration->type ==
                    RHI::ResourceBindingType::ReadOnlyStorageBuffer ||
                declaration->type ==
                    RHI::ResourceBindingType::ReadWriteStorageBuffer)
            {
                auto* buffer = dynamic_cast<D3D12Buffer*>(binding.buffer);
                const RHI::ResourceState requiredState =
                    declaration->type == RHI::ResourceBindingType::ConstantBuffer
                    ? RHI::ResourceState::ConstantBuffer
                    : declaration->type ==
                        RHI::ResourceBindingType::ReadOnlyStorageBuffer
                        ? RHI::ResourceState::ShaderResource
                        : RHI::ResourceState::UnorderedAccess;
                if (buffer == nullptr ||
                    !RequireBufferState(
                        m_internal, buffer, requiredState))
                {
                    RejectRecording(m_internal);
                    return;
                }
            }
            else if (declaration->type ==
                    RHI::ResourceBindingType::SampledTexture ||
                declaration->type == RHI::ResourceBindingType::StorageTexture)
            {
                auto* texture = dynamic_cast<D3D12Texture*>(binding.texture);
                const RHI::ResourceState requiredState =
                    declaration->type ==
                        RHI::ResourceBindingType::SampledTexture
                    ? RHI::ResourceState::ShaderResource
                    : RHI::ResourceState::UnorderedAccess;
                if (texture == nullptr ||
                    !RequireTextureSubresourcesInState(
                        m_internal,
                        texture,
                        binding.subresources,
                        requiredState))
                {
                    RejectRecording(m_internal);
                    return;
                }
                TrackSwapchainImage(
                    m_internal->referencedSwapchainImages, texture);
            }
            else
            {
                RejectRecording(m_internal);
                return;
            }
        }
        if (heap != nullptr)
        {
            ID3D12DescriptorHeap* heaps[] = { heap };
            m_internal->commandList->SetDescriptorHeaps(1, heaps);
            const D3D12_GPU_DESCRIPTOR_HANDLE start =
                heap->GetGPUDescriptorHandleForHeapStart();
            for (const D3D12PipelineBinding& binding :
                m_internal->pipeline->GetBindings())
            {
                D3D12_GPU_DESCRIPTOR_HANDLE handle = start;
                handle.ptr += static_cast<UINT64>(binding.descriptorOffset) *
                    set->GetDescriptorSize();
                m_internal->commandList->SetGraphicsRootDescriptorTable(
                    binding.rootParameter, handle);
            }
            RetainObject(m_internal->retainedObjects, heap);
        }
        for (uint32_t index = 0; index < set->GetNativeResourceCount(); ++index)
            RetainObject(m_internal->retainedObjects, set->GetNativeResource(index));
    }

    void D3D12CommandList::BindVertexBuffer(
        uint32_t binding,
        RHI::BufferHandle buffer,
        uint32_t offset)
    {
        if (!CanRecord(m_internal) || !m_internal->rendering ||
            m_internal->pipeline == nullptr ||
            buffer == nullptr ||
            !HasUsage(buffer->GetDesc().usage, RHI::BufferUsage::Vertex) ||
            binding >= D3D12_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT ||
            offset >= buffer->GetDesc().size)
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
        auto* d3dBuffer = dynamic_cast<D3D12Buffer*>(buffer);
        if (d3dBuffer == nullptr)
        {
            RejectRecording(m_internal);
            return;
        }
        auto* resource = static_cast<ID3D12Resource*>(d3dBuffer->GetNativeResource());
        if (resource == nullptr)
        {
            RejectRecording(m_internal);
            return;
        }
        if (!RequireBufferState(
                m_internal,
                d3dBuffer,
                RHI::ResourceState::VertexBuffer))
        {
            RejectRecording(m_internal);
            return;
        }

        D3D12_VERTEX_BUFFER_VIEW view = {};
        view.BufferLocation = resource->GetGPUVirtualAddress() + offset;
        view.SizeInBytes = buffer->GetDesc().size - offset;
        view.StrideInBytes = stride;
        m_internal->commandList->IASetVertexBuffers(binding, 1, &view);
        RetainObject(m_internal->retainedObjects, resource);
    }

    void D3D12CommandList::BindIndexBuffer(
        RHI::BufferHandle buffer,
        RHI::Format format,
        uint32_t offset)
    {
        const uint32_t indexSize = format == RHI::Format::R16_UINT ? 2u : 4u;
        if (!CanRecord(m_internal) || !m_internal->rendering ||
            buffer == nullptr ||
            !HasUsage(buffer->GetDesc().usage, RHI::BufferUsage::Index) ||
            offset >= buffer->GetDesc().size ||
            (format != RHI::Format::R16_UINT && format != RHI::Format::R32_UINT))
        {
            RejectRecording(m_internal);
            return;
        }
        if ((offset % indexSize) != 0 || buffer->GetDesc().size - offset < indexSize)
        {
            RejectRecording(m_internal);
            return;
        }
        auto* d3dBuffer = dynamic_cast<D3D12Buffer*>(buffer);
        if (d3dBuffer == nullptr)
        {
            RejectRecording(m_internal);
            return;
        }
        auto* resource = static_cast<ID3D12Resource*>(d3dBuffer->GetNativeResource());
        if (resource == nullptr)
        {
            RejectRecording(m_internal);
            return;
        }
        if (!RequireBufferState(
                m_internal,
                d3dBuffer,
                RHI::ResourceState::IndexBuffer))
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

    void D3D12CommandList::SetInlineConstants(
        uint32_t offset,
        uint32_t size,
        const void* data)
    {
        if (!CanRecord(m_internal) || m_internal->pipeline == nullptr ||
            data == nullptr ||
            size == 0 || (offset % 4) != 0 || (size % 4) != 0 ||
            offset > m_internal->pipeline->GetLayout().inlineConstantSize ||
            size > m_internal->pipeline->GetLayout().inlineConstantSize - offset)
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
        m_internal->commandList->SetGraphicsRoot32BitConstants(
            rootParameter, size / 4, data, offset / 4);
        std::fill(
            m_internal->inlineConstantCoverage.begin() + offset,
            m_internal->inlineConstantCoverage.begin() + offset + size,
            1);
    }

    void D3D12CommandList::SetStencilReference(uint32_t reference)
    {
        if (!CanRecord(m_internal) || !m_internal->rendering ||
            reference > std::numeric_limits<UINT8>::max())
        {
            RejectRecording(m_internal);
            return;
        }
        m_internal->commandList->OMSetStencilRef(reference);
        m_internal->stencilReferenceSet = true;
    }

    void D3D12CommandList::SetViewport(const RHI::Viewport& viewport)
    {
        if (!CanRecord(m_internal) || !m_internal->rendering ||
            !std::isfinite(viewport.x) || !std::isfinite(viewport.y) ||
            !std::isfinite(viewport.width) || !std::isfinite(viewport.height) ||
            !std::isfinite(viewport.minDepth) ||
            !std::isfinite(viewport.maxDepth) ||
            viewport.width < 0.0f || viewport.height < 0.0f ||
            viewport.minDepth < 0.0f || viewport.maxDepth > 1.0f ||
            viewport.minDepth > viewport.maxDepth)
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
        m_internal->viewportSet = true;
    }

    void D3D12CommandList::SetScissor(const RHI::Rect& rect)
    {
        const int64_t right = static_cast<int64_t>(rect.x) + rect.width;
        const int64_t bottom = static_cast<int64_t>(rect.y) + rect.height;
        if (!CanRecord(m_internal) || !m_internal->rendering ||
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
        m_internal->scissorSet = true;
    }

    void D3D12CommandList::DrawInstanced(
        uint32_t vertexCount,
        uint32_t instanceCount,
        uint32_t startVertex,
        uint32_t startInstance)
    {
        if (vertexCount == 0 || instanceCount == 0) return;
        if (!CanRecord(m_internal) || !m_internal->rendering ||
            !HasCompleteDrawState(m_internal))
        {
            RejectRecording(m_internal);
            return;
        }
        m_internal->commandList->DrawInstanced(
            vertexCount, instanceCount, startVertex, startInstance);
    }

    void D3D12CommandList::DrawIndexedInstanced(
        uint32_t indexCount,
        uint32_t instanceCount,
        uint32_t firstIndex,
        int32_t vertexOffset,
        uint32_t firstInstance)
    {
        if (indexCount == 0 || instanceCount == 0) return;
        if (!CanRecord(m_internal) || !m_internal->rendering ||
            !HasCompleteDrawState(m_internal))
        {
            RejectRecording(m_internal);
            return;
        }
        m_internal->commandList->DrawIndexedInstanced(
            indexCount, instanceCount, firstIndex, vertexOffset, firstInstance);
    }

    void D3D12CommandList::Close()
    {
        if (m_internal == nullptr || m_internal->closed) return;
        if (m_internal->rendering || m_internal->commandList == nullptr)
        {
            RejectRecording(m_internal);
            return;
        }
        if (FAILED(m_internal->commandList->Close()))
        {
            RejectRecording(m_internal);
            return;
        }
        m_internal->closed = true;
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
            m_internal->device == nullptr ||
            buffer == nullptr || data == nullptr || size == 0 ||
            offset > buffer->GetDesc().size || size > buffer->GetDesc().size - offset)
        {
            return false;
        }
        auto* destination = static_cast<ID3D12Resource*>(buffer->GetNativeResource());
        if (destination == nullptr) return false;

        D3D12_HEAP_PROPERTIES heapProperties = {};
        heapProperties.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC resourceDesc = {};
        resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        resourceDesc.Width = size;
        resourceDesc.Height = 1;
        resourceDesc.DepthOrArraySize = 1;
        resourceDesc.MipLevels = 1;
        resourceDesc.SampleDesc.Count = 1;
        resourceDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        ComPtr<ID3D12Resource> upload;
        if (FAILED(m_internal->device->CreateCommittedResource(
                &heapProperties,
                D3D12_HEAP_FLAG_NONE,
                &resourceDesc,
                D3D12_RESOURCE_STATE_GENERIC_READ,
                nullptr,
                IID_PPV_ARGS(&upload))))
        {
            return false;
        }
        void* mapped = nullptr;
        D3D12_RANGE readRange = {};
        if (FAILED(upload->Map(0, &readRange, &mapped))) return false;
        std::memcpy(mapped, data, size);
        D3D12_RANGE writeRange = { 0, size };
        upload->Unmap(0, &writeRange);

        m_internal->commandList->CopyBufferRegion(
            destination, offset, upload.Get(), 0, size);
        RetainObject(m_internal->retainedObjects, destination);
        RetainObject(m_internal->retainedObjects, upload.Get());
        D3D12Operation operation = {};
        operation.kind = OperationKind::BufferWrite;
        operation.buffer = buffer;
        m_internal->operations.push_back(operation);
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
            texture == nullptr || data == nullptr || dataSize == 0 ||
            rowPitch == 0 || slicePitch == 0 ||
            mipLevel >= texture->GetDesc().mipLevels ||
            arrayLayer >= texture->GetDesc().depthOrArraySize ||
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
        if (rowCount == 0 || rowSize == 0 || uploadSize == 0 ||
            rowSize > rowPitch ||
            rowCount > (std::numeric_limits<uint32_t>::max() / rowPitch))
        {
            return false;
        }
        const uint64_t requiredSourceSize =
            static_cast<uint64_t>(rowCount - 1) * rowPitch + rowSize;
        if (requiredSourceSize > slicePitch || slicePitch > dataSize)
            return false;

        D3D12_HEAP_PROPERTIES heapProperties = {};
        heapProperties.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC uploadDesc = {};
        uploadDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        uploadDesc.Width = uploadSize;
        uploadDesc.Height = 1;
        uploadDesc.DepthOrArraySize = 1;
        uploadDesc.MipLevels = 1;
        uploadDesc.SampleDesc.Count = 1;
        uploadDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> upload;
        if (FAILED(m_internal->device->CreateCommittedResource(
                &heapProperties,
                D3D12_HEAP_FLAG_NONE,
                &uploadDesc,
                D3D12_RESOURCE_STATE_GENERIC_READ,
                nullptr,
                IID_PPV_ARGS(&upload))))
        {
            return false;
        }

        uint8_t* mapped = nullptr;
        D3D12_RANGE readRange = {};
        if (FAILED(upload->Map(
                0, &readRange, reinterpret_cast<void**>(&mapped))))
        {
            return false;
        }
        const auto* source = static_cast<const uint8_t*>(data);
        for (UINT row = 0; row < rowCount; ++row)
        {
            std::memcpy(
                mapped + footprint.Offset +
                    static_cast<std::size_t>(row) * footprint.Footprint.RowPitch,
                source + static_cast<std::size_t>(row) * rowPitch,
                static_cast<std::size_t>(rowSize));
        }
        D3D12_RANGE writeRange = { 0, static_cast<SIZE_T>(uploadSize) };
        upload->Unmap(0, &writeRange);

        D3D12_TEXTURE_COPY_LOCATION sourceLocation = {};
        sourceLocation.pResource = upload.Get();
        sourceLocation.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        sourceLocation.PlacedFootprint = footprint;
        D3D12_TEXTURE_COPY_LOCATION destinationLocation = {};
        destinationLocation.pResource = destination;
        destinationLocation.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        destinationLocation.SubresourceIndex = subresource;
        m_internal->commandList->CopyTextureRegion(
            &destinationLocation, 0, 0, 0, &sourceLocation, nullptr);
        TrackSwapchainImage(m_internal->referencedSwapchainImages, texture);
        RetainObject(m_internal->retainedObjects, destination);
        RetainObject(m_internal->retainedObjects, upload.Get());
        D3D12Operation operation = {};
        operation.kind = OperationKind::TextureWrite;
        operation.texture = texture;
        operation.mipLevel = mipLevel;
        operation.arrayLayer = arrayLayer;
        m_internal->operations.push_back(operation);
        return true;
    }

    bool D3D12CommandList::ValidateForSubmit(
        D3D12SubmissionState& state) const
    {
        if (m_internal == nullptr || m_internal->recordingFailed)
            return false;

        for (const D3D12Operation& operation : m_internal->operations)
        {
            switch (operation.kind)
            {
            case OperationKind::BufferBarrier:
            {
                const auto found = state.buffers.find(operation.buffer);
                const RHI::ResourceState current =
                    found == state.buffers.end()
                    ? operation.buffer->GetState()
                    : found->second;
                if (current != operation.before) return false;
                state.buffers[operation.buffer] = operation.after;
                break;
            }
            case OperationKind::TextureBarrier:
            {
                const auto key = TextureKey(
                    operation.texture,
                    operation.mipLevel,
                    operation.arrayLayer);
                const auto found = state.textureSubresources.find(key);
                const RHI::ResourceState current =
                    found == state.textureSubresources.end()
                    ? operation.texture->GetState(
                        operation.mipLevel, operation.arrayLayer)
                    : found->second;
                if (current != operation.before) return false;
                state.textureSubresources[key] = operation.after;
                break;
            }
            case OperationKind::BufferRequirement:
            {
                const auto found = state.buffers.find(operation.buffer);
                const RHI::ResourceState current =
                    found == state.buffers.end()
                    ? operation.buffer->GetState()
                    : found->second;
                if (current != operation.before) return false;
                break;
            }
            case OperationKind::TextureRequirement:
            {
                const auto key = TextureKey(
                    operation.texture,
                    operation.mipLevel,
                    operation.arrayLayer);
                const auto found = state.textureSubresources.find(key);
                const RHI::ResourceState current =
                    found == state.textureSubresources.end()
                    ? operation.texture->GetState(
                        operation.mipLevel, operation.arrayLayer)
                    : found->second;
                if (current != operation.before) return false;
                break;
            }
            case OperationKind::BufferWrite:
            {
                const auto found = state.buffers.find(operation.buffer);
                const RHI::ResourceState current =
                    found == state.buffers.end()
                    ? operation.buffer->GetState()
                    : found->second;
                if (current != RHI::ResourceState::CopyDestination)
                    return false;
                break;
            }
            case OperationKind::TextureWrite:
            {
                const auto key = TextureKey(
                    operation.texture,
                    operation.mipLevel,
                    operation.arrayLayer);
                const auto found = state.textureSubresources.find(key);
                const RHI::ResourceState current =
                    found == state.textureSubresources.end()
                    ? operation.texture->GetState(
                        operation.mipLevel, operation.arrayLayer)
                    : found->second;
                if (current != RHI::ResourceState::CopyDestination)
                    return false;
                break;
            }
            }
        }
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
