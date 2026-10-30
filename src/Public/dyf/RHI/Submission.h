#pragma once
#include <cstdint>
namespace dyf::RHI
{
class IDevice;
class ICommandList;
struct FenceHandle
{
    explicit operator bool() const {return m_device && m_value;}
    // Completion tokens identify one submission on one device, including an empty token.
    friend constexpr bool operator==(FenceHandle left, FenceHandle right) noexcept
    {return left.m_device == right.m_device && left.m_value == right.m_value;}
    friend constexpr bool operator!=(FenceHandle left, FenceHandle right) noexcept
    {return !(left == right);}
private:
    friend class IDevice;
    const IDevice* m_device=nullptr;
    uint64_t m_value=0;
};
struct SubmitDesc
{
    ICommandList* const* commandLists=nullptr;
    uint32_t commandListCount=0;
    const FenceHandle* waits=nullptr;
    uint32_t waitCount=0;
};
}
