#pragma once
#include "dyf/RHI/Query.h"
#include "VulkanContext.h"
#include <stdexcept>

namespace dyf::Backends
{
class VulkanTimestampQuery final:public RHI::TimestampQuery
{
public:
    VulkanTimestampQuery(VkDevice device,uint32_t count):TimestampQuery(count),m_device(device)
    {
        VkQueryPoolCreateInfo info{};
        info.sType=VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
        info.queryType=VK_QUERY_TYPE_TIMESTAMP;
        info.queryCount=count;
        if(vkCreateQueryPool(device,&info,nullptr,&pool)!=VK_SUCCESS)
            throw std::runtime_error("Vulkan timestamp query creation failed.");
    }
    ~VulkanTimestampQuery() override {vkDestroyQueryPool(m_device,pool,nullptr);}
    VkQueryPool pool=VK_NULL_HANDLE;
private:
    VkDevice m_device;
};
}
