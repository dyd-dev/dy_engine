#pragma once

#include "dyf/RHI/ICommandList.h"
#include <array>
#include <atomic>
#include <memory>
#include <unordered_map>
#include <vector>

namespace dyf::Backends
{
    class MetalBuffer;
    class MetalTexture;
    enum class MetalOperationKind : uint8_t { BufferBarrier, TextureBarrier };
    struct MetalOperation
    {
        MetalOperationKind kind = MetalOperationKind::BufferBarrier;
        MetalBuffer* buffer = nullptr;
        MetalTexture* texture = nullptr;
        RHI::ResourceState after = RHI::ResourceState::Undefined;
        uint32_t mipLevel = 0;
        uint32_t arrayLayer = 0;
    };
    struct MetalVertexBinding { MetalBuffer* buffer = nullptr; uint32_t offset = 0; };

    // Contains no command buffer, encoder, resource ownership or upload lease.
    struct MetalCommandScratch
    {
        std::vector<MetalOperation> operations;
        std::unordered_map<uint32_t, MetalVertexBinding> vertexBindings;
        std::vector<RHI::Format> colorFormats;
        std::vector<uint8_t> inlineConstants;
        void Clear() noexcept
        {
            operations.clear();vertexBindings.clear();colorFormats.clear();inlineConstants.clear();
        }
    };

    class MetalCommandScratchPool final
    {
    public:
        ~MetalCommandScratchPool()
        { for(auto& slot : m_available) delete slot.scratch.load(std::memory_order_relaxed); }
        std::unique_ptr<MetalCommandScratch> Acquire()
        {
            for(auto& slot : m_available)
                if(auto* scratch = slot.scratch.exchange(nullptr, std::memory_order_acquire))
                    return std::unique_ptr<MetalCommandScratch>(scratch);
            return std::make_unique<MetalCommandScratch>();
        }
        void Return(std::unique_ptr<MetalCommandScratch> scratch) noexcept
        {
            if(!scratch) return;
            scratch->Clear();
            for(auto& slot : m_available)
            {
                auto* empty = static_cast<MetalCommandScratch*>(nullptr);
                if(slot.scratch.compare_exchange_strong(empty, scratch.get(), std::memory_order_release,
                    std::memory_order_relaxed))
                { scratch.release();return; }
            }
            // A full cache discards only empty CPU storage.
        }
    private:
        struct Slot { std::atomic<MetalCommandScratch*> scratch{nullptr}; };
        std::array<Slot, 64> m_available;
    };
}
