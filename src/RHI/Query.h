#pragma once

#include <cstdint>
#include "dyf/RHI/ResourceHandles.h"

namespace dyf::RHI
{
class TimestampQuery
{
public:
    TimestampQuery(const TimestampQuery&) = delete;
    TimestampQuery& operator=(const TimestampQuery&) = delete;
    uint32_t GetCount() const { return m_count; }

protected:
    explicit TimestampQuery(uint32_t count) : m_count(count) {}
    virtual ~TimestampQuery() = default;

private:
    uint32_t m_count;
};
}
