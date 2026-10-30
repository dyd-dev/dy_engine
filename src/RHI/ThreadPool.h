#pragma once

#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

namespace dyf::RHI
{
// Internal workers for CPU command recording. ParallelFor joins all its items
// and delivers their exceptions before returning. The owner must keep the pool
// alive while another thread calls it. Idle workers park in native wake objects.
// Destruction must run on an external owner thread, never inside one of its tasks.
class ThreadPool final
{
public:
    explicit ThreadPool(uint32_t threadCount = 0);
    ~ThreadPool();
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    // Reuses one job slot per worker; no queued closures or futures per item.
    // Calls from pool workers run serially, including calls into another pool.
    // Concurrent external batches are serialized. Returns after every item finishes.
    template<class Task>
    void ParallelFor(uint32_t count, const Task& task)
    {
        RunBatch(count, &task, [](const void* context, uint32_t index) {
            (*static_cast<const Task*>(context))(index);
        });
    }
    [[nodiscard]] uint32_t GetThreadCount() const { return static_cast<uint32_t>(m_workers.size()); }
    [[nodiscard]] bool IsWorkerThread() const;

private:
    struct Batch;
    struct Storage;
    using BatchTask = void(*)(const void*, uint32_t);
    void WorkerLoop(uint32_t index);
    void RunBatch(uint32_t count, const void* context, BatchTask task);
    void ExecuteBatch(Batch& batch, uint32_t worker);
    void StopWorkers();
    std::unique_ptr<Storage> m_storage;
    std::vector<std::thread> m_workers;
};
}
