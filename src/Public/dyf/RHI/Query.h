#pragma once
#include <cstdint>

namespace dyf::RHI
{
struct TimestampQueryDesc { uint32_t count=0; };
class TimestampQuery
{
public:
    uint32_t GetCount() const { return m_count; }
protected:
    explicit TimestampQuery(uint32_t count):m_count(count) {}
    virtual ~TimestampQuery()=default;
private:
    uint32_t m_count;
};
using TimestampQueryHandle=TimestampQuery*;
}
