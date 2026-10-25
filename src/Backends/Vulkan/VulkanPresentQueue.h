#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <stdexcept>
#include <type_traits>

namespace dyf::Backends
{
// Exactly one producer and one consumer. Job-owned resources must outlive
// consumption; destruction requires both threads to have stopped using the queue.
// Sleeping and wake-up synchronization belong to the caller.
template<class T>
class VulkanPresentQueue final
{
    static_assert(std::is_trivially_copyable_v<T>, "Present jobs must be trivially copyable.");
    static_assert(std::atomic<uint32_t>::is_always_lock_free, "Present queue indices must be lock-free.");

    struct Slot
    {
        alignas(T) unsigned char bytes[sizeof(T)];
    };
    struct alignas(64) Cursor
    {
        std::atomic<uint32_t> index{0};
    };

    static uint32_t SlotCount(uint32_t capacity)
    {
        if(!capacity || capacity == std::numeric_limits<uint32_t>::max())
            throw std::invalid_argument("Present queue capacity must be in [1, UINT32_MAX - 1].");
        // One unused slot distinguishes full from empty without a shared count.
        return capacity + 1;
    }
    uint32_t Next(uint32_t index) const noexcept
    {
        // Bounded indices also avoid monotonic uint32_t wrap/modulo ambiguity
        // for capacities which are not powers of two.
        return index + 1 == m_slotCount ? 0 : index + 1;
    }

public:
    // Capacity is the number of jobs accepted, excluding the internal sentinel.
    explicit VulkanPresentQueue(uint32_t capacity)
        : m_slotCount(SlotCount(capacity)), m_slots(new Slot[m_slotCount]) {}

    VulkanPresentQueue(const VulkanPresentQueue&) = delete;
    VulkanPresentQueue& operator=(const VulkanPresentQueue&) = delete;
    VulkanPresentQueue(VulkanPresentQueue&&) = delete;
    VulkanPresentQueue& operator=(VulkanPresentQueue&&) = delete;

    [[nodiscard]] uint32_t Capacity() const noexcept { return m_slotCount - 1; }

    // Producer only. A false result leaves the job and queue unchanged.
    [[nodiscard]] bool TryPush(const T& job) noexcept
    {
        const auto write = m_producer.index.load(std::memory_order_relaxed);
        const auto next = Next(write);
        if(next == m_consumer.index.load(std::memory_order_acquire)) return false;
        std::memcpy(m_slots[write].bytes, &job, sizeof(T));
        m_producer.index.store(next, std::memory_order_release);
        return true;
    }

    // Consumer only. A false result leaves the output job unchanged.
    [[nodiscard]] bool TryPop(T& job) noexcept
    {
        const auto read = m_consumer.index.load(std::memory_order_relaxed);
        if(read == m_producer.index.load(std::memory_order_acquire)) return false;
        std::memcpy(&job, m_slots[read].bytes, sizeof(T));
        m_consumer.index.store(Next(read), std::memory_order_release);
        return true;
    }

    // Consumer-side observation only; a producer may publish immediately after
    // this returns. TryPop is the authority for consuming a job.
    [[nodiscard]] bool Empty() const noexcept
    {
        return m_consumer.index.load(std::memory_order_relaxed) ==
            m_producer.index.load(std::memory_order_acquire);
    }

private:
    const uint32_t m_slotCount;
    std::unique_ptr<Slot[]> m_slots;
    Cursor m_producer;
    Cursor m_consumer;
};
}
