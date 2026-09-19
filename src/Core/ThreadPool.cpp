#include "dyf/Core/ThreadPool.h"
#include <algorithm>
#include <stdexcept>

namespace dyf::Core
{
namespace { thread_local const ThreadPool* currentPool = nullptr; }

ThreadPool::ThreadPool(uint32_t threadCount)
{
    if(!threadCount) threadCount = (std::max)(1u, std::thread::hardware_concurrency());
    m_workers.reserve(threadCount);
    try
    {
        for(uint32_t i = 0; i < threadCount; ++i)
            m_workers.emplace_back([this] { WorkerLoop(); });
    }
    catch(...)
    {
        { std::lock_guard<std::mutex> lock(m_mutex); m_stopping = true; }
        m_ready.notify_all();
        for(auto& worker : m_workers) worker.join();
        throw;
    }
}

ThreadPool::~ThreadPool()
{
    { std::lock_guard<std::mutex> lock(m_mutex); m_stopping = true; }
    m_ready.notify_all();
    for(auto& worker : m_workers) worker.join();
}

std::future<void> ThreadPool::Enqueue(std::function<void()> task)
{
    if(!task) throw std::invalid_argument("ThreadPool requires a callable task.");
    std::packaged_task<void()> pending(std::move(task));
    auto result = pending.get_future();
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if(m_stopping) throw std::runtime_error("ThreadPool is stopping.");
        m_tasks.push_back(std::move(pending));
    }
    m_ready.notify_one();
    return result;
}

bool ThreadPool::IsWorkerThread() const { return currentPool == this; }

void ThreadPool::WaitAll()
{
    if(IsWorkerThread()) throw std::logic_error("A ThreadPool worker cannot wait for its own pool.");
    std::unique_lock<std::mutex> lock(m_mutex);
    m_idle.wait(lock, [this] { return m_tasks.empty() && m_active == 0; });
}

void ThreadPool::WorkerLoop()
{
    currentPool = this;
    for(;;)
    {
        std::packaged_task<void()> task;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_ready.wait(lock, [this] { return m_stopping || !m_tasks.empty(); });
            if(m_tasks.empty()) break;
            task = std::move(m_tasks.front());
            m_tasks.pop_front();
            ++m_active;
        }
        task();
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            --m_active;
            if(m_tasks.empty() && !m_active) m_idle.notify_all();
        }
    }
    currentPool = nullptr;
}
}
