#pragma once
#if defined(DY_TRACY_ENABLED)
#include "VulkanContext.h"
#include <tracy/Tracy.hpp>
#include <tracy/TracyVulkan.hpp>
#include <memory>

namespace dyf::Backends
{
// Marker pools belong to the same fence as the commands they surround.
struct VulkanTracyBatch
{
    VkDevice device = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer commands[2]{};
    uint16_t queries[2]{};
    ~VulkanTracyBatch() { if(pool) vkDestroyCommandPool(device, pool, nullptr); }

    bool Initialize(const VulkanContext& native)
    {
        device = native.device;
        VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        poolInfo.queueFamilyIndex = native.queueIndices.graphicsFamily;
        if(vkCreateCommandPool(device, &poolInfo, nullptr, &pool) != VK_SUCCESS) return false;
        VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        allocate.commandPool = pool; allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; allocate.commandBufferCount = 2;
        return vkAllocateCommandBuffers(device, &allocate, commands) == VK_SUCCESS;
    }
};

class VulkanTracy
{
public:
    ~VulkanTracy() { if(m_context) TracyVkDestroy(m_context); }
    bool Initialize(const VulkanContext& native)
    {
        if(!m_initial.Initialize(native)) return false;
        auto domains = reinterpret_cast<PFN_vkGetPhysicalDeviceCalibrateableTimeDomainsEXT>(
            vkGetInstanceProcAddr(native.instance, "vkGetPhysicalDeviceCalibrateableTimeDomainsKHR"));
        if(!domains) domains = reinterpret_cast<PFN_vkGetPhysicalDeviceCalibrateableTimeDomainsEXT>(
            vkGetInstanceProcAddr(native.instance, "vkGetPhysicalDeviceCalibrateableTimeDomainsEXT"));
        m_context = TracyVkContextCalibrated(native.physicalDevice, native.device, native.graphicsQueue,
            m_initial.commands[0], domains, native.getCalibratedTimestamps);
        TracyVkContextName(m_context, "Vulkan graphics queue", 21);
        return true;
    }

    std::unique_ptr<VulkanTracyBatch> Record(const VulkanContext& native)
    {
        if(m_disabled) return {};
        std::unique_ptr<VulkanTracyBatch> batch(new(std::nothrow) VulkanTracyBatch);
        if(!batch) { m_disabled = true; return {}; }
        if(!batch->Initialize(native)) { m_disabled = true; return {}; }
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        for(auto command : batch->commands)
            if(vkBeginCommandBuffer(command, &begin) != VK_SUCCESS) { m_disabled = true; return {}; }
        m_context->Collect(batch->commands[0]);
        for(unsigned i = 0; i < 2; ++i)
        {
            batch->queries[i] = uint16_t(m_context->NextQueryId());
            vkCmdWriteTimestamp(batch->commands[i], VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                m_context->GetQueryPool(), batch->queries[i]);
            if(vkEndCommandBuffer(batch->commands[i]) != VK_SUCCESS)
            {
                // A discarded query would leave a hole in Tracy's query ring.
                m_disabled = true;
                return {};
            }
        }
        return batch;
    }

    void Publish(const VulkanTracyBatch& batch, int64_t cpuBegin, int64_t cpuEnd)
    {
        static constexpr tracy::SourceLocationData source{
            "Vulkan queue submission", "VulkanDevice::Submit", __FILE__, __LINE__, 0};
        // Publish only successful submits, retaining the actual vkQueueSubmit CPU interval.
        auto event = tracy::Profiler::QueueSerial();
        tracy::MemWrite(&event->hdr.type, tracy::QueueType::GpuZoneBeginSerial);
        tracy::MemWrite(&event->gpuZoneBegin.cpuTime, cpuBegin);
        tracy::MemWrite(&event->gpuZoneBegin.srcloc, uint64_t(&source));
        tracy::MemWrite(&event->gpuZoneBegin.thread, tracy::GetThreadHandle());
        tracy::MemWrite(&event->gpuZoneBegin.queryId, batch.queries[0]);
        tracy::MemWrite(&event->gpuZoneBegin.context, m_context->GetId());
        tracy::Profiler::QueueSerialFinish();
        // The C GPU-end API hardcodes thread 0; Vulkan needs the submitting thread.
        event = tracy::Profiler::QueueSerial();
        tracy::MemWrite(&event->hdr.type, tracy::QueueType::GpuZoneEndSerial);
        tracy::MemWrite(&event->gpuZoneEnd.cpuTime, cpuEnd);
        tracy::MemWrite(&event->gpuZoneEnd.thread, tracy::GetThreadHandle());
        tracy::MemWrite(&event->gpuZoneEnd.queryId, batch.queries[1]);
        tracy::MemWrite(&event->gpuZoneEnd.context, m_context->GetId());
        tracy::Profiler::QueueSerialFinish();
    }

    // Call only after device idle; collect final timestamps before destroying the pool.
    void Flush()
    {
        if(m_disabled) return;
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if(vkBeginCommandBuffer(m_initial.commands[0], &begin) != VK_SUCCESS) return;
        m_context->Collect(m_initial.commands[0]);
        // The recorded resets need not execute: the query pool is being destroyed.
        vkEndCommandBuffer(m_initial.commands[0]);
    }
private:
    VulkanTracyBatch m_initial;
    tracy::VkCtx* m_context = nullptr;
    bool m_disabled = false;
};
}
#endif
