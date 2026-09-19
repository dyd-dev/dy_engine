#pragma once
#include <cstdint>
namespace dyf::RHI
{
class IDevice;
class ICommandList;
struct FenceHandle
{
    explicit operator bool() const {return m_device && m_value;}
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
