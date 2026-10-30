#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

namespace dyf::RHI::Detail
{
// Callers validate the write range first. Completeness changes only on a write
// or pipeline bind, so draw/dispatch validation need not rescan every byte.
class InlineConstantCoverage
{
public:
    void Reset(uint32_t size)
    {
        m_written.assign(size, 0);
        m_remaining = size;
    }

    void Write(uint32_t offset, uint32_t size) noexcept
    {
        if (!m_remaining) return;
        if (!offset && size == m_written.size())
        {
            m_remaining = 0;
            return;
        }
        const auto first = m_written.begin() + offset;
        const auto last = first + size;
        m_remaining -= static_cast<uint32_t>(std::count(first, last, uint8_t{0}));
        std::fill(first, last, uint8_t{1});
    }

    [[nodiscard]] bool IsComplete() const noexcept { return m_remaining == 0; }

private:
    std::vector<uint8_t> m_written;
    uint32_t m_remaining = 0;
};
}
