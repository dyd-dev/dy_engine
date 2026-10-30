#pragma once

#include "dyf/RHI/Query.h"
#include <d3d12.h>
#include <wrl.h>
#include <vector>

namespace dyf::Backends
{
class D3D12TimestampQuery final : public RHI::TimestampQuery
{
public:
    D3D12TimestampQuery(uint32_t count, ID3D12Fence* completionFence)
        : TimestampQuery(count), fence(completionFence), completions(count, 0) {}
    bool InFlight(uint32_t index) const
    {
        const auto completed = fence->GetCompletedValue();
        return completed == UINT64_MAX || completions[index] > completed;
    }
    Microsoft::WRL::ComPtr<ID3D12QueryHeap> heap;
    Microsoft::WRL::ComPtr<ID3D12Resource> readback;
    Microsoft::WRL::ComPtr<ID3D12Fence> fence;
    // Zero means unwritten/reset. Nonzero values belong to this device's submission fence.
    std::vector<uint64_t> completions;
};
}
