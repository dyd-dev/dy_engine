#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>

namespace dyf
{
namespace Detail
{
struct QueryTransferBounds
{
    size_t length = 0;
    size_t remaining = 0;

    bool Begin(uint64_t bytes)
    {
        if(bytes == 0 || bytes > (std::numeric_limits<size_t>::max)() - 11) return false;
        length = static_cast<size_t>(bytes);
        remaining = length + 11; // The wire format pads its final part to 12 bytes.
        return true;
    }

    bool ConsumePart()
    {
        if(length == 0 || remaining < 12) return false;
        remaining -= 12;
        return true;
    }

    bool Complete() const { return length != 0 && remaining < 12; }
};
}
}
