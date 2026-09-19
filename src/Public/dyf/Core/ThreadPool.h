#pragma once

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <future>
#include <mutex>
#include <thread>
#include <vector>

namespace dyf::Core
{
// Tasks finish before destruction. A task's exception is delivered by its future.
// The owner must keep the pool alive while other threads enqueue or wait.
// Destruction must run on an external owner thread, never inside one of its tasks.
class ThreadPool final
{
public:
    explicit ThreadPool(uint32_t threadCount = 0);
    ~ThreadPool();
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    [[nodiscard]] std::future<void> Enqueue(std::function<void()> task);
    // Waits for the entire pool to become idle; task exceptions remain in futures.
    // Calling WaitAll from one of this pool's workers is rejected.
    void WaitAll();
    [[nodiscard]] uint32_t GetThreadCount() const { return static_cast<uint32_t>(m_workers.size()); }
    [[nodiscard]] bool IsWorkerThread() const;

private:
    void WorkerLoop();
    std::vector<std::thread> m_workers;
    std::deque<std::packaged_task<void()>> m_tasks;
    std::mutex m_mutex;
    std::condition_variable m_ready, m_idle;
    size_t m_active = 0;
    bool m_stopping = false;
};
}
