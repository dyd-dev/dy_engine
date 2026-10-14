#pragma once

#include <cstdint>

namespace dyf::Backends
{
    struct MetalTessellationFactorRange
    {
        uint64_t bytes = 0;
        uint64_t writeOffset = 0;
        uint64_t instanceStride = 0;
    };

    [[nodiscard]] inline bool GetMetalTessellationFactorRange(
        uint32_t factorStride, uint32_t patchCount, uint32_t instanceCount,
        uint32_t patchStart, uint32_t baseInstance, uint64_t maxBytes,
        MetalTessellationFactorRange& range)
    {
        if(factorStride == 0 || patchCount == 0 || instanceCount == 0) return false;
        // Metal fetches absolute patch and instance coordinates. Keep Hull's
        // zero-based output contiguous by binding its output after this prefix.
        // ponytail: prefix padding is capped by maxBytes; compact it only with a new shader indexing contract.
        const uint64_t firstFactor = uint64_t(patchCount) * baseInstance + patchStart;
        const uint64_t factors = uint64_t(patchCount) * instanceCount;
        const uint64_t maxFactors = maxBytes / factorStride;
        if(firstFactor > maxFactors || factors > maxFactors - firstFactor) return false;
        range = {(firstFactor + factors) * factorStride, firstFactor * factorStride,
            uint64_t(patchCount) * factorStride};
        return true;
    }
}
